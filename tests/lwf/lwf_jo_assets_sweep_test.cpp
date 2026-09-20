// Retail sound-profile sweep: every .lwf / .pwf directly under the extracted
// asset tree parses, re-encodes byte-exact (the raw-slot and string-pool
// preservation the reader/writer pair promises), and carries the structural
// facts the reader's contract states: 'LWF1' magic, single_count == the
// singles table, at least one set/layer/member, and every member's single
// index in range. Reports Skipped without OPENNOVA_JO_ASSETS.

#include <formats/lwf/lwf.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/retail_paths.h"

namespace {

bool is_profile(const std::filesystem::path &path) {
	std::string ext = path.extension().string();
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".lwf" || ext == ".pwf";
}

int check(const std::filesystem::path &path) {
	const std::string name = path.filename().string();
	const std::vector<uint8_t> original = test_io::read_file(path.string());
	if (original.empty()) {
		std::printf("FAIL: %s unreadable or empty\n", name.c_str());
		return 1;
	}
	opennova::lwf::File file;
	std::string error;
	if (!opennova::lwf::parse_lwf_buffer(original.data(), original.size(), file, error)) {
		std::printf("FAIL: %s parse: %s\n", name.c_str(), error.c_str());
		return 1;
	}
	int failures = 0;
	if (file.header.magic != opennova::lwf::kMagic) { std::printf("FAIL: %s magic\n", name.c_str()); ++failures; }
	if (file.header.single_count != file.singles.size()) { std::printf("FAIL: %s single_count\n", name.c_str()); ++failures; }
	if (file.multis.empty() || file.playlists.empty() || file.sndparms.empty()) {
		std::printf("FAIL: %s has no sets/layers/members\n", name.c_str());
		++failures;
	}
	for (const auto &sp : file.sndparms) {
		if (sp.single_index >= file.singles.size()) {
			std::printf("FAIL: %s member single index %u out of range\n", name.c_str(), sp.single_index);
			++failures;
			break;
		}
	}
	std::vector<uint8_t> encoded;
	if (!opennova::lwf::encode_lwf(file, encoded, error)) {
		std::printf("FAIL: %s encode: %s\n", name.c_str(), error.c_str());
		return failures + 1;
	}
	if (encoded.size() != original.size() || std::memcmp(encoded.data(), original.data(), original.size()) != 0) {
		std::printf("FAIL: %s is not byte-exact across parse->encode (%zu vs %zu bytes)\n",
		            name.c_str(), encoded.size(), original.size());
		++failures;
	}
	return failures;
}

} // namespace

int main() {
	const std::string assets = retail::assets();
	if (assets.empty())
		return retail::skip("OPENNOVA_JO_ASSETS (the extracted retail tree with its loose .lwf/.pwf sound profiles)");
	int failures = 0;
	int swept = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		if (!entry.is_regular_file() || !is_profile(entry.path())) continue;
		failures += check(entry.path());
		++swept;
	}
	if (swept == 0) {
		std::printf("FAIL: no .lwf/.pwf directly under %s\n", assets.c_str());
		return 1;
	}
	std::printf("lwf_jo_assets_sweep: %d profiles, %d failures\n", swept, failures);
	return failures == 0 ? 0 : 1;
}
