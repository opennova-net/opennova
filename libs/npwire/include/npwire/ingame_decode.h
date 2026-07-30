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
//   - libs/npruntime + libs/netsim                 (the in-match runtime's fold paths)
//   - any future replay tool                       (re-emit captured C2S)
//
// Convention: every conditional field is left default-constructed when its
// `spawn_flags` / `flags_byte` gate is clear — callers must mask the flags
// to know which fields are valid. This matches how the retail handlers leave
// the entity slot's matching offsets unwritten.
//
// [orig: NapiNPClientMsg_0x00D @ 0x432C40]  — pool-entity spawn batch.
// [orig: NapiNPClientMsg_0x020 @ 0x425C00]  — bulk pool-3 entity sync.

#include <npwire/entity_class.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova {

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

// Lift a vehicle-LOCAL offset into world space. [orig: Entity_TransformLocalToWorld
// @ 0x43BD00 — called by the read path at NetPacket_SerializeInfantryEntityState @
// 0x4C0320 / NetPacket_SerializePlayerState @ 0x4C09C0 for a mounted record]: rotate
// (lx,ly,lz) by the parent's Euler — roll about X, then pitch about Y, then yaw about
// Z — in 22-bit fixed-point sin/cos, then add the parent's world position. Angles are
// 32-bit BAM (full circle = 2^32; the engine `fild`s them as signed). Mounts nest
// (a rider on a weapon mount on a vehicle), so callers resolve the parent's WORLD
// pose first and feed it here. The unmounted S2C 0x0A vehicle record transmits only
// the parent's yaw (euler_z); pitch/roll are integrated locally by the engine and
// are NOT on the wire, so callers pass 0 for them (a wire limitation, not a
// divergence). i32 16.16 in and out. (std::sin/cos vs the x87 path is a CRT/platform
// primitive — the structural Euler rotation + 2^22 fixed-point is the faithful port.)
struct WorldPose { int32_t x = 0, y = 0, z = 0; };
WorldPose network_transform_local_to_world(int32_t lx, int32_t ly, int32_t lz,
                                           int32_t px, int32_t py, int32_t pz,
                                           uint32_t yaw_bam, uint32_t pitch_bam,
                                           uint32_t roll_bam);

// Project a WORLD position into a carrier's local frame — the exact inverse of
// network_transform_local_to_world. [orig: Entity_TransformWorldToLocal @ 0x43BB50 —
// the op1/op3 write paths of NetPacket_SerializePlayerState @ 0x4C09C0 run it against
// the mount (+0x16C) or ground entity (+0x28) before compressing a carrier-relative
// record]: delta = world - carrier position, then the transposed rotation in reverse
// order — yaw about Z, pitch about Y, roll about X — in 22-bit fixed-point sin/cos.
// The binary folds the inverse-rotation sign into a -2^22 sine scale (dbl_7C57B0);
// this port keeps +2^22 sines and writes the subtractions out, which is the same
// arithmetic. The original is a 6-dword pose transform: out[3] = heading - carrier
// heading, out[4]/out[5] pitch/roll pass through untouched (@0x43bb7b-0x43bb8d) —
// callers compose headings with plain BAM subtraction, so this returns position only.
WorldPose network_transform_world_to_local(int32_t wx, int32_t wy, int32_t wz,
                                           int32_t px, int32_t py, int32_t pz,
                                           uint32_t yaw_bam, uint32_t pitch_bam,
                                           uint32_t roll_bam);

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
	// Orientation Euler triple (each 32-bit BAM) — NOT velocity (Hex-Rays mislabels
	// these "velX/Y/Z"). entity+16 is the yaw heading the engine builds the spawn
	// pose from. [orig: NapiNPClientMsg_0x00D @0x432c40 writes entity+16/+20/+24 (DWORD
	// idx 4/5/6); consumed by Entity_UpdateOrientationMatrix @0x43b440 ->
	// Math_BuildFixedPointMatrixFromEulerAngles @0x613f40 reading euler[3..5] =
	// entity+16/+20/+24]. euler_z is the engine heading = (90 - bms_yaw) deg (D-NET-86).
	int32_t  euler_z = 0;           // 0x0001   entity+16  (yaw heading, 32-bit BAM)
	int32_t  euler_x = 0;           // 0x0002   entity+20  (32-bit BAM)
	int32_t  euler_y = 0;           // 0x0004   entity+24  (32-bit BAM)
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

	// Zone block (the old "health" reading was a decode-era misnomer — these are
	// zone-object fields, witness 2026-07-03): `0x2000` reads (u8 zone_number_rank =
	// zoneNumber + 32*rank → entity+538 [orig: ZoneSlotChain_GetZoneInfo @0x503eeb],
	// u16 zone_radius → entity+350); a def-attrib-0x40000 SpawnPoint without a zone
	// number instead gates `0x8000` = u16 zone_radius alone [orig: @0x503f29]. Golden
	// ASH_I5A bunkers: 0x22/0x0046 = zone 2 rank 1, radius 70.
	uint8_t  zone_number_rank = 0;  // 0x2000   entity+538 (+ the chain rank in bits 5-7)
	uint16_t zone_radius = 0;       // 0x2000 OR 0x8000   entity+350

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

// One record from a S2C 0x10 static-entity batch (§5.9). Pool-2 statics —
// purely static structures (armory, oil pump, oil-field decorations) that carry
// no AI/destructible state, so they replicate here rather than via the pool-1
// 0x0D path. Header is [u16 startIndex][u16 entityCount] (like 0x20); each record
// is flags-first variable-length (like 0x0D), itemTypeId == 0 is the empty-slot
// sentinel. [orig: NapiNPClientMsg_0x010 @ 0x433400]
struct StaticEntityRecord {
	uint16_t item_type_id = 0;   // always; 0 ⇒ empty slot (record ends, slot left zero)
	bool     is_empty_slot = false;

	uint16_t field_flags = 0;    // always
	int32_t  pos_x = 0;          // always  entity+4  (i32 16.16 world)
	int32_t  pos_y = 0;          // always  entity+8
	int32_t  pos_z = 0;          // always  entity+12

	// Orientation Euler triple (each 32-bit BAM) — NOT velocity. entity+16 is the
	// yaw heading the static's spawn pose builds from (Hex-Rays mislabels these
	// "velX/Y/Z"). [orig: NapiNPClientMsg_0x010 @0x433400 writes entity+16/+20/+24;
	// consumed by Entity_UpdateOrientationMatrix @0x43b440 (euler[3..5])]. euler_z is
	// the engine heading = (90 - bms_yaw) deg (D-NET-86).
	int32_t  euler_z = 0;        // 0x01    entity+16  (yaw heading, 32-bit BAM)
	int32_t  euler_x = 0;        // 0x02    entity+20  (32-bit BAM)
	int32_t  euler_y = 0;        // 0x04    entity+24  (32-bit BAM)
	int32_t  section_mask = 0;   // 0x08    entity+308
	uint8_t  team_byte = 0;      // 0x10    entity+354 (BMS team 1=Blue/2=Red)
	// entity+36 = the entity FLAGS dword, streamed raw (was misread as "parentSlot" — the
	// D-NET-147 grill witnessed the serializer source @0x50435f: BMS Indestructible/Reflective/
	// NoShadow attributes + Building/indestructible def bits; golden buildings carry 0x04020400).
	uint32_t entity_flags = 0;   // 0x20    entity+36 [orig: serialize_pool2_static_to_buffer @0x5044e6]
	uint8_t  ammo_count = 0;     // always  entity+290 (BMS record byte 81)
	uint8_t  bone_a = 0;         // 0x40    entity+533 refNum (BMS byte 153; D-NET-94)
	uint8_t  bone_b = 0;         // 0x80    entity+532 subType (0xFF on indestructible defs)
	uint8_t  score_flag = 0;     // 0x100   entity+624
	uint8_t  weapon_byte = 0;    // always  entity+538
	uint16_t attach_ref = 0;     // weapon_byte != 0 || flags & 0x200; entity+350
};

struct StaticEntityBatch {
	uint16_t start_index = 0;
	int16_t  entity_count = 0;
	std::vector<StaticEntityRecord> records;
};

// Decode a S2C 0x10 body per the §5.9 field map. Same return contract as
// decode_pool3_sync_batch: true iff the body was consumed exactly.
// [orig: NapiNPClientMsg_0x010 @ 0x433400]
bool decode_static_entity_batch(const uint8_t *body, size_t len,
                                StaticEntityBatch &out);

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
	uint8_t  anim_slot = 0;      // entity+884 (0x374 = GamePlayerEntity.animSlot, the character-model/anim-set selector — BMS AnimSlot / avatar / wire; net-re §5.2b)
	uint16_t net_id = 0;         // entity+348 (0x15C)
	uint8_t  player_class = 0;   // entity+660 (0x294 = GamePlayerEntity.playerClass, the soldier class 5-9; net-re D-NET-103)
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

// S2C 0x18 FULL-ENTITY-SPAWN (§5.46) — the reactive single-entity repair record.
// A client whose per-frame 0x0A tail cross-check finds a stale/mismatched entity
// (@0x4307c4: !itemDef || itemDef.id != wire type || ItemTypeIndex !=
// FindIndexByTypeId(wire type)) queues C2S 0x0F [u16 handle]; the server answers
// with this record and the client DESTROYS + fully REBUILDS the entity from it
// (itemDef/models/playerClass/minimap slot/anim registration). It never appears
// in a healthy join — the retail↔retail golden carries zero 0x0F/0x18 — it is
// the self-heal path. [orig: server NapiNPServerMsg_HandlePlayerInfoRequest
// @0x514180 → serialize_object_to_buffer @0x504d10; client
// NapiNPClientMsg_FullEntitySpawn @0x433780]
struct FullEntitySpawnRecord {
	uint16_t slot_id = 0;          // (pool<<12)|slot; 0xFFFF ⇒ client returns immediately
	uint16_t item_type_id = 0;     // itemDef+0x50 low16 (wire type id); 0 on an empty slot
	uint8_t  item_type = 0;        // itemDef+0x5C ItemDefType low byte (1=vehicle, 3=person);
	                               // 0 ⇒ the client stops after the destroy+memset (slot cleared)
	uint8_t  team = 0;             // entity+354 (0x162)
	uint16_t minimap_flags = 0;    // entity+36 (0x24) low16; bit 0x100 gates minimap registration
	uint32_t entity_flags = 0;     // entity+120 (0x78) — the owning connection id (dcb)
	std::string entity_name;       // entity+244; sent iff itemDef attrib & 0x100000 (aidata), else ""
	uint16_t parent_vehicle_handle = 0xFFFF; // entity+368 (0x170), pointer resolved to a handle
	uint16_t ground_entity_handle = 0xFFFF;  // entity+40  (0x28)
	uint16_t parent_entity_handle = 0xFFFF;  // entity+364 (0x16C) — the mounted vehicle
	                                         // [orig: Entity_AttachToVehicleSlot @0x4946d0]
	uint8_t  seat_mask = 0;        // itemDef+604 seatMask; bit i ⇒ mount_handles[i] on the wire
	uint16_t mount_handles[8] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	                             0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // entity+400+2i seat occupants
	uint16_t mount_handle_8 = 0;   // entity+416 — always present, after the seat block
	uint16_t mount_handle_9 = 0;   // entity+418
	int32_t  pos_x = 0;            // entity+4 (i32 16.16 world)
	int32_t  pos_y = 0;            // entity+8
	int32_t  pos_z = 0;            // entity+12
	uint16_t heading_hi = 0;       // entity+18 — Yaw high word; client restores Yaw = (i16)<<16
	uint16_t pitch_hi = 0;         // entity+22 — Pitch high word
	uint8_t  ai_state = 0;         // entity+692 (0x2B4)
	uint8_t  anim_slot = 0;        // entity+884 (0x374)
	uint16_t net_id = 0;           // entity+348 (0x15C) minimap slot id
	uint8_t  player_class = 0;     // entity+660 (0x294)
	uint8_t  skip_byte = 0;        // wire constant 0 (client discards; cursor advance only)
	uint8_t  unused_byte = 0;      // entity+340 (0x154)
	uint8_t  alert_level = 0;      // entity+533 (0x215)
	uint8_t  sub_type = 0;         // entity+532 (0x214)
};

// Decode a S2C 0x18 body per the §5.46 field map. A slot_id of 0xFFFF mirrors
// the client's immediate return (true iff the sentinel is the whole body).
// Otherwise same return contract as decode_organic_spawn_batch.
// [orig: NapiNPClientMsg_FullEntitySpawn @ 0x433780]
bool decode_full_entity_spawn(const uint8_t *body, size_t len,
                              FullEntitySpawnRecord &out);

// S2C 0x16 player-list (§5.20) — the scoreboard. One message = the full list;
// the server may re-sort rows between frames, so slot_id is authoritative.
// [orig: NapiNPClientMsg_PlayerList @ 0x42FAE0]
struct PlayerListRow {
	uint8_t  slot_id = 0;
	uint16_t ping = 0;
	uint16_t score1 = 0;      // score
	uint16_t score2 = 0;      // deaths
	uint8_t  flags = 0;       // bit0 = SPECTATOR (subtracted from the HUD count), team = flags >> 1
	                          // [orig: NapiNPClientMsg_PlayerList @0x42FAE0 row apply]
};
struct PlayerListTeamRow {
	uint16_t score1 = 0;
	uint16_t score2 = 0;
	uint8_t  player_count = 0;
	uint8_t  alive_count = 0;
};
struct PlayerList {
	uint8_t  flags = 0;        // byte 0 -> g_scoreboard_flags: bit0 team-mode, bit1 timed-scores
	                           // (the old `max_players` reading was a misnomer, witness 2026-07-03)
	uint8_t  player_count = 0; // row count -> g_scoreboard_row_count (HUD count minuend, D-NET-158)
	std::vector<PlayerListRow> players; // rows accepted ONLY for 0x46-known slots; a row for an
	                                    // unknown slot is dropped + retried via C2S 0x22 [slot, 0x1CF7]
	uint8_t  team_count = 0;
	std::vector<PlayerListTeamRow> teams;  // team_count + 1 rows (T0 neutral + per team)
	uint8_t  in_game_count = 0;   // trailer -> g_scoreboard_ingame_count (g_scoreboard_ingame_count)
	uint8_t  spectator_count = 0; // trailer -> g_scoreboard_spectator_count (g_scoreboard_spectator_count);
	                              // HUD "Number of players" = accepted rows − this
};
bool decode_player_list(const uint8_t *body, size_t len, PlayerList &out);

// S2C 0x46 player-sync (§5.21) — one player record, fields gated by a bitmask
// read in SOURCE order (NON-numeric: 0x10 before 0x04, 0x1000 before 0x40).
// [orig: NapiNPClientMsg_PlayerSync @ 0x431370]
struct PlayerSync {
	uint8_t  slot_id = 0;
	uint16_t field_bitmask = 0;
	bool     removal = false;        // bitmask & 0x8000 — no body follows
	uint8_t  entity_slot_id = 0;     // present when !removal; pool-0 → handle (0<<12)|slot
	std::string name;                // 0x0001
	std::string clan;                // 0x0002
	std::string id_label;            // 0x0010
	uint8_t  team = 0;               // 0x0004
	uint8_t  type_subtype = 0;       // 0x0008 (type = v & 0x7F, subtype = v >> 7)
	uint8_t  field_0020 = 0;         // 0x0020
	uint8_t  field_1000 = 0;         // 0x1000
	uint8_t  field_0040 = 0;         // 0x0040
	uint8_t  field_0080 = 0;         // 0x0080
	uint8_t  quality = 0;            // 0x0400 (clamp 4)
	uint32_t entity_ref = 0;         // 0x0800
	bool     queue_ack = false;      // 0x4000 — no body byte; client queues a C2S 0x22 ack
};
bool decode_player_sync(const uint8_t *body, size_t len, PlayerSync &out);

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
// type from the §5.10b entity-class dispatch table. Three of the witnessed
// callbacks land here; guided weapons use a variable-length field-group codec,
// and known null callbacks consume no record body.
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
	uint8_t  vehicle_bone = 0;        // entity+0x157 attachBoneId (0 unless seat-mounted @0x4c0a1a)
	uint8_t  seat_type = 0;           // seat-attribute byte (0 unless seat-mounted @0x4c0a50)
	uint16_t carrier_handle = 0xFFFF; // pool<<12|slot, 0xFFFF=none. op1 select: mount (+0x16C)
	                                  // wins, else groundEntity (+0x28) — a grounded-standing
	                                  // player echoes its floor/deck with bone=0 seat=0
	                                  // [orig: @0x4c0a08]. The client mirrors this back into
	                                  // its own groundEntity (@0x4c1353). (Renamed from the
	                                  // vehicle_handle misnomer — D-NET-151.)
	uint16_t pos_x_compressed = 0;    // entity+4 — CARRIER-LOCAL when carrier_handle != none
	                                  // (Entity_TransformWorldToLocal @0x4c0b07), else world
	                                  // minus the frame anchor (g_priority_ref_*)
	uint16_t pos_y_compressed = 0;    // entity+8
	uint16_t pos_z_compressed = 0;    // entity+0xC
	uint8_t  yaw_byte = 0;            // high byte of 32-bit BAM -> entity+0x10 (heading) on read
	                                  // [D-NET-57]; CARRIER-RELATIVE (local heading, sar 24
	                                  // @0x4c0b85) when carrier_handle != none
	uint8_t  pitch_byte = 0;          // -> entity+0x14 (pitch) on read [D-NET-57]
	uint8_t  move_input_byte = 0;     // the movement-INPUT bitfield, entity+0x12C low byte — remote
	                                  // players are motor-driven from replicated input; the read
	                                  // re-derives stance bits 8-9 from the anim-state flag table
	                                  // [orig: apply @0x4c11ec-0x4c1246, remote-only @0x4c11d7;
	                                  // renamed from the anim_slot_low misnomer, witness 2026-07-02]
	uint8_t  state_flags = 0;         // entity+0x24. Bit 0x02 = DEAD/UNDEPLOYED (the spawn hook
	                                  // fires on its 1->0 edge @0x4c1109; the XOR masks exclude it:
	                                  // local 0xE1 / remote 0xFD @0x4c12ff)
	uint8_t  anim_state_id = 0;       // body/weapon anim-STATE id -> entity+0x2BC (vs the per-state
	                                  // flags table g_animStateFlagsTable; transition-arbitrated, remote-only
	                                  // apply except the wire-bit2 dead path) [orig: @0x4c1153;
	                                  // renamed from weapon_anim_state/weapon_id — witness 2026-07-02]
	uint8_t  anim_channel_ratio = 0;  // elapsed half-frame ticks in the current body loop (legacy field name)
	                                  // (f32[+0x28]/f32[+0x2C] or f32[+8]/f32[+0xC] by obj+0x14);
	                                  // read side stores it at entity+0x377, remote-only and ONLY
	                                  // inside the anim-state-accept branch [orig: @0x4c0cf2 write /
	                                  // @0x4c11a6 read; renamed from the `priority` misnomer]
	uint8_t  anim_def_index = 0;      // ADM anim-def index -> entity+0x2B0 + AdmDef_GetEntryByIndex
	                                  // -> entity+0x298. 0 is a VALID index — 0xFF is the null
	                                  // sentinel (entries stride 1120) [orig: @0x4c11f2/@0x4c120d;
	                                  // witness 2026-07-02 — an unknowing sender must use 0xFF]
	uint8_t  health_class_byte = 0;   // → Entity_SetHealthFromDifficultyByte
};

// One §5.13 compact record (15 B dead-pose / 21 B live). Decoded by
// [orig: Entity_SerializeVehicleState @ 0x460560]. Used by items with
// `ai_function` in {CHel, cveh, cbot, cpln, ctrn} — controllable vehicles
// and AI ground/air units sharing the vehicle network callback.
//
// FORM SEMANTICS (drive-authority witness 2026-07-04, supersedes the "mounted"
// reading): the flags bit 0x04 short form is the DEAD/WRECK pose-only form — the
// death family sets `Flags |= 6` (bits 1+2 together [orig: Entity_HandleDeathEvent
// @0x407118 / Entity_ProcessVehicleDestruction @0x466b7c / Entity_InitDeathState
// @0x48f96b et al.]), and the euler tail is the frozen wreck ORIENTATION (golden
// ASH_I5A: 16 parked buggies flip to flags=0x06 short-form in one mass-death frame
// f=237868). A LIVE vehicle — including one being DRIVEN — always streams the 21-B
// full form; drive replication is host-side simulation, not a form switch.
struct VehicleCompactRecord {
	uint16_t parent_slot_handle = 0xFFFF; // pool<<12|slot, 0xFFFF=none
	uint16_t pos_x_compressed = 0;        // entity+4   (vehicle-local if parent != none)
	uint16_t pos_y_compressed = 0;        // entity+8
	uint16_t pos_z_compressed = 0;        // entity+12
	// Orientation Euler triple (BAM-high i16 (v+0x8000)>>16). euler_z is read
	// pre-branch (always present); euler_x/euler_y follow only in the dead-pose
	// branch (the wreck's frozen full orientation). Together they feed
	// Math_BuildFixedPointMatrixFromEulerAngles.
	int16_t  euler_z = 0;                 // src entity+16 -> read-dest entity+576
	uint8_t  flags_byte = 0;              // entity+36 low byte
	bool     is_dead_pose = false;        // (flags_byte & 4) != 0 — the short/wreck
	                                      // form (renamed from the `is_mounted` misnomer)

	// Dead-pose branch (is_dead_pose = true) — the other two Euler components:
	int16_t  euler_y = 0;                 // src entity+24 -> read-dest entity+584
	int16_t  euler_x = 0;                 // src entity+20 -> read-dest entity+580

	// Live branch (is_dead_pose = false) — vehicle health + weapon-aim block:
	uint16_t weapon_x = 0;                // entity+160 (compressed)
	uint16_t health_word = 0;             // entity+286 (raw u16) = the vehicle HEALTH word: the
	                                      // read stores it back to entity+286 [orig: @0x460aff]
	                                      // and the destroyed-transition kill gates on it being
	                                      // non-zero [orig: @0x460a99 -> Entity_KillBySlotId
	                                      // @0x460ad9]. Renamed from the `turret_pitch_raw`
	                                      // misnomer (an unwitnessed decode-era guess, D-NET-63):
	                                      // a 0 here zeroes the vehicle's health EVERY frame —
	                                      // live-witnessed as all map vehicles dying repeatedly
	                                      // (retail-join v12, 2026-07-02).
	uint16_t weapon_aim_y = 0;            // src vehicleData[136] -> read-dest vehicleData[177] (compressed)
	uint16_t weapon_aim_z = 0;            // src vehicleData[135] -> read-dest vehicleData[178] (compressed)
	int16_t  weapon_heading_bam = 0;      // src vehicleData[132] -> read-dest vehicleData[179] (BAM high i16)
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

// One §5.9.1 ROUND-EVENT record (ex "weapon-hit" — a decode-era misnomer): a round
// FIRED by another player, carried as the fire origin + direction the receiving
// client re-simulates the round from (RoundData_SpawnRound); no impact is on the
// wire. Host write side: NetPacket_SerializeRoundEvent @0x504820 serializes one
// g_round_ring record per event; client read side:
// [orig: NetPacket_DeserializeRoundEvent @ 0x42F270], sole sender is the tag==2
// branch of the S2C 0x0A event loop [0x4306EF]. Variable length 17-20 B by
// `flags` gate bits (witness 2026-07-03, D-NET-152):
//   17 B if flags == 0
//   18 B if (flags & 0x80) — adds slot_byte
//   19 B if (flags & 0x40) — adds target_handle
//   20 B if (flags & 0xC0) — adds both
struct RoundEventRecord {
	uint8_t  flags = 0;               // fire-mode byte (ring+30: bit0 alt-fire, bit1 adm-indexed,
	                                  // bits 4-5 pre-consume magazine count low two bits)
	                                  // | 0x80 → slot_byte present
	                                  // [ring+32 != 0 @0x5048bb] | 0x40 → target_handle present
	                                  // [shooter's live fire target set @0x50485a]
	uint8_t  adm_index = 0;           // → AdmDef_GetEntryByIndex (action descriptor index)
	uint8_t  subtype = 0;             // shooter fire-context composite (ring+31) → dword_A822E0
	uint8_t  slot_byte = 0;           // weapon-slot id / uplink misc_byte (ring+32); iff flags&0x80
	uint16_t shooter_handle = 0xFFFF; // (pool<<12)|slot of the SHOOTER (ring+4) — the client
	                                  // resolves it as the round's owner entity [0x42f337]
	uint16_t target_handle = 0xFFFF;  // iff (flags & 0x40): the shooter's claimed target
	                                  // (shooter+104→+12, stamped by @0x50c2ad); 0xFFFF=sentinel
	uint16_t shot_seq = 0;            // per-shot sequence word (ring+28; the C2S 0x06 hit_part
	                                  // fire counter round-trips here) → word_B7C670
	uint16_t pos_x_compressed = 0;    // fire ORIGIN: Network_DecompressFixedPoint → + dword_A822E4
	uint16_t pos_y_compressed = 0;    // → + dword_A822E8
	uint16_t pos_z_compressed = 0;    // → + dword_A822EC
	uint16_t yaw_bam_high = 0;        // fire DIRECTION yaw, BAM high half (<< 16 on apply)
	uint16_t pitch_bam_high = 0;      // fire DIRECTION pitch, BAM high half

	bool has_slot_byte() const { return (flags & 0x80) != 0; }
	bool has_target_handle() const { return (flags & 0x40) != 0; }
};

bool decode_player_compact_record(const uint8_t *body, size_t len,
                                  PlayerCompactRecord &out, size_t &consumed);

bool decode_vehicle_compact_record(const uint8_t *body, size_t len,
                                   VehicleCompactRecord &out, size_t &consumed);

bool decode_infantry_compact_record(const uint8_t *body, size_t len,
                                    InfantryCompactRecord &out, size_t &consumed);

bool decode_round_event_record(const uint8_t *body, size_t len,
                               RoundEventRecord &out, size_t &consumed);

// ===========================================================================
// §5.15 Guided weapon record — per-(mode, field-group) projectile-state codec.
// [orig: Entity_SerializeGuidedMissileState @ 0x447C50]. Used by item classes
// rokt / stng / hlfr / jvln / arty / arti. UNLIKE the §5.10 / §5.13 / §5.14
// compact records, this is NOT one fixed body keyed on format 11 — it is a
// matrix of `mode` (packetCtx[6] ∈ {1..4}) × `field-group` (packetCtx[7] ∈
// {1..6}); each call serializes exactly ONE group. The group selector rides the
// wire as the `sub_op` byte of the 5-byte entity sub-header (EntityPacketSubHeader
// .sub_op): [orig: dispatch_entity_packet_callback @ 0x4D6A80] copies it to
// packetCtx[7] and hardwires packetCtx[6]=4 (read-apply) on the host C2S-receive
// path. The serializer rejects format 11, so guided entities NEVER appear as a
// §5.10b 0x0A compact record — decode_frame_update correctly fails closed on
// EntityClass::Guided.
//
// Per-group payload sizes (the bytes AFTER the sub-header):
//   group              write-full(1)  read-full(2)  write-delta(3)  read-apply(4)
//   1 Status            1 B (0x00)      0 B           1 B (0x00)      0 B
//   2 ClearTarget       1 B (0x00)      0 B           1 B (0x00)      0 B
//   3 TargetPos        14 B           14 B            2 B (target)    2 B (target)
//   4 TargetTypePos    18 B (+target) 18 B (+target) 16 B (no target)16 B (no target)
//   5 Pos              12 B           12 B           12 B            12 B
//   6 AttachOffsets    12 B           12 B           12 B            12 B
//
// Wire integration into the 0x0C entity-packet path + a full field-validation are
// DEFERRED: no capture in hand carries guided traffic (the 2026-06-16b loopback
// fired no rockets), so the per-group layouts are an IDA-structural port pinned
// only by the encode↔decode round-trip in nw_ingame_guided_test. The write-side
// 1-byte 0x00 marker for the status/clear groups (read side reads 0 B) is a
// framing detail the dispatcher owns; it is reproduced but not round-trippable
// at the serializer layer (see the test).
// ===========================================================================

enum class GuidedMode : uint8_t {
	WriteFull  = 1,  // host serialize, full state
	ReadFull   = 2,  // client deserialize, full state (no-op when authority)
	WriteDelta = 3,  // host serialize, delta
	ReadApply  = 4,  // deserialize-apply, delta (the 0x4D6A80 host-receive path)
};

enum class GuidedFieldGroup : uint8_t {
	Status        = 1,  // launch bits (entity+696|=1, entity+276|=0x1000)
	ClearTarget   = 2,  // clear target (entity+696&=~2, +724=0, +728=-1)
	TargetPos     = 3,  // full: u16 target + 3× i32 pos; delta: u16 target only
	TargetTypePos = 4,  // full: u16 target + i32 type + 3× i32 pos; delta: drops target
	Pos           = 5,  // 3× i32 pos (clears target on read)
	AttachOffsets = 6,  // 3× i32 attach offsets
};

// One guided projectile's replicated state. A given (mode, group) call touches
// only the subset of these fields its group covers; the rest stay default.
struct GuidedRecord {
	uint16_t target_slot = 0xFFFF;  // entity+724 — (pool<<12)|slot of the lock target
	uint16_t weapon_type = 0;       // entity+698 — wire-carried as a 4-byte field, low u16 kept
	int32_t  pos_x = 0;             // entity+700
	int32_t  pos_y = 0;             // entity+704
	int32_t  pos_z = 0;             // entity+708
	int32_t  attach_x = 0;          // entity+740
	int32_t  attach_y = 0;          // entity+744
	int32_t  attach_z = 0;          // entity+748
	// Status-bit effects of the read side (entity+696 flags), for callers.
	bool launched = false;          // group 1 read sets entity+696 |= 1
	bool target_bound = false;      // group 3/4 read sets entity+696 |= 2
	bool target_cleared = false;    // group 2/5 read clears the target
};

// Decode ONE guided field group from `body` (the bytes after the 5-byte entity
// sub-header). `mode` must be a read mode (ReadFull or ReadApply); `group` is the
// sub-header sub_op. Returns true iff the group's bytes were consumed cleanly;
// `consumed` is the byte count (0 for the read-side status/clear groups).
// [orig: Entity_SerializeGuidedMissileState @ 0x447C50]
bool decode_guided_field_group(GuidedMode mode, GuidedFieldGroup group,
                               const uint8_t *body, size_t len,
                               GuidedRecord &out, size_t &consumed);

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

// The 0x0A header's sub-block case 2 (`flags2 & 3 == 2`) — a global environment
// snapshot the host streams alongside motion: fog / time-of-day / clouds / quake.
// Every field's runtime landing + scale is witnessed.
// [orig: NapiNPClientMsg_0x00A @ 0x430244..0x43034C case 2]
struct FrameEnv {
	bool     present = false;
	uint16_t fog_dist = 0;      // → Env_FogDistTarget (raw << 16)        [0x430267]
	uint16_t fog_accel = 0;     // → Env_FogDistAccelClamp (raw << 8)     [0x430286]
	uint16_t tod_fixed = 0;     // → Env_CurTimeFixed24 (time-of-day, raw << 13) [0x4302AE]
	uint8_t  quake_ticks = 0;   // → Env_QuakeTicks (screen-shake)        [0x4302CE]
	uint8_t  cloud_scroll = 0;  // → Env_CloudScrollRateTarget (raw << 10) [0x4302EC]
	uint8_t  cloud_param2 = 0;  // → dword_26C6884 (raw << 8; 2nd cloud param) [0x430311]
	uint8_t  overcast = 0;      // → Env_OvercastBlendTarget (raw << 8)   [0x43032D]
	uint8_t  env_param = 0;     // → dword_2C059D0                        [0x43034C]
};

// The 0x0A header's sub-block case 0 (`flags2 & 3 == 0`, the common gameplay
// frame) — the recipient's WEAPON/reload/uniform state (the former name
// `FrameAimBlock` was a misnomer; server-side grill 2026-07-01). Written by
// NetPacket_WritePlayerState @0x4ff81b: preround timer, five per-player-slot
// weapon-overlay bytes (+360/+368 gated on entity+36 bit 1), the reload
// countdown, and the uniform team mask (ZoneSlotChain_GetOwnedZoneMask
// @0x4a2620). Client landings are exact. [orig: NetPacket_WritePlayerState
// @0x4ff81b (writer) / NapiNPClientMsg_0x00A @ 0x430054..0x430136 (reader)]
struct FrameWeaponBlock {
	bool     present = false;
	uint8_t  preround_timer = 0; // [orig: g_preround_delay_timer @0xC8D824] → dword_A85B64 [0x430064]
	uint8_t  slot_state360 = 0;  // playerSlot+360 (0 unless entity+36 bit 1) → dword_A85B5C [0x430084]
	uint8_t  slot_state368 = 0;  // playerSlot+368 (0 unless entity+36 bit 1) → dword_A85B60 [0x43009F]
	uint8_t  slot_state364 = 0;  // playerSlot+364 → dword_A85B68  [0x4300C3]
	uint8_t  slot_state356 = 0;  // playerSlot+356 → dword_A85B6C  [0x4300E3]
	uint8_t  slot_state460 = 0;  // playerSlot+460 → word_A85B7C   [0x430104]
	uint8_t  reload_seconds = 0; // reload countdown secs (0xFE cap; 0xFF = belt-fed special; 0 = idle)
	                             // [orig: @0x4ff8f0..0x4ff992] → dword_A85B70/B74 [0x43014D]
	int32_t  uniform_team_mask = 0; // [orig: @0x4ff9a3] → dword_A85BBC  [0x430136]
};

// The 0x0A header's sub-block case 1 (`flags2 & 3 == 1`) — the round/game timer
// snapshot (client only). [orig: NapiNPClientMsg_0x00A @ 0x430191..0x430235]
struct FrameTimerBlock {
	bool     present = false;
	uint8_t  state0 = 0;        // → dword_C6EAE0  [0x4301A1]
	uint8_t  state1 = 0;        // → dword_C6EAE4  [0x4301BC]
	uint8_t  state2 = 0;        // → dword_C8FC64  [0x4301E0]
	uint8_t  state3 = 0;        // → dword_C8FC68  [0x430200]
	int16_t  timer_seconds = 0; // → dword_24C1958 = 62 × this (62 Hz ticks); <0 ⇒ -1 [0x430235]
};

// The 0x0A header's sub-block case 3 (`flags2 & 3 == 3`) — objective-gametype
// state (4× i32, 16 B), present ONLY when the host's `g_GameType & 0x20000` bit is
// set. That gate is NOT on the wire, so decode_frame_update reads the body only
// when its `is_objective_gametype` hint is set. First witnessed in probe3 (Co-op,
// g_GameType 0x30020). [orig: NapiNPClientMsg_0x00A gate @ 0x430361, body
// @ 0x430363..0x4303D0]
struct FrameObjectiveBlock {
	bool     present = false;   // true iff all 16 objective bytes decoded (hint on + sub_block 3)
	int32_t  state[4] = {0, 0, 0, 0}; // → dword_AC86F4/F0/EC/E8
};

// The 0x0A conditional vehicle-passenger record (`flags2 & 0xF == 8`) — the
// local player's seat orientation when riding as a passenger (not driver).
// [orig: NapiNPClientMsg_0x00A @ 0x430459..0x4304DC]
struct FramePassenger {
	bool     present = false;
	uint16_t handle = 0xFFFF;   // seat's vehicle handle; 0xFFFF ⇒ no seat yaw/pitch
	bool     has_seat = false;  // true when handle != 0xFFFF (yaw/pitch follow)
	uint16_t seat_yaw = 0;
	uint16_t seat_pitch = 0;
};

struct FrameUpdate {
	// Header refs (dword_A822E4/E8/EC) — the i32 16.16 world anchor each compact
	// record's decompressed position is added to (when unmounted).
	int32_t anchor_x = 0, anchor_y = 0, anchor_z = 0;
	uint8_t  flags1 = 0;            // loadprog / death-spectator signals
	uint8_t  flags2 = 0;            // low 2 bits = sub-block selector, bit 3 = passenger gate
	uint8_t  sub_block = 0;         // flags2 & 3 (0/1/2/3)
	// 7-byte fixed tail (local-player state) [orig: 0x4303E5..0x430442].
	uint8_t  state_flag_byte = 0;
	uint16_t mount_handle = 0xFFFF; // local-player vehicle-mount (header tail)
	int16_t  health = 0;            // local-player health
	int16_t  state_word = 0;
	bool     local_tail_present = false; // all seven recipient-local tail bytes decoded
	FrameWeaponBlock    weapon;     // valid iff sub_block == 0
	FrameTimerBlock     timer;      // valid iff sub_block == 1
	FrameEnv            env;        // valid iff sub_block == 2
	FrameObjectiveBlock objective;  // valid iff sub_block == 3 (+ objective gate)
	FramePassenger      passenger;  // valid iff (flags2 & 0xF) == 8
	std::vector<FrameUpdateRecord> records;      // tag==1 per-entity motion
	std::vector<RoundEventRecord>  round_events; // tag==2 fired-round events (§5.9.1)
	// Walk status: `complete` is true iff the event loop hit its terminator (tag
	// 0 / end) cleanly. `consumed` is the byte count walked (for diagnostics).
	bool   complete = false;
	size_t consumed = 0;
};

// Walk a S2C 0x0A body into a FrameUpdate — the single, complete decode of the
// message (the same walk nw_pp's printer renders). Captures the anchor + header
// flags, the env sub-block (case 2), the local-player tail, the conditional
// passenger record, every tag==1 per-entity compact record, AND every tag==2
// weapon-hit record (§5.9.1). `class_of` maps a wire type_id to its compact
// dispatch class (built from items.def). Known null callbacks consume only the
// `[tag][handle][typeId]` header and continue [orig: null-callback branch
// @ 0x430814..0x43081D]. Returns true (and sets out.complete) iff the event loop
// reached its terminator cleanly; on any short read / unresolved-width class it
// stops, leaving everything decoded so far in `out` (out.complete=false,
// out.consumed = bytes walked) so callers can render partial state + the failure
// point. [orig: NapiNPClientMsg_0x00A @ 0x42FEC0]
// `is_objective_gametype` gates the sub-block-3 objective body (16 B): the host
// only emits it when `g_GameType & 0x20000` is set — a gate not on the wire, so
// the caller supplies it (e.g. from the 0x7B `extra` field = g_GameType). Default
// false (correct for every non-objective capture).
bool decode_frame_update(const uint8_t *body, size_t len,
                         const std::function<EntityClass(uint16_t)> &class_of,
                         FrameUpdate &out, bool is_objective_gametype = false);

// ===========================================================================
// S2C game-event + kill messages — the kill feed and entity-death replication.
// ===========================================================================

// S2C 0x1E — game event (kill feed + objectives + zone control). Fixed 8 B.
// [orig: NetPacket_HandleGameEvent @ 0x426270]. The client resolves the three
// pool-0 indices to entities, then a ~60-case switch on event_type selects a
// "Canned Msg"/STRCNDnn string, formats it via HUD_FormatKillEventMessage
// (@ 0x422DA0) and posts it to the kill feed (Chat_AddDebugMessage); some types
// also trigger a sound / progress-bar / effect. Only processed in-session (except
// type 48). pos is the event's world map location (handler shifts i16 << 16 → 16.16).
struct GameEventRecord {
	uint8_t  event_type = 0;        // 1-60; selects the canned message + behavior
	uint8_t  attacker_index = 0xFF; // pool-0 index (0xFF = none) → Pool_GetEntryUnchecked(0,*)
	uint8_t  victim_index = 0xFF;   // pool-0 index (0xFF = none)
	uint8_t  aux_index = 0xFF;      // pool-0 index (0xFF = none) — 3rd actor / means
	int16_t  pos_x = 0;             // world X in meters (handler shifts << 16 to 16.16)
	int16_t  pos_y = 0;             // world Y in meters
};

bool decode_game_event(const uint8_t *body, size_t len, GameEventRecord &out,
                       size_t &consumed);

// S2C 0x61 — the per-player TICK SEED (the "session key" name is a misnomer). The client
// anchors its whole network-role clock to this value: it stores the seed into currentTick
// AND the keepalive anchor, skips the per-frame increment while the clock is zero, and
// stamps the resulting tick at off-0 of every C2S 0x06. The host stamps the same value as
// that player's fire-freshness floor and rejects a shot whose tick is zero or not past it,
// so a client that ignores this message can never land a shot on a stock host. A body
// shorter than four bytes seeds ZERO — which is also the witnessed round-end disarm form,
// so a short read is a valid seed, not a decode failure (this always returns true).
// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 — currentTick @0xA8229C @0x4297f8,
//  g_lastKeepaliveTick @0xA822A0 @0x4297fd, short-body zero @0x4297eb;
//  Server_SendRandomSeedToPlayer @0x5101a0 (value @0x5101d4, disarm @0x510237);
//  gate PlayerSlot_IsActive @0x4fc760]
bool decode_tick_seed(const uint8_t *body, size_t len, uint32_t &out);

// Coarse classification of a 0x1E event_type, derived structurally from the
// handler's switch [orig: 0x426270]. Drives the viewer's kill-feed styling.
enum class GameEventKind : uint8_t {
	Other = 0,     // single-actor canned / misc HUD message
	Kill,          // attacker killed victim (the kill feed proper)
	Objective,     // flag / capture / zone control / camp events
};
GameEventKind game_event_kind(uint8_t event_type);

// The witnessed "Canned Msg" string key (e.g. "STRCND04") an event_type maps to,
// or nullptr when the type has none. Faithful to the 0x426270 switch.
const char *game_event_strcnd_key(uint8_t event_type);

// S2C 0x26 — entity kill replication. Fixed 4 B `[u16 victim_slot][u16 attacker]`.
// [orig: NapiNPClientMsg_0x026 @ 0x42EC30 → Entity_KillBySlotId(victim, attacker, 0)
//  @ 0x42BCE0 — arg0 is the DYING entity, arg1 the killer]. Client-only.
struct KillRecord {
	uint16_t victim_slot = 0xFFFF;  // (pool<<12)|slot of the entity that dies
	uint16_t attacker = 0xFFFF;     // killer handle / id recorded on the hit
};
bool decode_kill_record(const uint8_t *body, size_t len, KillRecord &out,
                        size_t &consumed);

// S2C 0x4E — batch despawn/kill. `[u16 count][count × u16 slot]`; each slot is
// killed via Entity_KillBySlotId(slot, 0, 1), then the handler replies C2S 0x28.
// [orig: NapiNPClientMsg_HandleBatchKill @ 0x431870 (Kong labeled this HandleBatchSpawn; it kills)].
struct BatchKillBatch {
	uint16_t count = 0;
	std::vector<uint16_t> slots;    // (pool<<12)|slot of each despawned entity
};
bool decode_batch_kill(const uint8_t *body, size_t len, BatchKillBatch &out);

// S2C 0x5D — the EMPTY-SLOT SWEEP. `[i16 pool0Index] × N` — RAW POOL-0 INDICES,
// not packed (pool<<12|slot) handles: the handler resolves each with
// `Pool_GetEntryUnchecked(0, idx)`. Per entry it runs `Entity_Destroy`, then
// `PlayerSlot_FindByType(idx)` and, when that slot is active (slot+13), calls
// `PlayerSlot_ClearAndUnlink @0x434730`. The whole handler is gated
// `!is_authority`. It is the reply to a client's C2S 0x32 request: the server
// answers with the pool-0 slots IT considers empty and the client destroys
// whatever it still holds there — a permanent-ghost sweep, not a per-kill
// despawn. [orig: NapiNPClientMsg_DestroyEntityList @ 0x429730; sender
//  NapiNPServerMsg_SendEmptySlots @ 0x51a600, body builder @ 0x5160f0]
struct DestroyEntityList {
	std::vector<uint16_t> pool0_indices; // raw pool-0 slot indices (NOT handles)
};
bool decode_destroy_entity_list(const uint8_t *body, size_t len,
                                DestroyEntityList &out);

// S2C 0x50 — TEAM ASSIGN. `[u16 entityHandle][u8 team][u16 netId][u8 animSlot]`;
// a short body defaults each REMAINING field to 0 (the handler reads what is
// there and leaves the rest zero). Gates: handle != 0xFFFF,
// `(handle & 0xF000) < 0x5000`, slot < that pool's capacity.
// Legs, in the witnessed order:
//   (1) entity == local player -> `byte_A85B48 = team` @0x4319db — the SAME latch
//       the S2C 0x04 tail byte writes (our JoinerConnection::assigned_team_);
//   (2) !is_authority -> `entity->Team = team` @0x4319ee for ANY pool 0..4 entity;
//   (3) the player-slot team byte mirrors it (slot+14, @0x431a0b);
//   (4) `entity->Flags & 0x100` (a player) AND entity == local player -> retail
//       re-selects the per-side profile (team 1/3 -> side A block, else side B),
//       refreshes restrictionData, and RE-SENDS ONE C2S 0x2F via
//       NetPacket_SendLoadoutSubmit @0x431a9e carrying the NEW team, the per-side
//       profile class, and slot 195 RAW (the pre-Player_InitPlayer form, NOT the
//       live g_currentWeaponSlot), then C2S 0x22/0x23 acks (@0x431acb..0x431b05),
//       Player_InitPlayer(1) @0x431b14, then the identity stores
//       `entity->animSlot = animSlot` @0x431b3a / `entity->NetId = netId` @0x431b46
//       and the minimap maintenance they feed (@0x431b4d..0x431b91).
// [orig: NapiNPClientMsg_0x050 @ 0x431910; host producer Server_ChangeEntityTeam
//  @ 0x518D70 — it retargets ANY entity, including capture zones (§5.61)]
struct TeamAssign {
	uint16_t entity_handle = 0xFFFF; // (pool<<12)|slot
	uint8_t  team = 0;
	// entity+0x15C wire NetId + entity+0x374 animSlot, the same identity pair the 0x0C
	// player record carries (fields 3-4 / 5). Retail's producer ZEROES both for a
	// non-player entity — the `entity->Flags & 0x100` gate @0x506b3d picks between the
	// live values @0x506b51/@0x506b6b and the zero arms @0x506b96/@0x506bab — so any
	// future emitter must reproduce that gate. [orig: write_entity_handle_packet @0x506ad0]
	uint16_t net_id = 0;
	uint8_t  anim_slot = 0;
};
bool decode_team_assign(const uint8_t *body, size_t len, TeamAssign &out,
                        size_t &consumed);

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
	uint8_t  sub_op = 0;        // ENTITY_SUB_OP_EXTENDED or ENTITY_SUB_OP_COMPACT
};

// sub_op selector values (§5.10b). NOT message tags — sub_op 0x0A is unrelated
// to tag 0x0A. [orig: Pool_SerializeEntityViaVTable @0x4D64E0 writes;
// dispatch_entity_packet_callback @0x4D6A80 dispatches]
inline constexpr uint8_t ENTITY_SUB_OP_EXTENDED = 0x0A; // type-10 extended (§5.10 joiner uplink)
inline constexpr uint8_t ENTITY_SUB_OP_COMPACT = 0x0B;  // type-11 compact (S2C 0x0A trailing records)

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
// Position fields are 16.16 fixed-point. Vehicle-LOCAL when `carrier_handle !=
// 0xFFFF` (the host's case-4 path adds map origin only on the unmounted branch).
//
// The 8 trailing u16 pairs are the host-validated anti-cheat block: 4 ×
// (weapon_id, fire_counter). The host compares these against its own per-slot
// counters to detect shot/hit tally tampering.
struct PlayerExtendedUplink {
	uint16_t carrier_handle = 0xFFFF;  // pool<<12|slot; 0xFFFF=none. The sender's GROUND
	                                   // entity (+0x28) — building floor, vehicle deck; ANY
	                                   // pool 0-4, pool-2 statics included [orig: op3 reads
	                                   // entity+0x28 at its case head (the same field op1
	                                   // reads @0x4c0a08); the op4 apply resolves it against
	                                   // g_pool_list @0x4c1d07-0x4c1d26]. Renamed from the
	                                   // carrier_handle misnomer (witness 2026-07-03,
	                                   // D-NET-151).
	int32_t  pos_x = 0;                // entity+4/+8/+0xC — ABSOLUTE world 16.16 when free
	int32_t  pos_y = 0;                // (no map-origin add); CARRIER-LOCAL when
	int32_t  pos_z = 0;                // carrier_handle != 0xFFFF (Entity_TransformWorldToLocal
	                                   // @0x43BB50 on write / LocalToWorld @0x43BD00 on apply)
	int16_t  heading = 0;              // entity+0x10 hi-word; CARRIER-RELATIVE when grounded
	                                   // (transform out[3] = heading - carrier heading; the
	                                   // apply re-adds the carrier heading @0x43be7e)
	int16_t  pitch   = 0;              // entity+0x14 hi-word (pose pass-through, never local)
	uint8_t  anticheat_flags = 0;      // the sender's rotating self-check accumulator
	                                   // (IsDebuggerPresent / D3D9-hook / speed checks — the
	                                   // op3 dword_B5ABA8 counter switch); the op4 apply
	                                   // advances the cursor WITHOUT storing it (the byte
	                                   // right after the pose block) — renamed from
	                                   // reserved_18, same no-read behavior
	uint8_t  move_input_byte = 0;      // entity+0x12C low byte — the movement-INPUT bitfield the
	                                   // client reports for its own player (renamed from the
	                                   // anim_slot_low misnomer; witness 2026-07-02)
	uint8_t  state_flags_byte = 0;     // the RAW entity+0x24 (Flags) low byte, written verbatim
	                                   // by op3; the apply REPLACES bits 2-4 of the host
	                                   // entity's Flags: `flags ^= (flags ^ wire) & 0x1C`
	                                   // [orig: @0x4c1e4d]. NOT an xor-delta — the old
	                                   // flags_xor name and the xor-apply it induced were
	                                   // wrong (witness 2026-07-03, D-NET-151; the crouch/
	                                   // prone stance family is bits 2-4).
	uint8_t  analog_x = 0;           // entity+0x130
	uint8_t  analog_y = 0;           // entity+0x131
	uint8_t  analog_z = 0;           // entity+0x132
	uint8_t  equipped_adm_index = 0;   // entity+0x2B0 equipped-weapon AdmDef index — case-4 store
	                                   // @0x4C20A3 gated AdmDefs[idx].category < 11; the host
	                                   // ECHOES it at 0x0A off-16 (renamed from the reserved_24
	                                   // "read into AL, discarded" misnomer; witness 2026-07-02,
	                                   // D-NET-143)
	uint8_t  stat_byte_0 = 0;          // playerSlot+0x15F78
	uint8_t  stat_byte_1 = 0;          // playerSlot+0x15F79

	// Entity-priority feedback: 4 × (handle_u16, score_u16) — the sender's top-4
	// interest pairs from Server_BuildEntityPriorityListForPlayer(entity, .., 4)
	// [orig: op3 call @0x4c1be9], stored by the host at playerSlot+0x1708A..+0x170A0
	// (scores zero-extended to u32). The old weapon_id/fire_counter names were a
	// decode-era guess — v26 shows the ridden buggy's handle scored first while
	// standing on it (witness 2026-07-03, D-NET-151).
	uint16_t priority_handle_0 = 0;    // playerSlot+0x1708A
	uint16_t priority_score_0 = 0;     // playerSlot+0x17094 (zero-ext)
	uint16_t priority_handle_1 = 0;    // playerSlot+0x1708C
	uint16_t priority_score_1 = 0;     // playerSlot+0x17098 (zero-ext)
	uint16_t priority_handle_2 = 0;    // playerSlot+0x1708E
	uint16_t priority_score_2 = 0;     // playerSlot+0x1709C (zero-ext)
	uint16_t priority_handle_3 = 0;    // playerSlot+0x17090
	uint16_t priority_score_3 = 0;     // playerSlot+0x170A0 (zero-ext)
};

// Decode a 43-B extended uplink body (the bytes AFTER the 5-byte sub-header).
// Returns true iff 43 B were consumed cleanly.
bool decode_player_extended_uplink(const uint8_t *body, size_t len,
                                   PlayerExtendedUplink &out, size_t &consumed);

// ===========================================================================
// C2S 0x06 — "client fired round". Fixed 45 B. Joiner reports a single
// weapon-fire event (calculated pose + target + shot counter + five low-word
// pose deltas). The host validates it in Server_ClientFiredRound @0x50baa0
// (anti-spoof, cease-fire, adm lookup, warp compensation, mounted-fire, ammo)
// and an accepted PRIMARY fire runs the adm 'fire' action → re-enters the
// validator locally → RoundData_AddRound appends a g_round_ring event that
// fans to every OTHER in-match recipient as an S2C 0x0A tag-2 round event
// (§5.9.1); alt fire appends directly. (D-NET-152)
// [orig: NapiNPServerMsg_0x006_ClientFiredRound @ 0x513310]
// ===========================================================================

// C2S 0x06 hit_part packing: `(roster slot << 9) | (shot seq & 0x1FF)`
// [orig: Server_ClientFiredRound @0x50bda5]. The 9-bit split is the witnessed one; the
// roster slot is the S2C 0x04 body byte 17 the host assigned this client
// [orig: NetPacket_WriteSlotAssignment @0x502b30]. Named because sending a bare
// sequence (slot bits 0) makes a retail host attribute the round to its OWN slot 0.
inline uint16_t pack_fired_round_hit_part(uint8_t roster_slot, uint16_t shot_seq)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(roster_slot) << 9) |
	                             (shot_seq & 0x1FFu));
}
inline uint8_t fired_round_hit_part_slot(uint16_t hit_part) { return static_cast<uint8_t>(hit_part >> 9); }
inline uint16_t fired_round_hit_part_seq(uint16_t hit_part) { return static_cast<uint16_t>(hit_part & 0x1FFu); }

struct ClientFiredRound {
	uint32_t current_tick = 0;        // client network-role currentTick; host cooldown/freshness anchor
	uint16_t shooter_handle = 0xFFFF; // pool<<12|slot; >= 0x5000 high nibble = invalid
	uint8_t  fire_flags = 0;          // bit 0 set → "alt fire" path (ammo not deducted)
	uint8_t  adm_index = 0;           // AdmDef_GetEntryByIndex key — action descriptor (§5.9.1 shares this)
	int32_t  pos_x = 0;               // calculated fire-pose origin (i32 LE, 16.16)
	int32_t  pos_y = 0;
	int32_t  pos_z = 0;
	int32_t  dir_x = 0;               // direction (host shifts << 16 to BAM-extend); wire is raw i32 LE
	int32_t  dir_y = 0;
	uint16_t target_handle = 0xFFFF;  // 0xFFFF = no target
	// NOT a bare sequence — a PACKED word, `(roster slot << 9) | (shot seq & 0x1FF)`.
	// The host copies it verbatim into the global word_B7C670 on the network arm
	// [orig: Server_ClientFiredRound @0x50c2ba / @0x50c774], and its own composition of
	// the same word packs the shooter's per-player record slot+20 into bits 9.. exactly
	// this way [orig: @0x50bda5 `(*((WORD*)v91 + 10) << 9) | (packet & 0x1FF)`]. slot+20
	// is the roster id the S2C 0x04 hands the client in body byte 17
	// [orig: NetPacket_WriteSlotAssignment @0x502b30].
	// Leaving bits 9.. zero names roster slot 0 — on a listen host, the HOST ITSELF —
	// and a live retail host then attributed our rounds to its own player. Build it with
	// npwire::pack_fired_round_hit_part.
	uint16_t hit_part = 0;
	uint8_t  extra_byte1 = 0;         // shooter entity+352 low byte → dest[18]
	uint8_t  extra_byte2 = 0;         // → dword_C86FB4 global (last-fire context)
	uint8_t  misc_byte = 0;           // → LOBYTE(dest[20])
	// Low-word modulo deltas: calculated fire pose {X,Y,Z,Yaw,Pitch} minus
	// shooter live pose dwords 1..5. Host adds them to its shooter pose to
	// reconstruct dest[10..14] [orig: NetPacket_WriteEntityPositionUpdate
	// @ 0x42a80f..0x42a890 producer; @0x513310 receiver].
	uint16_t delta_x = 0;             // fire X low16 - shooter X low16
	uint16_t delta_y = 0;             // fire Y low16 - shooter Y low16
	uint16_t delta_z = 0;             // fire Z low16 - shooter Z low16
	uint16_t delta_yaw = 0;           // fire Yaw low16 - shooter Yaw low16
	uint16_t delta_pitch = 0;         // fire Pitch low16 - shooter Pitch low16
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

// ===========================================================================
// Uncharacterized-tag bodies field-mapped from IDA (D-NET-73 / D-NET-74). These
// dispatch + frame cleanly; their bodies were the §8 D-NET-72 deferral. Field
// maps: docs/net/novaworld-net-re.md §5.28-§5.33.
// ===========================================================================

// S2C 0x5A — weapon-loadout sync (§5.30). `[u8 avatarClass]` then a slot chain
// `{ u8 typeId, u8 ammoPrimary, u8 ammoSecondary, u8 ammoAlt }` terminated by
// `typeId == 0xFF` (the terminator replaces the next typeId). The retail handler
// drops slots whose typeId fails AdmDef_GetEntryByIndex and caps at 40 raw
// slots; the wire decoder keeps every slot (AdmDef validation is a runtime
// concern, not a wire field — a deliberate non-divergence).
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @ 0x4290E0]
struct WeaponLoadoutSlot {
	uint8_t type_id = 0;
	uint8_t ammo_primary = 0;
	uint8_t ammo_secondary = 0;
	uint8_t ammo_alt = 0;
};
struct WeaponLoadout {
	uint8_t avatar_class = 0;
	std::vector<WeaponLoadoutSlot> slots;   // chain until typeId 0xFF (<= 40)
};
bool decode_weapon_loadout(const uint8_t *body, size_t len, WeaponLoadout &out);

// S2C 0x6E — team/squad roster sync (§5.31). `[u8 teamCount]` then per team
// `{ u16 teamEntityHandle, u16 teamSlotIndex, u8 memberCount, u16 teamSlotHandle,
//   u16 member × memberCount }`. teamEntityHandle == 0xFFFF marks "no team
// entity" (the handler skips the entity-slot write but still reads every field).
// [orig: NapiNPClientMsg_HandleSquadRosterSync @ 0x429880]
struct RosterTeam {
	uint16_t team_entity_handle = 0xFFFF; // (pool<<12)|slot; 0xFFFF = none
	uint16_t team_slot_index = 0;         // index into the roster arrays
	uint8_t  member_count = 0;            // -> entity+550
	uint16_t team_slot_handle = 0;        // -> entity+548
	std::vector<uint16_t> members;        // member_count member handles
};
struct RosterSync {
	uint8_t team_count = 0;
	std::vector<RosterTeam> teams;
};
bool decode_roster_sync(const uint8_t *body, size_t len, RosterSync &out);

// S2C 0x7B — full player/session info (§5.32). Five NUL-terminated strings, then
// `[u32 extra]`, then two more NUL-terminated strings. The retail handler caps the
// dest buffers (32 / 512) but advances the wire by strlen+1 — the caps are dest
// sizes, not wire widths.
//
// Field roles are witnessed from the landing globals, NOT the Hex-Rays
// "clan/squad/label/rank" auto-comment (which is wrong on every field). The
// PunkBuster cvar map [orig: PunkBuster_GetCvarValue @ 0x4D96A0] ties three of the
// strings to named cvars (`name` → string 1, `sv_hostname` → string 3, `mapname` →
// string 5, `gamename` → string 7), and string 2 lands in the slot the S2C 0x7A
// player-name handler also writes (stru_A86920.pad9[196] @ 0x429B40). Cross-capture:
// string 2 is a persistent per-player zero-padded number (FooPlayer = "00000003"
// across every loopback; a second player = "00000005"), populated INSTEAD of the
// display name on a NovaWorld account join and empty for a LAN/local join — i.e. the
// server's player/account ID, NOT a clan tag.
// [orig: NapiNPClientMsg_HandlePlayerInfoFull @ 0x429BB0]
struct FullPlayerInfo {
	std::string player_name;   // 1 — local/LAN display name (PunkBuster `name`); empty on account joins
	std::string player_id;     // 2 — NovaWorld player/account ID (0-padded numeric, persistent per player); empty on LAN joins — NOT a clan tag
	std::string server_name;   // 3 — host/server name (PunkBuster `sv_hostname`)
	std::string mission_name;  // 4 — mission display title
	std::string map_file;      // 5 — .bms filename (PunkBuster `mapname`)
	uint32_t    extra = 0;     // u32
	std::string motd;          // 6 — unwitnessed (empty in every capture); the "MOTD" guess is unconfirmed
	std::string game_name;     // 7 — game name (PunkBuster `gamename`)
};
bool decode_full_player_info(const uint8_t *body, size_t len, FullPlayerInfo &out);

// S2C 0x0F — world-state-load (§5.29). The joiner's spawn pose + game flags + a
// fixed team-score table + waypoint/team-name lists (~624 B). Layout:
//   [i32 sessionTick][i32 posX][i32 posY][i32 posZ]   (pos 16.16)
//   [i16 yaw][i16 pitch][i16 roll]                     (each <<16 to 16.16)
//   [u8 gameFlags]
//   i32 teamScores[kWorldStateScoreCount]              (fixed 128-entry block)
//   [u16 waypointCount]
//     { u16 slotId, u16 nameId, u8 pad } × waypointCount  // host-gametype-gated
//   [u16 teamNameCount]
//     cstring × teamNameCount
// The waypoint records are gated on the HOST by (g_GameType & 0xFFFDFFFF) ==
// 0x10020 (a waypoint gametype). That gate is NOT on the wire, so an off-wire
// decoder takes the is_waypoint_gametype hint (default false; TDM/DM send
// waypointCount 0 / no records). [orig: NapiNPClientMsg_0x00F @ 0x42E200]
inline constexpr int kWorldStateScoreCount = 128; // (data - outTable)/4 @ 0x42e324
struct WorldStateWaypoint {
	uint16_t slot_id = 0;
	uint16_t name_id = 0;
	uint8_t  pad = 0;          // read-and-discard by the handler (cursor advance)
};
struct WorldStateLoad {
	uint32_t session_tick = 0;
	int32_t  pos_x = 0, pos_y = 0, pos_z = 0;
	int16_t  yaw = 0, pitch = 0, roll = 0;
	uint8_t  game_flags = 0;
	std::array<int32_t, kWorldStateScoreCount> team_scores{};
	uint16_t waypoint_count = 0;
	std::vector<WorldStateWaypoint> waypoints;  // populated only when the hint is set
	uint16_t team_name_count = 0;
	std::vector<std::string> team_names;
};
bool decode_world_state_load(const uint8_t *body, size_t len, WorldStateLoad &out,
                             bool is_waypoint_gametype = false);

// S2C 0x60 / 0x64 — chunked file transfer (§5.28). BOTH tags share a 12-byte
// header `[u32 transferId/checksum][u32 totalSize][u32 chunkOffset]` then
// `len - 12` RAW file bytes, written at chunkOffset into a reassembly buffer.
// On `chunkOffset + chunkSize >= totalSize` the transfer completes; otherwise the
// client re-requests the next chunk (0x60 -> C2S 0x33, 0x64 -> C2S 0x37; payload
// `[transferId][nextOffset]`, 8 B). There is NO compression codec — the payload
// is literal file content (0x60 reassembles into a CDataStream; 0x64 into a
// buffer whose completion yields 3 mission-name strings). probe2 completed each
// transfer in one chunk, so the re-requests never fired (D-NET-74; refines the
// D-NET-69 "streamed, not chunked" wording).
// [orig: NapiNPClientMsg_HandleFileTransferChunk @ 0x432350 (0x60) /
//        NapiNPClientMsg_HandleMissionDataChunk @ 0x432410 (0x64)]
struct FileTransferChunk {
	uint32_t transfer_id = 0;            // dword_A822C0 (0x60) / dword_A822C4 (0x64)
	uint32_t total_size = 0;             // full transfer size (all chunks)
	uint32_t chunk_offset = 0;           // where this chunk's bytes land
	size_t   chunk_size = 0;             // len - 12 (this chunk's payload bytes)
	const uint8_t *chunk_data = nullptr; // points into `body` at +12
	bool is_final() const {
		return uint64_t(chunk_offset) + chunk_size >= total_size;
	}
};
bool decode_file_transfer_chunk(const uint8_t *body, size_t len, FileTransferChunk &out);

// ---------------------------------------------------------------------------
// C2S burst replies (§5.33) — small client->server requests the client queues in
// response to S2C load/sync messages. Field-mapped from the authority SERVER
// read-handlers (the canonical body); each serializes a reply back to the client.
// ---------------------------------------------------------------------------

// C2S 0x22 — player-sync request `[u8 slot][u16 fieldFlags]` (3 B). Server replies
// S2C 0x46 for `slot` with `fieldFlags`. [orig: NapiNPServerMsg_0x022 @ 0x514C90]
struct BurstPlayerSyncRequest {
	uint8_t  slot = 0;
	uint16_t field_flags = 0;
};
bool decode_burst_player_sync_request(const uint8_t *body, size_t len,
                                      BurstPlayerSyncRequest &out, size_t &consumed);

// C2S 0x23 — visible-players request, EMPTY body (0 B). Server replies S2C 0x4C
// with a visible-players snapshot. [orig: NapiNPServerMsg_0x023 @ 0x514D50]
bool decode_burst_visible_request(const uint8_t *body, size_t len, size_t &consumed);

// C2S 0x32 — EMPTY-SLOT SWEEP REQUEST. The client queues it inside its S2C 0x0F
// world-state-load reply burst (@0x42e647, beside 0x28/0x29/0x2D — §5.29); the
// host's handler reads NO fields from it and answers S2C 0x5D with every empty
// pool-0 slot index. The handler is authority-gated and skipped while
// `g_net_spawn_suspended` or `g_spawn_success_gate` (round over) is set. Because
// the body is never read, this decoder consumes nothing and accepts any length —
// the sender's exact filler (if retail writes any) is unwitnessed.
// [orig: NapiNPServerMsg_SendEmptySlots @ 0x51a600; body builder @ 0x5160f0;
//  client sender NapiNPClientMsg_0x00F @ 0x42e647]
bool decode_empty_slots_request(const uint8_t *body, size_t len, size_t &consumed);

// C2S 0x28 — weapon-loadout request `[u32 loadoutFilter][u32 flags][u16 extra]`
// (10 B). Server replies S2C 0x4E.
// [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @ 0x51A550]
struct BurstLoadoutRequest {
	uint32_t loadout_filter = 0;
	uint32_t flags = 0;
	uint16_t extra = 0;
};
bool decode_burst_loadout_request(const uint8_t *body, size_t len,
                                  BurstLoadoutRequest &out, size_t &consumed);

// C2S 0x29 — team/spawn ack `[u16 team_change_index]` (2 B). The client emits it with
// team_index+1 from its 0x51 apply [orig: NapiNPClientMsg_HandlePlayerSpawn @ 0x431c99]
// and at deploy/team pick; the server treats the value as a g_team_change_entity_list
// index and replies S2C 0x51 ONLY for a pending team-change entry [orig:
// NapiNPServerMsg_0x029 @ 0x514F10 @ 0x514f7c] — never on a plain join (D-NET-148).
struct TeamSpawnAck {
	uint16_t team_change_index = 0;
};
bool decode_team_spawn_ack(const uint8_t *body, size_t len,
                           TeamSpawnAck &out, size_t &consumed);

// C2S 0x4C — client quality/state byte `[u8 value]` (server clamps to 0..4 and
// sets the player's connection-quality state). [orig: NapiNPServerMsg_0x04C @ 0x5111B0]
struct BurstClientQuality {
	uint8_t value = 0;  // 0..4 after clamp
};
bool decode_burst_client_quality(const uint8_t *body, size_t len,
                                 BurstClientQuality &out, size_t &consumed);

// ===========================================================================
// Session/transport control pings (§5.34) — RTT ping/pong + periodic request
// trio. These are NAPI transport / anti-cheat keepalives, not gameplay
// replication: each carries a single scalar and triggers a fixed reply. They
// dominate the wire by volume (the RTT pair alone is ~10k each per session).
// ===========================================================================

// S2C 0x57 / C2S 0x2C — RTT ping/pong. Identical 5-B body `[u32 timestamp]
// [u8 echo_flag]`. The two handlers mirror each other: when echo_flag != 0 the
// receiver bounces the timestamp straight back (0x57→C2S 0x2C, 0x2C→S2C 0x57)
// with echo_flag cleared; when echo_flag == 0 the receiver measures
// rtt = GetTickCount() - timestamp into a 10-sample ring (the server side also
// enforces g_MinPing / g_MaxPing, kicking persistent violators).
// [orig: NapiNPClientMsg_0x057_RTT @ 0x432210 (S2C 0x57);
//        NapiNPServerMsg_HandlePingResponse @ 0x515070 (C2S 0x2C)]
struct RttSample {
	uint32_t timestamp = 0;   // sender's GetTickCount() ms stamp to echo / measure
	uint8_t  echo_flag = 0;   // !=0 ⇒ bounce back; 0 ⇒ measure rtt = now - timestamp
};
bool decode_rtt_sample(const uint8_t *body, size_t len,
                       RttSample &out, size_t &consumed);

// S2C 0x68 / 0x43 / 0x39 — periodic request trio. Each parses a single `[u32]`
// (4 B) and queues a fixed reply built from local state; the inbound parse is
// structurally identical across the three, so one reader serves all of them.
// Per-tag semantics (field meaning + the reply each triggers):
//   0x68  start_index      → reply C2S 0x3D (frozen loaded-model rows, paged from start_index)
//                            [orig: NapiNPClientMsg_0x068 @ 0x42DAA0]
//   0x43  server_timestamp → reply C2S 0x08 (`[u32 server_ts][u32 GetTickCount]`,
//                            the time-sync / anti-speedhack echo)
//                            [orig: NapiNPClientMsg_0x043 @ 0x42FA90]
//   0x39  challenge_seed   → reply C2S 0x1C (seed XOR CRC of the local player's
//                            124-byte charattr CHARACTER row)
//                            [orig: NapiNPClientMsg_HandleChecksumChallenge @ 0x42E6D0]
bool decode_u32_scalar(const uint8_t *body, size_t len,
                       uint32_t &out_value, size_t &consumed);

// ===========================================================================
// Minimap overlays, weapon reload, second death path, entity-checksum, and
// misc client scalars (§5.35) — the per-entity HUD / lifecycle notifications
// the host streams alongside the 0x0A frame.
// ===========================================================================

// S2C 0x6B — minimap overlay batch. `[u8 count]` + `count × 12-B records`; the
// handler reads only the `[u16 handle]` at each record's offset 0 (resolved via
// the pool table) and rebuilds that entity's minimap blip from its OWN state
// (position, type, team @ entity+354 → icon / team color). The 10 trailing bytes
// per record are NOT consumed by the handler — the blip is recomputed
// engine-side, not taken from the wire — so the decoder keeps them raw.
// [orig: NapiNPClientMsg_0x06B @ 0x425520 → update_minimap_overlay_entity @ 0x5BEC10]
struct MinimapOverlayBatch {
	struct Entry {
		uint16_t handle = 0;        // (pool<<12)|slot of the overlaid entity
		uint8_t  extra[10] = {};    // server-side blip state; NOT read by the handler
	};
	std::vector<Entry> entries;
};
bool decode_minimap_overlay_batch(const uint8_t *body, size_t len,
                                  MinimapOverlayBatch &out);

// S2C 0x49 — weapon-reload notification. `[u16 entityHandle][u16 reloadParam]`
// (4 B). The handler resolves the entity and calls WeaponSlot_ReloadAmmo(entity,
// reloadParam); a vehicle entity instead arms an 80-tick timer. NOTE the IDB
// name `handle_camera_sync_packet_0x049` is WRONG — there is no camera code, it
// reloads ammo. [orig: handle_camera_sync_packet_0x049 @ 0x42C0A0 (IDA-misnamed)
//  → WeaponSlot_ReloadAmmo @ 0x541720]
struct WeaponReload {
	uint16_t entity_handle = 0;
	uint16_t reload_param = 0;   // WeaponSlot_ReloadAmmo arg (reload slot / amount)
};
bool decode_weapon_reload(const uint8_t *body, size_t len,
                          WeaponReload &out, size_t &consumed);

// S2C 0x13 — entity death (the SECOND death path, beside 0x26 kill-sync).
// `[u16 entityHandle][i16 killerSource]` (4 B). The handler sets the entity's
// Health=0, stores killerSource at entity+pad9[36], clears entity+pad8[86], and
// fires its death callback(entity, 4, 0); if the local player died it stamps the
// respawn tick + toggles the weapon scope. Unlike 0x26 (which routes through
// Entity_KillBySlotId), this path acts directly on the entity.
// [orig: NapiNPClientMsg_EntityDeath @ 0x42EB50]
struct EntityDeathRecord {
	uint16_t entity_handle = 0;  // the dying entity
	int16_t  killer_source = 0;  // killer / damage source (i16)
};
bool decode_entity_death(const uint8_t *body, size_t len,
                         EntityDeathRecord &out, size_t &consumed);

// S2C 0x30 — entity-checksum request. `[u8 entityId][u16 checksum]` (3 B). The
// client builds NetPacket_WriteEntityChecksum(entityId, checksum) and replies
// C2S 0x20 (entity checksum). [orig: NapiNPClientMsg_HandleChecksumRequest @ 0x431170]
struct EntityChecksumRequest {
	uint8_t  entity_id = 0;
	uint16_t checksum = 0;
};
bool decode_entity_checksum_request(const uint8_t *body, size_t len,
                                    EntityChecksumRequest &out, size_t &consumed);

// S2C 0x31 — loadout/ammo CRC request. Same 3-byte shape as 0x30 but a different source: the
// client CRCs the indexed 276-byte ammo-definition record (six volatile dwords temporarily
// zeroed), XORs with `xor_key`, and replies C2S 0x21 `[u8 index][u32 crc^key][u32 key]`. An index
// outside the loaded table writes a ZERO crc dword, not `key ^ 0`.
// [orig: NapiNPClientMsg_0x031 @ 0x4311E0 -> NetPacket_WriteEntityCRCChecksum @ 0x42B020
//  (out-of-range arm @0x42B114)]
struct LoadoutCrcRequest {
	uint8_t  ammo_index = 0;
	uint16_t xor_key = 0;
};
bool decode_loadout_crc_request(const uint8_t *body, size_t len,
                                LoadoutCrcRequest &out, size_t &consumed);

// S2C 0x42 — input/state-flags push `[u16 stateFlags]` (2 B) → Input_UnpackStateFlags.
// [orig: NapiNPClientMsg_0x042 @ 0x4281A0]
bool decode_input_state_flags(const uint8_t *body, size_t len,
                              uint16_t &out_flags, size_t &consumed);

// S2C 0x79 — spectator-mode flag `[u8]` (1 B) → dword_82BEE4.
// [orig: NapiNPClientMsg_0x079 @ 0x429B00]
bool decode_spectator_flag(const uint8_t *body, size_t len,
                           uint8_t &out_flag, size_t &consumed);

// S2C 0x2A — chat-history entry `[i32 a][i32 b][i16 c]` (10 B) → Chat_AddToHistory.
// [orig: NapiNPClientMsg_0x02A @ 0x425BA0]
struct ChatHistoryEntry {
	int32_t field_a = 0;
	int32_t field_b = 0;
	int16_t field_c = 0;
};
bool decode_chat_history_entry(const uint8_t *body, size_t len,
                               ChatHistoryEntry &out, size_t &consumed);

// ===========================================================================
// Deployed-item / weapon-overlay spawn (0x59) + entity-routed sub-packet
// (0x44) (§5.36).
// ===========================================================================

// S2C 0x59 — deployed-item / weapon-overlay spawn-or-update. Fixed 32-B record.
// The host streams the placeable / weapon-overlay entities a player drops (mines,
// beacons, satchels, deployed guns…). The handler searches 512 weapon-overlay
// slots for a matching entity and either updates its transform or allocates a
// new pool entry initialised from the item def. `itemId` / `friendlyItemId` /
// `enemyItemId` let one deployable show a different model to friend vs foe
// (selected by the owner's team @ +354 vs the local player); `itemId` is the
// fallback. The handler reads 15 u16s (30 B); the 2 trailing bytes are unread.
// [orig: NapiNPClientMsg_0x059 @ 0x4228E0 → Entity_SpawnOrUpdateFromSlotPacket @ 0x546770]
struct DeployedItemSpawn {
	uint16_t item_id = 0;          // packet[0] — fallback / base item id
	uint16_t owner_handle = 0;     // packet[1] — the placing entity
	uint16_t friendly_item_id = 0; // packet[2] — model shown to the owner's team
	uint16_t enemy_item_id = 0;    // packet[3] — model shown to the other team
	uint16_t slot_handle = 0;      // packet[4] — the spawned entity (pool<<12)|slot
	uint16_t parent_handle = 0xFFFF; // packet[5] — attach parent (0xFFFF = none)
	int32_t  pos_x = 0, pos_y = 0, pos_z = 0;       // i32 16.16 world position
	uint16_t angle_x = 0, angle_y = 0, angle_z = 0; // Euler; the engine shifts << 16
	uint16_t reserved = 0;         // 2 trailing bytes (not read by the handler)
};
bool decode_deployed_item_spawn(const uint8_t *body, size_t len,
                                DeployedItemSpawn &out, size_t &consumed);

// S2C 0x44 — entity-routed sub-packet. A 5-B sub-header `[u16 field0][i16 netId]
// [u8 subtype]` followed by a class-dependent body the dispatcher routes to the
// target entity's per-class serialize callback (entity def+356, source_type=2) —
// the SAME per-class path the C2S 0x0C entity-uplink uses (§5.10b). We decode the
// sub-header + expose the body slice; the body's field layout is class-specific
// (PARTIAL — same deferral as the §5.15 guided record). [orig: NapiNPClientMsg_0x044
//  @ 0x422710 → NetPacket_DispatchToEntityByNetId @ 0x4D6960]
struct EntityRoutedPacket {
	uint16_t field0 = 0;            // [0..1] not read by the dispatcher
	int16_t  net_id = 0;            // [2..3] EntitySlot_FindByNetId key
	uint8_t  subtype = 0;           // [4] selects the def+356 callback path
	const uint8_t *body = nullptr;  // class-dependent body (size = len - 5)
	size_t   body_size = 0;
};
bool decode_entity_routed_packet(const uint8_t *body, size_t len,
                                 EntityRoutedPacket &out);

// ===========================================================================
// S2C 0x45 — terrain-tile load batch (§5.37). The host streams the multiplayer
// terrain-tile array to a JOINING client as phase 5 of the initial-state load
// sequence (Server_SendInitialGameStateToPlayer @ 0x51BBA0 → repeats until the
// serializer returns 0). It is LOAD-ONLY (no gameplay-tick path), PAGED, and one
// of the few messages that carries an actual payload despite the §4 dispatch
// row historically reading "empty payload" (corrected by D-NET-83).
//
// Wire shape, witnessed byte-exact from BOTH the writer and the reader:
//   - First chunk: wire start word == 0xFFFF → a 16-B header follows the 4-B
//     [u16 0xFFFF][u16 end] frame: [u32 'til0' magic][u32 tile_count]
//     [u32 hdr2][u32 hdr3]; tiles [0, end) start at byte 20.
//   - Subsequent chunk: [u16 start][u16 end]; tiles [start, end) start at byte 4.
// Each tile entry is 12 OPAQUE bytes the loader copies verbatim into
// g_TerrainTileData+16+12*idx (the network layer never interprets the 3 dwords —
// the terrain renderer does, later), so we expose them at the engine's own copy
// granularity rather than inventing field names.
// [orig: serialize_terrain_tiles @ 0x6080F0 (writer) / PolyTrn_LoadTileData
//  @ 0x6081D0 (reader) / NapiNPClientMsg_0x045 @ 0x422890 (handler)]
struct TerrainTileEntry {
	uint32_t word0 = 0;  // 12-B opaque tile record (copied raw into g_TerrainTileData)
	uint32_t word1 = 0;
	uint32_t word2 = 0;
};
struct TerrainLoadBatch {
	bool     has_header = false;  // first chunk (wire start word == 0xFFFF)
	uint16_t start_index = 0;     // first tile index this chunk carries (0 for the header chunk)
	uint16_t end_index = 0;       // one past the last tile index this chunk carries
	// Header-chunk only (has_header):
	uint32_t magic = 0;           // 'til0' == 0x74696C30 (reader bails on mismatch)
	uint32_t tile_count = 0;      // total tiles in the full terrain set (drives the alloc)
	uint32_t header_field2 = 0;
	uint32_t header_field3 = 0;
	std::vector<TerrainTileEntry> tiles;  // end_index - start_index entries
};
// Returns true iff the body was consumed exactly (header + N×12-B entries).
bool decode_terrain_load_batch(const uint8_t *body, size_t len, TerrainLoadBatch &out);

// ===========================================================================
// Session/HUD state channel (§5.48–§5.55) — the 2026-07-01 coverage-sweep wave.
// ===========================================================================

// §5.48 S2C 0x58 — SESSION-STATUS block (server name, mission name, session
// up-time sync, and the end-game scoring-rule table). The client parses it into
// the single global g_session_status (0x24E3E88): server name feeds the
// STROVER_SERVERNAME end-game line, uptime_ms is elapsed-at-send (the client
// stamps GetTickCount at parse so elapsed = wire − parseTick + now,
// CSessionTimer_GetElapsedMS), and the 39 i32s are the per-stat point values
// STROVER_STATVAR00..38 the end-game stats screen awards (positive) or
// penalizes (negative). Historic "texture loader (terrain assets)" note was
// wrong. [orig: NapiNPClientMsg_SessionStatus @ 0x4228C0 →
// SessionStatus_ParseFromBuffer @ 0x530ED0; readers Overlay_BuildEndGameStatsText
// @ 0x54A240, SessionStatus_GetStatPointValue @ 0x52D5D0]
struct SessionStatusKV {
	uint8_t  key = 0;    // key <= 9 kept (first 8 pairs stored)
	uint32_t value = 0;
};
struct SessionStatusBlock {
	std::string server_name;   // cstr; client keeps <= 31 chars
	std::string mission_name;  // cstr; client keeps <= 63 chars
	uint8_t  byte0 = 0, byte1 = 0, byte2 = 0; // → g_session_status[25..27]
	uint32_t uptime_ms = 0;    // session elapsed ms at send time
	int32_t  stat_values[39] = {}; // STROVER_STATVAR00..38 point table
	uint8_t  kv_count = 0;     // wire count; MAY exceed the pairs present (reader
	                           // is bounds-tolerant, missing pairs read as zeros)
	std::vector<SessionStatusKV> kv; // the pairs actually on the wire
	size_t   trailing_bytes = 0; // bytes after the kv pairs the retail parser
	                             // never reads (golden carries 5 zero bytes)
};
bool decode_session_status(const uint8_t *body, size_t len, SessionStatusBlock &out);

// §5.49 S2C 0x6F — ZONE-TIMER VALUE update (15 B). Programs the per-zone-entity
// timer entry the capture/takeover HUD reads: value/limit are SECONDS on the
// wire, scaled ×62 into 62 Hz ticks by the client; rate is the per-tick
// increment (the client advances value += rate each frame, clamped at limit).
// Also tracks the nearest zone entity to the local player for the takeover
// widget. NOT a cinematic-camera message (historic label was wrong).
// [orig: NapiNPClientMsg_ZoneTimerValue @ 0x428D60 → ZoneTimerList_SetEntryValue
//  @ 0x537EC0; per-frame advance Client_ProcessNetworkFrame @ 0x42C2E6;
//  consumer HUD_DrawTakeoverStatus @ 0x59B630]
struct ZoneTimerValue {
	uint16_t zone_handle = 0;  // (pool<<12)|slot of the zone entity
	uint8_t  mode = 0;
	int32_t  value_s = 0;      // current value, 16.16 fixed seconds (client ×62 → tick-fixed)
	int32_t  limit_s = 0;      // clamp limit, 16.16 fixed seconds (golden: 1.0 for owned zones)
	int16_t  rate = 0;         // per-tick accumulator increment
	uint8_t  byte544 = 0;      // → zone entity+544
	uint8_t  byte545 = 0;      // → zone entity+545
};
bool decode_zone_timer_value(const uint8_t *body, size_t len,
                             ZoneTimerValue &out, size_t &consumed);

// §5.49 S2C 0x53 — ZONE-TIMER WINDOW update (9 B). The companion channel of the
// same zone-timer entry: a [start, end) window in SECONDS (client ×62 → ticks)
// plus a rate byte; mode_b lands at zone entity+547. The tracked-nearest-zone
// adoption additionally requires the zone within 20.0 world units (1310720 in
// 16.16). [orig: NapiNPClientMsg_ZoneTimerWindow @ 0x428AE0 →
//  ZoneTimerList_SetEntryWindow @ 0x537DE0]
struct ZoneTimerWindow {
	uint16_t zone_handle = 0;
	uint8_t  mode_a = 0;
	uint8_t  mode_b = 0;       // → zone entity+547
	uint16_t start_s = 0;      // window start, seconds
	uint16_t end_s = 0;        // window end, seconds
	uint8_t  rate = 0;
};
bool decode_zone_timer_window(const uint8_t *body, size_t len,
                              ZoneTimerWindow &out, size_t &consumed);

// §5.50 S2C 0x34 — PLAY-SOUND by sound-profile name. flag 0 → flat/ambient
// play; flag 1 → positioned 3D one-shot at full volume (the 3 i16 coords are
// shifted << 16 into 16.16 world space). No position block on the wire when
// flag != 1. Gated is_mp_session_peer. The IDB name "GotoTeleport" was a
// misnomer. [orig: NapiNPClientMsg_PlaySoundByName @ 0x4283A0 →
//  SoundProfile_FindLoadedByName @ 0x5274F0 / Entity_PlaySound3D_FullVolume @ 0x528E20]
struct PlaySoundCommand {
	uint8_t     flag = 0;      // 0 = flat play, 1 = positioned 3D
	std::string sound_name;    // sound-profile name (cstr)
	bool        has_pos = false; // true iff flag == 1 (position block present)
	int16_t     pos_x = 0, pos_y = 0, pos_z = 0; // world units (engine shifts << 16)
};
bool decode_play_sound(const uint8_t *body, size_t len, PlaySoundCommand &out);

// §5.51 S2C 0x2C — SESSION + MISSION-FILE NAME assign: [cstr sessionName]
// [cstr bmsFileName] → byte_A82378 / g_map_file_name; bumps g_loading_progress
// to >= 1. A join-burst member; the historic "chat entry" table note was wrong
// (chat-history is 0x2A). Golden values: "Untitled" (the host's session name,
// matching the 0x58 server name) + "TDH_I5A.BMS".
// [orig: NapiNPClientMsg_MissionMapNames @ 0x427E10]
struct MissionMapNames {
	std::string session_name;   // → byte_A82378 (host session/server name)
	std::string map_file_name;  // → g_map_file_name (0x24D1F3E), the .BMS file
};
bool decode_mission_map_names(const uint8_t *body, size_t len, MissionMapNames &out);

// §5.52 chat text channel. C2S 0x0D uplink: [u8 channel][cstr text] — the
// server strips <...> tags, rate-limits 1000 ms/player, prepends name(/squad),
// then fans the formatted line out as S2C 0x14 [u8][u8][cstr] per recipient
// (channel routing: 2=team, 4/5=per-side, 11/12=squad/commander, 13=proximity
// <= 100.0 world units, default=all). The historic C2S 0x0D "replication frame
// ACK" note was wrong. [orig: uplink NapiNPServer_HandleChatMessage @ 0x513760;
//  downlink NapiNPClientMsg_ChatMessage @ 0x42F240 → Chat_DispatchToChannel @ 0x42B910]
struct ChatUplink {
	uint8_t     channel = 0;
	std::string text;
};
bool decode_chat_uplink(const uint8_t *body, size_t len, ChatUplink &out);
struct ChatBroadcast {
	uint8_t     sender_slot = 0; // one of the two header bytes (see §5.52 note)
	uint8_t     channel = 0;     // the other header byte
	std::string text;            // formatted "name(/squad): text" line
};
bool decode_chat_broadcast(const uint8_t *body, size_t len, ChatBroadcast &out);

// §5.53 S2C 0x04 — SESSION SLOT CONFIG (24 B): four leading i32s the handler
// skips, then [u8 sessionConfig][u8 teamMode][u8 maxPlayers] (maxPlayers drives
// PlayerSlotTable_Reallocate), one more skipped i32, and a trailing byte.
// [orig: NapiNPClientMsg_SessionSlotConfig @ 0x425410]
struct SessionSlotConfig {
	uint32_t skipped[4] = {};   // on the wire, not read by the handler
	uint8_t  session_config = 0; // → dword_24D2110
	uint8_t  local_player_slot = 0; // → g_local_player_slot_id = g_local_player_slot_id, the recipient's
	                                // OWN roster slot [orig: the write side is slot+20,
	                                // NetPacket_WriteSlotAssignment @0x502b30; the old
	                                // `team_mode` reading was a misnomer, witness 2026-07-03]
	uint8_t  max_players = 0;    // → g_max_player_slots = g_max_player_slots + PlayerSlotTable_Reallocate;
	                             // the 0x46/0x22 roster walk terminates at this count
	uint32_t skipped4 = 0;       // on the wire, not read
	uint8_t  trailing = 0;       // → byte_A85B48
};
bool decode_session_slot_config(const uint8_t *body, size_t len, SessionSlotConfig &out);

// §5.54 S2C 0x08 — SESSION CONFIG (fixed 51 B; the historic "game-state
// snapshot ~2 KB" note was wrong): [10 × i32][7 × u8][u32 bitflags]. fields[3]
// = gameType (→ g_GameType); bitflags bits 13/15/16 are latched into
// byte_A821EE/EF/F0. Bumps g_loading_progress to >= 1.
// [orig: NapiNPClientMsg_HandleSessionConfig @ 0x4281D0]
struct SessionConfig {
	int32_t  fields[10] = {};  // → dword_A821BC..A821E0; fields[3] = gameType
	uint8_t  bytes[7] = {};    // → byte_A821E8..ED + dword_24D2110
	uint32_t bitflags = 0;     // → dword_A821E4 (bits 13/15/16 latched)
};
bool decode_session_config(const uint8_t *body, size_t len, SessionConfig &out);

// §5.55 S2C 0x02 — JOIN POSITION-ACK + PADDING PROBE. The handler reads only
// [i32 posX][i32 posY][i32 paddingLen]; the rest of the (typically ~512 B) body
// is ignored filler. The client replies C2S 0x02 = position + paddingLen random
// bytes (NetPacket_WritePositionWithPadding) and resets its send holdoff.
// The decoder consumes the filler explicitly (filler_bytes = len - 12).
// [orig: NapiNPClientMsg_HandleJoinResponse @ 0x42E0F0]
struct JoinPaddingProbe {
	int32_t  pos_x = 0;
	int32_t  pos_y = 0;
	uint32_t padding_len = 0;  // random-filler byte count the client must echo
	size_t   filler_bytes = 0; // trailing wire bytes after the 12-B header
};
bool decode_join_padding_probe(const uint8_t *body, size_t len, JoinPaddingProbe &out);

// §5.56 C2S 0x2F — LOADOUT SUBMIT (spawn-menu accept). The client uploads its
// server-assigned team, chosen character class, current weapon slot, and the
// ADM weapon-slot picks of its per-side profile kit; the server validates the
// envelope FIRST (team 1..4 — above 4 only in a team-less game type @0x5158a9;
// a NONZERO class must be 5..9 @0x5158b1, else the handler aborts @0x515fa5 and
// only re-sends the player's current slot list), writes the class to entity+660
// (playerClass — class 0 applies with an empty soldier-type mask @0x5159af),
// rebuilds the avatar display list + weapon slots, and replies with the S2C 0x5A
// weapon-slot list. The in-range class is remapped through the host class-allow
// mask g_hostClassAllowMask @0x24D59FC (@0x5158e6) before the [5,9]-else-8 tail
// @0x515913. Entries repeat until an 0xFF adm_index terminator — the same
// {typeId, ammoP, ammoS, variant} slot vocabulary as the §5.30 S2C 0x5A downlink.
// [orig: builder NetPacket_SendLoadoutSubmit @ 0x42cdc0 (team = byte_A85B48,
//  the S2C 0x04 tail byte; class = the profile's per-side class byte; slot =
//  195 pre-Player_InitPlayer, g_currentWeaponSlot after);
//  decoder NapiNPServerMsg_HandlePlayerLoadout @ 0x515790]
struct LoadoutSubmitEntry {
	uint8_t adm_index = 0;      // AdmDef index (0xFF = list terminator, not stored)
	uint8_t ammo_primary = 0;   // clamped to admEntry[83], scaled by admEntry[22]
	uint8_t ammo_secondary = 0; // sub-entry ammo
	uint8_t variant = 0;        // → player+89688+admEntry[1]
};
struct LoadoutSubmit {
	uint8_t  team = 0;              // wire byte 0; read SIGNED, accepted 1..4 [orig: byte_A85B48]
	uint8_t  player_class = 0;      // wire byte 1; 0 or 5..9 → entity+660 playerClass
	uint32_t weapon_slot_index = 0; // selected weapon slot (entity+280 binding)
	std::vector<LoadoutSubmitEntry> entries;
	bool     terminated = false;    // saw the 0xFF terminator
};
bool decode_loadout_submit(const uint8_t *body, size_t len, LoadoutSubmit &out);

} // namespace opennova
