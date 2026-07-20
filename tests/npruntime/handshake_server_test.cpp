// Drives a crafted JointOperations joiner through the npruntime server legs
// (opennova::np::handle_server_datagram / frame_in_match_s2c) end-to-end: the handshake (0x41/0x42),
// the SESSION-framed (0x43) reactive §5.1 reply path (dispatch_session_replies — handshake /
// server-info / mission-metadata / loadout / roster / spawn-confirm), and the join-leg validation
// (non-JO / HK / capacity / retransmit).
//
// This is the P2 port of tests/novaworld/host_session_accept_test.cpp, retargeted onto the promoted np
// free functions over a NapiNPServerCtx (connection state on NapiNPConnection; the reactive replies
// produced by server_message_dispatch off ctx.config, P8). The WORLD-driven spawn / F3-ordering
// flow is covered by joiner_connection_test / initial_state_burst_test / two_endpoint_socket_test (which
// wire ctx.world); this World-less test asserts the reactive replies + the handshake legs.
//
// nw_encode_outbound is the client's exact inverse of the server's nw_decode_inbound, so it doubles as
// the joiner-side framer; the server's internally-generated SCRK is recovered from the ServerAuth reply,
// so the encrypted 0x83 replies decode without any test accessor.

#include <npruntime/napi_np_protocol.h>
#include <npruntime/ammo_table_build.h>   // build_ammo_table / resolve_weapon_round_types
#include <npruntime/weapon_table_build.h> // build_weapon_table (the D-NET-141 armory resolve)

#include <def/def.h>
#include <world/world.h>

#include "common/test_paths.h"
#include "host_test_setup.h"

#include <netsim/loopback_channel.h> // LoopbackChannel (run_listen_host_lifecycle's host loopback)

#include <npwire/ingame_decode.h> // WeaponLoadout / decode_weapon_loadout (the 0x5A reply check)
#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

// The host advertises this key in ServerHello.hk; the join leg checks ClientAuth.hk against it, so
// crafted joiners echo it. [orig: NapiNPProtocol_HandleClientJoin @0x62b750 HK gate]
constexpr uint32_t kHostKey = 0x0FE0E112u;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

uint16_t le16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// Craft an inbound NW-UDP datagram (opcode + plaintext body) the way a joiner would:
// nw_encode_outbound is envelope_encode∘nwu_decrypt, the exact inverse of the server's
// nw_decode_inbound (envelope_decode∘nwu_encrypt).
std::vector<uint8_t> craft(uint8_t opcode, std::vector<uint8_t> body) {
	return nw_encode_outbound(opcode, std::move(body));
}

// Craft an inbound 0x43 SESSION datagram carrying `messages`, inner-encrypted with the joiner's SCRK
// (== the server's stored client_scrk).
std::vector<uint8_t> craft_session(std::string_view client_scrk, uint32_t seq,
                                   const std::vector<ProtocolMessage> &messages) {
	ProtocolPacketHeader hdr;
	hdr.session_id = 0; // server reads seq_num only; session_id unchecked on receive
	hdr.seq_num = seq;
	hdr.ack_count = 0;
	hdr.connection_flags = 0;
	std::vector<uint8_t> body;
	encode_protocol_packet_plaintext(hdr, messages, client_scrk, body);
	return craft(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

// Build a crafted 0x42 ClientAuth datagram with the given PN / HK (for the rejection cases).
std::vector<uint8_t> craft_auth(const std::string &pn, uint32_t hk, uint32_t ck, std::string_view scrk) {
	ClientAuth auth;
	auth.pn = pn;
	auth.ci = 1;
	auth.ck = ck;
	auth.hk = hk;
	auth.na = "jop:cus2";
	auth.scrk = scrk;
	return craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
}

// Decode a server 0x83 SESSION reply with the recovered server SCRK.
bool decode_s2c(const std::vector<uint8_t> &datagram, std::string_view server_scrk,
                ProtocolPacketHeader &hdr, std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(datagram.data(), datagram.size(), opcode, body)) return false;
	if (opcode != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE) return false;
	return decode_protocol_packet_plaintext(body.data(), body.size(), server_scrk, hdr, messages);
}

bool reply_has_tag(const std::vector<ProtocolMessage> &msgs, uint8_t tag) {
	for (const auto &m : msgs) if (m.tag == tag) return true;
	return false;
}

// Complete the 0x41/0x42 handshake for `peer` and recover the server SCRK so the test can decode the
// encrypted 0x83 replies.
bool handshake(np::NapiNPServerCtx &ctx, const PeerAddr &peer, std::string_view client_scrk,
               uint32_t client_ck, std::string &out_server_scrk) {
	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "TestJoiner";
	hello.ci = 1;
	auto hdg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = np::handle_server_datagram(ctx, peer, hdg.data(), hdg.size(), 1);
	if (!expect(rh.outbound.size() == 1, "0x41 -> one ServerHello")) return false;

	ClientAuth auth;
	auth.pn = "JointOperations";
	auth.ci = 1;
	auth.ck = client_ck;
	auth.hk = kHostKey;
	auth.na = "jop:cus2";
	auth.scrk = std::string(client_scrk);
	auto adg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto ra = np::handle_server_datagram(ctx, peer, adg.data(), adg.size(), 2);
	if (!expect(ra.outbound.size() >= 1, "0x42 -> ServerAuth + post-handshake")) return false;
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!expect(nw_decode_inbound(ra.outbound[0].data(), ra.outbound[0].size(), op, body) &&
	            op == SESSION_OPCODE_SERVER_AUTH, "0x42 reply decodes to 0x82")) return false;
	ServerAuth sa;
	if (!expect(parse_server_auth(body.data(), body.size(), sa), "0x82 parses")) return false;
	out_server_scrk = sa.scrk;
	return true;
}

// World-less: the reactive §5.1 reply path (dispatch_session_replies). No World wired => no spawn (the
// World-path spawn/F3 flow is covered by joiner_connection_test / two_endpoint_socket_test). Asserts the
// handshake / server-info / mission-metadata / loadout / roster / spawn-confirm reactive replies a retail
// joiner expects.
bool run_reactive_replies() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	const PeerAddr peer{0x0100007Fu, 30000};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB"; // 61
	const uint32_t client_ck = 0xDEADBEEFu;
	std::string server_scrk;
	if (!handshake(ctx, peer, client_scrk, client_ck, server_scrk)) return false;

	uint32_t seq = 1;
	auto send_session = [&](std::vector<ProtocolMessage> msgs, uint32_t now,
	                        std::vector<ProtocolMessage> &out) -> bool {
		auto dg = craft_session(client_scrk, seq++, msgs);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		if (r.outbound.empty()) return false;
		ProtocolPacketHeader hdr;
		return decode_s2c(r.outbound.back(), server_scrk, hdr, out);
	};

	// 0x02 GLB_JOIN -> the post-handshake burst is now emitted at JOIN time (handle_client_join),
	// so C2S 0x02 is a no-op (roster_pushed already set). Verify it doesn't crash or double-emit.
	{
		std::vector<ProtocolMessage> msgs;
		send_session({make_protocol_message(0x02, std::vector<uint8_t>(8, 0))}, 100, msgs);
		// no assertion on reply content — the burst was already sent with the 0x82 ServerAuth
	}
	// 0x33 -> 0x60 server-info chunk. [orig: NapiNPServerMsg_0x033 @0x515230]
	{
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x33, {})}, 110, msgs) &&
		            reply_has_tag(msgs, 0x60), "0x33 -> 0x60 server-info")) return false;
	}
	// 0x37 -> 0x64 mission metadata chunk. [orig: NapiNPServerMsg_0x037 @0x5152E0]
	{
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x37, {})}, 120, msgs) &&
		            reply_has_tag(msgs, 0x64), "0x37 -> 0x64 mission metadata")) return false;
	}
	// 0x2F -> 0x5A weapon loadout, DERIVED from the request [orig: NapiNPServerMsg_0x02F
	// @0x515790 -> Server_SendWeaponSlotListToPlayer @0x502550]: reply set = the request's adm
	// entries sorted ascending (the weapon-slot table walk order), avatarClass = the accepted
	// soldier type, ammo bytes echoed (the client clamps them on apply @0x4295d7), 4th byte 0.
	// Request bytes = the live retail v14 capture's C2S 0x2F (class 2 / soldier 8 / entries
	// {21,3,83,76,77,78,2}, ammo 255/255, var 255).
	{
		std::vector<uint8_t> req = {0x02, 0x08, 0xC3, 0x00, 0x00, 0x00};
		for (uint8_t adm : {uint8_t(21), uint8_t(3), uint8_t(83), uint8_t(76), uint8_t(77),
		                    uint8_t(78), uint8_t(2)}) {
			req.push_back(adm); req.push_back(0xFF); req.push_back(0xFF); req.push_back(0xFF);
		}
		req.push_back(0xFF); // adm-index terminator
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x2F, req)}, 130, msgs) &&
		            reply_has_tag(msgs, 0x5A), "0x2F -> 0x5A loadout")) return false;
		for (const ProtocolMessage &m : msgs) {
			if (m.tag != 0x5A) continue;
			WeaponLoadout lo;
			if (!expect(decode_weapon_loadout(m.payload.data(), m.payload.size(), lo),
			            "0x5A reply decodes")) return false;
			if (!expect(lo.avatar_class == 8, "avatarClass = accepted soldier type")) return false;
			const uint8_t want[7] = {2, 3, 21, 76, 77, 78, 83};
			if (!expect(lo.slots.size() == 7, "one reply slot per accepted request entry"))
				return false;
			for (size_t i = 0; i < 7; ++i) {
				if (!expect(lo.slots[i].type_id == want[i],
				            "slots sorted ascending by adm index (the golden order)")) return false;
				if (!expect(lo.slots[i].ammo_primary == 0xFF && lo.slots[i].ammo_alt == 0,
				            "ammo bytes echoed; restriction byte 0")) return false;
			}
		}
	}
	// 0x0A -> 0x19 ack only (golden f161-162: C 0x0A empty -> S 0x19 tick). The prior emit_roster
	// (0x46/0x16/0x19/0x1A) sent an unsolicited 0x1A + 0x16 that triggered 0x22 re-request storms.
	// The 0x16 roster pushes are proactive (post-handshake + periodic), not reactive to 0x0A.
	{
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x0A, {})}, 140, msgs) &&
		            reply_has_tag(msgs, 0x19),
		            "0x0A -> 0x19 ack")) return false;
	}
	// 0x29 team/spawn ack -> NO 0x51 on a plain join. [orig: NapiNPServerMsg_0x029 @0x514F10]
	// only replies 0x51 for a pending g_team_change_entity_list entry (team-change flow,
	// unmodeled); the golden session's deploy-time C 0x29 draws no 0x51 anywhere. The old
	// unconditional zero-id 0x51 made the client REBIND its own player's CharacterEntity
	// (@0x431BB0 field-parses it) onto a vehicle archetype — the DBuggy1 shadow (D-NET-148).
	{
		std::vector<ProtocolMessage> msgs;
		const bool got = send_session({make_protocol_message(0x29, {0x00, 0x00})}, 150, msgs);
		if (!expect(!got || !reply_has_tag(msgs, 0x51),
		            "0x29 draws NO 0x51 on a plain join (D-NET-148)")) return false;
	}
	// 0x2C RTT probe -> 0x57 pong when echo_flag != 0 (the host bounces [u32 ts][u8 0]); echo_flag == 0
	// is the client's return leg (server-internal RTT/kick, no reply). [orig: NapiNPServerMsg_HandlePingResponse @0x515070]
	{
		std::vector<ProtocolMessage> msgs;
		// timestamp 0x11223344 LE, echo_flag = 1.
		if (!expect(send_session({make_protocol_message(0x2C, {0x44, 0x33, 0x22, 0x11, 0x01})}, 170, msgs) &&
		            reply_has_tag(msgs, 0x57), "0x2C echo -> 0x57 pong")) return false;
		bool body_ok = false;
		for (const auto &m : msgs)
			if (m.tag == 0x57)
				body_ok = m.payload == std::vector<uint8_t>{0x44, 0x33, 0x22, 0x11, 0x00};
		if (!expect(body_ok, "0x57 pong body = echoed timestamp + 0 byte")) return false;
		// echo_flag = 0 -> no 0x57 reply (RTT compute only).
		std::vector<ProtocolMessage> msgs0;
		const bool got0 = send_session({make_protocol_message(0x2C, {0x44, 0x33, 0x22, 0x11, 0x00})}, 180, msgs0);
		if (!expect(!got0 || !reply_has_tag(msgs0, 0x57),
		            "0x2C echo_flag=0 -> no 0x57 (RTT compute only)")) return false;
	}
	return true;
}

// A plain-join C2S 0x29 draws no S2C 0x51 even with a bound player: the original replies 0x51
// only for a pending g_team_change_entity_list entry [orig: NapiNPServerMsg_0x029 @0x514F10
// @0x514f7c], and the client field-parses 0x51 (@0x431BB0 CharacterEntity rebind) — an
// invented zero-id confirm re-bound the joiner to a vehicle archetype (D-NET-148).
bool run_plain_join_tag29_draws_no_tag51() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	const PeerAddr peer{0x0100007Fu, 30600};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t client_ck = 0xBEEFCAFEu;
	std::string server_scrk;
	if (!handshake(ctx, peer, client_scrk, client_ck, server_scrk)) return false;

	uint32_t seq = 1;
	auto mission_dg = craft_session(client_scrk, seq++, {make_protocol_message(0x37, {})});
	np::handle_server_datagram(ctx, peer, mission_dg.data(), mission_dg.size(), 100);
	if (!expect(np::bind_connection_player(ctx, peer, 1, 0x0005),
	            "server binds the connection to its allocated player entity handle")) return false;

	auto spawn_req = craft_session(client_scrk, seq++, {make_protocol_message(0x29, {0x00, 0x00})});
	auto spawn_r = np::handle_server_datagram(ctx, peer, spawn_req.data(), spawn_req.size(), 200);
	for (const auto &dg : spawn_r.outbound) {
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> msgs;
		if (!decode_s2c(dg, server_scrk, hdr, msgs)) continue;
		for (const ProtocolMessage &m : msgs) {
			if (!expect(m.tag != 0x51,
			            "plain-join 0x29 never draws 0x51 (D-NET-148)")) return false;
		}
	}
	return true;
}

// ClientGoodbye tears the player down [orig: Server_HandlePlayerDisconnect @0x51B5C0]: the owned
// world entity despawns — a leaked body kept streaming and re-entered every future joiner's 0x0C
// batch (the retail-join v23 ghost players, D-NET-149) — and the node erases so the roster slot
// frees. (The 0x46 removal fan to remaining in-match peers rides their transports; covered by the
// staging call, no in-match second peer modeled here.)
bool run_goodbye_despawns_player_entity() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	world::World w;
	w.registry.configure_pool(0, 8);
	const world::EntityHandle h = w.registry.spawn(0, world::Entity{});
	if (!expect(h.valid() && w.registry.get(h) != nullptr, "test entity spawns")) return false;
	ctx.world = &w;

	const PeerAddr peer{0x0100007Fu, 30700};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	std::string server_scrk;
	if (!handshake(ctx, peer, client_scrk, 0xC0FFEE01u, server_scrk)) return false;
	if (!expect(np::bind_connection_player(ctx, peer, 1, h.packed),
	            "connection binds the live world entity")) return false;

	auto bye = craft(SESSION_OPCODE_CLIENT_GOODBYE, {});
	np::handle_server_datagram(ctx, peer, bye.data(), bye.size(), 400);

	if (!expect(w.registry.get(h) == nullptr,
	            "goodbye despawns the owned world entity (D-NET-149)")) return false;
	bool node_gone = true;
	for (const auto &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) node_gone = false;
	}
	if (!expect(node_gone, "goodbye erases the connection node")) return false;
	return true;
}

bool run_non_jo_peer_is_ignored() {
	// The join legs validate the JO identity + HK echo (the @0x62b750 gate): a lobby (non-JO) PN, a
	// non-JO 0x42, and a wrong-HK 0x42 must all be dropped with no reply and no connection.
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 31000};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

	// (a) non-JO ClientHello -> no ServerHello, no node.
	{
		ClientHello hello;
		hello.pn = "NOVAWORLDUDP";
		hello.ci = 1;
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
		if (!expect(r.outbound.empty(), "lobby PN produces no JO ServerHello")) return false;
		if (!expect(np::connection_count(ctx) == 0, "lobby PN registers no JO connection")) return false;
	}
	// (b) non-JO ClientAuth -> no ServerAuth, no node (the join re-validates PN).
	{
		auto dg = craft_auth("NOVAWORLDUDP", kHostKey, 0xDEADBEEFu, scrk);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 2);
		if (!expect(r.outbound.empty(), "non-JO 0x42 produces no ServerAuth")) return false;
		if (!expect(np::connection_count(ctx) == 0, "non-JO 0x42 registers no connection")) return false;
	}
	// (c) JO ClientAuth with the WRONG host key -> dropped (HK echo gate).
	{
		auto dg = craft_auth("JointOperations", kHostKey ^ 0x1u, 0xDEADBEEFu, scrk);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 3);
		if (!expect(r.outbound.empty(), "wrong-HK 0x42 produces no ServerAuth")) return false;
		if (!expect(np::connection_count(ctx) == 0, "wrong-HK 0x42 registers no connection")) return false;
	}
	return true;
}

// A live handshake against a host that was NOT brought up (host_running == 0) is rejected.
bool run_handshake_rejected_when_host_down() {
	np::NapiNPServerCtx ctx;
	np::configure_session_runtime(ctx); // config only — NO create_session, so host_running stays 0
	if (!expect(ctx.np_protocol.host_running == 0, "host not running before create_session")) return false;
	const PeerAddr peer{0x0100007Fu, 31100};

	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "EarlyBird";
	hello.ci = 1;
	auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	if (!expect(r.outbound.empty(), "0x41 to a down host produces no ServerHello")) return false;
	if (!expect(np::connection_count(ctx) == 0, "0x41 to a down host registers no connection")) return false;
	return true;
}

// The full P0->P1->P2 listen-host bring-up preserves the host's own type-2 loopback through
// configure_session_runtime, and a remote joiner is added alongside it (not in place of it).
bool run_listen_host_lifecycle() {
	netsim::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::GameConfig settings;
	settings.max_players = 8; // co-op listen host: host loopback + up to 7 joiners (capacity gate)
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        kHostKey, &loopback, settings);

	if (!expect(ctx.is_in_session == 1, "listen host is in session")) return false;
	if (!expect(ctx.is_authority == 1 && ctx.is_mp_session_peer == 1, "HostClient = host + client")) return false;
	if (!expect(ctx.np_protocol.host_running == 1, "listen host is running")) return false;
	if (!expect(ctx.np_protocol.host_key == kHostKey, "host key seeded")) return false;
	if (!expect(np::connection_count(ctx) == 1, "loopback preserved through configure_session_runtime")) return false;
	if (!expect(ctx.np_protocol.connection_list[0].type == 2, "preserved node is the type-2 loopback")) return false;

	const PeerAddr peer{0x0100007Fu, 31200};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "RemoteJoiner";
	hello.ci = 1;
	auto h = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = np::handle_server_datagram(ctx, peer, h.data(), h.size(), 1);
	if (!expect(rh.outbound.size() == 1, "remote 0x41 -> one ServerHello")) return false;
	auto a = craft_auth("JointOperations", kHostKey, 0xDEADBEEFu, scrk);
	auto ra = np::handle_server_datagram(ctx, peer, a.data(), a.size(), 2);
	if (!expect(ra.outbound.size() >= 1, "remote 0x42 -> ServerAuth + post-handshake")) return false;

	if (!expect(np::connection_count(ctx) == 2, "joiner added alongside the loopback")) return false;
	int loopbacks = 0, remotes = 0;
	for (const auto &c : ctx.np_protocol.connection_list) {
		if (c.type == 2) ++loopbacks;
		else if (c.type == 1) ++remotes;
	}
	if (!expect(loopbacks == 1 && remotes == 1, "exactly one loopback + one remote joiner")) return false;
	return true;
}

// A retransmitted 0x42 ClientAuth (normal lossy UDP) for an already-joined connection must RE-SEND the
// cached ServerAuth, not re-mint the server SCRK/SK. [orig: HandleClientJoin @0x62b750 — conn_state==1
// && CI && CK match -> NapiNPConnection_SendSessionInit @0x620ef0]
bool run_retransmit_0x42_keeps_keys() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 30800};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t ck = 0x12345678u;

	auto first_auth = [&](uint32_t now, ServerAuth &out_sa) -> bool {
		auto a = craft_auth("JointOperations", kHostKey, ck, scrk); // craft_auth uses CI = 1
		auto r = np::handle_server_datagram(ctx, peer, a.data(), a.size(), now);
		if (!expect(r.outbound.size() >= 1, "0x42 -> ServerAuth + post-handshake")) return false;
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(r.outbound[0].data(), r.outbound[0].size(), op, body) &&
		            op == SESSION_OPCODE_SERVER_AUTH, "0x42 reply decodes to 0x82")) return false;
		return expect(parse_server_auth(body.data(), body.size(), out_sa), "0x82 parses");
	};

	ServerAuth sa1, sa2;
	if (!first_auth(1, sa1)) return false;
	if (!first_auth(2, sa2)) return false; // identical retransmit (same CI + CK)

	if (!expect(sa2.sk == sa1.sk, "retransmit re-sends the SAME ServerAuth SK (no re-mint)")) return false;
	if (!expect(sa2.scrk == sa1.scrk, "retransmit re-sends the SAME ServerAuth SCRK (no re-mint)")) return false;
	if (!expect(sa2.mi == sa1.mi, "retransmit re-sends the SAME MI (connection_id)")) return false;
	if (!expect(np::connection_count(ctx) == 1, "retransmit does not create a second node")) return false;
	return true;
}

// The join leg enforces capacity. A dedicated host with max_players == 2 admits two joiners; the third
// 0x42 is rejected. [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0 — current_player_count >= max]
bool run_capacity_rejects_when_full() {
	np::NapiNPServerCtx ctx;
	np::GameConfig settings;
	settings.max_players = 2; // dedicated host: two joiner slots, no host loopback
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey,
	                        nullptr, settings);
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

	auto join = [&](const PeerAddr &p, uint32_t ck, uint32_t now) {
		auto a = craft_auth("JointOperations", kHostKey, ck, scrk);
		return np::handle_server_datagram(ctx, p, a.data(), a.size(), now);
	};

	if (!expect(join(PeerAddr{0x0100007Fu, 32001}, 0x1111u, 1).outbound.size() >= 1,
	            "joiner 1 admitted (ServerAuth + post-handshake)")) return false;
	if (!expect(join(PeerAddr{0x0100007Fu, 32002}, 0x2222u, 2).outbound.size() >= 1,
	            "joiner 2 admitted (ServerAuth + post-handshake)")) return false;
	if (!expect(np::connection_count(ctx) == 2, "two joiners fill the server")) return false;

	auto r3 = join(PeerAddr{0x0100007Fu, 32003}, 0x3333u, 3);
	if (!expect(r3.outbound.empty(), "over-capacity 0x42 produces no ServerAuth")) return false;
	if (!expect(np::connection_count(ctx) == 2, "over-capacity join creates no node")) return false;
	return true;
}

// Armory-fed loadout resolve (D-NET-141): with world.weapons built from the committed fixture,
// the 0x5A reply resolves REAL ammo counts through the witnessed rules instead of echoing —
// filters drop unfiltered (emplaced) request entries, counts come from startrounds/clipsize
// (min(req,maxclips) on an explicit request), the alt byte carries the first different-ammoclass
// sub-variant, and the reply sorts by weapon-slot combo (category*65+rank).
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 / WeaponSlot_GetTotalClips @0x5425F0]
// NOTE fixture truth ≠ live-install truth: a real JO:CA root resolves a larger weapon.def whose
// indices reproduce the golden bytes end-to-end — that equality is the live v16 wire gate.
bool run_loadout_resolve_with_armory() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	// The armory: fixtures/def/weapon.def -> the witnessed table (null@0 + file order).
	const char *repo_root = test_paths_repo_root(__FILE__);
	char def_path[4096];
	std::snprintf(def_path, sizeof(def_path), "%s/fixtures/def/weapon.def", repo_root);
	DefWeaponsFile wf{};
	if (!expect(def_parse_weapons(def_path, &wf) == 0, "fixture weapon.def parses")) return false;
	world::World world;
	world.weapons = np::build_weapon_table(wf);
	def_free_weapons(&wf);
	char ammo_path[4096];
	std::snprintf(ammo_path, sizeof(ammo_path), "%s/fixtures/def/ammo.def", repo_root);
	DefAmmoFile af{};
	if (!expect(def_parse_ammo(ammo_path, &af) == 0, "fixture ammo.def parses")) return false;
	world.ammo = np::build_ammo_table(af);
	def_free_ammo(&af);
	np::resolve_weapon_round_types(world.weapons, world.ammo);
	ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 30100};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	std::string server_scrk;
	if (!handshake(ctx, peer, client_scrk, 0xDEADBEE2u, server_scrk)) return false;

	uint32_t seq = 1;
	auto send_session = [&](std::vector<ProtocolMessage> msgs, uint32_t now,
	                        std::vector<ProtocolMessage> &out) -> bool {
		auto dg = craft_session(client_scrk, seq++, msgs);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		if (r.outbound.empty()) return false;
		ProtocolPacketHeader hdr;
		return decode_s2c(r.outbound.back(), server_scrk, hdr, out);
	};

	auto loadout_reply = [&](const std::vector<uint8_t> &req, uint32_t now,
	                         WeaponLoadout &lo) -> bool {
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x2F, req)}, now, msgs) &&
		            reply_has_tag(msgs, 0x5A), "0x2F -> 0x5A loadout"))
			return false;
		for (const ProtocolMessage &m : msgs) {
			if (m.tag != 0x5A) continue;
			return expect(decode_weapon_loadout(m.payload.data(), m.payload.size(), lo),
			              "0x5A reply decodes");
		}
		return false;
	};
	auto malformed_loadout_rejected = [&](const std::vector<uint8_t> &req, uint32_t now,
	                                      const char *label) -> bool {
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x2F, req)});
		auto response = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		bool saw_5a = false;
		for (const std::vector<uint8_t> &outbound : response.outbound) {
			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> messages;
			if (!decode_s2c(outbound, server_scrk, hdr, messages)) continue;
			if (reply_has_tag(messages, 0x5A)) saw_5a = true;
		}
		if (!expect(!saw_5a, label)) return false;
		const np::NapiNPConnection *connection = nullptr;
		for (const np::NapiNPConnection &candidate : ctx.np_protocol.connection_list)
			if (candidate.peer == peer) connection = &candidate;
		return expect(connection != nullptr && !connection->reply.loadout_synced &&
		                      !connection->burst.loadout_received &&
		                      connection->reply.last_loadout_reply.empty(),
		              "malformed 0x2F leaves the loadout gate and prior grant untouched");
	};

	// Retail's 0x2F reader stops the entry loop ONLY on the 0xFF terminator and
	// never checks for trailing bytes after it (@0x515a99), so trailing bytes are
	// accepted; an unterminated/truncated list is the retail zero-fill infinite
	// loop (@0x5159c0) and is rejected here as the crash-safe divergence.
	if (!malformed_loadout_rejected(
			{0x01, 0x08, 0, 0, 0, 0, 9, 0xFF, 0xFF, 1}, 120,
			"0x2F without the 0xFF terminator produces no 0x5A"))
		return false;
	if (!malformed_loadout_rejected(
			{0x01, 0x08, 0, 0, 0, 0, 9, 4, 0xFF}, 121,
			"0x2F with a truncated entry produces no 0x5A"))
		return false;
	{
		WeaponLoadout trailing_lo;
		if (!expect(loadout_reply({0x01, 0x08, 0, 0, 0, 0, 0xFF, 0x00}, 122, trailing_lo),
		            "0x2F with trailing bytes after its terminator is accepted like retail"))
			return false;
	}

	// The live retail v14 request (class 2 red / soldier 8 rifleman, default 0xFF ammo). Fixture
	// truth: {2 KNIFE2, 3 colt45, 21 AK47M203AUTO} pass the masks; {76,77,78,83} land on
	// unfiltered emplaced/vehicle entries in the 94-weapon fixture and are dropped.
	{
		std::vector<uint8_t> req = {0x02, 0x08, 0xC3, 0x00, 0x00, 0x00};
		for (uint8_t adm : {uint8_t(21), uint8_t(3), uint8_t(83), uint8_t(76), uint8_t(77),
		                    uint8_t(78), uint8_t(2)}) {
			req.push_back(adm); req.push_back(0xFF); req.push_back(0xFF); req.push_back(0xFF);
		}
		req.push_back(0xFF);
		WeaponLoadout lo;
		if (!loadout_reply(req, 130, lo)) return false;
		if (!expect(lo.avatar_class == 8, "avatarClass = accepted soldier type")) return false;
		if (!expect(lo.slots.size() == 3, "mask filter drops the unfiltered emplaced entries"))
			return false;
		// Sorted by slot combo: KNIFE2 (1*65+1=66), colt45 (2*65+0=130), AK47M203AUTO (3*65+12=207).
		if (!expect(lo.slots[0].type_id == 2 && lo.slots[0].ammo_primary == 0xFF &&
		                    lo.slots[0].ammo_secondary == 0xFF,
		            "KNIFE2: clipsize -1 -> the 0xFF no-clip sentinel")) return false;
		if (!expect(lo.slots[1].type_id == 3 && lo.slots[1].ammo_primary == 5 &&
		                    lo.slots[1].ammo_secondary == 0xFF,
		            "colt45: startrounds 35 / clipsize 7 -> 5 clips")) return false;
		if (!expect(lo.slots[2].type_id == 21 && lo.slots[2].ammo_primary == 10 &&
		                    lo.slots[2].ammo_secondary == 6,
		            "AK47M203AUTO: 300/30 -> 10; alt = the different-class M203HE 6/1 -> 6"))
			return false;
		for (const WeaponLoadoutSlot &s : lo.slots)
			if (!expect(s.ammo_alt == 0, "restriction byte 0")) return false;
	}

	// Soldier-type mask in isolation: a BLUE sniper (class 1 / soldier 6) requests M4AUTO + the
	// blue KNIFE — the team mask passes both, the charfilter drops only the M4AUTO
	// (rifleman|medic|engineer band). The all-class KNIFE (idx 1) survives alone.
	{
		std::vector<uint8_t> req = {0x01, 0x06, 0xC3, 0x00, 0x00, 0x00,
		                            9, 0xFF, 0xFF, 0xFF, 1, 0xFF, 0xFF, 0xFF, 0xFF};
		WeaponLoadout lo;
		if (!loadout_reply(req, 140, lo)) return false;
		if (!expect(lo.avatar_class == 6, "soldier 6 accepted verbatim")) return false;
		if (!expect(lo.slots.size() == 1 && lo.slots[0].type_id == 1,
		            "charfilter drops the rifleman-band M4AUTO for a sniper")) return false;
	}

	// Explicit requested count: blue rifleman asks 4 mags of M4AUTO -> min(4, maxclips 10) = 4.
	{
		std::vector<uint8_t> req = {0x01, 0x08, 0xC3, 0x00, 0x00, 0x00,
		                            9, 4, 0xFF, 2, 0xFF};
		WeaponLoadout lo;
		if (!loadout_reply(req, 150, lo)) return false;
		if (!expect(lo.slots.size() == 1 && lo.slots[0].type_id == 9 &&
		                    lo.slots[0].ammo_primary == 4 &&
		                    lo.slots[0].ammo_secondary == 0xFF &&
		                    lo.slots[0].ammo_alt == 2,
		            "explicit request -> clips plus the accepted damage-class byte")) return false;
	}

	const int m4_auto = world.weapons.index_of("WPN_M4AUTO");
	const int m4_m203_auto = world.weapons.index_of("WPN_M4M203AUTO");
	if (!expect(m4_auto > 0 && m4_m203_auto > 0,
	            "shared-ammo loadout fixtures resolve")) return false;
	const int16_t m4_ammo = world.weapons.entries[static_cast<size_t>(m4_auto)].ammo_index;
	if (!expect(m4_ammo >= 0 &&
	                    world.weapons.entries[static_cast<size_t>(m4_m203_auto)].ammo_index == m4_ammo,
	            "M4AUTO and M4M203AUTO share one resolved AmmoDef index")) return false;

	// player+89688 is indexed by AmmoDef, not by granted weapon. The later accepted M4M203AUTO
	// therefore overwrites the M4AUTO's class, and BOTH serialized slots read back the final 2.
	{
		std::vector<uint8_t> req = {
				0x01, 0x08, 0, 0, 0, 0,
				static_cast<uint8_t>(m4_auto), 2, 0xFF, 1,
				static_cast<uint8_t>(m4_m203_auto), 3, 0xFF, 2,
				0xFF};
		WeaponLoadout lo;
		if (!loadout_reply(req, 160, lo)) return false;
		if (!expect(lo.slots.size() == 2, "two different shared-ammo slots are granted"))
			return false;
		for (const WeaponLoadoutSlot &slot : lo.slots)
			if (!expect(slot.ammo_alt == 2,
			            "all shared-ammo slots serialize the final per-ammo damage class"))
				return false;
	}

	// Repeating the same category*65+rank does not create a second granted slot: the load
	// table entry is replaced. Its later ammo count and normalized damage class are retained.
	{
		std::vector<uint8_t> req = {
				0x01, 0x08, 0, 0, 0, 0,
				static_cast<uint8_t>(m4_auto), 2, 0xFF, 2,
				static_cast<uint8_t>(m4_auto), 4, 0xFF, 1,
				0xFF};
		WeaponLoadout lo;
		if (!loadout_reply(req, 170, lo)) return false;
		if (!expect(lo.slots.size() == 1 && lo.slots[0].type_id == m4_auto &&
		                    lo.slots[0].ammo_primary == 4 && lo.slots[0].ammo_alt == 1,
		            "duplicate weapon slot serializes once with its last accepted values"))
			return false;
	}
	return true;
}

// The 0x42 CU chunks carry the joiner's character/profile vars — the per-side character selection
// (CI0/CI1 per-side char ids, TR requested side, CTA/CTB classes, VCA/VCB avatars). Parsed with the
// witnessed LoadFromConnTags semantics: type-2 chunks only, case-insensitive names, atol values
// (u16 truncation for CI, TR clamped to {0,1,0xFF}). [orig: NapiNPProtocol_HandleClientJoin
// @0x62b750 CU loop -> NapiNetConfig_LoadFromConnTags @0x4c7260; wire: retail-ashi5a f=199140;
// D-NET-146]
bool run_character_join_vars_parsed() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 30900};

	ClientAuth auth;
	auth.pn = "JointOperations";
	auth.ci = 1;
	auth.ck = 0x0BADF00Du;
	auth.hk = kHostKey;
	auth.na = "jop:cus2";
	auth.scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	// The golden retail set, with wrinkles the parser must honor: CI0 as the full profile u32
	// ("8126976" = 0x7C0200 — only the low u16 lands, the LoadFromConnTags WORD store), a
	// lower-case tag name (Napi_StrCaseEqual is case-insensitive), TR=-1 (valid "auto"), and a
	// type-1 chunk that must be IGNORED (only type-2 chunks are tag-list vars).
	auth.cu.push_back(make_client_cu_chunk(2, "CI0", "8126976"));  // -> 0x0200
	auth.cu.push_back(make_client_cu_chunk(2, "CI1", "33287"));    // -> 0x8207
	auth.cu.push_back(make_client_cu_chunk(2, "TR", "-1"));        // -> 0xFF (auto)
	auth.cu.push_back(make_client_cu_chunk(2, "cta", "8"));        // case-insensitive
	auth.cu.push_back(make_client_cu_chunk(2, "CTB", "5"));
	auth.cu.push_back(make_client_cu_chunk(2, "VCA", "1"));
	auth.cu.push_back(make_client_cu_chunk(2, "VCB", "4"));
	auth.cu.push_back(make_client_cu_chunk(1, "CI0", "9999"));     // type 1: NOT a tag var
	auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	if (!expect(r.outbound.size() >= 1, "0x42 with CU vars admitted")) return false;

	const np::NapiNPConnection *conn = nullptr;
	for (const auto &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) conn = &c;
	}
	if (!expect(conn != nullptr, "joiner node exists")) return false;
	if (!expect(conn->char_vars.char_id[0] == 0x0200, "CI0 u16-truncated (8126976 -> 0x0200)")) return false;
	if (!expect(conn->char_vars.char_id[1] == 0x8207, "CI1 parsed (33287 = 0x8207)")) return false;
	if (!expect(conn->char_vars.team_request == 0xFF, "TR=-1 kept as 0xFF (auto)")) return false;
	if (!expect(conn->char_vars.char_class[0] == 8, "lower-case 'cta' matched (case-insensitive)")) return false;
	if (!expect(conn->char_vars.char_class[1] == 5, "CTB parsed")) return false;
	if (!expect(conn->char_vars.avatar[0] == 1 && conn->char_vars.avatar[1] == 4,
	            "VCA/VCB avatar bytes parsed (golden 1/4)")) return false;

	// TR out-of-range clamps to 0xFF [orig: @0x4c752f tr != -1 && (u8)tr >= 2 -> -1].
	np::NapiNPServerCtx ctx2;
	np::test::bring_up_host(ctx2, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer2{0x0100007Fu, 30901};
	ClientAuth auth2 = auth;
	auth2.cu.clear();
	auth2.cu.push_back(make_client_cu_chunk(2, "TR", "7"));
	auto dg2 = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth2));
	np::handle_server_datagram(ctx2, peer2, dg2.data(), dg2.size(), 1);
	for (const auto &c : ctx2.np_protocol.connection_list) {
		if (c.peer == peer2 &&
		    !expect(c.char_vars.team_request == 0xFF, "TR=7 clamps to 0xFF")) return false;
	}
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = run_reactive_replies() && ok;
	ok = run_loadout_resolve_with_armory() && ok;
	ok = run_plain_join_tag29_draws_no_tag51() && ok;
	ok = run_goodbye_despawns_player_entity() && ok;
	ok = run_non_jo_peer_is_ignored() && ok;
	ok = run_handshake_rejected_when_host_down() && ok;
	ok = run_listen_host_lifecycle() && ok;
	ok = run_retransmit_0x42_keeps_keys() && ok;
	ok = run_capacity_rejects_when_full() && ok;
	ok = run_character_join_vars_parsed() && ok;
	return ok ? 0 : 1;
}
