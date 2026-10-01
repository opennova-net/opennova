// The connection indicators' per-frame state; see net_quality_indicators.h
// for the witness map.

#include <runtime/hud/net_quality_indicators.h>

namespace opennova::hud {

namespace {

// One level channel: +4 toward the target while its level is selected, else
// -4 toward 0 [orig: the +0x08 arm @0x4c30cd..0x4c30eb (up to 251 then 255),
// the +0x0C / +0x10 arms @0x4c30f2..0x4c3143 (below target - 4 then the target)].
// The two up-ramp spellings land on the same values for the 255 target.
void ramp_level_channel(int32_t &channel, bool selected, int32_t target) {
	if (selected) {
		if (channel < target - 4)
			channel += 4;
		else
			channel = target;
		return;
	}
	if (channel <= 4)
		channel = 0;
	else
		channel -= 4;
}

} // namespace

void net_quality_reset(NetQualityIndicators &q, uint32_t now_ms) {
	// [orig: CNetQuality_Reset @0x4c58c0 — the change byte rises when a level
	//  was held @0x4c58c7, every dword below restarts, the cooldown takes the
	//  clock @0x4c5914]
	if (q.level != 0) q.level_changed = true;
	q.level = 0;
	q.level1_alpha = 0;
	q.level2_alpha = 0;
	q.level3_alpha = 0;
	q.ramp_target = 255;
	q.link_error_bits = 0;
	q.link_error_countdown = 0;
	q.link_error_alpha = 0;
	q.link_error_blink_timer = 0;
	q.link_error_blink = 0;
	q.flag_cooldown_until_ms = now_ms;
	q.novaworld_alpha = 0;
	q.novaworld_blink_timer = 0;
	q.novaworld_blink = 0;
}

int32_t net_quality_set_level(NetQualityIndicators &q, int32_t level) {
	// [orig: CNetQuality_SetLevel @0x4c3060 — the clamp @0x4c3066..0x4c3071,
	//  the change byte @0x4c307c]
	int32_t clamped = level;
	if (clamped < 0)
		clamped = 0;
	else if (clamped > 4)
		clamped = 4;
	const bool changed = q.level != clamped;
	q.level = clamped;
	if (changed) q.level_changed = true;
	return clamped;
}

void net_quality_set_flag(NetQualityIndicators &q, int flag_type, uint32_t now_ms) {
	if (flag_type == kNetLinkErrorOutgoing || flag_type == kNetLinkErrorIncoming ||
			flag_type == 3) {
		// Only once the clock has reached the cooldown: OR the flag in and
		// restart the countdown [orig: `cmp eax, [esi+30h]; jb` @0x4c3514,
		// `or [esi+1Ch], edi` @0x4c3519, 155 @0x4c351d].
		if (now_ms < q.flag_cooldown_until_ms) return;
		q.link_error_bits |= static_cast<uint32_t>(flag_type);
		q.link_error_countdown = kNetLinkErrorFrames;
		return;
	}
	if (flag_type == kNetLinkErrorClear) {
		// [orig: @0x4c352d..0x4c3543 — bits and countdown to 0, the cooldown
		//  GetTickCount() + 0x2710, the alpha to 0; the blink pair is kept]
		q.link_error_bits = 0;
		q.link_error_countdown = 0;
		q.flag_cooldown_until_ms = now_ms + kNetLinkErrorCooldownMs;
		q.link_error_alpha = 0;
	}
}

void net_quality_update_indicators(NetQualityIndicators &q, const NovaWorldLinkFacts &nw) {
	// The level channels: the one the level selects fades in, the others out
	// [orig: the level switch @0x4c309e..0x4c30b9 (1 / 2 / 3; 0 and 4 select
	//  none), the target store @0x4c30c5].
	q.ramp_target = 255;
	ramp_level_channel(q.level1_alpha, q.level == 1, q.ramp_target);
	ramp_level_channel(q.level2_alpha, q.level == 2, q.ramp_target);
	ramp_level_channel(q.level3_alpha, q.level == 3, q.ramp_target);

	// The T/R link-error icon, only while its countdown runs: at zero the bits
	// and the alpha clear; above 31 frames the alpha climbs 64 a frame to 255,
	// then falls 10 a frame; the blink toggles every 11 frames throughout
	// [orig: @0x4c3146..0x4c3174 and @0x4c319c..0x4c31c1].
	if (q.link_error_countdown > 0) {
		const int32_t remaining = --q.link_error_countdown;
		if (remaining == 0) {
			q.link_error_bits = 0;
			q.link_error_alpha = 0;
		} else if (remaining > 31) {
			if (q.link_error_alpha < 255) {
				q.link_error_alpha += 64;
				if (q.link_error_alpha > 255) q.link_error_alpha = 255;
			}
		} else if (q.link_error_alpha <= 10) {
			q.link_error_alpha = 0;
		} else {
			q.link_error_alpha -= 10;
		}
		if (--q.link_error_blink_timer < 0) {
			q.link_error_blink_timer = 10;
			q.link_error_blink = q.link_error_blink == 0 ? 1 : 0;
		}
	}

	// The NovaWorld N: off NovaWorld it clears; with the NWU session ready it
	// fades 10 a frame, otherwise it sits at 255 and blinks every 11 frames
	// [orig: `cmp transport_mode, 1` @0x4c3177, the flag test @0x4c3180..0x4c318b,
	//  the fade @0x4c318d..0x4c31c6, 255 @0x4c31c8, the blink @0x4c31cb..0x4c31e4,
	//  the clears @0x4c31e8 / @0x4c31eb]. The NWU-in-use word gates only the draw.
	if (!nw.novaworld) {
		q.novaworld_alpha = 0;
		q.novaworld_blink = 0;
		return;
	}
	if (nwu_session_ready(nw.nwu_session_flags)) {
		if (q.novaworld_alpha <= 10)
			q.novaworld_alpha = 0;
		else
			q.novaworld_alpha -= 10;
	} else {
		q.novaworld_alpha = 255;
	}
	if (q.novaworld_alpha != 255) {
		q.novaworld_blink = 0;
		return;
	}
	if (--q.novaworld_blink_timer < 0) {
		q.novaworld_blink_timer = 10;
		q.novaworld_blink = q.novaworld_blink == 0 ? 1 : 0;
	}
}

} // namespace opennova::hud
