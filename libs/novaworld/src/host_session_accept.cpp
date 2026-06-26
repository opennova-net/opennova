#include <novaworld/host_session_accept.h>

#include <novaworld/nw_session_framing.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <novaworld/session_protocol.h>

#include <cstdio>
#include <utility>

namespace opennova {

HostSessionAccept::HostSessionAccept(GameServerRuntimeConfig config)
		: game_runtime_(std::move(config)) {}

void HostSessionAccept::start() { game_runtime_.start(); }
void HostSessionAccept::stop() { game_runtime_.stop(); }

void HostSessionAccept::configure(GameServerRuntimeConfig config) {
	game_runtime_.configure(std::move(config));
	peers_.clear();
}

std::string HostSessionAccept::peer_session_id(const PeerAddr &peer) {
	// PeerAddr.ip is LE octet packing (a | b<<8 | c<<16 | d<<24); print low->high
	// so the label reads a.b.c.d:port (matches nw_udp_listener's client_label).
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
	              peer.ip & 0xffu, (peer.ip >> 8) & 0xffu,
	              (peer.ip >> 16) & 0xffu, (peer.ip >> 24) & 0xffu, peer.port);
	return buf;
}

HostJoinerPose HostSessionAccept::pose_from_session(const GameSessionState &gss) const {
	HostJoinerPose p;
	if (gss.client_pos_valid) {
		// The joiner already sent a C2S 0x0C — spawn it where it reported.
		p.pos_valid = true;
		p.entity_handle = gss.client_entity_handle;
		p.item_type_id = gss.client_item_type_id != 0 ? gss.client_item_type_id : 0x14B9u;
		p.pos_x = static_cast<int32_t>(gss.client_pos_x);
		p.pos_y = static_cast<int32_t>(gss.client_pos_y);
		p.pos_z = static_cast<int32_t>(gss.client_pos_z);
		p.heading = gss.client_heading;
		p.pitch = gss.client_pitch;
	} else {
		// No uplink yet — fall back to the host-advertised spawn (the same
		// position the game-start bundle's 0x0F told the joiner).
		const GameSessionConfig &cfg = game_runtime_.config().session;
		p.pos_valid = false;
		p.item_type_id = 0x14B9u;
		p.pos_x = static_cast<int32_t>(cfg.spawn_x);
		p.pos_y = static_cast<int32_t>(cfg.spawn_y);
		p.pos_z = static_cast<int32_t>(cfg.spawn_z);
	}
	p.team = 1; // co-op default; the MP game-type/team matrix is a later pass.
	return p;
}

std::vector<uint8_t> HostSessionAccept::frame_session_replies(
		PeerState &state, const std::vector<ProtocolMessage> &replies) {
	// [orig: SESSION reply build apps/novaworld_server/nw_udp_listener.cpp:606-627]
	ProtocolPacketHeader rhdr;
	rhdr.session_id = state.client_ck;            // retail's local_key == ClientAuth.ck
	rhdr.seq_num = state.next_outbound_seq++;
	rhdr.ack_count = state.last_inbound_seq;
	rhdr.connection_flags = 0;

	std::vector<uint8_t> body_out;
	if (!encode_protocol_packet_plaintext(rhdr, replies, state.server_scrk, body_out)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body_out));
}

HostSessionAccept::HandleResult HostSessionAccept::handle_datagram(
		const PeerAddr &peer, const uint8_t *raw, size_t len, uint32_t now_tick) {
	HandleResult out;

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(raw, len, opcode, body)) {
		return out; // bad envelope — drop quietly
	}

	switch (opcode) {
	case SESSION_OPCODE_CLIENT_HELLO: {
		ClientHello hello;
		if (!parse_client_hello(body.data(), body.size(), hello)) break;
		if (classify_session_protocol(hello.pn) != SessionProtocolKind::JointOperations) {
			break; // not an in-match game join — the owner routes lobby PNs elsewhere
		}
		PeerState &st = peers_[peer];
		st.pn = hello.pn;
		st.player_name = hello.co; // the joiner's player name (CO is free/unvalidated);
		                           // streamed back in the organic-spawn 0x0C name-match
		st.session_id = peer_session_id(peer);
		// client_ip_net: the builders take the IP as the four payload octets in
		// LE packing (so retail's positional TLV reader prints a.b.c.d) — pass
		// peer.ip verbatim, matching nw_udp_listener. [orig: ip_to_le note]
		ServerHello reply = build_server_hello(hello, peer.ip, peer.port);
		out.outbound.push_back(
				nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply)));
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
		ev.peer = peer;
		out.events.push_back(std::move(ev));
		break;
	}

	case SESSION_OPCODE_CLIENT_AUTH: {
		ClientAuth auth;
		if (!parse_client_auth(body.data(), body.size(), auth)) break;
		PeerState &st = peers_[peer];
		if (st.session_id.empty()) st.session_id = peer_session_id(peer);
		// auth.scrk decrypts inbound SESSION; our server_scrk encrypts outbound
		// SESSION and is echoed in ServerAuth so the client can read our replies.
		st.client_scrk = auth.scrk;
		st.server_scrk = make_dev_scrk();
		st.client_ck = auth.ck;
		st.server_sk = make_random_session_u32();
		ServerAuth reply = build_server_auth(auth, peer.ip, peer.port, st.server_sk,
		                                     st.server_scrk, /*novaworld_name=*/"NWServer",
		                                     /*novaworld_web_url=*/"http://127.0.0.1:8080",
		                                     /*nwuid=*/make_dev_nwuid());
		out.outbound.push_back(
				nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(reply)));
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
		ev.peer = peer;
		out.events.push_back(std::move(ev));
		break;
	}

	case SESSION_OPCODE_PROTOCOL_MESSAGE: {
		auto it = peers_.find(peer);
		if (it == peers_.end() || it->second.client_scrk.empty()) {
			break; // SESSION before AUTH completed — drop
		}
		PeerState &st = it->second;

		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> messages;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), st.client_scrk,
		                                       hdr, messages)) {
			break;
		}
		st.last_inbound_seq = hdr.seq_num;

		// Learn the joiner's own ConnectionId (NapiNPConnection.unk_18 = its dcb) from its
		// in-match 0x48 client-ack (a 4-byte LE u32). This is the value the client's
		// Player_FindLocalPlayerEntity @0x4e0090 compares entity+0x78 against, so the host
		// MUST stamp it into the joiner's 0x0C entity_flags — a guessed sequential id does
		// NOT match (witnessed: capture2.pcapng ack=0x113F vs our guessed 3 -> crash; the
		// working retail join had ack==eFlags==3). The ack arrives during the early
		// handshake, before world streaming, so it's known by the time we stream the 0x0C.
		for (const ProtocolMessage &m : messages) {
			if (m.tag == 0x48 && m.payload.size() >= 4) {
				st.self_id = static_cast<uint32_t>(m.payload[0]) |
						(static_cast<uint32_t>(m.payload[1]) << 8) |
						(static_cast<uint32_t>(m.payload[2]) << 16) |
						(static_cast<uint32_t>(m.payload[3]) << 24);
				st.self_id_seen = true;
			}
		}

		// Drive the reactive handshake/spawn state machine (also caches the
		// joiner's 0x0C pose into client_*); BYPASS GameSession::tick here — the
		// periodic emitter runs via tick_handshakes (pre-Spawned) / NetSystem.
		SessionProtocolDispatchResult disp = dispatch_in_match_session_messages(
				SessionProtocolKind::JointOperations, game_runtime_, st.session_id,
				messages, now_tick);
		if (!disp.replies.empty()) {
			std::vector<uint8_t> dg = frame_session_replies(st, disp.replies);
			if (!dg.empty()) out.outbound.push_back(std::move(dg));
		}

		const GameSessionState *gss = game_runtime_.session_state(st.session_id);

		// Surface the streaming-entered event once the host's tick has begun emitting
		// entity batches (the joiner is provably in its world-load pump). Checked BEFORE
		// the spawned latch so the owner admits early + streams the joiner's own 0x0C
		// during load, ahead of the game-start bundle's 0x0F — else the client's
		// Player_InitPlayer can't find its dcb (F3). Mirrors is_ready_for_late_spawn_
		// acceptance's entity_batch_count>0 readiness; one-shot via world_stream_announced.
		// Also gate on self_id_seen: the joiner's 0x0C entity_flags must carry its real
		// ConnectionId (from the 0x48 ack), so don't admit/stream until we've learned it.
		// (The ack always precedes streaming, so this never actually defers in practice.)
		if (gss != nullptr && gss->entity_batch_count > 0 && !gss->spawned &&
				st.self_id_seen && !st.world_stream_announced) {
			st.world_stream_announced = true;
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerEnteredWorldStreaming;
			ev.peer = peer;
			ev.pose = pose_from_session(*gss);
			ev.self_id = st.self_id;
			ev.peer_name = st.player_name;
			out.events.push_back(std::move(ev));
		}

		// Detect the handshake reaching Spawned — surface the joiner's pose once.
		if (gss != nullptr && gss->spawned && !st.spawned_announced) {
			st.spawned_announced = true;
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerSpawned;
			ev.peer = peer;
			ev.pose = pose_from_session(*gss);
			ev.self_id = st.self_id;
			ev.peer_name = st.player_name; // for the joiner-side name-match (D.0)
			out.events.push_back(std::move(ev));
		}

		// Once a connection exists, surface the joiner's in-match C2S 0x0C
		// uplinks for NetSystem to read-apply. Pre-spawn 0x0C only updates the
		// cached pose (handled above) — no connection to route it to yet.
		if (st.spawned_announced) {
			std::vector<ProtocolMessage> c2s;
			for (const ProtocolMessage &m : messages) {
				if (m.tag == 0x0C) c2s.push_back(m);
			}
			if (!c2s.empty()) {
				HostAcceptEvent ev;
				ev.kind = HostAcceptEvent::Kind::PeerC2SInMatch;
				ev.peer = peer;
				ev.in_match_c2s = std::move(c2s);
				out.events.push_back(std::move(ev));
			}
		}
		break;
	}

	case SESSION_OPCODE_CLIENT_GOODBYE: {
		game_runtime_.reset_session(peer_session_id(peer));
		peers_.erase(peer);
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
		ev.peer = peer;
		out.events.push_back(std::move(ev));
		break;
	}

	default:
		break;
	}

	return out;
}

std::vector<HostSessionAccept::TickOut> HostSessionAccept::tick_handshakes(
		int elapsed_ms, uint32_t now_tick) {
	std::vector<TickOut> out;
	for (auto &kv : peers_) {
		PeerState &st = kv.second;
		const GameSessionState *gss = game_runtime_.session_state(st.session_id);
		if (gss == nullptr) continue; // session not created yet (no 0x43 dispatched)
		if (gss->spawned) continue;   // NetSystem owns spawned peers' per-frame 0x0A
		GameServerDispatch disp = game_runtime_.tick_session(st.session_id, elapsed_ms, now_tick);

		TickOut to;
		to.peer = kv.first;
		if (!disp.replies.empty()) {
			std::vector<uint8_t> dg = frame_session_replies(st, disp.replies);
			if (!dg.empty()) to.outbound.push_back(std::move(dg));
		}

		// This loop drives the phase into WorldStreaming + increments entity_batch_count,
		// so it catches the streaming transition on the tick it happens. Re-read post-tick
		// state (gss points at the live session struct, mutated by tick_session). Surface
		// PeerEnteredWorldStreaming once so the owner admits early + streams the joiner's
		// own dcb-bearing 0x0C before the game-start bundle (F3). Same latch as the
		// datagram-driven path, whichever observes batches first wins.
		if (gss->entity_batch_count > 0 && !gss->spawned && st.self_id_seen &&
				!st.world_stream_announced) {
			st.world_stream_announced = true;
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerEnteredWorldStreaming;
			ev.peer = kv.first;
			ev.pose = pose_from_session(*gss);
			ev.self_id = st.self_id;
			ev.peer_name = st.player_name;
			to.events.push_back(std::move(ev));
		}

		if (to.outbound.empty() && to.events.empty()) continue;
		out.push_back(std::move(to));
	}
	return out;
}

bool HostSessionAccept::frame_in_match_s2c(const PeerAddr &peer, uint8_t inner_tag,
                                           const std::vector<uint8_t> &inner_body,
                                           std::vector<uint8_t> &out_datagram) {
	auto it = peers_.find(peer);
	if (it == peers_.end() || it->second.server_scrk.empty()) {
		return false;
	}
	ProtocolMessage msg = make_protocol_message(inner_tag, inner_body);
	out_datagram = frame_session_replies(it->second, std::vector<ProtocolMessage>{std::move(msg)});
	return !out_datagram.empty();
}

bool HostSessionAccept::peer_spawned(const PeerAddr &peer) const {
	auto it = peers_.find(peer);
	return it != peers_.end() && it->second.spawned_announced;
}

bool HostSessionAccept::bind_peer_player_entity(
		const PeerAddr &peer, uint8_t player_slot, uint16_t entity_handle) {
	auto it = peers_.find(peer);
	if (it == peers_.end() || it->second.session_id.empty()) {
		return false;
	}
	const std::string player_name = !it->second.player_name.empty()
			? it->second.player_name
			: game_runtime_.config().session.player_name;
	return game_runtime_.bind_session_player(
			it->second.session_id, player_name, player_slot, entity_handle);
}

} // namespace opennova
