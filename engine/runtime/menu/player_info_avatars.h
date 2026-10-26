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

// The PLAYER_INFO portrait's motion, stepped once a menu update (the game loop's
// fixed 62.5 Hz cadence: MainMenu's update slot runs on the drained 16 ms ticks,
// Game_MainLoop @0x52b630 - @0x52ba47 / @0x52ba5a, the bank clamped to 500 ms
// @0x52b836..0x52b83e) while the screen is up. The
// hover zoom blend moves 5% of the way to 1 hovered, to 0 otherwise. The yaw (BAM)
// spins 0x800000 an update at rest; hovered it closes 5% of its error each update on a sway about
// a fixed facing: sway = ftol(sin(ticks * 0.0008) * 2^28), the yaw moved by
// ftol((yaw - sway + 0x58000000) * -0.05), all in 32-bit words. The hover is
// the pointer over PLAYER_PREVIEW alone.
// [orig: PlayerInfo_UpdatePlayerPreviewAnimation @0x55dba0, from Menu_UpdateFrame
//  @0x5528f7 - the hover walk exiting after PLAYER_PREVIEW @0x55dc36, the blend
//  flt_25DC538 * 0.95 (flt_7C6FB8) + target * 0.05 (flt_7C68E8) @0x55dc4b..0x55dc61,
//  GetTickCount @0x55dc71 (the unsigned fix-up @0x55dc7b), * 0.0008 (flt_7D5B78),
//  fsin, * 2^28 (flt_7D5B74) @0x55dc81..0x55dc8f, the error + 0x58000000
//  @0x55dca4, * -0.05 (flt_7D5B70) @0x55dca8..0x55dcb2, the yaw store dword_25DC53C
//  @0x55dcb9, the rest spin @0x55dcc4; PlayerInfo_RenderPlayerPreview3D @0x5609c0
//  reads both @0x560a67 / @0x560c3b] (D-PLAYERINFO-37).
float player_info_preview_blend_step(float blend, bool hovered);
int32_t player_info_preview_yaw_step(int32_t yaw, bool hovered, uint32_t tick_ms);
} // namespace opennova::menu
