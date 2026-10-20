// The PFF writer against the retail packer's layout (ADR 0008 as amended 2026-10-15): an archive laid
// out byte by byte the way pack.exe lays out every JO:CA archive (tests/pff/pff_packer_archive.h: the
// payloads and the directory in the packer's list order, mixed-case names out of name order, each
// entry's time and checksum word, the footer c0a8a939 00000000 'KING'), written again through
// pff_write_archive_again (pff_apply_edits' write with nothing to apply), is its own bytes
// [orig: sub_4040E0 @ 0x4041A3..0x40432F (pack.exe)] (D-VFS-15). The archives are built from integer
// data, never read from an install: the retail row this replaces (pff_retail_rewrite) proved the
// writer on JO:CA's five archives once, and their shape is the builder's.
//
// The laid-out bytes are pinned by FNV-1a64 vectors (the nw_codec_identity pattern), so the builder
// cannot drift from the shape the writer is held to: a vector changes only with an [orig] witness or a
// D-entry for the packer's layout. `pff_packer_rewrite_test --dump` prints the replacement table.

#include "common/temp_dir.h"
#include "common/test_expect.h"
#include "pff_packer_archive.h"

#include "common/file_io.h"

#include <formats/pff/pff.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::pff;

namespace {

uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

struct Case {
	std::string name;
	std::vector<uint8_t> bytes;
};

pff_packer::Entry entry(const char *name, uint32_t seed, size_t size, uint32_t time) {
	pff_packer::Entry e;
	e.name = name;
	e.data = pff_packer::payload(seed, size);
	e.time = time;
	e.checksum = pff_packer::plain_checksum(e.data);
	return e;
}

// The JO:CA shapes: a base archive's mix of kinds, its entries in the packer's list order (no name
// order: the first sorts after the second), mixed case, times across retail's range (the oldest and
// newest stamps of the five archives at the ends); an expansion archive of a few entries; one entry.
std::vector<Case> build_corpus() {
	std::vector<Case> corpus;
	corpus.push_back({"base_archive",
			pff_packer::archive({
					entry("gmpv305.wav", 1, 97, 1069184788u),
					entry("Imdec13b.3di", 2, 64, 473418166u),
					entry("qd50_024.bad", 3, 33, 1125078666u),
					entry("I_walkf.bad", 4, 1, 1078812964u),
					entry("menu_style.mns", 5, 150, 1121734066u),
					entry("main.mnu", 6, 72, 1248404912u),
					entry("CP14.pcx", 7, 255, 1124836260u),
			})});
	corpus.push_back({"expansion_archive",
			pff_packer::archive({
					entry("jox01.bin", 8, 40, 1078812964u),
					entry("JOX01L.LWF", 9, 28, 1121734066u),
					entry("07TR.pcx", 10, 19, 1121734066u),
			})});
	corpus.push_back({"one_entry", pff_packer::archive({ entry("Trash02.3di", 11, 5, 1069184788u) })});
	return corpus;
}

const Vector kExpected[] = {
	{"base_archive", 956u, 0x5d8b851938d0f101ull},
	{"expansion_archive", 227u, 0x792e9af5e4aa1ba8ull},
	{"one_entry", 73u, 0x0a8486d7116261f2ull},
};

void dump(const std::vector<Case> &corpus) {
	std::printf("const Vector kExpected[] = {\n");
	for (const Case &c : corpus)
		std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", c.name.c_str(), c.bytes.size(),
				(unsigned long long)fnv1a64(c.bytes));
	std::printf("};\n");
}

// The archive at `path` written again is its own bytes; the first differing offset reported otherwise.
int check_written_again(const std::string &name, const std::vector<uint8_t> &bytes) {
	test_temp::TempDir dir("pff_packer_rewrite");
	const std::string path = dir.file("packed.pff");
	const std::string again = dir.file("again.pff");
	TEST_EXPECT(test_io::write_file(path, bytes));
	PffArchive archive{};
	TEST_EXPECT(pff_open(&archive, path.c_str()) == 0);
	TEST_EXPECT(pff_footer_kept_word(&archive) == 0);
	pff_close(&archive);
	const int rc = pff_write_archive_again(path.c_str(), again.c_str());
	if (rc != PFF_WRITE_OK) {
		std::fprintf(stderr, "%s: written again: %s\n", name.c_str(), pff_write_error_string(rc));
		return 1;
	}
	const std::vector<uint8_t> written = test_io::read_file(again);
	if (written != bytes) {
		size_t at = 0;
		while (at < written.size() && at < bytes.size() && written[at] == bytes[at])
			++at;
		std::fprintf(stderr, "%s: written again differs at byte %zu (%zu bytes, %zu laid out)\n", name.c_str(), at,
				written.size(), bytes.size());
		return 1;
	}
	std::printf("%s: written again byte for byte (%zu bytes)\n", name.c_str(), bytes.size());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	const std::vector<Case> corpus = build_corpus();
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--dump") == 0) {
			dump(corpus);
			return 0;
		}

	int failures = 0;
	const size_t expected_count = sizeof(kExpected) / sizeof(kExpected[0]);
	if (expected_count != corpus.size()) {
		std::printf("FAIL: %zu cases, %zu vectors (regenerate with --dump)\n", corpus.size(), expected_count);
		++failures;
	}
	for (size_t i = 0; i < corpus.size() && i < expected_count; ++i) {
		const Case &c = corpus[i];
		const uint64_t h = fnv1a64(c.bytes);
		if (c.name != kExpected[i].name || c.bytes.size() != kExpected[i].len || h != kExpected[i].hash) {
			std::printf("FAIL %s: len %zu hash 0x%016llx, expected %s len %zu hash 0x%016llx\n", c.name.c_str(),
					c.bytes.size(), (unsigned long long)h, kExpected[i].name, kExpected[i].len,
					(unsigned long long)kExpected[i].hash);
			++failures;
		}
		failures += check_written_again(c.name, c.bytes);
	}
	// The footer the builder lays out is the writer's own PFF_RETAIL_FOOTER, a new archive's kept word 0.
	const std::vector<uint8_t> &base = corpus[0].bytes;
	if (std::memcmp(base.data() + base.size() - PFF_FOOTER_SIZE, PFF_RETAIL_FOOTER, PFF_FOOTER_SIZE) != 0) {
		std::printf("FAIL: the laid-out footer is not PFF_RETAIL_FOOTER\n");
		++failures;
	}
	if (failures) {
		std::printf("\n%d check(s) failed. A vector changes only with a witness for the packer's layout "
				"([orig] or a D-entry; --dump prints the table).\n", failures);
		return 1;
	}
	std::printf("pff_packer_rewrite: %zu packer-laid archives, each written again as its own bytes\n", corpus.size());
	return 0;
}
