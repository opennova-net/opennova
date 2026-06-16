#pragma once

// In-game replication record decoders — pool-entity spawn (S2C 0x0D) and
// bulk pool-3 entity sync (S2C 0x20).
//
// Wire-format witnesses live in docs/net/novaworld-net-re.md §5.11 and §5.12;
// the field tables there are the authoritative spec. Field names here mirror
// those tables. Cross-witnessed byte-exact against the 2026-06-16b loopback
// (437 × 0x0D records / 792 × 0x20 records, zero leftover bytes).
//
// Single decoder shared between:
//   - tests/novaworld/nw_ingame_pool_records_test  (byte-witness assertions)
//   - apps/nw_pp                                   (pretty-printer)
//   - libs/novaworld/src/game_session.cpp          (real handlers — pending
//                                                  D-NET-53 + D-NET-55 fixes)
//   - any future replay tool                       (re-emit captured C2S)
//
// Convention: every conditional field is left default-constructed when its
// `spawn_flags` / `flags_byte` gate is clear — callers must mask the flags
// to know which fields are valid. This matches how the retail handlers leave
// the entity slot's matching offsets unwritten.
//
// [orig: NapiNPClientMsg_0x00D @ 0x432C40]  — pool-entity spawn batch.
// [orig: NapiNPClientMsg_0x020 @ 0x425C00]  — bulk pool-3 entity sync.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// One record from a S2C 0x0D pool-entity spawn batch (§5.11).
struct PoolSpawnRecord {
	uint16_t spawn_flags = 0;
	uint16_t slot_id = 0;       // (pool << 12) | slot; 0xFFFF or
	                            // (s & 0xF000) >= 0x5000 ends the batch.
	uint16_t item_type_id = 0;
	std::string entity_name;    // cstring; for AI-flagged item defs this is
	                            // copied to entity+244.

	// Always-present position (i32 16.16 world). Landing entity+4/+8/+12.
	int32_t pos_x = 0;
	int32_t pos_y = 0;
	int32_t pos_z = 0;

	// Always-present unconditional byte after the weapon block. Landing
	// entity+290 (u16 zero-ext).
	uint8_t team_byte = 0;

	// Conditional fields. Gate column = exact spawn_flags bit to test.
	// gate            field                landing
	uint32_t entity_flags = 0;      // 0x0020   entity+36
	int32_t  vel_x = 0;             // 0x0001   entity+16
	int32_t  vel_y = 0;             // 0x0002   entity+20
	int32_t  vel_z = 0;             // 0x0004   entity+24
	int32_t  section_mask = 0;      // 0x0008   entity+308
	uint8_t  orient_byte = 0;       // 0x0010   entity+354
	uint16_t parent_handle = 0xFFFF;// 0x0100   resolved → entity+368
	uint16_t target_handle = 0xFFFF;// 0x0200   resolved → entity+40

	// Weapon block (gated by `spawn_flags & 0x0400`):
	//   u8 weapon_mask, then 1 × u16 per set bit (0xFFFF skips storage),
	//   then unconditional u16 extra_handle_0 + u16 extra_handle_1.
	// `weapon_handles[bit]` is the value read for that bit (0xFFFF if the
	// bit was clear AND the value wasn't read). `extra_handle_0/1` are
	// only valid when (spawn_flags & 0x0400).
	uint8_t weapon_mask = 0;
	std::array<uint16_t, 8> weapon_handles{
			{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};
	uint16_t extra_handle_0 = 0xFFFF;
	uint16_t extra_handle_1 = 0xFFFF;

	// AI trailer (gated by `spawn_flags & 0x0800`). D-NET-52 confirms each
	// of the two pre-cstring fields is a wire u32 (handler advances cursor
	// by 4 bytes per read).
	uint32_t ai_profile_1 = 0;       // 0x0800   aiSlot+16
	uint32_t ai_profile_2 = 0;       // 0x0800   aiSlot+20
	std::string ai_name;             // 0x0800   aiSlot+156 (NUL-terminated)

	uint8_t alert_byte = 0;          // 0x0040   entity+533
	uint8_t action_byte = 0;         // 0x0080   entity+532
	uint8_t weapon_type_byte = 0;    // 0x1000   entity+176

	// Health block: `0x2000` reads (u8 health_byte → entity+538, u16
	// health_short → entity+350); `0x8000` without `0x2000` reads u16
	// health_short alone.
	uint8_t  health_byte = 0;        // 0x2000   entity+538
	uint16_t health_short = 0;       // 0x2000 OR 0x8000   entity+350

	uint8_t  difficulty_byte = 0;    // 0x4000   entity+624
};

struct PoolSpawnBatch {
	std::vector<PoolSpawnRecord> records;
	// True when the loop terminated early via the slot-id sentinel
	// (0xFFFF or (slot & 0xF000) >= 0x5000) before `entity_count` records
	// were read — the retail handler returns immediately on this case.
	bool sentinel_ended_early = false;
	// Header u16 entity_count, retained for re-encode parity.
	int16_t entity_count = 0;
};

// One record from a S2C 0x20 bulk pool-3 entity sync batch (§5.12).
struct Pool3SyncRecord {
	// item_type_id == 0 is the empty-slot sentinel: nothing else is read,
	// the slot is left zero. is_empty_slot is set to make this state
	// unambiguous for callers.
	uint16_t item_type_id = 0;
	bool is_empty_slot = false;

	uint8_t flags_byte = 0;
	int32_t pos_x = 0;
	int32_t pos_y = 0;
	int32_t pos_z = 0;

	// netHandle is always present (no flag gate) when the record isn't an
	// empty-slot sentinel.
	uint16_t net_handle = 0xFFFF;     // always   entitySlot+124

	// Conditional fields.
	uint32_t parent_handle = 0;       // 0x01     entitySlot+16
	uint32_t orientation_val = 0;     // 0x02     entitySlot+0
	uint16_t ammo_count = 0;          // 0x04     entitySlot+290
	uint8_t  team_byte = 0;           // 0x08     entitySlot+354
	uint16_t weapon_type = 0;         // 0x10     entitySlot+640
	uint8_t  score_byte = 0;          // 0x20     entitySlot+672
};

struct Pool3SyncBatch {
	uint16_t start_index = 0;
	int16_t  entity_count = 0;
	std::vector<Pool3SyncRecord> records;
};

// Decode a S2C 0x0D body per the §5.11 field map. Returns true iff the
// body was consumed exactly (no overrun / no leftover). Partially-decoded
// records are still appended to `out.records` on failure so callers can
// pinpoint where the decode went wrong.
//
// `body` is the inner payload AFTER protocol/reassembly stripping —
// what `reassemble_protocol_payload` hands the caller for tag 0x0D.
bool decode_pool_spawn_batch(const uint8_t *body, size_t len,
                              PoolSpawnBatch &out);

// Decode a S2C 0x20 body per the §5.12 field map. Same return contract.
bool decode_pool3_sync_batch(const uint8_t *body, size_t len,
                              Pool3SyncBatch &out);

// ===========================================================================
// Per-entity compact records — appear inside S2C 0x0A's trailing event loop,
// `tag==1` branch. Each record is decoded by a callback selected per item
// type from the §5.10b entity-class dispatch table. Three of the four
// witnessed callbacks land here; the fourth (guided weapons, §5.15) is a
// variable-length delta codec deferred until a capture carries projectile
// traffic.
//
// Convention divergence from decode_pool_*_batch: these consume a PREFIX of a
// larger event-loop buffer, so they emit `consumed` (the exact byte count
// taken) and return true only when the body had enough room AND the read
// finished cleanly. Callers advance their cursor by `consumed`.
// ===========================================================================

// One §5.10 compact record (18 B fixed). Decoded by
// [orig: NetPacket_SerializePlayerState case 1/2 @ 0x4C09C0]. Used by items
// with `ai_function plyr` — the local player.
struct PlayerCompactRecord {
	uint8_t  vehicle_bone = 0;        // entity+0x157
	uint8_t  seat_type = 0;           // local seat-type byte
	uint16_t vehicle_handle = 0xFFFF; // pool<<12|slot, 0xFFFF=none
	uint16_t pos_x_compressed = 0;    // entity+4   (vehicle-local if mounted)
	uint16_t pos_y_compressed = 0;    // entity+8
	uint16_t pos_z_compressed = 0;    // entity+0xC
	uint8_t  yaw_byte = 0;            // entity+0x14 (high byte of 32-bit BAM)
	uint8_t  pitch_byte = 0;
	uint8_t  anim_slot_low = 0;       // entity+0x12C
	uint8_t  state_flags = 0;         // entity+0x24 (bit 2 = spawning, bit 4 = mounted)
	uint8_t  weapon_anim_state = 0;   // entity+0x2B8 / 0x2BC
	uint8_t  priority = 0;            // entity+0x377
	uint8_t  anim_def_index = 0;      // entity+0x2B0
	uint8_t  health_class_byte = 0;   // → Entity_SetHealthFromDifficultyByte
};

// One §5.13 compact record (15 B mounted / 21 B unmounted). Decoded by
// [orig: Entity_SerializeMountedVehicleState @ 0x460560]. Used by items with
// `ai_function` in {CHel, cveh, cbot, cpln, ctrn} — controllable vehicles
// and AI ground/air units sharing the vehicle network callback.
struct VehicleCompactRecord {
	uint16_t parent_slot_handle = 0xFFFF; // pool<<12|slot, 0xFFFF=none
	uint16_t pos_x_compressed = 0;        // entity+4   (vehicle-local if parent != none)
	uint16_t pos_y_compressed = 0;        // entity+8
	uint16_t pos_z_compressed = 0;        // entity+12
	int16_t  yaw_high = 0;                // entity+16 (BAM high i16 (v+0x8000)>>16)
	uint8_t  flags_byte = 0;              // entity+36 low byte
	bool     is_mounted = false;          // (flags_byte & 4) != 0

	// Mounted branch (is_mounted = true):
	uint16_t secondary_heading = 0;       // entity+24, valid iff is_mounted

	// Unmounted branch (is_mounted = false):
	uint16_t weapon_x_compressed = 0;     // entity+160
	uint16_t weapon_y_raw = 0;            // entity+286 (raw u16, not compressed)
	uint16_t weapon_z_compressed = 0;     // vehicleData[136] = entity+544
	uint16_t weapon_heading_compressed = 0;// vehicleData[135] = entity+540

	// Always present, both branches:
	uint16_t final_heading = 0;           // entity+20 (mounted) or vehicleData[132]=entity+528
};

// One §5.14 compact record (14 B fixed). Decoded by
// [orig: NetPacket_SerializeInfantryEntityState @ 0x4C0320]. Used by items
// with `ai_function` in {org0, org1} — AI infantry / organic pool-0 entities
// that aren't the player.
struct InfantryCompactRecord {
	uint8_t  seat_bone_idx = 0;            // entity+343 if mounted else 0
	uint16_t vehicle_slot_handle = 0xFFFF; // pool<<12|slot, 0xFFFF=none
	uint16_t pos_x_compressed = 0;         // entity+4 (vehicle-local if parent set)
	uint16_t pos_y_compressed = 0;
	uint16_t pos_z_compressed = 0;
	uint8_t  yaw_byte = 0;                 // entity+16 BAM high (v+0x800000)>>24
	uint8_t  flags_byte = 0;               // entity+36
	uint8_t  pitch_byte = 0;               // entity+748
	uint8_t  aim_yaw_byte = 0;             // entity+720
	uint8_t  anim_byte = 0;                // entity+696 if non-zero else entity+700
};

bool decode_player_compact_record(const uint8_t *body, size_t len,
                                  PlayerCompactRecord &out, size_t &consumed);

bool decode_vehicle_compact_record(const uint8_t *body, size_t len,
                                   VehicleCompactRecord &out, size_t &consumed);

bool decode_infantry_compact_record(const uint8_t *body, size_t len,
                                    InfantryCompactRecord &out, size_t &consumed);

} // namespace opennova
