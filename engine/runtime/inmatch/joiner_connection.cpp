#include <runtime/inmatch/joiner_connection.h>

#include <cstring>
#include <formats/mission/bms.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/flat_tlv.h>         // append_flat_tlv (the C2S 0x00 JOIN body)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h> // decode_cs_config_update
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/novacrypto/crc32.h>
#include <base/gameprofile/gameprofile.h>
#include <base/io/le.h>
#include <base/io/log.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Verbatim port of novaworld::JoinerSession (the client mirror), with SCRK/seq/ack stored on the
// type-2 NapiNPConnection conn_. Self-identification reads the authenticated connection owner in on_server_session.
namespace opennova::inmatch {

namespace {

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
		opennova::io::write_u32_le(echo.data() + off, static_cast<uint32_t>(value));
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
std::vector<uint8_t> build_join_request(
		std::string_view expansion, int32_t expansion_version_checksum,
		const std::vector<uint8_t> &cd_cookie) {
	std::vector<uint8_t> body;
	// One flat TLV: [name\0][LE16 size][size bytes]. A string value carries
	// its NUL inside the size; a binary value is emitted as-is.
	auto append_binary_tlv = [&](std::string_view name, const uint8_t *data,
	                             size_t size) {
		append_flat_tlv(body, name, data, static_cast<uint16_t>(size));
	};
	auto append_string_tlv = [&](std::string_view name, std::string_view value) {
		std::string with_nul(value);
		with_nul.push_back('\0');
		append_binary_tlv(name, reinterpret_cast<const uint8_t *>(with_nul.data()),
		                  with_nul.size());
	};
	if (!expansion.empty()) append_string_tlv("EXP", expansion);
	// The checksum rides as SIGNED decimal — retail formats its
	// g_ExpansionChecksum with "%ld", so a CRC with bit 31 set goes out
	// negative [orig: the sprintf @0x42a287, format "%ld" @0x7c3818]. A
	// checksum-less caller (no game root / base game / no loose version.txt)
	// keeps the golden "0" byte-for-byte (D-NET-166).
	append_string_tlv("VERSIONCRCSTRING",
			std::to_string(expansion_version_checksum));
	// The CD identity cookie (the packed PUB* blob) rides after VERSIONCRCSTRING,
	// as a BINARY TLV: it carries embedded NULs between the [name\0][value\0]
	// pairs and is already self-terminated, so the length is the raw blob size
	// (no appended NUL). Absent when the jar carried no PUB* cookie — the host
	// then punts code 23. The emit and the host's read are binary-witnessed;
	// the [name\0][value\0] packing of the blob itself is capture-witnessed
	// (an unnamed global feeds the writer). [orig: the "CD" TLV emit in
	// NapiNP_WriteClientAuthPayload @0x42a180 = NapiNP_WriteTLV(&unk_24CFDB8,
	// size); the gather Config_QueryMatchingEntries @0x64eb70; the host read
	// NapiNPServer_HandlePlayerJoinMessage @0x512aa0 "CD" -> the cookie buffer]
	// [wire: the stock .204 join capture 2026-08-31 — the blob's packing]
	if (!cd_cookie.empty()) {
		if (cd_cookie.size() > 0xFFFFu) {
			// A 16-bit size field cannot describe the blob; a truncated length
			// would put a malformed record on the wire, so the cookie is
			// dropped and the host's code-23 punt names the problem.
			io::logf(io::LogLevel::kWarn,
					"np joiner: CD cookie of %zu bytes exceeds the TLV size field; not sent",
					cd_cookie.size());
		} else {
			append_binary_tlv("CD", cd_cookie.data(), cd_cookie.size());
		}
	}
	return body;
}

std::vector<uint8_t> le32_pair(uint32_t first, uint32_t second) {
	std::vector<uint8_t> body;
	body.reserve(8);
	opennova::io::append_u32_le(body, first);
	opennova::io::append_u32_le(body, second);
	return body;
}

std::vector<uint8_t> le32_value(uint32_t value) {
	std::vector<uint8_t> body;
	body.reserve(4);
	opennova::io::append_u32_le(body, value);
	return body;
}

std::vector<uint8_t> build_retail_mission_status() {
	std::vector<uint8_t> body(13, 0);
	body[9] = 0x07;
	body[10] = 0x05;
	body[11] = 0x07;
	body[12] = 0x01;
	return body;
}

// The settings-flagged H:0x00 record, decoded by the shared receiver port
// (npwire decode_cs_config_update, CNapiNPConnection_HandleCSConfigUpdate).
bool is_cs_config_update(const ProtocolMessage &message) {
	return message.flags.settings_update &&
	       message.full_tag == (PROTOCOL_FULL_TAG_HIGH_BASE | hightag::CS_CONFIG_UPDATE);
}

// ACCEPTED single-packet assumption: both directions must arrive in ONE deframed packet.
// Retail groups them into a single sequence (§5.0d frame 6), our host frames them the same
// way on the fresh, 0x42-retry, and 0x44/0x84 paths, and reconstruction always re-frames a
// sequence whole — so a split pair is out-of-contract for a JO host, not a loss mode this
// per-packet detector must survive. The direction comes from the decoder (any nonzero byte
// is cs_dir0) and a mask-0 update is a no-op; the receiver has no length rule, so none is
// applied here.
bool has_initial_cs_config_pair(const std::vector<ProtocolMessage> &messages) {
	bool saw_direction[2] = {false, false};
	for (const ProtocolMessage &message : messages) {
		if (!is_cs_config_update(message)) continue;
		const CsConfigUpdate update =
				decode_cs_config_update(message.payload.data(), message.payload.size());
		if (update.mask != 0) saw_direction[update.to_dir0 ? 1 : 0] = true;
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
	// Stock-Avatars.def fresh-profile seed for socket-free/headless callers; the
	// Godot binding replaces it from the mounted Avatars.def + weapon.sav before
	// start(), so a user's real selection wins (retail_fresh_profile_character_vars).
	character_join_vars_ = retail_fresh_profile_character_vars();
	conn_.type = 2;                  // client-side connection (the joiner's view of the host)
	conn_.player_name = player_name_;
}

bool JoinerConnection::set_integrity_challenge_profile(std::string_view id) {
	integrity_challenge_profile_ = find_integrity_challenge_profile(id);
	return integrity_challenge_profile_ != nullptr;
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
	server_password_required_ = false;
	conn_.server_sk = 0;
	conn_.connection_id = 0;
	conn_.server_scrk.clear();
	conn_.seq = make_jo_game_session_sequencing();
	handshake_retry_clock_armed_ = false;
	handshake_last_send_ms_ = 0;
	client_join_start_ms_ = 0;
	post_auth_stage_ = PostAuthStage::Inactive;
	goodbye_sent_ = false;
	session_last_send_ms_ = 0;
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	pending_spawn_menu_request_ = false;
	spawn_ack_timestamp_ = 0;
	game_start_ack_timestamp_ = 0;
	rtt_ring_.fill(0);
	rtt_ring_index_ = 0;
	rtt_current_ms_ = 0;
	session_rtt_ms_ = 0;
	preload_ready_ = false;
	sync_tail_seen_ = false;
	player_list_seen_ = false;
	deployment_policy_seen_ = false;
	initial_loadout_grant_count_ = 0;
	initial_admission_complete_ = false;
	deployment_pick_sent_ = false;
	deployment_pick_sequence_ = 0;
	deployment_pick_sequence_unbound_ = false;
	deployment_reply_seen_ = false;
	has_self_handle_ = false;
	self_handle_ = 0;
	max_player_slot_ = 0;
	spawn_ = SelfSpawn{};
	assigned_team_ = 0; // re-latched from the fresh session's S2C 0x04 (the kit seam persists);
	spectator_mode_ = false;
	                    // zero like retail's byte_A85B48 until the assignment arrives
	class_allow_mask_ = 0x03FFu; // replaced by the fresh session's S2C 0x76
	current_player_class_ = 0;
	mission_known_ = false;
	mission_header_bytes_.clear();
	reset_terrain_load();
	server_name_.clear();
	mission_name_.clear();
	map_file_.clear();
	expansion_.clear();
	advertised_expansion_.clear();
	game_type_ = 0;
	mp_attributes_ = 0;
	session_max_players_ = 0;
	mission_metadata_transfer_id_ = 0;
	mission_metadata_total_size_ = 0;
	mission_metadata_fixed_bytes_.fill(0);
	mission_metadata_fixed_byte_mask_ = 0;
	mission_metadata_bytes_.clear();
	reloading_ = false;
	last_error_.clear();
	host_disconnect_reason_.clear();
	last_disconnect_event_ = DisconnectEvent{};
	disconnect_event_set_ = false;
	conn_.timeouts = CsConfig{}; // the template; the accepted 0x82 overlays it
	silence_timeout_latched_ = false;
	// Arm the receive clock at connect: the reap window is measured from the moment
	// this connection started expecting traffic, not from the first reply.
	last_receive_ms_ = monotonic_milliseconds_();
	receive_clock_armed_ = true;
	if (discovered_.present) {
		// A join from a discovered session: the record already holds the 0x81, so the
		// connection starts in the connect state with its HK and SF and sends the 0x42
		// [orig: CNapiNPConnection_InitFromSession @0x626320, state 3 @0x626496, its
		//  window's start @0x62648a; PumpStateMachine case 3 @0x629508].
		server_hk_ = discovered_.host_key;
		server_password_required_ = discovered_.password_required;
		advertised_expansion_ = discovered_.expansion;
		phase_ = Phase::Auth;
		handshake_retry_datagram_ = build_client_auth();
		handshake_last_send_ms_ = monotonic_milliseconds_();
		client_join_start_ms_ = handshake_last_send_ms_;
		handshake_retry_clock_armed_ = true;
		return handshake_retry_datagram_;
	}
	phase_ = Phase::Hello;
	handshake_retry_datagram_ = build_client_hello();
	handshake_last_send_ms_ = monotonic_milliseconds_();
	handshake_retry_clock_armed_ = true;
	return handshake_retry_datagram_;
}

std::vector<uint8_t> JoinerConnection::build_client_hello() {
	// The browse and connect legs use the same retail JO identity. The callsign
	// belongs in ClientAuth.NA (not CO) and is later echoed into the organic-spawn
	// named organic record by the host.
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
	                          client_hello_to_bytes(make_jointoperations_client_hello(client_index_)));
}

std::vector<uint8_t> JoinerConnection::build_client_auth() {
	ClientAuth auth = make_jointoperations_client_auth(
			client_index_, client_key_, server_hk_, player_name_, conn_.client_scrk);
	// [orig: CNapiNPConnection_SendClientJoin @0x61fe20, SF & 1 gates PW]
	if (server_password_required_) auth.pw = server_password_;
	// Lifecycle trace (kInfo -> MCP log ring): the APPID join token (decoded .joi
	// CK) on this ClientAuth. "0" on a NovaWorld join means the CK never arrived.
	io::logf(io::LogLevel::kInfo,
			"np joiner: ClientAuth APPID = '%s'", app_id_.c_str());
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
	// only (@0x4c7260 SetVersionString/SetCountryCode/TZB). VERSIONSTRING is the binary's own
	// sprintf("V%i.%i.%i.%i", 1, 7, 5, 7), the JO profile's version text, as every pinned
	// constant here is that binary's [orig:
	// Game_ParseCommandLineAndInit @0x4a7d81 into byte_B4C0B0, copied @0x569b5c]; COUNTRYCODE
	// is the install's CC.BIN, omitted when empty
	// [orig: @0x569b70; the strlen gate @0x4c385a] (D-NET-296).
	// [wire: retail-lan-host-join-session ClientAuth; orig: NapiNetConfig_LoadFromConnTags
	// @0x4c7260 -> Server_ValidatePlayerJoinRequest @0x512100]
	// BT stays "0" (retail's LAN default; the host's code-6/7 ban-type gate).
	// The decimal recovered from the .joi CK is the APPID CU, which is what the
	// NovaWorld host validates: a mismatch (or an absent APPID, our old bug) is
	// the code-9 punt. Witnessed live 2026-08-31 in a stock client's successful
	// join to a genuine .204 host: `CU BT="0"`, `CU APPID="3225"` (= atol(decoded
	// CK)). [orig: net_config.bt = atol(decoded CK) @0x569b8e; the host reads
	// the APPID tag into that same field and the BT tag into char_name —
	// NapiNetConfig_LoadFromConnTags @0x4c7260 — then
	// Server_ValidatePlayerJoinRequest @0x512100 compares it @0x5122c5]
	for (const auto &field : {
			std::pair<const char *, const char *>{"BT", "0"},
			std::pair<const char *, const char *>{"VN", "2"},
			std::pair<const char *, const char *>{"BN", "1"},
			std::pair<const char *, const char *>{"DB", "0"},
			std::pair<const char *, const char *>{"MBN", "20042002"},
			std::pair<const char *, const char *>{"SOPD", "180"},
			std::pair<const char *, const char *>{"VERSIONSTRING",
					gameprofile::gameprofile_by_id(gameprofile::GAME_JO)->version_text},
	}) {
		auth.cu.push_back(make_client_cu_chunk(2, field.first, field.second));
	}
	const std::string country_code = vfs_country_code(expansion_version_root_);
	if (!country_code.empty()) auth.cu.push_back(make_client_cu_chunk(2, "COUNTRYCODE", country_code));
	// APPID (the .joi CK decimal) rides only a NovaWorld join, in retail's wire
	// order right after COUNTRYCODE. LAN sends no APPID (app_id_ == "0"), the
	// host's code-9 gate being NovaWorld-transport only.
	if (app_id_ != "0" && !app_id_.empty()) {
		auth.cu.push_back(make_client_cu_chunk(2, "APPID", app_id_));
	}
	// JSP is the shared side/squad password string, before the character tags.
	// FID is a separate numeric identifier and is not a password field.
	// [orig: CNapiServerInfo_SerializeToSession @0x4C39D7..0x4C3A13]
	if (!join_password_.empty()) {
		auth.cu.push_back(make_client_cu_chunk(2, "JSP", join_password_));
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
	// Spectator is opt-in. Keeping both fields absent for Player preserves the
	// captured retail player-join bytes exactly.
	// [orig: CNapiServerInfo_SerializeToSession @0x4c3650 writes JSR/JSPP]
	if (join_role_ == JoinRole::Spectator) {
		auth.cu.push_back(make_client_cu_chunk(2, "JSR", "1"));
		if (!spectator_password_.empty()) {
			auth.cu.push_back(make_client_cu_chunk(
					2, "JSPP", spectator_password_));
		}
	}
	// TZB is the OS time-zone bias, omitted when 0; MPS the stock mpmaxpacketsize (no cfg loader).
	// [orig: tzb @0x569dbd, its gate @0x4c3da4; max_packet_bytes = maxPacketSize_338 @0x569ca0,
	//  cfg row @0x833380 default "1300"] (D-NET-296)
	if (time_zone_bias_ != 0)
		auth.cu.push_back(make_client_cu_chunk(2, "TZB", std::to_string(time_zone_bias_)));
	auth.cu.push_back(make_client_cu_chunk(2, "MPS", "1300"));
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
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
	ProtocolMessage reply =
			make_protocol_message(c2s::LOADED_MODEL_PAGE_REPLY, std::move(page));
	// Retail queues this producer with userParam=1, so the page is present on
	// its first physical send but is removed before NACK replay.
	// [orig: NapiNPClientMsg_0x068 @0x42DAA0, queue @0x42DAE5]
	reply.reliable = false;
	return reply;
}

namespace {
const char *joiner_phase_name(JoinerConnection::Phase phase) {
	switch (phase) {
	case JoinerConnection::Phase::Idle: return "idle";
	case JoinerConnection::Phase::Hello: return "hello";
	case JoinerConnection::Phase::Auth: return "auth";
	case JoinerConnection::Phase::Driving: return "driving";
	case JoinerConnection::Phase::InMatch: return "in-match";
	case JoinerConnection::Phase::Error: return "error";
	}
	return "unknown";
}
} // namespace

JoinerConnection::PollResult JoinerConnection::handle_datagram(const uint8_t *raw, std::size_t len) {
	// Diagnostic transition trace, emitted on every exit path: one kInfo line
	// per phase/admission-stage change per datagram, plus the authoritative
	// 0x7B mission identity the moment it is learned. The sink is silent unless
	// an embedder installed one (io/log.h), so live joins pay one branch here.
	struct TransitionTrace {
		JoinerConnection &c;
		Phase phase_before;
		PostAuthStage stage_before;
		bool mission_before;
		~TransitionTrace() {
			if (c.phase_ != phase_before || c.post_auth_stage_ != stage_before) {
				io::logf(io::LogLevel::kInfo,
						"np joiner: phase %s, stage: %s",
						joiner_phase_name(c.phase_), c.post_auth_stage_name());
			}
			if (c.mission_known_ && !mission_before) {
				io::logf(io::LogLevel::kInfo,
						"np joiner: host mission identity: server='%s' map='%s' expansion='%s'",
						c.server_name_.c_str(), c.map_file_.c_str(),
						c.expansion_.c_str());
			}
		}
	} transition_trace{*this, phase_, post_auth_stage_, mission_known_};

	PollResult out;
	if (session_lost() || phase_ == Phase::Error) return out;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(raw, len, opcode, body)) {
		return out; // bad envelope — drop quietly, don't kill the session
	}
	// The reap clock is NOT a per-datagram stamp: only an in-order admitted 0x83 (inside
	// on_server_session) refreshes it, exactly retail's ParseMessages-only write; a 0x84
	// resend list, a stale/other-key 0x83 and every ignored opcode leave it alone.
	// [orig: HandleSessionPacket @0x626A00 in-order leg -> ParseMessages @0x625d54;
	//  NapiNP_HandleResendList @0x62395f discards its GetTickCount; 0x81/0x82 stamp nothing]
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
	case SESSION_OPCODE_SERVER_GOODBYE:
		// Dispatched only for an ACTIVE connection (retail: `is_response && !conn_flag0` drops
		// it; our Driving/InMatch phases are the accepted-0x82 state 5).
		// [orig: g_NPOpcodeHandlers @0x849D90 entry 12 -> Nwu_HandleServerGoodbye @0x624310
		//  -> Nwu_HandleDisconnect(type 2) @0x623CE0, the active gate @0x623df2]
		if (phase_ == Phase::Driving || phase_ == Phase::InMatch) on_server_goodbye(body, out);
		break;
	case SESSION_OPCODE_SERVER_PING:
		// The outer connection ping rides the same active-connection gate as
		// the goodbye (retail resolves the type-2 connection and drops the
		// datagram unless conn_flag0 is set) [orig: g_NPOpcodeHandlers
		// @0x849D90 entry 11 -> Nwu_HandleServerPing @0x6242E0 ->
		// Nwu_HandlePing @0x623A70, the FindConnection + conn_flag0 gate].
		if (phase_ == Phase::Driving || phase_ == Phase::InMatch) on_server_ping(body, out);
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
	server_password_required_ = (sh.sf & 1u) != 0;
	// The host's ADVERTISED expansion: learned here and echoed in the C2S JOIN EXP TLV below,
	// nothing more (this member has no accessor). The value the resource-root preload
	// reconciles against before the world loads is the AUTHORITATIVE S2C 0x7B expansion
	// (expansion_, surfaced by ClientRuntime::expansion()) (D-NET-178).
	advertised_expansion_ = sh.sus2;
	phase_ = Phase::Auth;
	handshake_retry_datagram_ = build_client_auth();
	handshake_last_send_ms_ = monotonic_milliseconds_();
	// The connect state's start: its 30000 ms window runs from here [orig: @0x62648a].
	client_join_start_ms_ = handshake_last_send_ms_;
	handshake_retry_clock_armed_ = true;
	out.outbound.push_back(handshake_retry_datagram_);
}

JoinScreenStage JoinerConnection::join_screen_stage() const {
	switch (post_auth_stage_) {
	case PostAuthStage::Inactive:
		return JoinScreenStage::Joining;
	case PostAuthStage::AwaitServerSettings:
	case PostAuthStage::AwaitJoinAck:
		return JoinScreenStage::Connecting;
	case PostAuthStage::AwaitPaddingProbe:
	case PostAuthStage::AwaitGameStart:
		// [orig: the state-6 wait on the verification value @0x56a68f, the
		//  queue line only once it reads 1 @0x56a6cd]
		return verification_value_ == 1 ? JoinScreenStage::Queued : JoinScreenStage::Verifying;
	default:
		return JoinScreenStage::Starting;
	}
}

ConnectionErrorRecord JoinerConnection::connection_error_record() const {
	ConnectionErrorRecord record;
	if (last_join_reject_.set) {
		record.connect_error = last_join_reject_.jfc;
		record.connect_param = last_join_reject_.jfp;
		record.connect_text = last_join_reject_.jfs;
	}
	if (disconnect_event_set_) {
		record.disconnect_code = last_disconnect_event_.dc;
		record.disconnect_text = last_disconnect_event_.dstr;
		record.disconnect_param = last_disconnect_event_.dpc;
	}
	return record;
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
		// A CR=0 0x82 carries the NP-layer failure family in JFC and, for the
		// validate-callback family (JFC=14), the sub-reason in JFP. Spectator
		// failures are NOT this packet — they arrive post-admission as DPC
		// 14/15/16 description punts (on_host_disconnect below).
		// [orig: NapiNPProtocol_SendJoinRejection @0x620cd0; the JFC values at
		// the @0x62b750 call sites (3 HK / 4 PW / 5 empty NA / 6 disabled /
		// 7 PV2 / 9,10,15 CU / 11 chunk / 12,14 callback); the JFP reasons
		// CNapiNetwork_ValidateJoinRequest @0x4c61b0 (2 locked / 3 banned /
		// 4 full / 5 full with spectator-only slots); client store
		// NapiNP_HandleServerJoinResponse @0x629840]
		last_join_reject_.set = true;
		last_join_reject_.jfc = sa.jfc;
		last_join_reject_.jfp = sa.jfp;
		last_join_reject_.jfs = sa.jfs;
		io::logf(io::LogLevel::kWarn,
				"np joiner: join rejected: jfc=%u jfp=%u jfs='%s'",
				static_cast<unsigned>(sa.jfc), static_cast<unsigned>(sa.jfp),
				sa.jfs.c_str());
		// The player-facing text is retail's reason string over this record
		// (disconnect_reason.h), resolved where gameerr.bin is loaded; the
		// connection's own error names the gameerr entry it reads.
		const ConnectionErrorRecord record = connection_error_record();
		fail("join refused (" + disconnect_reason_key(&record).key + ")");
		return;
	}
	conn_.server_sk = sa.sk;      // session_id for our outbound 0x43s
	conn_.server_scrk = sa.scrk;  // decrypts inbound 0x83 inner streams
	conn_.connection_id = sa.mi;  // [orig: 0x82 MI TLV = the host-assigned dcb/ConnectionId; the
	                              // client stores it on its own NapiNPConnection (+0x18,
	                              // NapiNP_GetLocalConnectionId @0x4c6d40) and echoes it in the 0x48
	                              // client-ack so the host stamps it into our 0x0C ownerConnectionId]
	// The host's CS block overlays this connection's cs_dir0 template: CLIENT-direction
	// (byte 1) entries land in cs_dir0, the block this connection's pumps read — the reap
	// (field 0), the teardown burst (1), the empty and active send intervals (4, 5), the
	// out-of-order queue bound (10), the pool bound (11), the packet ceiling (13) and the
	// packets per build (14) — so a host `_NSTMOUT.TXT` override (or a NEVER -1) or its
	// `mpmaxpacketsize` reaches us. Fields the 0x82 omits keep the template.
	// [orig: NapiNP_HandleServerJoinResponse @0x629840 — the template seed @0x6299ae, the
	//  CS overlay @0x629b4c..0x629b75, the copy onto the connection @0x629d72/@0x629d89]
	for (const CsField &field : sa.client_cs)
		apply_cs_field(conn_.timeouts, field.field_index,
				static_cast<int32_t>(field.value));
	sync_session_sequencing_limits(conn_.seq, conn_.timeouts);
	phase_ = Phase::Driving;
	// State-5 entry initializes the reap clock: the 120 s window runs from the accepted
	// 0x82 onward, through every join stage, with no gameplay gate.
	// [orig: @0x629eac conn_state = 5 -> CNapiNPConnection_OnStateChange @0x626060, the
	//  conn+0x5E8 store @0x62612f; PumpStateMachine case 5 @0x6295a2..0x62961c]
	last_receive_ms_ = monotonic_milliseconds_();
	receive_clock_armed_ = true;
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	session_ack_pending_ = false;
	s2c_reassembly_ = {};
	// Retail first sends a sequenced settings packet after 0x82. Its receipt drives the header-only
	// ACK + C2S 0x00 JOIN pair in on_server_session; do not skip straight to 0x01. This remains active
	// while the binding holds world_ready_ false so a joiner can discover what it must load.
	post_auth_stage_ = PostAuthStage::AwaitServerSettings;
}

void JoinerConnection::reset_terrain_load() {
	terrain_til_state_ = TerrainTilState::Absent;
	terrain_til_staging_.clear();
	terrain_tile_count_ = 0;
	terrain_next_index_ = 0;
	terrain_til_bytes_.clear();
}

void JoinerConnection::invalidate_terrain_load() {
	terrain_til_state_ = TerrainTilState::Invalid;
	terrain_til_staging_.clear();
	terrain_tile_count_ = 0;
	terrain_next_index_ = 0;
	terrain_til_bytes_.clear();
}

void JoinerConnection::retain_terrain_load_page(
		const std::vector<uint8_t> &body) {
	if (terrain_til_state_ == TerrainTilState::Invalid) return;
	if (terrain_til_state_ == TerrainTilState::Complete) {
		invalidate_terrain_load();
		return;
	}
	TerrainLoadBatch batch;
	if (!decode_terrain_load_batch(body.data(), body.size(), batch)) {
		invalidate_terrain_load();
		return;
	}

	// The canonical retail writer advances a signed 16-bit cursor. Enforcing its
	// exact contiguous output shape rejects gaps/overlaps that retail's trusting
	// reader would otherwise overwrite, without changing valid-host behavior.
	// [orig: Terrain_SerializeTiles @0x6080F0]
	constexpr uint32_t kMaxTileCount =
			static_cast<uint32_t>(std::numeric_limits<int16_t>::max());
	if (batch.has_header) {
		if (terrain_til_state_ != TerrainTilState::Absent ||
				batch.tile_count == 0 || batch.tile_count > kMaxTileCount ||
				batch.start_index != 0 || batch.end_index == 0 ||
				batch.end_index > batch.tile_count) {
			invalidate_terrain_load();
			return;
		}

		terrain_til_staging_.assign(
				16u + 12u * static_cast<std::size_t>(batch.tile_count), 0u);
		// Preserve all 16 header bytes exactly as sent. In particular hdr2/hdr3
		// are opaque file bytes; reconstructing them from decoded integers would
		// introduce an unnecessary normalization seam.
		std::copy(body.begin() + 4, body.begin() + 20,
				terrain_til_staging_.begin());
		terrain_tile_count_ = batch.tile_count;
		terrain_next_index_ = 0;
		terrain_til_bytes_.clear();
	} else {
		if (terrain_til_state_ != TerrainTilState::Receiving ||
				batch.start_index != terrain_next_index_ ||
				batch.end_index <= batch.start_index ||
				batch.end_index > terrain_tile_count_) {
			invalidate_terrain_load();
			return;
		}
	}

	const std::size_t source_offset = batch.has_header ? 20u : 4u;
	const std::size_t record_count =
			static_cast<std::size_t>(batch.end_index - batch.start_index);
	std::copy(body.begin() + static_cast<std::ptrdiff_t>(source_offset),
			body.begin() + static_cast<std::ptrdiff_t>(
					source_offset + 12u * record_count),
			terrain_til_staging_.begin() + static_cast<std::ptrdiff_t>(
					16u + 12u * batch.start_index));
	terrain_next_index_ = batch.end_index;
	if (terrain_next_index_ == terrain_tile_count_) {
		terrain_til_bytes_ = terrain_til_staging_;
		terrain_til_state_ = TerrainTilState::Complete;
	} else {
		terrain_til_state_ = TerrainTilState::Receiving;
	}
}

void JoinerConnection::retain_server_info_chunk(const FileTransferChunk &chunk) {
	const uint64_t chunk_end = uint64_t(chunk.chunk_offset) + chunk.chunk_size;
	if (chunk.chunk_offset > chunk.total_size || chunk_end > chunk.total_size ||
			chunk.total_size > (1u << 20))
		return;
	if (chunk.transfer_id != server_info_transfer_id_ || chunk.chunk_offset == 0) {
		server_info_transfer_id_ = chunk.transfer_id;
		server_info_bytes_.assign(chunk.total_size, 0);
	}
	if (server_info_bytes_.size() != chunk.total_size) return;
	std::memcpy(server_info_bytes_.data() + chunk.chunk_offset, chunk.chunk_data,
			chunk.chunk_size);
	// The completed stream parses into the session variables
	// [orig: SaveFile_SendAndWaitForServerAck @0x5205d0 ->
	//  Client_ParseServerSessionVariables @0x5202f0].
	if (chunk.is_final())
		decode_session_vars(server_info_bytes_.data(), server_info_bytes_.size(), session_vars_);
}

void JoinerConnection::retain_mission_metadata_chunk(
		const FileTransferChunk &chunk) {
	// The fixed block's dwords at 36 (the published player cap) and 44
	// (mpattrib) ride one retained [36, 48) window, so either dword still
	// assembles when a chunk boundary splits it.
	constexpr uint32_t kFixedWindowOffset = 36;
	constexpr uint32_t kFixedWindowSize = 12;
	constexpr uint16_t kMaxPlayersMask = 0x000Fu; // window bytes 0..3 = offset 36
	constexpr uint16_t kMpAttributesMask = 0x0F00u; // window bytes 8..11 = offset 44
	const uint64_t chunk_end = uint64_t(chunk.chunk_offset) + chunk.chunk_size;
	if (chunk.total_size < kFixedWindowOffset + kFixedWindowSize ||
			chunk.chunk_offset > chunk.total_size ||
			chunk_end > chunk.total_size)
		return;

	const bool new_transfer =
			chunk.transfer_id != mission_metadata_transfer_id_ ||
			chunk.total_size != mission_metadata_total_size_;
	if (new_transfer || chunk.chunk_offset == 0) {
		mission_metadata_transfer_id_ = chunk.transfer_id;
		mission_metadata_total_size_ = chunk.total_size;
		mission_metadata_fixed_bytes_.fill(0);
		mission_metadata_fixed_byte_mask_ = 0;
		mission_metadata_bytes_.assign(chunk.total_size, 0);
	}
	if (mission_metadata_bytes_.size() == chunk.total_size)
		std::memcpy(mission_metadata_bytes_.data() + chunk.chunk_offset, chunk.chunk_data,
				chunk.chunk_size);

	for (uint32_t i = 0; i < kFixedWindowSize; ++i) {
		const uint32_t absolute = kFixedWindowOffset + i;
		if (absolute < chunk.chunk_offset || absolute >= chunk_end) continue;
		mission_metadata_fixed_bytes_[i] =
				chunk.chunk_data[absolute - chunk.chunk_offset];
		mission_metadata_fixed_byte_mask_ |= static_cast<uint16_t>(1u << i);
	}
	const auto window_dword = [&](std::size_t at) {
		return static_cast<uint32_t>(mission_metadata_fixed_bytes_[at]) |
				(static_cast<uint32_t>(mission_metadata_fixed_bytes_[at + 1]) << 8) |
				(static_cast<uint32_t>(mission_metadata_fixed_bytes_[at + 2]) << 16) |
				(static_cast<uint32_t>(mission_metadata_fixed_bytes_[at + 3]) << 24);
	};
	if ((mission_metadata_fixed_byte_mask_ & kMaxPlayersMask) == kMaxPlayersMask)
		session_max_players_ = window_dword(0);
	if ((mission_metadata_fixed_byte_mask_ & kMpAttributesMask) == kMpAttributesMask)
		mp_attributes_ = window_dword(8);
}

void JoinerConnection::retire_self_handle(uint16_t handle) {
	if (!has_self_handle_ || self_handle_ != handle) return;
	has_self_handle_ = false;
	self_handle_ = 0;
	spawn_ = {};
}

void JoinerConnection::apply_self_spawn(uint16_t handle, bool has_body, uint32_t owner,
		uint16_t flags, const SelfSpawn &spawn, PollResult &out) {
	// A replay seed may supply H without authenticating a connection ID. An
	// unowned row (owner 0) cannot establish numeric identity in that case.
	if (conn_.connection_id == 0) {
		if (!has_body) retire_self_handle(handle);
		return;
	}
	// Retail scans only the used pool-0 player entries for the local dcb.
	// [orig: Player_FindLocalPlayerEntity @0x4E0090]
	const bool is_self = has_body &&
			handle < world::retail_pool_capacity(0) && (flags & 0x100u) != 0 &&
			owner == conn_.connection_id;
	if (!is_self) {
		retire_self_handle(handle);
		return;
	}
	self_handle_ = handle;
	has_self_handle_ = true;
	spawn_ = spawn;
	if (deployment_reply_seen_ && phase_ != Phase::InMatch) {
		phase_ = Phase::InMatch;
		out.reached_in_match = true;
	}
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
		// The reap clock: stamped by ParseMessages, which only an in-order packet (or the
		// drain that closes a gap) reaches; a zero-message keepalive counts. Duplicates,
		// futures and stale receiver keys return earlier and stamp nothing.
		// [orig: HandleSessionPacket @0x626beb..0x626bfc -> ParseMessages @0x625d54;
		//  dup @0x626c03 / future @0x626c0c..0x626c3a / seq 0 @0x626bcc]
		last_receive_ms_ = monotonic_milliseconds_();
		receive_clock_armed_ = true;
	}
	bool received_settings = false;
	for (const SessionDeframeAdmission::Packet &packet : admission.packets) {
		if (has_initial_cs_config_pair(packet.messages)) {
			received_settings = true;
			break;
		}
	}
	// Deframing admits physical records in sequence order, including packets
	// released from a just-closed gap. Fold FIRST/MID/FINAL pieces here, before
	// either metadata inspection or gameplay dispatch. The semantic record is
	// associated with the ACK carried by its final containing packet.
	std::vector<ProtocolMessage> semantic_messages;
	std::vector<uint32_t> containing_packet_ack;
	semantic_messages.reserve(messages.size());
	containing_packet_ack.reserve(messages.size());
	for (const SessionDeframeAdmission::Packet &packet : admission.packets) {
		for (const ProtocolMessage &physical : packet.messages) {
			std::vector<uint8_t> payload;
			bool was_fragmented = false;
			if (!reassemble_protocol_payload(
						s2c_reassembly_, physical, payload, &was_fragmented))
				continue;
			if (!was_fragmented) {
				semantic_messages.push_back(physical);
			} else {
				// Fragment and physical-length bits do not belong to the
				// reassembled semantic record. Preserve dispatch-table and skip
				// metadata, then choose the appropriate completed-body length form.
				constexpr uint8_t kSemanticFlagMask =
						PROTOCOL_MSG_FLAG_SETTINGS_UPDATE |
						PROTOCOL_MSG_FLAG_SKIP1 | PROTOCOL_MSG_FLAG_SKIP2 | 0x01u;
				uint8_t semantic_flags =
						static_cast<uint8_t>(physical.flags.raw & kSemanticFlagMask);
				if (!payload.empty()) {
					semantic_flags = static_cast<uint8_t>(
							semantic_flags |
							(payload.size() > 0xFFu ? PROTOCOL_MSG_FLAG_LEN16
							                          : PROTOCOL_MSG_FLAG_LEN8));
				}
				ProtocolMessage semantic = make_protocol_message(
						physical.tag, std::move(payload), semantic_flags);
				semantic.skip_bytes = physical.skip_bytes;
				semantic_messages.push_back(std::move(semantic));
			}
			containing_packet_ack.push_back(packet.header.ack_count);
		}
	}
	messages = std::move(semantic_messages);
	if (containing_packet_ack.size() != messages.size())
		containing_packet_ack.assign(messages.size(), hdr.ack_count);
	std::vector<ProtocolMessage> periodic_replies;
	// periodic_replies' size when each reducer entry was emitted: the replies its handler
	// produced follow it.
	std::vector<std::size_t> reducer_reply_start;
	// The fixed 0x0F header carries gameFlags at byte 22. Bit 0 says the host
	// has spawn zones and will keep this player hidden until C2S 0x0E. Keep an
	// explicit unknown state because OpenNova and retail may send the initial
	// 0x5A grants before 0x0F, either in this packet or an earlier packet.
	for (const ProtocolMessage &m : messages) {
		// A cs_dir0 update stores every slot it carries: field 3 (the send holdoff) for
		// ClientRuntime's gate, the rest onto this connection's cs_dir0 — a retail host's
		// 0x2000 update is the negotiated packet ceiling (field 13).
		// [orig: CNapiNPConnection_HandleCSConfigUpdate @0x621940, the cs_dir0 store
		//  @0x6219c8; the host senders NapiNPServer_UpdateHoldoffTicks @0x4c5f5e (mask 8),
		//  NapiNPServer_HandleNewConnection @0x4c81bd (mask 0x2000)]
		if (is_cs_config_update(m)) {
			const CsConfigUpdate settings =
					decode_cs_config_update(m.payload.data(), m.payload.size());
			if (settings.to_dir0) {
				if ((settings.written & (1u << 3)) != 0) {
					out.send_holdoff_set = true;
					out.send_holdoff = static_cast<uint32_t>(settings.value[3]);
				}
				for (uint32_t slot = 0; slot < static_cast<uint32_t>(kCsConfigSlots); ++slot) {
					if ((settings.written & (1u << slot)) != 0)
						apply_cs_field(conn_.timeouts, slot, settings.value[slot]);
				}
				sync_session_sequencing_limits(conn_.seq, conn_.timeouts);
			}
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
			// The S2C 0x0F world-state-load is the deployment-policy marker that
			// (with both initial 0x5A grants) completes initial admission. Its
			// game_flags bit0 (payload[22] & 1) raises g_DeployScreenActive.
			// Completing admission must preserve the user's later selection:
			// a spawn-zone host can still require C2S 0x0E to release its hold.
			// [orig: NapiNPClientMsg_0x00F @0x42e2ed]
			deployment_policy_seen_ = true;
			// The 0x0F ends a reload's last wait [orig: Game_StartMission's
			// 0x0B loop @0x52629B..0x5262E5].
			reloading_ = false;
			// The authority's ammo-pool image rides the fixed span at body offset
			// 23, ahead of the off-wire waypoint gate, so it reads without the
			// gametype hint. Retail copies all 128 dwords into g_LocalAmmoPools
			// (a short body reads zeros for the missing tail @0x42e337).
			// [orig: @0x42e324..0x42e34a]
			out.ammo_pools_set = true;
			out.ammo_pools.fill(0);
			for (size_t i = 0; i < out.ammo_pools.size(); ++i) {
				const size_t at = 23u + i * 4u;
				if (at + 4u > m.payload.size()) break;
				const uint8_t *p = m.payload.data() + at;
				out.ammo_pools[i] = static_cast<int32_t>(
						uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
						(uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
			}
		}
	}
	auto release_deployment = [&](bool deployment_complete) {
		const bool release_edge = !deployment_reply_seen_;
		deployment_reply_seen_ = true;
		if (deployment_complete)
			post_auth_stage_ = PostAuthStage::Complete;
		if (has_self_handle_) {
			const bool entered_in_match = phase_ != Phase::InMatch;
			phase_ = Phase::InMatch;
			// Multiple release conditions can fold into one datagram. Preserve an
			// earlier owner-ID match/release edge instead of overwriting it with a later
			// idempotent UI-completion call.
			out.reached_in_match =
					out.reached_in_match || entered_in_match || release_edge;
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
		// always passes, and its CRC gate only fires when the two installs' loose
		// version.txt files differ (D-NET-166: VERSIONCRCSTRING carries our real
		// checksum now). The claim is still made good on OUR side: the preload re-mounts
		// the resource root onto this expansion and ABORTS the join when it is not
		// installed, rather than entering the world with a different ADM index space
		// than the host (D-NET-178).
		// Retail switched the ONE global mount onto the host's expansion BEFORE
		// connecting, so its Expansion_LoadAssets pass has already stamped
		// g_ExpansionChecksum for exactly this name; computing it here from the
		// binding-supplied install root and the latched SUS2 name reads the same
		// loose file at the same point in the exchange [orig: Expansion_SwitchTo
		// @0x5688c0 -> Expansion_LoadAssets @0x4a4885; the JOIN write @0x42a2b4].
		out.outbound.push_back(frame_inner_pending(
				0x00, build_join_request(advertised_expansion_,
						vfs_expansion_version_checksum(
								expansion_version_root_,
								advertised_expansion_),
						cd_cookie_)));
		io::logf(io::LogLevel::kInfo,
				"np joiner: 0x00 JOIN CD identity cookie = %zu bytes",
				cd_cookie_.size());
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
				// An active retail client records the description, marks its NP
				// connection pending-disconnect, and drives the ordinary teardown:
				// four identical keyed ClientGoodbye datagrams for the JO template.
				// Queue them before fail() makes this runtime terminal so a retail
				// host can release the player immediately instead of waiting for its
				// 120-second receive timeout.
				// [orig: HandleDescriptionPacket @0x621D53..0x621D6B ->
				// TeardownActiveConnection @0x6253C0]
				// Teardown owns this receive result. Admission ACK/JOIN traffic and
				// metadata discovered earlier in the same physical packet must not
				// precede or survive the terminal goodbye burst.
				out = PollResult{};
				// The received record is latched with the LOCAL role (2) before the
				// burst so the 0x46 echoes the host's punt field for field.
				// [orig: HandleDescriptionPacket builds the record with the local role
				//  @0x621cca.., latches if !valid @0x621d3c..0x621d4d]
				latch_disconnect_event(event, 2);
				std::vector<std::vector<uint8_t>> goodbye = disconnect();
				out.outbound.insert(out.outbound.end(),
						std::make_move_iterator(goodbye.begin()),
						std::make_move_iterator(goodbye.end()));
				on_host_disconnect(event);
				return; // the session is closed; nothing later in this packet applies
			}
		}
		// The initial settings records use low tag 0x00 with the high/settings flag set. They are
		// connection control, not the regular S2C 0x00 JOIN acknowledgement below.
		if (m.flags.settings_update || m.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;

		// Every admitted game message reaches the client in wire order, including
		// handlers that need no connection-level response. A selective list here
		// silently lost text commands, chat, and full-entity repair replies while
		// reducer-only and loopback tests still passed.
		// [orig: g_NPMsgInfoClient @0x82AE28]
		out.inbound_reducer.emplace_back(m.tag, m.payload);
		reducer_reply_start.push_back(periodic_replies.size());
		on_list_walk_page(m, periodic_replies);

		// Dispatch records in wire order. Keeping this out of the metadata pre-pass
		// ensures a 0x39 before a same-packet 0x5A still sees the prior class.
		bool valid_weapon_loadout = false;
		if (m.tag == s2c::WEAPON_LOADOUT) {
			WeaponLoadout loadout;
			if (decode_weapon_loadout(
					m.payload.data(), m.payload.size(), loadout)) {
				valid_weapon_loadout = true;
				// The handler's tail clears dword_81474C for every valid apply,
				// including a loadout echo unrelated to deploy-UI completion.
				out.gameplay_release_applied = true;
				current_player_class_ = loadout.avatar_class;
			}
		}

		if (m.tag == s2c::SYNC_STATE) {
			verification_value_ = m.payload.size() >= 4 ? io::read_u32_le(m.payload.data()) : 0;
		} else if (m.tag == s2c::SYNC_TICK) {
			fold_join_queue_record(join_queue_, m.payload.data(), m.payload.size(),
					monotonic_milliseconds_());
		}
		if (m.tag == s2c::INIT && post_auth_stage_ == PostAuthStage::AwaitJoinAck) {
			// Golden retail frames 9-11: acknowledge S2C 0x00 with a header-only sequence, then post
			// the one-byte zero form body. An empty 0x01 is not accepted by the retail host FSM.
			out.outbound.push_back(frame_session({}));
			out.outbound.push_back(frame_inner_pending(0x01, {0x00}));
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
				out.outbound.push_back(frame_inner_pending(
						0x02, build_join_padding_echo(probe, net_frame_counter_)));
				post_auth_stage_ = PostAuthStage::AwaitGameStart;
			}
		} else if (m.tag == s2c::GAME_START_SIGNAL &&
		           post_auth_stage_ == PostAuthStage::AwaitGameStart &&
		           !m.payload.empty() && m.payload[0] != 0) {
			// Golden frame 17: the game-start flag completes verification with ONE grouped C2S
			// packet. This exchange runs before wire-header world construction; 0x48 echoes ServerAuth.MI.
			out.outbound.push_back(frame_session({
					make_protocol_message(c2s::GAME_START_ACK, std::vector<uint8_t>(4, 0)),
					make_protocol_message(
							c2s::AUTO_MEDIC_PREFERENCE,
							encode_auto_medic_preference(
									AutoMedicPreference{auto_medic_disabled_})),
					make_protocol_message(c2s::CLIENT_ACK, le32_value(conn_.connection_id)),
					make_protocol_message(c2s::PING, {}),
					// [the last 0x60 id, 0]: eight zero bytes on a first join
					// [orig: SaveFile_SendAndWaitForServerAck @0x5204B0, dword_A822C0].
					make_protocol_message(c2s::FILE_CHUNK_REQUEST,
							le32_pair(server_info_transfer_id_, 0)),
			}));
			post_auth_stage_ = PostAuthStage::AwaitServerInfo;
		} else if (m.tag == s2c::FILE_TRANSFER_CHUNK &&
		           post_auth_stage_ == PostAuthStage::AwaitServerInfo) {
			FileTransferChunk chunk;
			if (decode_file_transfer_chunk(
					m.payload.data(), m.payload.size(), chunk)) {
				retain_server_info_chunk(chunk);
				if (!chunk.is_final()) {
					const uint32_t next_offset = static_cast<uint32_t>(
							uint64_t(chunk.chunk_offset) + chunk.chunk_size);
					out.outbound.push_back(frame_inner_pending(
							0x33, le32_pair(chunk.transfer_id, next_offset)));
				} else {
					// Golden frames 19-20 are separate packets: state re-broadcast, then the
					// initial eight-zero mission-data request.
					out.outbound.push_back(frame_inner_pending(0x47, {}));
					// [the last 0x64 id, 0] [orig: CNapiGameSession_InitRandomSeedOrRequest
					// @0x51E8F0, dword_A822C4].
					out.outbound.push_back(frame_inner_pending(
							0x37, le32_pair(mission_metadata_transfer_id_, 0)));
					post_auth_stage_ = PostAuthStage::AwaitMissionData;
				}
			}
		} else if (m.tag == s2c::MISSION_DATA_CHUNK &&
		           post_auth_stage_ == PostAuthStage::AwaitMissionData) {
			FileTransferChunk chunk;
			if (decode_file_transfer_chunk(
					m.payload.data(), m.payload.size(), chunk)) {
				retain_mission_metadata_chunk(chunk);
				if (!chunk.is_final()) {
					const uint32_t next_offset = static_cast<uint32_t>(
							uint64_t(chunk.chunk_offset) + chunk.chunk_size);
					out.outbound.push_back(frame_inner_pending(
							0x37, le32_pair(chunk.transfer_id, next_offset)));
				} else {
					// The last chunk copies the server name and the map's file out
					// of the block: on a map change, the next map's g_MapFileName
					// [orig: NapiNPClientMsg_HandleMissionDataChunk @0x4324CC (the
					//  server name, block+52), @0x4324DD (g_MapFileName, block+84),
					//  each Napi_CopyString(.., 32)].
					const auto block_string = [&](std::size_t at) {
						std::string text;
						for (std::size_t i = at; i < at + 31 && i < mission_metadata_bytes_.size() &&
								mission_metadata_bytes_[i] != 0; ++i)
							text.push_back(static_cast<char>(mission_metadata_bytes_[i]));
						return text;
					};
					if (mission_metadata_bytes_.size() >= 116) {
						server_name_ = block_string(52);
						map_file_ = block_string(84);
					}
					post_auth_stage_ = PostAuthStage::AwaitPlayerList;
					out.mission_started = true;
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
				// Beyond the join-gate latch below, the body IS the Tab
				// board's data (D-HUD-24): surface it on the canonical
				// reducer stream so ClientReplicaPipeline::apply_player_list
				// folds it — ClientRuntime applies inbound_reducer alone.
				// [orig: NapiNPClientMsg_PlayerList @0x42FAE0]
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
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
		} else if (m.tag == s2c::END_ROUND_HEADER) {
			EndRoundHeader header;
			// A joiner is in-session by definition, so the header form is
			// the game-type bit alone. [orig: NapiNPClientMsg_0x01D form
			// pick @0x43086c..0x430883]
			if (decode_end_round_header(
					m.payload.data(), m.payload.size(),
					(game_type_ & 0x10000u) == 0, header)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
				// A valid 0x1D immediately starts the requester-driven board stream
				// at zero. [orig: NapiNPClientMsg_0x01D @0x430840 queues
				// reliable C2S 0x2B {0}]
				periodic_replies.push_back(make_protocol_message(
						c2s::END_ROUND_STATS_REQUEST,
						encode_end_round_stats_request(0)));
			}
		} else if (m.tag == s2c::END_ROUND_STATS) {
			EndRoundStatsChunk chunk;
			if (decode_end_round_stats_chunk(
					m.payload.data(), m.payload.size(), chunk)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
				if (!chunk.complete()) {
					const uint16_t next = static_cast<uint16_t>(
							chunk.chunk_offset + chunk.chunk.size());
					periodic_replies.push_back(make_protocol_message(
							c2s::END_ROUND_STATS_REQUEST,
							encode_end_round_stats_request(next)));
				}
			}
		} else if (m.tag == s2c::BMS_HEADER) {
			// Retail memcpy's the received 0x268-byte block verbatim. Retain the
			// exact body rather than parsing/re-encoding it: ignored/padding bytes
			// remain authoritative inputs to the later terrain/environment load.
			// Short or long bodies are not a complete header and cannot replace a
			// previously accepted one.
			// [orig: NapiNPClientMsg_HandleBMSHeader (0x0B) @0x422660]
			if (m.payload.size() == opennova::bms::kHeaderSize)
				mission_header_bytes_ = m.payload;
		} else if (m.tag == s2c::SESSION_CONFIG) {
			// Our initial-state burst carries the same g_GameType in the fixed
			// session-config block before any live frame. Retail keeps this equal
			// to the later 0x7B `extra` value.
			SessionConfig config;
			if (decode_session_config(m.payload.data(), m.payload.size(), config))
				game_type_ = static_cast<uint32_t>(config.fields[3]);
		} else if (m.tag == s2c::CLASS_ALLOW_MASK) {
			// Retail's handler reads exactly one little-endian word and clears the
			// global to zero when the packet is too short.
			// [orig: NapiNPClientMsg_HandleClassAllowMask @0x42d540]
			class_allow_mask_ = m.payload.size() >= 2
					? static_cast<uint16_t>(m.payload[0]) |
						  (static_cast<uint16_t>(m.payload[1]) << 8)
					: 0u;
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
		} else if (m.tag == s2c::TERRAIN_LOAD) {
			retain_terrain_load_page(m.payload);
		} else if (m.tag == s2c::ENTITY_SPAWN_BATCH) {
			// The spawn's entity+120 is the connection ID, not entity flags. The
			// local player is a pool-0 player whose owner matches ServerAuth.MI.
			// [orig: NapiNPClientMsg_0x00C @0x42E864; Player_FindLocalPlayerEntity
			// @0x4E0090; NapiNP_GetLocalConnectionId @0x4C6D40]
			OrganicSpawnBatch batch;
			if (!decode_organic_spawn_batch(m.payload.data(), m.payload.size(), batch) &&
					batch.last_record_partial && !batch.records.empty())
				batch.records.pop_back();
			// Match the replica fold: complete records survive a malformed tail.
			for (const OrganicSpawnRecord &rec : batch.records) {
				// The original stops the page at the first out-of-capacity slot.
				// [orig: NapiNPClientMsg_0x00C @0x42E7CE]
				if ((rec.slot_id & 0x0FFFu) >= world::retail_pool_capacity(rec.slot_id >> 12))
					break;
				SelfSpawn spawn;
				spawn.pos_x = rec.pos_x;
				spawn.pos_y = rec.pos_y;
				spawn.pos_z = rec.pos_z;
				spawn.orientation = rec.orientation;
				spawn.team = rec.team;
				spawn.item_type_id = rec.item_type_id;
				spawn.anim_slot = rec.anim_slot;
				spawn.net_id = rec.net_id;
				apply_self_spawn(rec.slot_id, rec.has_body(), rec.owner_connection_id,
						rec.minimap_flags, spawn, out);
			}
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::FULL_ENTITY_SPAWN) {
			// A repair destroys and rebuilds this slot, even for the same type.
			// Ownership can disappear or move here without another organic page.
			// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780]
			FullEntitySpawnRecord rec;
			if (decode_full_entity_spawn(m.payload.data(), m.payload.size(), rec)) {
				SelfSpawn spawn;
				spawn.pos_x = rec.pos_x;
				spawn.pos_y = rec.pos_y;
				spawn.pos_z = rec.pos_z;
				spawn.orientation = static_cast<int32_t>(uint32_t(rec.heading_hi) << 16);
				spawn.team = rec.team;
				spawn.item_type_id = rec.item_type_id;
				spawn.anim_slot = rec.anim_slot;
				spawn.net_id = rec.net_id;
				apply_self_spawn(rec.slot_id, rec.item_type != 0, rec.entity_flags,
						rec.minimap_flags, spawn, out);
			}
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::POOL_SPAWN || m.tag == s2c::STATIC_ENTITY_BATCH || m.tag == s2c::POOL3_SYNC) {
			// The rest of the load-time world stream (§5.2a): pool-1 spawns / pool-2 statics /
			// pool-3 markers. Surface raw for the caller's ClientReplicaPipeline to upsert.
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::SESSION_SLOT_CONFIG) {
			// S2C 0x04 slot assignment: the tail byte is this joiner's server-assigned team —
			// retail's byte_A85B48, the 0x2F loadout-submit header byte 0. A retail host always
			// writes the full 24-byte body [orig: NetPacket_WriteSlotAssignment @0x502b30];
			// latch only that witnessed shape and keep the golden default otherwise.
			// [orig: NapiNPClientMsg_SessionSlotConfig (0x04) @0x425410 -> byte_A85B48 @0x425499]
			// Byte 17 is this joiner's OWN roster slot id — retail's
			// g_LocalPlayerSlotId, and the same value the host keeps at its
			// per-player record slot+20 [orig: the write side
			// NetPacket_WriteSlotAssignment @0x502b30]. It is NOT cosmetic: the client
			// packs it into the high 7 bits of every fired round's hit_part word, which
			// is how the host attributes the shot. Latching 0 makes every shot we send
			// read as SLOT 0 — the host's own — see simulation's hit_part builder.
			if (m.payload.size() >= 24) {
				local_player_slot_ = m.payload[17];
				max_player_slot_ = m.payload[18];
				assigned_team_ = m.payload[23];
			}
		} else if (m.tag == s2c::SPECTATOR_FLAGS) {
			// Despite the historical catalog name, byte 1 is the live player-slot
			// team. Retail overwrites the same byte_A85B48 latch installed by
			// 0x04, and its subsequent 0x2F copies this value verbatim.
			// [orig: NetPacket_SerializeHostEntityState (ex sub_510890) @0x510890 writes playerSlot+416;
			// NapiNPClientMsg_SetSpectatorMode @0x425A32]
			if (!m.payload.empty()) spectator_mode_ = (m.payload[0] & 1u) != 0;
			if (m.payload.size() >= 2) assigned_team_ = m.payload[1];
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
			//       RAW (the pre-Player_InitPlayer form, NOT g_CurrentWeaponSlot).
			// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910]
			TeamAssign assign;
			std::size_t assign_consumed = 0;
			if (decode_team_assign(m.payload.data(), m.payload.size(), assign,
					assign_consumed) &&
			    world::EntityHandle{assign.entity_handle}.valid() &&
			    world::EntityHandle{assign.entity_handle}.pool() < world::kEntityPoolCount) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
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
					// class can legally hold. The C2S 0x22/0x23 pair that
					// follows (@0x431acb..0x431b05) is the replica fold's
					// (ClientReplicaPipeline, S2C 0x50). Still deferred with
					// witnesses in D-NET-168: Player_InitPlayer(1) @0x431b14
					// and the minimap NetId maintenance @0x431b3a..0x431b91.
					if (post_auth_stage_ == PostAuthStage::AwaitDeployment ||
					    post_auth_stage_ == PostAuthStage::AwaitDeployPick ||
					    post_auth_stage_ == PostAuthStage::AwaitDeployRelease ||
					    post_auth_stage_ == PostAuthStage::Complete) {
						// [orig: @0x431a9e passes slot 195 RAW — the
						//  pre-Player_InitPlayer form, NOT the live
						//  g_CurrentWeaponSlot]
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
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
				for (uint16_t index : destroy_list.pool0_indices) {
					out.destroyed_pool0_slots.push_back(index);
					retire_self_handle(index);
				}
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
			if (decode_player_sync(m.payload.data(), m.payload.size(), sync)) {
				// The roster fold (D-HUD-24) lives in the replica pipeline;
				// surface the validated body on the canonical reducer stream
				// beside the connection-level bookkeeping below.
				// [orig: NapiNPClientMsg_PlayerSync @0x431370]
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
				if (sync.removal)
					out.cleared_player_slots.push_back(sync.slot_id);
				// Bit 0x4000 is the ACK-driven roster cursor. Retail advances
				// through EMPTY rows too and treats the 0x04 capacity byte as an
				// inclusive final slot: max=4 requests 0,1,2,3,4, then stops.
				// Every continuation uses the fixed 0x5CF7 field mask.
				// [orig: NapiNPClientMsg_PlayerSync @0x431370 tail;
				//  golden retail-to-retail f190/193/196/198/200]
				if (sync.queue_ack && sync.slot_id < max_player_slot_) {
					periodic_replies.push_back(make_protocol_message(
							c2s::PLAYER_SYNC_REQUEST,
							{static_cast<uint8_t>(sync.slot_id + 1), 0xF7, 0x5C}));
				}
			}
		} else if (m.tag == s2c::SPAWN_ACK_TIMESTAMP) {
			// This is not merely an acknowledgement: retail carries the scalar into
			// dword 0 of its later C2S 0x28 world-completion loadout request. A body
			// of any size other than four clears the retained value.
			// [orig: NapiNPClientMsg_0x019 @0x425e80 -> dword_A82360]
			spawn_ack_timestamp_ = 0;
			if (m.payload.size() == sizeof(uint32_t)) {
				std::size_t consumed = 0;
				decode_u32_scalar(m.payload.data(), m.payload.size(),
						spawn_ack_timestamp_, consumed);
			}
		} else if (m.tag == s2c::WORLD_STATE_LOAD) {
			// Retail's exact post-world completion burst, queued at the tail of
			// EVERY 0x0F handler run with no latch: a host normally sends one
			// 0x0F per admission (NetPacket_WriteWorldStateLoad0x0F's only
			// caller is Server_OnPlayerJoin), but a round-boundary re-join
			// re-runs that leg and expects the burst again. C2S 0x28 is
			// [S19 timestamp][S0F session tick][u16 0]; the remaining bodies and
			// order are fixed. Besides requesting the 0x5D empty-slot sweep, the
			// 0x22/0x23 legs make the host return the full player rows used to bind
			// the newly admitted local player.
			// [orig: NapiNPClientMsg_0x00F @0x42e5af..0x42e6ab; queues exact order
			//  0x28,0x29,0x2D,0x32,0x22,0x23; the one caller @0x51a84c]
			// The reducer folds the 0x0F's retained client globals (the
			// deploy-map overlay arm) in this same wire position
			// [orig: g_DeployScreenActive @0x42e2d8/@0x42e2f8].
			uint32_t world_state_tick = 0;
			std::size_t consumed = 0;
			decode_u32_scalar(m.payload.data(), m.payload.size(),
					world_state_tick, consumed);
			std::vector<uint8_t> loadout_request =
					le32_pair(spawn_ack_timestamp_, world_state_tick);
			loadout_request.push_back(0);
			loadout_request.push_back(0);

			periodic_replies.push_back(make_protocol_message(
					c2s::LOADOUT_REQUEST, std::move(loadout_request)));
			periodic_replies.push_back(make_protocol_message(
					c2s::TEAM_SPAWN_ACK, {0x00, 0x00}));
			periodic_replies.push_back(make_protocol_message(c2s::BURST_MEMBER_2D, {}));
			periodic_replies.push_back(make_protocol_message(
					c2s::EMPTY_SLOT_SWEEP_REQUEST, {}));
			periodic_replies.push_back(make_protocol_message(
					c2s::PLAYER_SYNC_REQUEST, {0x00, 0xF7, 0x5C}));
			periodic_replies.push_back(make_protocol_message(
					c2s::VISIBLE_PLAYERS_REQUEST, {}));
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
			    consumed == m.payload.size()) {
				if (sample.echo_flag != 0) {
					std::vector<uint8_t> pong = le32_value(sample.timestamp);
					pong.push_back(0);
					ProtocolMessage reply =
							make_protocol_message(c2s::RTT_CONSUMED, std::move(pong));
					// The reactive pong uses the same one-send queue parameter as the
					// per-frame RTT producer: userParam=1, not retained reliability.
					// [orig: NapiNPClientMsg_0x057_RTT @0x432210, queue @0x43226D]
					reply.reliable = false;
					periodic_replies.push_back(std::move(reply));
				} else {
					// The pong of our own C2S 0x2C ping: the completed round trip
					// lands in the ten-entry ring and the current-ping word (the
					// quality metric's ping term). Retail measures against
					// GetTickCount; our 0x2C stamps monotonic milliseconds, so
					// the same clock closes the loop.
					// [orig: @0x432280 `rtt = GetTickCount() - ts`,
					//  ring[index] = rtt @0x432282, dword_A860D4 = rtt,
					//  `++index >= 10 -> 0`]
					const uint32_t rtt_ms = monotonic_milliseconds32() - sample.timestamp;
					rtt_ring_[rtt_ring_index_] = rtt_ms;
					rtt_current_ms_ = rtt_ms;
					if (++rtt_ring_index_ >= rtt_ring_.size()) rtt_ring_index_ = 0;
				}
			}
		} else if (m.tag == s2c::CHARATTR_PROPERTY_CLEAR) {
			// The session's restriction of the boot charattr table: the property
			// zeroed in every class, then disabled. A short body defaults to
			// property 0; trailing bytes are ignored. Every row is touched before
			// the next inner message dispatches. A host sends 0, 5, 6 and 7 at a
			// join, one for each of its mp_No* words.
			// [orig: NapiNPClientMsg_CharAttrDisableProperty @0x4254C0 ->
			//  CharAttr_SetProperty @0x412890, CharAttr_SetPropertyAllowed
			//  @0x4125C0; the sender Server_SendCharAttrRestrictionsToPlayer
			//  @0x509950]
			const uint8_t property_id =
					m.payload.empty() ? uint8_t{0} : m.payload[0];
			charattr_disable_property(charattr_table_, property_id);
			++challenge_diagnostics_.property_clears;
		} else if (m.tag == s2c::CHARATTR_DISABLED_PROPERTIES) {
			// The authority's disable latches: eight of the table's fourteen set
			// from the word (a body short of two bytes reads 0), at the join and
			// with every periodic challenge.
			// [orig: NapiNPClientMsg_CharAttrDisabledProperties @0x4281A0 ->
			//  CharAttr_UnpackDisabledProperties @0x4124D0]
			uint16_t word = 0;
			std::size_t word_consumed = 0;
			if (!decode_charattr_disabled_properties(m.payload.data(), m.payload.size(), word, word_consumed))
				word = 0;
			charattr_unpack_disabled(charattr_table_, word);
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
			//  len 4 @0x42e723); CharAttr_GetClassChecksum @0x412aa0 (IDB
			//  AnimMap_GetSlotChecksum until 2026-10-07) — the `return 0` arm
			//  @0x412abf; the host's discard + counter reset
			//  NapiNPServerMsg_AnimChecksumRequest @0x501D40 @0x501d71/@0x501d79]
			uint32_t challenge_seed = 0;
			std::size_t seed_consumed = 0;
			decode_u32_scalar(m.payload.data(), m.payload.size(), challenge_seed,
					seed_consumed);
			++challenge_diagnostics_.charattr_seen;
			bool row_found = false;
			const uint32_t checksum = charattr_class_checksum(
					charattr_table_, current_player_class_, challenge_seed, &row_found);
			if (!row_found) ++challenge_diagnostics_.charattr_row_missing;
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
			// The default remains deliberately silent: a guessed value can only mismatch,
			// and the host punts on a difference [orig:
			// NapiNPServerMsg_HandleAntiCheatCRCCheck @0x502050, "PUNT ACRC"]. A named profile may
			// answer only sources independently reproduced from that exact retail corpus.
			// Uncovered individual ADM rows remain silent. This preserves the 2026-07-26
			// live safety witness while allowing the revx02 table CRC proved on 2026-08-01
			// (D-NET-181; net-re section 5.65).
			EntityChecksumRequest request;
			std::size_t consumed = 0;
			++challenge_diagnostics_.entity_checksum_seen;
			if (integrity_challenge_profile_ != nullptr &&
			    decode_entity_checksum_request(m.payload.data(), m.payload.size(),
					request, consumed) &&
			    consumed == m.payload.size()) {
				uint32_t source_crc = 0;
				bool source_witnessed = false;
				if (request.entity_id == 0xFFu) {
					// The nonzero-challenge branch is a literal retail
					// short-circuit and never reads the ADM table.
					source_crc = request.checksum != 0
							? 42u
							: integrity_challenge_profile_->weapon_slot_table_crc;
					source_witnessed = true;
				} else {
					source_witnessed = integrity_challenge_profile_->
							find_animation_definition_crc(
									request.entity_id, source_crc);
				}
				if (source_witnessed) {
					std::vector<uint8_t> body{request.entity_id};
					const std::vector<uint8_t> value = le32_value(
							static_cast<uint32_t>(request.checksum) ^ source_crc);
					body.insert(body.end(), value.begin(), value.end());
					periodic_replies.push_back(make_protocol_message(
							c2s::ENTITY_CHECKSUM_REPLY, std::move(body)));
					++challenge_diagnostics_.entity_checksum_answered;
				}
			}
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
			++challenge_diagnostics_.loadout_crc_seen;
			// As above, no profile means silence and an uncovered in-range record stays
			// silent. A selected profile may answer only CRCs reproduced from retail's
			// sanitized 276-byte record image. The independently witnessed table count also
			// permits retail's exact out-of-range zero arm; neither case invents a CRC
			// (D-NET-181; net-re section 5.65).
			if (integrity_challenge_profile_ != nullptr &&
			    decode_loadout_crc_request(m.payload.data(), m.payload.size(),
					crc_request, crc_consumed) &&
			    crc_consumed == m.payload.size()) {
				uint32_t reply_crc = 0;
				bool source_witnessed = false;
				if (crc_request.ammo_index >=
				    integrity_challenge_profile_->ammo_definition_count) {
					// Retail writes literal zero here, not xor_key ^ 0.
					source_witnessed = true;
				} else {
					uint32_t source_crc = 0;
					source_witnessed = integrity_challenge_profile_->
							find_ammo_definition_crc(
									crc_request.ammo_index, source_crc);
					if (source_witnessed) {
						reply_crc = static_cast<uint32_t>(crc_request.xor_key) ^
								source_crc;
					}
				}
				if (source_witnessed) {
					std::vector<uint8_t> body{crc_request.ammo_index};
					const std::vector<uint8_t> crc_bytes = le32_value(reply_crc);
					const std::vector<uint8_t> key_bytes =
							le32_value(crc_request.xor_key);
					body.insert(body.end(), crc_bytes.begin(), crc_bytes.end());
					body.insert(body.end(), key_bytes.begin(), key_bytes.end());
					periodic_replies.push_back(make_protocol_message(
							c2s::CHECKSUM_REPLY, std::move(body)));
					++challenge_diagnostics_.loadout_crc_answered;
				}
			}
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
			//  validator NapiNPServerMsg_ValidateTimeSync @0x502210]
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
			// Per-frame world snapshot — surface for the caller's ClientReplicaPipeline.
			out.inbound_0a.push_back(m.payload);
		} else if (m.tag == s2c::CAPTURE_ZONE_STATE) {
			CaptureZoneOverlayBatch batch;
			if (decode_capture_zone_overlay(m.payload.data(), m.payload.size(), batch)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::MINIMAP_OVERLAY) {
			MinimapOverlayBatch batch;
			if (decode_minimap_overlay_batch(m.payload.data(), m.payload.size(), batch)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::WEAPON_LOADOUT && valid_weapon_loadout &&
		           post_auth_stage_ == PostAuthStage::AwaitDeployment) {
			// Every valid retail 0x5A apply clears dword_81474C immediately,
			// independently from the deploy screen becoming pick-ready. Retain
			// the pair count only for that later UI/pick transition.
			// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0]
			if (initial_loadout_grant_count_ < 2)
				++initial_loadout_grant_count_;
			release_deployment(/*deployment_complete=*/false);
		} else if (m.tag == s2c::WEAPON_LOADOUT && valid_weapon_loadout &&
		           post_auth_stage_ == PostAuthStage::AwaitDeployRelease) {
			// A split second grant may arrive after we have queued the deploy
			// pick. Only a packet that cumulatively ACKs the C2S 0x0E can be its
			// causal post-pick release.
			if (deployment_pick_sent_ &&
			    packet_ack >= deployment_pick_sequence_) {
				release_deployment(/*deployment_complete=*/true);
			}
		} else if (m.tag == s2c::DEPLOYED_ITEM) {
			DeployedItemSpawn spawn;
			std::size_t consumed = 0;
			if (decode_deployed_item_spawn(
					m.payload.data(), m.payload.size(), spawn, consumed) &&
					consumed == m.payload.size()) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::ENTITY_REMOVE) {
			EntityRemove removal;
			std::size_t consumed = 0;
			if (decode_entity_remove(
					m.payload.data(), m.payload.size(), removal, consumed) &&
					consumed == m.payload.size()) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
				retire_self_handle(removal.entity_handle);
			}
		} else if (m.tag == s2c::OBJECTIVE_ENTITY_STATE) {
			ObjectiveEntityState state;
			std::size_t consumed = 0;
			if (decode_objective_entity_state(
					m.payload.data(), m.payload.size(), state, consumed) &&
					consumed == m.payload.size()) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::SPAWN_WAVE_STATUS) {
			SpawnWaveStatus status;
			if (decode_spawn_wave_status(
					m.payload.data(), m.payload.size(), status)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::SCORE_DELTA_SOUND) {
			ScoreDeltaSound score;
			if (decode_score_delta_sound(
					m.payload.data(), m.payload.size(), score)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::WEAPON_RELOAD) {
			// The host echoes the same four-byte C2S 0x25 reload body as S2C 0x49.
			// Surface it once through the decoded client-view event path.
			out.inbound_gameplay.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::WEAPON_PICKUP) {
			// A powerup `weapon` grant: the replica pipeline folds it and the
			// role lands and mounts the weapon when the picker is its own
			// player. [orig: NapiNPClientMsg_0x035 @0x4261A0]
			WeaponPickupNotice notice;
			std::size_t consumed = 0;
			if (decode_weapon_pickup(m.payload.data(), m.payload.size(), notice, consumed) &&
					consumed == m.payload.size())
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
		} else if (m.tag == s2c::ENTITY_DEATH) {
			// S2C 0x13 ENTITY DEATH — the host's per-death notify for every
			// non-player victim (the AI/item leg of Entity_CheckAndProcessDeath).
			// The replica pipeline folds it (Health = 0 + the surfaced death
			// record) and the embedding sim runs the class death callback on the
			// world twin — a destructible's husk/explosion chain (reason 4).
			// Without this surface a joiner never saw a destructible die unless
			// its own visual round predicted it.
			// [orig: NapiNPClientMsg_EntityDeath @0x42EB50; sender
			//  Entity_CheckAndProcessDeath @0x51b550 — msg 19, mask 0x90]
			EntityDeathRecord death;
			std::size_t death_consumed = 0;
			if (decode_entity_death(
					m.payload.data(), m.payload.size(), death, death_consumed)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::SCRIPT_REMOTE_COMMAND) {
			// S2C 0x23 — a WAC command the host VM replicated. The replica
			// pipeline folds it and the role runs the registry row's handler
			// against its world. [orig: GameMode_DispatchRemoteCommand @0x4F81E0]
			ScriptRemoteCommand command;
			std::size_t consumed = 0;
			if (decode_script_remote_command(
					m.payload.data(), m.payload.size(), command, consumed)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::DEATH_CAMERA_TARGET) {
			DeathCameraTarget target;
			std::size_t consumed = 0;
			if (decode_death_camera_target(
					m.payload.data(), m.payload.size(), target, consumed) &&
					consumed == m.payload.size()) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::PLAYER_DOWNED_STATE) {
			PlayerDownedState state;
			std::size_t consumed = 0;
			if (decode_player_downed_state(
					m.payload.data(), m.payload.size(), state, consumed) &&
					consumed == m.payload.size()) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
        } else if (m.tag == s2c::EXPLOSION_EFFECT) {
            out.inbound_gameplay.emplace_back(m.tag,m.payload);
		} else if (m.tag == s2c::KILL_SYNC) {
			// S2C 0x26 KILL SYNC — the second death route (the destructible
			// deathCallback's own authority resend among its senders); the
			// client kills the addressed slot through the same class death
			// callback. Folded beside 0x13 by the replica pipeline.
			// [orig: NapiNPClientMsg_0x026 @0x42EC30 → Entity_KillBySlotId
			//  @0x42BCE0; resend Server_SendEntityStatePacket @0x509d70 via
			//  Entity_HandleDestructibleDeathEvent @0x440210]
			KillRecord kill;
			std::size_t kill_consumed = 0;
			if (decode_kill_record(
					m.payload.data(), m.payload.size(), kill, kill_consumed)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::GAME_EVENT) {
			// S2C 0x1E kill/objective/medic feed event. The replica pipeline
			// folds it into ClientState feed events (D-HUD-23) — before this
			// branch a joiner silently dropped the lane (the #511 fold only
			// ever fired on the host's loopback view, which applies every
			// tag). [orig: NetPacket_HandleGameEvent @0x426270]
			GameEventRecord game_event;
			std::size_t game_event_consumed = 0;
			if (decode_game_event(m.payload.data(), m.payload.size(),
					game_event, game_event_consumed)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
		} else if (m.tag == s2c::ENTITY_ROUTED) {
			// S2C 0x44 entity-routed sub-packet — the guided-missile channel
			// (D-NET-64). The replica pipeline folds the GUIDED subtype into
			// the ClientState guided bank; before this branch a joiner
			// silently dropped the lane (the #508 fold only ever fired on the
			// host's loopback view, which applies every tag — the same gap as
			// 0x16/0x46/0x1E before it). [orig: NapiNPClientMsg_0x044
			// @0x422710 → NetPacket_DispatchToEntityByNetId @0x4D6960]
			EntityRoutedPacket routed;
			if (decode_entity_routed_packet(
					m.payload.data(), m.payload.size(), routed)) {
				out.inbound_gameplay.emplace_back(m.tag, m.payload);
			}
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
		} else if (m.tag == s2c::ZONE_PRESENCE_COUNT) {
			ZonePresenceCount presence;
			std::size_t consumed = 0;
			if (decode_zone_presence_count(
					m.payload.data(), m.payload.size(), presence, consumed) &&
			    consumed == m.payload.size())
				out.zone_timer_updates.emplace_back(presence);
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
				out.outbound.push_back(frame_inner_pending(0x0A, {}));
				pending_spawn_menu_request_ = false;
				post_auth_stage_ = PostAuthStage::AwaitWorldStreamEnd;
			}
		}
		// Other tags (game-start bundle scalars, world-state-load 0x0F) are not entity data.
	}
	std::size_t reducer_entries = 0;
	for (std::size_t reply = 0; reply < periodic_replies.size(); ++reply) {
		while (reducer_entries < reducer_reply_start.size() &&
		       reducer_reply_start[reducer_entries] <= reply)
			++reducer_entries;
		out.queued_send_after.push_back(reducer_entries);
		out.queued_send_messages.push_back(std::move(periodic_replies[reply]));
	}
	if (admission.admitted && initial_loadout_grant_count_ >= 2 &&
	    deployment_policy_seen_ &&
	    post_auth_stage_ == PostAuthStage::AwaitDeployment) {
		initial_admission_complete_ = true;
		// The grant pair completes admission independently of the deploy-map
		// overlay: a wave host can spawn without a manual selection. A spawn-zone
		// host can instead keep its alive player respawn-pending (0x0A flags1 bit1)
		// until the user selects a spawn. prepare_deployment_pick accepts that
		// initial overlay selection from Complete as well as a death re-pick.
		// [orig: NapiNPClientMsg_0x00F @0x42E2ED revives the local player @0x42E403;
		// Server_OnPlayerJoin @0x51A680, hold @0x51A6F2 holds slot state bit0x10 for spawn zones;
		// NetPacket_WritePlayerState @0x4FF6B0, @0x4FF7BD exposes that hold as the overlay]
		release_deployment(/*deployment_complete=*/true);
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
	// A list that named a sequence fires the outgoing link-error callback
	// after the resends [orig: NapiNP_HandleResendList @0x6239aa latches it on
	// a nonzero dword, @0x6239ef..0x623a37 calls cb_client_3 =
	// Network_LogOutgoingPacketError @0x4c4920].
	if (std::any_of(requested.begin(), requested.end(),
			[](uint32_t sequence) { return sequence != 0; }))
		net_quality_link_errors_ |= kNetQualityLinkErrorOutgoing;

	// Each requested sequence is rebuilt and TRANSMITTED by the handler itself, inside the
	// receive pump: retail's SendSessionPacket writes the packet straight to the socket, so a
	// resend never waits for the send-holdoff gate and never touches the flush counter.
	// [orig: NapiNP_HandleResendList @0x623980..0x6239da -> CNapiNPConnection_SendSessionPacket
	//  @0x6239b6 -> CNapiNPManager_SendTo @0x61f039]
	for (uint32_t requested_sequence : requested) {
		const uint32_t sequence = requested_sequence == 0
				? conn_.seq.next_outbound_seq
				: requested_sequence;
		std::vector<uint8_t> datagram = frame_retained_session(sequence);
		if (!datagram.empty()) out.immediate_outbound.push_back(std::move(datagram));
	}
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
	s2c_reassembly_ = {};
	// frame_session stamps next_seq then post-increments; last_ack -> 0x43 ack_count
	self_handle_ = self_handle;
	has_self_handle_ = true;
	spawn_.item_type_id = self_type;
	game_type_ = game_type;
	mp_attributes_ = 0;
	session_max_players_ = 0;
	mission_metadata_transfer_id_ = 0;
	mission_metadata_total_size_ = 0;
	mission_metadata_fixed_bytes_.fill(0);
	mission_metadata_fixed_byte_mask_ = 0;
	mission_header_bytes_.clear();
	reset_terrain_load();
	class_allow_mask_ = 0x03FFu;
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	post_auth_stage_ = PostAuthStage::Complete;
	initial_admission_complete_ = true;
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
	silence_timeout_latched_ = false;
}

std::vector<uint8_t> JoinerConnection::begin_mission_reload() {
	if (!in_session() || conn_.server_scrk.empty()) return {};
	phase_ = Phase::Driving;
	post_auth_stage_ = PostAuthStage::AwaitServerInfo;
	reloading_ = true;
	// The previous mission's admission state; the connection, the transfer
	// ids the requests echo, the team latch (the 0x47's S2C 0x75 re-latches
	// it) and the session's identity stay.
	preload_ready_ = false;
	sync_tail_seen_ = false;
	player_list_seen_ = false;
	deployment_policy_seen_ = false;
	initial_loadout_grant_count_ = 0;
	initial_admission_complete_ = false;
	deployment_pick_sent_ = false;
	deployment_pick_sequence_ = 0;
	deployment_pick_sequence_unbound_ = false;
	deployment_reply_seen_ = false;
	pending_spawn_menu_request_ = false;
	has_self_handle_ = false;
	self_handle_ = 0;
	spawn_ = SelfSpawn{};
	mission_header_bytes_.clear();
	reset_terrain_load();
	server_info_bytes_.clear();
	mission_metadata_bytes_.clear();
	mission_metadata_total_size_ = 0;
	return frame_session({
			make_protocol_message(c2s::CLIENT_ACK, le32_value(conn_.connection_id)),
			make_protocol_message(c2s::PING, {}),
			make_protocol_message(c2s::FILE_CHUNK_REQUEST, le32_pair(server_info_transfer_id_, 0)),
	});
}

uint64_t JoinerConnection::milliseconds_since_last_receive() const {
	if (!receive_clock_armed_) return 0;
	const uint64_t now = monotonic_milliseconds_();
	return now >= last_receive_ms_ ? now - last_receive_ms_ : 0;
}

} // namespace opennova::inmatch
