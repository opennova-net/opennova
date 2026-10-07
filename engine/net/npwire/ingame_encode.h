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
// [orig: NetPacket_SerializeEntityPoolToPacket   @ 0x503460]  — S2C 0x20 bulk pool-3 sync.
// [orig: NetPacket_SerializeEntityPoolToPacket_0 @ 0x503940]  — S2C 0x0D pool spawn.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/replication_model.h> // PlayerReplicationState — the §5.1 reply encoders' host-side input

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
// [orig: NetPacket_SerializeEntityPoolToPacket @ 0x503460]:
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
// [orig: NetPacket_SerializeEntityPoolToPacket_0 @ 0x503940]. Header is `[u16 count]`
// (NO start_index — unlike 0x20), then per record `[u16 spawn_flags][u16 slot_id]
// [u16 item_type_id][cstr entity_name]` and the flag-gated body (entity_flags,
// always-pos, vel/section/orient/parent/target, the 0x400 mount-occupancy block, the
// always bone_byte (+290; team is the 0x0010-gated byte, D-NET-58), the 0x800 AI trailer, alert/action/sound_latch, the zone
// block, difficulty) — see decode_pool_spawn_batch for the exact field order.
//
// As with the pool-3 encoder, the spawn_flags word is DERIVED from the record
// (the original sets each value-gated bit inside `if (value) { … }`), so
// `PoolSpawnRecord::spawn_flags` on the input is ignored and recomputed:
//   0x0020 entity_flags!=0 · 0x0001/2/4 vel_{x,y,z}!=0 · 0x0008 section_mask!=0
//   0x0010 team_byte!=0 (entity+354; D-NET-58) · 0x0100 parent_handle!=0xFFFF · 0x0200 target_handle!=0xFFFF
//   0x0400 seat_mask!=0 · 0x0800 has_ai_trailer
//   0x0040 alert_byte!=0 · 0x0080 action_byte!=0 · 0x1000 has_sound_latch_byte
//   0x2000 has_zone_number_rank (writes zone_number_rank+zone_radius) ELSE 0x8000
//   has_zone_radius_alt (zone_radius) · 0x4000 has_difficulty_byte.
// Mount-occupancy block (D-NET-56): when 0x0400 is set, `mount_handle_8/9` are ALWAYS
// written after the per-set-bit handles. The original only sets 0x0400 when the
// mask is non-zero, so this encoder never emits the (0x400, mask==0) record.
//
// Boundary note: the original gates 0x0800 and 0x1000 on component pointers off the
// live engine entity (the AIData def's AI slot entity+104, the vehicle brain
// entity+100), 0x2000 on the zone number byte (entity+538), 0x8000 on the def's
// SpawnPoint attrib (itemDef+84 & 0x40000) and 0x4000 on the def's callbacks, not on
// the written values, so each rides an explicit presence field the host driver sets
// from those sources; 0x0400 reads itemDef+604 through the seat mask. The round-trip
// with decode_pool_spawn_batch is field-identical.
std::vector<uint8_t> encode_pool_spawn_batch(const PoolSpawnBatch &batch);

// Encode a §5.9 S2C 0x10 pool-2 static-entity batch — the inverse of
// decode_static_entity_batch and the bytes a host streams in phase 1 of the world-load
// sequence for purely-static structures (buildings, oil-field props). Header
// `[u16 start_index][u16 count]`, then per record `[u16 item_type_id]` (0 ⇒ empty-slot
// sentinel) else the flag-driven body. The field_flags word is DERIVED from populated
// fields (the original sets each gate bit inside `if (value) { flags |= bit; write }`), so
// `StaticEntityRecord::field_flags` on the input is ignored and recomputed; 0x0100 is the
// exception, riding `has_score_flag` because the original tests the def's callbacks.
// [orig: NetPacket_SerializePool2StaticToBuffer @0x5042f0 (write) / NapiNPClientMsg_0x010 @ 0x433400 (decode).]
std::vector<uint8_t> encode_static_entity_batch(const StaticEntityBatch &batch);

// Encode a §5.37 S2C 0x45 terrain-tile load chunk — the inverse of
// decode_terrain_load_batch and the bytes a host streams in phase 5 of the world-load
// sequence so a JOINER loads the mission's terrain-tile (.til) array. The first chunk carries
// the header (wire start word 0xFFFF + `'til0'` magic + total tile_count + hdr2/hdr3); every
// chunk then writes its `[start_index]..[end_index)` run of 12-B opaque tile entries. The full
// tile set is PAGED into ~650 B datagrams by the host emit loop (one TerrainLoadBatch per page).
// [orig: Terrain_SerializeTiles @ 0x6080F0 (write) / PolyTrn_LoadTileData @ 0x6081D0 (read) /
//  NapiNPClientMsg_0x045 @ 0x422890 (handler); D-NET-83.]
std::vector<uint8_t> encode_terrain_load_batch(const TerrainLoadBatch &batch);

// Encode a §5.23 S2C 0x0C organic-entity spawn batch — the inverse of
// decode_organic_spawn_batch and the bytes a host streams so a JOINER can name-match its
// own pool-0 player (the type-0x14b9 organic whose entity_name == the joiner's player name)
// and learn its wire handle. Every field after def_type is unconditional (no flag gates),
// so this is a straight field-order write. [orig: NapiNPClientMsg_0x00C @ 0x42E730.]
std::vector<uint8_t> encode_organic_spawn_batch(const OrganicSpawnBatch &batch);

// Encode a §5.46 S2C 0x18 FULL-ENTITY-SPAWN — the inverse of decode_full_entity_spawn
// and the host's reply to a C2S 0x0F entity-info query (the client's self-heal request
// for a stale/mismatched entity). Faithful port of the retail reply serializer
// [orig: NetPacket_SerializeObjectToBuffer @ 0x504d10, invoked by
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
// `authority_recipient` is the listen host's OWN player: retail's writer stops
// after the phase byte (plus the phase-0 block when flags2&3 == 0) — no
// server-status/env/gametype sub-block, no 7-byte tail, no mounted-ammo record,
// no records, no rounds, no terminator; the local client reads process memory
// [orig: NetPacket_WritePlayerState local gate @0x4ff9cd;
//  NetPacket_SerializeEntityStatesToPacket early return @0x50f07e].
std::vector<uint8_t> encode_frame_update(const FrameUpdate &fu,
                                         bool authority_recipient = false);

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
// round-trippable in the same lib (the encode side previously lived in runtime/inmatch/server_message_
// dispatch.cpp, decoupled from its decoder). The host-side SOURCE is the PlayerReplicationState reply
// POD; inmatch's reactive dispatcher fills it and calls these.
// ===========================================================================

// tag=0x46 PLAYER-SYNC — the inverse of decode_player_sync (PlayerSync). Flag-driven slot-state record:
// [u8 slot][u16 fieldFlags][u8 entitySlot] then the bit-gated fields in the witnessed order (name 0x1 /
// clan 0x2 / vehicle-name 0x10 / team 0x4 / class 0x8 / vehicle-score 0x20 / late-join 0x1000 / squad
// 0x40 / side 0x80 / quality 0x400 / account netId 0x800). [orig: NetPacket_SerializePlayerSync0x46
// @0x505e80; client NapiNPClientMsg_PlayerSync @0x431370]. Per-field slot-state modeling
// (score/squad/side/timer) is the remaining D-NET-127 nicety; the wire SHAPE is faithful and
// round-trips through decode_player_sync.
//
// `field_flags` is the REPLY mask, serialized verbatim and gating each field — the server answers
// EXACTLY the fieldFlags the C2S 0x22 requested, ack bit included [orig: @0x505f05 echoes 0x4000].
// Bit 0x4000 = the ROSTER-WALK ack: on receipt the client re-requests the NEXT slot (C2S 0x22 for
// slot+1, fieldFlags 0x5CF7) until slot >= max_players [orig: @0x431370 tail `if (slot < g_MaxPlayerSlots)
// QueueReliableMessage(0x22, slot+1)`; g_MaxPlayerSlots = the 0x04 slot-config maxPlayers byte]. Default
// 0x1CF7 = the join-broadcast field set [orig: Server_PlayerAdd @0x51d2bf `push 7415`].
std::vector<uint8_t> encode_player_sync(const PlayerReplicationState &ctx,
                                        uint16_t field_flags = kPlayerSyncJoinBroadcastFields);

// tag=0x46 PLAYER-SYNC REMOVAL — a 3-byte record [u8 slot][u16 flags] with the 0x8000 removal bit set
// (and 0x4000 ack to keep the walk going, matching golden's 0xC000). The client clears/unlinks that slot
// [orig: NapiNPClientMsg_PlayerSync @0x431370 `if (fieldBitmask & 0x8000) PlayerSlot_ClearAndUnlink`].
// Sent for empty roster slots the client's ack-walk requests, so the walk terminates cleanly at max_players.
std::vector<uint8_t> encode_player_sync_removal(uint8_t slot, bool with_ack = true);

// tag=0x51 TEAM-CHANGE CONFIRM — `[u16 index]` + the 0x50 body (8 B): the host's team-change
// list entry a C2S 0x29 asked for. The original emits it ONLY for a live g_TeamChangeEntityList
// entry [orig: NapiNPServerMsg_0x029 @0x514F10]; the client FIELD-PARSES it — @0x431BB0 stamps
// team/NetId and REBINDS CharacterEntity — so an invented zero-id 0x51 re-binds the joiner's
// player to a vehicle archetype (the DBuggy1 shadow, D-NET-148). The identity pair is zero for a
// non-player, the caller's Flags & 0x100 gate as for 0x50.
// [orig: NetPacket_WriteEntityPacket @0x506BB0 — the index @0x506BCE, the handle @0x506C10, the team
//  @0x506C24, the identity pair @0x506C26..0x506C5C (zero @0x506C7C..0x506C9D)]
std::vector<uint8_t> encode_team_change_confirm(uint16_t index, const TeamAssign &assign);

// One 0x16 PLAYER-LIST entry (the host roster row the dispatcher extracts from the live connection list).
struct PlayerListEntry {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint16_t status_flags = 0;
	uint16_t score1 = 0;
	uint16_t score2 = 0;
	bool spectator = false;
};

// Complete 0x16 snapshot. Keeping the team rows and trailer in the same value
// prevents the authoritative score from being lost between the match model and
// the serializer.
struct PlayerListFrame {
	uint8_t flags = 0x01;
	std::vector<PlayerListEntry> players;
	uint8_t team_count = 2;
	std::vector<PlayerListTeamRow> teams;
	uint8_t in_game_count = 0;
	uint8_t spectator_count = 0;
};

// tag=0x16 PLAYER-LIST/SCOREBOARD — the inverse of decode_player_list. [orig:
// NetPacket_SerializeScoreboard0x16 @0x504b80 / client NapiNPClientMsg_PlayerList @0x42FAE0].
// The dispatcher builds `players` from the roster (the inmatch-side walk that can see
// NapiNPConnection); this serializes the witnessed wire shape: [u8 flags (bit0 team-mode, bit1
// timed-scores)][u8 rowCount] then per-player [u8 slot][u16 statusFlags][u16 score1][u16 score2]
// [u8 (team<<1)|spectator], then [u8 team_count=2] + (team_count+1) × {u16 score1, u16 score2,
// u8 kothHold, u8 ctfFlag}, then [u8 inGameCount][u8 spectatorCount] — the HUD player count is
// acceptedRows − spectatorCount (D-NET-158).
std::vector<uint8_t> encode_player_list(const PlayerListFrame &frame);

// End-of-round wire transaction. Retail first sends the fixed seven-byte 0x1D
// header, then serves the frozen board through C2S 0x2B / S2C 0x56 chunks of at
// most 200 bytes. [orig: EndRoundScoreboard_SerializeHeader @0x505280;
// NapiNPServerMsg_HandleReplayDataRequest @0x514FE0; NetPacket_WriteReplayStreamChunk @0x506F60]
std::vector<uint8_t> encode_end_round_header(const EndRoundHeader &header,
		bool non_team_form);
std::vector<uint8_t> encode_end_round_stats(const EndRoundStats &stats);
std::vector<uint8_t> encode_end_round_stats_chunk(
		const std::vector<uint8_t> &board, uint16_t offset);
std::vector<uint8_t> encode_end_round_stats_request(uint16_t offset);

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

// S2C 0x35 WEAPON-PICKUP -- [u16 pickerHandle][u16 powerupHandle] (4 B), the
// inverse of decode_weapon_pickup. [orig: Server_BroadcastWeaponOverlayUpdate
// @0x509FC0 -- the picker's handle @0x50A054, the row's @0x50A06C, length 4
// @0x50A073]
std::vector<uint8_t> encode_weapon_pickup(const WeaponPickupNotice &notice);

// C2S 0x16 -- exact inverse of decode_mounted_weapon_slot_selection; retail's
// NetPacket_WriteBoolAsInt16 emits canonical 0 or 1 in a two-byte body.
std::vector<uint8_t> encode_mounted_weapon_slot_selection(
		const MountedWeaponSlotSelection &selection);

// C2S 0x03 — canonical i32 inverse-Auto-Medic preference (0 enabled, 1 disabled).
// C2S 0x2E medic request: the requester's own entity slot index as one dword.
// [orig: NetPacket_WriteEntityIndex32 from Input_HandleActionBinding case 217
// @0x49B4EB]
std::vector<uint8_t> encode_medic_request(const MedicRequest &request);

// S2C 0x14 chat broadcast, the inverse of decode_chat_broadcast:
// [u8 channel][u8 senderSlot][cstr text]. The host writer takes (senderSlot,
// channel, text) and stores the channel FIRST.
// [orig: NetPacket_WriteTwoBytesAndCString @0x5047A0 — byte2 @0x5047C1,
// byte1 @0x5047CE, the string copy @0x5047F9..0x50480A]
std::vector<uint8_t> encode_chat_broadcast(const ChatBroadcast &chat);

// S2C 0x28 dialog line, the inverse of decode_dialog_line: [cstr name][i16 line].
// [orig: sub_5038A0 @0x5038A0 — the strcpy @0x5038D1, the word @0x5038F4]
std::vector<uint8_t> encode_dialog_line(const DialogLine &line);

// S2C 0x32 formatted game text, the inverse of decode_formatted_game_text:
// [u8 subtype][cstr text], subtypes 1/2 then [u8 team]. Retail builds 1 inline
// in Server_PlayerAdd and 2 through NetPacket_SerializeMinimapSlot_0 — the
// same byte layout; 3/4/5 carry the text alone.
// [orig: Server_PlayerAdd @0x51d21e..0x51d277; NetPacket_SerializeMinimapSlot_0
//  @0x505a60 — the type byte @0x505a81, the name @0x505b6c..0x505b7a, the team
//  byte @0x505b80..0x505b95, the name-only type 5 @0x505ac9..0x505ada]
std::vector<uint8_t> encode_formatted_game_text(const FormattedGameText &text);

std::vector<uint8_t> encode_auto_medic_preference(
		const AutoMedicPreference &preference);

// S2C 0x23 — the host VM's replicated WAC command, the inverse of
// decode_script_remote_command: [u16 index] then the row's operands (Text/
// Filename cstr capped at 250 chars + NUL, Ssn u16, else u32). An operand the
// record does not carry is written as zero / empty.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 — index @0x4f5cf9, operand loop
//  @0x4f5d26..0x4f5dc2]
std::vector<uint8_t> encode_script_remote_command(const ScriptRemoteCommand &command);

// The two fixed player-death tail bodies emitted by GameEvent_PlayerDeath.
std::vector<uint8_t> encode_death_camera_target(
		const DeathCameraTarget &target);
std::vector<uint8_t> encode_player_downed_state(
		const PlayerDownedState &state);

// Fixed placed-device lifecycle bodies: S2C 0x59 is 32 bytes and S2C 0x12 is
// one packed handle. [orig: Entity_SpawnOrUpdateFromSlotPacket @0x546770;
// Server_RemoveEntityAndNotify @0x50A270]
std::vector<uint8_t> encode_deployed_item_spawn(const DeployedItemSpawn &spawn);
std::vector<uint8_t> encode_entity_remove(const EntityRemove &removal);

// S2C 0x3F — kind 0 is 14 bytes, kind 1 carries the key with its NUL.
// [orig: Server_BroadcastEntityActionPacket @0x5080D0]
std::vector<uint8_t> encode_objective_notification(const ObjectiveNotification &notice);

// S2C 0x2F — exact 19-byte objective/carryable state record.
// [orig: NetPacket_SerializeEntityWithParentAndTarget @0x505810]
std::vector<uint8_t> encode_objective_entity_state(
		const ObjectiveEntityState &state);

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
//  NetPacket_WriteEntityHandlePacket @ 0x506ad0; client handler NapiNPClientMsg_TeamAssign (0x50) @ 0x431910
//  stores them back @0x431b3a / @0x431b46]
std::vector<uint8_t> encode_team_assign(const TeamAssign &assign);

// §5.50 S2C 0x34 — positioned sound. The host fans a sound-profile NAME plus an
// optional world position; clients look the name up in their loaded profiles and
// play it in 3D. Byte layout, witnessed exactly: [u8 actionType][cstr name], and
// for actionType 1 three i16 world-unit coordinates (the engine's 16.16 fixed
// positions shifted down 16). Retail's writer copies the name from the referenced
// def's +4 field and appends the position triple only for type 1.
// [orig: NetPacket_WriteOverlayAction @0x505d50; fanned by
//  Server_SendOverlayActionToAlive @0x50a1b0 with send_mask 128 (alive players)]
std::vector<uint8_t> encode_play_sound(const PlaySoundCommand &cmd);

// S2C 0x6D, the inverse of decode_tracked_player_voice: [u8 event][u8 the
// caller's raw pool-0 index][i16 location-name index, -1 none] (4 B). The
// host packs one dword: the low byte the radio call, byte 1 the index, the
// high word the nearest location marker's +0x280 word.
// [orig: NapiNPServerMsg_HandleRadioCall @0x514330 — the byte @0x5143b2,
//  Pool_GetIndexFromPtr @0x5143c5, the 0xFFFF seed @0x51440f and the marker
//  word @0x514488..0x51448f, SendFiltered(0x6D, .., 4) @0x5144c8 / @0x51480e]
std::vector<uint8_t> encode_tracked_player_voice(const TrackedPlayerVoice &voice);

// S2C 0x6B, the inverse of decode_minimap_overlay_batch: [u8 count] then
// count x 12 B [u16 handle][s16 x][s16 y][s16 z][u16 seconds][u8 type]
// [u8 height]. The host fills each record from one live designation
// (world units = the Q16 value divided by 0x10000 toward zero, seconds = the
// remaining ticks / 62 toward zero); a batch with no record is not sent.
// [orig: NetPacket_SerializeDesignations @0x5116A0 (ex
//  NetPacket_SerializeWeaponOverlaySlots) — the stores @0x51171b..0x51178a,
//  the count byte @0x5117cf; Server_SendDesignationsToPlayer @0x517F70]
std::vector<uint8_t> encode_minimap_overlay_batch(const MinimapOverlayBatch &batch);

// The two water-crossing sets retail fans through 0x34, both witnessed in the
// Base Assault baseline capture (BODYWATER1 x24, SURFACE_WTR x15, all at the
// water plane z=12/13).
//
// WHICH set a crossing takes is not a family split: every caller passes the
// same two registry slots and chooses on the crossing entity's Flags & 0x2000
// (airborne). The 0x2000 arm reads g_SndBodyWater1 (@0x24E09B0), the slot the
// registry @0x82F590 binds to BODYWATER1 (row 33 @0x82FA34); the other arm
// reads g_SndSurfaceWtr (@0x24E09B4), SURFACE_WTR (row 32 @0x82FA10). JO:CA's
// sets agree: BODYWATER1 is a plunge and a splash, SURFACE_WTR a lap and a
// stroke. A swimmer's dive below the surface takes SURFACE_WTR outright.
// (D-SND-37: these were once named the other way round, from the capture's
// proximity: BODYWATER1 lay beside infantry, SURFACE_WTR beside vehicles.)
// [orig: Entity_UpdateInfantryPlayerBody @0x4b82e3..0x4b82f6, the dive
//  @0x4b8260; Entity_UpdateInfantryAI @0x4bfc24..0x4bfc39;
//  Entity_ProcessVehicleSuspension @0x464b16..0x464b2f; the fan is
//  Server_SendOverlayActionToAlive @0x50a1b0 (send_mask 128)]
inline constexpr char kWaterCrossAirborneEffect[] = "BODYWATER1"; // entered from the air
inline constexpr char kWaterCrossWadeEffect[] = "SURFACE_WTR";    // entered grounded


std::vector<uint8_t> encode_explosion_effect(const ExplosionEffectRecord &event);

// S2C 0x4E KILL-LIST PAGE — the inverse of decode_batch_kill: `[u16 resume][u16 slot]×N`.
// The host builder appends the page's slots after a reserved leading word and then
// stores the iterator's current slot (0xFFFF = exhausted) into it, so a walk that
// finds nothing is the bare `FF FF`. Sent reliable (msgClass 1), send_mask 160, to
// the requester, only when the body is non-empty (an exhausted-at-start walk or a
// suspended spawn phase yields nothing). [orig: Server_CollectValidWeaponSlots @0x516000
// (slot append @0x5160a9, resume store @0x5160d7);
// NapiNPServerMsg_HandleWeaponLoadoutRequest @0x51A550 (send @0x51a5f4)]
std::vector<uint8_t> encode_batch_kill(const BatchKillBatch &page);

// C2S 0x28 KILL-WINDOW REQUEST — the inverse of decode_burst_loadout_request (10 B):
// `[u32 windowMin][u32 windowMax][u16 start]`. [orig: the 0x0F reply-burst sender
// @0x42e5d3..0x42e5f7 and the 0x4E continuation @0x4318db..0x4318ff]
std::vector<uint8_t> encode_burst_loadout_request(const BurstLoadoutRequest &request);

// S2C 0x6A CLAN-ROSTER — the inverse of decode_clan_roster_update: `[u8 action]
// [u32 accountNetId]` and, for actions 1/3, `[cstr name][cstr tag]` (strlen+1 each,
// from the node's char[65] / char[9] buffers, so at most 64 / 8 chars). Any other
// action serializes to an EMPTY body: retail's serializer returns 0 and the caller
// does not send. [orig: NetPacket_SerializeMinimapSlot @0x5073B0 — type byte @0x5073dd,
//  id @0x50741a / @0x5073fb, name @0x507440, tag @0x507470, the 0-return @0x507408]
std::vector<uint8_t> encode_clan_roster_update(const ClanRosterUpdate &update);

// C2S 0x4E CLAN-ROSTER WALK — `[u32 afterNetId]`, the inverse of
// decode_clan_roster_walk_request. [orig: the kick @0x42e1c9..0x42e1d9 ({0});
//  the action-3 continuation @0x43265c..0x43266c]
std::vector<uint8_t> encode_clan_roster_walk_request(const ClanRosterWalkRequest &request);

// S2C 0x37 / C2S 0x1A DOOR-SLOT ACTION — `[u16 handle][u16 state][u8 number]` (5 B),
// the inverse of decode_door_slot_action for both directions. [orig: the inline
// authority writer @0x50fa15..0x50fa3a; NetPacket_WriteShortShortByte @0x42B2B0
// (the client request); NetPacket_WriteTwoShortsAndByte @0x505E00 (the host reply)]
std::vector<uint8_t> encode_door_slot_action(const DoorSlotAction &action);

// S2C 0x70 VEHICLE-SPAWN AVAILABILITY — the inverse of decode_vehicle_spawn_availability:
// the constant leading byte 3, one `[u16 typeId][u8 avail][u8 max]` per row, then the
// `u16 0` terminator. The avail/max ladder is the host's, computed from its EntityLimit
// table (see the decoder note); this encoder writes the rows handed to it.
// [orig: NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0 — 3 @0x5105c3, rows
//  @0x510660..0x510681, terminator @0x5106b8]
std::vector<uint8_t> encode_vehicle_spawn_availability(const VehicleSpawnAvailabilityList &list);

// C2S 0x40 VEHICLE-SPAWN REQUEST — `[u16 sourceHandle][u8 typeIndex]` (3 B), the inverse
// of decode_vehicle_spawn_request. The retail sender is the vehicle.mnu pick (unported);
// the layout is the host reader's. [orig: NapiNPServerMsg_HandleVehicleSpawnRequest
// @0x51C4C0 reads @0x51c515..0x51c52a]
std::vector<uint8_t> encode_vehicle_spawn_request(const VehicleSpawnRequest &request);

} // namespace opennova
