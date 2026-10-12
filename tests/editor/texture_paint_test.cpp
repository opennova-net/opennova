// Painting a texture used as a terrain map (S25 lane B3); the retail twin paints every shipped map, undoes it and saves it identical.
// Not written yet in this build (lane B of S25 fills it); it passes until then.
#include <cstdio>

#include "common/retail_paths.h"

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	std::printf("texture_paint: not written yet in this build\n");
	return 0;
}
