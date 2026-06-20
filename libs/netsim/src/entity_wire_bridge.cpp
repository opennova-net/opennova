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
	// Phase 1 mapping: treat Entity::yaw as the BAM-high i16 of the 32-bit engine
	// heading (entity+16, D-NET-86). The faithful (90 - bms_yaw) conversion + the
	// promote.cpp yaw convention land in Phase 2 (open question Q1).
	s.euler_z = static_cast<int32_t>(e.yaw) << 16;
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
