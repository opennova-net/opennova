// The projectile target ring: the records the flare rounds' motor keeps fresh, which the
// heat-seeker's target query walks for decoys (its context-flag 0x200 leg).
#pragma once

#include <array>
#include <cstdint>

namespace opennova::world {

// 128 records of {a round, a word, its Position, a countdown}, in a ring whose index is
// incremented ahead of each store (a fresh ring's first record lands in slot 1) and whose
// count saturates at 128; its walkers read records [0, count). A round (its index into
// RoundSim::rounds, retail's round pointer) keeps its record's index (round +0x29C): each
// motor tick refreshes the record's countdown while the record is still the round's, and a
// round whose record a later round took over loses it. The end of every projectile pass
// counts each held record down and frees it at zero; a round's release frees its own.
// Words [1..4] (the round's +0x208 at the allocation, 0x20000 at a refresh, its Position)
// are written and never read: unmodeled.
// [orig: g_ProjectileTargetRing @0xB7D388 (128 x 24 bytes), g_ProjectileTargetRingIndex
//  @0xB7C674, g_ProjectileTargetRingCount @0xB7C678; ProjectileTargetRing_Update
//  @0x4E70F0; Weapon_UpdateAllProjectiles @0x4EC020 (the countdown @0x4EC0A1..0x4EC0CF);
//  Projectile_ReleaseEffects @0x4E8280 (the release @0x4E82E3..0x4E82F2);
//  Projectile_InitDragTable @0x4E78D0 (the clear @0x4E792E..0x4E7945)]
struct ProjectileTargetRing {
	static constexpr int32_t kCapacity = 128;
	struct Record {
		int32_t round = -1; // -1: none (retail's null)
		int32_t ttl = 0;    // [5]
	};
	std::array<Record, kCapacity> records{};
	int32_t index = 0;
	int32_t count = 0;

	// ProjectileTargetRing_Update: with no record (`slot` < 0) the next record takes
	// `round` and `ttl`; else, while that record is still the round's, a nonzero `ttl`
	// refreshes it and 0 frees it (its index returned either way), and a record another
	// round took returns -1.
	int32_t update(int32_t round, int32_t slot, int32_t ttl);
	// The pass's countdown over the counted records.
	void count_down();
	void reset() { *this = ProjectileTargetRing{}; }
};

} // namespace opennova::world
