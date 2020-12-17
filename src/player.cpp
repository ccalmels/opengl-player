#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>
#include <cassert>
#include <atomic>

#include "egl.hpp"
#include "gl.hpp"
#include "ffmpeg.hpp"

#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#if HAVE_VA
#include <va/va_drmcommon.h>
extern "C" {
#include <libavutil/hwcontext_vaapi.h>
}
#endif

#if HAVE_CUDA
#include <cuda.h>
#include <cudaGL.h>
extern "C" {
#include <libavutil/hwcontext_cuda.h>
}
#endif

#include <unistd.h>

static int width;
static int height;

static const std::string vertex = R""(
#version 450

	layout (location = 0) in vec2 va_position;
	out vec2 v_uv;
	uniform vec2 scale = vec2(1.0, 1.0);

	void main() {
		v_uv = va_position;
		v_uv.y = -v_uv.y;
		v_uv = scale * (v_uv + vec2(1.0)) * 0.5;

		gl_Position = vec4(va_position, 0.0, 1.0);
	}
	)"";

static const std::string fragment_yuv = R""(
#version 450

	precision highp float;

	in vec2 v_uv;
	out vec4 color;

	uniform sampler2D plane0, plane1, plane2;

	const mat3 yuv2rgb = mat3(1.0, 1.0, 1.0,
				  0.0, -0.39465, 2.03211,
				  1.13983, -0.58060, 0.0);

	void main() {
		vec3 yuv, rgb;

		yuv.r = texture(plane0, v_uv).r;
		yuv.g = texture(plane1, v_uv).r - 0.5;
		yuv.b = texture(plane2, v_uv).r - 0.5;

		rgb = yuv2rgb * yuv;
		color = vec4(rgb, 1.0);
        }
        )"";

static const std::string fragment_nv12 = R""(
#version 450

	precision highp float;

	in vec2 v_uv;
	out vec4 color;

	uniform sampler2D plane0, plane1;

	const mat3 yuv2rgb = mat3(1.0, 1.0, 1.0,
				  0.0, -0.39465, 2.03211,
				  1.13983, -0.58060, 0.0);

	void main() {
		vec3 yuv, rgb;

		yuv.r = texture(plane0, v_uv).r;
		yuv.gb = texture(plane1, v_uv).rg - vec2(0.5);

		rgb = yuv2rgb * yuv;
		color = vec4(rgb, 1.0);
        }
        )"";

static void init_quad()
{
	GLuint vao_quad, vbo_quad;
	glGenVertexArrays(1, &vao_quad);
	glBindVertexArray(vao_quad);

	glGenBuffers(1, &vbo_quad);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);

	float vertex_data[] = {
		1, 1,
		1, -1,
		-1, 1,
		-1, -1,
	};

	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data,
		     GL_STATIC_DRAW);

	GLuint va_position = 0;
	glEnableVertexAttribArray(va_position);
	glVertexAttribPointer(va_position, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
}

struct queue {
	queue(size_t n = 3) : is_ok(true), size(n) {}

	bool push(av::frame &f) {
		std::unique_lock<std::mutex> l(m);

		while (filled.size() >= size && is_ok)
			cv.wait(l);

		filled.push_back(f);
		l.unlock();

		cv.notify_one();
		return is_ok;
	}

	bool get(int64_t pts, av::frame &f) {
		std::lock_guard<std::mutex> l(m);
		bool ret = false;
		int count = -1;

		while (!filled.empty() && filled.front().f->pts <= pts) {
			f = std::move(filled.front());
			filled.pop_front();

			ret = true;
			count++;
		}

		if (ret && count)
			std::cerr << "drop: " << count << " frames" << std::endl;

		if (filled.size() < size)
			cv.notify_one();
		return ret;
	}

	bool wait() {
		std::unique_lock<std::mutex> l(m);

		while (filled.empty() && is_ok)
			cv.wait(l);

		return !filled.empty();
	}

	bool operator!() {
		return !is_ok;
	}

	void stop() {
		is_ok = false;
		cv.notify_all();
	}

	std::atomic<bool> is_ok;
	size_t size;
	std::mutex m;
	std::condition_variable cv;
	std::list<av::frame> filled;
};

struct video {
	video(int w, int h) : w(w), h(h), planes() {}
	virtual ~video() {}

	virtual void update(const av::frame &f) = 0;
	virtual gl::program &get_program() = 0;

	void active(int active) {
		gl::program &p = get_program();

		p.use();

		for (size_t i = 0; i < planes.size(); i++) {
			planes[i].active(active + i);
			p.set(std::string("plane") + std::to_string(i), (int)(active + i));
		}
	}

	int w, h;
	std::vector<gl::texture> planes;
};

struct nv12_video : video {
	nv12_video(int w, int h) : video(w, h) {
		planes.emplace_back(GL_RED, w, h);
		planes.emplace_back(GL_RG, w * 0.5, h * 0.5);
	}

	void update(const av::frame &f) override {
		av::frame sw_frame;

		if (f.is_hardware())
			sw_frame = f.transfer();
		else
			sw_frame = f;

		for (size_t i = 0; i < planes.size(); i++)
			planes[i].update(sw_frame.f->data[i]);
	}

	gl::program &get_program() override {
		static gl::program nv12(vertex, fragment_nv12);
		return nv12;
	}
};

struct yuv_video : video {
	yuv_video(int w, int h) : video(w, h) {
		planes.emplace_back(GL_RED, w, h);
		planes.emplace_back(GL_RED, w * 0.5, h * 0.5);
		planes.emplace_back(GL_RED, w * 0.5, h * 0.5);
	}

	void update(const av::frame &f) override {
		for (size_t i = 0; i < planes.size(); i++)
			planes[i].update(f.f->data[i]);
	}

	gl::program &get_program() override {
		static gl::program yuv(vertex, fragment_yuv);
		return yuv;
	}
};

#if HAVE_VA
struct vaapi_video : nv12_video {
	vaapi_video(int w, int h) : nv12_video(w, h) {}

	void update(const av::frame &f) override {
		AVVAAPIDeviceContext *vactx = (AVVAAPIDeviceContext*)(((AVHWFramesContext*)f.f->hw_frames_ctx->data)->device_ctx->hwctx);
		VASurfaceID surface_id = (VASurfaceID)(uintptr_t)f.f->data[3];
		VADRMPRIMESurfaceDescriptor va_desc;
		uint32_t export_flags = VA_EXPORT_SURFACE_SEPARATE_LAYERS
			| VA_EXPORT_SURFACE_READ_ONLY;

		assert(vaExportSurfaceHandle(vactx->display, surface_id,
					     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
					     export_flags, &va_desc) == VA_STATUS_SUCCESS);
		assert(vaSyncSurface(vactx->display, surface_id) == VA_STATUS_SUCCESS);

		get_program().use();
		get_program().set("scale", ((float)w) / va_desc.width,
				  ((float)h) / va_desc.height);

		for(int i = 0; i < 2; i++) {
			EGLint attribs[] = {
				EGL_LINUX_DRM_FOURCC_EXT, (EGLint)va_desc.layers[i].drm_format,
				EGL_WIDTH, (EGLint)(va_desc.width / (1 + i)),
				EGL_HEIGHT, (EGLint)(va_desc.height / (1 + i)),
				EGL_DMA_BUF_PLANE0_FD_EXT, (EGLint)va_desc.objects[ va_desc.layers[i].object_index[0] ].fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)va_desc.layers[i].offset[0],
				EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)va_desc.layers[i].pitch[0],
				EGL_NONE
			};

			EGLImageKHR image;
			image = CreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
			assert(image);

			planes[i].active(0);
			EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

			assert(eglDestroyImageKHR(eglGetCurrentDisplay(), image) == EGL_TRUE);
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		for (unsigned int i = 0 ; i < va_desc.num_objects; i++)
			close(va_desc.objects[i].fd);
	}
	static PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
	static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
	static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC EGLImageTargetTexture2DOES;

	static bool initialize_extensions() {
		if (CreateImageKHR)
			return true;

		if (!egl::has_extension("EGL_KHR_image_base")
		    || !gl::has_extension("GL_OES_EGL_image"))
			return false;

		CreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		EGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		return true;
	}
};

PFNEGLCREATEIMAGEKHRPROC vaapi_video::CreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC vaapi_video::eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC vaapi_video::EGLImageTargetTexture2DOES;
#endif

#if HAVE_CUDA
struct cuda_video : nv12_video {
	cuda_video(int w, int h) : nv12_video(w, h), initialized(false) {}
	~cuda_video() {
		if (initialized)
			for (int i = 0; i < 2; i++)
				assert(cuGraphicsUnregisterResource(res[i]) == CUDA_SUCCESS);
	}

	void update(const av::frame &f) override {
		AVCUDADeviceContext *cudactx = (AVCUDADeviceContext*)(((AVHWFramesContext*)f.f->hw_frames_ctx->data)->device_ctx->hwctx);
		CUcontext dummy;

		assert(cuCtxPushCurrent(cudactx->cuda_ctx) == CUDA_SUCCESS);

		if (!initialized) {
			for (unsigned int i = 0; i < 2; i++) {
				assert(cuGraphicsGLRegisterImage(&res[i], planes[i].id , GL_TEXTURE_2D, 0) == CUDA_SUCCESS);
				assert(cuGraphicsMapResources(1, &res[i], 0) == CUDA_SUCCESS);
				assert(cuGraphicsSubResourceGetMappedArray(&array[i], res[i], 0, 0) == CUDA_SUCCESS);
				assert(cuGraphicsUnmapResources(1, &res[i], 0) == CUDA_SUCCESS);
			}
			initialized = true;
		}

		for (unsigned int i = 0; i < 2; i++) {
			CUDA_MEMCPY2D cpy = {
				.srcY          = 0,
				.srcMemoryType = CU_MEMORYTYPE_DEVICE,
				.srcDevice     = (CUdeviceptr)f.f->data[i],
				.srcPitch      = (unsigned int)f.f->linesize[i],
				.dstMemoryType = CU_MEMORYTYPE_ARRAY,
				.dstArray      = array[i],
				.WidthInBytes  = (unsigned int)f.f->width,
				.Height        = (unsigned int)(f.f->height >> i),
			};
			assert(cuMemcpy2DAsync(&cpy, 0) == CUDA_SUCCESS);
		}

		assert(cuStreamSynchronize(cudactx->stream) == CUDA_SUCCESS);
		assert(cuCtxPopCurrent(&dummy) == CUDA_SUCCESS);
	}

	bool initialized;
	CUarray array[2];
	CUgraphicsResource res[2];
};
#endif

video *create_video_from_frame(const av::frame &f)
{
	AVPixelFormat format = (AVPixelFormat)f.f->format;

	switch (format) {
	case AV_PIX_FMT_CUDA:
#if HAVE_CUDA
		std::cerr << "Using CUDA GL Interop" << std::endl;
		return new cuda_video(f.f->width, f.f->height);
#else
		return new nv12_video(f.f->width, f.f->height);
#endif
	case AV_PIX_FMT_VAAPI_VLD:
#if HAVE_VA
		if (vaapi_video::initialize_extensions()) {
			std::cerr << "Using VAAPI GL Interop" << std::endl;
			return new vaapi_video(f.f->width, f.f->height);
		}
#endif
	case AV_PIX_FMT_NV12:
		return new nv12_video(f.f->width, f.f->height);
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return new yuv_video(f.f->width, f.f->height);
	default:
		;
	}
	return nullptr;
}

static bool decode_video(av::input &video, queue &qframe,
			 av::hw_device &hw)
{
	av::packet p;
	av::frame f;
	av::decoder dec;
	int id;

	id = video.get_video_index(0);

	dec = video.get(hw, id);
	if (!dec)
		return false;

	while (video >> p) {
		if (p.stream_index() != id)
			continue;

		if (!(dec << p))
			return false;

		while (dec >> f)
			if (!qframe.push(f))
				return false;
	}

	return true;
}

static void read_video(av::input &video, queue &qframe)
{
	av::hw_device hw("vaapi");
	if (!hw)
		hw = av::hw_device("cuda");

	decode_video(video, qframe, hw);

	qframe.stop();
}

int main(int argc, char* argv[])
{
	std::string videoname = "pipe:0";
	queue qframe;

	if (argc > 1)
		videoname = argv[1];

	av::input video;
	if (!video.open(videoname))
		return -1;

	std::thread read = std::thread(read_video, std::ref(video), std::ref(qframe));

	if (!qframe.wait()) {
		read.join();
		return -1;
	}

	const av::frame &first = qframe.filled.front();
	int64_t first_pts = first.f->pts;
	width = first.f->width;
	height = first.f->height;

	if (!egl::init(argv[0], width, height))
		return -1;

	std::cerr << "SDL with egl  : " << width << "x" << height << std::endl;
	std::cerr << "EGL version   : " << egl::version() << std::endl;
	std::cerr << "OpenGL version: " << gl::version() << std::endl;

	init_quad();

	auto v = create_video_from_frame(qframe.filled.front());

	bool run = true;
	AVRational time_base = video.time_base(0);

	Uint32 start_tick = SDL_GetTicks();
	while (run && !!qframe) {
		int64_t pts = av_rescale(SDL_GetTicks() - start_tick, time_base.den,
					 time_base.num * 1000) + first_pts;
		SDL_Event e;

		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_KEYDOWN:
				switch (e.key.keysym.scancode) {
				case SDL_SCANCODE_ESCAPE:
					run = false;
					break;
				default:
					;
				}
				break;
			case SDL_QUIT:
				run = false;
				break;
			}
		}

		av::frame f;
		if (qframe.get(pts, f)) {
			v->update(f);
			v->active(0);
		}
		glViewport(0, 0, width, height);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		eglSwapBuffers(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW));
	}

	qframe.stop();
	read.join();
	return 0;
}
