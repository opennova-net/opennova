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
//   - tests/novaworld (the inline-pcap and fixture replays)
//   - apps/nw_pp                                   (pretty-printer)
//   - engine/runtime/inmatch + engine/runtime/replication                 (the in-match runtime's fold paths)
//   - any future replay tool                       (re-emit captured C2S)
//
// Convention: every conditional field is left default-constructed when its
// `spawn_flags` / `flags_byte` gate is clear — callers must mask the flags
// to know which fields are valid. This matches how the retail handlers leave
// the entity slot's matching offsets unwritten.
//
// [orig: NapiNPClientMsg_0x00D @ 0x432C40]  — pool-entity spawn batch.
// [orig: NapiNPClientMsg_0x020 @ 0x425C00]  — bulk pool-3 entity sync.

#include <net/npwire/entity_class.h>
#include "ingame_decode_session.h" // the session/HUD state channel records

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

// S2C 0x0D spawn_flags gate bits (§5.11 field map). Names follow the witnessed
// wire-field names; the struct fields alert_byte/action_byte below keep their
// older decode-era spellings of the same two fields (refNum/subType, D-NET-94).
inline constexpr uint16_t kPoolSpawnHasEulerZ         = 0x0001; // entity+16 yaw heading
inline constexpr uint16_t kPoolSpawnHasEulerX         = 0x0002; // entity+20
inline constexpr uint16_t kPoolSpawnHasEulerY         = 0x0004; // entity+24
inline constexpr uint16_t kPoolSpawnHasSectionMask    = 0x0008; // entity+308
inline constexpr uint16_t kPoolSpawnHasTeamByte       = 0x0010; // entity+354 (D-NET-58)
inline constexpr uint16_t kPoolSpawnHasEntityFlags    = 0x0020; // entity+36
inline constexpr uint16_t kPoolSpawnHasRefNum         = 0x0040; // entity+533 (D-NET-94)
inline constexpr uint16_t kPoolSpawnHasSubType        = 0x0080; // entity+532
inline constexpr uint16_t kPoolSpawnHasParentHandle   = 0x0100; // entity+368 occupant back-ref (D-NET-195)
inline constexpr uint16_t kPoolSpawnHasTargetHandle   = 0x0200; // entity+40 structural carrier
inline constexpr uint16_t kPoolSpawnHasMountOccupancy = 0x0400; // seat mask + occupant handles
inline constexpr uint16_t kPoolSpawnHasAiTrailer      = 0x0800; // aiSlot+16/+20/+156
inline constexpr uint16_t kPoolSpawnHasSoundLatchByte = 0x1000; // brain+0x318 -> entity+0xB0
inline constexpr uint16_t kPoolSpawnHasZoneNumberRank = 0x2000; // entity+538 + radius entity+350
inline constexpr uint16_t kPoolSpawnHasDifficultyByte = 0x4000; // entity+624
inline constexpr uint16_t kPoolSpawnHasZoneRadiusAlt  = 0x8000; // entity+350 (SpawnPoint path @0x503f29)

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

	// Always-present unconditional byte after the mount-occupancy block. Landing
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

	// Mount-occupancy block (gated by `spawn_flags & 0x0400`):
	//   u8 seat_mask, then 1 × u16 per set bit for retail slots 0..7
	//   (0xFFFF is an empty occupant), followed unconditionally by the
	//   occupant handles for retail slots 8 and 9.
	// `mount_handles[slot]` is valid when that slot's mask bit is set;
	// `mount_handle_8/9` are valid whenever (spawn_flags & 0x0400).
	uint8_t seat_mask = 0;
	std::array<uint16_t, 8> mount_handles{
			{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};
	uint16_t mount_handle_8 = 0xFFFF;
	uint16_t mount_handle_9 = 0xFFFF;

	// AI trailer (gated by `spawn_flags & 0x0800`). D-NET-52 confirms each
	// of the two pre-cstring fields is a wire u32 (handler advances cursor
	// by 4 bytes per read). The serializer gates it on the AIData def's AI
	// slot pointer (entity+0x68), never on the values, so presence rides its
	// own field. [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503D3D..0x503D5C]
	bool has_ai_trailer = false;     // 0x0800
	uint32_t ai_profile_1 = 0;       // 0x0800   aiSlot+16
	uint32_t ai_profile_2 = 0;       // 0x0800   aiSlot+20
	std::string ai_name;             // 0x0800   aiSlot+156 (NUL-terminated)

	uint8_t alert_byte = 0;          // 0x0040   entity+533
	uint8_t action_byte = 0;         // 0x0080   entity+532
	// The vehicle brain's +0x318 sound-latch byte (the movers' engine, reverse,
	// lights/flare, skid, collision, pivot and tumble cue bits; 0x80 also while
	// a helo profile's rotor spins), gated on the brain pointer (entity+0x64)
	// whatever its value: every retail vehicle carries it, 0x00 when parked. The
	// client stores it sign-extended into entity+0xB0.
	// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503E7F..0x503EC0;
	//  NapiNPClientMsg_0x00D `movsx ecx,cl; mov [ebx+0B0h],ecx` @0x4331B7..0x4331BA]
	bool has_sound_latch_byte = false; // 0x1000
	uint8_t sound_latch_byte = 0;    // 0x1000   brain+0x318 -> entity+0xB0

	// Zone block (the old "health" reading was a decode-era misnomer — these are
	// zone-object fields, witness 2026-07-03): `0x2000` reads (u8 zone_number_rank =
	// zoneNumber + 32*rank → entity+538 [orig: ZoneSlotChain_GetZoneInfo @0x503eeb],
	// u16 zone_radius → entity+350); a def-attrib-0x40000 SpawnPoint without a zone
	// number instead gates `0x8000` = u16 zone_radius alone [orig: @0x503f29]. Golden
	// ASH_I5A bunkers: 0x22/0x0046 = zone 2 rank 1, radius 70. Neither gate reads the
	// written values: 0x2000 rides the zone number byte (entity+538 != 0), whatever the
	// packed info byte, and 0x8000 the def's SpawnPoint attrib, whatever the radius, so
	// both ride presence fields. The client reads 0x8000 only without 0x2000.
	// [orig: NetPacket_SerializeEntityPoolToPacket_0 `cmp byte [ebp+21Ah],0` @0x503ECC..0x503ED3,
	//  `test dword [def+54h],40000h` @0x503F1F..0x503F29; NapiNPClientMsg_0x00D
	//  @0x4331C6..0x433206]
	bool has_zone_number_rank = false; // 0x2000
	bool has_zone_radius_alt = false;  // 0x8000 (only without 0x2000)
	uint8_t  zone_number_rank = 0;  // 0x2000   entity+538 (+ the chain rank in bits 5-7)
	uint16_t zone_radius = 0;       // 0x2000 OR 0x8000   entity+350

	// The 0x4000 byte is gated on the def's callbacks (the weapon-overlay damage
	// callback or the physics-step mover), not on its value.
	// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503F4C..0x503F80]
	bool has_difficulty_byte = false;  // 0x4000
	uint8_t  difficulty_byte = 0;    // 0x4000   entity+624
};

struct PoolSpawnBatch {
	std::vector<PoolSpawnRecord> records;
	// Set when a decode failure left the LAST records entry half-read (the
	// walk pushes the failing record for diagnostics). A false return with
	// this clear means every stored record is complete (sentinel/tail
	// mismatch only) — the retail handler applies records as it walks, so
	// consumers apply that complete prefix.
	bool last_record_partial = false;
	// True when the loop terminated early via the slot-id sentinel
	// (0xFFFF or (slot & 0xF000) >= 0x5000) before `entity_count` records
	// were read — the retail handler returns immediately on this case.
	bool sentinel_ended_early = false;
	// Header u16 entity_count, retained for re-encode parity.
	int16_t entity_count = 0;
};

// S2C 0x20 pool-3 flags_byte gate bits (§5.12 field map).
inline constexpr uint8_t kPool3SyncHasMovementVal    = 0x01; // entitySlot+16 BAM heading (D-NET-59)
inline constexpr uint8_t kPool3SyncHasOrientationVal = 0x02; // entitySlot+0
inline constexpr uint8_t kPool3SyncHasAmmoCount      = 0x04; // entitySlot+290
inline constexpr uint8_t kPool3SyncHasTeamByte       = 0x08; // entitySlot+354
inline constexpr uint8_t kPool3SyncHasWeaponType     = 0x10; // entitySlot+640
inline constexpr uint8_t kPool3SyncHasScoreByte      = 0x20; // entitySlot+672

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
	// Set when a decode failure left the LAST records entry half-read (the
	// walk pushes the failing record for diagnostics). A false return with
	// this clear means every stored record is complete (sentinel/tail
	// mismatch only) — the retail handler applies records as it walks, so
	// consumers apply that complete prefix.
	bool last_record_partial = false;
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
//
// S2C 0x10 field_flags gate bits (§5.9 field map). Same values as the 0x0D set
// but a DIFFERENT family — 0x0040/0x0080/0x0100/0x0200 gate different fields
// here. attach_ref presence is `weapon_byte != 0 || (flags & kStaticEntityHasAttachRef)`.
inline constexpr uint16_t kStaticEntityHasEulerZ      = 0x0001; // entity+16 yaw heading
inline constexpr uint16_t kStaticEntityHasEulerX      = 0x0002; // entity+20
inline constexpr uint16_t kStaticEntityHasEulerY      = 0x0004; // entity+24
inline constexpr uint16_t kStaticEntityHasSectionMask = 0x0008; // entity+308
inline constexpr uint16_t kStaticEntityHasTeamByte    = 0x0010; // entity+354 (D-NET-58/62)
inline constexpr uint16_t kStaticEntityHasEntityFlags = 0x0020; // entity+36 Flags dword (D-NET-147)
inline constexpr uint16_t kStaticEntityHasRefNum      = 0x0040; // entity+533 (D-NET-94; struct field bone_a)
inline constexpr uint16_t kStaticEntityHasSubType     = 0x0080; // entity+532 (struct field bone_b)
inline constexpr uint16_t kStaticEntityHasScoreFlag   = 0x0100; // entity+624
inline constexpr uint16_t kStaticEntityHasAttachRef   = 0x0200; // entity+350 (with the weapon_byte OR-gate)

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
	// A door def carries the section word whenever its door count is nonzero,
	// even a zero word; any other def only a nonzero one. [orig:
	// NetPacket_SerializePool2StaticToBuffer @0x5044A2..0x5044B3]
	bool     has_section_mask = false;
	uint8_t  team_byte = 0;      // 0x10    entity+354 (BMS team 1=Blue/2=Red)
	// entity+36 = the entity FLAGS dword, streamed raw (was misread as "parentSlot" — the
	// D-NET-147 grill witnessed the serializer source @0x50435f: BMS Indestructible/Reflective/
	// NoShadow attributes + Building/indestructible def bits; golden buildings carry 0x04020400).
	uint32_t entity_flags = 0;   // 0x20    entity+36 [orig: NetPacket_SerializePool2StaticToBuffer @0x5044e6]
	uint8_t  ammo_count = 0;     // always  entity+290 (BMS record byte 81)
	uint8_t  bone_a = 0;         // 0x40    entity+533 refNum (BMS byte 153; D-NET-94)
	uint8_t  bone_b = 0;         // 0x80    entity+532 subType (0xFF on indestructible defs)
	// entity+624 (0x270), gated on the def's callbacks (a palm damage callback or
	// a psec mover), not its value. [orig: NetPacket_SerializePool2StaticToBuffer
	// @0x504554..0x504588]
	bool     has_score_flag = false; // 0x100
	uint8_t  score_flag = 0;     // 0x100   entity+624
	uint8_t  weapon_byte = 0;    // always  entity+538
	uint16_t attach_ref = 0;     // weapon_byte != 0 || flags & 0x200; entity+350
};

struct StaticEntityBatch {
	uint16_t start_index = 0;
	int16_t  entity_count = 0;
	std::vector<StaticEntityRecord> records;
	// Set when a decode failure left the LAST records entry half-read (the
	// walk pushes the failing record for diagnostics). A false return with
	// this clear means every stored record is complete (sentinel/tail
	// mismatch only) — the retail handler applies records as it walks, so
	// consumers apply that complete prefix.
	bool last_record_partial = false;
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
	uint32_t owner_connection_id = 0; // entity+120 (0x78); authenticated connection dcb
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
	uint8_t  player_slot_id = 0; // entity+340 (0x154): a player's own roster slot id
	uint8_t  alert_level = 0;    // entity+533 (0x215)
	uint8_t  sub_type = 0;       // entity+532 (0x214)
	uint8_t  weapon_type = 0;    // entity+343 (0x157)
	uint8_t  parent_slot = 0;    // entity+360 (0x168)
	uint16_t parent_handle = 0xFFFF; // (pool<<12)|slot, resolved → entity+364 (0x16C)
};

struct OrganicSpawnBatch {
	uint16_t entity_count = 0;   // header u16 (no start-index, unlike 0x10/0x20)
	std::vector<OrganicSpawnRecord> records;
	// Set when a decode failure left the LAST records entry half-read (the
	// walk pushes the failing record for diagnostics). A false return with
	// this clear means every stored record is complete (sentinel/tail
	// mismatch only) — the retail handler applies records as it walks, so
	// consumers apply that complete prefix.
	bool last_record_partial = false;
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
// @0x514180 → NetPacket_SerializeObjectToBuffer @0x504d10; client
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
	uint8_t  player_slot_id = 0;   // entity+340 (0x154): a player's own roster slot id
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
	// NOT a ping (the decode-era name, refuted 2026-07-28): a STATUS BITFIELD.
	// It lands at scoreboard record+0x36 [orig: @0x42fdb4] and its only reader
	// bit-tests it to append the row's glyph suffix [orig: the read
	// movzx ecx,[ebx+6] @0x423f1f and the thirteen bit tests
	// @0x423f23-0x4240e0]; both ends zero it behind a capability gate
	// (the server per-recipient @0x504bd6; the client when
	// g_ScoreboardStatusSuffixEnabled — the 'SU' text command @0x429f71 —
	// is clear [orig: @0x42fbfb]) — which a latency number never would be.
	uint16_t status_flags = 0;
	// The MODE's primary stat, filled by the server's game-type switch
	// [orig: ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52C850 — DM/TDM stats[5] enemy kills; A&S/TacOps
	// stats[39] zone takeovers; KOTH a seconds value the client renders mm:ss
	// under the timed-scores flag]. The ONLY row score the Tab list draws
	// [orig: the sole score read @0x423E76 in HUD_DrawKillList @0x423A30].
	uint16_t score1 = 0;
	// stats[29], the accumulated point/EXP total — NOT deaths (the decode-era
	// guess, refuted 2026-08-06; deaths is stats[7] and is absent from 0x16)
	// [orig: encoder @0x50D960 via CPlayerStats_GetFieldPlusOne (ex CRenderState_GetFieldByIndex)(stats, 0x1C);
	// sole writer CPlayerStats_RecordEvent case 28 @0x52C8E0]. The retail
	// CLIENT never reads it; the server sorts team-mode rows by it
	// [orig: Player_ComputeScore @0x500A80].
	uint16_t score2 = 0;
	uint8_t  flags = 0;       // bit0 = SPECTATOR (subtracted from the HUD count), team = flags >> 1
	                          // [orig: NapiNPClientMsg_PlayerList @0x42FAE0 row apply]
};
// One team-table row. The u16 pair carries the SAME two stats as the player
// rows — the mode stat and the accumulated points [orig: the team stores
// @0x50dcb8/@0x50dce4 in Server_BuildAndBroadcastScoreboard]. The byte pair
// is mode-specific: the KOTH hold byte (game type 0x10001 [orig: @0x50dc62])
// and the CTF flag state (types 0x10002/0x90002/0x10004 [orig: @0x50dd30]).
// The old player_count/alive_count names were decode-era guesses (refuted
// 2026-08-19 against the serializer's fills).
struct PlayerListTeamRow {
	uint16_t score1 = 0;
	uint16_t score2 = 0;
	uint8_t  koth_hold = 0;
	uint8_t  ctf_flag = 0;
};
struct PlayerList {
	uint8_t  flags = 0;        // byte 0 -> g_ScoreboardFlags: bit0 team-mode, bit1 timed-scores
	                           // (the old `max_players` reading was a misnomer, witness 2026-07-03)
	uint8_t  player_count = 0; // row count -> g_ScoreboardRowCount (HUD count minuend, D-NET-158)
	std::vector<PlayerListRow> players; // rows accepted ONLY for 0x46-known slots; a row for an
	                                    // unknown slot is dropped + retried via C2S 0x22 [slot, 0x1CF7]
	uint8_t  team_count = 0;
	std::vector<PlayerListTeamRow> teams;  // team_count + 1 rows (T0 neutral + per team)
	uint8_t  in_game_count = 0;   // trailer -> g_ScoreboardInGameCount (g_ScoreboardInGameCount)
	uint8_t  spectator_count = 0; // trailer -> g_ScoreboardSpectatorCount (g_ScoreboardSpectatorCount);
	                              // HUD "Number of players" = accepted rows − this
};
// The row loop runs min(player_count, 252) rows — the retail parser's 252-row
// table clamp [orig: @0x42fb3a..0x42fb3c]; `player_count` keeps the wire byte.
bool decode_player_list(const uint8_t *body, size_t len, PlayerList &out);

// S2C 0x46 player-sync field bits. Names follow the SERIALIZER's field
// semantics [orig: NetPacket_SerializePlayerSync0x46 @0x505e80 write sites
// @0x505f9b..0x506230]; the §5.21 client read labels the same fields
// generically ("id" for 0x0010, "entityRef" for 0x0800) — both ends noted at
// the struct fields below.
inline constexpr uint16_t kPlayerSyncHasName         = 0x0001; // cstr
inline constexpr uint16_t kPlayerSyncHasTeamString   = 0x0002; // cstr; retail always "" (@0x505ff7)
inline constexpr uint16_t kPlayerSyncHasTeamByte     = 0x0004;
inline constexpr uint16_t kPlayerSyncHasDownedState  = 0x0008; // revive seconds low7 | medic-request bit7
// cstr: the slot's player's NovaWorld PCID (net config +0x184; "" on LAN)
// [orig: @0x506046..0x5060ac]. The old "vehicle name" label misread slot+28,
// the slot's NapiNPPlayer.
inline constexpr uint16_t kPlayerSyncHasPcid         = 0x0010;
// u8: NapiNPPlayer+0x9C inside a NovaWorld session while dword_24D21A4 is -1,
// else 0; the only writers found zero it (NapiNPPlayer_Create @0x4c7a01,
// CNetPlayer_ResetSendState @0x500b5e). [orig: @0x50613b..0x50618b]
inline constexpr uint16_t kPlayerSyncHasVehicleScore = 0x0020;
inline constexpr uint16_t kPlayerSyncHasSquad        = 0x0040;
inline constexpr uint16_t kPlayerSyncHasSide         = 0x0080;
inline constexpr uint16_t kPlayerSyncHasQuality      = 0x0400;
// u32 NovaWorld squad id: the serializer reads the slot's player's net config
// squad_id (+0x1A4, the SQUADINFO cookie's leading dword; 0 on LAN)
// @0x506257/@0x506246; the client stores it in slot dword 15 and keys
// PlayerSlot_SetName on it (@0x431736 -> @0x43173d) to pull the clan-roster TAG
// into the Tab row. The old "vehicle timer" / §5.21 "entityRef" labels were
// misreads of that offset.
inline constexpr uint16_t kPlayerSyncHasAccountId    = 0x0800;
inline constexpr uint16_t kPlayerSyncHasLateJoinFlag = 0x1000;
inline constexpr uint16_t kPlayerSyncAck             = 0x4000; // roster-walk ack; no body
inline constexpr uint16_t kPlayerSyncRemoval         = 0x8000; // removal; no body fields

// The join-broadcast field set [orig: Server_PlayerAdd @0x51d2bf `push 7415`]
// and the client's roster-walk request mask (the same set + the ack bit).
inline constexpr uint16_t kPlayerSyncJoinBroadcastFields =
		kPlayerSyncHasName | kPlayerSyncHasTeamString | kPlayerSyncHasTeamByte |
		kPlayerSyncHasPcid | kPlayerSyncHasVehicleScore |
		kPlayerSyncHasSquad | kPlayerSyncHasSide | kPlayerSyncHasQuality |
		kPlayerSyncHasAccountId | kPlayerSyncHasLateJoinFlag;
static_assert(kPlayerSyncJoinBroadcastFields == 0x1CF7,
              "the witnessed Server_PlayerAdd broadcast mask");
inline constexpr uint16_t kPlayerSyncRosterWalkFields =
		kPlayerSyncJoinBroadcastFields | kPlayerSyncAck;
static_assert(kPlayerSyncRosterWalkFields == 0x5CF7,
              "the witnessed client roster-walk request mask");

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
	std::string id_label;            // 0x0010 the player's NovaWorld PCID
	uint8_t  team = 0;               // 0x0004
	uint8_t  downed_state = 0;       // 0x0008 (revive seconds = v & 0x7F; medic request = v >> 7)
	uint8_t  field_0020 = 0;         // 0x0020
	uint8_t  field_1000 = 0;         // 0x1000
	uint8_t  field_0040 = 0;         // 0x0040
	uint8_t  field_0080 = 0;         // 0x0080
	uint8_t  quality = 0;            // 0x0400 (clamp 4)
	uint32_t account_id = 0;         // 0x0800 NovaWorld squad id -> slot dword 15, the clan-roster key (0 on LAN)
	bool     queue_ack = false;      // 0x4000 — no body byte; client queues a C2S 0x22 ack
};
bool decode_player_sync(const uint8_t *body, size_t len, PlayerSync &out);

// §5.19 minimap-overlay entry vocabulary: the icon-color byte indexes
// g_MinimapOverlayColorTable @ 0x840A10 (LE 0xAARRGGBB dwords), and the
// flags byte carries the two witnessed marker bits.
inline constexpr uint8_t kZoneIconNeutral = 0x0C; // 0xFF208020 green — neutral (BMS team 0)
inline constexpr uint8_t kZoneIconRed     = 0x09; // 0xFF802020 — Red (BMS team 2)
inline constexpr uint8_t kZoneIconBlue    = 0x0A; // 0xFF304080 — Blue (BMS team 1)
inline constexpr uint8_t kZoneOverlayFlagPersistent = 0x10; // persistent capture-zone marker
inline constexpr uint8_t kZoneOverlayFlagClearSlot  = 0x20; // clear slot (handle -> 0xFFFF, lifetime 0)

// One entry from a S2C 0x40 minimap-overlay update / capture-zone state batch
// (§5.19). 6 bytes per entry, prefixed by a u8 count. Overlay position is read
// from the resolved pool entity, not the wire — this packet carries no coords.
// [orig: NapiNPClientMsg_0x040 @ 0x425A50 → MapOverlay_DecodeOverlayEntries @ 0x5BEBB0 (6-byte walker)
//  → MapOverlay_UpdateOrCreateSlot @ 0x5BEA60; color table g_MinimapOverlayColorTable @ 0x840A10]
struct CaptureZoneOverlay {
	uint16_t handle = 0;       // +0  (pool<<12)|slot, resolved via g_PoolList
	uint8_t  param = 0;        // +2  → slot+2
	uint8_t  icon_color = 0;   // +3  index into g_MinimapOverlayColorTable: 0x0c neutral / 0x09 Red / 0x0a Blue (BMS team 0/2/1)
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

// S2C 0x7E mission/server briefing strings: exactly two NUL-terminated raw
// cp1252 strings. Presentation interprets the embedded retail markup; the wire
// layer preserves it opaquely. [orig: NapiNPClientMsg_ServerConfigStrings @0x425E20;
// NetPacket_WriteBriefingText @0x506620]
struct ServerConfigStrings {
	std::string briefing3;
	std::string briefing2; // [info]/briefing2, with [info]/briefing fallback resolved by host
};
bool decode_server_config_strings(const uint8_t *body, size_t len,
                                  ServerConfigStrings &out);

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
	                                  // flags table g_AnimStateFlagsTable; transition-arbitrated, remote-only
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

// The §5.13 form selector: flags_byte bit 0x04 picks the 15-B dead/wreck
// pose-only form over the 21-B live form (the death family sets Flags |= 6).
inline constexpr uint8_t kVehicleCompactFlagDeadPose = 0x04;

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

	// Live branch (is_dead_pose = false) — vertical velocity + vehicle health + the
	// prediction registers:
	uint16_t vertical_velocity = 0;       // entity+0xA0 slideDecay (compressed 16.16 u/tick): the
	                                      // vehicle's vertical velocity, re-landed at entity+0xA0
	                                      // by the read when !(stateFlags & 2) [orig: write
	                                      // @0x460d5a..0x460d7b; read @0x460910..0x46091e]. The
	                                      // old `weapon_x` name was a D-NET-63-era misnomer.
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

// §5.9.1 round-event `flags` bits (the fire-mode byte, ring+30). Bits 4-5 are
// the pre-consume magazine-count low two bits — a 2-bit lane, deliberately not
// named as single masks.
inline constexpr uint8_t kRoundEventFlagAltFire         = 0x01; // bit0 alt-fire
inline constexpr uint8_t kRoundEventFlagAdmIndexed      = 0x02; // bit1 adm-indexed
inline constexpr uint8_t kRoundEventHasSlotByte         = 0x80; // [ring+32 != 0 @0x5048bb]
inline constexpr uint8_t kRoundEventHasTargetHandle     = 0x40; // [live fire target @0x50485a]

// One §5.9.1 ROUND-EVENT record (ex "weapon-hit" — a decode-era misnomer): a round
// FIRED by another player, carried as the fire origin + direction the receiving
// client re-simulates the round from (RoundData_SpawnRound); no impact is on the
// wire. Host write side: NetPacket_SerializeRoundEvent @0x504820 serializes one
// g_RoundRing record per event; client read side:
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

	bool has_slot_byte() const { return (flags & kRoundEventHasSlotByte) != 0; }
	bool has_target_handle() const { return (flags & kRoundEventHasTargetHandle) != 0; }
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
// .sub_op): [orig: NetPacket_DispatchEntityPacketCallback @ 0x4D6A80] copies it to
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
	uint16_t fog_dist = 0;      // → g_EnvFogDistTarget (raw << 16)        [0x430267]
	uint16_t fog_accel = 0;     // → g_EnvFogDistAccelClamp (raw << 8)     [0x430286]
	uint16_t tod_fixed = 0;     // → g_EnvCurTimeFixed24 (time-of-day, raw << 13) [0x4302AE]
	uint8_t  quake_ticks = 0;   // → g_EnvQuakeTicks (screen-shake)        [0x4302CE]
	uint8_t  cloud_scroll = 0;  // → g_EnvCloudScrollRateTarget (raw << 10) [0x4302EC]
	uint8_t  rain_pct = 0;      // → g_EnvRainPctTarget (raw << 8)             [0x430311]
	uint8_t  overcast = 0;      // → g_EnvOvercastBlendTarget (raw << 8)   [0x43032D]
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
	uint8_t  preround_timer = 0; // [orig: g_PreRoundDelayTimer @0xC8D824] → dword_A85B64 [0x430064]
	uint8_t  slot_state360 = 0;  // playerSlot+360 (0 unless entity+36 bit 1) → dword_A85B5C [0x430084]
	uint8_t  slot_state368 = 0;  // revive seconds, playerSlot+368 (0 unless dead) → dword_A85B60 [0x43009F]
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
	uint8_t  state0 = 0;        // → g_WacVarBreathTime  [0x4301A1]
	uint8_t  state1 = 0;        // → g_WacVarFallMps  [0x4301BC]
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

// The 0x0A conditional mounted-weapon ammo record (`flags2 & 0xF == 8`). The
// handle is the recipient's mount target; when present the two words mirror the
// selected MountSlot's clip/reserve fields at +0x10/+0x12.
// [orig: writer @0x4FFD9A..0x4FFE7B; reader @0x430459..0x430541]
struct FrameMountAmmo {
	bool     present = false;          // true only after the whole conditional record decodes
	uint16_t mount_handle = 0xFFFF; // recipient mount target; 0xFFFF ⇒ on foot
	bool     has_mount = false;     // true when clip/reserve follow
	uint16_t clip = 0;              // selected MountSlot +0x10
	uint16_t reserve = 0;           // selected MountSlot +0x12
};

// The 0x0A header flags2 selectors: the low nibble routes the sub-block —
// values 0..3 are the plain sub-block cycle, and the exact value 8 (sub-block 0
// with bit 3) appends the phase-8 mounted-ammo record
// [orig: writer @0x4FFD9A / reader @0x430459; high nibble ignored, D-NET-75].
inline constexpr uint8_t kFrameFlags2SubBlockCycleMask = 0x03;
inline constexpr uint8_t kFrameFlags2RouteMask         = 0x0F;
inline constexpr uint8_t kFrameFlags2MountedAmmoRoute  = 0x08;

struct FrameUpdate {
	// Header refs (dword_A822E4/E8/EC) — the i32 16.16 world anchor each compact
	// record's decompressed position is added to (when unmounted).
	int32_t anchor_x = 0, anchor_y = 0, anchor_z = 0;
	uint8_t  flags1 = 0;            // loadprog / death-spectator signals
	uint8_t  flags2 = 0;            // low 2 bits = sub-block selector, bit 3 = phase-8 mounted-ammo gate
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
	FrameMountAmmo      passenger;  // legacy packet-section name; mounted ammo, valid at phase 8
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
// phase-8 mounted-ammo record, every tag==1 per-entity compact record, AND every
// tag==2 fired-round record (§5.9.1). `class_of` maps a wire type_id to its compact
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
// `authority_recipient` is the listen host's own player parsing its loopback
// frame: the walk returns after the anchor, flags1, phase byte, and the phase-0
// block — retail's local client never reads a tail, records, or rounds
// [orig: NapiNPClientMsg_0x00A local return @0x430174].
bool decode_frame_update(const uint8_t *body, size_t len,
                         const std::function<EntityClass(uint16_t)> &class_of,
                         FrameUpdate &out, bool is_objective_gametype = false,
                         bool authority_recipient = false);

// ===========================================================================
// S2C game-event + kill messages — the kill feed and entity-death replication.
// ===========================================================================

// S2C 0x1E — game event (kill feed + objectives + zone control). Fixed 8 B.
// [orig: NetPacket_HandleGameEvent @ 0x426270]. The client resolves the three
// pool-0 indices to entities, then a ~60-case switch on event_type selects a
// "Canned Msg"/STRCNDnn string, formats it via HUD_FormatKillEventMessage
// (@ 0x422DA0) and posts it to the kill feed (Chat_AddMessageChannel2); some types
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
// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 — g_ClientCurrentTick @0xA8229C @0x4297f8,
//  g_LastKeepaliveTick @0xA822A0 @0x4297fd, short-body zero @0x4297eb;
//  Server_SendRandomSeedToPlayer @0x5101a0 (value @0x5101d4, disarm @0x510237);
//  gate PlayerSlot_IsActive @0x4fc760]
bool decode_tick_seed(const uint8_t *body, size_t len, uint32_t &out);

// S2C 0x26 — item class-state replication: [u16 entity][i16 hit section].
// Word one is stored in the hit record before class callback phase four.
// [orig: NapiNPClientMsg_0x026 @ 0x42EC30 -> Entity_KillBySlotId @ 0x42BCE0]
struct KillRecord {
	uint16_t victim_slot = 0xFFFF;  // (pool<<12)|slot of the entity that dies
	int16_t section = 0;           // hitRecord[14]; a missing second word reads zero
};
bool decode_kill_record(const uint8_t *body, size_t len, KillRecord &out,
                        size_t &consumed);

// S2C 0x4E — one PAGE of the join-window kill list: `[u16 resume][u16 slot]×N`
// to the body end. `count` is the host iterator's resume index (0xFFFF when the
// walk is exhausted; the retail handler names it count and never uses it as a
// read bound); every following slot dies via Entity_KillBySlotId(slot, 0, 1)
// and, when at least one slot followed, the client queues the C2S 0x28
// continuation {dword_A82360 (the S2C 0x19 value), dword_A82364 (the S2C 0x1A
// value), count} so the host serves the next page from `count`. A bare `FF FF`
// (a walk that found nothing) ends the exchange with no reply. A page holds at
// most 33 slots (the builder loops while `count <= 32`).
// [orig: NapiNPClientMsg_HandleBatchKill @ 0x431870 — count @0x43188a, the kill
//  loop @0x4318a5..0x4318c2, the reply @0x4318db..0x4318ff, the bare-page return
//  @0x431904; page builder Server_CollectValidWeaponSlots @0x516000 (resume store
//  @0x5160d7); Kong labeled the handler HandleBatchSpawn; it kills].
struct BatchKillBatch {
	uint16_t count = 0;             // the resume index echoed as the next C2S 0x28 `start`
	std::vector<uint16_t> slots;    // (pool<<12)|slot of each entity that died inside the window
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
//       live g_CurrentWeaponSlot), then C2S 0x22/0x23 acks (@0x431acb..0x431b05),
//       Player_InitPlayer(1) @0x431b14, then the identity stores
//       `entity->animSlot = animSlot` @0x431b3a / `entity->NetId = netId` @0x431b46
//       and the minimap maintenance they feed (@0x431b4d..0x431b91).
// [orig: NapiNPClientMsg_TeamAssign (0x50) @ 0x431910; host producer Server_ChangeEntityTeam
//  @ 0x518D70 — it retargets ANY entity, including capture zones (§5.61)]
struct TeamAssign {
	uint16_t entity_handle = 0xFFFF; // (pool<<12)|slot
	uint8_t  team = 0;
	// entity+0x15C wire NetId + entity+0x374 animSlot, the same identity pair the 0x0C
	// player record carries (fields 3-4 / 5). Retail's producer ZEROES both for a
	// non-player entity — the `entity->Flags & 0x100` gate @0x506b3d picks between the
	// live values @0x506b51/@0x506b6b and the zero arms @0x506b96/@0x506bab — so any
	// future emitter must reproduce that gate. [orig: NetPacket_WriteEntityHandlePacket @0x506ad0]
	uint16_t net_id = 0;
	uint8_t  anim_slot = 0;
};
bool decode_team_assign(const uint8_t *body, size_t len, TeamAssign &out,
                        size_t &consumed);

// S2C 0x51 TEAM-CHANGE CONFIRM — `[i16 index]` then the 0x50 body
// `[u16 handle][u8 team][u16 netId][u8 animSlot]`: one entry of the host's
// team-change list, the reply to a C2S 0x29 {index}. The handler reads every
// field with a zero default (a short body still dispatches, handle 0
// included), so this decoder never fails; it reports whether the whole 8-byte
// body arrived. [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 — the reads
// @0x431bc4..0x431c11; the writer NetPacket_WriteEntityPacket @0x506BB0]
struct TeamChangeConfirm {
	uint16_t index = 0;   // the list index the C2S 0x29 asked for
	TeamAssign assign;    // the entry's 0x50 record; a short tail reads zero
};
bool decode_team_change_confirm(const uint8_t *body, size_t len, TeamChangeConfirm &out);

// ===========================================================================
// C2S 0x0C — per-entity client-to-host packet. Outer body starts with a 5-byte
// sub-header `[u16 handle][u16 itemTypeId][u8 sub_op]` written by
// [orig: Pool_SerializeEntityViaVTable @ 0x4D64E0] and parsed by
// [orig: NetPacket_DispatchEntityPacketCallback @ 0x4D6A80]. `sub_op` selects the
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
// NetPacket_DispatchEntityPacketCallback @0x4D6A80 dispatches]
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
	                                   // g_PoolList @0x4c1d07-0x4c1d26]. Renamed from the
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
	                                   // @0x4C20A3 gated g_AdmDefs[idx].category < 11; the host
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
// validator locally → RoundData_AddRound appends a g_RoundRing event that
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
// [orig: NapiNPServerMsg_HandleAntiCheatCRCCheck @ 0x502050]
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

// S2C 0x6E — spawn-wave/deploy-screen status (§5.31). `[u8 groupCount]` then
// per group `{ u16 zoneHandle, u16 zoneIndex, u8 queuedCount,
// u16 waveCountdown, u16 member × queuedCount }`. zoneHandle == 0xFFFF marks
// "no zone entity" (the client skips the entity-slot write but still reads the
// group). The old team/squad-roster names described the destination arrays,
// not the server-side meaning of the packet.
// [orig: NetPacket_WriteSpawnWaveStatus @0x507490;
//        NapiNPClientMsg_HandleSquadRosterSync @0x429880]
struct SpawnWaveGroup {
	uint16_t zone_handle = 0xFFFF;        // (pool<<12)|slot; 0xFFFF = none
	uint16_t zone_index = 0;              // deploy-screen spawn-zone-list index
	uint8_t queued_count = 0;             // number of queued member handles
	uint16_t wave_countdown = 0;          // seconds until this wave releases
	std::vector<uint16_t> members;        // queued_count player/entity handles
};
struct SpawnWaveStatus {
	uint8_t group_count = 0;
	std::vector<SpawnWaveGroup> groups;
};
bool decode_spawn_wave_status(const uint8_t *body, size_t len, SpawnWaveStatus &out);

// S2C 0x81 — requester-local accumulated points `[i32 score]`. This is
// CRenderState field 0x1C (the direct array word at index 29), not the
// game-type-specific primary score shown in the first scoreboard column.
// [orig: Server_UpdateCaptureZoneProximity @0x5086A0;
//        CPlayerStats_GetFieldPlusOne @0x52D7D0;
//        NapiNPClientMsg_ScoreDeltaSound @0x42A0B0]
struct ScoreDeltaSound {
	int32_t score = 0;
};
bool decode_score_delta_sound(const uint8_t *body, size_t len,
		ScoreDeltaSound &out);


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
	std::string mission_name;  // 4 — MissionText title, or waypoint-family filename fallback
	std::string map_file;      // 5 — .bms filename (PunkBuster `mapname`)
	uint32_t    extra = 0;     // u32
	std::string motd;          // 6 — unwitnessed (empty in every capture); the "MOTD" guess is unconfirmed
	std::string game_name;     // 7 — game name (PunkBuster `gamename`)
};
bool decode_full_player_info(const uint8_t *body, size_t len, FullPlayerInfo &out);

// S2C 0x0F — world-state-load (§5.29). The joiner's spawn pose + game flags + the
// authority's per-ammo-class POOL table + waypoint/location-name lists (~624 B).
// Layout:
//   [i32 sessionTick][i32 posX][i32 posY][i32 posZ]   (pos 16.16)
//   [i16 yaw][i16 pitch][i16 roll]                     (each <<16 to 16.16)
//   [u8 gameFlags]
//   i32 ammoPools[kWorldStateAmmoPoolCount]            (fixed 128-entry block: the
//       serverPlayer+88664 image, copied straight into the client's
//       g_LocalAmmoPools @0xB75FE8 and followed by the clip recalculation
//       [orig: loop @0x42e324..0x42e34a -> WeaponSlots_RecalculateAmmoFromCapacity
//        @0x42e424]; NOT a score table — the pre-2026-09-10 label was wrong)
//   [u16 waypointCount]
//     { u16 slotId, u16 nameId, u8 pad } × waypointCount  // host-gametype-gated
//   [u16 teamNameCount]
//     cstring × teamNameCount
// The waypoint records are gated on the HOST by (g_GameType & 0xFFFDFFFF) ==
// 0x10020 (a waypoint gametype). That gate is NOT on the wire, so an off-wire
// decoder takes the is_waypoint_gametype hint (default false; TDM/DM send
// waypointCount 0 / no records). [orig: NapiNPClientMsg_0x00F @ 0x42E200]
inline constexpr int kWorldStateAmmoPoolCount = 128; // (data - g_LocalAmmoPools)/4 @ 0x42e324
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
	std::array<int32_t, kWorldStateAmmoPoolCount> ammo_pools{};
	uint16_t waypoint_count = 0;
	std::vector<WorldStateWaypoint> waypoints;  // populated only when the hint is set
	uint16_t team_name_count = 0;
	std::vector<std::string> team_names;
};
bool decode_world_state_load(const uint8_t *body, size_t len, WorldStateLoad &out,
                             bool is_waypoint_gametype = false);

// S2C 0x3A — medic-reviving: no fields (the handler reads no bytes), any
// length accepted. [orig: NapiNPClientMsg_0x03A @ 0x422680]
bool decode_medic_reviving(const uint8_t *body, size_t len);

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
// with a visible-players snapshot. [orig: NapiNPServerMsg_0x023_WeaponOverlayBroadcast @ 0x514D50]
bool decode_burst_visible_request(const uint8_t *body, size_t len, size_t &consumed);

// C2S 0x32 — EMPTY-SLOT SWEEP REQUEST. The client queues it inside its S2C 0x0F
// world-state-load reply burst (@0x42e647, beside 0x28/0x29/0x2D — §5.29); the
// host's handler reads NO fields from it and answers S2C 0x5D with every empty
// pool-0 slot index. The handler is authority-gated and skipped while
// `g_NetSpawnSuspended` or `g_SpawnSuccessGate` (round over) is set. Because
// the body is never read, this decoder consumes nothing and accepts any length —
// the sender's exact filler (if retail writes any) is unwitnessed.
// [orig: NapiNPServerMsg_SendEmptySlots @ 0x51a600; body builder @ 0x5160f0;
//  client sender NapiNPClientMsg_0x00F @ 0x42e647]
bool decode_empty_slots_request(const uint8_t *body, size_t len, size_t &consumed);

// C2S 0x28 — the join-window KILL-LIST request `[u32 windowMin][u32 windowMax]
// [u16 start]` (10 B; each missing field reads 0 @0x51a58e/@0x51a59d/@0x51a5ac).
// The host pages the entities whose last state-change stamp (entity+560, a
// GetTickCount ms) lies in [windowMin, windowMax] AND that are dead/destroyed
// (flags & 2 || flags & 4 || health <= 0) over pools 0..2 from `start`
// (category = start >> 12 advancing 0 -> 0x1000 -> 0x2000, index = start &
// 0xFFF; 0xFFFF = exhausted), replying ONE S2C 0x4E page of <= 33 slots plus its
// resume word, or nothing when spawns are suspended, the round gate is set, or
// the walk starts exhausted. Client senders: the 0x0F reply burst
// {dword_A82360 (S2C 0x19), dword_A82368 (the 0x0F body's session tick), 0}
// @0x42e5d3..0x42e5f7, and the 0x4E continuation {dword_A82360, dword_A82364
// (S2C 0x1A), the page's leading word} @0x4318db..0x4318ff. The IDB's
// "weapon loadout" reading named the fields below; the consumers still spell
// them: loadout_filter = windowMin, flags = windowMax, extra = start.
// [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @ 0x51A550 ->
//  Server_CollectValidWeaponSlots @0x516000 (spawn-suspended / round-gate return
//  @0x51602e, exhausted-at-begin return @0x516057, append @0x5160a9, loop bound
//  @0x5160d1, resume word @0x5160d7); WeaponLoadout_IteratorBegin @0x501680;
//  ItemPoolIterator_Advance @0x501740; NetSync_IsEntityEligibleInWindow @0x507AA0]
struct BurstLoadoutRequest {
	uint32_t loadout_filter = 0;  // windowMin: the S2C 0x19 spawn-ack timestamp (dword_A82360)
	uint32_t flags = 0;           // windowMax: the S2C 0x1A value (dword_A82364) or the 0x0F session tick (dword_A82368)
	uint16_t extra = 0;           // start: 0 for the burst, the previous page's resume word after
};
bool decode_burst_loadout_request(const uint8_t *body, size_t len,
                                  BurstLoadoutRequest &out, size_t &consumed);

// C2S 0x29 — team/spawn ack `[u16 team_change_index]` (2 B). The client emits it with
// team_index+1 from its 0x51 apply [orig: NapiNPClientMsg_HandlePlayerSpawn @ 0x431c99]
// and at deploy/team pick; the server treats the value as a g_TeamChangeEntityList
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

// S2C 0x6B — minimap overlay batch. `[u8 count]` + `count × 12-B records`.
// The handler consumes ALL 12 bytes of each record: the handle is resolved via
// the pool table (invalid pool/slot skips the record), the marker POSITION
// comes from the wire as whole-unit s16s shifted to 16.16, the u16 field is
// the linked-marker lifetime in SECONDS (x62 to ticks), the type byte selects
// the pulse icon (3 -> 24, else 253), and the height byte (shifted to 16.16)
// is the map ring radius. Only the team color is taken from the entity
// (+354). [orig: NapiNPClientMsg_0x06B @ 0x425520 marshalling
//  @0x42559f..0x4255dd → Minimap_UpdateOverlayEntity @ 0x5BEC10]
struct MinimapOverlayBatch {
	struct Entry {
		uint16_t handle = 0;      // +0  (pool<<12)|slot of the overlaid entity
		int16_t x = 0;            // +2  marker X, whole world units
		int16_t y = 0;            // +4  marker Y, whole world units
		int16_t z = 0;            // +6  marker Z, whole world units
		uint16_t lifetime_s = 0;  // +8  linked-marker lifetime, seconds (x62 ticks)
		uint8_t type = 0;         // +10 blip type: 3 person -> icon 24, else 253
		uint8_t height = 0;       // +11 ring radius, whole world units
	};
	std::vector<Entry> entries;
};
bool decode_minimap_overlay_batch(const uint8_t *body, size_t len,
                                  MinimapOverlayBatch &out);

// S2C 0x49 — weapon-reload notification. `[u16 entityHandle][u16 reloadParam]`
// (4 B). The handler resolves the entity and calls WeaponSlot_ReloadAmmo(entity,
// reloadParam); a vehicle entity instead arms an 80-tick timer. NOTE the IDB
// name `NapiNPClientMsg_WeaponReload_0x049 (ex handle_camera_sync_packet_0x049)` is WRONG — there is no camera code, it
// reloads ammo. [orig: NapiNPClientMsg_WeaponReload_0x049 @ 0x42C0A0 (IDA-misnamed)
//  → WeaponSlot_ReloadAmmo @ 0x541720]
struct WeaponReload {
	uint16_t entity_handle = 0;
	uint16_t reload_param = 0;   // WeaponSlot_ReloadAmmo arg (reload slot / amount)
};
bool decode_weapon_reload(const uint8_t *body, size_t len,
                          WeaponReload &out, size_t &consumed);

// S2C 0x35 -- a powerup's `weapon` grant: `[u16 pickerHandle][u16 powerupHandle]`
// (4 B), reliable, to every in-match slot but the listen host (mask 0x90),
// sent by the authority for every picker its pickup admitted. The client acts
// only when the picker is its own live player: it lands the weapon the
// powerup row's +0x2B0 byte names (its own copy of the row, written by its own
// pickup) in its slot table and mounts it. This decoder takes exactly the four
// bytes a stock host writes; retail's handler reads a short body as handle 0.
// [orig: NapiNPClientMsg_0x035 @0x4261A0 -> sub_4E03D0 @0x4E03D0; sender
//  Server_BroadcastWeaponOverlayUpdate @0x509FC0]
struct WeaponPickupNotice {
	uint16_t picker_handle = 0;
	uint16_t powerup_handle = 0;
};
bool decode_weapon_pickup(const uint8_t *body, size_t len,
                          WeaponPickupNotice &out, size_t &consumed);

// C2S 0x16 -- the designated-G mounted weapon route selector. Retail's action-6
// producer writes a bool as a little-endian i16; the authority tests the full
// word for zero/nonzero. This guarded codec accepts exactly two bytes.
// [orig: Input_HandleActionBinding_0 @0x4e0492;
// NapiNPServerMsg_HandleWeaponToggle @0x511a70]
struct MountedWeaponSlotSelection {
	bool use_parent_slot = false;
};
bool decode_mounted_weapon_slot_selection(
		const uint8_t *body, size_t len,
		MountedWeaponSlotSelection &out, size_t &consumed);

// C2S 0x03 — the inverse OPTIONS_AUTOMEDIC preference. Retail writes the
// profile dword at +1660; zero means the checkbox is enabled, nonzero means a
// downed player must explicitly request a medic. The authority retains that
// value at playerSlot+372 and tests only zero versus nonzero.
// [orig: NetPacket_WriteAutoMedicPreference @0x42A400 (was NetPacket_WriteSessionTick; D-NET-216); producer
// MultiPlayer_JoinSessionStateMachine @0x56A340; consumer
// NapiNPServerMsg_AutoMedicPreference @0x501BE0]
struct AutoMedicPreference {
	bool enabled = true;
};
bool decode_auto_medic_preference(
		const uint8_t *body, size_t len,
		AutoMedicPreference &out, size_t &consumed);

// C2S 0x2E — a downed player's manual medic call. The client writes its own
// entity's pool slot index as one dword (`NetPacket_WriteEntityIndex32`) and
// sends it reliably under a 310-frame latch while dead and in session; the
// host handler never reads the body — it resolves the requester from the
// connection's player slot — so the dword is carried for the printer only.
// [orig: Input_HandleActionBinding case 217 @0x49B4B4..0x49B51B;
// Server_BroadcastMedicRequest @0x515390]
struct MedicRequest {
	uint32_t entity_index = 0;
};
bool decode_medic_request(const uint8_t *body, size_t len,
		MedicRequest &out, size_t &consumed);

// S2C 0x13 — entity death (the SECOND death path, beside 0x26 kill-sync).
// `[u16 entityHandle][i16 deathAnimStateId]` (4 B). The host writes word1 from
// the victim's entity+0x2C0 staged death-anim slot — never the killer; both
// infantry death edges consume and zero that slot before the send, so it is 0
// on every edge-driven death. The handler sets the entity's Health=0, stores
// the word sign-extended at entity+0x2C0 (deathAnimStateId), clears
// entity+0x1BA, and fires its death callback(entity, 4, 0); if the local
// player died it stamps the respawn tick + toggles the weapon scope. Unlike
// 0x26 (which routes through Entity_KillBySlotId), this path acts directly on
// the entity.
// [orig: NetPacket_BuildDeathNotifyPayload @0x5036E0 (@0x503733);
//  NapiNPClientMsg_EntityDeath @0x42EB50 (movsx @0x42EB8D, store @0x42EBDF)]
struct EntityDeathRecord {
	uint16_t entity_handle = 0;        // the dying entity
	int16_t  death_anim_state_id = 0;  // victim entity+0x2C0 death-anim slot (i16)
};
bool decode_entity_death(const uint8_t *body, size_t len,
                         EntityDeathRecord &out, size_t &consumed);

// S2C 0x52 — victim-only third-person death-camera target. Retail sends the
// killer's fixed position when a killer exists, otherwise the victim's, and the
// client stores the triple for Camera_ComputeThirdPersonPositions.
// [orig: GameEvent_PlayerDeath @0x516DD0 -> NetPacket_WriteThreeInt32s
// @0x506CB0; NapiNPClientMsg_0x052 @0x428A80; camera consumer @0x438B80]
struct DeathCameraTarget {
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
};
bool decode_death_camera_target(const uint8_t *body, size_t len,
		DeathCameraTarget &out, size_t &consumed);

// S2C 0x54 — one player slot's downed/revive state. The low seven bits are
// the remaining whole-second revive window (retail arms 120); bit seven is the
// retained medic-request flag. The handler resolves entity -> roster slot and
// splits the byte into playerSlot+16/+44.
// [orig: NetPacket_WriteEntityHandleWithByte @0x507030;
// NapiNPClientMsg_0x054 @0x429040 -> PlayerSlot_SetDownedState @0x4348D0]
struct PlayerDownedState {
	uint16_t entity_handle = 0xFFFF;
	uint8_t revive_seconds = 0;
	bool medic_request_active = false;
};
bool decode_player_downed_state(const uint8_t *body, size_t len,
		PlayerDownedState &out, size_t &consumed);

// S2C 0x23 — a WAC command the host VM replicated because its registry flags
// carry 0x18. `[u16 wireIndex]` then one field per declared registry operand:
// Text/Filename as a cstring of at most 250 chars plus NUL, Ssn as the packed
// u16 handle, every other type as the resolved u32. The client reads the same
// registry row; a body that ends early sets `read_error` and ZERO-FILLS the
// remaining operands but still dispatches, and bytes past the last operand are
// ignored. The decoder rejects only what that row walk cannot address — an
// index past the registry (retail would read beyond its table) — and a row
// without the 0x18 flags, which the client never dispatches.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 — u16 index @0x4f5cf9, operands
//  @0x4f5d26..0x4f5dc2; GameMode_DispatchRemoteCommand @0x4F81E0 — u16 id
//  @0x4f827e, operand loop @0x4f830a..0x4f840a, string cap @0x4f83d0,
//  `(flags & 0x18)` gate @0x4f8429]
struct ScriptRemoteCommandArg {
	uint32_t value = 0;   // Ssn (u16) and every numeric operand
	std::string text;     // Text / Filename operands
};
struct ScriptRemoteCommand {
	uint16_t command_index = 0;
	std::vector<ScriptRemoteCommandArg> args; // one per declared registry operand
	bool read_error = false;
};
bool decode_script_remote_command(const uint8_t *body, size_t len,
		ScriptRemoteCommand &out, size_t &consumed);

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

// S2C 0x79 — host network-quality scalar `[u8]` (1 B). The client stores it
// into CNetStats.host_quality (+0x0C), then S2C 0x7D makes the client report
// that value plus three adjacent metrics in C2S 0x50.
// [orig: Server_TickUpdate @0x51E1B2..0x51E202 / NapiNPClientMsg_NetworkQuality @0x429B00]
bool decode_network_quality(const uint8_t *body, size_t len,
                            uint8_t &out_quality, size_t &consumed);


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

// S2C 0x12 — destroy ONE entity by packed pool/slot handle. The host emits the
// record before releasing the authoritative row; the client destroys that one
// row and DETACHES its dependents (children keep their rows, parent link
// cleared). [orig: Server_RemoveEntityAndNotify @0x50A270 -> Entity_Destroy
//  @0x43E810 (occupant/mount detach @0x43e9e9/@0x43ea38, one-row memset
//  @0x43ea70)]
struct EntityRemove {
	uint16_t entity_handle = 0xFFFF;
};
bool decode_entity_remove(const uint8_t *body, size_t len,
	                      EntityRemove &out, size_t &consumed);

// S2C 0x3F — the authority's HUD relay, two kinds behind a leading byte.
// Kind 0, the objective notification [i32 slot][i32 is_win][i32 is_active]
// [u8 flag]: the client re-runs HUD_ShowObjectiveNotification with flag 0,
// then plays NEW_GOAL at its local player for flag 1. Kind 1, the
// mission-text chat relay [i32 team][cstr key]: the key resolves in the
// client's mission text and the line posts when non-empty. The kind-0 arm
// falls through into the kind-1 reads; a kind-0 body carries no tail, so
// those guarded reads give team 0 and an empty key, whose lookup posts
// nothing. Any other kind reads nothing. The key copy stops at 255 chars.
// [orig: NapiNPClientMsg_0x03F @0x42BB20 — the kind byte @0x42bb53, kind 0
//  @0x42bb80..0x42bbb9, HUD_ShowObjectiveNotification @0x42bbc2, NEW_GOAL
//  @0x42bbc7..0x42bbe4, the kind-1 reads @0x42bbec..0x42bc1c and relay
//  @0x42bc39; sender Server_BroadcastEntityActionPacket @0x5080D0]
struct ObjectiveNotification {
	uint8_t kind = 0;
	int32_t slot = 0;
	int32_t is_win = 0;
	int32_t is_active = 0;
	uint8_t flag = 0;
	int32_t team = 0;
	std::string key;
};
bool decode_objective_notification(const uint8_t *body, size_t len,
		ObjectiveNotification &out, size_t &consumed);

// S2C 0x2F — complete live state for flag/carryable objectives. The first
// relationship is entity+368 occupantEntity (the carrier); the second is
// entity+40 groundEntity. The flags field is deliberately one byte: retail
// merges it into the low byte of the client's existing entity flags.
// [orig: NetPacket_SerializeEntityWithParentAndTarget @0x505810;
// NapiNPClientMsg_0x02F @0x430E10]
struct ObjectiveEntityState {
	uint16_t entity_handle = 0xFFFF;
	uint8_t flags_byte = 0;
	int32_t pos_x = 0;
	int32_t pos_y = 0;
	int32_t pos_z = 0;
	uint16_t attach_handle = 0xFFFF;
	uint16_t ground_handle = 0xFFFF;
};
bool decode_objective_entity_state(const uint8_t *body, size_t len,
	                               ObjectiveEntityState &out,
	                               size_t &consumed);

// S2C 0x59 — deployed-item spawn-or-update. Fixed 32-B record. The host
// streams the placeable entities a player drops (mines, beacons, satchels,
// deployed guns…). The handler searches the 512-entry ROUND pool (the same
// 780-B records the projectile machinery walks, cursor from unk_B7E1C4) for a
// matching entity and either updates its transform in place or allocates a
// new pool entry initialised from the item def. `friendlyItemId`/`enemyItemId`
// let one deployable show a different model to friend vs foe — applied at
// FRESH SPAWN only and only when BOTH are nonzero (owner team @ +354 vs the
// local player); `itemId` is the base/fallback. The handler reads 15 u16s
// (30 B); the 2 trailing bytes are unread.
// [orig: NapiNPClientMsg_0x059 @ 0x4228E0 → Entity_SpawnOrUpdateFromSlotPacket
//  @ 0x546770 (variant pick @0x5469db..0x546a11)]
struct DeployedItemSpawn {
	uint16_t item_id = 0;          // packet[0] — fallback / base item id
	uint16_t owner_handle = 0;     // packet[1] — the placing entity
	uint16_t friendly_item_id = 0; // packet[2] — model shown to the owner's team
	uint16_t enemy_item_id = 0;    // packet[3] — model shown to the other team
	uint16_t slot_handle = 0;      // packet[4] — the spawned entity (pool<<12)|slot
	uint16_t parent_handle = 0xFFFF; // packet[5] — attach parent (0xFFFF = none)
	int32_t  pos_x = 0, pos_y = 0, pos_z = 0;       // i32 16.16 world position
	// packet[12..14]: the high halves of entity+16/+20/+24 = the eulerZ/eulerX/
	// eulerY triple (yaw heading, pitch, roll); the handler stores each << 16
	// [orig: @ 0x5468cb..0x5468df; emitters Entity_UpdateSatchelPhysics
	//  @ 0x448aeb..0x448b09 / Entity_UpdateClaymorePhysics @ 0x447a6c..0x447a8a]
	uint16_t angle_x = 0, angle_y = 0, angle_z = 0;
	uint16_t reserved = 0;         // 2 trailing bytes (not read by the handler)
};
bool decode_deployed_item_spawn(const uint8_t *body, size_t len,
                                DeployedItemSpawn &out, size_t &consumed);

// S2C 0x44 — entity-routed sub-packet. A 5-B sub-header `[u16 field0][i16 netId]
// [u8 subtype]` followed by a class-dependent body the dispatcher routes to the
// target entity's per-class serialize callback (entity def+356, source_type=2) —
// the SAME per-class path the C2S 0x0C entity-uplink uses (§5.10b). We decode the
// sub-header + expose the body slice; for the GUIDED class the body is fully
// mapped and folded (§5.15, replication `apply_entity_routed`); other classes' bodies
// remain unmapped. [orig: NapiNPClientMsg_0x044
//  @ 0x422710 → NetPacket_DispatchToEntityByNetId @ 0x4D6960]
struct EntityRoutedPacket {
	uint16_t field0 = 0;            // [0..1] the shooter handle (entity+368;
	                                //  fork-witnessed — supersedes the earlier
	                                //  "not read by the dispatcher" gloss)
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
// [orig: Terrain_SerializeTiles @ 0x6080F0 (writer) / PolyTrn_LoadTileData
//  @ 0x6081D0 (reader) / NapiNPClientMsg_0x045 @ 0x422890 (handler)]
struct TerrainTileEntry {
	uint32_t word0 = 0;  // 12-B opaque tile record (copied raw into g_TerrainTileData)
	uint32_t word1 = 0;
	uint32_t word2 = 0;
};

// §5.37 stream framing: the first chunk announces itself with the 0xFFFF wire
// start word, then the 'til0' header magic (the reader bails without loading on
// a mismatch). engine/formats/til's TIL_MAGIC pins the same literal for the on-disk form.
inline constexpr uint16_t kTerrainFirstChunkStartWord = 0xFFFF;
inline constexpr uint32_t kTerrainTileMagic = 0x74696C30; // 'til0' little-endian
static_assert(kTerrainTileMagic == 0x74696C30u);
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
// Door-row sync — S2C 0x37 / C2S 0x1A (docs/world/world-wac-ai-re.md §33.14).
// The IDB calls both "weapon slot" messages; the rows they carry are the
// 0xA8A418 door records {state, phaseQ16, stepQ16, maxAngle, entity, number}.
// ===========================================================================

// The one 5-byte body both directions share: `[u16 handle][i16 state][u8 number]`.
// Every retail reader takes each field as 0 when the body runs short
// [orig: client @0x431263..0x431293; server @0x514b5f..0x514b8c], so a short
// body decodes zero-filled and returns false (its `number` is then 0, which the
// `number != 0` gate of every receiver drops — behaviorally identical).
//   S2C 0x37 (authority -> clients, send_mask 0x90; the 0x1A reply uses 0x30):
//     row = (int16)entity+0x2B8 + number - 1; gate itemDef && row > -1 &&
//     number != 0 && number <= (int8)itemDef+0x890; row.state = state; state 0
//     snaps the phase to 0, state 2 to 0x10000, 1/3 leave it for FadeEffect_UpdateAll
//     [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250 — gate @0x4312f8,
//      store @0x431307, snaps @0x43131f / @0x431316].
//     Senders: Server_SendWeaponSlotActionPacket @0x50F9A0 — the state word is
//     the state of row (door_slot + n - 1), 0 when n > door count (@0x50f9c0..
//     0x50f9ce; unguarded for n == 0), called with the 1-based row number on
//     each completion (FadeEffect_UpdateAll @0x44e978..0x44e982) and with the
//     0-based section index for EVERY selected section of a door command,
//     transition or not (Entity_ProcessSectionDamageTransition @0x43f462).
//   C2S 0x1A (non-authority -> host): the same command sites queue it reliable
//     with the row's CURRENT state when row > -1 && idx != 0 && idx <= count
//     [orig: NetPacket_SendWeaponSwitch @0x42D0C0 — gate @0x42d0f9, payload
//      @0x42d138, queue @0x42d169]. The host: authority + sender player + its
//     entity, pool lookup, the same gate, then state 0/3 -> 1 iff value == 1;
//     state 1/2 -> value iff number == 3 (the SECTION number, @0x514c2a); reply
//     S2C 0x37 {handle, row.state, number} to the requester
//     [orig: NapiNPServerMsg_HandleVoteUpdate @0x514B20 — switch @0x514c20..
//      0x514c35, reply @0x514c46..0x514c74].
struct DoorSlotAction {
	uint16_t entity_handle = 0;  // (pool<<12)|slot; 0xFFFF when the sender's entity is in no pool
	int16_t  state = 0;          // door row state: 0 closed, 1 opening, 2 open, 3 closing
	uint8_t  number = 0;         // 1-based row number on completions; 0-based section idx on commands
};
bool decode_door_slot_action(const uint8_t *body, size_t len,
                             DoorSlotAction &out, size_t &consumed);

// ===========================================================================
// NovaWorld clan roster — S2C 0x6A / C2S 0x4E. A per-account linked list on
// every peer (node +12 netId, +16 name[65], +81 tag[9]) whose TAG is the
// highlighted third column of the Tab list: PlayerSlotTable_UpdateAllDisplayNames
// @0x434A00 -> PlayerSlot_SetName @0x4348F0 copies the tag of the node whose
// netId equals the slot's account id (slot dword 15 = the 0x46 field-0x0800
// u32, PlayerSync::account_id) into slot dword 8, which
// NapiNPClientMsg_PlayerList @0x42fd85 copies into the row's 8-char third
// string. LAN accounts have netId 0: no node, no 0x6A, no reply to the walk.
// ===========================================================================

inline constexpr uint8_t kClanRosterAdd = 1;        // Server_PlayerAdd @0x51ceef, first slot of an account (mask 4112)
inline constexpr uint8_t kClanRosterRemove = 2;     // Server_HandlePlayerDisconnect @0x51b7eb, last slot (mask 4112)
inline constexpr uint8_t kClanRosterWalkReply = 3;  // reply to C2S 0x4E (requester only, mask 32); the client re-queues C2S 0x4E {netId}
inline constexpr size_t  kClanRosterNameChars = 64; // node +16 is char[65]
inline constexpr size_t  kClanRosterTagChars = 8;   // node +81 is char[9]

// S2C 0x6A — `[u8 action][u32 accountNetId]`, then for actions 1/3 `[cstr name]
// [cstr tag]`. The reader keeps at most 64 / 8 chars (stopping at NUL or the
// body end) but skips to the NUL of the FULL name before the tag; a short id
// reads 0; any other action is ignored (this decoder returns false for it).
// Non-authority only: the host keeps its own list from the join records.
// [orig: NapiNPClientMsg_HandlePlayerJoinLeave @0x432510 — action @0x43254b,
//  remove @0x432578, id @0x4325b1, name loop @0x4325bc..0x4325db, tag
//  @0x4325ea..0x43261b, upsert CLinkedList_FindOrCreateByNetId @0x43262e,
//  the action-3 walk continuation @0x43266c]
struct ClanRosterUpdate {
	uint8_t     action = 0;      // kClanRosterAdd / kClanRosterRemove / kClanRosterWalkReply
	uint32_t    account_id = 0;  // the NovaWorld account netId (0 on LAN)
	std::string name;            // <= kClanRosterNameChars (actions 1/3)
	std::string tag;             // <= kClanRosterTagChars (actions 1/3)
};
bool decode_clan_roster_update(const uint8_t *body, size_t len, ClanRosterUpdate &out);

// C2S 0x4E — `[u32 afterNetId]` (a short body reads 0). {0} is the walk KICK
// the client sends when the S2C 0x05 flag is nonzero, after freeing its list
// (@0x42e1b3 / @0x42e1d9); {netId} is the continuation queued from each 0x6A
// action 3 (@0x43266c). The host answers with the smallest node id strictly
// greater than the value as 0x6A action 3, or nothing when none exists.
// [orig: NapiNPServerMsg_HandleMinimapSlotRequest @0x511210 — read @0x511245,
//  sub_52B190 @0x511253, NetPacket_SerializeMinimapSlot(3) @0x51127c, send @0x51129e]
struct ClanRosterWalkRequest {
	uint32_t after_account_id = 0;
};
bool decode_clan_roster_walk_request(const uint8_t *body, size_t len,
                                     ClanRosterWalkRequest &out, size_t &consumed);

// ===========================================================================
// Vehicle spawning — C2S 0x42 -> S2C 0x70 availability list, C2S 0x40 pick.
// The host keeps a 128-row EntityLimit table (23 rows at mission start: per
// type the cap, the initial per-team allotment and 70 per-team counters)
// [orig: EntityLimit_InitTable @0x509A70, EntityLimit_SetEntry @0x500E50,
//  the availability gate sub_5104C0 @0x5104C0, Server_CountEntitiesByTypeAndTeam
//  @0x510460]; this is that table's wire face.
// ===========================================================================

// S2C 0x70 — `[u8 3]` then per table row `[u16 typeId][u8 avail][u8 max]`,
// terminated by `u16 0`. Per row the writer ladders: unlimited vehicles
// (g_RulesUnlimitedVehicles) -> 0xFF/0xFF; max == -1 && initial == -1 -> 0xFF/0xFF;
// initial == -1 -> max 0xFF, avail = (u8)max_count - liveCount; else max =
// limit[team], avail = (max_count == -1) ? max : min((u8)max_count - liveCount,
// max) (unsigned compare) [orig: NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0 —
// the constant 3 @0x5105c3, the ladder @0x5105ff..0x510651, rows @0x510660..
// 0x510681, terminator @0x5106b8]. The client stores the leading byte
// (dword_A81BB4) and fills a 12-B-stride table until the 0 word or fewer than
// two bytes remain; missing row bytes read 0 [orig:
// NapiNPClientMsg_HandleWeaponLoadoutList @0x429a30 — count @0x429a53, the loop
// @0x429a6a..0x429ab9]. Requester-only reply to C2S 0x42 (send_mask 32)
// [orig: NapiNPServerMsg_SendWeaponSlotStates @0x510930].
inline constexpr uint8_t kVehicleSpawnAvailabilityLeadingByte = 3;
inline constexpr uint8_t kVehicleSpawnUnlimited = 0xFF;
struct VehicleSpawnAvailabilityRow {
	uint16_t type_id = 0;    // items.def id from a pcvehicle_spawnlist row (never 0: that is the terminator)
	uint8_t  available = 0;  // spawns left for the requester's team (kVehicleSpawnUnlimited)
	uint8_t  max_count = 0;  // the team's cap (kVehicleSpawnUnlimited)
};
struct VehicleSpawnAvailabilityList {
	uint8_t leading_byte = kVehicleSpawnAvailabilityLeadingByte; // retail writes the constant; the client stores it unused
	std::vector<VehicleSpawnAvailabilityRow> rows;
	bool terminated = false;  // the u16 0 word was seen
};
bool decode_vehicle_spawn_availability(const uint8_t *body, size_t len,
                                       VehicleSpawnAvailabilityList &out);

// C2S 0x42 — the availability request. The host reads NO fields (it only needs
// the sender's player entity), so the decoder consumes nothing and accepts any
// length; the client's sender is the unported vehicle.mnu screen.
// [orig: NapiNPServerMsg_SendWeaponSlotStates @0x510930]
bool decode_vehicle_spawn_availability_request(const uint8_t *body, size_t len,
                                               size_t &consumed);

// C2S 0x40 — the spawn pick `[u16 sourceHandle][u8 typeIndex]` (3 B; each
// missing field reads 0 @0x51c517 / @0x51c52a, and this decoder then returns
// false). typeIndex is a bit of the SOURCE entity's def+2772 pcvehicle_spawnlist
// mask and indexes the shared g_ItemGroups slot table for the item id
// [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0 — reads
//  @0x51c515..0x51c52a, gates @0x51c532..0x51c5f6].
struct VehicleSpawnRequest {
	uint16_t source_handle = 0;  // the spawner entity (carrier / base object) the player is at
	uint8_t  type_index = 0;     // index into the source def's vehicle_spawn_mask / the shared slot table
};
bool decode_vehicle_spawn_request(const uint8_t *body, size_t len,
                                  VehicleSpawnRequest &out, size_t &consumed);

struct ExplosionEffectRecord {
    uint8_t type = 0;
    uint8_t count = 0;
    uint16_t source = 0;
    int32_t x = 0, y = 0, z = 0;
    int16_t heading = 0;
};
bool decode_explosion_effect(const uint8_t *body, size_t len, ExplosionEffectRecord &out);

} // namespace opennova
