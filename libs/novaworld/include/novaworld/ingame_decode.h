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
#include <functional>
#include <string>
#include <vector>

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
};

// Map a 4-char items.def class-tag (case-sensitive §5.10b match) to its class.
EntityClass class_from_tag(const char *tag);

// Decompress a 16-bit network-compressed fixed-point value back to i32 16.16.
// Faithful port of [orig: Network_DecompressFixedPoint @ 0x4C27E0]:
//   sign = (bit0 of c) sign-extended; magnitude = mantissa(bits 4-15) shifted
//   left by exponent((bits 1-3)|1); result = sign ^ magnitude.
// Compact-record positions ride the wire compressed; the world coordinate is
// network_decompress_fixedpoint(c) + the per-message anchor (the S2C 0x0A header
// refs, §5.9) when unmounted, or vehicle-local (parent transform) when mounted.
inline int32_t network_decompress_fixedpoint(uint16_t c) {
	const int32_t sign = int32_t((uint32_t(c) << 31) | (uint32_t(c) >> 1)) >> 31;
	const int32_t mag = int32_t(uint32_t(c & 0xFFF0) << ((c & 0x0E) | 1));
	return sign ^ mag;
}

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
	// entity+290 (u16 zero-ext). Bone/other byte — NOT team (D-NET-58); the
	// team byte is the 0x0010-gated field at entity+354 below.
	uint8_t bone_byte = 0;

	// Conditional fields. Gate column = exact spawn_flags bit to test.
	// gate            field                landing
	uint32_t entity_flags = 0;      // 0x0020   entity+36
	int32_t  vel_x = 0;             // 0x0001   entity+16
	int32_t  vel_y = 0;             // 0x0002   entity+20
	int32_t  vel_z = 0;             // 0x0004   entity+24
	int32_t  section_mask = 0;      // 0x0008   entity+308
	uint8_t  team_byte = 0;         // 0x0010   entity+354 — BMS team 1=Blue/2=Red (D-NET-58)
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
	uint32_t movement_val = 0;        // 0x01     entitySlot+16 — raw u32, BAM heading for markers; NOT a parent handle (D-NET-59)
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

// One record from a S2C 0x0C organic-entity spawn batch (§5.23).
// [orig: NapiNPClientMsg_0x00C @ 0x42E730]. Pool-0 "organics" — AI infantry and
// human-player infantry — enter the world via 0x0C, NOT 0x0D (which handles
// pool 1/3 and crashes on the player template type 0x14B9, §5.6). Unlike
// PoolSpawnRecord, EVERY field after `has_body` is UNCONDITIONAL — there are no
// flag-gated optionals — and the record is slot-id-first (0x0D is flags-first).
// The name is parsed inline for every record (why 0x0C is crash-safe on 0x14B9).
struct OrganicSpawnRecord {
	uint16_t slot_id = 0;        // (pool<<12)|slot; 0xFFFF or (s&0xF000)>=0x5000 ends the batch
	bool     has_body = false;   // u8 != 0; 0 ⇒ empty spawn, record ends after this byte

	uint16_t item_type_id = 0;   // entity+28 (ItemList_FindIndexByTypeId → Entity_InitFromItemDef)
	uint32_t entity_flags = 0;   // entity+120 (0x78)
	std::string entity_name;     // cstring → entity+244 (Name[16], capped)
	uint16_t minimap_flags = 0;  // entity+36 (Flags 0x24); bit 0x100 = minimap-register

	int32_t  pos_x = 0;          // entity+4  (i32 16.16 world)
	int32_t  pos_y = 0;          // entity+8
	int32_t  pos_z = 0;          // entity+12
	int32_t  orientation = 0;    // entity+16 (Yaw 0x10; 32-bit BAM)

	uint8_t  team = 0;           // entity+354 (Team 0x162) — BMS team 1=Blue/2=Red (D-NET-58/62)
	uint8_t  ai_state = 0;       // entity+692 (0x2B4)
	uint8_t  anim_slot = 0;      // entity+884 (0x374)
	uint16_t net_id = 0;         // entity+348 (0x15C)
	uint8_t  weapon_state = 0;   // entity+660 (0x294)
	uint8_t  ai_action = 0;      // *(entity+104)+32 (AI sub-struct)
	uint8_t  skip_byte = 0;      // cursor advance only; retail discards it
	uint8_t  unused_byte = 0;    // entity+340 (0x154)
	uint8_t  alert_level = 0;    // entity+533 (0x215)
	uint8_t  sub_type = 0;       // entity+532 (0x214)
	uint8_t  weapon_type = 0;    // entity+343 (0x157)
	uint8_t  parent_slot = 0;    // entity+360 (0x168)
	uint16_t parent_handle = 0xFFFF; // (pool<<12)|slot, resolved → entity+364 (0x16C)
};

struct OrganicSpawnBatch {
	uint16_t entity_count = 0;   // header u16 (no start-index, unlike 0x10/0x20)
	std::vector<OrganicSpawnRecord> records;
	// Set when the slot-id sentinel (0xFFFF or (slot & 0xF000) >= 0x5000) ends
	// the batch before entity_count records were read.
	bool sentinel_ended_early = false;
};

// Decode a S2C 0x0C body per the §5.23 field map. Same return contract as
// decode_pool_spawn_batch: true iff the body was consumed without overrun /
// leftover. [orig: NapiNPClientMsg_0x00C @ 0x42E730]
bool decode_organic_spawn_batch(const uint8_t *body, size_t len,
                                OrganicSpawnBatch &out);

// One entry from a S2C 0x40 minimap-overlay update / capture-zone state batch
// (§5.19). 6 bytes per entry, prefixed by a u8 count. Overlay position is read
// from the resolved pool entity, not the wire — this packet carries no coords.
// [orig: NapiNPClientMsg_0x040 @ 0x425A50 → MapOverlay_DecodeOverlayEntries @ 0x5BEBB0 (6-byte walker)
//  → MapOverlay_UpdateOrCreateSlot @ 0x5BEA60; color table g_minimap_overlay_color_table @ 0x840A10]
struct CaptureZoneOverlay {
	uint16_t handle = 0;       // +0  (pool<<12)|slot, resolved via g_pool_list
	uint8_t  param = 0;        // +2  → slot+2
	uint8_t  icon_color = 0;   // +3  index into g_minimap_overlay_color_table: 0x0c neutral / 0x09 Red / 0x0a Blue (BMS team 0/2/1)
	uint8_t  flags = 0;        // +4  0x10 = persistent capture-zone marker; 0x20 = clear slot
	uint8_t  source = 0;       // +5  → slot+4
};

struct CaptureZoneOverlayBatch {
	uint8_t count = 0;
	std::vector<CaptureZoneOverlay> entries;
};

// Decode a S2C 0x40 body: [u8 count][count × 6-byte entry]. Returns true iff the
// body was consumed exactly.
bool decode_capture_zone_overlay(const uint8_t *body, size_t len,
                                 CaptureZoneOverlayBatch &out);

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
	uint8_t  yaw_byte = 0;            // high byte of 32-bit BAM -> entity+0x10 (heading) on read [D-NET-57]
	uint8_t  pitch_byte = 0;          // -> entity+0x14 (pitch) on read [D-NET-57]
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

// One §5.9.1 weapon-hit record. Decoded by
// [orig: NetPacket_DeserializeWeaponHit @ 0x42F270]. Sole sender is the tag==2
// branch of the S2C 0x0A event loop [0x4306EF]. Variable length 17-20 B by
// `flags` gate bits:
//   17 B if flags == 0
//   18 B if (flags & 0x80) — adds parent_byte
//   19 B if (flags & 0x40) — adds weapon_handle
//   20 B if (flags & 0xC0) — adds both
struct WeaponHitRecord {
	uint8_t  flags = 0;              // gate byte; 0x80 → parent_byte, 0x40 → weapon_handle
	uint8_t  adm_index = 0;          // → AdmDef_GetEntryByIndex (action descriptor index)
	uint8_t  hit_subtype = 0;        // → dword_A822E0 (last-hit subtype global)
	uint8_t  parent_byte = 0;        // present iff (flags & 0x80)
	uint16_t target_handle = 0xFFFF; // (pool<<12)|slot of the hit entity; 0xFFFF=no target
	uint16_t weapon_handle = 0xFFFF; // present iff (flags & 0x40); 0xFFFF=sentinel
	uint16_t damage_extra_raw = 0;   // raw u16 → word_B7C670 (damage/radius/weapon-extra)
	uint16_t pos_x_compressed = 0;   // Network_DecompressFixedPoint → position[0] + dword_A822E4
	uint16_t pos_y_compressed = 0;   // → position[1] + dword_A822E8
	uint16_t pos_z_compressed = 0;   // → position[2] + dword_A822EC
	uint16_t yaw_bam_high = 0;       // raw u16 (interpreted as BAM high half via << 16)
	uint16_t pitch_bam_high = 0;     // raw u16 (interpreted as BAM high half via << 16)

	bool has_parent_byte() const { return (flags & 0x80) != 0; }
	bool has_weapon_handle() const { return (flags & 0x40) != 0; }
};

bool decode_player_compact_record(const uint8_t *body, size_t len,
                                  PlayerCompactRecord &out, size_t &consumed);

bool decode_vehicle_compact_record(const uint8_t *body, size_t len,
                                   VehicleCompactRecord &out, size_t &consumed);

bool decode_infantry_compact_record(const uint8_t *body, size_t len,
                                    InfantryCompactRecord &out, size_t &consumed);

bool decode_weapon_hit_record(const uint8_t *body, size_t len,
                              WeaponHitRecord &out, size_t &consumed);

// ===========================================================================
// S2C 0x0A per-frame update — the whole message, walked into structured form.
// [orig: NapiNPClientMsg_0x00A @ 0x42FEC0]. The 12-byte header's three i32 refs
// are stored into dword_A822E4/E8/EC (verbatim; the per-message position anchor)
// and the trailing event loop carries one compact record per nearby entity,
// each prefixed by `[u8 tag=1][u16 handle][u16 typeId]`. The compact decoder is
// selected by the type's EntityClass (§5.10b), so the per-record width is
// class-dependent — the caller must supply a type_id→class resolver.
// ===========================================================================

// One tag==1 event-loop record: the entity it updates + its per-class compact.
struct FrameUpdateRecord {
	uint16_t handle = 0;   // (pool<<12)|slot of the updated entity
	uint16_t type_id = 0;  // wire itemTypeId
	EntityClass cls = EntityClass::Unknown;
	PlayerCompactRecord   player{};   // valid iff cls == Player
	VehicleCompactRecord  vehicle{};  // valid iff cls == Vehicle
	InfantryCompactRecord infantry{}; // valid iff cls == Infantry
};

struct FrameUpdate {
	// Header refs (dword_A822E4/E8/EC) — the i32 16.16 world anchor each compact
	// record's decompressed position is added to (when unmounted).
	int32_t anchor_x = 0, anchor_y = 0, anchor_z = 0;
	uint16_t mount_handle = 0xFFFF; // local-player vehicle-mount (header tail)
	std::vector<FrameUpdateRecord> records;
};

// Walk a S2C 0x0A body into a FrameUpdate. `class_of` maps a wire type_id to its
// compact dispatch class (built from items.def). Returns true iff the walk
// reached the event-loop terminator (tag 0) or end cleanly; on any short read /
// unknown class it stops, leaving the records decoded so far in `out`. Weapon-hit
// (tag==2) records are consumed but not stored (motion-irrelevant here).
bool decode_frame_update(const uint8_t *body, size_t len,
                         const std::function<EntityClass(uint16_t)> &class_of,
                         FrameUpdate &out);

// ===========================================================================
// C2S 0x0C — per-entity client-to-host packet. Outer body starts with a 5-byte
// sub-header `[u16 handle][u16 itemTypeId][u8 sub_op]` written by
// [orig: Pool_SerializeEntityViaVTable @ 0x4D64E0] and parsed by
// [orig: dispatch_entity_packet_callback @ 0x4D6A80]. `sub_op` selects the
// per-entity callback's mode:
//   0x0A (=10) → extended (type-10) — case 3/4, joiner uplink, §5.10 "Tag 0x0C body"
//   0x0B (=11) → compact (type-11)  — case 1/2, S2C 0x0A trailing-record format
// The compact decoders already exist above (PlayerCompactRecord +
// VehicleCompactRecord + InfantryCompactRecord); the extended uplink lands here.
// ===========================================================================

struct EntityPacketSubHeader {
	uint16_t handle = 0;        // pool<<12|slot of the entity this packet describes
	uint16_t item_type_id = 0;  // (items.def id − 100000); §5.10b dispatch key
	uint8_t  sub_op = 0;        // 0x0A=extended (type 10), 0x0B=compact (type 11)
};

// Decode the 5-byte sub-header. Returns true iff the read fit; on success
// `consumed` is 5.
bool decode_entity_packet_sub_header(const uint8_t *body, size_t len,
                                     EntityPacketSubHeader &out,
                                     size_t &consumed);

// §5.10 extended (type-10) player uplink body — 43 B fixed. Decoded by
// [orig: NetPacket_SerializePlayerState case 3/4 @ 0x4C09C0]. Joiner sends one
// of these per frame for its own player entity. Two reserved bytes are read by
// the receiver and discarded (cursor-advance only) — stored here for the
// re-emitter's benefit.
//
// Position fields are 16.16 fixed-point. Vehicle-LOCAL when `vehicle_handle !=
// 0xFFFF` (the host's case-4 path adds map origin only on the unmounted branch).
//
// The 8 trailing u16 pairs are the host-validated anti-cheat block: 4 ×
// (weapon_id, fire_counter). The host compares these against its own per-slot
// counters to detect shot/hit tally tampering.
struct PlayerExtendedUplink {
	uint16_t vehicle_handle = 0xFFFF;  // pool<<12|slot; 0xFFFF=none
	int32_t  pos_x = 0;                // entity+0x234 / +4 (vehicle-local if mounted, else world + map_origin)
	int32_t  pos_y = 0;                // entity+0x238 / +8
	int32_t  pos_z = 0;                // entity+0x23C / +0xC
	int16_t  heading = 0;              // entity+0x240 (sign-ext ×0x10000 = 32-bit BAM)
	int16_t  pitch   = 0;              // entity+0x244 (sign-ext ×0x10000)
	uint8_t  reserved_18 = 0;          // cursor advance, no read on host
	uint8_t  anim_slot_low = 0;        // entity+0x12C low byte
	uint8_t  flags_xor = 0;            // bits 2-4 XOR'd into entity+0x24
	uint8_t  anim_def_1 = 0;           // entity+0x130
	uint8_t  anim_def_2 = 0;           // entity+0x131
	uint8_t  anim_def_3 = 0;           // entity+0x132
	uint8_t  reserved_24 = 0;          // read into AL, discarded
	uint8_t  stat_byte_0 = 0;          // playerSlot+0x15F78
	uint8_t  stat_byte_1 = 0;          // playerSlot+0x15F79

	// Anti-cheat block: 4 × (weapon_id_u16, fire_counter_u16). On the wire
	// every counter is a u16; the host stores it zero-extended into a u32
	// field (playerSlot+0x17094 / +0x17098 / +0x1709C / +0x170A0).
	uint16_t weapon_id_0 = 0;          // playerSlot+0x1708A
	uint16_t fire_counter_0 = 0;       // playerSlot+0x17094 (zero-ext)
	uint16_t weapon_id_1 = 0;          // playerSlot+0x1708C
	uint16_t fire_counter_1 = 0;       // playerSlot+0x17098 (zero-ext)
	uint16_t weapon_id_2 = 0;          // playerSlot+0x1708E
	uint16_t fire_counter_2 = 0;       // playerSlot+0x1709C (zero-ext)
	uint16_t weapon_id_3 = 0;          // playerSlot+0x17090
	uint16_t fire_counter_3 = 0;       // playerSlot+0x170A0 (zero-ext)
};

// Decode a 43-B extended uplink body (the bytes AFTER the 5-byte sub-header).
// Returns true iff 43 B were consumed cleanly.
bool decode_player_extended_uplink(const uint8_t *body, size_t len,
                                   PlayerExtendedUplink &out, size_t &consumed);

// ===========================================================================
// C2S 0x06 — "client fired round". Fixed 45 B. Joiner reports a single
// weapon-fire event (origin + direction + target + body part hit + muzzle
// offset block). Server validates against the shooter's authority + ammo
// state and runs Server_ValidateAndFireRound (which may emit S2C 0x0A trailing
// weapon-hit records, §5.9.1, when validation succeeds).
// [orig: NapiNPServerMsg_0x006_ClientFiredRound @ 0x513310]
// ===========================================================================

struct ClientFiredRound {
	uint32_t current_tick = 0;        // server-side game tick anchor
	uint16_t shooter_handle = 0xFFFF; // pool<<12|slot; >= 0x5000 high nibble = invalid
	uint8_t  fire_flags = 0;          // bit 0 set → "alt fire" path (ammo not deducted)
	uint8_t  adm_index = 0;           // AdmDef_GetEntryByIndex key — action descriptor (§5.9.1 shares this)
	int32_t  pos_x = 0;               // fire origin world coords (i32 LE, 16.16)
	int32_t  pos_y = 0;
	int32_t  pos_z = 0;
	int32_t  dir_x = 0;               // direction (host shifts << 16 to BAM-extend); wire is raw i32 LE
	int32_t  dir_y = 0;
	uint16_t target_handle = 0xFFFF;  // 0xFFFF = no target
	uint16_t hit_part = 0;            // body part / collision sub-section
	uint8_t  extra_byte1 = 0;         // → dest[18] / extra_val1
	uint8_t  extra_byte2 = 0;         // → dword_C86FB4 global (last-fire context)
	uint8_t  misc_byte = 0;           // → LOBYTE(dest[20])
	uint16_t base_offset = 0;         // dest[10] += this — muzzle offset on entity coords
	uint16_t offset_x = 0;            // dest[11] += this
	uint16_t offset_y = 0;            // dest[12] += this
	uint16_t offset_z = 0;            // dest[13] += this
	uint16_t offset_w = 0;            // dest[14] += this
};

bool decode_client_fired_round(const uint8_t *body, size_t len,
                               ClientFiredRound &out, size_t &consumed);

// ===========================================================================
// C2S 0x21 — anti-cheat CRC reply. Fixed 9 B (effective 5; trailing 4 B are
// observed-zero in capture and discarded by the handler). Sent in response to
// S2C 0x30 / 0x31 challenges. Host re-computes CRC over the indexed 276-byte
// player record (with 6 volatile fields temporarily zeroed), XORs against a
// per-connection salt (`playerCtx+89924`), and compares against the reply's
// `expected_crc`. Mismatch logs "ACRC" and disconnects with "PUNT ACRC".
// [orig: handle_anti_cheat_crc_check @ 0x502050]
// ===========================================================================

struct ClientChecksumReply {
	uint8_t  player_index = 0;      // index into the 276-stride player array
	uint32_t expected_crc = 0;      // u32 LE; host XORs computed CRC vs salt before compare
};

bool decode_client_checksum_reply(const uint8_t *body, size_t len,
                                  ClientChecksumReply &out, size_t &consumed);

} // namespace opennova
