#include <runtime/world/projectile_target_ring.h>
#include <runtime/world/round_sim.h>

namespace opennova::world {

// [orig: ProjectileTargetRing_Update @0x4E70F0]
int32_t ProjectileTargetRing::update(int32_t round, int32_t slot, int32_t ttl) {
	if (slot >= 0 && round >= 0) { // @0x4E70FE
		Record &record = records[static_cast<size_t>(slot)];
		if (record.round != round) return -1; // @0x4E7102..0x4E7104
		if (ttl != 0) record.ttl = ttl;       // @0x4E710D..0x4E7112
		else record.round = -1;               // @0x4E712E
		return slot;
	}
	if (++index >= kCapacity) index = 0;        // @0x4E7135..0x4E714B
	if (++count > kCapacity) count = kCapacity; // @0x4E7150..0x4E7167
	Record &record = records[static_cast<size_t>(index)];
	record.round = round; // @0x4E717F
	record.ttl = ttl;     // @0x4E7181
	return index;
}

// [orig: Weapon_UpdateAllProjectiles @0x4EC020 - @0x4EC0A1..0x4EC0CF]
void ProjectileTargetRing::count_down() {
	for (int32_t i = 0; i < count; ++i) {
		Record &record = records[static_cast<size_t>(i)];
		if (record.round < 0) continue;           // @0x4EC0B7..0x4EC0BA
		if (--record.ttl == 0) record.round = -1; // @0x4EC0BC..0x4EC0C1
	}
}

// [orig: Projectile_ReleaseEffects @0x4E8280 (the ring release @0x4E82E3..0x4E82F2, the
//  call @0x4E82EA)]
void RoundSim::retire_round(LiveRound &round) {
	if (round.target_ring_slot >= 0)
		(void)target_ring.update(static_cast<int32_t>(&round - rounds.data()),
				round.target_ring_slot, 0);
	round.target_ring_slot = -1;
	round.active = false;
	--active_count;
}

} // namespace opennova::world
