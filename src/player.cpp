#include "vc/sdl.hpp"
#include "vc/gl.hpp"

#include <SDL2/SDL.h>

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

int main(int argc, char* argv[])
{
	if (!init_sdl(argv[0], width, height))
		return -1;

	shaders red;
	if (!red.init(vertex, fragment))
		return -1;

	red.use();

	init_quad();

	bool run = true;
	while (run) {
		SDL_Event e;

                while (SDL_PollEvent(&e)) {
                        switch (e.type) {
                        case SDL_QUIT:
                                run = false;
                                break;
                        }
                }

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		SDL_GL_SwapWindow(SDL_GL_GetCurrentWindow());
	}
	return 0;
}
