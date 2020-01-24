#include "sdl.hpp"

#include <SDL2/SDL.h>

#include <iostream>

static SDL_Window *window;
static SDL_GLContext glcontext;

static void exit_sdl()
{
	SDL_GL_DeleteContext(glcontext);
	SDL_DestroyWindow(window);
}

static void print_gl_version()
{
        int major, minor, profile;

        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);

	std::cerr << "OpenGL" << ((profile == SDL_GL_CONTEXT_PROFILE_ES)? " ES " : " ")
		  << major << '.' << minor << std::endl;
}

bool init_sdl(const std::string &name, int &width, int &height, int flags)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::cerr << "SDL_Init: " << SDL_GetError() << std::endl;
		return false;
	}

	window = SDL_CreateWindow(
		name.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, SDL_WINDOW_OPENGL | flags);
	if (!window) {
		std::cerr << "SDL_CreateWindow: " << SDL_GetError() << std::endl;
		return false;
	}

	SDL_GetWindowSize(window, &width, &height);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
			    SDL_GL_CONTEXT_PROFILE_ES);

	glcontext = SDL_GL_CreateContext(window);
	if (!glcontext) {
		std::cerr << "SDL_GL_CreateContext: " << SDL_GetError()
			  << std::endl;
		SDL_DestroyWindow(window);
		window = nullptr;
		return false;
	}

	if (SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1))
		std::cerr << "SDL_GL_DOUBLEBUFFER: " << SDL_GetError()
			  << std::endl;

	if (SDL_GL_SetSwapInterval(1))
		std::cerr << "SDL_GL_SetSwapInterval: " << SDL_GetError()
			  << std::endl;

	if (atexit(exit_sdl))
		std::cerr << "registering sdl cleanup fails" << std::endl;

	std::cerr << "SDL window " << width << "x" << height << " with ";
	print_gl_version();
	return true;
}
