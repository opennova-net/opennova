// The HUD's key-driven toggles (hud_toggles.h). Pushed down from the Godot
// shell's HUD presenter and its panel lanes (ADR 0040 ladder E3), where each
// row carried the same edge machine.
#include <runtime/hud/hud_toggles.h>

namespace opennova::hud {

bool HudKeyEdge::step(bool down, bool active, bool chorded) {
	// [orig: the first-match key scan @0x49d42f fires once per press]
	const bool edge = down && !was_down && active && !chorded;
	was_down = down;
	return edge;
}

uint32_t hud_toggles_poll(HudToggleState &s, const HudKeyPoll &k) {
	using namespace hud_toggle_event;
	uint32_t events = 0;
	// huddetail (row 50) precedes hudcolor (row 76): when both rows resolve to
	// the same physical key the huddetail row consumes the edge and hudcolor
	// ships dormant on it; distinct keys leave both rows live (D-CTRL-4).
	// [orig: the first-match key scan @0x49d42f; rows 50 < 76]
	if (s.huddetail.step(k.huddetail, k.active, k.chorded)) {
		// The huddetail cycle: level + 1, wrapping past 3 to 0, on the LIVE
		// level only (game.cfg keeps the config value) [orig:
		// Input_HandleActionBinding_0 @0x4E0601..0x4E0624 ->
		// CRenderState_SetLayerVisibility @0x59B0F0]
		s.hud_detail_level = next_hud_detail_level(s.hud_detail_level);
		events |= kHudDetailCycled;
	}
	const bool hudcolor_down = k.hudcolor && !(k.huddetail && k.rows_share_key);
	if (s.hudcolor.step(hudcolor_down, k.active, k.chorded)) {
		// The color-scheme cycle 0..5 with wrap; deliberately NO toast — the
		// retail case only cycles and restamps the color [orig: the `hudcolor`
		// action, dispatch code 10 @0x49afc7 — idx+1, >5 wraps to 0]
		s.hud_color_index = next_hud_color_index(s.hud_color_index);
		events |= kHudColorCycled;
	}
	if (s.showhud.step(k.showhud, k.active, k.chorded)) {
		// flags = (flags + 1) & 3: bit 1 the corner spinmap block, bit 0 the FP
		// gun [orig: g_FpWeaponViewFlags cycle @0x4E0561; bit0 @0x4DEDEA; bit1
		// @0x5A8635]
		s.showhud_flags = next_showhud_flags(s.showhud_flags);
		events |= kShowHudCycled;
	}
	if (s.dotsize.step(k.dotsize, k.active, k.chorded)) events |= kDotsizeCycled;
	if (s.goals.step(k.goals, k.active, k.chorded)) {
		// The objectives panel toggle [orig: the co-op action toggle @0x49b68b —
		// dword_24C18CC ^= 0xFF]
		s.objectives_visible = !s.objectives_visible;
		events |= kObjectivesToggled;
	}
	// The view-action rows (catalog 107/108/109 = view1st F2, viewwithgun F3,
	// viewchase F4): first person clears the FP-gun bit, gun view sets it, and
	// both select first person; chase selects the chase preference. None of
	// them moves the camera by itself — the sim's arbiter resolves the mode
	// from the preference and the seat (stock JO has no on-foot third person).
	// The 412 cycle and the 405-410 orbit actions have no catalog row and are
	// unreachable from a key. [orig: Input_HandleActionBinding cases 400
	// @0x49c073, 401 @0x49c0d9, 402 @0x49c0f6; the records @0x8186CC /
	// @0x818738 / @0x8187A4 (keys F2/F3/F4) — their row flag gates (0x1 /
	// 0x40 / 0x400 / 0x8000000) ride the unported binding layer, D-CTRL-3;
	// g_FpWeaponViewFlags bit 0 cleared @0x49c073, set @0x49c0d9]
	if (s.view1st.step(k.view1st, k.active, k.chorded)) {
		s.showhud_flags &= ~kShowHudFlagGun;
		events |= kGunBitChanged | kFirstPersonSelected;
	}
	if (s.viewwithgun.step(k.viewwithgun, k.active, k.chorded)) {
		s.showhud_flags |= kShowHudFlagGun;
		events |= kGunBitChanged | kFirstPersonSelected;
	}
	if (s.viewchase.step(k.viewchase, k.active, k.chorded)) events |= kThirdPersonSelected;
	// The Tab player list TOGGLES the panel-visible flag — retail keeps the
	// board up until the next press [orig: Scoreboard_TogglePlayerList
	// @0x4244c0 from the dispatch case @0x49bb68]
	if (s.playerlist.step(k.playerlist, k.active, k.chorded)) {
		s.scoreboard_open = !s.scoreboard_open;
		events |= kScoreboardToggled;
	}
	// The Recent Messages window [orig: `xor g_showMessageLog, 1` @0x49b55a]
	if (s.old_messages.step(k.old_messages, k.active, k.chorded)) {
		s.message_log_open = !s.message_log_open;
		events |= kMessageLogToggled;
	}
	// The SP Show Score panel: settable only OUTSIDE a session; each flip runs
	// the respawn-init wrapper, which clears the other overlay windows and
	// keeps this one [orig: case 422 @0x49bd29 (the !is_in_session gate) ->
	// Game_InitRespawnStateKeepingToggle @0x4993c0]
	if (s.show_score.step(k.show_score, k.active && !k.in_session, k.chorded)) {
		s.end_round_stats_open = !s.end_round_stats_open;
		s.message_log_open = false;
		events |= kShowScoreToggled;
	}
	return events;
}

void hud_toggles_reset_mission(HudToggleState &s) {
	s.scoreboard_open = false;
	s.message_log_open = false;
	s.end_round_stats_open = false;
	s.huddetail.reset();
	s.hudcolor.reset();
	s.showhud.reset();
	s.dotsize.reset();
	s.goals.reset();
	s.view1st.reset();
	s.viewwithgun.reset();
	s.viewchase.reset();
	s.playerlist.reset();
	s.old_messages.reset();
	s.show_score.reset();
}

void hud_toggles_death_screen(HudToggleState &s) {
	s.hud_detail_level = kHudDetailLevelBlank;
}

const char *friendly_tag_toast_key(FriendlyTagMode mode) {
	switch (mode) {
		case FriendlyTagMode::kOff: return "STRMISC_FRIENDLYTAGS_OFF";
		case FriendlyTagMode::kFarBrief: return "STRMISC_FRIENDLYTAGS_FARBRIEF";
		case FriendlyTagMode::kFull: return "STRMISC_FRIENDLYTAGS_FULL";
		case FriendlyTagMode::kBrief: return "STRMISC_FRIENDLYTAGS_BRIEF";
	}
	return "";
}

const char *hud_toggles_cycle_friendly_tags(HudToggleState &s) {
	s.friendly_tag_mode = next_friendly_tag_mode(s.friendly_tag_mode);
	return friendly_tag_toast_key(s.friendly_tag_mode);
}

} // namespace opennova::hud
