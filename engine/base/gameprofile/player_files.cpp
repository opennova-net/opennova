#include <base/gameprofile/player_files.h>

#include <cctype>

#include <base/gameprofile/required_resources.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/pff/pff.h>

namespace opennova::gameprofile {

namespace {

// What a manifest player file is, by its row's role; the membership is the manifest's flag.
std::string manifest_words(const std::string &role) {
	if (role == "player_sav" || role == "weapon_sav") return "the player's saved profiles";
	if (role == "epass_bin" || role == "passgen_bin") return "the player's stored NovaWorld credentials";
	if (role == "hiscore_txt") return "the high scores the game writes";
	if (role == "admin_cfg") return "the admin server's configuration";
	return "this machine's configuration of the game";
}

// The files the game writes beside itself as it runs, each by its writer.
struct Written {
	const char *name;
	const char *what;
	const char *orig;
};
constexpr Written kWritten[] = {
	{"_errlog.txt", "the game's error log", "[orig: ErrorLog_WriteTimestamped @ 0x53c6d0]"},
	{"_netlog.txt", "the game's network log", "[orig: NapiNPManager_Init @ 0x62c480]"},
	{"SYSDUMP.TXT", "the game's crash report", "[orig: Game_SaveSysdumpAndShowError @ 0x4a55b0]"},
	{"admin_log.txt", "the admin server's log", "[orig: CAdminServer_Construct @ 0x402c10]"},
	{"mru.txt", "the menu cache's recent list", "[orig: CUICache_ScanCacheDirectory @ 0x64d6b0, \"cache\\mru.txt\"]"},
	{"hello.bin", "the chunk file test's output", "[orig: ChunkFile_TestWriteAndReload @ 0x56f810]"},
	{"hello2.bin", "the chunk file test's output", "[orig: ChunkFile_TestWriteAndReload @ 0x56f810]"},
	{"activesrvr.txt", "a hosted session's marker",
	 "[orig: Game_HostMultiplayerSession @ 0x4a65a0; deleted by Game_Run @ 0x4a7fb0]"},
};

// "SS" and five digits, then a dot: a screenshot [orig: Screenshot_CaptureToFile @ 0x5221a0, "SS%0.5d.%s"].
bool screenshot_name(const std::string &name) {
	if (name.size() < 8 || std::toupper(static_cast<unsigned char>(name[0])) != 'S' ||
	    std::toupper(static_cast<unsigned char>(name[1])) != 'S' || name[7] != '.')
		return false;
	for (size_t i = 2; i < 7; ++i)
		if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
	return true;
}

} // namespace

const std::vector<PlayerFile> &player_files() {
	static const std::vector<PlayerFile> rows = [] {
		std::vector<PlayerFile> out;
		for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
			const RequiredResource *row = gameprofile_required_resource_at(i);
			if (row->flags & RES_F_PLAYER_FILE) out.push_back({row->name, manifest_words(row->role), row->orig});
		}
		for (const Written &written : kWritten) out.push_back({written.name, written.what, written.orig});
		return out;
	}();
	return rows;
}

std::string player_file_words(const std::string &logical_name) {
	const std::string name = io::utf8_file_name(logical_name);
	const std::string wanted = pff::normalized_logical_name(name);
	for (const PlayerFile &file : player_files())
		if (pff::normalized_logical_name(file.name) == wanted) return file.what;
	if (strutil::ends_with_icase(name, ".sav")) return "the player's saved profiles";
	if (screenshot_name(name)) return "a screenshot the game took";
	return std::string();
}

bool is_player_file(const std::string &logical_name) { return !player_file_words(logical_name).empty(); }

} // namespace opennova::gameprofile
