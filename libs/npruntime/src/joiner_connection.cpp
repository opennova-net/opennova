#include "npruntime/joiner_connection.h"

#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>
#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>
#include <novacrypto/crc32.h>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

// Verbatim port of novaworld::JoinerSession (the client mirror), with SCRK/seq/ack stored on the
// type-2 NapiNPConnection conn_. The D.0 name-match in on_server_session is copied byte-for-byte.
namespace opennova::np {

namespace {

// Pre-session Hello/Auth retransmit cadence. The golden captures prove these handshake packets
// retransmit at about one second; the exact retail handshake timer is still ungrilled. The
// connection template does not govern pre-session sends, and the 1000-ms CS value this previously
// cited belongs to the NOVAWORLDUDP service template (@0x4d3e60), so this stays a capture-pinned
// policy value until the handshake timer xref is witnessed.
constexpr uint64_t kHandshakeRetryMilliseconds = 1000;

// Established-session active-send probe interval: the JOINTOPERATIONS connection template's
// cs_dir0/cs_dir1.active_send_interval_ms (idle 30000). While reliable records remain retained, a
// sender with nothing else to send waits strictly more than this, then mints a header-only
// sequence so the peer's ordinary 0x44/0x84 machinery requests the lost semantic packet.
// [orig: CNapiNetwork_Init @0x4ca4a0 stores @0x4caac5/@0x4cab98 -> read by
// CNapiNPConnection_PumpSendIntervals @0x628FD0]
constexpr uint64_t kSessionActiveSendIntervalMilliseconds = 10000;

// The connection template's EMPTY send interval, the third leg of retail's send pump: with
// NOTHING queued to send and NOTHING retained, the pump still mints a packet once this
// elapses, so the peer's connection timeout never fires on a quiet client. This is the
// keepalive that carries a client parked at the deploy pick, killed, or simply idle — the
// peer reaps at cs_dir0.timeout_ms = 120000 from the same initializer, so a client without
// this leg is dropped after ~2 minutes of silence.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 — the empty_interval leg
//  @0x629041..0x629067 (`!static_payload_max && !retained && now - last_send > interval`
//  -> force_send -> BuildOutgoingPackets); the value is
//  cs_dir0/cs_dir1.idle_send_interval_ms = 30000, CNapiNetwork_Init @0x4ca4a0 stores
//  @0x4caab5/@0x4cab88; the reaping timeout_ms = 120000 is stored @0x4caa81/@0x4cab54]
constexpr uint64_t kSessionIdleSendIntervalMilliseconds = 30000;

uint64_t steady_milliseconds() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
	        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// C2S 0x02 echo of the post-auth join probe. Retail writes THREE leading dwords — the probe's two
// position values plus the client's net-frame counter — then random filler up to the requested
// padding_len clamped to [12, 512]: a negative request writes no filler, a request beyond 512
// clamps, and the three dwords always ship, so the echo is never empty and never exceeds one
// ordinary datagram. The retail server reads dwords 0 and 2 into its stats sampler, so the counter
// dword is consumed data, not filler. (Retail fills each pad byte with CRT rand()>>3; the CRT
// generator is a platform primitive, so the filler rides the session RNG instead.)
// [orig: NetPacket_WritePositionWithPadding @0x42A360 (clamp + three-dword prefix); caller
// NapiNPClientMsg_HandleJoinResponse @0x42e0f0 passes the @0xA822A4 net-frame counter; consumed
// by NapiNPServerMsg_0x002 @0x512fd0]
std::vector<uint8_t> build_join_padding_echo(
		const JoinPaddingProbe &probe, uint32_t net_frame_counter) {
	constexpr int32_t kRetailPaddingClampBytes = 512; // [orig: @0x42a360 clamp to 512]
	const int32_t requested = static_cast<int32_t>(probe.padding_len);
	const int32_t clamped =
			requested < 0 ? 0 : (requested > kRetailPaddingClampBytes ? kRetailPaddingClampBytes
	                                                                  : requested);
	const std::size_t body_len = clamped > 12 ? static_cast<std::size_t>(clamped) : 12u;

	std::vector<uint8_t> echo(body_len, 0);
	auto write_i32 = [&](std::size_t off, int32_t value) {
		const uint32_t v = static_cast<uint32_t>(value);
		echo[off + 0] = static_cast<uint8_t>(v);
		echo[off + 1] = static_cast<uint8_t>(v >> 8);
		echo[off + 2] = static_cast<uint8_t>(v >> 16);
		echo[off + 3] = static_cast<uint8_t>(v >> 24);
	};
	write_i32(0, probe.pos_x);
	write_i32(4, probe.pos_y);
	write_i32(8, static_cast<int32_t>(net_frame_counter));
	for (std::size_t off = 12; off < echo.size();) {
		const uint32_t random = make_random_session_u32();
		for (unsigned byte = 0; byte < 4 && off < echo.size(); ++byte, ++off)
			echo[off] = static_cast<uint8_t>(random >> (byte * 8));
	}
	return echo;
}

// Retail NapiNP_WriteClientAuthPayload @0x42a180 writes EXP when an expansion
// is active, followed by VERSIONCRCSTRING. Base-game captures therefore contain
// only the latter; expansion hosts require both.
std::vector<uint8_t> build_join_request(std::string_view expansion) {
	std::vector<uint8_t> body;
	auto append_string_tlv = [&](std::string_view name, std::string_view value) {
		body.insert(body.end(), name.begin(), name.end());
		body.push_back(0);
		const uint16_t size = static_cast<uint16_t>(value.size() + 1);
		body.push_back(static_cast<uint8_t>(size));
		body.push_back(static_cast<uint8_t>(size >> 8));
		body.insert(body.end(), value.begin(), value.end());
		body.push_back(0);
	};
	if (!expansion.empty()) append_string_tlv("EXP", expansion);
	// The active retail revx02 install has no loose expansion/<name>/version.txt,
	// so retail's CRC-32/MPEG-2 accumulator remains zero. Nonzero expansion
	// version checksums still need runtime resource-path plumbing.
	append_string_tlv("VERSIONCRCSTRING", "0");
	return body;
}

std::vector<uint8_t> le32_pair(uint32_t first, uint32_t second) {
	return {
			static_cast<uint8_t>(first),
			static_cast<uint8_t>(first >> 8),
			static_cast<uint8_t>(first >> 16),
			static_cast<uint8_t>(first >> 24),
			static_cast<uint8_t>(second),
			static_cast<uint8_t>(second >> 8),
			static_cast<uint8_t>(second >> 16),
			static_cast<uint8_t>(second >> 24),
	};
}

std::vector<uint8_t> le32_value(uint32_t value) {
	std::vector<uint8_t> body = le32_pair(value, 0);
	body.resize(4);
	return body;
}

// The golden joiner's profile kit (retail-lan-host-join-session): the capture default every
// headless caller keeps submitting. Rows carry the profile's "-1" ammo/flags strings (atol
// low byte -> 0xFF) [orig: the kit tuple buffer NetPacket_SendLoadoutSubmit @0x42cdc0 walks].
LoadoutSubmit build_retail_default_loadout_submit(uint8_t team, bool alternate) {
	LoadoutSubmit submit;
	submit.team = team;
	submit.player_class = 0x08;
	// First submission: the fixed Primary-key slot 195 (pre-Player_InitPlayer the slot table is
	// empty, so the builder's side-mask resolution returns the argument raw). Second: the live
	// g_currentWeaponSlot after the spawn fill — the golden kit's primary landed at category 3
	// rank 17 = 212. [orig: Game_StartMission @0x525836 (195) / @0x525c2e (g_currentWeaponSlot)]
	submit.weapon_slot_index = alternate ? 212u : 195u;
	for (uint8_t adm : {
			uint8_t{0x18}, uint8_t{0x03}, uint8_t{0x2C}, uint8_t{0x28},
			uint8_t{0x29}, uint8_t{0x2A}, uint8_t{0x02},
	}) {
		submit.entries.push_back(LoadoutSubmitEntry{adm, 0xFF, 0xFF, 0xFF});
	}
	return submit;
}

std::vector<uint8_t> build_retail_mission_status() {
	std::vector<uint8_t> body(13, 0);
	body[9] = 0x07;
	body[10] = 0x05;
	body[11] = 0x07;
	body[12] = 0x01;
	return body;
}

bool is_structural_cs_config_update(
		const ProtocolMessage &message, uint8_t &direction_out) {
	if (!message.flags.settings_update ||
	    message.full_tag != (PROTOCOL_FULL_TAG_HIGH_BASE | hightag::CS_CONFIG_UPDATE) ||
	    message.payload.size() < 5 || message.payload[0] > 1) {
		return false;
	}
	const uint32_t field_mask =
			static_cast<uint32_t>(message.payload[1]) |
			(static_cast<uint32_t>(message.payload[2]) << 8) |
			(static_cast<uint32_t>(message.payload[3]) << 16) |
			(static_cast<uint32_t>(message.payload[4]) << 24);
	if (field_mask == 0) return false;
	std::size_t value_count = 0;
	for (uint32_t remaining = field_mask; remaining != 0; remaining >>= 1)
		value_count += remaining & 1u;
	if (message.payload.size() != 5 + value_count * sizeof(uint32_t))
		return false;
	direction_out = message.payload[0];
	return true;
}

bool read_cs_config_field(const ProtocolMessage &message, uint8_t wanted_field,
		uint8_t &direction_out, uint32_t &value_out) {
	if (!is_structural_cs_config_update(message, direction_out) || wanted_field >= 32)
		return false;
	const uint32_t field_mask =
			static_cast<uint32_t>(message.payload[1]) |
			(static_cast<uint32_t>(message.payload[2]) << 8) |
			(static_cast<uint32_t>(message.payload[3]) << 16) |
			(static_cast<uint32_t>(message.payload[4]) << 24);
	if ((field_mask & (uint32_t{1} << wanted_field)) == 0) return false;
	std::size_t value_offset = 5;
	for (uint8_t field = 0; field < wanted_field; ++field) {
		if ((field_mask & (uint32_t{1} << field)) != 0)
			value_offset += sizeof(uint32_t);
	}
	value_out =
			static_cast<uint32_t>(message.payload[value_offset]) |
			(static_cast<uint32_t>(message.payload[value_offset + 1]) << 8) |
			(static_cast<uint32_t>(message.payload[value_offset + 2]) << 16) |
			(static_cast<uint32_t>(message.payload[value_offset + 3]) << 24);
	return true;
}

// ACCEPTED single-packet assumption: both directions must arrive in ONE deframed packet.
// Retail groups them into a single sequence (§5.0d frame 6), our host frames them the same
// way on the fresh, 0x42-retry, and 0x44/0x84 paths, and reconstruction always re-frames a
// sequence whole — so a split pair is out-of-contract for a JO host, not a loss mode this
// per-packet detector must survive.
bool has_initial_cs_config_pair(const std::vector<ProtocolMessage> &messages) {
	bool saw_direction[2] = {false, false};
	for (const ProtocolMessage &message : messages) {
		uint8_t direction = 0;
		if (is_structural_cs_config_update(message, direction))
			saw_direction[direction] = true;
	}
	return saw_direction[0] && saw_direction[1];
}

} // namespace

JoinerConnection::JoinerConnection(std::string player_name)
		: JoinerConnection(std::move(player_name), &steady_milliseconds) {}

JoinerConnection::JoinerConnection(std::string player_name,
		MonotonicMilliseconds monotonic_milliseconds)
		: player_name_(std::move(player_name)),
		  monotonic_milliseconds_(std::move(monotonic_milliseconds)) {
	if (!monotonic_milliseconds_) monotonic_milliseconds_ = &steady_milliseconds;
	// Stock-Avatars.def fallback for socket-free/headless callers. Retail initializes
	// each side from the first AvatarDefs entry of the matching alignment:
	// packed {nat=0,div=0,combo=1,good}=0x0200 and
	// {nat=7,div=0,combo=1,evil}=0x8207. The Godot binding replaces these from
	// the mounted Avatars.def before start(), so a user's real selection wins.
	// With JO's stock table, the two selected heads carry voice/avatar bytes 1/10.
	// The witnessed VCB=4 is a saved profile override, not the fresh default.
	// [orig: PlayerProfile_InitDefaults @0x54bbe0..0x54bc24 ->
	//  lookup_entity_slot_and_pack_entry @0x57ad40; wire: retail join f=199140]
	character_join_vars_.char_id[0] = 0x0200;
	character_join_vars_.char_id[1] = 0x8207;
	character_join_vars_.team_request = 0xFF;
	character_join_vars_.char_class[0] = 8;
	character_join_vars_.char_class[1] = 8;
	character_join_vars_.avatar[0] = 1;
	character_join_vars_.avatar[1] = 10;
	conn_.type = 2;                  // client-side connection (the joiner's view of the host)
	conn_.player_name = player_name_;
}

std::vector<uint8_t> JoinerConnection::start() {
	conn_.client_scrk = make_dev_scrk();
	// Fresh per-connection numeric key, like the per-connection key material
	// retail regenerates on connect [orig: CNapiNPConnection_GenerateTxKey
	// @ 0x61dfe0 charset-PRNG pattern]: a CONSTANT ck would alias a quick
	// reconnect from the same endpoint to the host's same-(CI,CK) retransmit
	// echo (stale auth material) instead of a fresh node. CI stays 1 — the
	// first connection node's index. 0 is reserved (absent-key sentinel).
	client_key_ = make_random_session_u32();
	if (client_key_ == 0) client_key_ = 1;
	server_hk_ = 0;
	conn_.server_sk = 0;
	conn_.server_scrk.clear();
	conn_.seq = make_jo_game_session_sequencing();
	handshake_retry_clock_armed_ = false;
	handshake_last_send_ms_ = 0;
	post_auth_stage_ = PostAuthStage::Inactive;
	goodbye_sent_ = false;
	session_last_send_ms_ = 0;
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	pending_spawn_menu_request_ = false;
	empty_slot_sweep_requested_ = false;
	preload_ready_ = false;
	sync_tail_seen_ = false;
	player_list_seen_ = false;
	deployment_policy_seen_ = false;
	deployment_pick_required_ = false;
	initial_loadout_grant_count_ = 0;
	deployment_pick_sent_ = false;
	deployment_pick_sequence_ = 0;
	deployment_reply_seen_ = false;
	has_self_handle_ = false;
	self_handle_ = 0;
	spawn_ = SelfSpawn{};
	assigned_team_ = 0; // re-latched from the fresh session's S2C 0x04 (the kit seam persists);
	                    // zero like retail's byte_A85B48 until the assignment arrives
	current_player_class_ = 0;
	mission_known_ = false;
	server_name_.clear();
	mission_name_.clear();
	map_file_.clear();
	expansion_.clear();
	advertised_expansion_.clear();
	game_type_ = 0;
	last_error_.clear();
	host_disconnect_reason_.clear();
	phase_ = Phase::Hello;
	// Arm the receive clock at connect: the reap window is measured from the moment
	// this connection started expecting traffic, not from the first reply.
	last_receive_ms_ = monotonic_milliseconds_();
	receive_clock_armed_ = true;
	handshake_retry_datagram_ = build_client_hello();
	handshake_last_send_ms_ = monotonic_milliseconds_();
	handshake_retry_clock_armed_ = true;
	return handshake_retry_datagram_;
}

std::vector<uint8_t> JoinerConnection::build_client_hello() {
	// The browse and connect legs use the same retail JO identity. The callsign
	// belongs in ClientAuth.NA (not CO) and is later echoed into the organic-spawn
	// name-match record by the host.
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
	                          client_hello_to_bytes(make_jointoperations_client_hello(client_index_)));
}

std::vector<uint8_t> JoinerConnection::build_client_auth() {
	ClientAuth auth = make_jointoperations_client_auth(
			client_index_, client_key_, server_hk_, player_name_, conn_.client_scrk);
	// The GAME-session 0x42's NA TLV is the player CALLSIGN — the retail host's display-name
	// source (the golden joiner's 0x0C record name equals its NA). The "jop:cus2" gate tag is
	// the NOVAWORLD-gate connect's NA, not the game join's. [wire: retail-ashi5a f=199140
	// na="FooPlayer" / retail_join_v18 f=47676 na="TestPlayer"; net-re §5.0b]
	// Retail's game ClientAuth always uploads this profile-INDEPENDENT environment block. A stock
	// host folds it into NapiNetConfig before handling C2S JOIN; omitting it fails the very first
	// Server_ValidatePlayerJoinRequest checks and produces H:0x03 DS=1,DC=2. The validated four are
	// LITERAL constants of the 1.7.5.7 binary, not capture-specific values (witness 2026-07-24):
	// BN==1 (DC=2 @0x512155), VN==2 (DC=3), MBN==20042002 (DC=4; the 0x131D112 immediate),
	// SOPD==180 (DC=8) — any client of this patch must send exactly these; do not "unpin" them.
	// BT is the account ban state (1/2 reject, DC=6/7; 0 for LAN). The rest are stored/display
	// only (@0x4c7260 SetVersionString/SetCountryCode/TZB): retail derives VERSIONSTRING from its
	// exe resource and COUNTRYCODE/TZB from the OS locale — deriving ours is a fidelity follow-up.
	// [wire: retail-lan-host-join-session ClientAuth; orig: NapiNetConfig_LoadFromConnTags
	// @0x4c7260 -> Server_ValidatePlayerJoinRequest @0x512100]
	for (const auto &field : {
			std::pair{"BT", "0"},
			std::pair{"VN", "2"},
			std::pair{"BN", "1"},
			std::pair{"DB", "0"},
			std::pair{"MBN", "20042002"},
			std::pair{"SOPD", "180"},
			std::pair{"VERSIONSTRING", "V1.7.5.7"},
			std::pair{"COUNTRYCODE", "us"},
	}) {
		auth.cu.push_back(make_client_cu_chunk(2, field.first, field.second));
	}
	// Retail serializes the live profile's per-side character block between the
	// environment strings and the trailing locale/packet scalars. A zero value is
	// absent, matching CNapiServerInfo's per-field guards. TR uses signed decimal:
	// 0xFF is the profile's -1/automatic-team sentinel.
	// [orig: CNapiServerInfo_SerializeToSession @0x4c3a1b..0x4c3c4c]
	auto append_profile_value = [&](const char *name, int value, bool present) {
		if (!present) return;
		auth.cu.push_back(make_client_cu_chunk(
				2, name, std::to_string(value)));
	};
	append_profile_value("CI0", character_join_vars_.char_id[0],
			character_join_vars_.char_id[0] != 0);
	append_profile_value("CI1", character_join_vars_.char_id[1],
			character_join_vars_.char_id[1] != 0);
	append_profile_value("TR",
			character_join_vars_.team_request == 0xFF
					? -1
					: character_join_vars_.team_request,
			character_join_vars_.team_request != 0);
	append_profile_value("CTA", character_join_vars_.char_class[0],
			character_join_vars_.char_class[0] != 0);
	append_profile_value("CTB", character_join_vars_.char_class[1],
			character_join_vars_.char_class[1] != 0);
	append_profile_value("VCA", character_join_vars_.avatar[0],
			character_join_vars_.avatar[0] != 0);
	append_profile_value("VCB", character_join_vars_.avatar[1],
			character_join_vars_.avatar[1] != 0);
	for (const auto &field : {
			std::pair{"TZB", "300"},
			std::pair{"MPS", "1300"},
	}) {
		auth.cu.push_back(make_client_cu_chunk(2, field.first, field.second));
	}
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
}

std::vector<uint8_t> JoinerConnection::frame_session(const std::vector<ProtocolMessage> &messages) {
	// Joiner C2S direction: encrypt with our client_scrk, stamp session_id = the server's SK
	// (ServerAuth.sk, the peer's local_key). Shared seq/ack framing (ADR 0013).
	std::vector<uint8_t> body;
	if (!frame_session_packet(conn_.seq, SessionCrypto{conn_.client_scrk, {}, conn_.server_sk}, messages,
	                          body)) {
		return {};
	}
	session_last_send_ms_ = monotonic_milliseconds_();
	session_send_clock_armed_ = true;
	session_ack_pending_ = false; // every sequenced C2S packet carries the latest S2C ACK
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

std::vector<std::vector<uint8_t>> JoinerConnection::frame_messages(
		const std::vector<ProtocolMessage> &messages,
		std::size_t max_packet_body_bytes) {
	std::vector<std::vector<uint8_t>> out;
	if (messages.empty() || max_packet_body_bytes <= PROTOCOL_PACKET_HEADER_SIZE) return out;
	std::vector<ProtocolMessage> packet;
	std::size_t packet_bytes = PROTOCOL_PACKET_HEADER_SIZE;
	auto flush = [&] {
		if (packet.empty()) return;
		std::vector<uint8_t> datagram = frame_session(packet);
		if (!datagram.empty()) out.push_back(std::move(datagram));
		packet.clear();
		packet_bytes = PROTOCOL_PACKET_HEADER_SIZE;
	};
	for (const ProtocolMessage &message : messages) {
		std::vector<uint8_t> encoded;
		if (!encode_protocol_messages({message}, encoded)) continue;
		if (!packet.empty() && packet_bytes + encoded.size() > max_packet_body_bytes)
			flush();
		// A single oversized semantic record is left intact. Fragmentation belongs at the
		// ProtocolMessage producer seam; splitting its payload here would change its flags.
		packet.push_back(message);
		packet_bytes += encoded.size();
	}
	flush();
	return out;
}

ProtocolMessage JoinerConnection::make_loaded_model_page_reply(
		uint32_t start_index) const {
	std::vector<uint8_t> page = le32_value(start_index);
	if (start_index < loaded_model_challenge_snapshot_.size()) {
		const std::size_t end = std::min<std::size_t>(
				loaded_model_challenge_snapshot_.size(),
				static_cast<std::size_t>(start_index) + 50);
		page.reserve(4 + (end - start_index) * sizeof(uint32_t));
		for (std::size_t i = start_index; i < end; ++i) {
			const uint32_t index = loaded_model_challenge_snapshot_[i];
			for (int shift = 0; shift < 32; shift += 8)
				page.push_back(
						static_cast<uint8_t>((index >> shift) & 0xFFu));
		}
	}
	return make_protocol_message(c2s::LOADED_MODEL_PAGE_REPLY, std::move(page));
}

std::vector<uint8_t> JoinerConnection::frame_retained_session(uint32_t sequence) {
	std::vector<uint8_t> body;
	if (!frame_session_packet_for_sequence(
			conn_.seq,
			SessionCrypto{conn_.client_scrk, {}, conn_.server_sk},
			sequence, body)) {
		return {};
	}
	session_ack_pending_ = false;
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

JoinerConnection::PollResult JoinerConnection::handle_datagram(const uint8_t *raw, std::size_t len) {
	PollResult out;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(raw, len, opcode, body)) {
		return out; // bad envelope — drop quietly, don't kill the session
	}
	// Receive activity for the connection reap clock: retail's transport stamps it
	// for any datagram that passes the envelope check, and reaps the peer after
	// cs_dir0.timeout_ms of silence. [orig: CNapiNetwork_Init @0x4ca4a0 stores
	// timeout_ms = 120000 @0x4caa81/@0x4cab54 -> CNapiNetwork_OnDisconnectedFromServer
	// @0x4c63d0]
	last_receive_ms_ = monotonic_milliseconds_();
	receive_clock_armed_ = true;
	switch (opcode) {
	case SESSION_OPCODE_SERVER_HELLO:
		if (phase_ == Phase::Hello) on_server_hello(body, out);
		break;
	case SESSION_OPCODE_SERVER_AUTH:
		if (phase_ == Phase::Auth) on_server_auth(body, out);
		break;
	case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
		if (phase_ == Phase::Driving || phase_ == Phase::InMatch) on_server_session(body, out);
		break;
	case SESSION_OPCODE_SERVER_RESEND_LIST:
		if (phase_ == Phase::Driving || phase_ == Phase::InMatch)
			on_server_resend_list(body, out);
		break;
	default:
		break; // server-only / unexpected opcodes are non-fatal
	}
	return out;
}

void JoinerConnection::on_server_hello(const std::vector<uint8_t> &body, PollResult &out) {
	ServerHello sh;
	if (!parse_server_hello(body.data(), body.size(), sh)) {
		fail("bad ServerHello");
		return;
	}
	// Correlate the reply with the outstanding ClientHello before accepting host material. A
	// phase-correct stale/spoofed 0x81 from another handshake must not advance this connection.
	if (sh.ci != client_index_) return;
	server_hk_ = sh.hk;          // echo this in ClientAuth.hk
	// The host's ADVERTISED expansion: learned here and echoed in the C2S JOIN EXP TLV below,
	// nothing more (this member has no accessor). The value the resource-root preload
	// reconciles against before the world loads is the AUTHORITATIVE S2C 0x7B expansion
	// (expansion_, surfaced by ClientRuntime::expansion()) (D-NET-178).
	advertised_expansion_ = sh.sus2;
	phase_ = Phase::Auth;
	handshake_retry_datagram_ = build_client_auth();
	handshake_last_send_ms_ = monotonic_milliseconds_();
	handshake_retry_clock_armed_ = true;
	out.outbound.push_back(handshake_retry_datagram_);
}

void JoinerConnection::on_server_auth(
		const std::vector<uint8_t> &body, PollResult & /*out*/) {
	ServerAuth sa;
	if (!parse_server_auth(body.data(), body.size(), sa)) {
		fail("bad ServerAuth");
		return;
	}
	// ServerSessionInit echoes both client identifiers. Correlate before even
	// accepting its result code: a rejection addressed to another in-flight
	// handshake is not this connection's failure.
	if (sa.ci != client_index_ || sa.ck != client_key_) return;
	if (sa.cr != 1) {
		fail("ServerAuth rejected (cr != 1)");
		return;
	}
	conn_.server_sk = sa.sk;      // session_id for our outbound 0x43s
	conn_.server_scrk = sa.scrk;  // decrypts inbound 0x83 inner streams
	conn_.connection_id = sa.mi;  // [orig: 0x82 MI TLV = the host-assigned dcb/ConnectionId; the
	                              // client stores it on its own NapiNPConnection (+0x18,
	                              // NapiNP_GetLocalConnectionId @0x4c6d40) and echoes it in the 0x48
	                              // client-ack so the host stamps it into our 0x0C ownerConnectionId]
	phase_ = Phase::Driving;
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	session_ack_pending_ = false;
	// Retail first sends a sequenced settings packet after 0x82. Its receipt drives the header-only
	// ACK + C2S 0x00 JOIN pair in on_server_session; do not skip straight to 0x01. This remains active
	// while the binding holds world_ready_ false so a joiner can discover what it must load.
	post_auth_stage_ = PostAuthStage::AwaitServerSettings;
}

void JoinerConnection::on_server_session(const std::vector<uint8_t> &body, PollResult &out) {
	// Joiner recv: decrypt inbound 0x83 with the server's SCRK; deframe latches conn_.seq.last_inbound_seq.
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	SessionDeframeAdmission admission;
	if (!deframe_session_packet(
			conn_.seq, SessionCrypto{{}, conn_.server_scrk, 0, client_key_}, body.data(),
	                            body.size(), hdr, messages, &admission)) {
		return; // lenient: an in-match streaming packet we can't parse is not fatal
	}
	if (admission.admitted) {
		acknowledge_session_packets(conn_.seq, admission.max_ack_count);
	}
	bool received_settings = false;
	for (const SessionDeframeAdmission::Packet &packet : admission.packets) {
		if (has_initial_cs_config_pair(packet.messages)) {
			received_settings = true;
			break;
		}
	}
	std::vector<uint32_t> containing_packet_ack;
	containing_packet_ack.reserve(messages.size());
	for (const SessionDeframeAdmission::Packet &packet : admission.packets) {
		for (std::size_t i = 0; i < packet.messages.size(); ++i)
			containing_packet_ack.push_back(packet.header.ack_count);
	}
	if (containing_packet_ack.size() != messages.size())
		containing_packet_ack.assign(messages.size(), hdr.ack_count);
	std::vector<ProtocolMessage> periodic_replies;
	// The fixed 0x0F header carries gameFlags at byte 22. Bit 0 says the host
	// has spawn zones and will keep this player hidden until C2S 0x0E. Keep an
	// explicit unknown state because OpenNova and retail may send the initial
	// 0x5A grants before 0x0F, either in this packet or an earlier packet.
	for (const ProtocolMessage &m : messages) {
		uint8_t settings_direction = 0;
		uint32_t holdoff = 0;
		if (read_cs_config_field(m, 3, settings_direction, holdoff) &&
		    settings_direction == 1) {
			out.send_holdoff_set = true;
			out.send_holdoff = holdoff;
		}
		if (!m.flags.settings_update && m.full_tag < PROTOCOL_FULL_TAG_HIGH_BASE && m.tag == s2c::WEAPON_LOADOUT) {
			WeaponLoadout loadout;
			if (decode_weapon_loadout(
					m.payload.data(), m.payload.size(), loadout)) {
				out.loadout_grants.push_back(std::move(loadout));
			}
		}
		if (!m.flags.settings_update && m.full_tag < PROTOCOL_FULL_TAG_HIGH_BASE &&
		    m.tag == s2c::WORLD_STATE_LOAD && m.payload.size() > 22) {
			deployment_policy_seen_ = true;
			deployment_pick_required_ = (m.payload[22] & 0x01u) != 0;
		}
	}
	auto release_deployment = [&] {
		deployment_reply_seen_ = true;
		if (has_self_handle_ && phase_ != Phase::InMatch) {
			phase_ = Phase::InMatch;
			post_auth_stage_ = PostAuthStage::Complete;
			out.reached_in_match = true;
		}
	};
	auto enter_initial_sync_tail = [&] {
		post_auth_stage_ = PostAuthStage::AwaitInitialSyncTail;
		// A latched early 0x11 (see the 0x11 handler) completes this stage immediately;
		// the held C2S 0x0A then releases at the pump send boundary once world_ready.
		if (sync_tail_seen_) {
			preload_ready_ = true;
			pending_spawn_menu_request_ = true;
		}
	};
	if (admission.admitted && received_settings &&
	    post_auth_stage_ == PostAuthStage::AwaitServerSettings) {
		// Golden retail frames 6-8: acknowledge the host's initial settings packet in a header-only
		// sequence, then send the exact 21-byte C2S 0x00 JOIN request in the following sequence.
		handshake_retry_datagram_.clear();
		handshake_retry_clock_armed_ = false;
		out.outbound.push_back(frame_session({}));
		// The EXP claim is the host's own SUS2 echoed back, sent BEFORE anything has verified
		// that this install can mount that expansion — the host's string compare therefore
		// always passes, and its CRC gate passes too while VERSIONCRCSTRING stays "0"
		// (D-NET-166). Neither host gate can catch a mismatched client, so the claim is made
		// good on OUR side: the preload re-mounts the resource root onto this expansion and
		// ABORTS the join when it is not installed, rather than entering the world with a
		// different ADM index space than the host (D-NET-178).
		out.outbound.push_back(frame_inner(
				0x00, build_join_request(advertised_expansion_)));
		post_auth_stage_ = PostAuthStage::AwaitJoinAck;
	}
	std::size_t message_index = 0;
	for (const ProtocolMessage &m : messages) {
		const uint32_t packet_ack = containing_packet_ack[message_index++];
		// The ONE settings-flagged record that is not connection control to skip: the
		// connection-description block the host sends to close the session. Gated on its exact
		// full tag and on the body actually decoding as a description, so every other
		// settings-update message keeps its behavior below.
		if (m.flags.settings_update &&
		    m.full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION) {
			DisconnectEvent event;
			if (parse_disconnect_event(
					m.payload.data(), m.payload.size(), event)) {
				on_host_disconnect(event);
				return; // the session is closed; nothing later in this packet applies
			}
		}
		// The initial settings records use low tag 0x00 with the high/settings flag set. They are
		// connection control, not the regular S2C 0x00 JOIN acknowledgement below.
		if (m.flags.settings_update || m.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;

		// Dispatch records in wire order. Keeping this out of the metadata pre-pass
		// ensures a 0x39 before a same-packet 0x5A still sees the prior class.
		if (m.tag == s2c::WEAPON_LOADOUT) {
			WeaponLoadout loadout;
			if (decode_weapon_loadout(
					m.payload.data(), m.payload.size(), loadout)) {
				current_player_class_ = loadout.avatar_class;
			}
		}

		if (m.tag == s2c::INIT && post_auth_stage_ == PostAuthStage::AwaitJoinAck) {
			// Golden retail frames 9-11: acknowledge S2C 0x00 with a header-only sequence, then post
			// the one-byte zero form body. An empty 0x01 is not accepted by the retail host FSM.
			out.outbound.push_back(frame_session({}));
			out.outbound.push_back(frame_inner(0x01, {0x00}));
			post_auth_stage_ = PostAuthStage::AwaitPaddingProbe;
		} else if (m.tag == s2c::JOIN_PADDING_PROBE &&
		           post_auth_stage_ == PostAuthStage::AwaitPaddingProbe) {
			// Retail's response to the post-auth join probe. This sequenced reply is what causes the
			// server to send the post-handshake burst containing authoritative S2C 0x7B metadata.
			JoinPaddingProbe probe;
			if (decode_join_padding_probe(m.payload.data(), m.payload.size(), probe)) {
				// The retail builder always produces at least the three-dword prefix, so the echo
				// always ships and the stage always advances — a small or zero padding_len must not
				// wedge the FSM here.
				out.outbound.push_back(frame_inner(
						0x02, build_join_padding_echo(probe, net_frame_counter_)));
				post_auth_stage_ = PostAuthStage::AwaitGameStart;
			}
		} else if (m.tag == s2c::GAME_START_SIGNAL &&
		           post_auth_stage_ == PostAuthStage::AwaitGameStart &&
		           !m.payload.empty() && m.payload[0] != 0) {
			// Golden frame 17: the game-start flag completes verification with ONE grouped C2S
			// packet. This exchange runs before local mission loading; 0x48 echoes ServerAuth.MI.
			out.outbound.push_back(frame_session({
					make_protocol_message(c2s::GAME_START_ACK, std::vector<uint8_t>(4, 0)),
					make_protocol_message(c2s::SET_PLAYER_VALUE, std::vector<uint8_t>(4, 0)),
					make_protocol_message(c2s::CLIENT_ACK, le32_value(conn_.connection_id)),
					make_protocol_message(c2s::PING, {}),
					make_protocol_message(c2s::FILE_CHUNK_REQUEST, std::vector<uint8_t>(8, 0)),
			}));
			post_auth_stage_ = PostAuthStage::AwaitServerInfo;
		} else if (m.tag == s2c::FILE_TRANSFER_CHUNK &&
		           post_auth_stage_ == PostAuthStage::AwaitServerInfo) {
			FileTransferChunk chunk;
			if (decode_file_transfer_chunk(
					m.payload.data(), m.payload.size(), chunk)) {
				if (!chunk.is_final()) {
					const uint32_t next_offset = static_cast<uint32_t>(
							uint64_t(chunk.chunk_offset) + chunk.chunk_size);
					out.outbound.push_back(frame_inner(
							0x33, le32_pair(chunk.transfer_id, next_offset)));
				} else {
					// Golden frames 19-20 are separate packets: state re-broadcast, then the
					// initial eight-zero mission-data request.
					out.outbound.push_back(frame_inner(0x47, {}));
					out.outbound.push_back(frame_inner(
							0x37, std::vector<uint8_t>(8, 0)));
					post_auth_stage_ = PostAuthStage::AwaitMissionData;
				}
			}
		} else if (m.tag == s2c::MISSION_DATA_CHUNK &&
		           post_auth_stage_ == PostAuthStage::AwaitMissionData) {
			FileTransferChunk chunk;
			if (decode_file_transfer_chunk(
					m.payload.data(), m.payload.size(), chunk)) {
				if (!chunk.is_final()) {
					const uint32_t next_offset = static_cast<uint32_t>(
							uint64_t(chunk.chunk_offset) + chunk.chunk_size);
					out.outbound.push_back(frame_inner(
							0x37, le32_pair(chunk.transfer_id, next_offset)));
				} else {
					post_auth_stage_ = PostAuthStage::AwaitPlayerList;
					if (player_list_seen_) {
						out.outbound.push_back(frame_session({
								make_protocol_message(c2s::CHECKSUM_RESPONSE, {}),
								make_protocol_message(
										0x22, {0x00, 0xF7, 0x1C}),
						}));
						enter_initial_sync_tail();
					}
				}
			}
		} else if (m.tag == s2c::PLAYER_LIST) {
			PlayerList player_list;
			if (decode_player_list(
					m.payload.data(), m.payload.size(), player_list)) {
				player_list_seen_ = true;
				if (post_auth_stage_ == PostAuthStage::AwaitPlayerList) {
					// Golden frame 23: transition marker and first player-sync request share
					// one packet. OpenNova hosts may send 0x16 early, so the latch above lets
					// the final 0x64 release the same wire packet without a compatibility fork.
					out.outbound.push_back(frame_session({
							make_protocol_message(c2s::CHECKSUM_RESPONSE, {}),
							make_protocol_message(
									0x22, {0x00, 0xF7, 0x1C}),
					}));
					enter_initial_sync_tail();
				}
			}
		} else if (m.tag == s2c::SESSION_CONFIG) {
			// Our initial-state burst carries the same g_GameType in the fixed
			// session-config block before any live frame. Retail keeps this equal
			// to the later 0x7B `extra` value.
			SessionConfig config;
			if (decode_session_config(m.payload.data(), m.payload.size(), config))
				game_type_ = static_cast<uint32_t>(config.fields[3]);
		} else if (m.tag == s2c::FULL_PLAYER_INFO) {
			// The retail post-auth source of truth for the session the joiner is about to load.
			FullPlayerInfo info;
			if (decode_full_player_info(m.payload.data(), m.payload.size(), info)) {
				server_name_ = std::move(info.server_name);
				mission_name_ = std::move(info.mission_name);
				map_file_ = std::move(info.map_file);
				game_type_ = info.extra;
				expansion_ = std::move(info.game_name);
				mission_known_ = true;
			}
		} else if (m.tag == s2c::ENTITY_SPAWN_BATCH) {
			// S2C 0x0C organic-spawn batch — the self name-match (§5.23). ALSO surface the whole
			// batch to the NetClientView (below) so every other organic upserts too.
			OrganicSpawnBatch batch;
			if (decode_organic_spawn_batch(m.payload.data(), m.payload.size(), batch)) {
				for (const OrganicSpawnRecord &rec : batch.records) {
					if (!rec.has_body || rec.entity_name != player_name_) continue;
					if (has_self_handle_ && rec.slot_id != self_handle_) {
						// Name-match self-ID (D.0, §5.23) cannot disambiguate two live players
						// sharing a callsign — the 0x0C record carries no connection id, and
						// adopting either handle may uplink against the other player's entity.
						// Pre-release this is fatal ambiguity; once in-match our handle is
						// latched and a later same-name spawn (another player joining with our
						// callsign) must not re-bind us. The faithful numeric self-ID (dcb ->
						// player table, Player_FindLocalPlayerEntity @0x4e0090 via 0x4D/0x46
						// §5.21) is the tracked burn-down path (D-NET-169).
						if (phase_ != Phase::InMatch) {
							fail("duplicate callsign '" + player_name_ +
							     "' in session (self-identification is name-match; pick a unique callsign)");
							return;
						}
						continue;
					}
					self_handle_ = rec.slot_id; // the wire handle H (pool<<12|slot)
					has_self_handle_ = true;
					spawn_.pos_x = rec.pos_x;
					spawn_.pos_y = rec.pos_y;
					spawn_.pos_z = rec.pos_z;
					spawn_.orientation = rec.orientation;
					spawn_.team = rec.team;
					spawn_.item_type_id = rec.item_type_id;
					spawn_.anim_slot = rec.anim_slot;
					spawn_.net_id = rec.net_id;
					// Retail learns H during the world stream but does not deploy/uplink yet.
					// Only the post-0x0E 0x5A releases that separate boundary.
					if (deployment_reply_seen_ && phase_ != Phase::InMatch) {
						phase_ = Phase::InMatch;
						post_auth_stage_ = PostAuthStage::Complete;
						out.reached_in_match = true;
					}
				}
			}
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::POOL_SPAWN || m.tag == s2c::STATIC_ENTITY_BATCH || m.tag == s2c::POOL3_SYNC) {
			// The rest of the load-time world stream (§5.2a): pool-1 spawns / pool-2 statics /
			// pool-3 markers. Surface raw for the caller's NetClientView to upsert.
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::SESSION_SLOT_CONFIG) {
			// S2C 0x04 slot assignment: the tail byte is this joiner's server-assigned team —
			// retail's byte_A85B48, the 0x2F loadout-submit header byte 0. A retail host always
			// writes the full 24-byte body [orig: NetPacket_WriteSlotAssignment @0x502b30];
			// latch only that witnessed shape and keep the golden default otherwise.
			// [orig: NapiNPClientMsg_0x004 @0x425410 -> byte_A85B48 @0x425499]
			// Byte 17 is this joiner's OWN roster slot id — retail's
			// g_local_player_slot_id, and the same value the host keeps at its
			// per-player record slot+20 [orig: the write side
			// NetPacket_WriteSlotAssignment @0x502b30]. It is NOT cosmetic: the client
			// packs it into the high 7 bits of every fired round's hit_part word, which
			// is how the host attributes the shot. Latching 0 makes every shot we send
			// read as SLOT 0 — the host's own — see nova_simulation's hit_part builder.
			if (m.payload.size() >= 24) {
				local_player_slot_ = m.payload[17];
				assigned_team_ = m.payload[23];
			}
		} else if (m.tag == s2c::TEAM_ASSIGN) {
			// S2C 0x50 TEAM ASSIGN — the SECOND witnessed writer of the byte_A85B48
			// team latch (the S2C 0x04 tail is the first). The host emits it for any
			// entity retargeted by Server_ChangeEntityTeam @0x518D70, capture zones
			// included. Witnessed legs, in order:
			//   (1) entity == local player -> byte_A85B48 = team @0x4319db;
			//   (2) !is_authority -> entity->Team = team @0x4319ee (any pool 0..4);
			//   (3) the player-slot team byte mirrors it @0x431a0b;
			//   (4) player (Flags & 0x100) + self -> per-side profile reselect and ONE
			//       C2S 0x2F re-submission @0x431a9e carrying the NEW team and slot 195
			//       RAW (the pre-Player_InitPlayer form, NOT g_currentWeaponSlot).
			// [orig: NapiNPClientMsg_0x050 @0x431910]
			TeamAssign assign;
			std::size_t assign_consumed = 0;
			if (decode_team_assign(m.payload.data(), m.payload.size(), assign,
					assign_consumed) &&
			    world::EntityHandle{assign.entity_handle}.valid() &&
			    world::EntityHandle{assign.entity_handle}.pool() < world::kEntityPoolCount) {
				// Retail's header gates: not the 0xFFFF sentinel, and the pool
				// nibble must address one of the five entity pools.
				out.entity_team_assigns.emplace_back(
						assign.entity_handle, assign.team);
				if (has_self_handle_ && assign.entity_handle == self_handle_) {
					assigned_team_ = assign.team; // [orig: @0x4319db]
					out.self_team_changed = true;
					out.self_team = assign.team;
					// The re-submission exists only after the 0x1A-released
					// initial pair (the profile/armory state it rebuilds is
					// in-world state). The per-side profile reselect
					// (@0x431a35..0x431a9a) is PORTED: the NEW team byte picks the
					// profile side block, that block's own class byte is the wire
					// class, and the page that class indexes is the kit — so a
					// joiner pushed across the line submits a kit the new side's
					// class can legally hold. Still deferred with witnesses in
					// D-NET-168: the C2S 0x22/0x23 acks (@0x431acb..0x431b05),
					// Player_InitPlayer(1) @0x431b14 and the minimap NetId
					// maintenance @0x431b3a..0x431b91.
					if (post_auth_stage_ == PostAuthStage::AwaitDeployment ||
					    post_auth_stage_ == PostAuthStage::AwaitDeployPick ||
					    post_auth_stage_ == PostAuthStage::AwaitDeployRelease ||
					    post_auth_stage_ == PostAuthStage::Complete) {
						// [orig: @0x431a9e passes slot 195 RAW — the
						//  pre-Player_InitPlayer form, NOT the live
						//  g_currentWeaponSlot]
						const LoadoutSubmit resubmit =
								build_profile_loadout_submit(
										assigned_team_, false);
						periodic_replies.push_back(make_protocol_message(
								0x2F, encode_loadout_submit(resubmit)));
					}
				}
			}
		} else if (m.tag == s2c::EMPTY_SLOT_SWEEP) {
			// S2C 0x5D EMPTY-SLOT SWEEP — the reply to our C2S 0x32. Every entry is a
			// RAW pool-0 index the host considers empty; the client destroys whatever
			// it still holds there and unlinks the matching player slot. This is the
			// only channel that retires a peer's entity permanently, so without it a
			// leaver's decoded row lives on as a ghost proxy (D-NET-176).
			// [orig: NapiNPClientMsg_DestroyEntityList @0x429730 (!is_authority) ->
			//  Entity_Destroy + PlayerSlot_FindByType(idx) -> PlayerSlot_ClearAndUnlink
			//  @0x434730]
			DestroyEntityList destroy_list;
			if (decode_destroy_entity_list(
					m.payload.data(), m.payload.size(), destroy_list)) {
				for (uint16_t index : destroy_list.pool0_indices)
					out.destroyed_pool0_slots.push_back(index);
			}
		} else if (m.tag == s2c::PLAYER_SYNC) {
			// S2C 0x46 PLAYER-SYNC. Bit 15 (0x8000) is the roster REMOVAL form: no
			// entity byte follows, and PlayerSlot_ClearAndUnlink clears BOOKKEEPING
			// ONLY (active flag, names, team, entity ref). The ENTITY IS NEVER
			// DESTROYED on this leg — the entity-field wipes @0x431437 are dead code
			// there because entitySlotPtr is null. Only the 0x5D sweep retires an
			// entity (this corrects the D-NET-80 note that read the removal bit as the
			// per-entity removal channel).
			// [orig: NapiNPClientMsg_PlayerSync @0x431370 @0x431411..0x43144c]
			PlayerSync sync;
			if (decode_player_sync(m.payload.data(), m.payload.size(), sync) &&
			    sync.removal) {
				out.cleared_player_slots.push_back(sync.slot_id);
			}
		} else if (m.tag == s2c::WORLD_STATE_LOAD && !empty_slot_sweep_requested_) {
			// The world-state-load reply burst (§5.29). Retail queues C2S
			// 0x28/0x29/0x2D/0x32 here; the 0x32 leg is what makes the host answer
			// S2C 0x5D with its empty pool-0 slots, so a joiner that never asks can
			// never clear a stale entity. The other three burst members remain unsent
			// (D-NET-176 residual).
			// [orig: NapiNPClientMsg_0x00F @0x42e647]
			empty_slot_sweep_requested_ = true;
			periodic_replies.push_back(make_protocol_message(c2s::EMPTY_SLOT_SWEEP_REQUEST, {}));
		} else if (m.tag == s2c::WAIT_FOR_GAME_START_ACK &&
		           post_auth_stage_ == PostAuthStage::AwaitWorldStreamEnd) {
			// The world-stream terminator releases retail's loadout/status submission: the SAME
			// per-side profile kit twice — first with the fixed Primary-key slot 195, then with
			// the live equipped slot — plus the witnessed nonzero mission-status tail. The kit
			// content comes from the binding seam when set (the shell's applied loadout); the
			// capture-default kit keeps headless callers byte-identical.
			// [orig: Game_StartMission @0x525836/@0x525c2e -> NetPacket_SendLoadoutSubmit
			//  @0x42cdc0; the 0x0B status tail rides the same flush]
			// "Per-side" is literal: the latched team byte picks the profile side
			// block, and that block's class byte picks the page [orig: @0x525767].
			const LoadoutSubmit first =
					build_profile_loadout_submit(assigned_team_, false);
			const LoadoutSubmit second =
					build_profile_loadout_submit(assigned_team_, true);
			out.outbound.push_back(frame_session({
					make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(first)),
					make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(second)),
					make_protocol_message(
							0x0B, build_retail_mission_status()),
			}));
			post_auth_stage_ = PostAuthStage::AwaitDeployment;
		} else if (m.tag == s2c::RTT_ECHO) {
			RttSample sample;
			std::size_t consumed = 0;
			if (decode_rtt_sample(
					m.payload.data(), m.payload.size(), sample, consumed) &&
			    consumed == m.payload.size() && sample.echo_flag != 0) {
				std::vector<uint8_t> pong = le32_value(sample.timestamp);
				pong.push_back(0);
				periodic_replies.push_back(
						make_protocol_message(c2s::RTT_CONSUMED, std::move(pong)));
			}
		} else if (m.tag == s2c::CHARATTR_PROPERTY_CLEAR) {
			// Session-option mutation of the boot charattr table. A short body
			// defaults to property 0; trailing bytes are ignored. Every row is
			// touched before the next inner message dispatches.
			// [orig: NapiNPClientMsg_ClearAnimSlot @0x4254C0 (misnamed) ->
			//  CharAttr_SetSlotProperty @0x412890]
			const uint8_t property_id =
					m.payload.empty() ? uint8_t{0} : m.payload[0];
			clear_charattr_challenge_property(
					charattr_challenge_table_, property_id);
		} else if (m.tag == s2c::CHARATTR_CRC_CHALLENGE) {
			// Anti-cheat character-attribute CRC challenge. The reply is one
			// 4-byte C2S 0x1C, and
			// the host DISCARDS the value: its handler calls its own
			// checksum function, throws the result away, and the message's only
			// effect is zeroing a per-player counter. That counter is what matters —
			// each unanswered challenge leaves it climbing, and a live retail host was
			// observed stopping the joiner's entire entity-record stream after eight
			// silent challenges while the transport stayed healthy.
			// The selected 124-byte source is one g_CharAttr CHARACTER row loaded
			// from charattr.def. Its embedded class must match the latest
			// authoritative 0x5A class; absent/inactive/mismatched rows return zero.
			// [orig: NapiNPClientMsg_HandleChecksumChallenge @0x42E6D0 (reply tag 0x1C,
			//  len 4 @0x42e723); checksum @0x412aa0 (IDB
			//  AnimMap_GetSlotChecksum is misnamed) — the `return 0` arm
			//  @0x412abf; the host's discard + counter reset
			//  NapiNPServerMsg_0x01C @0x501D40 @0x501d71/@0x501d79]
			uint32_t challenge_seed = 0;
			std::size_t seed_consumed = 0;
			decode_u32_scalar(m.payload.data(), m.payload.size(), challenge_seed,
					seed_consumed);
			uint32_t checksum = 0;
			if (const CharAttrChallengeRow *row =
					    find_charattr_challenge_row(
							    charattr_challenge_table_,
							    current_player_class_)) {
				checksum = challenge_seed ^
						crc32_napi(row->data(), row->size());
			}
			periodic_replies.push_back(
					make_protocol_message(c2s::CHARATTR_CRC_REPLY, le32_value(checksum)));
		} else if (m.tag == s2c::ENTITY_CHECKSUM_REQ) {
			// Anti-cheat ANIMATION-DEFINITION checksum challenge. The reply is one 5-byte
			// C2S 0x20: [u8 echoed id][u32 challenge ^ source]. The source is selected by
			// the id: 0xFF asks for the whole loaded weapon-slot table, any other value
			// for that one .adm entry — and 0xFF with a NONZERO challenge short-circuits
			// to the literal 42 without touching state at all.
			// Both real sources CRC retail's in-memory 1120-byte .adm image (the
			// weapon-slot form concatenates every loaded entry after overwriting its
			// volatile fields with sentinels), a byte layout OpenNova does not model.
			// [orig: NapiNPClientMsg_HandleChecksumRequest @0x431170 (dispatch row 0x30
			//  @0x82b1a8; reply tag 0x20 @0x4311d7) -> NetPacket_WriteEntityChecksum
			//  @0x42afb0 — the 42 arm @0x42afcc, AnimDef_ComputeChecksum @0x541ef0,
			//  WeaponSlotDef_ComputeSanitizedChecksum @0x53f8f0]
			//
			// DELIBERATELY NOT ANSWERED until that image exists. A value we cannot compute
			// can only ever MISMATCH, and a mismatch is the one thing this challenge
			// punishes: the host recomputes the checksum itself and disconnects on a
			// difference [orig: handle_anti_cheat_crc_check @0x502050 — the compare and its
			// "PUNT ACRC" arm], while a challenge that is never answered is not punished at
			// all (the strike counter it would otherwise clear lives on the 0x1C path).
			// Witnessed live 2026-07-26 against a stock retail co-op host: a single C2S
			// 0x20 carrying the nothing-loaded placeholder drew an immediate punt
			// (DC=2, DPC=46, "PUNT ACRC") on a client that had been joining cleanly for
			// weeks while silent. Silence is the SAFE divergence here (D-NET-181).
			(void)m;
		} else if (m.tag == s2c::LOADOUT_CRC_REQ) {
			// Anti-cheat AMMO-DEFINITION CRC challenge. The reply is one 9-byte C2S 0x21:
			// [u8 echoed index][u32 crc][u32 echoed key]. Retail CRCs the indexed 276-byte
			// in-memory ammo record with six volatile dwords zeroed and ships key ^ crc —
			// again a byte image OpenNova does not model. Its own out-of-range arm is the
			// witnessed form for a table that holds nothing at that index, and that arm
			// writes a ZERO crc dword rather than key ^ 0, so the key echo is the only
			// challenge-derived field here. The unmodeled image is D-NET-181
			// (docs/net/novaworld-net-re.md §5.65).
			// [orig: NapiNPClientMsg_0x031 @0x4311e0 (dispatch row 0x31 @0x82b1b8; reply
			//  tag 0x21 @0x431245) -> NetPacket_WriteEntityCRCChecksum @0x42b020 — the
			//  out-of-range arm @0x42b114, the key echo @0x42b14d]
			LoadoutCrcRequest crc_request;
			std::size_t crc_consumed = 0;
			decode_loadout_crc_request(m.payload.data(), m.payload.size(),
					crc_request, crc_consumed);
			// DELIBERATELY NOT ANSWERED, for the same reason as 0x30 above. The host
			// recomputes this one itself — `handle_anti_cheat_crc_check @0x502050` zeroes
			// the six volatile dwords of ITS `g_ammoDefTable[276 * index]`, runs
			// `CRC_ComputeCustomTable(record, 276) @0x53c820`, XORs the per-player key at
			// `playerCtx + 89924` and compares. A placeholder can only ever mismatch, and
			// the mismatch arm disconnects ("PUNT WCRC" was the live 2026-07-26 result of
			// sending one). Answering honestly requires building retail's 276-byte ammo
			// record image from our parsed ammo.def, which we do not model yet
			// (D-NET-181) — until then silence, which the host does not punish.
			(void)crc_request;
		} else if (m.tag == s2c::LOADED_MODEL_PAGE_REQUEST) {
			// Loaded-model index-page request. The reply is C2S 0x3D, whose body the host
			// never parses (its handler ignores data/dataLen and only stores a global
			// into the player record), so what matters is that the reply ARRIVES.
			// Retail's writer emits `[u32 startIndex]` and then up to fifty dwords
			// from the renderer's frozen, non-foliage loaded-.3DI snapshot — but
			// only while the cursor is inside its list, so a
			// start index at or past the end writes the header ALONE. The binding
			// supplies that snapshot after renderer-resource finalization; an empty
			// snapshot therefore produces the original header-only form.
			// [orig: NapiNPClientMsg_0x068 @0x42DAA0 (reply tag 0x3D @0x42dae5);
			//  NetPacket_WriteEntityIndexList @0x42d950 — header @0x42d9bd, the
			//  `current_index < count` page gate @0x42d9c3; source snapshot getter
			//  @0x5B1560, builder @0x5B3A80; the host's body-ignoring handler
			//  NapiNPServerMsg_0x03D @0x500EC0]
			uint32_t start_index = 0;
			std::size_t index_consumed = 0;
			decode_u32_scalar(m.payload.data(), m.payload.size(), start_index,
					index_consumed);
			periodic_replies.push_back(
					make_loaded_model_page_reply(start_index));
		} else if (m.tag == s2c::TIME_SYNC_PING) {
			// Time-sync / anti-speedhack ping. The client echoes the server's stamp
			// back with its OWN clock appended, and the host validates that the
			// client's reported time deltas track wall-clock within 3% — so a client
			// that never answers is a client whose clock the host cannot verify. The
			// reply is one 8-byte C2S 0x08: [u32 echoed server stamp][u32 local ms].
			// (Retail reads GetTickCount here; that is an OS primitive, so the
			// monotonic millisecond clock is the equivalent.)
			// [orig: NapiNPClientMsg_0x043 @0x42FA90 — echo @0x42faa7, local stamp
			//  @0x42fac4, QueueReliableMessage(tag 8, len 8) @0x42fad8; the host-side
			//  validator validate_time_sync @0x502210]
			uint32_t server_stamp = 0;
			std::size_t scalar_consumed = 0;
			decode_u32_scalar(m.payload.data(), m.payload.size(), server_stamp,
					scalar_consumed);
			const uint32_t local_stamp =
					static_cast<uint32_t>(monotonic_milliseconds_() & 0xFFFFFFFFu);
			std::vector<uint8_t> reply;
			reply.reserve(8);
			for (int shift = 0; shift < 32; shift += 8)
				reply.push_back(static_cast<uint8_t>((server_stamp >> shift) & 0xFFu));
			for (int shift = 0; shift < 32; shift += 8)
				reply.push_back(static_cast<uint8_t>((local_stamp >> shift) & 0xFFu));
			periodic_replies.push_back(
					make_protocol_message(c2s::TIME_SYNC_REPLY, std::move(reply)));
		} else if (m.tag == s2c::TICK_SEED) {
			// The per-player TICK SEED. Every C2S fired-round descriptor stamps the
			// client's network-role tick at off-0, and the host rejects a shot whose
			// tick is zero or not past the seed it stamped into our slot — so an
			// unseeded client's fire is silently discarded forever. Re-sent on join,
			// death, revive and deploy release; the four-zero-byte form disarms.
			// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 (short body -> 0
			//  @0x4297eb); the host stamp Server_SendRandomSeedToPlayer @0x5101a0;
			//  the gate PlayerSlot_IsActive @0x4fc760 via
			//  NapiNPServerMsg_0x006_ClientFiredRound @0x51358d]
			decode_tick_seed(m.payload.data(), m.payload.size(), out.tick_seed);
			out.tick_seed_set = true;
		} else if (m.tag == s2c::PER_FRAME_UPDATE) {
			// Per-frame world snapshot — surface for the caller's NetClientView.
			out.inbound_0a.push_back(m.payload);
		} else if (m.tag == s2c::WEAPON_LOADOUT &&
		           post_auth_stage_ == PostAuthStage::AwaitDeployment) {
			// Retail grants both submitted profile-side loadouts before it can
			// process the deployment pick. Persist the count across packet
			// boundaries; waiting for both also prevents a lost/retransmitted
			// second grant from being mistaken for the post-pick release.
			if (initial_loadout_grant_count_ < 2)
				++initial_loadout_grant_count_;
		} else if (m.tag == s2c::WEAPON_LOADOUT &&
		           post_auth_stage_ == PostAuthStage::AwaitDeployRelease) {
			// A split second grant may arrive after we have queued the deploy
			// pick. Only a packet that cumulatively ACKs the C2S 0x0E can be its
			// causal post-pick release.
			if (deployment_pick_sent_ &&
			    packet_ack >= deployment_pick_sequence_) {
				release_deployment();
			}
		} else if (m.tag == s2c::WEAPON_RELOAD) {
			// The host echoes the same four-byte C2S 0x25 reload body as S2C 0x49.
			// Surface it once through the decoded client-view event path.
			out.inbound_gameplay.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::ZONE_TIMER_VALUE) {
			ZoneTimerValue value;
			std::size_t consumed = 0;
			if (decode_zone_timer_value(
					m.payload.data(), m.payload.size(), value, consumed) &&
			    consumed == m.payload.size())
				out.zone_timer_updates.emplace_back(value);
		} else if (m.tag == s2c::ZONE_TIMER_WINDOW) {
			ZoneTimerWindow window;
			std::size_t consumed = 0;
			if (decode_zone_timer_window(
					m.payload.data(), m.payload.size(), window, consumed) &&
			    consumed == m.payload.size())
				out.zone_timer_updates.emplace_back(window);
		} else if (m.tag == s2c::DISCONNECT_UNLOCK) {
			// S2C 0x11 is the terminal pre-world admission marker. Its cumulative ACK is the safe
			// point at which the binding may pause progress for synchronous mission loading. Hold
			// retail's empty C2S 0x0A world-stream request until that local world is installed.
			// The host emits this marker exactly ONCE, paced behind its C2S 0x02 reply regardless
			// of our transfer progress (our host: ~13 ticks, §5.5 bundle pacing). Latch an early
			// arrival — a joiner stalled past that window mid-0x60/0x64 would otherwise drop it
			// forever and park both sides (never preload_ready here, sync state 3 host-side).
			sync_tail_seen_ = true;
			if (post_auth_stage_ == PostAuthStage::AwaitInitialSyncTail) {
				preload_ready_ = true;
				pending_spawn_menu_request_ = true;
			}
			if (pending_spawn_menu_request_ && world_ready_) {
				out.outbound.push_back(frame_inner(0x0A, {}));
				pending_spawn_menu_request_ = false;
				post_auth_stage_ = PostAuthStage::AwaitWorldStreamEnd;
			}
		}
		// Other tags (game-start bundle scalars, world-state-load 0x0F) are not entity data.
	}
	for (ProtocolMessage &reply : periodic_replies)
		out.queued_send_messages.push_back(std::move(reply));
	if (admission.admitted && initial_loadout_grant_count_ >= 2 &&
	    deployment_policy_seen_ &&
	    post_auth_stage_ == PostAuthStage::AwaitDeployment) {
		if (deployment_pick_required_) {
			if (player_paced_deployment_) {
				// The binding shows the deploy-map screen and sends the player's
				// pick via frame_deployment_pick; the host keeps this player
				// hidden/respawn-pending meanwhile (0x0A flags1 bit1, §5.61).
				post_auth_stage_ = PostAuthStage::AwaitDeployPick;
			} else {
				// Headless auto-answer: retail's parameter-0 deploy-map pick is the
				// signed handle 0xFFFF (the DEATH list's Default Spawn row 0).
				deployment_pick_sequence_ = conn_.seq.next_outbound_seq;
				std::vector<uint8_t> deploy_pick =
						frame_inner(0x0E, {0xFF, 0xFF});
				if (!deploy_pick.empty()) {
					out.outbound.push_back(std::move(deploy_pick));
					deployment_pick_sent_ = true;
					post_auth_stage_ = PostAuthStage::AwaitDeployRelease;
				}
			}
		} else {
			// No spawn-zone/deploy screen exists; the initial grant is also
			// the final deployment reply.
			release_deployment();
		}
	}
	// If none of the handlers produced a substantive reply, carry this packet's ACK to the send
	// boundary. The held mission-load path must still retire retail's reliable metadata stream, while
	// an inbound header-only packet (messages.empty()) must not cause an ACK ping-pong.
	if (admission.admitted && !messages.empty() && out.outbound.empty())
		session_ack_pending_ = true;
}

void JoinerConnection::on_server_resend_list(
		const std::vector<uint8_t> &body, PollResult &out) {
	std::vector<uint32_t> requested;
	if (!decode_session_resend_list(
			body.data(), body.size(), client_key_, requested)) {
		return;
	}

	for (uint32_t requested_sequence : requested) {
		const uint32_t sequence = requested_sequence == 0
				? conn_.seq.next_outbound_seq
				: requested_sequence;
		std::vector<uint8_t> datagram = frame_retained_session(sequence);
		if (!datagram.empty()) out.outbound.push_back(std::move(datagram));
	}
}

std::vector<std::vector<uint8_t>> JoinerConnection::pump(uint32_t /*now_tick*/) {
	std::vector<std::vector<uint8_t>> out;
	// [orig: NetClient_HandleGameEnd @0x424752 increments @0xA822A4 once per client net pump]
	++net_frame_counter_;
	if (phase_ == Phase::Hello || phase_ == Phase::Auth) {
		if (handshake_retry_datagram_.empty()) return out;
		const uint64_t now_ms = monotonic_milliseconds_();
		// Each initial send records its wall-clock timestamp. Keep a defensive lazy-anchor for a future
		// caller that installs a pending leg without going through those builders.
		if (!handshake_retry_clock_armed_) {
			handshake_last_send_ms_ = now_ms;
			handshake_retry_clock_armed_ = true;
			return out;
		}
		if (now_ms >= handshake_last_send_ms_ &&
		    now_ms - handshake_last_send_ms_ >= kHandshakeRetryMilliseconds) {
			handshake_last_send_ms_ = now_ms;
			out.push_back(handshake_retry_datagram_);
		}
		return out;
	}
	// Pump is the receive-batch boundary used by ClientRuntime: every framed datagram has already
	// been drained through handle_datagram before this call. Clear retail's future-packet latch
	// once here; emit 0x44 only if the contiguous drain still left a gap.
	if (conn_.seq.missing_request_pending) {
		conn_.seq.missing_request_pending = false;
		if (!conn_.seq.queued_inbound.empty()) {
			const std::vector<uint32_t> missing =
					build_session_missing_sequence_list(conn_.seq, false);
			std::vector<uint8_t> missing_body;
			if (encode_session_resend_list(conn_.server_sk, missing, missing_body)) {
				out.push_back(nw_encode_outbound(
						SESSION_OPCODE_CLIENT_RESEND_LIST, std::move(missing_body)));
			}
		}
	}
	// Retail's reliable sender does not blindly resend an unanswered semantic packet. While retained
	// records remain, the active-send interval mints a NEW header-only sequence. A peer missing the
	// preceding semantic sequence observes the gap and requests it through 0x44/0x84; the existing
	// retained-record path then reconstructs that old sequence with the current ACK.
	if (phase_ == Phase::Driving || phase_ == Phase::InMatch) {
		if (conn_.seq.retained_outbound_message_count == 0) {
			session_send_clock_armed_ = false;
			// Nothing queued and nothing retained: the EMPTY interval keeps the
			// connection alive. Without it a joiner that owes no reply (parked at the
			// deploy pick, dead with the uplink gate shut, or idle) transmits nothing
			// and a stock host drops it at its 120 s connection timeout.
			const uint64_t now_ms = monotonic_milliseconds_();
			if (conn_.server_sk != 0 && session_last_send_ms_ != 0 &&
			    now_ms >= session_last_send_ms_ &&
			    now_ms - session_last_send_ms_ > kSessionIdleSendIntervalMilliseconds) {
				out.push_back(frame_session({}));
			}
		} else {
			const uint64_t now_ms = monotonic_milliseconds_();
			if (!session_send_clock_armed_) {
				session_last_send_ms_ = now_ms;
				session_send_clock_armed_ = true;
			} else if (now_ms >= session_last_send_ms_ &&
			           now_ms - session_last_send_ms_ > kSessionActiveSendIntervalMilliseconds) {
				out.push_back(frame_session({}));
			}
		}
	}
	if (phase_ != Phase::Driving) {
		if (phase_ == Phase::InMatch && session_ack_pending_)
			out.push_back(frame_session({}));
		return out;
	}

	// The admission FSM is entirely server-driven. When S2C 0x11 arrived during a synchronous
	// load hold, release its empty C2S 0x0A world-stream request as soon as the binding reports that
	// the advertised mission has been installed.
	if (pending_spawn_menu_request_ && world_ready_) {
		out.push_back(frame_inner(0x0A, {}));
		pending_spawn_menu_request_ = false;
		post_auth_stage_ = PostAuthStage::AwaitWorldStreamEnd;
	}

	// Carry a pending cumulative ACK only when no substantive C2S producer above already did so.
	// All semantic join messages are emitted at their captured S2C trigger in on_server_session.
	if (session_ack_pending_) out.push_back(frame_session({}));
	return out;
}

LoadoutSubmit JoinerConnection::build_profile_loadout_submit(
		uint8_t team, bool alternate) const {
	// Retail re-reads the PROFILE for every submission that is not the armory's: the
	// wire team byte picks the side block, the block's first byte is the wire class, and
	// that class indexes the 2048-byte kit page inside the same block. One integer, both
	// fields — the reason a retail kit can never be class-illegal.
	// [orig: Game_StartMission @0x525767-0x525836 (join) and NapiNPClientMsg_TeamAssign
	//  @0x431a35..0x431a9a (the S2C 0x50 reselect)]
	if (!loadout_kit_set_) return build_retail_default_loadout_submit(team, alternate);
	LoadoutSubmit submit;
	submit.team = team;
	const LoadoutKit::SideKit &side = loadout_kit_.side_for_team(team);
	if (side.set) {
		submit.player_class = side.player_class;
		submit.entries = side.rows;
	} else {
		// The binding supplied only the applied kit (the historical single-kit seam).
		submit.player_class = loadout_kit_.player_class;
		submit.entries = loadout_kit_.rows;
	}
	// [orig: @0x525836 passes 195, @0x525c2e passes g_currentWeaponSlot]
	submit.weapon_slot_index = alternate && loadout_kit_.equipped_combo >= 0
			? static_cast<uint32_t>(loadout_kit_.equipped_combo)
			: 195u;
	return submit;
}

std::vector<uint8_t> JoinerConnection::frame_loadout_resubmit() {
	ProtocolMessage message;
	if (!prepare_loadout_resubmit(message)) return {};
	return frame_session({std::move(message)});
}

bool JoinerConnection::prepare_loadout_resubmit(
		ProtocolMessage &message_out) const {
	// The armory-ACCEPT re-send [orig: WeaponLoadout_ApplyFromBuffer @0x565d94 ->
	// NetPacket_SendLoadoutSubmit]: one 0x2F, current team + class + kit, slot = the
	// live equipped combo (retail passes g_currentWeaponSlot). Only meaningful once
	// the initial submission has gone out — the armory opens in-world.
	// This leg does NOT re-read the profile side blocks: retail hands the ARMORY's
	// own edited buffer and the armory's class to the builder, so the applied kit
	// (LoadoutKit::player_class/rows) stays the source here.
	if (post_auth_stage_ != PostAuthStage::AwaitDeployment &&
	    post_auth_stage_ != PostAuthStage::AwaitDeployPick &&
	    post_auth_stage_ != PostAuthStage::AwaitDeployRelease &&
	    post_auth_stage_ != PostAuthStage::Complete) {
		return {};
	}
	LoadoutSubmit submit;
	if (loadout_kit_set_) {
		submit.team = assigned_team_;
		submit.player_class = loadout_kit_.player_class;
		submit.weapon_slot_index = loadout_kit_.equipped_combo >= 0
				? static_cast<uint32_t>(loadout_kit_.equipped_combo)
				: 195u;
		submit.entries = loadout_kit_.rows;
	} else {
		submit = build_retail_default_loadout_submit(assigned_team_, true);
	}
	message_out = make_protocol_message(
			0x2F, encode_loadout_submit(submit));
	return true;
}

std::vector<uint8_t> JoinerConnection::frame_deployment_pick(uint16_t wire_value) {
	ProtocolMessage message;
	if (!prepare_deployment_pick(wire_value, message)) return {};
	return frame_session({std::move(message)});
}

bool JoinerConnection::prepare_deployment_pick(
		uint16_t wire_value, ProtocolMessage &message_out) {
	// The player's deploy-map pick [orig: Input_HandleActionBinding case 12
	// @0x49b0c5-0x49b17b — dialogs reset, dword_81474C set, one C2S 0x0E [i16]].
	// A re-pick while awaiting the release is retail behavior: the host silently
	// drops an invalid/contested pick and the screen stays up; every list click
	// queues a fresh 0x0E. The release rule keys on the FIRST in-flight pick's
	// sequence: the host releases on the first VALID pick it processes, so a
	// re-pick racing an already-sent release must not raise the ack bar past it
	// (the grant-vs-release discrimination only needs the ack to clear the first
	// pick — a cumulative ack covering any later pick clears it too).
	if (post_auth_stage_ != PostAuthStage::AwaitDeployPick &&
	    post_auth_stage_ != PostAuthStage::AwaitDeployRelease) {
		return {};
	}
	if (!deployment_pick_sent_)
		deployment_pick_sequence_ = conn_.seq.next_outbound_seq;
	message_out = make_protocol_message(
			0x0E, {static_cast<uint8_t>(wire_value & 0xFFu),
			       static_cast<uint8_t>((wire_value >> 8) & 0xFFu)});
	deployment_pick_sent_ = true;
	post_auth_stage_ = PostAuthStage::AwaitDeployRelease;
	return true;
}

// C2S 0x1D stance-change: [i16 action id] 0xA9 crouch / 0xAA prone / 0xAC stand — sent
// straight from the stance key SELECT (the local latch rides the same loopback in
// retail); the server applies with mutual exclusion.
// [orig: senders (cases 169/170/172) @0x4e0d77/@0x4e0df3/@0x4e0e3e;
//  apply NapiNPServerMsg_HandleStanceChange @0x501c60]
bool JoinerConnection::begin_redeployment() {
	if (phase_ != Phase::InMatch || !has_self_handle_) return false;
	// The authenticated connection and self entity survive death. Only the spawn-success gate and
	// the pick/release sub-state reset; the host's subsequent 0x61 re-seeds the client clock.
	phase_ = Phase::Driving;
	post_auth_stage_ = PostAuthStage::AwaitDeployPick;
	deployment_pick_sent_ = false;
	deployment_pick_sequence_ = 0;
	deployment_reply_seen_ = false;
	return true;
}

std::vector<uint8_t> JoinerConnection::frame_stance_change(uint16_t action_id) {
	if (phase_ != Phase::InMatch) return {};
	return frame_session({make_protocol_message(
			0x1D, {static_cast<uint8_t>(action_id & 0xFFu),
	               static_cast<uint8_t>((action_id >> 8) & 0xFFu)})});
}

std::vector<uint8_t> JoinerConnection::frame_c2s_uplink(uint16_t handle_H, uint16_t type,
                                                        const PlayerExtendedUplink &body) {
	EntityPacketSubHeader sub;
	sub.handle = handle_H;
	sub.item_type_id = type;
	sub.sub_op = ENTITY_SUB_OP_EXTENDED; // extended (type-10) uplink
	std::vector<uint8_t> payload = encode_entity_packet_sub_header(sub);
	std::vector<uint8_t> ext = encode_player_extended_uplink(body);
	payload.insert(payload.end(), ext.begin(), ext.end());
	return frame_session({make_protocol_message(c2s::ENTITY_UPLINK, std::move(payload))});
}

void JoinerConnection::seed_in_match(uint32_t session_id, uint32_t client_key,
                                     std::string client_scrk, std::string server_scrk,
                                     uint32_t next_seq, uint32_t last_ack,
                                     uint16_t self_handle, uint16_t self_type,
                                     uint32_t game_type) {
	conn_.server_sk = session_id;            // 0x43 header session_id (= ServerAuth.sk)
	// The captured ClientAuth.ck: the inbound S2C admission gate validates every 0x83
	// header against the client-local key, so without this a seeded replay silently
	// drops the capture's whole S2C stream. 0 (the reserved sentinel) keeps the default.
	if (client_key != 0) client_key_ = client_key;
	conn_.client_scrk = std::move(client_scrk); // encrypts our outbound 0x43 (the captured client SCRK)
	conn_.server_scrk = std::move(server_scrk); // decrypts inbound 0x83 (for the S2C fold half)
	conn_.seq = make_jo_game_session_sequencing(next_seq, last_ack);
	// frame_session stamps next_seq then post-increments; last_ack -> 0x43 ack_count
	self_handle_ = self_handle;
	has_self_handle_ = true;
	spawn_.item_type_id = self_type;
	game_type_ = game_type;
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	post_auth_stage_ = PostAuthStage::Complete;
	// A seeded session is mid-stream: anchor the send clock so the empty-interval
	// keepalive has a valid reference even if this connection never frames anything.
	session_last_send_ms_ = monotonic_milliseconds_();
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	// A seeded replay never runs start(); anchor the reap clock so a golden
	// single-frame emission is not born already "timed out".
	last_receive_ms_ = monotonic_milliseconds_();
	receive_clock_armed_ = true;
	phase_ = Phase::InMatch;
}

uint64_t JoinerConnection::milliseconds_since_last_receive() const {
	if (!receive_clock_armed_) return 0;
	const uint64_t now = monotonic_milliseconds_();
	return now >= last_receive_ms_ ? now - last_receive_ms_ : 0;
}

// The host closed the session on its own terms (docs/net/novaworld-net-re.md §5.64 — the punt
// families and the captured bytes). Retail's receiver stores the event only when its
// slot is still empty, so the FIRST record wins and a repeat cannot restate the cause, then it
// leaves the active state — which tears the connection down. Phase::Error is our terminal state:
// pump() stops producing keepalives, the deployment sub-state clears so a parked deploy screen
// stops taking clicks, and the owner's next session-loss read carries the decoded reason.
// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0 — the store gate @0x621d3c, the
//  terminal transition @0x621d53..0x621d6b -> CNapiNPConnection_TeardownActiveConnection @0x6253c0]
void JoinerConnection::on_host_disconnect(const DisconnectEvent &event) {
	if (!host_disconnect_reason_.empty()) return;
	// The client's exit-reason switch keys on DPC and only runs for the DC == 2 family; both
	// therefore belong in the reason, alongside the sender's own tag and text (for the
	// witnessed deploy-screen idle punt: DPC 33, DC 2, "LogPuntEvent", "t35").
	// [orig: the DC gate @0x4c6563 and the DPC switch @0x4c6569]
	host_disconnect_reason_ = "the host closed the session (reason " +
			std::to_string(event.dpc) + ", class " + std::to_string(event.dc) + ")";
	if (!event.ddstr.empty()) host_disconnect_reason_ += ": " + event.ddstr;
	if (!event.dstr.empty()) host_disconnect_reason_ += " " + event.dstr;
	fail(host_disconnect_reason_);
}

// [orig: the cs_dir0/cs_dir1 timeout_ms = 120000 reap installed by CNapiNetwork_Init
//  @0x4ca4a0 -> CNapiNetwork_OnDisconnectedFromServer @0x4c63d0, which clears the
//  session strings and maps the disconnect code onto g_mission_exit_reason]
bool JoinerConnection::session_lost() const {
	// An explicit close is terminal at any stage; the silence reap is the in-match fallback
	// for a host that vanishes without sending one.
	if (!host_disconnect_reason_.empty()) return true;
	if (phase_ != Phase::InMatch || !receive_clock_armed_) return false;
	return milliseconds_since_last_receive() >= JO_GAME_SESSION_TIMEOUT_MS;
}

std::string JoinerConnection::session_loss_reason() const {
	if (!host_disconnect_reason_.empty()) return host_disconnect_reason_;
	if (!session_lost()) return {};
	// Retail maps THIRTEEN distinct disconnect codes onto distinct exit reasons
	// (@0x4c63d0); this is the one cause with no code on the wire at all — silence
	// past the reap window (D-NET-177 residual).
	return "lost connection to the host (no traffic for " +
	       std::to_string(JO_GAME_SESSION_TIMEOUT_MS / 1000u) + " seconds)";
}

const char *JoinerConnection::post_auth_stage_name() const {
	switch (post_auth_stage_) {
	case PostAuthStage::Inactive: return "inactive";
	case PostAuthStage::AwaitServerSettings: return "awaiting the host's initial settings";
	case PostAuthStage::AwaitJoinAck: return "awaiting the JOIN acknowledgement";
	case PostAuthStage::AwaitPaddingProbe: return "awaiting the join probe";
	case PostAuthStage::AwaitGameStart: return "awaiting the game-start flag";
	case PostAuthStage::AwaitServerInfo: return "awaiting the server-info transfer";
	case PostAuthStage::AwaitMissionData: return "awaiting the mission-data transfer";
	case PostAuthStage::AwaitPlayerList: return "awaiting the player list";
	case PostAuthStage::AwaitInitialSyncTail: return "awaiting the pre-world sync tail";
	case PostAuthStage::AwaitWorldStreamEnd: return "awaiting the world stream";
	case PostAuthStage::AwaitDeployment: return "awaiting the loadout grants";
	case PostAuthStage::AwaitDeployPick: return "awaiting the player's deployment pick";
	case PostAuthStage::AwaitDeployRelease: return "awaiting the deployment release";
	case PostAuthStage::Complete: return "complete";
	}
	return "unknown";
}

// The retail teardown of an active (or still-connecting) client connection sends a burst of
// disconnect packets so a lossy LAN still hears the leave, then clears the key material. The
// burst size is cs_dir0.recv_max_per_tick clamped to [0, 32] — 4 for the JOINTOPERATIONS
// template. Without this leg the host's only teardown trigger never fires from our client and
// every leave leaks an admitted peer plus its spawned player.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 (burst loop @0x6253ef..0x625424);
//  CNapiNPConnection_SendDisconnectPacket @0x61f2a0; CNapiNetwork_Init @0x4ca4a0 stores
//  recv_max_per_tick = 4 @0x4cab60]
std::vector<std::vector<uint8_t>> JoinerConnection::disconnect() {
	if (goodbye_sent_ || conn_.server_sk == 0) return {};
	goodbye_sent_ = true;
	constexpr int kDisconnectSendCount = 4; // JO cs_dir0.recv_max_per_tick
	std::vector<uint8_t> datagram = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(conn_.server_sk));
	return std::vector<std::vector<uint8_t>>(
			static_cast<std::size_t>(kDisconnectSendCount), std::move(datagram));
}

void JoinerConnection::fail(std::string reason) {
	last_error_ = std::move(reason);
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	post_auth_stage_ = PostAuthStage::Inactive;
	session_last_send_ms_ = 0;
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	phase_ = Phase::Error;
}

} // namespace opennova::np
