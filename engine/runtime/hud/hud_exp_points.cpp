// DFX2-36: the EXPPOINTS element's fade and strings (hud_exp_points.h carries the witness map).
// [orig: HUD_StartExpPointsFade @0x59B730; HUD_DrawExpPoints @0x59B7E0; HUD_InitOverlaySystem
//  @0x5A3490 (dfx2.exe)]

#include <runtime/hud/hud_exp_points.h>

namespace opennova::hud {

namespace {

// One channel byte of an ARGB word: 0 blue, 1 green, 2 red.
uint8_t channel(uint32_t argb, int byte) {
	return static_cast<uint8_t>(argb >> (8 * byte));
}

// The distance's bytes, red green blue, in the order the fade stores them.
constexpr int kChannelByte[3] = {2, 1, 0};

} // namespace

void hud_exp_points_fade_start(HudExpPointsFade &fade, bool hidden, uint16_t duration,
		uint32_t color, uint32_t active, int32_t pulse_percent) {
	// [orig: HUD_StartExpPointsFade @0x59B730 (dfx2.exe); the declaration carries the order]
	if (hidden || duration == 0) return; // @0x59B738 / @0x59B746
	fade.intensity = 1.0f;               // @0x59B751
	for (int i = 0; i < 3; ++i) {
		// `movzx si, bl; sub si, dx`: two zero-extended bytes, 16-bit
		// [orig: @0x59B779..0x59B78A, @0x59B791..0x59B7A4, @0x59B79C..0x59B7AF].
		const int b = kChannelByte[i];
		fade.delta[i] = static_cast<int16_t>(channel(color, b) - channel(active, b));
	}
	fade.rate = 1.0f / static_cast<float>(duration); // fdivr @0x59B76D, stored @0x59B7B7
	// fild, fmul 0.01f, fsubrp 1.0 [orig: @0x59B7BD..0x59B7CB].
	fade.pulse = static_cast<float>(pulse_percent) * 0.01f - 1.0f;
}

HudExpPointsPulse hud_exp_points_fade_step(HudExpPointsFade &fade, uint32_t active) {
	// [orig: HUD_DrawExpPoints @0x59B7E0 (dfx2.exe); the declaration carries the order]
	HudExpPointsPulse out;
	out.color = active;
	const float v = fade.intensity - fade.rate; // @0x59B805..0x59B811
	fade.intensity = v;                         // @0x59B81D
	if (!(0.0f < v)) {
		// The clear, and the number at scale 1 in the active colour [orig: @0x59B8F6..0x59B91B].
		fade = HudExpPointsFade{};
		return out;
	}
	out.scale = fade.pulse * v + 1.0f; // @0x59B83B..0x59B85E
	for (int i = 0; i < 3; ++i) {
		// The distance times the intensity, chopped, added to the byte in 8 bits [orig:
		//  @0x59B862..0x59B891 (red), @0x59B895..0x59B8BA (green), @0x59B8BE..0x59B8E8 (blue)].
		const int b = kChannelByte[i];
		const int32_t step = static_cast<int32_t>(static_cast<float>(fade.delta[i]) * v);
		const uint8_t byte =
				static_cast<uint8_t>(channel(out.color, b) + static_cast<uint8_t>(step));
		out.color = (out.color & ~(0xFFu << (8 * b))) | (static_cast<uint32_t>(byte) << (8 * b));
	}
	return out;
}

HudExpPointsTier hud_exp_points_tier(ScoreTone tone) {
	// [orig: @0x422380..0x4223C5 (dfx2.exe); the declaration carries the rows]
	switch (tone) {
		case ScoreTone::Hit: return {31, -1};
		case ScoreTone::Kill: return {62, 4};
		case ScoreTone::Headshot: return {93, 5};
		default: return {};
	}
}

void hud_exp_points_text(const GameTextLookup &lookup, HudExpPointsText &out) {
	// [orig: HUD_InitOverlaySystem @0x5A3490 (dfx2.exe); the declaration carries the calls]
	out.label =
			game_text(lookup, kGameTextOverlays, "STROVER_HUDEXPPOINTS", "!eXperience Points: ");
	out.session_format =
			game_text(lookup, kGameTextOverlays, "STROVER_HUDSESSIONPTS", "!Session XP: %d");
	out.total_format = game_text(lookup, kGameTextOverlays, "STROVER_HUDTOTALPTS", "!Total XP: %d");
}

} // namespace opennova::hud
