// Retail sound-profile sweep: every .lwf / .pwf directly under the extracted
// asset tree parses, is written from its fields and read back as the same bank
// (the stale bytes the authoring tools left past a name, a path or a count
// written zero, ADR 0003; the count of banks whose bytes so differ printed), our
// writer's bytes a fixed point, and carries the structural facts the reader's
// contract states: 'LWF1' magic, single_count == the singles table, at least one
// set/layer/member, and every member's single index in range. Reports Skipped
// without OPENNOVA_JO_ASSETS.

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

using opennova::lwf::File;

// The bank as its fields hold it: every record's fields but the offsets the writer computes.
bool same_bank(const File &a, const File &b) {
	if (a.header.magic != b.header.magic || a.header.reserved1 != b.header.reserved1 ||
			a.multi_header.reserved0 != b.multi_header.reserved0 ||
			a.multi_header.reserved1 != b.multi_header.reserved1 || a.triggers != b.triggers ||
			a.singles.size() != b.singles.size() || a.multis.size() != b.multis.size() ||
			a.playlists.size() != b.playlists.size() || a.sndparms.size() != b.sndparms.size())
		return false;
	for (size_t i = 0; i < a.singles.size(); ++i) {
		const auto &x = a.singles[i], &y = b.singles[i];
		if (x.name != y.name || x.value_hi != y.value_hi || x.path != y.path ||
				x.entry_word28 != y.entry_word28 || x.pad0 != y.pad0 || x.reserved0 != y.reserved0)
			return false;
	}
	for (size_t i = 0; i < a.multis.size(); ++i) {
		const auto &x = a.multis[i], &y = b.multis[i];
		if (x.name != y.name || x.pitch_base != y.pitch_base ||
				x.pitch_random_range != y.pitch_random_range ||
				x.playlist_indices != y.playlist_indices || x.target_id != y.target_id ||
				x.set_flags != y.set_flags)
			return false;
	}
	for (size_t i = 0; i < a.playlists.size(); ++i) {
		const auto &x = a.playlists[i], &y = b.playlists[i];
		if (x.falloff_radius != y.falloff_radius || x.min_distance != y.min_distance ||
				x.flags != y.flags || x.sndparm_indices != y.sndparm_indices ||
				x.reserved0 != y.reserved0)
			return false;
	}
	for (size_t i = 0; i < a.sndparms.size(); ++i) {
		const auto &x = a.sndparms[i], &y = b.sndparms[i];
		if (x.single_index != y.single_index || x.pitch_scaled != y.pitch_scaled ||
				x.random_pitch_scaled != y.random_pitch_scaled || x.volume != y.volume ||
				x.clamp_volume != y.clamp_volume || x.reserved != y.reserved)
			return false;
	}
	return true;
}

int g_differ = 0;

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
	opennova::lwf::File again;
	std::vector<uint8_t> twice;
	if (!opennova::lwf::parse_lwf_buffer(encoded.data(), encoded.size(), again, error) ||
			!same_bank(file, again)) {
		std::printf("FAIL: %s does not read back as the bank it was written from\n", name.c_str());
		++failures;
	} else if (!opennova::lwf::encode_lwf(again, twice, error) || twice != encoded) {
		std::printf("FAIL: %s written again is not the same bytes\n", name.c_str());
		++failures;
	}
	if (encoded != original)
		++g_differ;
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
	std::printf(
			"lwf_jo_assets_sweep: %d profiles, %d failures, %d written with stale bytes zeroed\n",
			swept, failures, g_differ);
	return failures == 0 ? 0 : 1;
}
