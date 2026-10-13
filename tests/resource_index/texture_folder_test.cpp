// One folder's files as opennova-3di's scene looks textures up in them
// (base/resource_index/texture_folder.h): every regular file keyed by its lower-case name with its
// path, a folder left out. Tooling, not a retail port, so it is pinned here through the public API
// (ADR 0018).
#include <cstdio>
#include <fstream>
#include <string>

#include <base/resource_index/texture_folder.h>

#include "common/temp_dir.h"

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

int main() {
	test_temp::TempDir dir("texture_folder");
	for (const char *name : { "Body.TGA", "skin_O.dds", "NOTES.txt" })
		std::ofstream(dir.file(name), std::ios::binary) << "x";
	std::filesystem::create_directories(dir.path / "Sub.tga");
	const TextureFolder listing = list_texture_folder(dir.path);
	CHECK(listing.size() == 3);
	CHECK(listing.count("body.tga") == 1 && listing.count("skin_o.dds") == 1 &&
			listing.count("notes.txt") == 1);
	CHECK(listing.count("sub.tga") == 0);
	const auto body = listing.find("body.tga");
	CHECK(body != listing.end() && body->second.size() >= 8 &&
			body->second.compare(body->second.size() - 8, 8, "Body.TGA") == 0);
	CHECK(list_texture_folder(dir.path / "missing").empty());
	if (failures == 0)
		std::printf("texture_folder: PASS\n");
	return failures == 0 ? 0 : 1;
}
