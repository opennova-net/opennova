#include "npruntime/joiner_connection.h"

#include <npwire/ingame_encode.h>
#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <utility>

// Verbatim port of novaworld::JoinerSession (the client mirror), with SCRK/seq/ack stored on the
// type-2 NapiNPConnection conn_. The D.0 name-match in on_server_session is copied byte-for-byte.
namespace opennova::np {

JoinerConnection::JoinerConnection(ClientSession::Config config, std::string player_name)
		: cfg_(std::move(config)), player_name_(std::move(player_name)) {
	conn_.type = 2;                  // client-side connection (the joiner's view of the host)
	conn_.player_name = player_name_;
	// Game-session 0x42 character vars — the per-side character selection the HOST folds into
	// our player record (per-side minimap/char ids, requested side, soldier classes, avatar
	// bytes). Without them the host stamps animSlot/NetId defaults and OTHER retail clients bind
	// our player to a wrong-type character slot (the mirror of D-NET-146). Values = a fresh
	// retail profile's defaults, wire-witnessed on both LAN captures (retail-ashi5a f=199140 /
	// retail_join_v18 f=47676): CI0=512 (0x0200) CI1=33287 (0x8207) TR=-1 CTA=CTB=8 (rifleman)
	// VCA=1 VCB=4. Only append when the caller hasn't provided its own set. [orig: client emit
	// CNapiServerInfo_SerializeToSession @0x4c3650, type-2 chunks; host consume
	// NapiNetConfig_LoadFromConnTags @0x4c7260 -> Server_PlayerAdd @0x51cbc0]
	const bool has_char_vars = [&] {
		for (const auto &v : cfg_.cu_vars) {
			if (v.name == "CI0" || v.name == "VCA") return true;
		}
		return false;
	}();
	if (!has_char_vars) {
		const std::pair<const char *, const char *> kCharVars[] = {
				{"CI0", "512"}, {"CI1", "33287"}, {"TR", "-1"},  {"CTA", "8"},
				{"CTB", "8"},   {"VCA", "1"},     {"VCB", "4"},
		};
		for (const auto &[name, value] : kCharVars) {
			cfg_.cu_vars.push_back(ClientSession::Config::CuVar{name, value, /*type=*/2});
		}
	}
}

std::vector<uint8_t> JoinerConnection::start() {
	conn_.client_scrk = make_dev_scrk();
	server_hk_ = 0;
	conn_.server_sk = 0;
	conn_.server_scrk.clear();
	conn_.seq = SessionSequencing{1, 0};
	pump_stage_ = 0;
	has_self_handle_ = false;
	self_handle_ = 0;
	spawn_ = SelfSpawn{};
	last_error_.clear();
	phase_ = Phase::Hello;
	return build_client_hello();
}

std::vector<uint8_t> JoinerConnection::build_client_hello() {
	// CO = the joiner's player name (the host echoes it into the organic-spawn entity_name — the
	// name-match key). The identity struct-fill is shared with the lobby ClientSession (make_client_hello
	// in novaworld/client_session.h); only the framing envelope (nw_encode_outbound) differs.
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
	                          client_hello_to_bytes(make_client_hello(cfg_, player_name_)));
}

std::vector<uint8_t> JoinerConnection::build_client_auth() {
	ClientAuth auth = make_client_auth(cfg_, player_name_, server_hk_, conn_.client_scrk);
	// The GAME-session 0x42's NA TLV is the player CALLSIGN — the retail host's display-name
	// source (the golden joiner's 0x0C record name equals its NA). The "jop:cus2" gate tag is
	// the NOVAWORLD-gate connect's NA, not the game join's. [wire: retail-ashi5a f=199140
	// na="FooPlayer" / retail_join_v18 f=47676 na="TestPlayer"; net-re §5.0b]
	auth.na = player_name_;
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
	out.outbound.push_back(build_client_auth());
}

void JoinerConnection::on_server_auth(const std::vector<uint8_t> &body, PollResult &out) {
	(void)out;
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
	// NO auto-emit here. pump() drives the in-match spawn-gate burst; the game connection has no
	// lobby-verify leg.
}

void JoinerConnection::on_server_session(const std::vector<uint8_t> &body, PollResult &out) {
	// Joiner recv: decrypt inbound 0x83 with the server's SCRK; deframe latches conn_.seq.last_inbound_seq.
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	if (!deframe_session_packet(conn_.seq, SessionCrypto{{}, conn_.server_scrk, 0}, body.data(),
	                            body.size(), hdr, messages)) {
		return; // lenient: an in-match streaming packet we can't parse is not fatal
	}
	for (const ProtocolMessage &m : messages) {
		if (m.tag == 0x08) {
			// Our initial-state burst carries the same g_GameType in the fixed
			// session-config block before any live frame. Retail keeps this equal
			// to the later 0x7B `extra` value.
			SessionConfig config;
			if (decode_session_config(m.payload.data(), m.payload.size(), config))
				game_type_ = static_cast<uint32_t>(config.fields[3]);
		} else if (m.tag == 0x7B) {
			// The authoritative off-wire layout hint for later 0x0A phase-3
			// frames. Every full-player-info record repeats g_GameType in extra.
			FullPlayerInfo info;
			if (decode_full_player_info(m.payload.data(), m.payload.size(), info))
				game_type_ = info.extra;
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
		} else if (m.tag == 0x11) {
			// S2C 0x11 ends the player-sync bundle; the retail client answers with the EMPTY C2S
			// 0x0A spawn-menu request — the message that advances the host's session sync state
			// to 4 and starts the §5.2a world stream [orig: NapiNPServerMsg_HandlePlayerSpawnRequest
			// @0x513260; golden retail-ashi5a S 0x11 f=201572 -> C 0x0A f=201573]. A host (ours or
			// retail) never streams the world to a joiner that hasn't sent it (D-NET-150).
			out.outbound.push_back(frame_inner(0x0A, {}));
		}
		// Other tags (game-start bundle scalars, world-state-load 0x0F) are not entity data.
	}
}

std::vector<std::vector<uint8_t>> JoinerConnection::pump(uint32_t /*now_tick*/) {
	std::vector<std::vector<uint8_t>> out;
	if (phase_ != Phase::Driving) return out; // only the pre-spawn drive window
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
	conn_.seq = SessionSequencing{next_seq, last_ack}; // frame_session stamps next_seq then post-increments; last_ack -> 0x43 ack_count
	self_handle_ = self_handle;
	has_self_handle_ = true;
	spawn_.item_type_id = self_type;
	game_type_ = game_type;
	phase_ = Phase::InMatch;
}

void JoinerConnection::fail(std::string reason) {
	last_error_ = std::move(reason);
	phase_ = Phase::Error;
}

} // namespace opennova::np
