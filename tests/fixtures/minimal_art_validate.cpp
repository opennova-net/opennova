// Validator for the minimal map's source art: the images mnml.trn names, plus the menu cursor
// and the mission-family map overview.
//
// This used to be a GENERATOR: it filled each image with a solid colour and asserted the
// committed bytes matched. The terrain art is now authored in ONED and exported through the
// engine's own terrain writer, so the byte guard only asserted that the throwaway C++ writer
// still agreed with itself -- and it went red the moment the editor authored a real colormap,
// which is the wrong signal entirely.
//
// What is worth guarding is what RETAIL needs from these files: that each one is present, that
// it decodes, and that the colormap is big enough to be split.
#include <pcx/pcx.h>
#include <pcx/pcx_io.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int fail = 0;

#define CHECK(cond, msg)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			std::fprintf(stderr, "FAIL: %s\n", (msg));    \
			++fail;                                       \
		}                                                 \
	} while (0)

// Retail quadrant-splits the terrain colormap into four 512x512 tiles, so anything smaller
// cannot split meaningfully -- the map spent its first life with a 64x64 colormap for exactly
// this reason, and it read as one flat wash of colour.
const int kColormapMinSize = 512;

std::string path(const char *name) { return std::string(GAME_ASSETS_DIR) + "/" + name; }

bool read_bytes(const std::string &p, std::vector<uint8_t> &out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

bool is_lfs_pointer(const std::vector<uint8_t> &b) {
	static const char kSentinel[] = "version https://git-lfs";
	const size_t n = sizeof(kSentinel) - 1;
	return b.size() >= n && std::equal(kSentinel, kSentinel + n, b.begin());
}

// An uncompressed TGA's 18-byte header carries width/height at +12 and bit depth at +16.
bool tga_dimensions(const std::vector<uint8_t> &b, int &w, int &h, int &bpp) {
	if (b.size() < 18) return false;
	w = b[12] | (b[13] << 8);
	h = b[14] | (b[15] << 8);
	bpp = b[16];
	return w > 0 && h > 0 && (bpp == 8 || bpp == 24 || bpp == 32);
}

struct Art {
	const char *name;
	bool is_pcx;
	int min_size; // 0 = any
};

} // namespace

int main() {
	// Every image mnml.trn references, plus the two the menu/mission families load by name.
	const Art art[] = {
	    {"mnml_c.tga", false, kColormapMinSize}, // colormap - the quadrant-split one
	    {"mnml_dm.tga", false, 0},               // detailmap
	    {"mnml_dc1.tga", false, 0},              // detailmap_c1
	    {"mnml_dc2.tga", false, 0},              // detailmap_c2
	    {"mnml_dc3.tga", false, 0},              // detailmap_c3
	    {"mnml_dmd.tga", false, 0},              // detailmapdist
	    {"mnml_d1.tga", false, 0},               // detailblendmap
	    {"mnml_t.tga", false, 0},                // tilestrip
	    {"mnml_m.pcx", true, 0},                 // charmap (surface ids)
	    {"mnml_f.pcx", true, 0},                 // foliagemap
	    {"newarow1.tga", false, 0},              // menu cursor
	    {"mnml.pcx", true, 0},                   // mission-family map overview
	};

	for (const Art &a : art) {
		std::vector<uint8_t> bytes;
		CHECK(read_bytes(path(a.name), bytes), (std::string("committed ") + a.name + " missing").c_str());
		if (bytes.empty()) continue;
		if (is_lfs_pointer(bytes)) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", a.name);
			continue;
		}

		if (a.is_pcx) {
			IndexedImage8 decoded;
			std::string err;
			CHECK(decode_pcx_indexed(bytes.data(), bytes.size(), decoded, err),
			      (std::string(a.name) + ": " + err).c_str());
			CHECK(decoded.width > 0 && decoded.height > 0,
			      (std::string(a.name) + " decodes to a non-empty image").c_str());
			continue;
		}

		int w = 0, h = 0, bpp = 0;
		CHECK(tga_dimensions(bytes, w, h, bpp),
		      (std::string(a.name) + " is a readable TGA").c_str());
		if (a.min_size > 0) {
			char msg[192];
			std::snprintf(msg, sizeof(msg),
			              "%s is at least %dx%d so retail can quadrant-split it (got %dx%d)",
			              a.name, a.min_size, a.min_size, w, h);
			CHECK(w >= a.min_size && h >= a.min_size, msg);
		}
	}

	if (fail == 0) std::printf("OK: minimal terrain source art present + valid\n");
	return fail == 0 ? 0 : 1;
}
