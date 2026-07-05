// Generator + guard for the minimal map's source-art images the mnml.trn
// references: the color/detail/tilestrip TGAs and the charmap/foliagemap PCX
// surface maps. Authored small from scratch (no retail asset) — solid fills for
// the flat map. TGA is hand-rolled (uncompressed type-2 BGR, an 18-byte header);
// the PCX maps go through libs/pcx encode_pcx_indexed. Dimensions are a modest
// minimal guess; which retail requires vs tolerates is validated at the retail
// launch (see fixtures/minimal/README.md). Emit with
// OPENNOVA_WRITE_MINIMAL_FIXTURES=1; else guard each decodes/round-trips.
#include <pcx/pcx.h>
#include <pcx/pcx_io.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

std::string path(const char *name) { return std::string(MINIMAL_FIXTURE_DIR) + "/resources/" + name; }

bool write_bytes(const std::string &p, const std::vector<uint8_t> &b) {
	std::ofstream o(p, std::ios::binary);
	if (!o) return false;
	if (!b.empty()) o.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
	return static_cast<bool>(o);
}

bool read_bytes(const std::string &p, std::vector<uint8_t> &b) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	b.resize(static_cast<size_t>(sz));
	if (!b.empty()) f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
	return true;
}

bool is_lfs_pointer(const std::vector<uint8_t> &b) {
	static const char k[] = "version https://git-lfs";
	return b.size() >= sizeof(k) - 1 && std::equal(k, k + sizeof(k) - 1, b.begin());
}

// A solid-fill uncompressed 24-bit TGA (type 2), the simplest retail-loadable
// image. 18-byte header, then w*h BGR triples.
std::vector<uint8_t> make_tga(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
	std::vector<uint8_t> out;
	const uint8_t header[18] = {
	    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    static_cast<uint8_t>(w & 0xFF), static_cast<uint8_t>((w >> 8) & 0xFF),
	    static_cast<uint8_t>(h & 0xFF), static_cast<uint8_t>((h >> 8) & 0xFF),
	    24, 0};
	out.insert(out.end(), header, header + 18);
	out.reserve(out.size() + static_cast<size_t>(w) * h * 3);
	for (int i = 0; i < w * h; ++i) {
		out.push_back(b);
		out.push_back(g);
		out.push_back(r);
	}
	return out;
}

// The menu cursor the .mnu <CURSOR> block names (newarow1.tga — retail
// FILENAME, our bytes). Retail's is a 32x32 uncompressed type-2 BGRA TGA
// (bpp=32, desc=0x08: 8 alpha bits, bottom-up rows); STANDARD_TRANSPARENT
// keys off alpha. A classic pointer: white fill, black outline, hotspot at
// the visual top-left.
std::vector<uint8_t> make_cursor_tga() {
	static const char *kArrow[19] = {
	    "#          ",
	    "##         ",
	    "#.#        ",
	    "#..#       ",
	    "#...#      ",
	    "#....#     ",
	    "#.....#    ",
	    "#......#   ",
	    "#.......#  ",
	    "#........# ",
	    "#.....#####",
	    "#..#..#    ",
	    "#.# #..#   ",
	    "##  #..#   ",
	    "#    #..#  ",
	    "     #..#  ",
	    "      #..# ",
	    "      #..# ",
	    "       ##  ",
	};
	const int w = 32, h = 32;
	std::vector<uint8_t> out;
	const uint8_t header[18] = {
	    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    static_cast<uint8_t>(w & 0xFF), static_cast<uint8_t>((w >> 8) & 0xFF),
	    static_cast<uint8_t>(h & 0xFF), static_cast<uint8_t>((h >> 8) & 0xFF),
	    32, 0x08};
	out.insert(out.end(), header, header + 18);
	out.resize(out.size() + static_cast<size_t>(w) * h * 4, 0); // all transparent
	uint8_t *px = out.data() + 18;
	for (int y = 0; y < 19; ++y) {
		const char *row = kArrow[y];
		for (int x = 0; row[x]; ++x) {
			if (row[x] == ' ') continue;
			const bool outline = row[x] == '#';
			// Bottom-up row order (desc bit 5 clear, matching retail).
			uint8_t *p = px + ((static_cast<size_t>(h - 1 - y)) * w + x) * 4;
			p[0] = p[1] = p[2] = outline ? 0 : 0xFF; // BGR
			p[3] = 0xFF;                             // opaque
		}
	}
	return out;
}

// A solid indexed PCX with a grayscale palette — for the surface/foliage maps.
std::vector<uint8_t> make_pcx(int w, int h, uint8_t index) {
	IndexedImage8 img;
	img.width = w;
	img.height = h;
	img.indices.assign(static_cast<size_t>(w) * h, index);
	for (int i = 0; i < 256; ++i) {
		img.palette[i][0] = static_cast<uint8_t>(i);
		img.palette[i][1] = static_cast<uint8_t>(i);
		img.palette[i][2] = static_cast<uint8_t>(i);
	}
	std::vector<uint8_t> out;
	std::string err;
	if (!encode_pcx_indexed(img, out, err)) {
		std::fprintf(stderr, "FAIL: encode_pcx %dx%d: %s\n", w, h, err.c_str());
		++fail;
	}
	return out;
}

struct Art {
	const char *name;
	std::vector<uint8_t> bytes;
	bool is_pcx; // else TGA
	int w = 0, h = 0; // PCX decode-check dims (0 = skip)
};

} // namespace

int main() {
	const bool write_mode = std::getenv("OPENNOVA_WRITE_MINIMAL_FIXTURES") != nullptr;

	// The art the mnml.trn names. Small solid fills for the flat map: a dirt-ish
	// colormap, neutral detail maps, a plain tilestrip, an all-surface-0 charmap,
	// and an all-0 (no-foliage) foliagemap.
	std::vector<Art> art;
	art.push_back({"mnml_c.tga", make_tga(64, 64, 120, 96, 72), false});   // colormap
	art.push_back({"mnml_dm.tga", make_tga(64, 64, 128, 128, 128), false}); // detailmap
	art.push_back({"mnml_dc1.tga", make_tga(64, 64, 128, 128, 128), false}); // detailmap_c1
	art.push_back({"mnml_t.tga", make_tga(64, 64, 110, 90, 64), false});    // tilestrip
	art.push_back({"mnml_m.pcx", make_pcx(256, 256, 0), true, 256, 256});   // charmap (surface 0)
	art.push_back({"mnml_f.pcx", make_pcx(256, 256, 0), true, 256, 256});   // foliagemap (none)
	art.push_back({"newarow1.tga", make_cursor_tga(), false});              // menu cursor
	// The mission-family map overview (retail: 800x600 8-bit indexed PCX in
	// language.pff) — a solid placeholder until the map render leg.
	art.push_back({"mnml.pcx", make_pcx(800, 600, 96), true, 800, 600});

	for (Art &a : art) {
		const std::string p = path(a.name);
		if (write_mode) {
			CHECK(write_bytes(p, a.bytes), (std::string("write ") + a.name).c_str());
			std::printf("wrote %s (%zu bytes)\n", a.name, a.bytes.size());
			continue;
		}
		std::vector<uint8_t> committed;
		CHECK(read_bytes(p, committed),
		      (std::string("committed ") + a.name + " missing — run with OPENNOVA_WRITE_MINIMAL_FIXTURES=1")
		          .c_str());
		if (committed.empty()) continue;
		if (is_lfs_pointer(committed)) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", a.name);
			continue;
		}
		CHECK(committed == a.bytes,
		      (std::string(a.name) + " committed bytes differ from the writer — regenerate with "
		                             "OPENNOVA_WRITE_MINIMAL_FIXTURES=1")
		          .c_str());
		// The PCX maps must also decode back (the TGAs are trivially valid).
		if (a.is_pcx) {
			IndexedImage8 decoded;
			std::string err;
			CHECK(decode_pcx_indexed(committed.data(), committed.size(), decoded, err), err.c_str());
			CHECK(decoded.width == a.w && decoded.height == a.h,
			      (std::string(a.name) + " decodes to its authored dimensions").c_str());
		}
	}

	if (fail == 0) std::printf("OK: minimal terrain source art authored + valid\n");
	return fail == 0 ? 0 : 1;
}
