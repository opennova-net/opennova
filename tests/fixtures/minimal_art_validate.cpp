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
#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

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

// The menu cursor is the one image whose FORMAT retail depends on, not just its presence: it
// is drawn alpha-keyed (STANDARD_TRANSPARENT), so it has to be an uncompressed truecolor TGA
// (type 2) at 32 bpp with an 8-bit alpha channel, stored bottom-up like retail's own, and at
// the cursor size the .mnu <CURSOR> block assumes. A 24 bpp or alpha-stripped re-export still
// "decodes" and then draws as an opaque square -- or nothing -- and nothing is clickable.
const int kCursorSize = 32;

void check_cursor(const char *name, const std::vector<uint8_t> &b, int w, int h, int bpp) {
	const uint8_t image_type = b.size() >= 3 ? b[2] : 0;
	const uint8_t descriptor = b.size() >= 18 ? b[17] : 0;
	const int alpha_bits = descriptor & 0x0F;
	const bool top_down = (descriptor & 0x20) != 0;
	std::string msg;
	msg = std::string(name) + " is an uncompressed truecolor TGA (image type 2)";
	CHECK(image_type == 2, msg.c_str());
	msg = std::string(name) + " is 32 bpp with an 8-bit alpha channel (retail keys the cursor on alpha)";
	CHECK(bpp == 32 && alpha_bits == 8, msg.c_str());
	msg = std::string(name) + " stores rows bottom-up, as retail's cursor art does";
	CHECK(!top_down, msg.c_str());
	char size_msg[128];
	std::snprintf(size_msg, sizeof(size_msg), "%s is %dx%d (got %dx%d)", name, kCursorSize, kCursorSize, w, h);
	CHECK(w == kCursorSize && h == kCursorSize, size_msg);
	// The pixel payload must actually be there: 18-byte header + w*h*4.
	msg = std::string(name) + " carries a full 32 bpp pixel payload";
	CHECK(b.size() >= 18u + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, msg.c_str());
}

struct Art {
	const char *name;
	bool is_pcx;
	int min_size;   // 0 = any
	bool is_cursor; // the alpha-keyed menu cursor: format-checked, not just decoded
};

} // namespace

int main() {
	// Every image mnml.trn references, plus the two the menu/mission families load by name.
	const Art art[] = {
	    {"mnml_c.tga", false, kColormapMinSize, false}, // colormap - the quadrant-split one
	    {"mnml_dm.tga", false, 0, false},               // detailmap
	    {"mnml_dc1.tga", false, 0, false},              // detailmap_c1
	    {"mnml_dc2.tga", false, 0, false},              // detailmap_c2
	    {"mnml_dc3.tga", false, 0, false},              // detailmap_c3
	    {"mnml_dmd.tga", false, 0, false},              // detailmapdist
	    {"mnml_d1.tga", false, 0, false},               // detailblendmap
	    {"mnml_t.tga", false, 0, false},                // tilestrip
	    {"mnml_m.pcx", true, 0, false},                 // charmap (surface ids)
	    {"mnml_f.pcx", true, 0, false},                 // foliagemap
	    {"newarow1.tga", false, 0, true},               // menu cursor
	    {"mnml.pcx", true, 0, false},                   // mission-family map overview
	};

	for (const Art &a : art) {
		std::vector<uint8_t> bytes;
		CHECK(read_bytes(path(a.name), bytes), (std::string("committed ") + a.name + " missing").c_str());
		// An empty file is not an image; it must not pass by having nothing to decode.
		CHECK(!bytes.empty(), (std::string("committed ") + a.name + " is empty").c_str());
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
		if (a.is_cursor) check_cursor(a.name, bytes, w, h, bpp);
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
