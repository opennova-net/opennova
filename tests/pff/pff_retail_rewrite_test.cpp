// The PFF writer against retail's own archives (ADR 0008 as amended 2026-10-15): every archive of
// the packed JO:CA install (the base's language, localres and resource .pff and each expansion's)
// written again through pff_write_archive_again, pff_apply_edits' write with nothing to apply, is
// its own bytes: the directory in the order the payloads lie and the retail packer's footer
// (formats/pff/pff.h PFF_RETAIL_FOOTER) [orig: sub_4040E0 @ 0x4041A3..0x40432F (pack.exe)].
// Gated on OPENNOVA_JO_DIR; one archive written at a time into a scratch directory and compared
// a chunk at a time, so the largest (resource.pff, ~400 MB) is never held in memory.

#include "common/retail_paths.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <formats/pff/pff.h>

#include <base/io/os_path.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace opennova::pff;

namespace {

// The .pff files directly under `dir`, sorted by path.
std::vector<std::string> archives_in(const std::string &dir) {
	std::vector<std::string> out;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec))
			continue;
		if (retail::lower_ascii(entry.path().extension().string()) == ".pff")
			out.push_back(entry.path().generic_string());
	}
	std::sort(out.begin(), out.end());
	return out;
}

// True when the two files hold the same bytes; `where` the first offset that differs otherwise
// (the shorter file's size when one is a prefix of the other).
bool same_bytes(const std::string &a, const std::string &b, long long &where) {
	FILE *fa = opennova::io::fopen_utf8(a.c_str(), "rb");
	FILE *fb = opennova::io::fopen_utf8(b.c_str(), "rb");
	bool same = fa != nullptr && fb != nullptr;
	where = 0;
	std::vector<unsigned char> ba(1 << 20), bb(1 << 20);
	while (same) {
		const size_t na = std::fread(ba.data(), 1, ba.size(), fa);
		const size_t nb = std::fread(bb.data(), 1, bb.size(), fb);
		const size_t n = std::min(na, nb);
		if (std::memcmp(ba.data(), bb.data(), n) != 0) {
			size_t i = 0;
			while (ba[i] == bb[i])
				++i;
			where += static_cast<long long>(i);
			same = false;
			break;
		}
		where += static_cast<long long>(n);
		if (na != nb)
			same = false;
		if (na == 0 || na < ba.size())
			break;
	}
	if (fa != nullptr)
		std::fclose(fa);
	if (fb != nullptr)
		std::fclose(fb);
	return same;
}

} // namespace

int main() {
	const std::string install = retail::install();
	if (install.empty())
		return retail::skip("OPENNOVA_JO_DIR (the packed JO:CA install)");
	std::vector<std::string> archives = archives_in(install);
	for (const std::string &exp : retail::expansions()) {
		const std::vector<std::string> more =
				archives_in(retail::join(retail::join(install, "expansion"), exp));
		archives.insert(archives.end(), more.begin(), more.end());
	}
	// At least the base game's three (language, localres, resource).
	if (archives.size() < 3)
		return retail::skip("language.pff, localres.pff and resource.pff in OPENNOVA_JO_DIR");

	test_temp::TempDir dir("pff_retail_rewrite");
	uint32_t entries = 0;
	for (const std::string &path : archives) {
		PffArchive source{};
		TEST_EXPECT(pff_open(&source, path.c_str()) == 0);
		entries += source.entry_count;
		pff_close(&source);
		const std::string out = dir.file("again.pff");
		const int rc = pff_write_archive_again(path.c_str(), out.c_str());
		if (rc != PFF_WRITE_OK) {
			std::fprintf(stderr, "%s: written again: %s\n", path.c_str(), pff_write_error_string(rc));
			return 1;
		}
		long long where = 0;
		if (!same_bytes(path, out, where)) {
			std::fprintf(stderr, "%s: written again differs at byte %lld\n", path.c_str(), where);
			return 1;
		}
		std::error_code ec;
		std::filesystem::remove(opennova::io::os_path(out), ec);
		std::printf("%s: %s\n", path.c_str(), "written again byte for byte");
	}
	std::printf("pff_retail_rewrite: %zu archives, %u entries, each its own bytes\n",
			archives.size(), entries);
	return 0;
}
