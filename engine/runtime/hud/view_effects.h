#pragma once

// The retail first-person view effects' witnessed spec — the binocular
// mask/rangefinder and NVG overlays drawn behind the ordinary HUD (kept
// separate from the HUD dispatcher in the original, so health/stance/ammo
// stay visible while either effect is active). Constants are the 1024x768
// virtual overlay space of hud_math.h kDesignWidth/kDesignHeight; the shell
// keeps only texture loads and CanvasItem draws.
// Witness record: docs/interface/hud-re.md. The NVG image under the mask is
// the terminal FrameFX pass's (runtime/renderer/frame_fx_effects.h).

namespace opennova::hud {

// The binocular crosshair overlay rect (BinoCH.tga stretched into it), drawn
// after the mask over the whole overlay rect.
// [orig: Binoculars_DrawMask @0x5cfe95..0x5cfee0 (the mask), @0x5cfee5..0x5cff17 (the
//  rect: flt_7DC618 / flt_7D1D70 / flt_7DC188 / flt_7C59AC), @0x5cff48..0x5cff5b
//  (the crosshair)]
inline constexpr int kBinocularCrosshairX = 384;
inline constexpr int kBinocularCrosshairY = 256;
inline constexpr int kBinocularCrosshairW = 256;
inline constexpr int kBinocularCrosshairH = 256;

// The rangefinder readout: four digits starting here, advancing
// kBinocularDigitStep per digit; each digit blits one kViewDigitCell-square
// cell from the 16px digit strip (BNumbers.tga row = digit * cell).
// [orig: Binoculars_DrawRangefinder @0x5908c0 (x), @0x5908cf (the cell),
//  @0x5908de (y), @0x590926 (the step)]
inline constexpr int kBinocularDigitX = 486;
inline constexpr int kBinocularDigitY = 683;
inline constexpr int kBinocularDigitStep = 10;
inline constexpr int kViewDigitCell = 16;

// The NVG gain scale indicator rect (Nvgscale.tga row = gain * cell), drawn
// at half brightness (kNvgScaleModulate/255 per channel) after the NVG.tga
// mask over the whole overlay rect; the composite draws both except under the
// death screen.
// [orig: NVG_DrawMaskAndGain @0x5cffab..0x5cfff1 (the mask), @0x5d0005..0x5d001d (the
//  rect), @0x5d003e (the gain row), @0x5d004a (modulate 0xFF7F7F7F);
//  NVG_Composite @0x5d107e..0x5d1080]
inline constexpr int kNvgScaleX = 960;
inline constexpr int kNvgScaleY = 32;
inline constexpr int kNvgScaleW = 48;
inline constexpr int kNvgScaleH = 32;
inline constexpr int kNvgScaleModulate = 127;

// Retail's persistent rangefinder easing
// [orig: Binoculars_DrawRangefinder @ 0x590810]: the target clamps to 1..1000; a
// correction beyond 1000 snaps; otherwise the displayed value steps toward
// the target on the 111/33/11/3/1 magnitude ladder, so large corrections
// move quickly while the final digits settle one unit at a time. The value
// is intentionally retained while the binocular view is toggled away.
inline constexpr int kBinocularRangeMin = 1;
inline constexpr int kBinocularRangeMax = 1000;

inline constexpr int binocular_range_step(int current, int target) {
	target = target < kBinocularRangeMin
			? kBinocularRangeMin
			: (target > kBinocularRangeMax ? kBinocularRangeMax : target);
	const int delta = target - current;
	const int magnitude = delta < 0 ? -delta : delta;
	if (magnitude == 0) {
		return current;
	}
	if (magnitude > 1000) {
		return target;
	}
	int step = 1;
	if (magnitude > 111) {
		step = 111;
	} else if (magnitude > 33) {
		step = 33;
	} else if (magnitude > 11) {
		step = 11;
	} else if (magnitude > 3) {
		step = 3;
	}
	return current + (delta > 0 ? step : -step);
}

} // namespace opennova::hud
