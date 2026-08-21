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
	// [orig: @0x45CFC8..0x45CFD4] — so a wheel re-entering oscillation always
	// starts from the same point in the curve.
	osc.phase = kOscPhasePreset;

	// Already bottomed: pin at travel, drop the stored energy, absorb nothing.
	if (compression > travel) { // [orig: @0x45CFE8..0x45CFEE]
		osc.energy = 0;         // [orig: @0x45D0A2]
		osc.amplitude = travel; // [orig: @0x45D0A5]
		return 0;
	}

	compression += step;   // [orig: @0x45CFFC]
	osc.extension -= step; // [orig: @0x45D003]

	// The spring term is computed ONCE as the wrapping int32 product
	// `2 * spring * step * step` [orig: @0x45D00F..0x45D020] and then halved
	// twice over: energy takes it times flt_7C59B0 (-0.5) @0x45D028..0x45D037,
	// the impact sink takes it times flt_7C3B94 (0.5) @0x45D059..0x45D06A. The
	// 2 and the 0.5 cancel only while the doubled product fits; past that the
	// wrapped double is what both halves divide, so keep the doubled form.
	const int32_t twice = wrap_i32(2LL * static_cast<int64_t>(spring) *
			static_cast<int64_t>(step) * static_cast<int64_t>(step));
	osc.energy += -(twice / 2); // ftol(-0.5 * twice): exact, twice is even

	// The impact sink only drains while positive [orig: the > 0 gate
	// @0x45D03A..0x45D042, the subtraction @0x45D06A].
	if (impact > 0) impact -= twice / 2;

	// Energy floors at -1, not 0 [orig: @0x45D075..0x45D077].
	if (osc.energy < 0) osc.energy = -1;

	// Amplitude tracks the compression, capped at the travel bound
	// [orig: @0x45D08F..0x45D093].
	osc.amplitude = compression < travel ? compression : travel;
	return step; // [orig: the new-minus-old compression @0x45D024 returned @0x45D095]
}

int32_t conform_spring_oscillate(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t &shock, int32_t spring, int32_t entity_a0) {
	osc.phase += kOscPhaseStep; // [orig: @0x45D11F..0x45D128]

	// A raised sine, clamped at 1 — the wheel's travel envelope
	// [orig: (sin(phase) + dbl_7C6A08 (1.0)) * dbl_7C3618 (0.5), fld1/fcom
	//  clamp @0x45D12B..0x45D148].
	float env = (std::sin(osc.phase) + 1.0f) * 0.5f;
	if (env > 1.0f) env = 1.0f;

	const int32_t old = compression;
	compression = static_cast<int32_t>(env * static_cast<float>(osc.amplitude)); // [orig: @0x45D14A..0x45D167]
	osc.extension = kSuspFull - compression; // [orig: @0x45D176]
	const int32_t delta = compression - old; // [orig: @0x45D189]

	// The def's shock is clamped to [0, 10] IN PLACE — retail writes the clamp
	// back into the item def @0x45D18F..0x45D1A2, and so does this.
	if (shock < 0) shock = 0;
	if (shock > 10) shock = 10;
	const int32_t s = shock;

	// Amplitude decays EVERY tick [orig: amplitude * flt_7C6A00 @0x45D1C8..0x45D1D5].
	osc.amplitude = static_cast<int32_t>(
			static_cast<float>(osc.amplitude) * kOscDecayB);

	// The shock damp applies ONLY on the tick the wheel lands, and it
	// multiplies the amplitude that was JUST decayed [orig: `cmp
	// [esi+ebx*4+2D4h], 0; jnz` @0x45D1D7..0x45D1E3, then `fimul` of the
	// stored decayed amplitude @0x45D1E5..0x45D1EE].
	if (compression == 0) {
		osc.amplitude = static_cast<int32_t>(static_cast<float>(11 - s) *
				kOscDecayA * static_cast<float>(osc.amplitude));

		// A landing also drains the impact sink, again only while positive
		// [orig: @0x45D1F0..0x45D1F8, the subtraction @0x45D219]: the same
		// doubled spring product as the compress step, taken at one step
		// (dword_815180 >> 4) and shifted down four more.
		if (impact > 0) {
			const int32_t drain = wrap_i32(2LL * static_cast<int64_t>(spring) *
					static_cast<int64_t>(kSuspStep) *
					static_cast<int64_t>(kSuspStep)) >> 4;
			impact -= drain;
		}

		// Below the entity+0xA0 threshold the landing is damped four times
		// harder [orig: @0x45D21F..0x45D22D].
		if (entity_a0 < kOscHardLandingBelow) osc.amplitude >>= 2;
	}
	return delta;
}

} // namespace opennova::world
