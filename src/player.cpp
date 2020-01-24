#include "vc/sdl.hpp"

int width = 800;
int height = 600;

int main(int argc, char* argv[])
{
	if (!init_sdl(argv[0], width, height))
		return -1;

	return 0;
}
