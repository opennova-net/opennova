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

std::string path(const char *name) { return std::string(MINIMAL_FIXTURE_DIR) + "/" + name; }

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
	art.push_back({"mnml_m.pcx", make_pcx(256, 256, 0), true});             // charmap (surface 0)
	art.push_back({"mnml_f.pcx", make_pcx(256, 256, 0), true});             // foliagemap (none)

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
			CHECK(decoded.width == 256 && decoded.height == 256,
			      (std::string(a.name) + " decodes to 256x256").c_str());
		}
	}

	if (fail == 0) std::printf("OK: minimal terrain source art authored + valid\n");
	return fail == 0 ? 0 : 1;
}
