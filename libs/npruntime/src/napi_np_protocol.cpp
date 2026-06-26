#include "npruntime/napi_np_protocol.h"

#include <novaworld/game_server_runtime.h>
#include <novaworld/nw_session_framing.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <novaworld/session_protocol.h>

#include <cstdio>
#include <utility>

namespace opennova::np {

// Out-of-line ctor/dtor/move: GameServerRuntime is incomplete in the ctx header (forward-declared
// to keep game_session.h out of it), so the unique_ptr<GameServerRuntime> ctor/dtor must be
// emitted here, where the type is complete.
NapiNPServerCtx::NapiNPServerCtx() = default;
NapiNPServerCtx::~NapiNPServerCtx() = default;
NapiNPServerCtx::NapiNPServerCtx(NapiNPServerCtx &&) noexcept = default;
NapiNPServerCtx &NapiNPServerCtx::operator=(NapiNPServerCtx &&) noexcept = default;

namespace {

// Stable "a.b.c.d:port" label — the GameServerRuntime sessions_ key. PeerAddr.ip is LE octet
// packing (a | b<<8 | c<<16 | d<<24); print low->high so the label reads a.b.c.d (matches
// nw_udp_listener's client_label). [orig: HostSessionAccept::peer_session_id]
std::string peer_session_id(const PeerAddr &peer) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
	              peer.ip & 0xffu, (peer.ip >> 8) & 0xffu,
	              (peer.ip >> 16) & 0xffu, (peer.ip >> 24) & 0xffu, peer.port);
	return buf;
}

NapiNPConnection *find_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	for (auto &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) return &c;
	}
	return nullptr;
}

// Linear scan + create-on-miss over connection_list — faithful to the original intrusive-list walk
// (NapiNPServer_SendFiltered @0x4C87E0 -> SendToConn per node). connection_list stays authoritative
// (P4 reconciles it vs the NetSystem table). A fresh node is a server-side connection (type 1).
NapiNPConnection &find_or_create_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	if (NapiNPConnection *existing = find_connection(ctx, peer)) return *existing;
	NapiNPConnection node;
	node.peer = peer;
	node.type = 1; // server-side: the host's view of a client
	ctx.np_protocol.connection_list.push_back(std::move(node));
	return ctx.np_protocol.connection_list.back();
}

HostJoinerPose pose_from_session(NapiNPServerCtx &ctx, const GameSessionState &gss) {
	// [orig: HostSessionAccept::pose_from_session]
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
		// No uplink yet — fall back to the host-advertised spawn (the same position the
		// game-start bundle's 0x0F told the joiner).
		const GameSessionConfig &cfg = ctx.game_runtime->config().session;
		p.pos_valid = false;
		p.item_type_id = 0x14B9u;
		p.pos_x = static_cast<int32_t>(cfg.spawn_x);
		p.pos_y = static_cast<int32_t>(cfg.spawn_y);
		p.pos_z = static_cast<int32_t>(cfg.spawn_z);
	}
	p.team = 1; // co-op default; the MP game-type/team matrix is a later pass.
	return p;
}

std::vector<uint8_t> frame_session_replies(NapiNPConnection &conn,
                                           const std::vector<ProtocolMessage> &replies) {
	// [orig: SESSION reply build apps/novaworld_server/nw_udp_listener.cpp:606-627]
	ProtocolPacketHeader rhdr;
	rhdr.session_id = conn.client_ck;            // retail's local_key == ClientAuth.ck
	rhdr.seq_num = conn.next_outbound_seq++;
	rhdr.ack_count = conn.last_inbound_seq;
	rhdr.connection_flags = 0;

	std::vector<uint8_t> body_out;
	if (!encode_protocol_packet_plaintext(rhdr, replies, conn.server_scrk, body_out)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body_out));
}

// 0x41 ClientHello -> 0x81 ServerHello. [orig: NapiNPProtocol_HandleClientHello @0x6213b0]
void handle_client_hello(NapiNPServerCtx &ctx, const PeerAddr &peer,
                         const std::vector<uint8_t> &body, HandleResult &out) {
	ClientHello hello;
	if (!parse_client_hello(body.data(), body.size(), hello)) return;
	if (classify_session_protocol(hello.pn) != SessionProtocolKind::JointOperations) {
		return; // not an in-match game join — the owner routes lobby PNs elsewhere (no node created)
	}
	NapiNPConnection &conn = find_or_create_connection(ctx, peer);
	conn.pn = hello.pn;
	conn.player_name = hello.co; // the joiner's player name (CO is free/unvalidated); streamed back
	                             // in the organic-spawn 0x0C name-match
	conn.session_id = peer_session_id(peer);
	conn.phase = ConnectionPhase::HelloReceived;
	// client_ip_net: the builders take the IP as the four payload octets in LE packing (so retail's
	// positional TLV reader prints a.b.c.d) — pass peer.ip verbatim, matching nw_udp_listener.
	ServerHello reply = build_server_hello(hello, peer.ip, peer.port);
	// R1: advertise our real host key (seed-injected via SessionStartup) rather than
	// build_server_hello's placeholder default, when one is set. The retail 0x81 carries host_key.
	if (ctx.np_protocol.host_key != 0) reply.hk = ctx.np_protocol.host_key;
	out.outbound.push_back(
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply)));
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

// 0x42 ClientAuth -> 0x82 ServerAuth (per-session SCRK established).
// [orig: NapiNPProtocol_HandleClientJoin @0x62b750]
void handle_client_join(NapiNPServerCtx &ctx, const PeerAddr &peer,
                        const std::vector<uint8_t> &body, HandleResult &out) {
	ClientAuth auth;
	if (!parse_client_auth(body.data(), body.size(), auth)) return;
	NapiNPConnection &conn = find_or_create_connection(ctx, peer);
	if (conn.session_id.empty()) conn.session_id = peer_session_id(peer);
	// auth.scrk decrypts inbound SESSION; our server_scrk encrypts outbound SESSION and is echoed
	// in ServerAuth so the client can read our replies.
	conn.client_scrk = auth.scrk;
	conn.client_ck = auth.ck;
	// R2: mint the server SCRK / SK randomly (retail) unless a deterministic source is forced
	// (golden byte-parity). [orig: make_dev_scrk / make_random_session_u32 / make_dev_nwuid]
	std::string nwuid;
	if (ctx.server_key_mint.forced) {
		conn.server_scrk = ctx.server_key_mint.server_scrk;
		conn.server_sk = ctx.server_key_mint.server_sk;
		nwuid = ctx.server_key_mint.nwuid;
	} else {
		conn.server_scrk = make_dev_scrk();
		conn.server_sk = make_random_session_u32();
		nwuid = make_dev_nwuid();
	}
	conn.phase = ConnectionPhase::Joined;
	ServerAuth reply = build_server_auth(auth, peer.ip, peer.port, conn.server_sk, conn.server_scrk,
	                                     ctx.server_key_mint.novaworld_name,
	                                     ctx.server_key_mint.novaworld_web_url, nwuid);
	out.outbound.push_back(
			nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(reply)));
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

// 0x43 SESSION -> 0x83 SESSION (drives the GameSession to Spawned; surfaces F3 + spawn + in-match
// C2S). [orig: NapiNPProtocol_HandleSessionPacket @0x626A00]
void handle_client_session(NapiNPServerCtx &ctx, const PeerAddr &peer,
                           const std::vector<uint8_t> &body, uint32_t now_tick, HandleResult &out) {
	NapiNPConnection *connp = find_connection(ctx, peer);
	if (connp == nullptr || connp->client_scrk.empty()) {
		return; // SESSION before AUTH completed — drop
	}
	NapiNPConnection &conn = *connp;

	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), conn.client_scrk,
	                                       hdr, messages)) {
		return;
	}
	conn.last_inbound_seq = hdr.seq_num;

	// Learn the joiner's own ConnectionId (NapiNPConnection.unk_18 = its dcb, our connection_id)
	// from its in-match 0x48 client-ack (a 4-byte LE u32). This is the value the client's
	// Player_FindLocalPlayerEntity @0x4e0090 compares entity+0x78 against, so the host MUST stamp it
	// into the joiner's 0x0C entity_flags — a guessed sequential id does NOT match (witnessed:
	// capture2.pcapng ack=0x113F vs our guessed 3 -> crash; the working retail join had
	// ack==eFlags==3). The ack arrives during the early handshake, before world streaming, so it's
	// known by the time we stream the 0x0C.
	for (const ProtocolMessage &m : messages) {
		if (m.tag == 0x48 && m.payload.size() >= 4) {
			conn.connection_id = static_cast<uint32_t>(m.payload[0]) |
					(static_cast<uint32_t>(m.payload[1]) << 8) |
					(static_cast<uint32_t>(m.payload[2]) << 16) |
					(static_cast<uint32_t>(m.payload[3]) << 24);
			conn.self_id_seen = true;
		}
	}

	// Drive the reactive handshake/spawn state machine (also caches the joiner's 0x0C pose into
	// client_*); BYPASS GameSession::tick here — the periodic emitter runs via tick_connections
	// (pre-Spawned) / NetSystem.
	SessionProtocolDispatchResult disp = dispatch_in_match_session_messages(
			SessionProtocolKind::JointOperations, *ctx.game_runtime, conn.session_id,
			messages, now_tick);
	if (!disp.replies.empty()) {
		std::vector<uint8_t> dg = frame_session_replies(conn, disp.replies);
		if (!dg.empty()) out.outbound.push_back(std::move(dg));
	}

	const GameSessionState *gss = ctx.game_runtime->session_state(conn.session_id);

	// Surface the streaming-entered event once the host's tick has begun emitting entity batches
	// (the joiner is provably in its world-load pump). Checked BEFORE the spawned latch so the owner
	// admits early + streams the joiner's own 0x0C during load, ahead of the game-start bundle's
	// 0x0F — else the client's Player_InitPlayer can't find its dcb (F3). Mirrors
	// is_ready_for_late_spawn_ acceptance's entity_batch_count>0 readiness; one-shot via
	// world_stream_announced. Also gate on self_id_seen: the joiner's 0x0C entity_flags must carry
	// its real ConnectionId (from the 0x48 ack), so don't admit/stream until we've learned it. (The
	// ack always precedes streaming, so this never actually defers in practice.)
	if (gss != nullptr && gss->entity_batch_count > 0 && !gss->spawned &&
			conn.self_id_seen && !conn.world_stream_announced) {
		conn.world_stream_announced = true;
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerEnteredWorldStreaming;
		ev.peer = peer;
		ev.pose = pose_from_session(ctx, *gss);
		ev.self_id = conn.connection_id;
		ev.peer_name = conn.player_name;
		out.events.push_back(std::move(ev));
	}

	// Detect the handshake reaching Spawned — surface the joiner's pose once.
	if (gss != nullptr && gss->spawned && !conn.spawned_announced) {
		conn.spawned_announced = true;
		conn.phase = ConnectionPhase::Spawned;
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerSpawned;
		ev.peer = peer;
		ev.pose = pose_from_session(ctx, *gss);
		ev.self_id = conn.connection_id;
		ev.peer_name = conn.player_name; // for the joiner-side name-match (D.0)
		out.events.push_back(std::move(ev));
	}

	// Once a connection exists, surface the joiner's in-match C2S 0x0C uplinks for NetSystem to
	// read-apply. Pre-spawn 0x0C only updates the cached pose (handled above) — no connection to
	// route it to yet.
	if (conn.spawned_announced) {
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
}

// 0x46 ClientGoodbye. [orig: Nwu_HandleClientGoodbye @0x624250]
void handle_client_goodbye(NapiNPServerCtx &ctx, const PeerAddr &peer, HandleResult &out) {
	if (ctx.game_runtime) ctx.game_runtime->reset_session(peer_session_id(peer));
	auto &list = ctx.np_protocol.connection_list;
	for (auto it = list.begin(); it != list.end(); ++it) {
		if (it->peer == peer) {
			list.erase(it);
			break;
		}
	}
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

} // namespace

void configure_session_runtime(NapiNPServerCtx &ctx, GameServerRuntimeConfig config) {
	if (!ctx.game_runtime) {
		ctx.game_runtime = std::make_unique<GameServerRuntime>(std::move(config));
	} else {
		ctx.game_runtime->configure(std::move(config));
	}
	ctx.game_runtime->start();
	ctx.np_protocol.connection_list.clear(); // [orig: HostSessionAccept::configure clears peers_]
}

HandleResult handle_server_datagram(NapiNPServerCtx &ctx, const PeerAddr &peer,
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick) {
	HandleResult out;

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(raw, len, opcode, body)) {
		return out; // bad envelope — drop quietly
	}

	switch (opcode) {
	case SESSION_OPCODE_CLIENT_HELLO:
		handle_client_hello(ctx, peer, body, out);
		break;
	case SESSION_OPCODE_CLIENT_AUTH:
		handle_client_join(ctx, peer, body, out);
		break;
	case SESSION_OPCODE_PROTOCOL_MESSAGE:
		handle_client_session(ctx, peer, body, now_tick, out);
		break;
	case SESSION_OPCODE_CLIENT_GOODBYE:
		handle_client_goodbye(ctx, peer, out);
		break;
	default:
		break;
	}
	return out;
}

std::vector<TickOut> tick_connections(NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick) {
	std::vector<TickOut> out;
	if (!ctx.game_runtime) return out;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		const GameSessionState *gss = ctx.game_runtime->session_state(conn.session_id);
		if (gss == nullptr) continue; // session not created yet (no 0x43 dispatched)
		if (gss->spawned) continue;   // NetSystem owns spawned peers' per-frame 0x0A
		GameServerDispatch disp = ctx.game_runtime->tick_session(conn.session_id, elapsed_ms, now_tick);

		TickOut to;
		to.peer = conn.peer;
		if (!disp.replies.empty()) {
			std::vector<uint8_t> dg = frame_session_replies(conn, disp.replies);
			if (!dg.empty()) to.outbound.push_back(std::move(dg));
		}

		// This loop drives the phase into WorldStreaming + increments entity_batch_count, so it
		// catches the streaming transition on the tick it happens. Re-read post-tick state (gss
		// points at the live session struct, mutated by tick_session). Surface
		// PeerEnteredWorldStreaming once so the owner admits early + streams the joiner's own
		// dcb-bearing 0x0C before the game-start bundle (F3). Same latch as the datagram-driven
		// path, whichever observes batches first wins.
		if (gss->entity_batch_count > 0 && !gss->spawned && conn.self_id_seen &&
				!conn.world_stream_announced) {
			conn.world_stream_announced = true;
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerEnteredWorldStreaming;
			ev.peer = conn.peer;
			ev.pose = pose_from_session(ctx, *gss);
			ev.self_id = conn.connection_id;
			ev.peer_name = conn.player_name;
			to.events.push_back(std::move(ev));
		}

		if (to.outbound.empty() && to.events.empty()) continue;
		out.push_back(std::move(to));
	}
	return out;
}

bool frame_in_match_s2c(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t inner_tag,
                        const std::vector<uint8_t> &inner_body, std::vector<uint8_t> &out_datagram) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->server_scrk.empty()) {
		return false;
	}
	ProtocolMessage msg = make_protocol_message(inner_tag, inner_body);
	out_datagram = frame_session_replies(*conn, std::vector<ProtocolMessage>{std::move(msg)});
	return !out_datagram.empty();
}

bool connection_spawned(const NapiNPServerCtx &ctx, const PeerAddr &peer) {
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) return c.spawned_announced;
	}
	return false;
}

bool bind_connection_player(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t player_slot,
                            uint16_t entity_handle) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->session_id.empty() || !ctx.game_runtime) {
		return false;
	}
	const std::string player_name = !conn->player_name.empty()
			? conn->player_name
			: ctx.game_runtime->config().session.player_name;
	return ctx.game_runtime->bind_session_player(
			conn->session_id, player_name, player_slot, entity_handle);
}

std::size_t connection_count(const NapiNPServerCtx &ctx) {
	return ctx.np_protocol.connection_list.size();
}

} // namespace opennova::np
