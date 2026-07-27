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
// Per ADR 0003 / ADR 0011 §3 these encoders replace the old map-locked fixture
// blobs: every byte is produced from the in-memory replication model, never
// carried through from a capture.
//
// [orig: serialize_entity_pool_to_packet   @ 0x503460]  — S2C 0x20 bulk pool-3 sync.
// [orig: serialize_entity_pool_to_packet_0 @ 0x503940]  — S2C 0x0D pool spawn (TODO).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "npwire/ingame_decode.h"
#include "npwire/replication_model.h" // PlayerReplicationState — the §5.1 reply encoders' host-side input

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
//   0x2000 zone_number_rank!=0 (writes zone_number_rank+zone_radius) ELSE 0x8000 zone_radius!=0
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

// Encode a §5.9 S2C 0x10 pool-2 static-entity batch — the inverse of
// decode_static_entity_batch and the bytes a host streams in phase 1 of the world-load
// sequence for purely-static structures (buildings, oil-field props). Header
// `[u16 start_index][u16 count]`, then per record `[u16 item_type_id]` (0 ⇒ empty-slot
// sentinel) else the flag-driven body. The field_flags word is DERIVED from populated
// fields (the original sets each gate bit inside `if (value) { flags |= bit; write }`), so
// `StaticEntityRecord::field_flags` on the input is ignored and recomputed.
// [orig: sub_5042F0 (write) / NapiNPClientMsg_0x010 @ 0x433400 (decode).]
std::vector<uint8_t> encode_static_entity_batch(const StaticEntityBatch &batch);

// Encode a §5.37 S2C 0x45 terrain-tile load chunk — the inverse of
// decode_terrain_load_batch and the bytes a host streams in phase 5 of the world-load
// sequence so a JOINER loads the mission's terrain-tile (.til) array. The first chunk carries
// the header (wire start word 0xFFFF + `'til0'` magic + total tile_count + hdr2/hdr3); every
// chunk then writes its `[start_index]..[end_index)` run of 12-B opaque tile entries. The full
// tile set is PAGED into ~650 B datagrams by the host emit loop (one TerrainLoadBatch per page).
// [orig: serialize_terrain_tiles @ 0x6080F0 (write) / PolyTrn_LoadTileData @ 0x6081D0 (read) /
//  NapiNPClientMsg_0x045 @ 0x422890 (handler); D-NET-83.]
std::vector<uint8_t> encode_terrain_load_batch(const TerrainLoadBatch &batch);

// Encode a §5.23 S2C 0x0C organic-entity spawn batch — the inverse of
// decode_organic_spawn_batch and the bytes a host streams so a JOINER can name-match its
// own pool-0 player (the type-0x14b9 organic whose entity_name == the joiner's player name)
// and learn its wire handle. Every field after has_body is unconditional (no flag gates),
// so this is a straight field-order write. [orig: NapiNPClientMsg_0x00C @ 0x42E730.]
std::vector<uint8_t> encode_organic_spawn_batch(const OrganicSpawnBatch &batch);

// Encode a §5.46 S2C 0x18 FULL-ENTITY-SPAWN — the inverse of decode_full_entity_spawn
// and the host's reply to a C2S 0x0F entity-info query (the client's self-heal request
// for a stale/mismatched entity). Faithful port of the retail reply serializer
// [orig: serialize_object_to_buffer @ 0x504d10, invoked by
// NapiNPServerMsg_HandlePlayerInfoRequest @ 0x514180 with the queried pool-0/1 entity].
// Field order is unconditional except the seat block (one u16 occupant handle per set
// seat_mask bit) and the name (always a cstr on the wire; the original writes the
// entity name iff itemDef attrib & 0x100000, else the empty string — callers model
// that gate by leaving entity_name empty). The original writes the post-player_class
// byte as a hard 0, so skip_byte is written as 0 regardless of input.
std::vector<uint8_t> encode_full_entity_spawn(const FullEntitySpawnRecord &rec);

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
// [orig: Entity_SerializeVehicleState case 1 (write, type 11) @ 0x460560].
// Used by CHel/cveh/cbot/cpln/ctrn-class entities (controllable vehicles + AI
// ground/air units) in the S2C 0x0A trailing event-loop.
//
// The mounted/unmounted split is gated on `flags_byte & 0x04` — the original
// branches on `entity+36 & 4`, so this encoder branches on the flag bit (not the
// cached `is_dead_pose`). Positions/headings are the already-compressed u16s
// (`health_word` is the raw entity+286 vehicle health u16 — a 0 kills the vehicle
// on the receiving client, @0x460aff); `Network_CompressFixedPoint` /
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

// Encode a §5.9.1 round-event record — the host write side of the tag-2 stream.
// [orig: NetPacket_SerializeRoundEvent @ 0x504820; client read
// NetPacket_DeserializeRoundEvent @ 0x42F270]. 17-20 B by the flags gate.
std::vector<uint8_t> encode_round_event_record(const RoundEventRecord &rec);

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

// Stamp the calculated pose fields of a C2S 0x06 descriptor. Retail writes
// full X/Y/Z, rounds Yaw/Pitch to their high words, then writes five modulo
// 2^16 low-word deltas against the shooter's live {X,Y,Z,Yaw,Pitch} dwords.
// The deltas reconstruct the same pose; they are not an independent
// weapon-specific muzzle-offset vector.
// [orig: NetPacket_WriteEntityPositionUpdate @0x42A6A1..0x42A890]
void set_client_fired_round_pose(
		ClientFiredRound &round,
		const std::array<int32_t, 5> &fire_pose,
		const std::array<int32_t, 5> &shooter_pose);

// Encode the fixed 45-byte C2S 0x06 fired-round descriptor -- the exact inverse
// of decode_client_fired_round. The local joiner predicts the same round, then
// sends this descriptor so the host can validate ammo/ownership and become the
// sole damaging authority. [orig: Entity_FireWeaponAndSendPacket @0x42bd80 ->
// NapiNPServerMsg_0x006_ClientFiredRound @0x513310]
std::vector<uint8_t> encode_client_fired_round(const ClientFiredRound &r);

// Encode the §5.10 extended (type-10) player uplink body — 43 B fixed, the inverse
// of decode_player_extended_uplink. This is the C2S 0x0C body a remote joiner sends
// for its own player each frame; the 5-byte sub-header (encode_entity_packet_sub_header)
// precedes it on the wire. [orig: NetPacket_SerializePlayerState case 3/4 @ 0x4C09C0]
std::vector<uint8_t> encode_player_extended_uplink(const PlayerExtendedUplink &r);

// ===========================================================================
// §5.1 reply-body encoders — colocated with their ingame_decode.cpp partners so the reply tags are
// round-trippable in the same lib (the encode side previously lived in npruntime/server_message_
// dispatch.cpp, decoupled from its decoder). The host-side SOURCE is the PlayerReplicationState reply
// POD; npruntime's reactive dispatcher fills it and calls these.
// ===========================================================================

// tag=0x46 PLAYER-SYNC — the inverse of decode_player_sync (PlayerSync). Flag-driven slot-state record:
// [u8 slot][u16 fieldFlags][u8 entitySlot] then the bit-gated fields in the witnessed order (name 0x1 /
// clan 0x2 / vehicle-name 0x10 / team 0x4 / class 0x8 / vehicle-score 0x20 / late-join 0x1000 / squad
// 0x40 / side 0x80 / quality 0x400 / vehicle-timer 0x800). [orig: NetPacket_SerializePlayerSync0x46
// @0x505e80; client NapiNPClientMsg_PlayerSync @0x431370]. Per-field slot-state modeling
// (score/squad/side/timer) is the remaining D-NET-127 nicety; the wire SHAPE is faithful and
// round-trips through decode_player_sync.
//
// `field_flags` is the REPLY mask, serialized verbatim and gating each field — the server answers
// EXACTLY the fieldFlags the C2S 0x22 requested, ack bit included [orig: @0x505f05 echoes 0x4000].
// Bit 0x4000 = the ROSTER-WALK ack: on receipt the client re-requests the NEXT slot (C2S 0x22 for
// slot+1, fieldFlags 0x5CF7) until slot >= max_players [orig: @0x431370 tail `if (slot < g_max_player_slots)
// QueueReliableMessage(0x22, slot+1)`; g_max_player_slots = the 0x04 slot-config maxPlayers byte]. Default
// 0x1CF7 = the join-broadcast field set [orig: Server_PlayerAdd @0x51d2bf `push 7415`].
std::vector<uint8_t> encode_player_sync(const PlayerReplicationState &ctx,
                                        uint16_t field_flags = 0x1CF7);

// tag=0x46 PLAYER-SYNC REMOVAL — a 3-byte record [u8 slot][u16 flags] with the 0x8000 removal bit set
// (and 0x4000 ack to keep the walk going, matching golden's 0xC000). The client clears/unlinks that slot
// [orig: NapiNPClientMsg_PlayerSync @0x431370 `if (fieldBitmask & 0x8000) PlayerSlot_ClearAndUnlink`].
// Sent for empty roster slots the client's ack-walk requests, so the walk terminates cleanly at max_players.
std::vector<uint8_t> encode_player_sync_removal(uint8_t slot, bool with_ack = true);

// (tag=0x51 TEAM-CHANGE CONFIRM has no encoder: the original emits it ONLY for a pending
// g_team_change_entity_list entry [orig: NapiNPServerMsg_0x029 @0x514F10], with a real
// write_entity_packet @0x506bb0 record. The client FIELD-PARSES it — @0x431BB0 stamps team/NetId
// and REBINDS CharacterEntity — so an invented zero-id 0x51 re-binds the joiner's player to a
// vehicle archetype (the DBuggy1 shadow, D-NET-148). Add the faithful encoder with the
// team-change flow.)

// One 0x16 PLAYER-LIST entry (the host roster row the dispatcher extracts from the live connection list).
struct PlayerListEntry {
	uint8_t slot = 0;
	uint8_t team = 0;
};

// tag=0x16 PLAYER-LIST/SCOREBOARD — the inverse of decode_player_list. [orig:
// NetPacket_SerializeScoreboard0x16 @0x504b80 / client NapiNPClientMsg_PlayerList @0x42FAE0].
// The dispatcher builds `players` from the roster (the npruntime-side walk that can see
// NapiNPConnection); this serializes the witnessed wire shape: [u8 flags (bit0 team-mode, bit1
// timed-scores)][u8 rowCount] then per-player [u8 slot][u16 ping][u16 score][u16 deaths]
// [u8 (team<<1)|spectator], then [u8 team_count=2] + (team_count+1) × {u16 score, u16 deaths,
// u8 kothHold, u8 ctfFlag}, then [u8 inGameCount][u8 spectatorCount] — the HUD player count is
// acceptedRows − spectatorCount (D-NET-158).
std::vector<uint8_t> encode_player_list(const std::vector<PlayerListEntry> &players);

// tag=0x5A WEAPON-LOADOUT — the inverse of decode_weapon_loadout:
// `[u8 avatarClass]` then per slot `[u8 typeId][u8 ammoPrimary][u8 ammoSecondary][u8 ammoAlt]`,
// 0xFF-terminated. [orig: Server_SendWeaponSlotListToPlayer @ 0x502550 — walks the player's
// weapon-slot table (loaded from the accepted C2S 0x2F entries, AdmDef-index order) emitting one
// 4-byte group per slot; terminator @0x5028b5.]
std::vector<uint8_t> encode_weapon_loadout(const WeaponLoadout &loadout);

// C2S 0x2F LOADOUT SUBMIT — the inverse of decode_loadout_submit (§5.56):
// `[u8 team][u8 playerClass][u32 weaponSlotIndex]` then per kit row
// `[u8 admIndex][u8 ammoPrimary][u8 ammoSecondary][u8 variant]`, 0xFF-terminated.
// The caller owns row resolution (name → ADM index, unknown names skipped) AND the
// already-resolved slot value: retail's builder RE-RESOLVES the slot at send time
// against the local player's team side (side mask 2 for teams 1/3, 1 for 2/4, else 3)
// — keep the passed slot when its def's +128 mask matches @0x42ce5d, else the first
// side-legal def in the same 65-slot page @0x42ce63..0x42ce8b, else the raw argument
// (an empty/unbuilt slot table falls through raw — the golden slot-195 case). This
// encoder deliberately takes the resolved value instead of modelling that walk.
// [orig: NetPacket_SendLoadoutSubmit @ 0x42cdc0
// (ex-NetPacket_SendWeaponRestrictionMask misnomer) — team = byte_A85B48, the
// S2C 0x04 tail byte; playerClass = the profile's per-side class byte 5..9;
// rows from the 2048-B {name\0 ammoPri\0 ammoSec\0 flags\0}* kit tuple buffer
// via AvatarDef_FindIndexByName + atol, terminator @0x42d076.]
std::vector<uint8_t> encode_loadout_submit(const LoadoutSubmit &submit);

// S2C 0x49 WEAPON-RELOAD — [u16 entityHandle][u16 weaponSlotCombo] (4 B): the host's broadcast
// relay of a C2S 0x25 reload request (same payload, rebuilt per ADR 0003). The client-side apply
// (NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0 -> WeaponSlot_ReloadAmmo @0x541720) is the ONLY
// place a client's clip refills. The entry-time 0x80 phase bit is transient (§5.58, D-NET-142).
// [orig: NapiNPServerMsg_HandleReloadRequest @ 0x514DF0]
std::vector<uint8_t> encode_weapon_reload(const WeaponReload &reload);

// S2C 0x5D EMPTY-SLOT SWEEP — the inverse of decode_destroy_entity_list: a bare
// `[u16 pool0Index] × N` run with no count word. Retail's builder walks pool 0
// and appends the index of every entry whose occupancy dword is zero, then the
// handler ships it to the REQUESTER ONLY (send_mask 32, target = requester slot).
// [orig: NapiNPServerMsg_SendEmptySlots @ 0x51a600 -> the body builder @ 0x5160f0]
std::vector<uint8_t> encode_destroy_entity_list(const DestroyEntityList &list);

// S2C 0x50 TEAM ASSIGN — the inverse of decode_team_assign:
// `[u16 entityHandle][u8 team][u16 netId][u8 animSlot]` (6 B). The trailing pair is the
// entity's identity (entity+0x15C / entity+0x374), which retail's shared handle writer
// ZEROES for a non-player entity behind the `Flags & 0x100` gate @0x506b3d; the caller
// owns that gate. [orig: producer Server_ChangeEntityTeam @ 0x518D70 ->
//  write_entity_handle_packet @ 0x506ad0; client handler NapiNPClientMsg_0x050 @ 0x431910
//  stores them back @0x431b3a / @0x431b46]
std::vector<uint8_t> encode_team_assign(const TeamAssign &assign);

} // namespace opennova
