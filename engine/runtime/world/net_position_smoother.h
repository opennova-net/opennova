#pragma once

#include <array>
#include <cstdint>

namespace opennova::world {

// RETAIL'S NETWORK POSITION SMOOTHER — how a networked entity actually moves
// between the position updates it receives.
//
// The deserializer does NOT write a received position onto the entity. For a
// LIVE player it parks it as a GOAL and clears the step counter
// [orig: NetPacket_DeserializePlayerState @0x4C09C0 — the goal store around
//  @0x4C1157, the counter clear @0x4C116B]; only a DEAD one is written
// straight through [orig: the dead branch around @0x4C11C0]. The body update
// then converts that goal into a CONSTANT per-tick increment and walks the
// live position toward it
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0, block @0x4B42A7..0x4B458A].
//
// WHY THE CONSTANT INCREMENT MATTERS BEYOND POSITION: because the step is
// constant for the whole window, the per-tick displacement is constant, so its
// FIRST DIFFERENCE is zero. The first-person weapon's movement lead is built
// on that first difference — feed it a continuously re-interpolated network
// track instead and the gun kicks at every sample boundary. The dead band
// below exists for the same reason: it removes quantisation noise that would
// otherwise reach the lead.
//
// All 16.16 fixed point: 65536 == 1.0 m.

inline constexpr int32_t kSmoothOne = 65536;

// Above this error the position SNAPS rather than smoothing — a 2 m
// correction is a teleport, not a walk [orig: cmp 0x20000 + jle @0x4B431F].
inline constexpr int32_t kSmoothSnapAbove = 0x20000; // 2.0 m
// Below this the update is IGNORED outright [orig: cmp 0x2aaa + jge @0x4B438E].
inline constexpr int32_t kSmoothDeadBelow = 0x2AAA;  // 0.1667 m
// Vertical-only softening [orig: the block around @0x4B4590].
inline constexpr int32_t kSmoothZHalfBelow = 0x5555; // 0.3333 m
inline constexpr int32_t kSmoothZZeroBelow = 0x2AAA; // 0.1667 m
// The step counter saturates here [orig: the `< 0x200` leg].
inline constexpr int32_t kSmoothCounterMax = 0x200;

// THE DURATION LADDER, ticks by error distance [orig: the eleven
// `cmp eax, <band>` / `mov word [esi+0x27e], <n>` pairs @0x4B44C4..0x4B4560].
// The first band the distance is BELOW wins; past the last, 18.
//
// IT IS NOT MONOTONIC. The run climbs 6,7,8,9 and then DROPS to 7 at the
// 0x7000 band before climbing again. That is what the image does. A tidied
// monotonic ladder would change the smoothing window for a whole class of
// mid-range corrections, which is exactly the range ordinary movement error
// falls into.
inline constexpr int kSmoothLadderBands = 10;
inline constexpr std::array<std::pair<int32_t, int>, kSmoothLadderBands>
		kSmoothLadder = {{
			{0x3000, 6}, {0x4000, 7}, {0x5000, 8}, {0x6000, 9}, {0x7000, 7},
			{0x8000, 8}, {0xA000, 10}, {0xC000, 12}, {0xE000, 14}, {0x10000, 16},
		}};
inline constexpr int kSmoothLadderTop = 18;

inline int smooth_duration_ticks(int32_t distance) {
	for (const auto &band : kSmoothLadder)
		if (distance < band.first) return band.second;
	return kSmoothLadderTop;
}

// The per-tick increment for one axis: the error spread over the duration with
// ROUND-HALF-UP, done as truncating integer division
// [orig: `(err + duration/2) / duration` around @0x4B458A].
inline int32_t smooth_increment(int32_t err, int duration) {
	if (duration <= 0) return 0;
	const int32_t half = static_cast<int32_t>(duration >> 1);
	return (err + half) / duration;
}

// The VERTICAL special case, applied after the increment is computed
// [orig: the block around @0x4B4590]: a small vertical error is halved by an
// ARITHMETIC SHIFT, and a very small one is dropped entirely. Halving with a
// divide instead would round the wrong way for negative z.
inline int32_t smooth_soften_z(int32_t inc_z, int32_t err_z) {
	const int32_t mag = err_z < 0 ? -err_z : err_z;
	if (mag < kSmoothZZeroBelow) return 0;
	if (mag < kSmoothZHalfBelow) return inc_z >> 1;
	return inc_z;
}

// What a new goal resolves to.
enum class SmoothOutcome { Snap, DeadBand, Smooth };

inline SmoothOutcome smooth_classify(int32_t distance) {
	// Order matters: the snap test runs FIRST, so a large error never falls
	// into the dead band by way of some other path.
	if (distance > kSmoothSnapAbove) return SmoothOutcome::Snap;
	if (distance < kSmoothDeadBelow) return SmoothOutcome::DeadBand;
	return SmoothOutcome::Smooth;
}

// The per-tick walk. The position only advances while the counter is under the
// duration, but the counter keeps climbing to its own ceiling afterwards
// [orig: the two separate tests around @0x4B44E8].
inline bool smooth_should_step(int32_t counter, int duration) {
	return counter < duration;
}
inline int32_t smooth_advance_counter(int32_t counter) {
	return counter < kSmoothCounterMax ? counter + 1 : counter;
}

} // namespace opennova::world
