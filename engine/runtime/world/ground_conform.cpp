#include <world/ground_conform.h>

#include <cmath>

namespace opennova::world {
namespace {

// The original's arithmetic wraps as int32; keep that rather than promoting,
// because the spring term below is a product of three values and overflows in
// normal play on a stiff suspension.
int32_t wrap_i32(int64_t v) {
	return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
}

} // namespace

int32_t conform_spring_compress(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t step, int32_t travel, int32_t spring) {
	// The phase is FORCED, not advanced, whenever a wheel is compressing
	// [orig: @0x45CFC8] — so a wheel re-entering oscillation always starts
	// from the same point in the curve.
	osc.phase = kOscPhasePreset;

	// Already bottomed: pin at travel, drop the stored energy, absorb nothing.
	if (compression > travel) { // [orig: @0x45CFE8]
		osc.energy = 0;
		osc.amplitude = travel;
		return 0; // [orig: @0x45D0A5]
	}

	compression += step;   // [orig: @0x45CFFC]
	osc.extension -= step; // [orig: @0x45D003]

	// S -= spring * step^2 [orig: += ftol(-0.5 * (2 * spring * step * step))
	//  @0x45D00F — the 0.5 and the 2 cancel, so this is the whole term].
	const int32_t term = wrap_i32(static_cast<int64_t>(spring) *
			static_cast<int64_t>(step) * static_cast<int64_t>(step));
	osc.energy -= term;

	// The impact sink only drains while positive [orig: the > 0 gate
	// @0x45D03A, the subtraction @0x45D06A].
	if (impact > 0) impact -= term;

	// Energy floors at -1, not 0 [orig: @0x45D077].
	if (osc.energy < 0) osc.energy = -1;

	osc.amplitude = compression < travel ? compression : travel; // [orig: @0x45D08F]
	return step; // [orig: @0x45D095]
}

int32_t conform_spring_oscillate(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t shock, int32_t spring, bool a0_negative) {
	osc.phase += kOscPhaseStep; // [orig: @0x45D128]

	// A raised sine, clamped at 1 — the wheel's travel envelope.
	float env = (std::sin(osc.phase) + 1.0f) * 0.5f;
	if (env > 1.0f) env = 1.0f;

	const int32_t old = compression;
	compression = static_cast<int32_t>(env * static_cast<float>(osc.amplitude)); // [orig: @0x45D167]
	osc.extension = kSuspFull - compression; // [orig: @0x45D176]
	const int32_t delta = compression - old; // [orig: @0x45D189]

	// Amplitude decays EVERY tick [orig: @0x45D1D5].
	osc.amplitude = static_cast<int32_t>(
			static_cast<float>(osc.amplitude) * kOscDecayB);

	// The shock damp applies ONLY on the tick the wheel lands
	// [orig: the comp == 0 arm @0x45D1D7..].
	if (compression == 0) {
		int32_t s = shock;
		if (s < 0) s = 0;
		if (s > 10) s = 10;
		osc.amplitude = static_cast<int32_t>(static_cast<float>(11 - s) *
				kOscDecayA * static_cast<float>(osc.amplitude));

		// A landing also drains the impact sink, again only while positive
		// [orig: @0x45D1F0, the subtraction @0x45D219].
		if (impact > 0) {
			const int32_t drain = wrap_i32(2LL * static_cast<int64_t>(spring) *
					static_cast<int64_t>(kSuspStep) *
					static_cast<int64_t>(kSuspStep)) >> 4;
			impact -= drain;
		}

		// A physics field the caller supplies; when set the landing is damped
		// four times harder [orig: @0x45D22D].
		if (a0_negative) osc.amplitude >>= 2;
	}
	return delta;
}

} // namespace opennova::world
