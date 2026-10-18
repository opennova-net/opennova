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

// The preview model's initial facing, a BAM word: the preview init reseeds the
// CRT stream from the clock word `seed`, then draws rand() % 180 whole degrees
// at 0xB60B60 BAM a degree (formats/avatars/preview_animation.h
// kPreviewInitialYawRangeDeg). The draw rides the render/menu CRT stream
// (base/crt/crt_rng.h), so the reseed and the draw step it as retail's
// per-thread stream steps
// [orig: PlayerInfo_InitPreviewModel @0x5600d0 -- srand(_time64(0))
//  @0x560120..0x560128, rand() @0x5601a2, cdq / idiv 0B4h @0x5601a7..0x5601ad,
//  imul 0B60B60h into dword_25DC53C @0x5601b1..0x5601b7].
int32_t player_info_preview_initial_yaw_bam(uint32_t seed);
} // namespace opennova::menu
