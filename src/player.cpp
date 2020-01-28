#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <list>

#include "vc/sdl.hpp"
#include "vc/gl.hpp"
#include "ffmpeg.hpp"

#include <SDL2/SDL.h>

extern "C" {
#include <libavutil/pixdesc.h>
}

int width = 800;
int height = 600;

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

static const std::string fragment = R""(
#version 300 es

        precision highp float;

        in vec2 v_uv;
        out vec4 color;
        void main() {
                color = vec4(1., 0., 0., 1.);
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
	queue() : is_ok(true) {}

	void push(av::frame &f) {
		std::lock_guard<std::mutex> l(m);

		filled.push_back(f);
	}
	bool get(int64_t pts, av::frame &f) {
		std::lock_guard<std::mutex> l(m);
		bool ret = false;

		while (!filled.empty() && filled.front().f->pts <= pts) {
			f = std::move(filled.front());
			filled.pop_front();

			ret = true;
		}
		return ret;
	}

	bool operator!() { return !is_ok; }
	void stop() { is_ok = false; }

	bool is_ok;
	std::mutex m;
	std::list<av::frame> filled;
};

static void read_video(av::input &video, queue &qframe)
{
	av::packet p;
	av::decoder dec = video.get(0);
	if (!dec)
		goto out;

	while (video >> p && !!qframe) {
		if (p.stream_index() != 0)
			continue;

		if (!(dec << p))
			return;

		av::frame f;

		while (dec >> f) {
			std::cerr << "send a frame pts: " << f.f->pts << " fmt: "
				  << av_get_pix_fmt_name(AVPixelFormat(f.f->format)) << std::endl;

			qframe.push(f);
		}
	}
out:
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

	if (!init_sdl(argv[0], width, height))
		return -1;

	shaders red;
	if (!red.init(vertex, fragment))
		return -1;

	red.use();

	init_quad();

	bool run = true;
	int64_t pts = 0;
	while (run && !!qframe) {
		SDL_Event e;

                while (SDL_PollEvent(&e)) {
                        switch (e.type) {
                        case SDL_QUIT:
                                run = false;
                                break;
                        }
                }

		av::frame f;
		if (qframe.get(pts, f)) {
			std::cerr << "receive a frame pts: " << f.f->pts << " fmt: "
				  << av_get_pix_fmt_name(AVPixelFormat(f.f->format)) << std::endl;

			pts = f.f->pts + 40;
		}

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		SDL_GL_SwapWindow(SDL_GL_GetCurrentWindow());
	}
	qframe.stop();
	read.join();
	return 0;
}
