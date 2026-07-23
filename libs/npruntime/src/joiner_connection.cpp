#include "npruntime/joiner_connection.h"

#include <npwire/ingame_encode.h>
#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <chrono>
#include <utility>

// Verbatim port of novaworld::JoinerSession (the client mirror), with SCRK/seq/ack stored on the
// type-2 NapiNPConnection conn_. The D.0 name-match in on_server_session is copied byte-for-byte.
namespace opennova::np {

namespace {

// Use the initialized retail NAPI CS template's witnessed 1000-ms active-send interval (field 5)
// for the pre-session resend cadence. Captures prove these handshake packets retransmit; the exact
// state-machine timer xref is still ungrilled, so keep this policy tied to a known retail NAPI value.
// [orig: CNapiGameSession_InitNPConnection @0x4d3e1f; NapiNPConnection CS field 5]
constexpr uint64_t kHandshakeRetryMilliseconds = 1000;

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
	pump_stage_ = 0;
	pending_spawn_menu_request_ = false;
	has_self_handle_ = false;
	self_handle_ = 0;
	spawn_ = SelfSpawn{};
	mission_known_ = false;
	server_name_.clear();
	mission_name_.clear();
	map_file_.clear();
	expansion_.clear();
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
	// Retail serializes CI0/CI1/TR/CTA/CTB/VCA/VCB from the ACTIVE player profile here.
	// They are not protocol identity and captured values vary by profile. OpenNova does not yet
	// persist that full selection, so omit them exactly as retail's zero-value gates do and let the
	// host's witnessed absent/invalid-field fallback choose the character. Do not replay one
	// capture's packed registry ids as global defaults. [orig: UI_JoinSelectedSession @0x569C14;
	// CNapiServerInfo_SerializeToSession @0x4c3650; host fallback @0x51d68b]
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
	phase_ = Phase::Auth;
	handshake_retry_datagram_ = build_client_auth();
	handshake_last_send_ms_ = monotonic_milliseconds_();
	handshake_retry_clock_armed_ = true;
	out.outbound.push_back(handshake_retry_datagram_);
}

void JoinerConnection::on_server_auth(const std::vector<uint8_t> &body, PollResult &out) {
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
	// Retail's game-session join FSM starts with this sequenced post-auth exchange. The host answers
	// with S2C 0x02; only the C2S 0x02 response unlocks the 0x7B mission/session summary. This remains
	// active while the binding holds world_ready_ false so a joiner can discover what it must load.
	out.outbound.push_back(frame_inner(0x01, {}));
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
	for (const ProtocolMessage &m : messages) {
		if (m.tag == 0x02) {
			// Retail's response to the post-auth join probe. This sequenced reply is what causes the
			// server to send the post-handshake burst containing authoritative S2C 0x7B metadata.
			JoinPaddingProbe probe;
			if (decode_join_padding_probe(m.payload.data(), m.payload.size(), probe)) {
				std::vector<uint8_t> echo = build_join_padding_echo(probe);
				if (!echo.empty()) out.outbound.push_back(frame_inner(0x02, std::move(echo)));
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
					if (phase_ != Phase::InMatch) {
						phase_ = Phase::InMatch;
						out.reached_in_match = true;
					}
				}
			}
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == 0x0D || m.tag == 0x10 || m.tag == 0x20) {
			// The rest of the load-time world stream (§5.2a): pool-1 spawns / pool-2 statics /
			// pool-3 markers. Surface raw for the caller's NetClientView to upsert.
			out.inbound_world.emplace_back(m.tag, m.payload);
		} else if (m.tag == 0x0A) {
			// Per-frame world snapshot — surface for the caller's NetClientView.
			out.inbound_0a.push_back(m.payload);
		} else if (m.tag == 0x49) {
			// The host echoes the same four-byte C2S 0x25 reload body as S2C 0x49.
			// Surface it once through the decoded client-view event path.
			out.inbound_gameplay.emplace_back(m.tag, m.payload);
		} else if (m.tag == 0x11) {
			// S2C 0x11 ends the player-sync bundle; the retail client answers with the EMPTY C2S
			// 0x0A spawn-menu request — the message that advances the host's session sync state
			// to 4 and starts the §5.2a world stream [orig: NapiNPServerMsg_HandlePlayerSpawnRequest
			// @0x513260; golden retail-ashi5a S 0x11 f=201572 -> C 0x0A f=201573]. A host (ours or
			// retail) never streams the world to a joiner that hasn't sent it (D-NET-150).
			if (world_ready_ && pump_stage_ >= 3)
				out.outbound.push_back(frame_inner(0x0A, {}));
			else
				pending_spawn_menu_request_ = true;
		}
		// Other tags (game-start bundle scalars, world-state-load 0x0F) are not entity data.
	}
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
		std::vector<uint8_t> session_body;
		if (!frame_session_packet_for_sequence(
				conn_.seq,
				SessionCrypto{conn_.client_scrk, {}, conn_.server_sk},
				sequence, session_body)) {
			continue;
		}
		out.outbound.push_back(nw_encode_outbound(
				SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(session_body)));
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
	if (phase_ != Phase::Driving) return out; // only the pre-spawn drive window
	// Retail learns the authoritative mission through 0x7B before beginning this drive. A pre-load
	// binding additionally holds here until it has installed that mission locally.
	if (!mission_known_ || !world_ready_) return out;
	// If 0x11 arrived during a load hold, first replay the early retail drive stages. Releasing the
	// spawn-menu request sooner lets a fast host stream the named 0x0C and move us to InMatch before
	// the required 0x2F/0x0B stage has been sent. At stage 3, emit 0x0A immediately before that final
	// loadout/status packet so the host observes the witnessed order without a tick gap.
	if (pending_spawn_menu_request_ && pump_stage_ >= 3) {
		out.push_back(frame_inner(0x0A, {}));
		pending_spawn_menu_request_ = false;
	}
	switch (pump_stage_) {
	case 0: { // 0x48 client-ack (echo our host-assigned ConnectionId, learned from the 0x82 MI) +
	          // 0x37 mission request (begins world streaming). The host stamps our dcb into our 0x0C
	          // ownerConnectionId from this ack (F3); bundling it with the first driving packet keeps
	          // it "before world streaming" as witnessed (capture: ack precedes the world stream).
	          // [orig: client 0x48 ConnectionId ack]
		const uint32_t id = conn_.connection_id;
		std::vector<uint8_t> ack = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
		                            static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24)};
		out.push_back(frame_session({
				make_protocol_message(0x48, std::move(ack)),
				make_protocol_message(0x37, {}),
		}));
		break;
	}
	case 1: // transition marker
		out.push_back(frame_session({make_protocol_message(0x09, {})}));
		break;
	case 2: // player-sync ack (witnessed body)
		out.push_back(frame_session({make_protocol_message(0x22, {0x00, 0xF7, 0x1C})}));
		break;
	case 3: { // loadout (0x2F x2) + mission-status (0x0B) burst -> trips the spawn gate
		// The retail v14 body is six header bytes, seven four-byte ADM rows, then the
		// mandatory 0xFF ADM terminator.  A zero-filled 35-byte placeholder has no
		// terminator and is correctly rejected by the exact server decoder.
		std::vector<uint8_t> loadout = {0x02, 0x08, 0xC3, 0x00, 0x00, 0x00};
		for (uint8_t adm : {uint8_t(21), uint8_t(3), uint8_t(83), uint8_t(76),
		                    uint8_t(77), uint8_t(78), uint8_t(2)}) {
			loadout.push_back(adm);
			loadout.push_back(0xFF);
			loadout.push_back(0xFF);
			loadout.push_back(0xFF);
		}
		loadout.push_back(0xFF);
		out.push_back(frame_session({
				make_protocol_message(0x2F, loadout),
				make_protocol_message(0x2F, std::move(loadout)),
				make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
		}));
		break;
	}
	default:
		// Keepalive after the gate-tripping burst: the real client never goes silent mid-join — it
		// keeps streaming C2S status while the server's spawn gate opens (which takes several server
		// ticks of world streaming). The host detects the spawn REACTIVELY, on an incoming SESSION
		// packet (PeerSpawned is surfaced from handle_server_datagram, not from the tick), so the
		// joiner must keep sending a 0x43 each frame or the gate could open with no inbound packet
		// to surface it on. Re-send the witnessed player-sync ack (idempotent host-side). [§5.38b]
		out.push_back(frame_session({make_protocol_message(0x22, {0x00, 0xF7, 0x1C})}));
		break;
	}
	++pump_stage_;
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
	phase_ = Phase::InMatch;
}

void JoinerConnection::fail(std::string reason) {
	last_error_ = std::move(reason);
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	phase_ = Phase::Error;
}

} // namespace opennova::np
