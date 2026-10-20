// The true-colour TGA forms the shipped JO:CA files take (fixtures/terrain/variants, minted by
// tests/fixtures/terrain_variants_gen.cpp: one file per form the extracted tree's 770 TGAs use) through the carried
// form. The colour map (_c.tga), the detail blend map (_d1.tga) and the tile atlas (trntile*.tga) each read a form
// that ends with the TGA 2.0 footer and, decoded the way the game's reader decodes it and written again in that
// form, are the same bytes. Then every TGA there that the editor paints as a plane (a form tga_read_form reads,
// editor/documents/texture_raster.cpp read_tga) is written again as the same bytes, the one without the footer
// among them: the raster core's claim that a file read and written again is its bytes. (The forms the writer does
// not make read no form: tga_roundtrip.) The bytes written again are pinned (FNV-1a64): a changed vector is a
// changed TGA writer and needs an [orig] witness or a D-entry in the same commit (`--dump` prints the table).

#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>

#include <base/io/hash.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/test_paths.h"

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool ends_with(const std::string &s, const char *tail) {
	const std::string t(tail);
	return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

const Vector kExpected[] = {
	{"bare24.tga", 402u, 0x426a493baf5e203full},
	{"map_c.tga", 428u, 0x9dd9a2ce4e8d3b9aull},
	{"map_d1.tga", 300u, 0x0ada306636ecfbe2ull},
	{"trntile_a.tga", 556u, 0x9c5e9ebc6956d3faull},
};

uint64_t fnv(const std::vector<uint8_t> &bytes) {
	return opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size());
}

// The file decoded as the game decodes it, written again in `form`.
bool written_again(const std::vector<uint8_t> &bytes, const opennova::tga::TgaForm &form, std::vector<uint8_t> &again) {
	opennova::tga::TgaImage image;
	std::string error;
	return opennova::tga::tga_decode_retail(bytes.data(), bytes.size(), image, error) &&
	       opennova::tga::tga_write(image.rgba.data(), uint32_t(image.width), uint32_t(image.height), form, again, error);
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	const std::string dir = test_paths_fixture("terrain/variants");
	std::vector<std::filesystem::path> files;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir))
		if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".tga") files.push_back(entry.path());
	std::sort(files.begin(), files.end());

	int failures = 0, colour = 0, blend = 0, atlas = 0;
	for (const std::filesystem::path &path : files) {
		const std::string name = path.filename().string();
		const std::string l = lower(name);
		const bool is_colour = ends_with(l, "_c.tga"), is_blend = ends_with(l, "_d1.tga");
		const bool is_atlas = l.rfind("trntile", 0) == 0;
		if (!is_colour && !is_blend && !is_atlas) continue;
		const std::vector<uint8_t> bytes = test_io::read_file(path.string());
		opennova::tga::TgaForm form;
		if (!opennova::tga::tga_read_form(bytes.data(), bytes.size(), form) || !form.footer) {
			std::printf("FAIL: %s: no form with the TGA 2.0 footer\n", name.c_str());
			++failures;
			continue;
		}
		colour += is_colour, blend += is_blend, atlas += is_atlas;
		// The colour maps 24 bits (no alpha); the blend map's weights and the atlas's alpha at 32.
		if ((is_colour && (form.bits != 24 || form.descriptor != 0)) ||
		    (!is_colour && (form.bits != 32 || form.descriptor != opennova::tga::TGA_DESCRIPTOR_ALPHA_8))) {
			std::printf("FAIL: %s: depth %u descriptor 0x%02x is not its role's\n", name.c_str(), form.bits, form.descriptor);
			++failures;
		}
		std::vector<uint8_t> again;
		if (!written_again(bytes, form, again) || again != bytes) {
			std::printf("FAIL: %s written again in its form differs (%zu bytes, written %zu)\n", name.c_str(), bytes.size(),
			            again.size());
			++failures;
		}
	}
	if (colour != 1 || blend != 1 || atlas != 1) {
		std::printf("FAIL: not one each of _c.tga, _d1.tga and trntile*.tga under %s\n", dir.c_str());
		return 1;
	}
	// Every paintable TGA there, the terrain maps among them.
	int paintable = 0, bare = 0;
	std::vector<std::pair<std::string, std::vector<uint8_t>>> written;
	for (const std::filesystem::path &path : files) {
		const std::string name = path.filename().string();
		const std::vector<uint8_t> bytes = test_io::read_file(path.string());
		opennova::tga::TgaForm form;
		if (!opennova::tga::tga_read_form(bytes.data(), bytes.size(), form)) {
			std::printf("FAIL: %s reads no form\n", name.c_str());
			++failures;
			continue;
		}
		++paintable;
		bare += !form.footer;
		std::vector<uint8_t> again;
		if (!written_again(bytes, form, again) || again != bytes) {
			std::printf("FAIL: %s written again in its form differs (%zu bytes, written %zu)\n", name.c_str(), bytes.size(),
			            again.size());
			++failures;
		}
		written.emplace_back(name, again);
	}
	if (paintable != 4 || bare != 1) {
		std::printf("FAIL: %d paintable TGA (%d without the footer), not the four forms\n", paintable, bare);
		return 1;
	}
	if (dump) {
		for (const auto &[name, bytes] : written)
			std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", name.c_str(), bytes.size(), (unsigned long long)fnv(bytes));
		return 0;
	}
	const size_t expected = sizeof(kExpected) / sizeof(kExpected[0]);
	if (written.size() != expected) {
		std::printf("FAIL: %zu forms written, %zu vectors committed\n", written.size(), expected);
		++failures;
	}
	for (size_t i = 0; i < written.size() && i < expected; ++i) {
		const uint64_t h = fnv(written[i].second);
		if (written[i].first != kExpected[i].name || written[i].second.size() != kExpected[i].len || h != kExpected[i].hash) {
			std::printf("FAIL: %s: len %zu hash 0x%016llx, expected %s len %zu hash 0x%016llx\n", written[i].first.c_str(),
			            written[i].second.size(), (unsigned long long)h, kExpected[i].name, kExpected[i].len,
			            (unsigned long long)kExpected[i].hash);
			++failures;
		}
	}
	std::printf("%s: %d colour map, %d detail blend map, %d tile atlas; %d paintable TGA written again (%d without the "
	            "footer), %d failures\n",
	            failures == 0 ? "OK" : "FAIL", colour, blend, atlas, paintable, bare, failures);
	return failures == 0 ? 0 : 1;
}
