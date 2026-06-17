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
//   0x01 parent_handle (u32) · 0x02 orientation_val (u32) · 0x04 ammo_count (u16)
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
// always team_byte, the 0x800 AI trailer, alert/action/weapon_type, the health
// block, difficulty) — see decode_pool_spawn_batch for the exact field order.
//
// As with the pool-3 encoder, the spawn_flags word is DERIVED from which record
// fields are populated (the original sets each bit inside `if (value) { … }`),
// so `PoolSpawnRecord::spawn_flags` on the input is ignored and recomputed:
//   0x0020 entity_flags!=0 · 0x0001/2/4 vel_{x,y,z}!=0 · 0x0008 section_mask!=0
//   0x0010 orient_byte!=0 · 0x0100 parent_handle!=0xFFFF · 0x0200 target_handle!=0xFFFF
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
// (`weapon_y_raw` is a raw i16); `Network_CompressFixedPoint` /
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

} // namespace opennova
