// The admission and join legs of the host's C2S dispatch (split off
// server_message_dispatch.cpp) -- see server_dispatch_join.h.
#include <runtime/inmatch/server_dispatch_join.h>
#include <runtime/inmatch/host_settings.h> // mission_session_title (the session title with no context)

#include <runtime/inmatch/server_join_check.h>    // the 0x00 JOIN's validation ladder
#include <net/npwire/nw_session_framing.h> // make_random_session_u32 (the 0x64 block's random regions)
#include <runtime/inmatch/server_spawn.h> // Server_ReservePlayerTeam (0x04/spawn identity)
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect
#include <runtime/inmatch/napi_np_server_ctx.h>  // ServerDispatchInputs::server_ctx

#include <base/gameprofile/game_type.h>       // is_waypoint_family / is_stock_coop (§5.32, D-NET-203/205)
#include <base/io/strutil.h>                  // iequals (the JOIN body's CD field)
#include <net/npwire/flat_tlv.h>        // the C2S 0x00 JOIN body walk (the CD identity blob)
#include <net/npwire/ingame_encode.h>   // encode_player_sync (the join's 0x46 push)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/session_vars.h>     // the 0x60 server-info stream
#include <net/npwire/replication_model.h> // PlayerReplicationState (POD) — the reply builders' input
#include <net/protocol/protocol.h> // protocol_in_match (the 0x64 rules word, DFX2-18)

#include <runtime/world/entity.h> // world::Entity / EntityHandle: the bound entity read through owned_entity
#include <runtime/world/world.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <base/io/le.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// A game-layer join-gate rejection: the empty-string description record with
// only the DPC reason set (DS=1 role, DC=2 class, DP1/DP2 0, DSTR/DDSTR "").
// [orig: Server_ValidatePlayerJoinRequest @0x512100 failures ride
// CNapiNPConnection_QueueDisconnectEvent @0x4c7ef0 with both strings g_EmptyStr ->
// NapiNPDataTransfer_SendDescription @0x628c80]
bool stage_join_gate_reject(NapiNPConnection &conn, uint32_t reason_dpc) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = reason_dpc;
	return Server_StageHostDisconnect(conn, event);
}

// An admission handler's reject: the connection's description queued (the first record wins,
// the description once), the handler returning with the net player's game state unchanged and
// the node kept in its stage; its description's sent byte then keeps every later record of the
// connection from its handler (the walks' per-record test, D-NET-824). False only when no
// description could be queued at all (a node with no remote transport), which the protocol owner
// releases (Rejected) (D-NET-821).
// [orig: CNapiNPConnection_QueueDisconnectEvent @0x4C7EF0 - the sent byte @0x4C7F58..0x4C7F5F,
//  DC 2 @0x4C7FAE, the first-wins slot @0x4C7FF2..0x4C8004; NapiNPServer_HandlePlayerJoinMessage
//  @0x512AA0 (the validator's reject returns @0x512EAC, game state 1 kept);
//  NapiNPServerMsg_HandleJoinRequest @0x512ED0 (the 17 called @0x512F7E, its return
//  @0x512F83..0x512F86, game state 2 kept); CNapiNPConnection_DispatchMessage @0x622570 (the sent
//  byte's test @0x622697 / @0x62269E)]
bool reject_join_record(NapiNPConnection &conn, uint32_t reason_dpc) {
	(void)stage_join_gate_reject(conn, reason_dpc);
	if (conn.host_disconnect_sent) return true;
	conn.admission_stage = GameAdmissionStage::Rejected;
	return false;
}

} // namespace

MissionMetadataBlob build_mission_metadata_blob(const GameConfig &config, bool mp_session_peer,
		const Protocol *title, const std::string &session_title) {
	MissionMetadataBlob blob{};
	auto write_u32 = [&](std::size_t offset, uint32_t value) {
		opennova::io::write_u32_le(blob.data() + offset, value);
	};
	auto write_fixed_string = [&](std::size_t offset, const std::string &value) {
		const std::size_t count = std::min<std::size_t>(value.size(), 31);
		std::memcpy(blob.data() + offset, value.data(), count);
	};
	for (std::size_t i = 0; i < 32; ++i) {
		blob[i] = static_cast<uint8_t>(make_random_session_u32());
		blob[148 + i] = static_cast<uint8_t>(make_random_session_u32());
	}
	uint32_t mission_session_id = 0;
	while (mission_session_id == 0)
		mission_session_id = make_random_session_u32() & 0xFFFFu;
	write_u32(32, mission_session_id);
	// +36: the slot limit less a non-peer (dedicated) host's own slot, at most 251: the
	// session's cap on every host; +48: the cfg's raw unlimited-vehicles word dword_24D2258.
	// [orig: Client_BuildMissionDataRequestBlock @0x51E897..0x51E8B9, @0x51E8C5..0x51E8CB]
	const int32_t players = static_cast<int32_t>(config.player_slot_limit()) - !mp_session_peer;
	write_u32(36, static_cast<uint32_t>(players > 251 ? 251 : players));
	write_u32(40, config.game_type);
	// A DFX2 authority ORs 0x10000 into the rules word [orig: @0x51b89e (dfx2.exe)].
	write_u32(44, config.mp_attributes | protocol_in_match(title).rules_word_bits);
	write_u32(48, static_cast<uint32_t>(config.unlimited_vehicles));
	write_fixed_string(52, config.server_name);
	write_fixed_string(84, config.mission_file);
	// +0x74 the session title, which is the map file unless the mount's override
	// table carries a [TITLE] entry for it (the 00TRg LAN oracle's two slots match).
	// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 - g_ServerNameStr into
	//  +0x34 @0x51E963, g_MapFileName into +0x54 @0x51E973, g_SessionTitle into +0x74
	//  @0x51E983, each Napi_CopyString(.., 32)]
	write_fixed_string(116, session_title);
	return blob;
}

namespace {

// ---------------------------------------------------------------------------
// Byte helpers. The §5.1
// identity bodies (0x7A PCID, 0x7B session info) are FAITHFUL ports of the witnessed serializers
// (NetPacket_WritePCID @0x5076e0, NapiNPMsg_0x7B_BuildPayload @0x507740; net-re §5.45, grilled
// 2026-06-27), and so are the 0x46 slot-state record (NetPacket_SerializePlayerSync0x46 @0x505e80,
// npwire encode_player_sync) and the 0x51 team-change confirm (NetPacket_WriteEntityPacket
// @0x506bb0): D-NET-127 closed, its last 0x46 fields with D-NET-295 (net-re §8).
// ---------------------------------------------------------------------------

void append_cstr(std::vector<uint8_t> &out, const std::string &s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

// The advertised mission's title arm, the session-variable stream's and the
// 0x7B summary's alike: the mission text's [info]/title, else (empty) the
// header's name. [orig: Game_SerializeMissionInfoToDataStream @0x523620
// @0x5237df..0x52380d; NapiNPMsg_0x7B_BuildPayload @0x507740 @0x507835..0x50784f]
const std::string &mission_title_or_header(const std::string &mission_text_title,
		const std::string &header_name) {
	return mission_text_title.empty() ? header_name : mission_text_title;
}

// The host context's resolved mission title; none on the World-less unit path.
const std::string &context_mission_text_title(const NapiNPServerCtx *ctx) {
	static const std::string kNone;
	return ctx != nullptr ? ctx->mission_text_title : kNone;
}

// The host context's session title (g_SessionTitle); with no context, the mission start's
// value with no text-override table: the map file's first 31 bytes (mission_session_title).
std::string context_session_title(const NapiNPServerCtx *ctx, const GameConfig &cfg) {
	return ctx != nullptr ? ctx->session_title : mission_session_title(nullptr, cfg.mission_file);
}

// ---------------------------------------------------------------------------
// §5.1 reply bodies — handshake + server-info + mission-metadata.
// ---------------------------------------------------------------------------

// tag=0x02, the join's padding probe: a file-transfer chunk of 512 bytes whose three words are
// the host's GetTickCount, g_FileTransferSessionId and the padding length 0x100 the client
// echoes, then `rand() >> 3` bytes from the process's CRT stream to the size. The client reads
// the three words (NapiNPClientMsg_HandleJoinResponse @0x42E0F0).
// [orig: CNetPlayer_SendFileTransferChunk @0x4C9440 - the id @0x4C9468, the size 0x200
//  @0x4C946E, the extra word 0x100 @0x4C9459 -> NetPacket_BuildPaddedTimestampPayload @0x502A40
//  - GetTickCount @0x502A4E into +0 @0x502A5F, the id into +4 @0x502A76, the extra word into +8
//  @0x502A82, the padding loop @0x502A93..0x502AAF (`rand() >> 3` @0x502A9D, its low byte
//  stored @0x502AAB)]
std::vector<uint8_t> build_tag02_push(uint32_t tick_count_ms, uint32_t file_transfer_session_id,
		io::CrtRand &crt) {
	constexpr size_t kProbeBytes = 0x200;
	constexpr uint32_t kPaddingLength = 0x100;
	std::vector<uint8_t> payload(kProbeBytes, 0);
	io::write_u32_le(payload.data(), tick_count_ms);
	io::write_u32_le(payload.data() + 4, file_transfer_session_id);
	io::write_u32_le(payload.data() + 8, kPaddingLength);
	for (size_t i = 12; i < kProbeBytes; ++i) payload[i] = static_cast<uint8_t>(crt.next() >> 3);
	return payload;
}

// The probe over the host context: GetTickCount (host_tick_count_ms, the process clock;
// D-NET-1024), the process's file-transfer session id as it stands at the send (read live, as
// the probe reads the global @0x4C9468; 0 with no process words) and the world's CRT stream (the
// one the session's draws share). With no context: zero words over a fresh stream.
std::vector<uint8_t> context_tag02_push(NapiNPServerCtx *ctx) {
	io::CrtRand fresh;
	if (ctx == nullptr) return build_tag02_push(0, 0, fresh);
	const uint32_t session_id =
			ctx->file_transfer_session != nullptr ? ctx->file_transfer_session->id : 0u;
	return build_tag02_push(host_tick_count_ms(*ctx), session_id,
			ctx->world != nullptr ? ctx->world->crt_rand : fresh);
}

// tag=0x7A: the recipient's own PCID (NOT the player name). [orig: NetPacket_WritePCID @0x5076e0 —
// copies player+0x250, the 0x002 handler's target player @0x5130c2]. Empty on a LAN host -> a
// single NUL (golden frame 134 = len 1, body 00).
std::vector<uint8_t> build_tag7a_pcid(const NapiNPConnection &conn) {
	std::vector<uint8_t> payload;
	append_cstr(payload, conn.account.pcid);
	return payload;
}

} // namespace

// tag=0x7B session/player info. [orig: NapiNPMsg_0x7B_BuildPayload @0x507740] field order:
// recipient player_name, PCID, server_name, advertised mission, map filename,
// gametype(u32), empty_str, expansion. Retail's field-4 source switches at
// @0x507822: waypoint-family modes use the active filename while other modes
// use the resolved MissionText title, else (empty) the header's name
// (@0x507835..0x50784f). The objective bit does not affect that selection.
// Field 5 is always g_MapFileName.
// (The PCID slot previously carried an invented "DEV-A02-0001" literal — D-NET-127; now sourced.)
std::vector<uint8_t> build_tag7b_session_summary(const GameConfig &cfg,
                                                 const std::string &mission_text_title,
                                                 const std::string &session_title,
                                                 const NapiNPConnection &conn) {
	std::vector<uint8_t> payload;
	const std::string &advertised_mission =
			game_type::is_waypoint_family(cfg.game_type)
					? session_title // [orig: g_SessionTitle @0x507824]
					: mission_title_or_header(mission_text_title, cfg.mission_name);
	append_cstr(payload, conn.player_name.empty() ? conn.reply.player_name
	                                             : conn.player_name); // recipient player_data+128
	append_cstr(payload, conn.account.pcid); // [orig: NapiNPMsg_0x7B_BuildPayload @0x5077ac, player+0x250] the recipient's PCID
	append_cstr(payload, cfg.server_name); // [orig: g_ServerNameStr @0x24D1FC4]
	append_cstr(payload, advertised_mission);
	append_cstr(payload, cfg.mission_file);// [orig: g_MapFileName @0x24D1F3E]
	io::append_u32_le(payload, cfg.game_type);  // [orig: g_GameType @0x24D2128]
	payload.push_back(0);                  // [orig: g_EmptyStr @0x7C08C6] empty C string
	append_cstr(payload, cfg.expansion);   // [orig: g_ExpansionName @0xB4C584]
	return payload;
}

namespace {

// The chunk header + at most 200 bytes of the stream from `offset`, shared by the
// 0x60 (server-info VarList) and 0x64 (mission block) transfers. Header dword 0 is
// the LIVE transfer identity, dword 1 the total length and dword 2 the clamped
// offset; an offset past the end yields an empty chunk. The client re-requests
// with [id][nextOffset] (C2S 0x33/0x37) and a mismatched id restarts at 0.
// [orig: NetPacket_WriteReplayDataBlock @0x5071F0 — copyLen clamp 200 @0x507255,
//  header @0x50727D..0x5072A0; NetPacket_WriteFixed180Block @0x507300 (180-byte
//  block, same 200 clamp); NapiNPServerMsg_HandleReplayRequest @0x515230 token
//  compare @0x51528B; NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0 @0x51533B]
constexpr uint32_t kTransferChunkBytes = 200;

std::vector<uint8_t> build_transfer_chunk(uint32_t transfer_id, const uint8_t *stream,
                                          uint32_t total, uint32_t offset) {
	const uint32_t clamped_offset = std::min(offset, total);
	const uint32_t copy_len = std::min(total - clamped_offset, kTransferChunkBytes);
	std::vector<uint8_t> reply(12, 0);
	io::write_u32_le(reply.data(), transfer_id);
	io::write_u32_le(reply.data() + 4, total);
	io::write_u32_le(reply.data() + 8, clamped_offset);
	reply.insert(reply.end(), stream + clamped_offset, stream + clamped_offset + copy_len);
	return reply;
}

// The [u32 id][u32 offset] body of a C2S 0x33/0x37 re-request. The retail
// handlers read both dwords and reset the offset to 0 when the token is not
// the live transfer id; a short body reads as {0, 0} (the first request is
// eight zero bytes). [orig: @0x515230 / @0x5152E0]
uint32_t chunk_request_offset(const std::vector<uint8_t> &payload, uint32_t live_transfer_id) {
	if (payload.size() < 8) return 0;
	const uint32_t token = static_cast<uint32_t>(payload[0]) |
			(static_cast<uint32_t>(payload[1]) << 8) |
			(static_cast<uint32_t>(payload[2]) << 16) |
			(static_cast<uint32_t>(payload[3]) << 24);
	const uint32_t offset = static_cast<uint32_t>(payload[4]) |
			(static_cast<uint32_t>(payload[5]) << 8) |
			(static_cast<uint32_t>(payload[6]) << 16) |
			(static_cast<uint32_t>(payload[7]) << 24);
	return token == live_transfer_id ? offset : 0u;
}

// tag=0x60 chunked server-info KV transfer: the authority's session-variable
// stream (host_session_vars). [orig: NapiNPClientMsg_HandleFileTransferChunk @0x432350;
// the stream Game_SerializeMissionInfoToDataStream @0x523620]
std::vector<uint8_t> build_tag60_server_info(const GameConfig &cfg,
                                             const std::string &mission_text_title,
                                             const std::string &session_title,
                                             uint32_t transfer_id, uint32_t offset) {
	const std::vector<uint8_t> info_body =
			encode_session_vars(host_session_vars(cfg, mission_text_title, session_title));
	return build_transfer_chunk(transfer_id, info_body.data(),
	                            static_cast<uint32_t>(info_body.size()), offset);
}

// tag=0x64 chunked mission-metadata transfer. [orig: NapiNPClientMsg_HandleMissionDataChunk (0x64) @0x432410]
std::vector<uint8_t> build_tag64_mission_metadata(
		const GameConfig &cfg, const MissionMetadataBlob *session_blob,
		uint32_t transfer_id, uint32_t offset) {
	MissionMetadataBlob fallback{};
	if (session_blob == nullptr) {
		fallback = build_mission_metadata_blob(cfg, !cfg.dedicated_server, nullptr,
				context_session_title(nullptr, cfg)); // no ctx
		session_blob = &fallback;
	}
	return build_transfer_chunk(transfer_id, session_blob->data(),
	                            static_cast<uint32_t>(session_blob->size()), offset);
}

// tag=0x04 join/slot-assignment body (24 B) [orig: NetPacket_WriteSlotAssignment @0x502b30, sent from
// CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]: [4x u32 netPlayer+60..72 stat dwords (0 on a fresh
// join)][u8 g_difficulty g_SessionDifficulty][u8 player_slot (slot+20)][u8 slot capacity @0x24c0ca4][u32 0]
// [u8 team (slot+416)]. The retail reader skips the four leading dwords and the
// pad and stores bytes 16/17/18/23; byte 16 lands in the joiner's own
// g_SessionDifficulty, the difficulty word its max-health and score tallies read.
// [orig: NapiNPClientMsg_SessionSlotConfig @0x425410 (@0x4254A8 byte 16);
//  Entity_GetMaxHealthWithDifficulty @0x43B8A0; Score_TallyKillByLocalPlayer @0x4FD160]
std::vector<uint8_t> build_tag04_slot_assignment(uint8_t difficulty, uint8_t player_slot,
                                                 uint8_t slot_capacity, uint8_t team) {
	std::vector<uint8_t> payload(24, 0);
	payload[16] = difficulty;    // [orig: g_SessionDifficulty's low byte, NetPacket_WriteSlotAssignment @0x502B96]
	payload[17] = player_slot;   // the joiner's assigned slot index [orig slot+20]
	payload[18] = slot_capacity; // player-slot array capacity [orig: g_PlayerSlotCapacity @0x24c0ca4]
	payload[23] = team;          // [orig slot+416]
	return payload;
}

// tag=0x02 GLB_JOIN admission boundary. [orig: NapiNPServerMsg_0x002 @0x512FD0 emits
// 0x01/0x7A/0x7B/0x03.] CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0 emits the later
// 0x03/settings/0x05/0x04/0x7B boundary via build_spawn_pump_metadata. Both bodies and the
// intervening packet boundary are witnessed by the PR #403 retail LAN capture:
//   0x00 (settings flag 0xA0) = CS-config update [orig: CNapiNPConnection_SendConfigUpdate @0x6286e0:
//        [u8 direction==0][u32 bit mask][u32 value per set bit]] — ours sets field idx 3 = 12 for both
//        directions, matching the golden.
//   0x03 = NetPacket_WriteSpawnQueueRecord @0x502ac0 (spawn_queue_record): [u8 1][u16 the
//        node's place on the spawn pump's pending list][u16 the list's count] while it is listed,
//        else [u8 0] (D-NET-1074).
//   0x05 = NetPacket_WriteBoolTrue @0x502c00: exactly {0x01}.
//   0x04 = NetPacket_WriteSlotAssignment @0x502b30 (build_tag04_slot_assignment above).]
// The 0x04 carries THIS connection's live slot + the host's slot capacity + the assigned team —
// byte 17 lands in g_LocalPlayerSlotId (the client's own slot id) and byte 18 in g_MaxPlayerSlots
// (g_MaxPlayerSlots, the 0x46/0x22 roster-walk terminator): the prior hardcoded (1, 2, 2)
// capped every client's walk at slot 1, so a third player's slot never bound (D-NET-158).
void append_connection_settings(const GameConfig &cfg,
                                std::vector<ProtocolMessage> &out) {
	const uint32_t configured_holdoff = cfg.effective_send_holdoff_ticks();
	const uint8_t holdoff = static_cast<uint8_t>(
			configured_holdoff < 255 ? configured_holdoff : 255);
	out.push_back(make_protocol_message(
			hightag::CS_CONFIG_UPDATE,
			{0, 0x08, 0, 0, 0, holdoff, 0, 0, 0},
			PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8));
	out.push_back(make_protocol_message(
			hightag::CS_CONFIG_UPDATE,
			{1, 0x08, 0, 0, 0, holdoff, 0, 0, 0},
			PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8));
}

void emit_admission_metadata(const GameConfig &cfg, NapiNPConnection &conn,
                             const NapiNPServerCtx *ctx, std::vector<ProtocolMessage> &out) {
	// Retail's 0x02 handler picks no slot row and assigns no team: it stamps the timing and
	// the holdoff, sends the 0x01 / 0x7A / 0x7B / 0x03 records and sets game state 7;
	// Server_PlayerAdd picks the row and assigns the team at the add, which the spawn pump's
	// 0x04 then sends. So a joiner the lineup queue admitted past the cap completes its
	// admission holding nothing, weighs on no other joiner's team, and the spawn pump holds
	// it (re-sending the 0x03) until the cap admits it (D-NET-714, D-NET-715).
	// [orig: the C2S 0x02 handler @0x512FD0 - NapiNPServer_UpdateHoldoffTicks @0x513068, the
	//  sends @0x5130AC..0x5131D5, game state 7 @0x51319C; Server_PlayerAdd's row pick
	//  (the PlayerSlot_PickFreeSlot call @0x51CC95), Server_AssignPlayerTeam @0x4FE310; CNapiServer_ProcessPendingPlayerSpawns's
	//  cap @0x4C8E13]
	// The both-direction send-holdoff dictation (H:0x00 mask 8 / CS field 3)
	// [orig: NapiNPServer_UpdateHoldoffTicks @0x4c5f40 sends @0x4c5fc0/@0x4c5fcb;
	// the golden NovaWorld capture carries 0x0C = 12 here]. OpenNova dictates
	// the resolved retail session period, unless the host supplied an explicit
	// GameConfig override (D-NET-197).
	const uint32_t configured_holdoff = cfg.effective_send_holdoff_ticks();
	append_connection_settings(cfg, out);
	conn.s2c_send_holdoff_ticks = clamp_send_holdoff_ticks(configured_holdoff);
	reset_s2c_send_holdoff_counter(conn);
	out.push_back(make_protocol_message(s2c::SYNC_STATE, {0x01, 0x00, 0x00, 0x00}));
	out.push_back(make_protocol_message(s2c::PLAYER_NAME, build_tag7a_pcid(conn)));
	out.push_back(make_protocol_message(
			s2c::FULL_PLAYER_INFO,
			build_tag7b_session_summary(cfg, context_mission_text_title(ctx),
					context_session_title(ctx, cfg), conn)));
	// The 0x03 follows the listing (complete_admission).
}

// The C2S 0x00 JOIN body's CD identity blob: the flat-TLV run is walked for
// the "CD" field, whose value packs [name\0][value\0] pairs back to back (the
// NovaWorld NAMEINFO / PCID / SQUADINFO / JOINTICKET cookies). A LAN join
// carries no CD and yields no pairs. Each CD the walk meets replaces the
// connection's cookie buffer (the previous one freed, a CD of no length leaving
// none), so the last CD is the cookie, however the walk's fields overlap.
// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0 - the "CD" compare
//  @0x512C45, the store @0x512C5D -> CNetPlayer_SetCookieBuffer @0x4C6060 (frees +0x2DC @0x4C6072, zeroes
//  +0x2DC / +0x2E0 @0x4C6080..0x4C6086, keeps none for a length below 1
//  @0x4C608E..0x4C6095, else a copy of the declared length); KeyValueBuffer_FindValue
//  @0x4C2A30]
std::vector<std::pair<std::string, std::string>> parse_join_identity_pairs(
		const std::vector<uint8_t> &payload, bool &cookie_present) {
	std::vector<std::pair<std::string, std::string>> pairs;
	const uint8_t *cookie = nullptr;
	size_t cookie_bytes = 0;
	size_t pos = 0;
	while (pos < payload.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(payload.data(), payload.size(), pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		pos = next;
		if (!strutil::iequals(field.name, "CD")) continue;
		// The cookie's bytes inside the JOIN: a CD that runs past them reads on in retail
		// (D-NET-410), and the pairs past them are not read here.
		cookie = field.size < 1 ? nullptr : field.value;
		cookie_bytes = field.size < 1 ? 0 : field.present;
	}
	cookie_present = cookie != nullptr;
	if (cookie != nullptr) {
		const uint8_t *cursor = cookie;
		const uint8_t *end = cookie + cookie_bytes;
		while (cursor < end) {
			const uint8_t *name_end = std::find(cursor, end, uint8_t{0});
			if (name_end == end) break;
			const uint8_t *value_end = std::find(name_end + 1, end, uint8_t{0});
			if (value_end == end) break;
			pairs.emplace_back(std::string(cursor, name_end), std::string(name_end + 1, value_end));
			cursor = value_end + 1;
		}
	}
	return pairs;
}

} // namespace

// The authority's session variables as its serializer writes them: the
// configured server name; session_mission_name (the session title for the
// stock Co-op selector, D-NET-964; objective/waypoint Co-op, bit 0x20000, and every
// other mode keep MissionText's display title, else the header's name); the game
// type; the custom text; the
// map file; score.ini's EXP_FANFARE lo/hi thresholds (the joiner arms its
// kill / headshot tones only when lo is nonzero and lo < hi, so {0,0}
// silences them).
// [orig: Game_SerializeMissionInfoToDataStream @0x523620 -- SERVERNAME
//  <- g_ServerNameStr @0x5236b1, MISSIONNAME the coop arm @0x523758 else
//  [info]/title @0x5237df, GAMETYPE @0x5238c7, CUSTOMTEXT @0x523914,
//  MISSIONFILENAME @0x5239a6, EXP_FANFARE word_24C1170 @0x523a39;
//  NapiNPClientMsg_ScoreDeltaSound @0x42A0B0]
std::string session_mission_name(uint32_t game_type, const std::string &session_title,
		const std::string &mission_text_title, const std::string &header_name) {
	return game_type::is_stock_coop(game_type)
			? session_title // [orig: g_SessionTitle @0x52375A]
			: mission_title_or_header(mission_text_title, header_name);
}

SessionVars host_session_vars(const GameConfig &cfg, const std::string &mission_text_title,
		const std::string &session_title) {
	SessionVars vars;
	vars.server_name = cfg.server_name;
	vars.mission_name = session_mission_name(
			cfg.game_type, session_title, mission_text_title, cfg.mission_name);
	vars.game_type = cfg.game_type;
	vars.custom_text = cfg.custom_text;
	vars.mission_file = cfg.mission_file;
	vars.exp_fanfare = cfg.exp_fanfare;
	return vars;
}

std::vector<ProtocolMessage> build_spawn_pump_metadata(
		const GameConfig &config, const std::string &mission_text_title,
		const std::string &session_title, NapiNPConnection &conn,
		const std::vector<NapiNPConnection> &roster, world::World *world,
		const HostRotation *rotation, const HostSlotRow *host_slot) {
	std::vector<ProtocolMessage> messages;
	const uint8_t capacity = static_cast<uint8_t>(
			config.total_player_slot_capacity());
	const std::optional<uint8_t> player_slot =
			Server_ReservePlayerSlot(roster, conn, capacity);
	if (!player_slot.has_value()) return messages;

	uint8_t team = 2; // only used by a World-less test seam
	if (world != nullptr) {
		const world::Entity *entity = conn.link.owned_entity.valid()
				? world->registry.get(conn.link.owned_entity)
				: nullptr;
		team = entity != nullptr
				? entity->team
				: Server_ReservePlayerTeam(
						config, /*is_in_session=*/true, roster, conn, rotation, host_slot);
	}

	// Exact CNapiServer_ProcessPendingPlayerSpawns ordering from the retail LAN
	// witness. 0x03 is the unlinked node's one byte (the pump unlinks it ahead of
	// this 0x03), distinct from the five-byte queue record of the C2S 0x02
	// handler and the hold (spawn_queue_record).
	messages.push_back(make_protocol_message(s2c::SYNC_TICK, {0x00}));
	append_connection_settings(config, messages);
	// A squad's first slot hears its own registry add after the add's send-state
	// reset and ahead of the 0x05 [orig: Server_PlayerAdd - CNetPlayer_ResetSendState
	//  @0x51CD6F, the 0x6A @0x51CEEF; the 0x05 @0x4C909D].
	if (!conn.squad_roster_add_staged.empty()) {
		messages.push_back(make_protocol_message(
				s2c::CLAN_ROSTER, std::move(conn.squad_roster_add_staged)));
		conn.squad_roster_add_staged.clear();
	}
	messages.push_back(make_protocol_message(s2c::GAME_START_SIGNAL, {0x01}));
	// config_bytes[6] is the session's g_SessionDifficulty low byte (the S2C 0x08
	// record's seventh byte) — the same word the 0x04 hands the joiner.
	messages.push_back(make_protocol_message(
			0x04, build_tag04_slot_assignment(
					config.config_bytes[6], *player_slot, capacity, team)));
	messages.push_back(make_protocol_message(
			s2c::FULL_PLAYER_INFO,
			build_tag7b_session_summary(config, mission_text_title, session_title, conn)));
	// A later add's 0x6A follows the block: retail's pump finishes this slot's
	// block and restarts its walk before the next add sends [orig:
	//  CNapiServer_ProcessPendingPlayerSpawns - the 0x7B @0x4C9111, goto LABEL_5
	//  @0x4C9128].
	for (std::vector<uint8_t> &body : conn.squad_roster_after_pump)
		messages.push_back(make_protocol_message(s2c::CLAN_ROSTER, std::move(body)));
	conn.squad_roster_after_pump.clear();
	return messages;
}

// See header.
ProtocolMessage admission_padding_probe(NapiNPConnection &conn, NapiNPServerCtx *ctx) {
	std::vector<uint8_t> challenge = context_tag02_push(ctx);
	conn.admission_stage = GameAdmissionStage::AwaitPaddingEcho;
	return make_protocol_message(s2c::JOIN_PADDING_PROBE, std::move(challenge));
}

// The C2S 0x02's handler, whatever the game state (no gate but the net player's): the holdoff
// dictation, the 0x01 / 0x7A / 0x7B / 0x03 records and game state 7, every time it runs. It
// lists the node on the spawn pump's pending list while it is unlisted (the pump unlinks it at
// the add), so a repeated 0x02 after the add lists it again and the pump adds the player a
// second time (Server_ProcessPendingPlayerSpawns: the first row orphaned). Its two echoed words
// are not compared with the probe.
// [orig: NapiNPServerMsg_0x002 @0x512FD0 - is_authority @0x512FFC, the net player @0x51300B, the
//  echoed tick and counter read @0x51302C / @0x513044 for NapiNPStats_RecordSample (called
//  @0x51305A), NapiNPServer_UpdateHoldoffTicks (called @0x513068), the sends @0x5130AC /
//  @0x5130E5 / @0x51311E, NapiNPPlayer_MoveNodeToEntityList (called @0x513193; it lists the node
//  while unlisted @0x4C7E65; the pump unlinks it @0x4C8FF6),
//  CNetPlayer_SetGameState(7) (called @0x51319C; leaving 10 zeroes the round watermark @0x4C4135), the
//  0x03 @0x5131D5] (D-NET-821)
static void complete_admission(const GameConfig &config, NapiNPConnection &conn,
		uint32_t now_tick, std::vector<NapiNPConnection> &roster,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies) {
	SessionReplyState &st = conn.reply;
	emit_admission_metadata(config, conn, inputs.server_ctx, replies);
	// Behind the 0x7B, a LAN-only NovaWorld host lists the joiner's reported address and port
	// (D-GAMECFG-8). [orig: NapiNPServerMsg_0x002 @0x513123..0x51318C]
	if (const NapiNPServerCtx *ctx = inputs.server_ctx;
			ctx != nullptr && lan_listing_address_owed(*ctx) && ctx->on_lan_listing_address)
		ctx->on_lan_listing_address(conn.novaworld_listing, conn.reported_ip, conn.reported_port);
	if (!st.admission_metadata_pushed) {
		st.admission_metadata_pushed = true;
		st.admission_completed_tick = now_tick;
	}
	if (conn.burst.game_state == 10) conn.link.round_watermark = 0;
	// Entering game state 7 from another state zeroes the net player's hold-nag stamp (+0x24),
	// so a spawn pump that holds the node nags it with its 0x03 at once (the stamp's zero test
	// @0x4C8F7A..0x4C8F7F); the setter returns early on an unchanged state, so a repeated 0x02 to
	// a node already in 7 (one the pump holds) keeps the stamp and the next nag still waits its
	// 1000 ms (@0x4C8F87..0x4C8F8D).
	// [orig: CNetPlayer_SetGameState @0x4C4060 - the unchanged-state return `cmp [esi+0A0h], ebp`
	//  @0x4C4074 / `jz` @0x4C407D, case 7 @0x4C4184]
	if (conn.burst.game_state != 7) st.admission_hold_nag_host_ms_valid = false;
	// Game state 7 leaves the NovaWorld ticket wait (states 3 / 4): the validation watchdog
	// visits those two alone and the enter result acts only in state 4, so a 0x02 in the wait
	// spawns the player with no ticket and a later result (either arm) does nothing.
	// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 - states 3 / 4 @0x4C8B7C / @0x4C8BD4;
	//  CNapiGameSession_HandlePlayEnterResponse @0x4D1940 - the state-4 gate @0x4D1B40]
	conn.join_ticket_validation = false;
	conn.player_enter_requested = false;
	conn.burst.game_state = 7;
	conn.admission_stage = GameAdmissionStage::Complete;
	// On the pending list: listed while unlisted (at its tail), so again after the pump's unlink.
	// [orig: NapiNPPlayer_MoveNodeToEntityList @0x4C7E60 - the list test @0x4C7E65]
	list_on_spawn_queue(roster, conn);
	// The 0x03 follows the listing, so it carries the node's place on the list and the list's
	// count, [1][1][1] for a lone joiner (D-NET-1074).
	// [orig: NetPacket_WriteSpawnQueueRecord called @0x5131B8, the 0x03 @0x5131D5]
	replies.push_back(make_protocol_message(s2c::SYNC_TICK, spawn_queue_record(roster, conn)));
}

// See header. Each admission record of the datagram reaches its handler in order, as retail's
// parse walk dispatches record by record, each handler with its own gate: the JOIN in game state
// 1, the form post in game state 2, the 0x02 in any, every game record behind the server slot
// the add sets (D-NET-821).
// [orig: CNapiNPConnection_ParseMessages @0x625BC0 (record by record); the server opcode table
//  registered by CNapiNetwork_Init @0x4CA4A0 (@0x4CA8E7: {id, name, handler, 0} rows)]
std::vector<ProtocolMessage> dispatch_admission(const GameConfig &config,
		NapiNPConnection &conn, const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick, std::vector<NapiNPConnection> &roster,
		const ServerDispatchInputs &inputs, size_t *consumed) {
	std::vector<ProtocolMessage> replies;
	size_t index = 0;
	for (; index < messages.size() && conn.admission_stage != GameAdmissionStage::Complete;
			++index) {
		const ProtocolMessage &message = messages[index];
		// No handler once the description went: a reject's description ends the handling of
		// every record behind it (D-NET-824) [orig: CNapiNPConnection_DispatchMessage
		//  @0x622570 - `cmp byte ptr [esi+70Dh], 0` @0x622697 / `jnz` @0x62269E].
		if (conn.host_disconnect_sent) continue;
		// Header-only ACKs and settings records are not gameplay records.
		if (message.flags.settings_update || message.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE)
			continue;
		switch (message.tag) {
			case 0x00:
				// The JOIN's handler acts only on a player in game state 1 (the 0x42 accepted)
				// with no server slot: out of turn it returns with nothing sent.
				// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0 - `cmp [ebx+0A0h], 1`
				//  @0x512AF9 / `jnz` @0x512B00 and `cmp [ebx+0C0h], ebp` @0x512B06 / `jnz`
				//  @0x512B0C to the return @0x512EAC]
				if (conn.admission_stage != GameAdmissionStage::AwaitJoinRequest) break;
				// The JOIN handler latches the spectator request before validation;
				// the local loopback bypasses the complete remote validator.
				// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512aa0;
				// Server_ValidatePlayerJoinRequest @0x5121ab..0x5121af -> @0x512a76 (conn+0x2E set = accept unvalidated)]
				// Its TLV walk keeps the CD cookie, whose PCID / SQUADINFO a remote
				// connection on a NovaWorld session decrypts before the validator.
				conn.join_identity_pairs = parse_join_identity_pairs(
						message.payload, conn.join_cookie_present);
				read_join_cd_key_ids(conn, message.payload);
				if (inputs.server_ctx != nullptr)
					(void)load_join_account_from_cookie(*inputs.server_ctx, conn);
				if (conn.type != NapiNPConnection::kTypeClientSide) {
					conn.link.spectator = conn.join_spectator_request != 0;
					const uint32_t reject_dpc = Server_ValidatePlayerJoinRequest(config,
							inputs.server_ctx, conn, roster, message.payload);
					if (reject_dpc != 0) {
						// The handler queues the description and returns, the net player
						// still in game state 1; no later record is handled (reject_join_record).
						if (!reject_join_record(conn, reject_dpc)) return {};
						break;
					}
				}
				replies.push_back(make_protocol_message(s2c::INIT, {}));
				conn.admission_stage = GameAdmissionStage::AwaitFormPost;
				break;

			case 0x01:
				// The form post's handler acts only on a player in game state 2 (the JOIN
				// accepted) with no server slot and reads no payload: out of turn it returns
				// with nothing sent.
				// [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0 - `cmp [edi+0A0h], 2`
				//  @0x512EF1 / `jnz` @0x512EF8 and `cmp [edi+0C0h], 0` @0x512EFE / `jnz`
				//  @0x512F05 to the return @0x512FC2, the handler taking the connection alone]
				if (conn.admission_stage != GameAdmissionStage::AwaitFormPost) break;
				// The side passwords' second test, whatever the team-choose rule (the 0x00's
				// validator tests them only under it): with either password set, a joiner that
				// is no spectator and whose JSP equals neither (case-insensitive; an empty JSP
				// equals an unset password) is rejected with reason 17 and gets no probe.
				// [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0 - the two lengths
				//  @0x512F0B..0x512F3B, the JSR test @0x512F3D, _stricmp(JSP, A) @0x512F52 and
				//  (JSP, B) @0x512F64, CNapiNPConnection_QueueDisconnectEvent(player, "", 17, "")
				//  @0x512F7E; NapiNetConfig_LoadFromConnTags @0x4C7260 (JSR @0x4C7607, JSP via
				//  NapiNetConfig_SetJsp @0x4C26B0)] (D-NET-819)
				if ((!config.side_a_password.empty() || !config.side_b_password.empty()) &&
						conn.join_spectator_request == 0 &&
						!strutil::iequals(conn.join_password, config.side_a_password) &&
						!strutil::iequals(conn.join_password, config.side_b_password)) {
					// The net player stays in game state 2; no later record is handled.
					if (!reject_join_record(conn, 17)) return {};
					break;
				}
				// State 3 under the ticket gate, which returns with no probe: the enter result's
				// success sends it (Server_ApplyPlayerEnterResult). Else state 6 and the probe
				// at once. [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0 - the gate, state 3
				//  and the return @0x512F87..0x512FA6; state 6 @0x512FB3,
				//  CNetPlayer_SendFileTransferChunk(0) called @0x512FBC]
				conn.join_ticket_validation =
						inputs.server_ctx != nullptr && join_tickets_required(*inputs.server_ctx, conn);
				if (conn.join_ticket_validation) {
					// State 3's entry stamps the net player's +0xA4 with GetTickCount, the
					// validation watchdog's deadline base (D-NET-1047).
					// [orig: CNetPlayer_SetGameState(3) called @0x512FA6 - GetTickCount
					//  @0x4C408A, the store @0x4C4156]
					conn.join_validated_host_ms = host_tick_count_ms(*inputs.server_ctx);
					conn.admission_stage = GameAdmissionStage::AwaitPlayEnter;
					break;
				}
				replies.push_back(admission_padding_probe(conn, inputs.server_ctx));
				break;

			case 0x02:
				// No game-state gate and no echo test: any 0x02 completes the admission, from
				// game state 1 too (ahead of the JOIN and its validator), as retail's does.
				complete_admission(config, conn, now_tick, roster, inputs, replies);
				break;

			default:
				// A game record ahead of the admission reaches its own handler, which returns
				// with nothing sent and nothing stored: every other handler of the server table
				// is a bare return or tests the net player's server slot (+0xC0) first, and only
				// the add sets the slot, after the admission. The node stays in its stage and the
				// walk goes on (D-NET-821; the per-handler gates in the L1-s13 audit).
				// [orig: the server table @0x82B5D8..0x82BA47 ({id, name, handler, 0} rows); e.g.
				//  NapiNPServerMsg_HandlePlayerSpawnRequest (0x0A) @0x513260 - the slot
				//  @0x513280..0x513288; NapiNPServerMsg_PlayerJoinRequest (0x0B) @0x51AB10 - the
				//  slot @0x51AB30..0x51AB38; NapiNPServerMsg_HandlePlayerLoadout (0x2F) @0x515790
				//  - the slot @0x5157CF..0x5157DB; the bare returns 0x07 @0x4FC970, 0x31 @0x5024A0, 0x35
				//  @0x500DF0, 0x36 @0x500E00, 0x38 @0x502510, 0x39 @0x500E20, 0x3E @0x500E10, 0x48
				//  @0x510F30; Server_PlayerAdd @0x51CBC0, the slot stores @0x51CC52 / @0x51CD51]
				break;
		}
	}
	if (consumed != nullptr) *consumed = index;
	return replies;
}

// See header. A 0x02 at a completed admission runs the handler again: the records re-sent and
// game state 7 (D-NET-821) [orig: NapiNPServerMsg_0x002 @0x512FD0].
void dispatch_join_padding_echo(const GameConfig &config, NapiNPConnection &conn,
		uint32_t now_tick, std::vector<NapiNPConnection> &roster,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies) {
	complete_admission(config, conn, now_tick, roster, inputs, replies);
}

// See header. [orig: NapiNPServerMsg_HandleReplayRequest (0x33) @0x515230]
void dispatch_file_chunk_request(const GameConfig &config, const ProtocolMessage &msg,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies) {
	// Between a map change's teardown and the next mission start the stream is empty: the
	// header alone, the offset clamped to 0 (NapiNPServerCtx::server_info_stream_emptied)
	// [orig: NetPacket_WriteReplayDataBlock @0x5071F0 - @0x50727F, @0x507221..0x50723F,
	//  @0x5072A2].
	if (inputs.server_ctx != nullptr && inputs.server_ctx->server_info_stream_emptied) {
		const std::vector<uint8_t> empty;
		replies.push_back(make_protocol_message(s2c::FILE_TRANSFER_CHUNK,
				build_transfer_chunk(inputs.server_info_transfer_id, empty.data(), 0,
						chunk_request_offset(msg.payload, inputs.server_info_transfer_id))));
		return;
	}
	replies.push_back(make_protocol_message(
			s2c::FILE_TRANSFER_CHUNK,
			build_tag60_server_info(
					config, context_mission_text_title(inputs.server_ctx),
					context_session_title(inputs.server_ctx, config),
					inputs.server_info_transfer_id,
					chunk_request_offset(msg.payload, inputs.server_info_transfer_id))));
}

// See header. The 0x64 chunk is the handler's one send: the joiner's first S2C 0x16 is the
// scoreboard clock's (emit_scoreboard_boundary), whose one serializer call is the
// periodic broadcast, so no list rides this reply (D-NET-504).
// [orig: NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0; NetPacket_SerializeScoreboard0x16
//  @0x504B80, its one call Server_BuildAndBroadcastScoreboard @0x50D960 (@0x50DDE2), whose one
//  caller is Server_TickUpdate (@0x51D90D) past g_ScoreboardBroadcastTimer's 0x136
//  (@0x51D8F9..0x51D917)]
void dispatch_mission_chunk_request(const GameConfig &config, const ProtocolMessage &msg,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies) {
	replies.push_back(make_protocol_message(
			s2c::MISSION_DATA_CHUNK,
			build_tag64_mission_metadata(
					config, inputs.mission_metadata_blob,
					inputs.mission_metadata_transfer_id,
					chunk_request_offset(msg.payload, inputs.mission_metadata_transfer_id))));
}

// See header. The net player's game-state test against 2 has no counterpart here: a
// connection reaches this switch only past its admission (dispatch_admission), and the
// handler needs the net player's slot, which the add links (player_slot_active).
// [orig: NapiNPServerMsg_HandlePlayerSyncRequest @0x513200 - the authority @0x513207, the net player
//  @0x51320D, its slot @0x513218, game state 9 @0x51322D, the slot-state test @0x513238,
//  the state-6 watermark clear @0x51323F, state 2 @0x513249]
void dispatch_player_sync_request(NapiNPConnection &conn) {
	if (!player_slot_active(conn)) return;
	// Leaving game state 10 or slot state 6 disarms the round-event watermark
	// [orig: CNetPlayer_SetGameState @0x4C4135].
	if (conn.burst.game_state == 10) conn.link.round_watermark = 0;
	// Its row's report clocks into phase 1, at the record's receive (D-NET-995)
	// [orig: CNetPlayer_SetGameState @0x4C4060 - @0x4C41BD / @0x4C41CA].
	gate_slot_clocks_game_state(conn, 9, conn.receive_stamp_ms, 0);
	conn.burst.game_state = 9;
	if (conn.slot_state == PlayerSlotState::PlayerSync) return;
	if (conn.slot_state == PlayerSlotState::Joined) conn.link.round_watermark = 0;
	conn.slot_state = PlayerSlotState::PlayerSync;
}

void broadcast_player_sync_on_join(const GameConfig &config,
                                   std::vector<NapiNPConnection> &roster,
                                   const NapiNPConnection &joined, const world::World *world) {
	// [orig: Server_PlayerAdd @0x51D296 — one 0x46 (fieldFlags 0x1CF7, no ack: the walk is the
	// JOINER's own concern) to every in-game connection]. Staged on each recipient's transport
	// like the other broadcast handlers (0x49 relay / the leave removal); the per-connection
	// flush frames it with that connection's own sequencing.
	// A bodyless row (its body did not spawn, D-NET-809) serializes the 0x8000
	// header alone [orig: NetPacket_SerializePlayerSync0x46 - the entity's null test @0x505EDC..0x505EDE,
	// 0x8000 @0x505ee0, the early return @0x505f37].
	const PlayerReplicationState rep = make_rep_state(config, joined, world);
	const std::vector<uint8_t> body = joined.link.owned_entity.valid()
			? encode_player_sync(rep, kPlayerSyncJoinBroadcastFields)
			: encode_player_sync_removal(static_cast<uint8_t>(joined.reply.player_slot),
					/*with_ack=*/false);
	for (NapiNPConnection &c : roster) {
		if (&c == &joined || !is_in_match(c) || c.link.transport == nullptr) continue;
		c.link.transport->host_send(s2c::PLAYER_SYNC, body);
	}
	// Retail's host-client reads the server's player-slot table in-process, so
	// no self-0x46 exists on its wire. Our host's client view is fed over the
	// type-2 loopback instead (ADR 0011 §3; the D-NET-114 self-stream), and it
	// never runs the joiner's C2S 0x22 walk — carry the loopback player's OWN
	// sync so the host-side roster (D-HUD-24) binds its slot and its 0x16 rows
	// are accepted rather than dropped as unknown.
	if (joined.type == NapiNPConnection::kTypeClientSide && joined.link.transport != nullptr)
		joined.link.transport->host_send(s2c::PLAYER_SYNC, body);
}

} // namespace opennova::inmatch
