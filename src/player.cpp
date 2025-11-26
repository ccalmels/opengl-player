#include <atomic>
#include <cassert>
#include <condition_variable>
#include <list>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/core.h>

#include "egl.hpp"
#include "ffmpeg.hpp"
#include "gl.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL2/SDL.h>

#if HAVE_VA
#include <va/va_drmcommon.h>
extern "C" {
#include <libavutil/hwcontext_vaapi.h>
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

        // https://fourcc.org/fccyvrgb.php
        const vec3 yuv_offset = vec3(0.0625, 0.5, 0.5);
	const mat3 yuv2rgb = mat3(1.164, 1.164, 1.164,
                                  0.0, -0.391, 2.018,
                                  1.596, -0.813, 0.0);

	void main() {
		vec3 yuv, rgb;

		yuv.r = texture(plane0, v_uv).r;
		yuv.g = texture(plane1, v_uv).r;
		yuv.b = texture(plane2, v_uv).r;

		rgb = yuv2rgb * (yuv - yuv_offset);
		color = vec4(rgb, 1.0);
        }
        )"";

static const std::string fragment_nv12 = R""(
#version 450

	precision highp float;

	in vec2 v_uv;
	out vec4 color;

	uniform sampler2D plane0, plane1;

        // https://fourcc.org/fccyvrgb.php
        const vec3 yuv_offset = vec3(0.0625, 0.5, 0.5);
	const mat3 yuv2rgb = mat3(1.164, 1.164, 1.164,
                                  0.0, -0.391, 2.018,
                                  1.596, -0.813, 0.0);

	void main() {
		vec3 yuv, rgb;

		yuv.r = texture(plane0, v_uv).r;
		yuv.gb = texture(plane1, v_uv).rg;

		rgb = yuv2rgb * (yuv - yuv_offset);
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
        1, 1, 1, -1, -1, 1, -1, -1,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data,
                 GL_STATIC_DRAW);

    GLuint va_position = 0;
    glEnableVertexAttribArray(va_position);
    glVertexAttribPointer(va_position, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)0);
}

struct queue {
    queue(size_t n = 3) : is_ok(true), size(n) {}

    bool push(av::frame &f)
    {
        std::unique_lock<std::mutex> l(m);

        while (filled.size() >= size && is_ok)
            cv.wait(l);

        filled.push_back(f);
        l.unlock();

        cv.notify_one();
        return is_ok;
    }

    bool get(int64_t pts, av::frame &f)
    {
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
            fmt::print(stderr, "drop {:>3d} frames\n", count);

        if (filled.size() < size)
            cv.notify_one();
        return ret;
    }

    bool wait()
    {
        std::unique_lock<std::mutex> l(m);

        while (filled.empty() && is_ok)
            cv.wait(l);

        return !filled.empty();
    }

    bool operator!() { return !is_ok; }

    void stop()
    {
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
    video() : aspect(1.0), planes() {}
    virtual ~video() {}

    virtual void update(const av::frame &f)
    {
        av::frame sw;

        if (f.is_hardware()) {
            sw = f.transfer();
        } else {
            sw = f;
        }

        for (size_t i = 0; i < planes.size(); i++)
            planes[i].update(sw.f->data[i]);
    }
    virtual gl::program &get_program() = 0;

    void active(int active)
    {
        static const std::string names[] = {
            "plane0",
            "plane1",
            "plane2",
        };
        gl::program &p = get_program();

        p.use();
        p.set("scale", aspect, 1.0f);

        for (size_t i = 0; i < planes.size(); i++) {
            planes[i].active(active + i);
            p.set(names[i], (int)(active + i));
        }
    }

    float aspect;
    std::vector<gl::texture> planes;
};

struct yuv : video {
    yuv(const av::frame &f) : video()
    {
        planes.emplace_back(GL_RED, f.f->linesize[0], f.f->height);
        planes.emplace_back(GL_RED, (float)f.f->linesize[1], f.f->height * 0.5);
        planes.emplace_back(GL_RED, (float)f.f->linesize[2], f.f->height * 0.5);

        aspect = (float)(f.f->width) / f.f->linesize[0];
    }

    gl::program &get_program() override
    {
        static gl::program yuv(vertex, fragment_yuv);
        return yuv;
    }
};

struct nv12 : video {
    nv12(const av::frame &f) : video()
    {
        planes.emplace_back(GL_RED, f.f->linesize[0], f.f->height);
        planes.emplace_back(GL_RG, f.f->linesize[1] * 0.5, f.f->height * 0.5);

        aspect = (float)(f.f->width) / f.f->linesize[0];
    }

    gl::program &get_program() override
    {
        static gl::program nv12(vertex, fragment_nv12);
        return nv12;
    }

    float aspect;
};

#if HAVE_VA
struct vaapi : video {
    vaapi(const av::frame &f) : video()
    {
        AVVAAPIDeviceContext *vactx =
            (AVVAAPIDeviceContext *)(((AVHWFramesContext *)
                                          f.f->hw_frames_ctx->data)
                                         ->device_ctx->hwctx);
        VASurfaceID surface_id = (VASurfaceID)(uintptr_t)f.f->data[3];
        VADRMPRIMESurfaceDescriptor va_desc;
        uint32_t export_flags =
            VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_ONLY;

        assert(vaExportSurfaceHandle(vactx->display, surface_id,
                                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                     export_flags,
                                     &va_desc) == VA_STATUS_SUCCESS);

        // NV12 only for the moment
        planes.emplace_back(GL_RED, va_desc.width, va_desc.height);
        planes.emplace_back(GL_RG, va_desc.width / 2, va_desc.height / 2);

        // aspect is f.f->witdth / va_desc.width

        for (unsigned int i = 0; i < va_desc.num_objects; i++)
            close(va_desc.objects[i].fd);
    }

    void update(const av::frame &f) override
    {
        AVVAAPIDeviceContext *vactx =
            (AVVAAPIDeviceContext *)(((AVHWFramesContext *)
                                          f.f->hw_frames_ctx->data)
                                         ->device_ctx->hwctx);
        VASurfaceID surface_id = (VASurfaceID)(uintptr_t)f.f->data[3];
        VADRMPRIMESurfaceDescriptor va_desc;
        uint32_t export_flags =
            VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_ONLY;

        assert(vaExportSurfaceHandle(vactx->display, surface_id,
                                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                     export_flags,
                                     &va_desc) == VA_STATUS_SUCCESS);
        assert(vaSyncSurface(vactx->display, surface_id) == VA_STATUS_SUCCESS);

        for (int i = 0; i < 2; i++) {
            uint32_t object_index = va_desc.layers[i].object_index[0];
            EGLint attribs[] = {
                EGL_LINUX_DRM_FOURCC_EXT,
                (EGLint)va_desc.layers[i].drm_format,
                EGL_WIDTH,
                (EGLint)(va_desc.width / (1 + i)),
                EGL_HEIGHT,
                (EGLint)(va_desc.height / (1 + i)),
                EGL_DMA_BUF_PLANE0_FD_EXT,
                (EGLint)va_desc.objects[object_index].fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                (EGLint)va_desc.layers[i].offset[0],
                EGL_DMA_BUF_PLANE0_PITCH_EXT,
                (EGLint)va_desc.layers[i].pitch[0],
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                (EGLint)(va_desc.objects[object_index].drm_format_modifier &
                         0xffffffff),
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                (EGLint)(va_desc.objects[object_index].drm_format_modifier >>
                         32),
                EGL_NONE};

            EGLImageKHR image;
            image = CreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
            assert(image);

            planes[i].active(0);
            EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

            assert(eglDestroyImageKHR(eglGetCurrentDisplay(), image) ==
                   EGL_TRUE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        for (unsigned int i = 0; i < va_desc.num_objects; i++)
            close(va_desc.objects[i].fd);
    }
    static PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
    static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC EGLImageTargetTexture2DOES;

    gl::program &get_program() override
    {
        static gl::program nv12(vertex, fragment_nv12);
        return nv12;
    }

    static bool initialize_extensions()
    {
        if (CreateImageKHR)
            return true;

        if (!egl::has_extension("EGL_KHR_image_base") ||
            !gl::has_extension("GL_OES_EGL_image"))
            return false;

        CreateImageKHR =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR =
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        EGLImageTargetTexture2DOES =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
                "glEGLImageTargetTexture2DOES");
        return true;
    }
};

PFNEGLCREATEIMAGEKHRPROC vaapi::CreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC vaapi::eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC vaapi::EGLImageTargetTexture2DOES;
#endif

video *create_video_from_frame(const av::frame &f)
{
    AVPixelFormat format = (AVPixelFormat)f.f->format;

    switch (format) {
    case AV_PIX_FMT_VAAPI:
#if HAVE_VA
        if (vaapi::initialize_extensions()) {
            fmt::print(stderr, "Using VAAPI GL Interop\n");
            return new vaapi(f);
        }
#endif
        return create_video_from_frame(f.transfer());
    case AV_PIX_FMT_NV12:
        return new nv12(f);
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return new yuv(f);
    default:;
    }
    return nullptr;
}

static bool decode_video(av::input &video, queue &qframe, av::hw_device &hw)
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
        fmt::print(stderr, "no vaapi HW decoder available\n");

    decode_video(video, qframe, hw);

    qframe.stop();
}

int main(int argc, char *argv[])
{
    std::string videoname = "pipe:0";
    queue qframe;

    if (argc > 1)
        videoname = argv[1];

    av::input video;
    if (!video.open(videoname))
        return -1;

    std::thread read =
        std::thread(read_video, std::ref(video), std::ref(qframe));

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

    fmt::print(stderr, "SDL with egl  : {:d}x{:d}\n", width, height);
    fmt::print(stderr, "EGL version   : {:s}\n", egl::version());
    fmt::print(stderr, "OpenGL version: {:s}\n", gl::version());

    init_quad();

    auto v = create_video_from_frame(first);

    bool run = true;
    AVRational time_base = video.time_base(0);

    Uint32 start_tick = SDL_GetTicks();
    while (run && !!qframe) {
        int64_t pts = av_rescale(SDL_GetTicks() - start_tick, time_base.den,
                                 time_base.num * 1000) +
                      first_pts;
        SDL_Event e;

        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_KEYDOWN:
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    run = false;
                    break;
                default:;
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
