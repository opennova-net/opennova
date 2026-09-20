// The §5.10b per-item replication class, split out of ingame_decode.h so the
// light consumers (replication state/views, replication_model, the Godot net
// client) stop pulling the full 1,700-line decode surface for one enum.
// ingame_decode.h re-includes this header, so full-surface consumers see no
// change.
#pragma once

#include <cstdint>

namespace opennova {

// §5.10b per-item dispatch class — selects which compact decoder a tag==1 record
// in the S2C 0x0A event loop uses. Seeded from the item's *_function class-tag in
// items.def (ai_function, else move_function) at load time. [orig: ItemDef+356]
enum class EntityClass : uint8_t {
	Unknown = 0,
	Player,   // §5.10  18 B fixed
	Infantry, // §5.14  14 B fixed
	Vehicle,  // §5.13  15 B mounted / 21 B unmounted
	Guided,   // §5.15  variable-length delta codec (deferred)
	NoNetworkCallback, // known ItemDef class with fn[3] == 0; tag==1 header only
};

// The DEFAULT player Person item template id (§5.2a host-built player entity;
// §5.6 — the one type the 0x0C organic path is crash-safe on where 0x0D is
// not). Every capture to date streams this id for players; it is a default the
// runtime carries as data (PlayerReplicationState.entity_type_id), not an
// invariant — consumers compare against it only on the no-items.def fallback
// legs, and the player's selected CHARACTER rides the separate avatar channel
// (S2C 0x29 packedCharId -> CharacterEntity, D-PLAYERINFO-1), never this id.
// world/player_spawn.h mirrors it as kPlayerInfantryTypeId (the historical wire
// vocabulary); engine/runtime/replication static_asserts the two agree.
inline constexpr uint16_t kPlayerPersonTypeId = 0x14B9;

// Map a 4-char items.def class-tag (case-sensitive §5.10b match) to its class.
EntityClass class_from_tag(const char *tag);

} // namespace opennova
