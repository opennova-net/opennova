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

} // namespace opennova
