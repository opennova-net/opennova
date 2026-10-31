#include "music_context.h"

namespace opennova::audio {

namespace {

// The joined path the loose expansion bank is opened at (a separator between the two unless the
// base already ends in one, or the name starts with one).
std::string path_join(const std::string &base, const std::string &name) {
	if (base.empty()) return name;
	if (base.back() == '/' || (!name.empty() && name.front() == '/')) return base + name;
	return base + "/" + name;
}

} // namespace

const char *music_context_name(MusicContextKind kind) {
	switch (kind) {
		case MusicContextKind::Menu:
			return "menu";
		case MusicContextKind::Game:
			return "game";
		case MusicContextKind::None:
			break;
	}
	return "";
}

MusicPair resolve_music_pair(MusicContextKind kind, const MusicFiles &files) {
	MusicPair pair;
	if (kind == MusicContextKind::None) return pair;
	const std::string expansion = files.expansion();
	const MusicPairNames names = kind == MusicContextKind::Menu ? menu_music_pair_names(expansion)
	                                                            : game_music_pair_names(expansion);
	if (!names.subdir.empty()) {
		// The expansion's bank streams loose from its folder, its spelling the file's when it is
		// there; both names are set unconditionally [orig: Expansion_LoadAssets @0x4a4906..
		// 0x4a494a], so the bank-first open fails silent on a missing file.
		const std::string expansion_dir = path_join(files.root_dir(), names.subdir);
		const std::string found = files.find_loose(expansion_dir, names.bank_file);
		pair.bank = !found.empty() ? found : path_join(expansion_dir, names.bank_file);
		pair.script_name = names.script_file;
		return pair;
	}
	// The base pair [orig: Expansion_LoadAssets @0x4a4798..0x4a47fc]: the loose bank, the script
	// the resource system finds.
	pair.bank = files.resolve_file(names.bank_file);
	pair.script_name = files.has_file(names.script_file) ? names.script_file : std::string();
	return pair;
}

std::vector<MusicVarSeed> game_music_seed() {
	std::vector<MusicVarSeed> seed;
	// [orig: Game_StartMission @0x5255b3 Var1 = g_MusicMissionStateSeed (never written: 0),
	//  @0x5255c7..0x5255e7 Var2..Var6 = 0, @0x5255f0 Var7 = 100, @0x5255f8..0x52561b Var8..Var12 = 0]
	for (int index = kGameMusicSeededVarFirst; index <= kGameMusicSeededVarLast; ++index)
		seed.push_back({index, index == kGameMusicHealthPctVarSlot ? kGameMusicHealthSeed : 0});
	return seed;
}

bool MusicContext::open(MusicContextKind kind, const MusicFiles *files, MusicContextPlayer &player,
		const std::string &script_override, const std::string &bank_override) {
	// The prior context's teardown first: a switch is a full reload [orig: AudioVM_OpenContextFile
	// @0x672168..0x6721a3].
	stop(player);
	MusicPair pair = files != nullptr ? resolve_music_pair(kind, *files) : MusicPair{};
	if (files != nullptr && !bank_override.empty()) pair.bank = files->resolve_file(bank_override);
	// The bank's file, then the script: no file, no script read [orig: CreateFileA @0x6721da, the
	// bail @0x6721df, ahead of AudioVM_LoadScriptFile @0x6721f7].
	if (!pair.bank.empty() && player.load_bank(pair.bank)) {
		if (!script_override.empty()) {
			// A dev override reads its loose file by the root's resolve.
			const std::string path = files != nullptr ? files->resolve_file(script_override)
			                                          : std::string();
			if (!path.empty() && !player.load_script_file(path)) player.drop_script();
		} else if (files != nullptr && !pair.script_name.empty()) {
			// The script through the resource system, its SCR0 bytes as the payload decode leaves
			// them [orig: AudioVM_LoadScriptFile @0x672d20 - File_LoadResource @0x672d5c]; a file
			// that holds no script loads none.
			std::vector<uint8_t> bytes;
			if (files->read_file(pair.script_name, bytes) && !bytes.empty() &&
					player.load_script_bytes(bytes, pair.script_name) <= 0)
				player.drop_script();
		}
	}
	const bool opened = player.open_vm();
	if (opened) kind_ = kind;
	// The mission start's seed follows its open on either outcome [orig: Game_StartMission
	// @0x5255b3, past both branches @0x525598 / @0x5255ae].
	if (kind == MusicContextKind::Game)
		for (const MusicVarSeed &seed : game_music_seed()) player.set_var(seed.index, seed.value);
	return opened;
}

void MusicContext::stop(MusicContextPlayer &player) {
	player.stop_context();
	kind_ = MusicContextKind::None;
}

} // namespace opennova::audio
