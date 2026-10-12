// The heights document (S25 lane A2): a .cpt or a heightmap input as its CDEP atlas; the retail twin opens and saves every JO:CA .cpt byte for byte.
// Not written yet in this build (lane A of S25 fills it); it passes until then.
#include <cstdio>

#include "common/retail_paths.h"

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	std::printf("terrain_heights_document: not written yet in this build\n");
	return 0;
}
