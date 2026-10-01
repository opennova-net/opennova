// The tip system (tip_system.h carries the witness map).
#include <runtime/hud/tip_system.h>

namespace opennova::hud {

void tip_show(TipSystem &t, int32_t tip) {
	// [orig: CTipSystem_ShowTip @0x5b6a60 — the keyboard range 1..10 gated on
	//  enableKeyboardTips @0x5b6a6d..0x5b6a79, the gameplay range 11..19 on
	//  enableGameplayTips @0x5b6a7d..0x5b6a8c; a refused tip returns before
	//  any write @0x5b6ac3]
	bool keyboard = false;
	bool gameplay = false;
	if (static_cast<uint32_t>(tip - 1) <= 9u) {
		if (!t.options.keyboard_tips) return;
		keyboard = true;
	}
	if (static_cast<uint32_t>(tip - 11) <= 8u) {
		if (!t.options.gameplay_tips) return;
		gameplay = true;
	}
	if (tip == 0) {
		// The fade clamps a longer countdown [orig: @0x5b6a94..0x5b6a9f].
		if (t.countdown > kTipFadeFrames) t.countdown = kTipFadeFrames;
		return;
	}
	t.tip = tip; // [orig: @0x5b6aa8]
	if (keyboard) t.countdown = kTipKeyboardFrames;      // [orig: @0x5b6aad]
	else if (gameplay) t.countdown = kTipGameplayFrames; // [orig: @0x5b6abc]
}

void tip_handle_event(TipSystem &t, int event) {
	// [orig: CTipSystem_HandleEvent @0x5b6ad0, the switch @0x5b6adc]
	switch (event) {
		case kTipEventBoardGround: tip_show(t, kTipGround); break;          // @0x5b6aeb
		case kTipEventBoardGroundHorn: tip_show(t, kTipGroundHorn); break;  // @0x5b6af8
		case kTipEventBoardHelo: tip_show(t, kTipHelo); break;              // @0x5b6b05
		case kTipEventBoardBoat: tip_show(t, kTipBoat); break;              // @0x5b6b12
		case kTipEventBoardEmplaced: tip_show(t, kTipEmplaced); break;      // @0x5b6b1f
		case kTipEventDetach:
		case kTipEventNvgOff:
		case kTipEventBinocularsOff:
		case kTipEventScopeElevationOff:
		case kTipEventDesignatorWeaponOff:
		case kTipEventDesignatorOff:
			tip_show(t, kTipNone); // [orig: the fade cases @0x5b6b28..0x5b6be9]
			break;
		case kTipEventNvgOn:
			// Once [orig: +0x2C < 1 @0x5b6b31; show 7 @0x5b6b39; ++ @0x5b6b3e].
			if (t.nvg_count < 1) {
				tip_show(t, kTipNvg);
				++t.nvg_count;
			}
			break;
		case kTipEventBinocularsOn:
			// Once [orig: +0x34 < 1 @0x5b6b52; show 16 @0x5b6b5a; ++ @0x5b6b5f].
			if (t.binoculars_count < 1) {
				tip_show(t, kTipBinocRange);
				++t.binoculars_count;
			}
			break;
		case kTipEventScopeElevationOn:
			// Four raises, alternating the elevation tip and the binocular hint
			// [orig: +0x30 < 4 @0x5b6b73..0x5b6b79; odd -> 15 @0x5b6b81, even
			//  -> 8 @0x5b6b8f; ++ @0x5b6b86 / @0x5b6b94].
			if (t.scope_count < 4) {
				tip_show(t, (t.scope_count & 1) != 0 ? kTipScopeUseBinoc : kTipScopeElevation);
				++t.scope_count;
			}
			break;
		case kTipEventDesignatorWeaponOn:
			// Twice [orig: +0x38 < 2 @0x5b6bac; show 17 @0x5b6bb0; ++ @0x5b6bb5].
			if (t.designator_weapon_count < 2) {
				tip_show(t, kTipMortarUseDesignator);
				++t.designator_weapon_count;
			}
			break;
		case kTipEventDesignatorOn:
			// Twice [orig: +0x3C < 2 @0x5b6bcd; show 18 @0x5b6bd5; ++ @0x5b6bda].
			if (t.designator_count < 2) {
				tip_show(t, kTipDesignatorHelpMortar);
				++t.designator_count;
			}
			break;
		case kTipEventSpectatorBegin:
			tip_show(t, kTipSpectatorBegin); // [orig: @0x5b6bf6]
			break;
		default:
			// 17..21 (the game-event handler's raises) and anything else fall
			// through the switch [orig: the default case @0x5b6b42].
			break;
	}
}

void tip_tick_countdown(TipSystem &t) {
	// [orig: CTipSystem_TickCountdown @0x5b69f0 — decrement, floor 0]
	if (t.countdown > 0) --t.countdown;
}

bool tip_is_showing(const TipSystem &t) {
	return t.countdown > kTipFadeFrames; // [orig: @0x5b6c60 setnle]
}

void tip_begin_fade(TipSystem &t) {
	t.countdown = kTipFadeFrames; // [orig: @0x5b6c70]
}

void tip_reset(TipSystem &t, bool clear_counters) {
	// [orig: CTipSystem_Reset @0x5b6940 — +0 / +4 @0x5b6946..0x5b6948, the
	//  counters behind the flag @0x5b694d..0x5b6959]
	t.tip = kTipNone;
	t.countdown = 0;
	if (clear_counters) {
		t.nvg_count = 0;
		t.scope_count = 0;
		t.binoculars_count = 0;
		t.designator_weapon_count = 0;
		t.designator_count = 0;
	}
}

bool tip_text_keys(int32_t tip, TipText &out) {
	// [orig: CTipSystem_Draw @0x5b6da6..0x5b6dea — (tip <= 1 || tip >= 10) and
	//  (tip <= 11 || tip >= 19) returns; the switch @0x5b6e07]
	out = TipText{};
	if (tip > 1 && tip < 10) {
		out.header_key = "StrKBTip";
		out.gameplay = false;
	} else if (tip > 11 && tip < 19) {
		out.header_key = "StrGPTip";
		out.gameplay = true;
	} else {
		return false;
	}
	switch (tip) {
		case kTipGround: out.body_key = "KB_GROUND"; break;
		case kTipGroundHorn: out.body_key = "KB_GROUNDH"; break;
		case kTipHelo: out.body_key = "KB_HELO"; break;
		case kTipBoat: out.body_key = "KB_BOAT"; break;
		case kTipEmplaced: out.body_key = "KB_EMPLACED"; break;
		case kTipNvg: out.body_key = "KB_NVG"; break;
		case kTipScopeElevation: out.body_key = "KB_SCOPEELEVATION"; break;
		case kTipSpectatorBegin: out.body_key = "KB_SPECTATORBEGIN"; break;
		case kTipAasBegin: out.body_key = "GP_AAS_BEGIN"; break;
		case kTipAasLostOurCamp: out.body_key = "GP_AAS_LOSTOURCAMP"; break;
		case kTipAasGainedOurCamp: out.body_key = "GP_AAS_GAINEDOURCAMP"; break;
		case kTipScopeUseBinoc: out.body_key = "GP_SCOPE_USEBINOC"; break;
		case kTipBinocRange: out.body_key = "GP_BINOC_RANGE"; break;
		case kTipMortarUseDesignator: out.body_key = "GP_MORTAR_USEDESIGNATOR"; break;
		case kTipDesignatorHelpMortar: out.body_key = "GP_DESIGNATOR_HELPMORTAR"; break;
		default:
			return false; // [orig: the default case, cases 10, 11 @0x5b6e86]
	}
	return true;
}

int tip_alpha(int32_t countdown) {
	const int a = 4 * countdown; // [orig: @0x5b6d76]
	return a > 255 ? 255 : a;    // [orig: @0x5b6d81..0x5b6d88]
}

std::string tip_expand_macros(const std::string &text, const TipKeyDisplay &key_display) {
	// [orig: TextResource_ExpandMacroVariables @0x5b6c80 — the escape pair
	//  @0x5b6caa..0x5b6ccd, the '$' name copy @0x5b6cd1..0x5b6cf8, the
	//  resolved copy @0x5b6d0f, the closing '$' skip @0x5b6d32..0x5b6d34]
	std::string out;
	out.reserve(text.size());
	const size_t n = text.size();
	size_t i = 0;
	while (i < n && text[i] != '\0') {
		const char ch = text[i];
		if (ch == '\\' && i + 1 < n) {
			if (text[i + 1] == 't') {
				out.push_back('\t');
				i += 2;
				continue;
			}
			if (text[i + 1] == 'n') {
				out.push_back('\n');
				i += 2;
				continue;
			}
		}
		if (ch == '$') {
			++i;
			std::string name;
			while (i < n && text[i] != '\0' && text[i] != '$') name.push_back(text[i++]);
			out += key_display ? key_display(name) : std::string("???");
			if (i >= n || text[i] != '$') continue;
		} else {
			out.push_back(ch);
		}
		++i;
	}
	return out;
}

bool tip_draw_text(int32_t tip, const GameTextLookup &gametext, const TipKeyDisplay &key_display,
		std::string &header, std::string &body) {
	header.clear();
	body.clear();
	TipText keys;
	if (!tip_text_keys(tip, keys)) return false;
	header = game_text(gametext, "Tips", keys.header_key, keys.header_key);
	body = tip_expand_macros(game_text(gametext, "Tips", keys.body_key, keys.body_key), key_display);
	return true;
}

} // namespace opennova::hud
