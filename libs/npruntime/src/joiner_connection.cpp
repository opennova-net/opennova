#include "npruntime/joiner_connection.h"

#include <npwire/ingame_encode.h>
#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <chrono>
#include <string_view>
#include <utility>

// Verbatim port of novaworld::JoinerSession (the client mirror), with SCRK/seq/ack stored on the
// type-2 NapiNPConnection conn_. The D.0 name-match in on_server_session is copied byte-for-byte.
namespace opennova::np {

namespace {

// Use the initialized retail NAPI CS template's witnessed 1000-ms active-send interval (field 5)
// for the pre-session resend cadence. Captures prove these handshake packets retransmit; the exact
// state-machine timer xref is still ungrilled, so keep this policy tied to a known retail NAPI value.
// [orig: CNapiGameSession_InitNPConnection @0x4d3e1f; NapiNPConnection CS field 5]
constexpr uint64_t kActiveSendRetryMilliseconds = 1000;

uint64_t steady_milliseconds() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
	        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// [orig: NetPacket_WritePositionWithPadding @0x42A360]. The golden S2C probe carries
// padding_len=256 and retail emits a 256-byte C2S body: the two echoed position dwords followed by
// random filler. Bound the peer-controlled allocation while preserving the witnessed body shape.
std::vector<uint8_t> build_join_padding_echo(const JoinPaddingProbe &probe) {
	constexpr uint32_t kMaxJoinPadding = 64u * 1024u;
	if (probe.padding_len < 8 || probe.padding_len > kMaxJoinPadding) return {};

	std::vector<uint8_t> echo(probe.padding_len, 0);
	auto write_i32 = [&](std::size_t off, int32_t value) {
		const uint32_t v = static_cast<uint32_t>(value);
		echo[off + 0] = static_cast<uint8_t>(v);
		echo[off + 1] = static_cast<uint8_t>(v >> 8);
		echo[off + 2] = static_cast<uint8_t>(v >> 16);
		echo[off + 3] = static_cast<uint8_t>(v >> 24);
	};
	write_i32(0, probe.pos_x);
	write_i32(4, probe.pos_y);
	for (std::size_t off = 8; off < echo.size();) {
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

std::vector<uint8_t> build_retail_default_loadout(bool alternate) {
	std::vector<uint8_t> body = {
			0x02, 0x08, alternate ? uint8_t{0xD4} : uint8_t{0xC3},
			0x00, 0x00, 0x00,
	};
	for (uint8_t adm : {
			uint8_t{0x18}, uint8_t{0x03}, uint8_t{0x2C}, uint8_t{0x28},
			uint8_t{0x29}, uint8_t{0x2A}, uint8_t{0x02},
	}) {
		body.push_back(adm);
		body.push_back(0xFF);
		body.push_back(0xFF);
		body.push_back(0xFF);
	}
	body.push_back(0xFF);
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

bool is_structural_cs_config_update(
		const ProtocolMessage &message, uint8_t &direction_out) {
	if (!message.flags.settings_update || message.full_tag != 0x100u ||
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
	session_last_send_ms_ = 0;
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	pending_spawn_menu_request_ = false;
	preload_ready_ = false;
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
	mission_known_ = false;
	server_name_.clear();
	mission_name_.clear();
	map_file_.clear();
	expansion_.clear();
	advertised_expansion_.clear();
	game_type_ = 0;
	last_error_.clear();
	phase_ = Phase::Hello;
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
	// Server_ValidatePlayerJoinRequest checks (BN/VN/MBN/SOPD) and produces H:0x03 DS=1,DC=2.
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
	server_hk_ = sh.hk;          // echo this in ClientAuth.hk
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
	const bool received_settings = has_initial_cs_config_pair(messages);
	// The fixed 0x0F header carries gameFlags at byte 22. Bit 0 says the host
	// has spawn zones and will keep this player hidden until C2S 0x0E. Keep an
	// explicit unknown state because OpenNova and retail may send the initial
	// 0x5A grants before 0x0F, either in this packet or an earlier packet.
	for (const ProtocolMessage &m : messages) {
		if (!m.flags.settings_update && m.full_tag < 0x100u &&
		    m.tag == 0x0F && m.payload.size() > 22) {
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
	if (admission.admitted && received_settings &&
	    post_auth_stage_ == PostAuthStage::AwaitServerSettings) {
		// Golden retail frames 6-8: acknowledge the host's initial settings packet in a header-only
		// sequence, then send the exact 21-byte C2S 0x00 JOIN request in the following sequence.
		handshake_retry_datagram_.clear();
		handshake_retry_clock_armed_ = false;
		out.outbound.push_back(frame_session({}));
		out.outbound.push_back(frame_inner(
				0x00, build_join_request(advertised_expansion_)));
		post_auth_stage_ = PostAuthStage::AwaitJoinAck;
	}
	for (const ProtocolMessage &m : messages) {
		// The initial settings records use low tag 0x00 with the high/settings flag set. They are
		// connection control, not the regular S2C 0x00 JOIN acknowledgement below.
		if (m.flags.settings_update || m.full_tag >= 0x100u) continue;

		if (m.tag == 0x00 && post_auth_stage_ == PostAuthStage::AwaitJoinAck) {
			// Golden retail frames 9-11: acknowledge S2C 0x00 with a header-only sequence, then post
			// the one-byte zero form body. An empty 0x01 is not accepted by the retail host FSM.
			out.outbound.push_back(frame_session({}));
			out.outbound.push_back(frame_inner(0x01, {0x00}));
			post_auth_stage_ = PostAuthStage::AwaitPaddingProbe;
		} else if (m.tag == 0x02 &&
		           post_auth_stage_ == PostAuthStage::AwaitPaddingProbe) {
			// Retail's response to the post-auth join probe. This sequenced reply is what causes the
			// server to send the post-handshake burst containing authoritative S2C 0x7B metadata.
			JoinPaddingProbe probe;
			if (decode_join_padding_probe(m.payload.data(), m.payload.size(), probe)) {
				std::vector<uint8_t> echo = build_join_padding_echo(probe);
				if (!echo.empty()) {
					out.outbound.push_back(frame_inner(0x02, std::move(echo)));
					post_auth_stage_ = PostAuthStage::AwaitGameStart;
				}
			}
		} else if (m.tag == 0x05 &&
		           post_auth_stage_ == PostAuthStage::AwaitGameStart &&
		           !m.payload.empty() && m.payload[0] != 0) {
			// Golden frame 17: the game-start flag completes verification with ONE grouped C2S
			// packet. This exchange runs before local mission loading; 0x48 echoes ServerAuth.MI.
			out.outbound.push_back(frame_session({
					make_protocol_message(0x4E, std::vector<uint8_t>(4, 0)),
					make_protocol_message(0x03, std::vector<uint8_t>(4, 0)),
					make_protocol_message(0x48, le32_value(conn_.connection_id)),
					make_protocol_message(0x47, {}),
					make_protocol_message(0x33, std::vector<uint8_t>(8, 0)),
			}));
			post_auth_stage_ = PostAuthStage::AwaitServerInfo;
		} else if (m.tag == 0x60 &&
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
		} else if (m.tag == 0x64 &&
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
								make_protocol_message(0x09, {}),
								make_protocol_message(
										0x22, {0x00, 0xF7, 0x1C}),
						}));
						post_auth_stage_ = PostAuthStage::AwaitInitialSyncTail;
					}
				}
			}
		} else if (m.tag == 0x16) {
			PlayerList player_list;
			if (decode_player_list(
					m.payload.data(), m.payload.size(), player_list)) {
				player_list_seen_ = true;
				if (post_auth_stage_ == PostAuthStage::AwaitPlayerList) {
					// Golden frame 23: transition marker and first player-sync request share
					// one packet. OpenNova hosts may send 0x16 early, so the latch above lets
					// the final 0x64 release the same wire packet without a compatibility fork.
					out.outbound.push_back(frame_session({
							make_protocol_message(0x09, {}),
							make_protocol_message(
									0x22, {0x00, 0xF7, 0x1C}),
					}));
					post_auth_stage_ = PostAuthStage::AwaitInitialSyncTail;
				}
			}
		} else if (m.tag == 0x08) {
			// Our initial-state burst carries the same g_GameType in the fixed
			// session-config block before any live frame. Retail keeps this equal
			// to the later 0x7B `extra` value.
			SessionConfig config;
			if (decode_session_config(m.payload.data(), m.payload.size(), config))
				game_type_ = static_cast<uint32_t>(config.fields[3]);
		} else if (m.tag == 0x7B) {
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
		} else if (m.tag == 0x0C) {
			// S2C 0x0C organic-spawn batch — the self name-match (§5.23). ALSO surface the whole
			// batch to the NetClientView (below) so every other organic upserts too.
			OrganicSpawnBatch batch;
			if (decode_organic_spawn_batch(m.payload.data(), m.payload.size(), batch)) {
				for (const OrganicSpawnRecord &rec : batch.records) {
					if (!rec.has_body || rec.entity_name != player_name_) continue;
					self_handle_ = rec.slot_id; // the wire handle H (pool<<12|slot)
					has_self_handle_ = true;
					spawn_.pos_x = rec.pos_x;
					spawn_.pos_y = rec.pos_y;
					spawn_.pos_z = rec.pos_z;
					spawn_.orientation = rec.orientation;
					spawn_.team = rec.team;
					spawn_.item_type_id = rec.item_type_id;
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
		} else if (m.tag == 0x0D || m.tag == 0x10 || m.tag == 0x20) {
			// The rest of the load-time world stream (§5.2a): pool-1 spawns / pool-2 statics /
			// pool-3 markers. Surface raw for the caller's NetClientView to upsert.
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == 0x1A &&
		           post_auth_stage_ == PostAuthStage::AwaitWorldStreamEnd) {
			// The world-stream terminator releases retail's loadout/status submission. Keep the
			// two profile slots distinct and preserve the witnessed nonzero mission-status tail.
			out.outbound.push_back(frame_session({
					make_protocol_message(
							0x2F, build_retail_default_loadout(false)),
					make_protocol_message(
							0x2F, build_retail_default_loadout(true)),
					make_protocol_message(
							0x0B, build_retail_mission_status()),
			}));
			post_auth_stage_ = PostAuthStage::AwaitDeployment;
		} else if (m.tag == 0x0A) {
			// Per-frame world snapshot — surface for the caller's NetClientView.
			out.inbound_0a.push_back(m.payload);
		} else if (m.tag == 0x5A &&
		           post_auth_stage_ == PostAuthStage::AwaitDeployment) {
			// Retail grants both submitted profile-side loadouts before it can
			// process the deployment pick. Persist the count across packet
			// boundaries; waiting for both also prevents a lost/retransmitted
			// second grant from being mistaken for the post-pick release.
			if (initial_loadout_grant_count_ < 2)
				++initial_loadout_grant_count_;
		} else if (m.tag == 0x5A &&
		           post_auth_stage_ == PostAuthStage::AwaitDeployRelease) {
			// A split second grant may arrive after we have queued the deploy
			// pick. Only a packet that cumulatively ACKs the C2S 0x0E can be its
			// causal post-pick release.
			if (deployment_pick_sent_ &&
			    hdr.ack_count >= deployment_pick_sequence_) {
				release_deployment();
			}
		} else if (m.tag == 0x49) {
			// The host echoes the same four-byte C2S 0x25 reload body as S2C 0x49.
			// Surface it once through the decoded client-view event path.
			out.inbound_gameplay.emplace_back(m.tag, m.payload);
		} else if (m.tag == 0x11) {
			// S2C 0x11 is the terminal pre-world admission marker. Its cumulative ACK is the safe
			// point at which the binding may pause progress for synchronous mission loading. Hold
			// retail's empty C2S 0x0A world-stream request until that local world is installed.
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
	if (admission.admitted && initial_loadout_grant_count_ >= 2 &&
	    deployment_policy_seen_ &&
	    post_auth_stage_ == PostAuthStage::AwaitDeployment) {
		if (deployment_pick_required_) {
			// Retail's parameter-0 deploy-map pick is the signed handle 0xFFFF.
			// The server keeps the player hidden/respawn-pending until this request.
			deployment_pick_sequence_ = conn_.seq.next_outbound_seq;
			std::vector<uint8_t> deploy_pick =
					frame_inner(0x0E, {0xFF, 0xFF});
			if (!deploy_pick.empty()) {
				out.outbound.push_back(std::move(deploy_pick));
				deployment_pick_sent_ = true;
				post_auth_stage_ = PostAuthStage::AwaitDeployRelease;
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
		    now_ms - handshake_last_send_ms_ >= kActiveSendRetryMilliseconds) {
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
		} else {
			const uint64_t now_ms = monotonic_milliseconds_();
			if (!session_send_clock_armed_) {
				session_last_send_ms_ = now_ms;
				session_send_clock_armed_ = true;
			} else if (now_ms >= session_last_send_ms_ &&
			           now_ms - session_last_send_ms_ > kActiveSendRetryMilliseconds) {
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

std::vector<uint8_t> JoinerConnection::frame_c2s_uplink(uint16_t handle_H, uint16_t type,
                                                        const PlayerExtendedUplink &body) {
	EntityPacketSubHeader sub;
	sub.handle = handle_H;
	sub.item_type_id = type;
	sub.sub_op = 0x0A; // extended (type-10) uplink
	std::vector<uint8_t> payload = encode_entity_packet_sub_header(sub);
	std::vector<uint8_t> ext = encode_player_extended_uplink(body);
	payload.insert(payload.end(), ext.begin(), ext.end());
	return frame_session({make_protocol_message(0x0C, std::move(payload))});
}

void JoinerConnection::seed_in_match(uint32_t session_id, std::string client_scrk,
                                     std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
                                     uint16_t self_handle, uint16_t self_type,
                                     uint32_t game_type) {
	conn_.server_sk = session_id;            // 0x43 header session_id (= ServerAuth.sk)
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
	session_last_send_ms_ = 0;
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	phase_ = Phase::InMatch;
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
