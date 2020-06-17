#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>
#include <cassert>
#include <atomic>

#include "sdl.hpp"
#include "gl.hpp"
#include "ffmpeg.hpp"

#include <SDL2/SDL.h>

static int width;
static int height;

static const std::string vertex = R""(
#version 300 es

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
#version 300 es

	precision highp float;

	in vec2 v_uv;
	out vec4 color;

	uniform sampler2D y_tex, u_tex, v_tex;

	const mat3 yuv2rgb = mat3(1.0, 1.0, 1.0,
				  0.0, -0.39465, 2.03211,
				  1.13983, -0.58060, 0.0);

	void main() {
		vec3 yuv, rgb;

		yuv.r = texture(y_tex, v_uv).r;
		yuv.g = texture(u_tex, v_uv).r - 0.5;
		yuv.b = texture(v_tex, v_uv).r - 0.5;

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

	void push(av::frame &f) {
		std::unique_lock<std::mutex> l(m);

		while (filled.size() >= size && is_ok)
			cv.wait(l);

		filled.push_back(f);
		cv.notify_one();
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

struct texture_yuv {
	texture_yuv() { w[0] = 0; h[0] = 0; }

	void update(const av::frame &f, int active) {
		if (!w[0])
			return create(f, active);

		for(int i = 0; i < 3; i++) {
			glActiveTexture(GL_TEXTURE0 + active + i);
			glBindTexture(GL_TEXTURE_2D, textures[i]);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w[i], h[i], GL_RED,
					GL_UNSIGNED_BYTE, f.f->data[i]);
		}
	}

	void create(const av::frame &f, int active) {
		w[0] = f.f->width;
		h[0] = f.f->height;

		w[1] = w[2] = f.f->width * 0.5;
		h[1] = h[2] = f.f->height * 0.5;

		glGenTextures(3, textures);

		for(int i = 0; i < 3; i++) {
			glActiveTexture(GL_TEXTURE0 + active + i);
			glBindTexture(GL_TEXTURE_2D, textures[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w[i], h[i] , 0, GL_RED,
				     GL_UNSIGNED_BYTE, f.f->data[i]);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
	}

	GLuint textures[3];
	int w[3], h[3];
};

static void decode_video(av::input &video, queue &qframe)
{
	av::packet p;
	av::frame f;
	av::decoder dec = video.get(0);
	if (!dec)
		return;

	while (video >> p && !!qframe) {
		if (p.stream_index() != 0)
			continue;

		if (!(dec << p))
			return;

		while (dec >> f)
			qframe.push(f);
	}
}

static void read_video(av::input &video, queue &qframe)
{
	decode_video(video, qframe);
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

	if (!qframe.wait())
		return -1;

	int64_t first_pts = qframe.filled.front().f->pts;
	width = qframe.filled.front().f->width;
	height = qframe.filled.front().f->height;

	if (!init_sdl(argv[0], width, height))
		return -1;

	shaders yuv;
	if (!yuv.init(vertex, fragment_yuv))
		return -1;

	yuv.use();
	yuv.set("y_tex", 0);
	yuv.set("u_tex", 1);
	yuv.set("v_tex", 2);

	init_quad();

	texture_yuv tex;
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
		if (qframe.get(pts, f))
			tex.update(f, 0);

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		SDL_GL_SwapWindow(SDL_GL_GetCurrentWindow());
	}
	qframe.stop();
	read.join();
	return 0;
}
