// The witnessed interactive-music driving policy — which AudioVM variable
// slots the shells write, the mission-start seed values, and the hardcoded
// music-pair naming scheme. One home for the menu shell, the game world, and
// the Godot MusicDirector binding (which re-exports these; the GDScript
// service consumes the re-exports and never restates a value).
//
// Full driving witness: docs/audio/mus-sbf-re.md §Game music driving.
#pragma once

#include <string>

namespace opennova::audio {

// --- AudioVM variable slots -------------------------------------------------

// menuscript: the shown screen's MUSICVAR value selects the menu-music section
// through this one var slot [orig: UI_DispatchScreenEvent @ 0x54e6a0 ->
// AudioVM_SetVariable(2, v) @ 0x54eff4].
inline constexpr int kMenuMusicVarSlot = 2;

// gamescript: the mission-start seed writes Var1..Var12
// [orig: Game_StartMission @ 0x5255b3-0x52561b], then the per-frame pump
// keeps health/team fresh [orig: health @ 0x4b6324; team @ 0x4b62fc].
inline constexpr int kGameMusicSeededVarFirst = 1;
inline constexpr int kGameMusicSeededVarLast = 12;
// Var1 = g_MusicMissionStateSeed — never written by retail, so always 0
// (gamemus loops its Multiplayerstart P0 track) [orig: @ 0x5255b3].
inline constexpr int kGameMusicMissionStateVarSlot = 1;
// Var7 = health percent, seeded 100 [orig: seed @ 0x5255f0; per-frame @ 0x4b6324].
inline constexpr int kGameMusicHealthPctVarSlot = 7;
inline constexpr int kGameMusicHealthSeed = 100;
// Var10 = local-player team [orig: @ 0x4b62fc].
inline constexpr int kGameMusicTeamVarSlot = 10;

// --- Music-pair naming ------------------------------------------------------

// The engine names the base pairs MENUMUS.SBF/.BIN and GAMEMUS.SBF/.BIN; when
// expansion <n> is active they become expansion\<n>\M<n>.sbf + M<n>.bin (menu)
// and expansion\<n>\G<n>.sbf + G<n>.bin (game). Bank and script always come
// from the SAME stem, so halves are never mixed.
// [orig: Expansion_LoadAssets @ 0x4a4798 (base names) / @ 0x4a4906-0x4a494a
//  (expansion forms, "expansion\\%s\\M%s.sbf" / "expansion\\%s\\G%s.sbf")]
inline constexpr const char *kMenuMusicBaseStem = "menumus";
inline constexpr const char *kGameMusicBaseStem = "gamemus";
inline constexpr const char *kMenuMusicExpansionPrefix = "M";
inline constexpr const char *kGameMusicExpansionPrefix = "G";
inline constexpr const char *kExpansionDirName = "expansion";

struct MusicPairNames {
	std::string stem;         // "menumus"/"gamemus", or "M<n>"/"G<n>"
	std::string bank_file;    // "<stem>.sbf" (streamed loose from subdir when set)
	std::string script_file;  // "<stem>.bin" (VFS-resolved)
	std::string subdir;       // "" for base pairs; "expansion/<n>" when active
};

// Pure name resolution — no filesystem access (retail's only reselect, the
// expansion .pff existence check, stays with the embedder). An empty
// expansion name yields the base pair.
inline MusicPairNames music_pair_names(const std::string &expansion_name,
		const char *prefix, const char *base_stem) {
	MusicPairNames out;
	if (expansion_name.empty()) {
		out.stem = base_stem;
	} else {
		out.stem = std::string(prefix) + expansion_name;
		out.subdir = std::string(kExpansionDirName) + "/" + expansion_name;
	}
	out.bank_file = out.stem + ".sbf";
	out.script_file = out.stem + ".bin";
	return out;
}

inline MusicPairNames menu_music_pair_names(const std::string &expansion_name) {
	return music_pair_names(expansion_name, kMenuMusicExpansionPrefix,
			kMenuMusicBaseStem);
}

inline MusicPairNames game_music_pair_names(const std::string &expansion_name) {
	return music_pair_names(expansion_name, kGameMusicExpansionPrefix,
			kGameMusicBaseStem);
}

} // namespace opennova::audio
