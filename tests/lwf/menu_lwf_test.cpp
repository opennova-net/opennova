// The menu sound profile (menu.LWF) drives MNU hover/click sounds: each .mnu
// <SOUND trigger="..."> names a Multi (sound set) in menu.LWF, which resolves
// through a Playlist (layer) -> Sndparm (member) -> Single (the .wav). This pins
// that the real fixture exposes the trigger sets the menus reference and that
// they resolve to loose .wav members. See plan + notes/audio.
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "lwf/lwf.h"

namespace {

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.good()) {
		return {};
	}
	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	file.read(reinterpret_cast<char *>(data.data()), size);
	return file.good() ? data : std::vector<uint8_t>{};
}

std::string to_upper(std::string s) {
	for (char &c : s) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	return s;
}

// Basename after the final '/' or '\\' (LWF paths are Windows-style, e.g.
// "SFX\\MENU\\MSOVR_2.wav"), uppercased -- the runtime resolves wavs by basename.
std::string wav_basename_upper(const std::string &path) {
	size_t slash = path.find_last_of("/\\");
	return to_upper(slash == std::string::npos ? path : path.substr(slash + 1));
}

// Resolve a Multi (sound set) by name to the .wav path of its first member,
// walking Multi -> Playlist -> Sndparm -> Single exactly as the runtime does.
// Returns "" if the set is missing or has no resolvable member.
std::string first_member_wav(const opennova::lwf::File &f, const std::string &set_name) {
	const std::string want = to_upper(set_name);
	for (const auto &multi : f.multis) {
		if (to_upper(multi.name) != want) {
			continue;
		}
		for (uint32_t pi : multi.playlist_indices) {
			if (pi >= f.playlists.size()) {
				continue;
			}
			const auto &pl = f.playlists[pi];
			for (uint32_t si : pl.sndparm_indices) {
				if (si >= f.sndparms.size()) {
					continue;
				}
				const uint32_t single = f.sndparms[si].single_index;
				if (single < f.singles.size()) {
					return f.singles[single].path;
				}
			}
		}
	}
	return "";
}

} // namespace

int main() {
	const std::string root = test_paths_repo_root(__FILE__);
	const std::string menu_lwf = root + "/fixtures/menu_sound/menu.LWF";

	const std::vector<uint8_t> bytes = read_file(menu_lwf);
	TEST_EXPECT(!bytes.empty());

	opennova::lwf::File file;
	std::string error;
	TEST_EXPECT(opennova::lwf::parse_lwf_buffer(bytes.data(), bytes.size(), file, error));
	TEST_EXPECT(file.header.magic == opennova::lwf::kMagic);

	// The two universal menu triggers resolve to their authored wav members.
	const std::string hover = first_member_wav(file, "MOUSE_OVER");
	TEST_EXPECT(!hover.empty());
	TEST_EXPECT(wav_basename_upper(hover).rfind("MSOVR", 0) == 0); // starts with MSOVR

	const std::string click = first_member_wav(file, "CLICK_SELECT");
	TEST_EXPECT(!click.empty());
	TEST_EXPECT(wav_basename_upper(click).rfind("SELECT", 0) == 0); // starts with SELECT

	// Every menu set name equals the trigger string the .mnu authors reference,
	// so an unknown trigger simply resolves to "" (no crash, no fallback noise).
	TEST_EXPECT(first_member_wav(file, "NOT_A_REAL_TRIGGER").empty());

	return 0;
}
