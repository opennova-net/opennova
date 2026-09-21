#pragma once

#include <formats/avatars/avatars.h>
#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {
class MenuRuntime;

// The PLAYER_INFO nationality -> division -> combo -> voice controller.
// Widgets may be absent: a missing stage stops its downstream cascade and
// retains its previous state. The embedder owns profile persistence and preview nodes.
struct PlayerInfoAvatars {
	enum class Change { Team, Nationality, Division, Combo, Voice };
	int nationality = -1;
	int division = -1;
	std::vector<int32_t> nationalities;
	std::vector<int32_t> voices;
	bool preview_changed = false;

	int update(MenuRuntime &menu, const avatars::AvatarsFile &file, Change change,
			int value, int team, int saved_voice, const hud::GameTextLookup &gameui,
			const hud::GameTextLookup &menutxt);
	const avatars::AvatarCombo *selected_combo(const MenuRuntime &menu,
			const avatars::AvatarsFile &file, bool default_first = false) const;
	std::string voice_preview_trigger(const MenuRuntime &menu,
			const avatars::AvatarsFile &file, int saved_voice) const;
};
inline constexpr const char *kPlayerInfoVoiceBank = "menu.lwf";
} // namespace opennova::menu
