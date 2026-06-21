#include "netsim/entity_wire_bridge.h"

#include <world/geom.h> // to_fixed

namespace opennova::netsim {

// The player infantry item template (§5.2a host-built player entity; the same id
// PlayerReplicationState::entity_type_id defaults to).
static constexpr uint16_t kPlayerInfantryTypeId = 0x14B9u;

EntityClass class_for_type_id(uint16_t type_id) {
	// Phase 1 minimal table. Phase 3 derives this from the item's *_function class
	// tag in items.def [orig: ItemDef+356]. Every replicated non-player type is
	// treated as AI infantry for now (org0/org1 — §5.14).
	if (type_id == kPlayerInfantryTypeId) return EntityClass::Player;
	return EntityClass::Infantry;
}

EntityClass entity_class_of(const world::Entity &e) {
	// Must agree with class_for_type_id for every entity snapshot_world emits.
	if (e.item_id == kPlayerInfantryTypeId) return EntityClass::Player;
	if (e.kind == world::EntityKind::Organic) return EntityClass::Infantry;
	// Markers / items / buildings have no §5.10b compact form — not 0x0A-replicated.
	return EntityClass::Unknown;
}

GameEntitySnapshot snapshot_of(const world::Entity &e) {
	GameEntitySnapshot s;
	s.pool = static_cast<uint8_t>(e.handle.pool());
	s.slot = static_cast<uint16_t>(e.handle.slot());
	s.type_id = static_cast<uint16_t>(e.item_id);
	s.flags = 0;
	s.team = e.team;
	// World stores mission-space floats; the wire is i32 16.16 (world::to_fixed).
	s.x = world::to_fixed(e.position.x);
	s.y = world::to_fixed(e.position.y);
	s.z = world::to_fixed(e.position.z);
	// Entity+16 on the wire is a 32-bit engine-frame BAM heading = (90 - mission_yaw) *
	// kBamPerDegree (D-NET-86 / §5.23/§5.24 — the same convention promote.cpp:87 and ai.cpp
	// build from). Entity::yaw is mission yaw in DEGREES, so reconcile our two-store split
	// (Entity::yaw degrees vs AiEntity::heading BAM) at the wire boundary here: the prior
	// `yaw << 16` was both the wrong scale (deg*65536, ~182x off) and missing the (90 - yaw)
	// frame inversion, collapsing nearly every facing to ~yaw 90 -> NPCs faced the wrong way.
	// The encoder takes the rounded high byte and the present inverts (90 - bam/deg) back to
	// mission yaw, so listen-server NPCs now face the same way as the AI-pool present and the
	// local-player avatar (which bypasses this bridge). [orig: Entity_SpawnFromBMSRecord
	// @0x40e9f0; resolves open question Q1]
	constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
	s.euler_z = static_cast<int32_t>(static_cast<int64_t>(90 - e.yaw) * kBamPerDegree);
	s.entity_class = entity_class_of(e);
	return s;
}

std::vector<GameEntitySnapshot> snapshot_world(const world::World &w) {
	std::vector<GameEntitySnapshot> out;
	w.registry.for_each([&](const world::Entity &e) {
		GameEntitySnapshot s = snapshot_of(e);
		if (s.entity_class == EntityClass::Unknown) return; // no 0x0A compact form
		out.push_back(s);
	});
	return out;
}

} // namespace opennova::netsim
