#pragma once

// In-game replication record ENCODERS — the symmetric partner to
// ingame_decode.h. Each encoder is a faithful structural port of the original
// server-side serializer (the host produces these bytes; the client decodes
// them via ingame_decode). The encode/decode pair is exercised by the loopback
// identity test (tests/novaworld/nw_ingame_encode_test) per ADR 0011 §4: the
// host's in-process listen-server path serializes real entity state to the wire
// and the local client decodes it, so encode→decode MUST be field-identical.
//
// Wire-format witnesses live in docs/net/novaworld-net-re.md §5.11/§5.12 (and
// the §5.2a "Server-side S2C serializer map" pairs each load-track tag with its
// originating serializer). Field names mirror the ingame_decode.h structs.
//
// Per ADR 0003 / ADR 0011 §3 these encoders replace the map-locked fixture
// blobs in replication_min.cpp: every byte is produced from the in-memory state
// model, never carried through from a capture.
//
// [orig: serialize_entity_pool_to_packet   @ 0x503460]  — S2C 0x20 bulk pool-3 sync.
// [orig: serialize_entity_pool_to_packet_0 @ 0x503940]  — S2C 0x0D pool spawn (TODO).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "novaworld/ingame_decode.h"

namespace opennova {

// Compress an i32 16.16 fixed-point value to its 16-bit network form — the exact
// inverse of `network_decompress_fixedpoint` (ingame_decode.h). Faithful port of
// [orig: Network_CompressFixedPoint @ 0x4C2780]: sign in bit 0, exponent (shift)
// in bits 1-3, 12-bit mantissa in bits 4-15. The host runs this on
// `world_pos - anchor` (or the vehicle-local delta) before emitting a §5.10/§5.13/
// §5.14 compact record; the result round-trips through the decompressor within one
// quantization step (the codec is lossy/float-like).
//
// Divergence (documented): the original calls `bsr` on the folded magnitude with
// no zero guard, so `value` in {0, -1} (which fold to 0) is undefined in retail.
// We return the sign bit there — the only value that round-trips (0->0, -1->1->-1).
inline uint16_t network_compress_fixedpoint(int32_t value) {
	const uint32_t sign = uint32_t(value) >> 31;        // 0 or 1  [4c279a]
	const int32_t  smask = value >> 31;                  // 0 or -1 (arith)  [4c2795]
	const uint32_t fold = uint32_t(value ^ smask);       // |value| folded  [4c2798]
	if (fold == 0) return uint16_t(sign);                // bsr-undefined guard
	uint32_t bsr = 31;                                   // highest set-bit index  [4c279d]
	for (uint32_t m = fold; !(m & 0x80000000u); m <<= 1) --bsr;
	const uint32_t shift = ((bsr >= 15) ? (bsr - 15) : 0u) | 1u; // [4c27a0..4c27ac]
	uint32_t mantissa = (fold + (8u << shift)) >> shift;          // round + scale  [4c27af..4c27b3]
	if (mantissa > 0xFFFFu) mantissa = 0xFFFFu;                   // clamp  [4c27b8..4c27bf]
	return uint16_t((mantissa & 0xFFF0u) | (shift & 0x0Eu) | sign); // [4c27b5..4c27cb]
}

// Encode a S2C 0x20 bulk pool-3 sync body (§5.12) — the exact bytes
// `decode_pool3_sync_batch` consumes. Faithful port of
// [orig: serialize_entity_pool_to_packet @ 0x503460]:
//   header `[u16 start_index][u16 count]`, then per record
//   `[u16 item_type_id]` (0 ⇒ empty-slot sentinel, record ends), else
//   `[u8 flags][i32 x][i32 y][i32 z]` then the flag-gated optional fields and
//   the always-present `net_handle`.
//
// The flag byte is DERIVED, not trusted: the original sets each gate bit iff
// the corresponding source field is non-zero (`if (value) flags |= bit; write`)
// — so `Pool3SyncRecord::flags_byte` on the input is ignored and recomputed
// here. Field→bit mapping (matching `decode_pool3_sync_batch`):
//   0x01 movement_val (u32, raw BAM heading — NOT a parent; D-NET-59) · 0x02 orientation_val (u32) · 0x04 ammo_count (u16)
//   0x08 team_byte (u8) · 0x10 weapon_type (u16) · 0x20 score_byte (u8).
// `net_handle` (u16) is always written, after the 0x04 field and before 0x08.
//
// Chunking note: the original caps each packet at 650 B (`+30 > 650` break) and
// advances a pool cursor across calls; that per-datagram limit is the host emit
// loop's concern. This function serializes exactly the records handed to it (the
// caller keeps a batch within one datagram, as the original's cursor loop does),
// keeping it a pure byte-format inverse of the decoder.
std::vector<uint8_t> encode_pool3_sync_batch(const Pool3SyncBatch &batch);

// Encode a S2C 0x0D pool-entity spawn batch (§5.11) — the exact bytes
// `decode_pool_spawn_batch` consumes, and a faithful port of
// [orig: serialize_entity_pool_to_packet_0 @ 0x503940]. Header is `[u16 count]`
// (NO start_index — unlike 0x20), then per record `[u16 spawn_flags][u16 slot_id]
// [u16 item_type_id][cstr entity_name]` and the flag-gated body (entity_flags,
// always-pos, vel/section/orient/parent/target, the 0x400 weapon block, the
// always bone_byte (+290; team is the 0x0010-gated byte, D-NET-58), the 0x800 AI trailer, alert/action/weapon_type, the health
// block, difficulty) — see decode_pool_spawn_batch for the exact field order.
//
// As with the pool-3 encoder, the spawn_flags word is DERIVED from which record
// fields are populated (the original sets each bit inside `if (value) { … }`),
// so `PoolSpawnRecord::spawn_flags` on the input is ignored and recomputed:
//   0x0020 entity_flags!=0 · 0x0001/2/4 vel_{x,y,z}!=0 · 0x0008 section_mask!=0
//   0x0010 team_byte!=0 (entity+354; D-NET-58) · 0x0100 parent_handle!=0xFFFF · 0x0200 target_handle!=0xFFFF
//   0x0400 weapon_mask!=0 · 0x0800 (ai_name non-empty || ai_profile_* != 0)
//   0x0040 alert_byte!=0 · 0x0080 action_byte!=0 · 0x1000 weapon_type_byte!=0
//   0x2000 health_byte!=0 (writes health_byte+health_short) ELSE 0x8000 health_short!=0
//   0x4000 difficulty_byte!=0.
// Weapon block (D-NET-56): when 0x0400 is set, `extra_handle_0/1` are ALWAYS
// written after the per-set-bit handles. The original only sets 0x0400 when the
// mask is non-zero, so this encoder never emits the (0x400, mask==0) record.
//
// Boundary note: the original's flag gates for 0x0400/0x0800/0x1000/0x8000 read
// item-def flags and component pointers off the live engine entity (itemDef+604,
// itemDef+84 & 0x100000/0x40000, entity+100/+104). The host driver computes the
// record's fields from a World entity per those gates; this record-level encoder
// then derives the wire flags from the populated fields — a faithful layout whose
// round-trip with decode_pool_spawn_batch is field-identical.
std::vector<uint8_t> encode_pool_spawn_batch(const PoolSpawnBatch &batch);

// Encode a §5.14 infantry / AI compact record (14 B fixed) — the bytes
// `decode_infantry_compact_record` consumes, and the byte order produced by
// [orig: NetPacket_SerializeInfantryEntityState case 1 (write, type 11) @ 0x4C0320].
// One of the per-class callbacks that fill the S2C 0x0A trailing event-loop
// `tag==1` records (org0/org1-class entities — AI infantry).
//
// The record carries the ALREADY-COMPRESSED positions (u16): the original's
// write path runs `Network_CompressFixedPoint` (and `Entity_TransformWorldToLocal`
// when mounted) on the live entity to produce them — work the host driver does
// when building the record from a World entity. This wire encoder writes the u16s
// raw, the exact inverse of `decode_infantry_compact_record`.
std::vector<uint8_t> encode_infantry_compact_record(const InfantryCompactRecord &rec);

// Encode a §5.13 vehicle compact record (15 B mounted / 21 B unmounted) — the
// bytes `decode_vehicle_compact_record` consumes, and the byte order produced by
// [orig: Entity_SerializeMountedVehicleState case 1 (write, type 11) @ 0x460560].
// Used by CHel/cveh/cbot/cpln/ctrn-class entities (controllable vehicles + AI
// ground/air units) in the S2C 0x0A trailing event-loop.
//
// The mounted/unmounted split is gated on `flags_byte & 0x04` — the original
// branches on `entity+36 & 4`, so this encoder branches on the flag bit (not the
// cached `is_mounted`). Positions/headings are the already-compressed u16s
// (`turret_pitch_raw` is a raw i16); `Network_CompressFixedPoint` /
// `Entity_TransformWorldToLocal` run at the World->record layer upstream.
std::vector<uint8_t> encode_vehicle_compact_record(const VehicleCompactRecord &rec);

// Encode a §5.10 player compact record (18 B fixed) — the bytes
// `decode_player_compact_record` consumes, and the byte order produced by
// [orig: NetPacket_SerializePlayerState case 1 (write compact, type 11) @ 0x4C09C0].
// This is the player's own state as replicated to OTHER clients in the S2C 0x0A
// trailing event-loop (the player class also sends the extended C2S 0x0C uplink,
// case 3 — encoded separately).
//
// Positions are the already-compressed u16s (case 1 runs `Network_CompressFixedPoint`
// / `Entity_TransformWorldToLocal` on the live entity at the World->record layer).
// The 18-byte field order is the §5.10 witnessed map, verified here as the exact
// inverse of `decode_player_compact_record`; the case-switch function exceeds a
// single clean decompile, so the layout is cited from the landed §5.10 grill + that
// verified decoder rather than a fresh re-decompile of the write block.
std::vector<uint8_t> encode_player_compact_record(const PlayerCompactRecord &rec);

// Encode ONE §5.15 guided field group — the write side (modes 1/3) of
// [orig: Entity_SerializeGuidedMissileState @ 0x447C50], the inverse of
// decode_guided_field_group. Produces the bytes that follow the 5-byte entity
// sub-header for `group`. `mode` must be a write mode (WriteFull or WriteDelta).
// See ingame_decode.h §5.15 for the (mode × group) size matrix + deferral note.
std::vector<uint8_t> encode_guided_field_group(GuidedMode mode,
                                               GuidedFieldGroup group,
                                               const GuidedRecord &rec);

// Encode a §5.9.1 weapon-hit record — the inverse of decode_weapon_hit_record.
// [orig: NetPacket_DeserializeWeaponHit @ 0x42F270]. 17-20 B by the flags gate.
std::vector<uint8_t> encode_weapon_hit_record(const WeaponHitRecord &rec);

// Build a complete S2C 0x0A frame from a FrameUpdate — the inverse of the §5.9
// decode_frame_update walk. [orig: NapiNPClientMsg_0x00A @ 0x42FEC0]. The host
// constructs the FrameUpdate from live entity state (anchor = subject world pos;
// each compact record's positions compressed via network_compress_fixedpoint
// relative to the anchor) and this emits the wire bytes the client decodes.
std::vector<uint8_t> encode_frame_update(const FrameUpdate &fu);

// ===========================================================================
// C2S encoders — the joiner-side uplinks (a remote client PRODUCES these; the
// host decodes them). Byte-exact inverses of the ingame_decode.cpp C2S parsers,
// validated against real capture frames by nw_ingame_c2s_uplink_test.
// ===========================================================================

// Encode the 5-byte C2S 0x0C sub-header — inverse of decode_entity_packet_sub_header.
// `[u16 handle][u16 item_type_id][u8 sub_op]`.
// [orig: Pool_SerializeEntityViaVTable @ 0x4D64E0]
std::vector<uint8_t> encode_entity_packet_sub_header(const EntityPacketSubHeader &hdr);

// Encode the §5.10 extended (type-10) player uplink body — 43 B fixed, the inverse
// of decode_player_extended_uplink. This is the C2S 0x0C body a remote joiner sends
// for its own player each frame; the 5-byte sub-header (encode_entity_packet_sub_header)
// precedes it on the wire. [orig: NetPacket_SerializePlayerState case 3/4 @ 0x4C09C0]
std::vector<uint8_t> encode_player_extended_uplink(const PlayerExtendedUplink &r);

} // namespace opennova
