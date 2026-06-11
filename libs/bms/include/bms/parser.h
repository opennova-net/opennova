#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Minimal .bms (mission) file parser. Witnessed against `Mission_LoadBMSFile@0x40F4E0`
// in Jointops.exe; format documented in notes/net_verification_log.md Phase D.2.19.
//
// Scope for D.2.19: extract the entity list across pools 1, 2, 3, 0 so the
// game-server can pick a valid spawn coord for tag=0x0E → tag=0x0F respawn.
// Full mission semantics (waypoints, groups, layers, events, nav zones, item
// definitions) are deferred to Series D / E when the gameserver needs them
// for actual gameplay logic.

struct BmsEntity {
	uint32_t type_id;     // ItemList type index — see notes/net_verification_log.md for known IDs
	uint32_t flags;       // record offset 12; SP/MP filter bits + behavior
	int32_t x, y, z;      // engine units, signed (record offsets 16/20/24)
	int16_t yaw;          // raw record yaw (offset 56); engine fixed-point conversion happens at use site
	int16_t pitch;
	int16_t roll;
	uint8_t team;         // record offset 73
	uint16_t pool;        // 0/1/2/3 — which pool the engine drops this entity into
	uint16_t slot;        // index within the pool (matches what tag=0x0E packed_id targets)
};

struct BmsMission {
	uint8_t version = 0;        // header[3]; must be >= 19
	uint32_t game_type = 0;     // header[0x88]
	std::vector<BmsEntity> entities;  // walked in (pool 1, pool 2, pool 3, pool 0) order
};

// Parse a .bms file from a raw byte buffer. Returns true on success; on
// failure leaves `out` partially populated (caller should treat as invalid).
// Tolerates trailing data the parser doesn't understand; only fails hard on
// magic/version mismatch or truncated body.
bool bms_parse(const uint8_t *data, size_t len, BmsMission &out);

// Read a .bms file from disk into `out`. Returns false if the file cannot be
// opened or fails parse.
bool bms_load_file(const std::string &path, BmsMission &out);

// Pick a "safe" spawn coordinate from the parsed mission. Strategy:
//   1. Drop entities with pos == (0,0,0) — those are typically uninitialized slots.
//   2. Sort remaining by Z.
//   3. Return the entity at the lower-third Z position (close to ground but
//      above any below-terrain noise).
// Returns false if no usable entity exists; in that case the caller should
// fall back to a hardcoded coord. The chosen entity is written to `out`.
bool bms_pick_safe_spawn(const BmsMission &mission, BmsEntity &out);

} // namespace opennova
