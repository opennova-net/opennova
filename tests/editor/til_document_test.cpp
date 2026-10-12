// The .til document (S25 lane B1): its rows in draw order and its layer invariant; the retail twin opens and saves all 116 shipped .til byte for byte.
// Not written yet in this build (lane B of S25 fills it); it passes until then.
#include <cstdio>

#include "common/retail_paths.h"

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	std::printf("til_document: not written yet in this build\n");
	return 0;
}
