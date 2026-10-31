#pragma once

#include <formats/avatars/avatars.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/world/tick_accumulator.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {
struct Title; // base/title/title.h
}

namespace opennova::menu {
class MenuRuntime;

// The PLAYER_INFO nationality -> division -> combo -> voice controller.
// Widgets may be absent. The populate hands each stage the side's record: a
// nationality or division list the screen lacks leaves the record's
// nationality or division as it stands (seed_record), and the next stage
// fills from it, as both executables' populate passes the record's bytes to
// each fill and each fill returns at once without its control
// [orig: PlayerInfo_PopulateAllControls @0x5606f0 - PlayerInfo_PopulateDivisionList
//  (the nationality byte) @0x560802, PlayerInfo_PopulateAvatarComboList (the
//  nationality and division bytes) @0x560865; PlayerInfo_PopulateNationalityList
//  @0x55d8c0 (@0x55d8dd), PlayerInfo_PopulateDivisionList @0x55da50 (@0x55da6a)]
// [orig: PlayerInfo_PopulateAllControls @0x55e9a0 (@0x55eab2, @0x55eb15) (dfx2.exe),
//  whose player.mnu holds no NATIONALITY or DIVISION]. A missing combo list
// still stops ours there, its voices unfilled, where retail's populate fills
// them from the record's combo [orig: PlayerInfo_PopulatePlayerVoiceCombo
// @0x55dce0 (g_CharSelCombo @0x55dd48), called @0x5608d8]; no shipped
// player.mnu lacks one. The embedder owns profile persistence and preview nodes.
struct PlayerInfoAvatars {
	enum class Change { Team, Nationality, Division, Combo, Voice };
	int nationality = -1;
	int division = -1;
	std::vector<int32_t> nationalities;
	std::vector<int32_t> voices;
	bool preview_changed = false;

	// The shown side's record ahead of a Team populate: its nationality and
	// division (Avatars.def tree indices), which a stage with no list keeps.
	void seed_record(int record_nationality, int record_division) {
		nationality = record_nationality;
		division = record_division;
	}
	// `title` the executable whose screen it is (Title::player_info_combo_names).
	int update(MenuRuntime &menu, const avatars::AvatarsFile &file, Change change,
			int value, int team, int saved_voice, const hud::GameTextLookup &gameui,
			const hud::GameTextLookup &menutxt, const Title &title);
	const avatars::AvatarCombo *selected_combo(const MenuRuntime &menu,
			const avatars::AvatarsFile &file, bool default_first = false) const;
	std::string voice_preview_trigger(const MenuRuntime &menu,
			const avatars::AvatarsFile &file, int saved_voice) const;
};
inline constexpr const char *kPlayerInfoVoiceBank = "menu.lwf";

// The value change the COMBO_LIST callback hears, as the runtime's ValueChanged kind
// (MenuEvent::text2): "combo", a combo box's select, or "spinlist", a spin list's step
// (Title::player_info_combo_spins). A registration's mask names the event classes its callback
// runs for, bit n class n (the event's top byte); a list's select is class 5, a spin list's step
// class 6 [orig: CWnd_EmitEventToNamedHandlerAndCallbacks @0x646970 - `1 << (event >> 24)`
// @0x6469b8 against the mask @0x6469c2; CListWnd_OnCommand @0x643cb0 (0x5000001 @0x643ec4);
// CSpinListWnd_SelectNext @0x64b910 (0x6000001 @0x64b993)]. Jointops.exe registers it for 5 and 9
// [orig: PlayerInfo_RegisterAllControls @0x561470 - 0x220 @0x561597], dfx2.exe for 6 alone
// [orig: PlayerInfo_RegisterAllControls @0x55f720 - 0x40 @0x55f847 (dfx2.exe)]; the one body runs
// either way, the value it stores the picked row's (the list event's +0x10, the step's +8) [orig:
// PlayerInfo_HandleComboListSelect @0x55fe00 (@0x55fe12..0x55fe1e, @0x55fe3f);
// PlayerInfo_HandleComboListSelect @0x55e0c0 (@0x55e0d2..0x55e0de, @0x55e0ff) (dfx2.exe)].
const char *player_info_combo_list_event(const Title &title);

// The preview model's initial facing, a BAM word: the preview init reseeds the
// CRT stream from the clock word `seed`, then draws rand() % 180 whole degrees
// at 0xB60B60 BAM a degree (formats/avatars/preview_animation.h
// kPreviewInitialYawRangeDeg). The draw rides the render/menu CRT stream
// (base/crt/crt_rng.h), so the reseed and the draw step it as retail's
// per-thread stream steps
// [orig: PlayerInfo_InitPreviewModel @0x5600d0 -- srand(_time64(0))
//  @0x560120..0x560128, rand() @0x5601a2, cdq / idiv 0B4h @0x5601a7..0x5601ad,
//  imul 0B60B60h into g_PlayerPreviewYaw @0x5601b1..0x5601b7].
int32_t player_info_preview_initial_yaw_bam(uint32_t seed);

// The PLAYER_INFO portrait's motion, stepped once a menu update (the game loop's
// fixed 62.5 Hz cadence: MainMenu's update slot runs on the drained 16 ms ticks,
// Game_MainLoop @0x52b630 - @0x52ba47 / @0x52ba5a, the bank clamped to 500 ms
// @0x52b836..0x52b83e) while the screen is up. The
// hover zoom blend moves 5% of the way to 1 hovered, to 0 otherwise. The yaw (BAM)
// spins 0x800000 an update at rest; hovered it closes 5% of its error each update on a sway about
// a fixed facing: sway = ftol(sin(ticks * 0.0008) * 2^28), the yaw moved by
// ftol((yaw - sway + 0x58000000) * -0.05), all in 32-bit words. The hover is
// the pointer over PLAYER_PREVIEW alone, under a title that hovers it at all
// (Title::player_info_preview_hover): dfx2.exe's twin clears the hover each
// update, so its portrait spins at rest and never zooms or sways
// [orig: PlayerInfo_UpdatePlayerPreviewAnimation @0x55bed0, from Menu_UpdateFrame @0x54fb57 - `xor
//  eax, eax` / `mov g_PlayerPreviewHovered, eax` @0x55bf03..0x55bf05 under the registered screen's
//  flag, the target 0 @0x55bf0a, the rest spin @0x55bf9c (dfx2.exe)].
// [orig: PlayerInfo_UpdatePlayerPreviewAnimation @0x55dba0, from Menu_UpdateFrame
//  @0x5528f7 - the hover walk exiting after PLAYER_PREVIEW @0x55dc36, the blend
//  g_PlayerPreviewZoomBlend * 0.95 (flt_7C6FB8) + target * 0.05 (flt_7C68E8) @0x55dc4b..0x55dc61,
//  GetTickCount @0x55dc71 (the unsigned fix-up @0x55dc7b), * 0.0008 (flt_7D5B78),
//  fsin, * 2^28 (flt_7D5B74) @0x55dc81..0x55dc8f, the error + 0x58000000
//  @0x55dca4, * -0.05 (flt_7D5B70) @0x55dca8..0x55dcb2, the yaw store g_PlayerPreviewYaw
//  @0x55dcb9, the rest spin @0x55dcc4; PlayerInfo_RenderPlayerPreview3D @0x5609c0
//  reads both @0x560a67 / @0x560c3b] (D-PLAYERINFO-37).
float player_info_preview_blend_step(float blend, bool hovered);
int32_t player_info_preview_yaw_step(int32_t yaw, bool hovered, uint32_t tick_ms);

// The combo parts the portrait submits, in its order: the body, then the head,
// each after its own camo store, both with the preview channel's bone matrices;
// the combo's arms graphic is never submitted [orig:
// PlayerInfo_RenderPlayerPreview3D @0x5609c0 - Avatar_SetBodyCamoCtrl @0x57a390
// and the body submit (blip+0) @0x56110b, Avatar_SetHeadCamoCtrl @0x57a370 and
// the head submit (blip+4) @0x56113c]. The skinned parts play the preview idle
// (PI_Idle.BAD) on the rest skeleton Dt1rst.bad, the raw .bad pair the preview
// init loads (formats/avatars/preview_animation.h) [orig:
// PlayerInfo_InitPreviewModel @0x5600d0 - BoneFile_Load + AnimChannel_InitFromData].
inline constexpr avatars::AvatarPartKind kPlayerInfoPreviewParts[2] = {
	avatars::AVATAR_PART_BODY, avatars::AVATAR_PART_HEAD};

// The PLAYER_INFO portrait's model state, which the embedder's preview device
// draws: its facing (g_PlayerPreviewYaw, a BAM word), its hover zoom blend
// (g_PlayerPreviewZoomBlend), the hover the update reads and the menu mode's
// frame-time bank its updates drain from (world::TickAccumulator: the menu
// update runs on the game loop's drained ticks). The hover is the PLAYER_PREVIEW
// window's mouse-over state -- the menu pump's claim on the window, which
// CWnd_IsMouseOver reads as its state 2 or 3 -- under a title that hovers it at
// all (Title::player_info_preview_hover) [orig: CWnd_IsMouseOver @0x6467d0 -
// the state word +0xE8 == 2 || == 3, the walk in
// PlayerInfo_UpdatePlayerPreviewAnimation @0x55dba0].
struct PlayerInfoPreview {
	bool framed = false;
	bool hovered = false;
	float zoom_blend = 0.0f;
	int32_t yaw_bam = 0;
	world::TickAccumulator bank;

	// A fresh preview model: the initial facing drawn from the clock word
	// `seed` (player_info_preview_initial_yaw_bam) and the bank afresh.
	void frame(uint32_t seed);
	void set_hovered(bool widget_hovered, bool title_hovers);
	// `delta` seconds of wall clock: every update the bank drains takes one
	// blend step and one yaw step at the millisecond clock word `tick_ms`;
	// nothing moves before frame(). Returns the updates run.
	int advance(double delta, uint32_t tick_ms);
	// The portrait camera's distance scale at the blend: `rest_scale` at rest,
	// eased toward rest_scale * kPreviewZoomInScale hovered (the shell's
	// framing of the blend, formats/avatars/preview_animation.h).
	float camera_distance_scale(float rest_scale) const;
};
} // namespace opennova::menu
