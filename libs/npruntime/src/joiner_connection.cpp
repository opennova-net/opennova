#include "npruntime/joiner_connection.h"

#include <novaworld/ingame_encode.h>
#include <novaworld/nw_session_framing.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <utility>

// Verbatim port of novaworld::JoinerSession (the client mirror), with SCRK/seq/ack stored on the
// type-2 NapiNPConnection conn_. The D.0 name-match in on_server_session is copied byte-for-byte.
namespace opennova::np {

JoinerConnection::JoinerConnection(ClientSession::Config config, std::string player_name)
		: cfg_(std::move(config)), player_name_(std::move(player_name)) {
	conn_.type = 2;                  // client-side connection (the joiner's view of the host)
	conn_.player_name = player_name_;
}

std::vector<uint8_t> JoinerConnection::start() {
	conn_.client_scrk = make_dev_scrk();
	server_hk_ = 0;
	conn_.server_sk = 0;
	conn_.server_scrk.clear();
	conn_.next_outbound_seq = 1;
	conn_.last_inbound_seq = 0;
	pump_stage_ = 0;
	has_self_handle_ = false;
	self_handle_ = 0;
	spawn_ = SelfSpawn{};
	last_error_.clear();
	phase_ = Phase::Hello;
	return build_client_hello();
}

std::vector<uint8_t> JoinerConnection::build_client_hello() {
	ClientHello hello;
	hello.nvs  = cfg_.nvs;
	hello.co   = player_name_; // the name-match key: the host echoes CO into the organic-spawn entity_name
	hello.ap   = cfg_.ap;
	hello.bdat = cfg_.bdat;
	hello.pn   = cfg_.pn;
	hello.pv1  = cfg_.pv1;
	hello.pv2  = cfg_.pv2;
	hello.pg   = cfg_.pg;       // jointoperations() sets the explicit JO GUID (use_default_pg=false);
	hello.pg_present = true;    // our host validates PN only, so PG bytes are non-gating here
	hello.ci   = cfg_.client_index;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
}

std::vector<uint8_t> JoinerConnection::build_client_auth() {
	ClientAuth auth;
	// Identity block — same values as the hello (the host re-validates PN; the real NW server
	// re-validates the whole block on the 0x42 join).
	auth.nvs  = cfg_.nvs;
	auth.co   = player_name_;
	auth.ap   = cfg_.ap;
	auth.bdat = cfg_.bdat;
	auth.pn   = cfg_.pn;
	auth.pg   = cfg_.pg;
	auth.pg_present = true;
	auth.pv1  = cfg_.pv1;
	auth.pv2  = cfg_.pv2;

	auth.ci   = cfg_.client_index;
	auth.hk   = server_hk_;     // echo ServerHello.hk
	auth.ck   = cfg_.client_key;
	auth.na   = cfg_.na;
	auth.scrk = conn_.client_scrk;
	for (const auto &v : cfg_.cu_vars) {
		auth.cu.push_back(make_client_cu_chunk(v.type, v.name, v.value));
	}
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
}

std::vector<uint8_t> JoinerConnection::frame_session(const std::vector<ProtocolMessage> &messages) {
	ProtocolPacketHeader hdr;
	hdr.session_id = conn_.server_sk; // peer's local_key = the server's SK (ServerAuth.sk)
	hdr.seq_num = conn_.next_outbound_seq++;
	hdr.ack_count = conn_.last_inbound_seq;
	hdr.connection_flags = 0;
	std::vector<uint8_t> body;
	if (!encode_protocol_packet_plaintext(hdr, messages, conn_.client_scrk, body)) {
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
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), conn_.server_scrk, hdr, messages)) {
		return; // lenient: an in-match streaming packet we can't parse is not fatal
	}
	conn_.last_inbound_seq = hdr.seq_num;
	for (const ProtocolMessage &m : messages) {
		if (m.tag == 0x0C) {
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
	case 3: // loadout (0x2F x2) + mission-status (0x0B) burst -> trips the spawn gate
		out.push_back(frame_session({
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
		}));
		break;
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

void JoinerConnection::fail(std::string reason) {
	last_error_ = std::move(reason);
	phase_ = Phase::Error;
}

} // namespace opennova::np
