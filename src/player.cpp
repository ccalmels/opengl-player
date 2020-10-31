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

static int width;
static int height;

static const std::string vertex = R""(
#version 450

	layout (location = 0) in vec2 va_position;
	out vec2 v_uv;

	void main() {
		v_uv = va_position;
		v_uv.y = -v_uv.y;
		v_uv = (v_uv + vec2(1.0)) * 0.5;

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

	void update(const av::frame &f) {
		av::frame sw_frame;

		if (f.is_hardware())
			sw_frame = f.transfer();
		else
			sw_frame = f;

		for (size_t i = 0; i < planes.size(); i++)
			planes[i].update(sw_frame.f->data[i]);
	}

	gl::program &get_program() {
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

	void update(const av::frame &f) {
		for (size_t i = 0; i < planes.size(); i++)
			planes[i].update(f.f->data[i]);
	}

	gl::program &get_program() {
		static gl::program yuv(vertex, fragment_yuv);
		return yuv;
	}
};

video *create_video_from_frame(const av::frame &f)
{
	AVPixelFormat format = (AVPixelFormat)f.f->format;

	switch (format) {
	case AV_PIX_FMT_VAAPI_VLD:
	case AV_PIX_FMT_CUDA:
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

	av::frame &first = qframe.filled.front();
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
