// The indexed PCX forms the shipped JO:CA files take (fixtures/terrain/variants, minted by
// tests/fixtures/terrain_variants_gen.cpp: one file per form the extracted tree's 206 PCX use) through the carried
// form. The char map (_m.pcx) and the foliage map (_f.pcx) read with their carried form (the _f maps' leftover EGA
// entries among the carried fields; rows both coders write alike, the header picking the shipped coding) and,
// written again in that form, are the same bytes. Then every PCX there that the editor paints as a plane (an 8-bit,
// one-plane PCX whose rows are its width, editor/documents/texture_raster.cpp read_pcx8) is written again as the
// same bytes: the raster core's claim that a file read and written again is its bytes. The three-plane 24-bit form
// is held whole. The bytes written again are pinned (FNV-1a64): a changed vector is a changed PCX writer and needs
// an [orig] witness or a D-entry in the same commit (`pcx_shipped_forms_test --dump` prints the table).

#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

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

bool ends(const std::string &l, const char *tail) {
	const std::string t(tail);
	return l.size() >= t.size() && l.compare(l.size() - t.size(), t.size(), t) == 0;
}

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

// The bytes each paintable form writes again (its own bytes).
const Vector kExpected[] = {
	{"charmap_m.pcx", 11594u, 0xea6043e171796f3eull},
	{"foliage_f.pcx", 3907u, 0xc127bb5904e9491full},
	{"grey_info2.pcx", 5393u, 0xcfb30b3e602a8758ull},
	{"picture.pcx", 3699u, 0xb81418e3d3ef8c1dull},
};

uint64_t fnv(const std::vector<uint8_t> &bytes) {
	return opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size());
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	const std::string dir = test_paths_fixture("terrain/variants");
	std::vector<std::filesystem::path> files;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir))
		if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".pcx") files.push_back(entry.path());
	std::sort(files.begin(), files.end());

	int failures = 0, swept = 0, foliage = 0;
	for (const std::filesystem::path &path : files) {
		const std::string name = path.filename().string();
		const std::string l = lower(name);
		if (!ends(l, "_m.pcx") && !ends(l, "_f.pcx")) continue;
		const std::vector<uint8_t> bytes = test_io::read_file(path.string());
		opennova::IndexedImage8 image;
		opennova::PcxIndexedForm form;
		std::string error;
		if (!opennova::decode_pcx_indexed(bytes.data(), bytes.size(), image, error, &form)) {
			std::printf("FAIL: %s: %s\n", name.c_str(), error.c_str());
			++failures;
			continue;
		}
		++swept;
		const bool is_foliage = ends(l, "_f.pcx");
		foliage += is_foliage;
		// The shipped maps' rows: each ends in a run, so both coders write them alike (the shipped tool's own tail
		// rule is unwitnessed) and the header (72 dpi, not MED's) reads them as the shipped coding.
		opennova::PcxIndexedForm as_med = form;
		as_med.coding = opennova::PcxCoding::Med;
		std::vector<uint8_t> med_again;
		if (form.coding != opennova::PcxCoding::Shipped || !opennova::encode_pcx_indexed(image, med_again, error, &as_med) ||
		    med_again != bytes) {
			std::printf("FAIL: %s: its rows are not the coding both coders write alike\n", name.c_str());
			++failures;
		}
		// The char maps' EGA palette all 0xFF; the foliage maps' its leftover entries, carried as read.
		const bool ega_filled = form.ega == opennova::pcx_ega_filled(0xFF);
		if (form.hdpi != 72 || form.vdpi != 72 || form.palette_info != 1 || ega_filled == is_foliage) {
			std::printf("FAIL: %s: the header is not the shipped %s map's\n", name.c_str(), is_foliage ? "foliage" : "char");
			++failures;
		}
	}
	if (swept != 2 || foliage != 1) {
		std::printf("FAIL: %d terrain maps (%d foliage) under %s, not the char and foliage forms\n", swept, foliage,
		            dir.c_str());
		return 1;
	}
	// Every paintable PCX there, the terrain maps among them.
	int paintable = 0, whole = 0;
	std::vector<std::pair<std::string, std::vector<uint8_t>>> written;
	for (const std::filesystem::path &path : files) {
		const std::string name = path.filename().string();
		const std::vector<uint8_t> bytes = test_io::read_file(path.string());
		opennova::PcxHeader header;
		opennova::IndexedImage8 image;
		opennova::PcxIndexedForm form;
		std::string error;
		if (!opennova::pcx_read_header(bytes.data(), bytes.size(), header) || header.bits != 8 || header.planes != 1 ||
		    opennova::pcx_row_fit(header) != opennova::PcxRowFit::Exact ||
		    !opennova::decode_pcx_indexed(bytes.data(), bytes.size(), image, error, &form) || image.empty()) {
			++whole;
			continue;
		}
		++paintable;
		std::vector<uint8_t> again;
		if (!opennova::encode_pcx_indexed(image, again, error, &form) || again != bytes) {
			std::printf("FAIL: %s written again in its form differs (%zu bytes, written %zu)\n", name.c_str(), bytes.size(),
			            again.size());
			++failures;
		}
		// The picture's rows end in a lone byte, which MED's coder writes as a pair: the shipped coding alone. The
		// sky map's palette info 2 is carried.
		if (name == "picture.pcx" && form.coding != opennova::PcxCoding::Shipped) ++failures, std::printf("FAIL: picture.pcx's coding\n");
		if (name == "grey_info2.pcx" && form.palette_info != 2) ++failures, std::printf("FAIL: grey_info2.pcx's palette info\n");
		written.emplace_back(name, again);
	}
	if (paintable != 4 || whole != 1) {
		std::printf("FAIL: %d paintable PCX (%d held whole), not the four indexed forms and the 24-bit one\n", paintable,
		            whole);
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
	std::printf("%s: %d terrain maps (%d foliage); %d paintable PCX written again (%d held whole), %d failures\n",
	            failures == 0 ? "OK" : "FAIL", swept, foliage, paintable, whole, failures);
	return failures == 0 ? 0 : 1;
}
