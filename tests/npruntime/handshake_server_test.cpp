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
#include <npruntime/lan_discovery.h>
#include <npruntime/server_spawn.h>
#include <npruntime/weapon_table_build.h> // build_weapon_table (the D-NET-141 armory resolve)

#include <def/def.h>
#include <world/ai.h>
#include <world/vehicle_attach.h>
#include <world/world.h>

#include "common/test_paths.h"
#include "host_test_setup.h"

#include <netsim/loopback_channel.h> // LoopbackChannel (run_listen_host_lifecycle's host loopback)

#include <npwire/ingame_decode.h> // WeaponLoadout / decode_weapon_loadout (the 0x5A reply check)
#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
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

bool mount_on_test_emplacement(
		world::World &world, world::EntityHandle player,
		world::EntityHandle &emplacement_out) {
	world::Entity emplacement;
	emplacement.kind = world::EntityKind::Item;
	world::Seat gunner;
	gunner.type = world::SeatType::Gunner;
	gunner.bone_index = 6;
	emplacement.seats.push_back(gunner);
	emplacement_out = world.registry.spawn(1, emplacement);
	return emplacement_out.valid() &&
	       world::entity_process_vehicle_attach(
					world, player, emplacement_out, gunner.bone_index);
}

bool test_emplacement_is_owned_by(
		const world::World &world, world::EntityHandle emplacement,
		world::EntityHandle player) {
	const world::Entity *live = world.registry.get(emplacement);
	return live != nullptr && live->seats.size() == 1 &&
	       live->seats.front().occupant == player &&
	       live->primary_weapon_owner == player &&
	       live->primary_occupant == player;
}

bool test_emplacement_is_released(
		const world::World &world, world::EntityHandle emplacement) {
	const world::Entity *live = world.registry.get(emplacement);
	return live != nullptr && live->seats.size() == 1 &&
	       !live->seats.front().occupant.valid() &&
	       !live->primary_weapon_owner.valid() &&
	       !live->primary_occupant.valid();
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

void add_retail_game_environment(ClientAuth &auth) {
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
}

ClientAuth make_valid_client_auth(uint32_t ci, uint32_t ck, uint32_t hk,
                                  std::string_view name, std::string_view scrk) {
	ClientAuth auth = make_jointoperations_client_auth(ci, ck, hk, name, scrk);
	add_retail_game_environment(auth);
	return auth;
}

std::vector<uint8_t> retail_join_request(std::string_view expansion) {
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
	append_string_tlv("VERSIONCRCSTRING", "0");
	return body;
}

// Craft an inbound 0x43 SESSION datagram carrying `messages`, inner-encrypted with the joiner's SCRK
// (== the server's stored client_scrk).
std::vector<uint8_t> craft_session(std::string_view client_scrk, uint32_t server_sk,
                                   uint32_t seq,
                                   const std::vector<ProtocolMessage> &messages) {
	ProtocolPacketHeader hdr;
	hdr.session_id = server_sk; // receiver-local key advertised by ServerAuth
	hdr.seq_num = seq;
	hdr.ack_count = 0;
	hdr.connection_flags = 0;
	std::vector<uint8_t> body;
	encode_protocol_packet_plaintext(hdr, messages, client_scrk, body);
	return craft(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

// Build a crafted 0x42 ClientAuth datagram with the given PN / HK (for the rejection cases).
std::vector<uint8_t> craft_auth(const std::string &pn, uint32_t hk, uint32_t ck, std::string_view scrk) {
	ClientAuth auth = make_valid_client_auth(1, ck, hk, "TestJoiner", scrk);
	auth.pn = pn;
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
               uint32_t client_ck, std::string &out_server_scrk,
               uint32_t *out_server_sk = nullptr,
               uint32_t *out_next_client_seq = nullptr,
               bool complete_game_admission = true,
               std::vector<ProtocolMessage> *out_post_handshake = nullptr) {
	const std::size_t connections_before_hello = np::connection_count(ctx);
	ClientHello hello = make_jointoperations_client_hello(1);
	hello.co = "TestJoiner";
	auto hdg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = np::handle_server_datagram(ctx, peer, hdg.data(), hdg.size(), 1);
	if (!expect(rh.outbound.size() == 1, "0x41 -> one ServerHello")) return false;
	if (!expect(rh.events.empty(), "0x41 is stateless and emits no peer event")) return false;
	if (!expect(np::connection_count(ctx) == connections_before_hello,
	            "0x41 is stateless and creates no connection")) return false;

	ClientAuth auth = make_valid_client_auth(
			1, client_ck, kHostKey, "TestJoiner", client_scrk);
	auto adg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto ra = np::handle_server_datagram(ctx, peer, adg.data(), adg.size(), 2);
	if (!expect(ra.outbound.size() >= 1, "0x42 -> ServerAuth + initial settings")) return false;
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!expect(nw_decode_inbound(ra.outbound[0].data(), ra.outbound[0].size(), op, body) &&
	            op == SESSION_OPCODE_SERVER_AUTH, "0x42 reply decodes to 0x82")) return false;
	ServerAuth sa;
	if (!expect(parse_server_auth(body.data(), body.size(), sa), "0x82 parses")) return false;
	out_server_scrk = sa.scrk;
	if (out_server_sk != nullptr) *out_server_sk = sa.sk;
	if (!complete_game_admission) {
		if (out_next_client_seq != nullptr) *out_next_client_seq = 1;
		return true;
	}

	uint32_t sequence = 1;
	auto join_dg = craft_session(
			client_scrk, sa.sk, sequence++,
			{make_protocol_message(
					0x00, retail_join_request(ctx.config.expansion))});
	auto join_result = np::handle_server_datagram(
			ctx, peer, join_dg.data(), join_dg.size(), 3);
	ProtocolPacketHeader session_header;
	std::vector<ProtocolMessage> messages;
	if (!expect(
				join_result.outbound.size() == 1 &&
						decode_s2c(
								join_result.outbound.front(), out_server_scrk,
								session_header, messages) &&
						messages.size() == 1 && messages.front().tag == 0x00,
				"handshake helper completes C2S 0x00 JOIN")) {
		return false;
	}

	auto form_dg = craft_session(
			client_scrk, sa.sk, sequence++,
			{make_protocol_message(0x01, {0x00})});
	auto form_result = np::handle_server_datagram(
			ctx, peer, form_dg.data(), form_dg.size(), 4);
	messages.clear();
	if (!expect(
				form_result.outbound.size() == 1 &&
						decode_s2c(
								form_result.outbound.front(), out_server_scrk,
								session_header, messages) &&
						messages.size() == 1 && messages.front().tag == 0x02 &&
						messages.front().payload.size() == 512,
				"handshake helper completes C2S 0x01 form post")) {
		return false;
	}

	std::vector<uint8_t> echo(
			messages.front().payload.begin(),
			messages.front().payload.begin() + 256);
	std::fill(echo.begin() + 8, echo.begin() + 12, 0);
	auto echo_dg = craft_session(
			client_scrk, sa.sk, sequence++,
			{make_protocol_message(0x02, std::move(echo))});
	auto echo_result = np::handle_server_datagram(
			ctx, peer, echo_dg.data(), echo_dg.size(), 5);
	if (!expect(
				!echo_result.outbound.empty() &&
						np::connection_count(ctx) ==
								connections_before_hello + 1,
				"handshake helper completes C2S 0x02 challenge echo")) {
		return false;
	}
	if (out_post_handshake != nullptr) {
		ProtocolPacketHeader post_header;
		if (!expect(
					decode_s2c(
							echo_result.outbound.front(), out_server_scrk,
							post_header, *out_post_handshake),
					"handshake helper decodes the post-handshake burst")) {
			return false;
		}
	}
	if (out_next_client_seq != nullptr) *out_next_client_seq = sequence;
	return true;
}

const ProtocolMessage *find_reply(
		const std::vector<ProtocolMessage> &messages, uint8_t tag) {
	for (const ProtocolMessage &message : messages) {
		if (message.tag == tag) return &message;
	}
	return nullptr;
}

// The socket owner broadcasts this neutral NP/NAPI 0x41 probe. A host replies
// with its live retail ServerHello fields but does not admit the scanner; only
// a subsequent validated 0x42 changes the player/connection count.
bool run_lan_discovery_metadata_is_live_and_stateless() {
	netsim::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::GameConfig config;
	config.server_name = "Configured LAN Host";
	config.game_type = 0x00010020u;
	config.max_players = 11;
	config.expansion = "jox99";
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Lan,
	                        kHostKey, &loopback, config);

	const std::vector<uint8_t> probe = np::build_lan_discovery_probe(0x11223344u);
	uint8_t probe_opcode = 0;
	std::vector<uint8_t> probe_body;
	if (!expect(nw_decode_inbound(probe.data(), probe.size(), probe_opcode, probe_body) &&
	            probe_opcode == SESSION_OPCODE_CLIENT_HELLO,
	            "LAN discovery probe is a framed 0x41")) return false;
	ClientHello decoded_probe;
	if (!expect(parse_client_hello(probe_body.data(), probe_body.size(), decoded_probe),
	            "LAN discovery probe ClientHello parses")) return false;
	if (!expect(decoded_probe.pn == "JOINTOPERATIONS" && decoded_probe.ci == 0x11223344u,
	            "LAN discovery probe carries JointOperations identity and client index")) return false;
	const std::array<uint8_t, 16> direct_join_pg =
			{0x46, 0xD6, 0x74, 0xB0, 0xF9, 0x81, 0x5F, 0x47,
			 0x92, 0xDA, 0xDE, 0xA7, 0x24, 0x7F, 0x14, 0x68};
	if (!expect(decoded_probe.nvs ==
	                    "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic" &&
	            decoded_probe.co == "NovaLogic Inc, Calabasas CA U.S.A." &&
	            decoded_probe.ap == "Jointops.exe" &&
	            decoded_probe.bdat == "Jul 21 2009 18:54:42" &&
	            decoded_probe.pv1 == "0.0.0 1/12/2004 EM" && decoded_probe.pv2 == "16" &&
	            decoded_probe.pg_present && decoded_probe.pg == direct_join_pg,
	            "LAN discovery probe matches the exact retail JO identity")) return false;

	const PeerAddr scanner{0x0100007Fu, 32100};
	const std::size_t before_scan = np::connection_count(ctx);
	auto first = np::handle_server_datagram(ctx, scanner, probe.data(), probe.size(), 1);
	if (!expect(first.outbound.size() == 1, "LAN discovery 0x41 receives one 0x81")) return false;
	if (!expect(first.events.empty(), "LAN discovery 0x41 emits no peer event")) return false;
	if (!expect(np::connection_count(ctx) == before_scan,
	            "LAN discovery 0x41 does not register the scanner")) return false;

	np::LanDiscoveryServer found;
	if (!expect(np::parse_lan_discovery_reply(first.outbound[0].data(), first.outbound[0].size(), found),
	            "LAN discovery parser accepts the host 0x81")) return false;
	if (!expect(found.server_name == config.server_name, "0x81 SN reflects server_name")) return false;
	if (!expect(found.gametype == config.game_type, "0x81 P1 reflects gametype")) return false;
	if (!expect(found.current_players == 1, "0x81 NP counts the host loopback")) return false;
	if (!expect(found.max_players == config.max_players, "0x81 MP reflects max_players")) return false;
	if (!expect(found.expansion == config.expansion, "0x81 SUS2 reflects expansion")) return false;
	if (!expect(found.session_id.empty(), "0x81 omits SUS1 when no real session user string exists")) return false;

	// An unrelated service-shaped 0x81 received during the browse window must
	// not become a clickable LAN game row merely because its flat TLV parses.
	uint8_t first_opcode = 0;
	std::vector<uint8_t> first_body;
	ServerHello foreign;
	if (!expect(nw_decode_inbound(first.outbound[0].data(), first.outbound[0].size(),
	                              first_opcode, first_body) &&
	                    parse_server_hello(first_body.data(), first_body.size(), foreign),
	            "test can decode the discovered 0x81")) return false;
	foreign.pn = "NOVAWORLDUDP";
	const std::vector<uint8_t> foreign_reply = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(foreign));
	np::LanDiscoveryServer ignored;
	if (!expect(!np::parse_lan_discovery_reply(
	                    foreign_reply.data(), foreign_reply.size(), ignored),
	            "LAN discovery rejects a non-JO 0x81")) return false;

	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const PeerAddr joiner{0x0100007Fu, 32101};
	auto auth = craft_auth("JOINTOPERATIONS", kHostKey, 0x12345678u, scrk);
	auto joined = np::handle_server_datagram(ctx, joiner, auth.data(), auth.size(), 2);
	if (!expect(!joined.outbound.empty(), "validated 0x42 admits the real joiner")) return false;
	if (!expect(np::connection_count(ctx) == before_scan + 1,
	            "only 0x42 adds the remote connection")) return false;

	auto second = np::handle_server_datagram(ctx, scanner, probe.data(), probe.size(), 3);
	np::LanDiscoveryServer refreshed;
	if (!expect(second.outbound.size() == 1 &&
	            np::parse_lan_discovery_reply(second.outbound[0].data(), second.outbound[0].size(), refreshed),
	            "repeated LAN discovery receives a parseable 0x81")) return false;
	if (!expect(refreshed.current_players == 2, "0x81 NP updates after admission")) return false;
	if (!expect(np::connection_count(ctx) == before_scan + 1,
	            "repeated discovery still creates no connection")) return false;

	// Zero-valued gated fields are absent on the wire. The discovery parser
	// must still report the live zeroes rather than ServerHello builder defaults.
	np::NapiNPServerCtx dedicated;
	np::GameConfig dedicated_config;
	dedicated_config.server_name = "Empty Dedicated Host";
	dedicated_config.game_type = 0;
	dedicated_config.max_players = 4;
	dedicated_config.expansion.clear();
	np::test::bring_up_host(dedicated, np::ConnectionMode::HostOnly,
	                        np::SocketMode::Lan, kHostKey, nullptr, dedicated_config);
	auto empty_reply = np::handle_server_datagram(
			dedicated, scanner, probe.data(), probe.size(), 4);
	np::LanDiscoveryServer empty;
	if (!expect(empty_reply.outbound.size() == 1 &&
	            np::parse_lan_discovery_reply(empty_reply.outbound[0].data(),
	                                          empty_reply.outbound[0].size(), empty),
	            "empty dedicated host returns a parseable 0x81")) return false;
	if (!expect(empty.current_players == 0 && empty.gametype == 0 && empty.expansion.empty(),
	            "omitted NP/P1/SUS2 fields parse as live zero/empty values")) return false;
	if (!expect(np::connection_count(dedicated) == 0,
	            "dedicated-host discovery remains stateless")) return false;
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
	uint32_t server_sk = 0;
	uint32_t seq = 1;
	if (!handshake(
				ctx, peer, client_scrk, client_ck, server_scrk,
				&server_sk, &seq)) {
		return false;
	}

	auto send_session = [&](std::vector<ProtocolMessage> msgs, uint32_t now,
	                        std::vector<ProtocolMessage> &out) -> bool {
		auto dg = craft_session(client_scrk, server_sk, seq++, msgs);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		if (r.outbound.empty()) return false;
		ProtocolPacketHeader hdr;
		return decode_s2c(r.outbound.back(), server_scrk, hdr, out);
	};

	// The helper completed the admission exchange, so a later C2S 0x02 is a no-op
	// (roster_pushed is already set). Verify it doesn't crash or double-emit.
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
	uint32_t server_sk = 0;
	uint32_t seq = 1;
	if (!handshake(
				ctx, peer, client_scrk, client_ck, server_scrk,
				&server_sk, &seq)) {
		return false;
	}

	auto mission_dg = craft_session(
			client_scrk, server_sk, seq++, {make_protocol_message(0x37, {})});
	np::handle_server_datagram(ctx, peer, mission_dg.data(), mission_dg.size(), 100);
	if (!expect(np::bind_connection_player(ctx, peer, 1, 0x0005),
	            "server binds the connection to its allocated player entity handle")) return false;

	auto spawn_req = craft_session(
			client_scrk, server_sk, seq++,
			{make_protocol_message(0x29, {0x00, 0x00})});
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
	w.registry.configure_pool(1, 8);
	const world::EntityHandle h = w.registry.spawn(0, world::Entity{});
	if (!expect(h.valid() && w.registry.get(h) != nullptr, "test entity spawns")) return false;
	world::EntityHandle emplacement;
	if (!expect(
				mount_on_test_emplacement(w, h, emplacement) &&
						test_emplacement_is_owned_by(w, emplacement, h),
				"goodbye fixture owns the emplaced-gun seat and control latches")) {
		return false;
	}
	ctx.world = &w;

	const PeerAddr peer{0x0100007Fu, 30700};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	std::string server_scrk;
	uint32_t server_sk = 0;
	if (!handshake(ctx, peer, client_scrk, 0xC0FFEE01u, server_scrk, &server_sk)) return false;
	if (!expect(np::bind_connection_player(ctx, peer, 1, h.packed),
	            "connection binds the live world entity")) return false;

	// The goodbye carries the receiver-local server key. Empty, truncated, wrong-key, and stale
	// prior-session packets must not tear down the live connection.
	auto malformed_bye = craft(SESSION_OPCODE_CLIENT_GOODBYE, {});
	np::handle_server_datagram(ctx, peer, malformed_bye.data(), malformed_bye.size(), 398);
	if (!expect(w.registry.get(h) != nullptr && np::connection_count(ctx) == 1,
	            "empty goodbye body is ignored")) return false;
	auto wrong_bye = craft(
			SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(server_sk ^ 0x01010101u));
	np::handle_server_datagram(ctx, peer, wrong_bye.data(), wrong_bye.size(), 399);
	if (!expect(w.registry.get(h) != nullptr && np::connection_count(ctx) == 1,
	            "wrong receiver-local goodbye key is ignored")) return false;

	auto bye = craft(SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(server_sk));
	np::handle_server_datagram(ctx, peer, bye.data(), bye.size(), 400);

	if (!expect(w.registry.get(h) == nullptr,
	            "goodbye despawns the owned world entity (D-NET-149)")) return false;
	if (!expect(
				test_emplacement_is_released(w, emplacement),
				"goodbye releases the emplaced-gun seat and control latches")) {
		return false;
	}
	bool node_gone = true;
	for (const auto &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) node_gone = false;
	}
	if (!expect(node_gone, "goodbye erases the connection node")) return false;
	return true;
}

// A different CI/CK reusing one UDP endpoint is a replacement, not a vector erase. The old
// authoritative entity and roster membership must be torn down before the new session is admitted.
bool run_same_endpoint_reconnect_fully_tears_down_old_session() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	world::World w;
	w.registry.configure_pool(0, 8);
	w.registry.configure_pool(1, 8);
	ctx.world = &w;

	const PeerAddr peer{0x0100007Fu, 30710};
	const std::string old_scrk =
			"OLDCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD";
	std::string server_scrk;
	if (!handshake(ctx, peer, old_scrk, 0xC0FFEE11u, server_scrk)) return false;
	const world::EntityHandle old_entity = w.registry.spawn(0, world::Entity{});
	if (!expect(old_entity.valid() &&
	                    np::bind_connection_player(ctx, peer, 1, old_entity.packed),
	            "endpoint-reuse fixture binds the old entity")) return false;
	world::EntityHandle emplacement;
	if (!expect(
				mount_on_test_emplacement(w, old_entity, emplacement) &&
						test_emplacement_is_owned_by(
								w, emplacement, old_entity),
				"endpoint-reuse fixture owns the emplaced-gun seat and control latches")) {
		return false;
	}
	for (np::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		conn.phase = np::ConnectionPhase::PlayerAdded;
		conn.burst.spawned = true;
		conn.reply.roster_counted = true;
	}
	const uint32_t roster_before = ctx.np_protocol.roster_generation;

	ClientAuth replacement = make_valid_client_auth(
			2, 0xC0FFEE22u, kHostKey, "ReplacementJoiner",
			"NEWCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD");
	auto replacement_dg = craft(
			SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(replacement));
	auto result = np::handle_server_datagram(
			ctx, peer, replacement_dg.data(), replacement_dg.size(), 500);

	if (!expect(!result.outbound.empty() && np::connection_count(ctx) == 1,
	            "replacement endpoint is admitted as one fresh connection")) return false;
	if (!expect(w.registry.get(old_entity) == nullptr,
	            "endpoint replacement despawns the old authoritative entity")) return false;
	if (!expect(
				test_emplacement_is_released(w, emplacement),
				"endpoint replacement releases the emplaced-gun seat and control latches")) {
		return false;
	}
	if (!expect(ctx.np_protocol.roster_generation == roster_before + 1,
	            "endpoint replacement advances the roster generation")) return false;
	const np::NapiNPConnection &fresh = ctx.np_protocol.connection_list.front();
	if (!expect(fresh.client_ci == replacement.ci && fresh.client_ck == replacement.ck &&
	                    fresh.player_name == replacement.na &&
	                    !fresh.link.owned_entity.valid(),
	            "replacement carries only the new session identity and no stale binding")) return false;
	return true;
}

// Retail's JO connection profile reaps an otherwise-valid peer after 120000 ms of receive
// inactivity. The reap must take the same complete entity/roster teardown path as keyed goodbye.
bool run_inactive_peer_is_reaped() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	world::World w;
	w.registry.configure_pool(0, 8);
	w.registry.configure_pool(1, 8);
	ctx.world = &w;

	const PeerAddr peer{0x0100007Fu, 30720};
	const std::string scrk =
			"IDLECLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABC";
	std::string server_scrk;
	uint32_t server_sk = 0;
	if (!handshake(
				ctx, peer, scrk, 0xC0FFEE33u, server_scrk, &server_sk)) {
		return false;
	}
	const world::EntityHandle entity = w.registry.spawn(0, world::Entity{});
	if (!expect(entity.valid() && np::bind_connection_player(ctx, peer, 1, entity.packed),
	            "idle-reap fixture binds the player entity")) return false;
	world::EntityHandle emplacement;
	if (!expect(
				mount_on_test_emplacement(w, entity, emplacement) &&
						test_emplacement_is_owned_by(w, emplacement, entity),
				"idle-reap fixture owns the emplaced-gun seat and control latches")) {
		return false;
	}
	for (np::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		conn.phase = np::ConnectionPhase::PlayerAdded;
		conn.burst.spawned = true;
		conn.reply.roster_counted = true;
		conn.receive_inactive_ms = 119999;
	}

	// A packet can decrypt with the peer SCRK yet name the wrong receiver-local SK. Deframing
	// classifies it as unadmitted; it must not refresh the live connection's receive clock.
	auto wrong_receiver = craft_session(
			scrk, server_sk ^ 0x01010101u, 4, {});
	auto ignored = np::handle_server_datagram(
			ctx, peer, wrong_receiver.data(), wrong_receiver.size(), 6);
	if (!expect(ignored.outbound.empty(), "wrong receiver-local 0x43 is silently ignored"))
		return false;
	if (!expect(
				ctx.np_protocol.connection_list.front().receive_inactive_ms == 119999,
				"wrong receiver-local 0x43 does not reset the inactivity clock")) {
		return false;
	}

	const std::vector<np::TickOut> outs =
			np::tick_connections(ctx, /*elapsed_ms=*/2, /*now_tick=*/7);
	bool saw_goodbye = false;
	for (const np::TickOut &out : outs)
		for (const np::HostAcceptEvent &event : out.events)
			if (event.kind == np::HostAcceptEvent::Kind::PeerGoodbye &&
			    event.peer == peer)
				saw_goodbye = true;
	if (!expect(saw_goodbye, "inactivity reap surfaces owner cleanup for the dead endpoint"))
		return false;
	if (!expect(np::connection_count(ctx) == 0,
	            "120000 ms inactive peer is removed from the connection list")) return false;
	if (!expect(w.registry.get(entity) == nullptr,
	            "inactivity reap despawns the authoritative player entity")) return false;
	return expect(
			test_emplacement_is_released(w, emplacement),
			"inactivity reap releases the emplaced-gun seat and control latches");
}

bool run_game_environment_and_admission_fsm_are_enforced() {
	const std::string scrk =
			"FSMCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABC";

	// The 1.7.5.7 host validates these NapiNetConfig values before allocating a
	// connection. They are binary literals, not negotiable host settings.
	{
		np::NapiNPServerCtx ctx;
		np::test::bring_up_host(
				ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
		struct BadField {
			const char *name;
			const char *value;
		};
		for (const BadField bad : {
					BadField{"BN", "0"},
					BadField{"VN", "3"},
					BadField{"MBN", "20042001"},
					BadField{"SOPD", "179"},
					BadField{"BT", "1"},
				}) {
			ClientAuth auth = make_valid_client_auth(
					1, 0xA0000000u, kHostKey, "BadEnvironment", scrk);
			// LoadFromConnTags is an ordered overwrite; the final duplicate is
			// the value Server_ValidatePlayerJoinRequest observes.
			auth.cu.push_back(make_client_cu_chunk(2, bad.name, bad.value));
			const PeerAddr peer{
					0x0100007Fu,
					static_cast<uint16_t>(31300 + (bad.name[0] + bad.name[1]))};
			auto dg = craft(
					SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
			auto result = np::handle_server_datagram(
					ctx, peer, dg.data(), dg.size(), 1);
			if (!expect(
						result.outbound.empty() && np::connection_count(ctx) == 0,
						"invalid retail game-environment literal is rejected before allocation")) {
				return false;
			}
		}

		ClientAuth missing = make_jointoperations_client_auth(
				1, 0xA0000001u, kHostKey, "MissingEnvironment", scrk);
		auto dg = craft(
				SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(missing));
		auto result = np::handle_server_datagram(
				ctx, PeerAddr{0x0100007Fu, 31320}, dg.data(), dg.size(), 2);
		if (!expect(
					result.outbound.empty() && np::connection_count(ctx) == 0,
					"missing required game-environment literals is rejected before allocation")) {
			return false;
		}
	}

	// A form post cannot skip the JOIN request. The protocol violation tears
	// down the pending node, so it occupies neither a player slot nor an entity.
	{
		np::NapiNPServerCtx ctx;
		np::test::bring_up_host(
				ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
		world::World w;
		world::AiSystem ai;
		w.ai = &ai;
		w.registry.configure_pool(0, 16);
		ctx.world = &w;
		const PeerAddr peer{0x0100007Fu, 31330};
		std::string server_scrk;
		uint32_t server_sk = 0;
		if (!handshake(
					ctx, peer, scrk, 0xA0000010u, server_scrk,
					&server_sk, nullptr, false)) {
			return false;
		}
		if (!expect(
					!ctx.np_protocol.connection_list.front().self_id_seen,
					"0x42 alone does not mark a remote player admitted")) {
			return false;
		}
		np::tick_connections(ctx, 16, 10);
		if (!expect(
					!ctx.np_protocol.connection_list.front().link.owned_entity.valid(),
					"0x42 alone cannot spawn an authoritative entity")) {
			return false;
		}

		auto dg = craft_session(
				scrk, server_sk, 1,
				{make_protocol_message(0x01, {0x00})});
		auto result = np::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), 11);
		if (!expect(
					result.outbound.empty() && np::connection_count(ctx) == 0,
					"out-of-order 0x01 is dropped and releases the pending admission")) {
			return false;
		}
	}

	// The JOIN request itself is structural: the golden base-game request is
	// VERSIONCRCSTRING="0". A malformed body cannot advance the FSM.
	{
		np::NapiNPServerCtx ctx;
		np::test::bring_up_host(
				ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
		const PeerAddr peer{0x0100007Fu, 31331};
		std::string server_scrk;
		uint32_t server_sk = 0;
		if (!handshake(
					ctx, peer, scrk, 0xA0000011u, server_scrk,
					&server_sk, nullptr, false)) {
			return false;
		}
		auto dg = craft_session(
				scrk, server_sk, 1,
				{make_protocol_message(0x00, {'b', 'a', 'd'})});
		auto result = np::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), 12);
		if (!expect(
					result.outbound.empty() && np::connection_count(ctx) == 0,
					"malformed 0x00 JOIN is dropped and releases the pending admission")) {
			return false;
		}
	}

	// Happy path: each request advances exactly one turn, and the authoritative
	// player becomes spawn-eligible only after the 256-byte challenge echo.
	{
		np::NapiNPServerCtx ctx;
		np::test::bring_up_host(
				ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
		world::World w;
		world::AiSystem ai;
		w.ai = &ai;
		w.registry.configure_pool(0, 16);
		ctx.world = &w;
		const PeerAddr peer{0x0100007Fu, 31332};
		std::string server_scrk;
		uint32_t server_sk = 0;
		if (!handshake(
					ctx, peer, scrk, 0xA0000012u, server_scrk,
					&server_sk, nullptr, false)) {
			return false;
		}

		auto join_dg = craft_session(
				scrk, server_sk, 1,
				{make_protocol_message(
						0x00, retail_join_request(ctx.config.expansion))});
		auto join_result = np::handle_server_datagram(
				ctx, peer, join_dg.data(), join_dg.size(), 100);
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> replies;
		if (!expect(
					join_result.outbound.size() == 1 &&
							decode_s2c(
									join_result.outbound.front(), server_scrk, hdr, replies) &&
							replies.size() == 1 && replies.front().tag == 0x00 &&
							replies.front().payload.empty(),
					"valid 0x00 receives the empty JOIN acknowledgement")) {
			return false;
		}
		if (!expect(
					!ctx.np_protocol.connection_list.front().self_id_seen,
					"0x00 acknowledgement still does not make the player spawn-eligible")) {
			return false;
		}

		auto form_dg = craft_session(
				scrk, server_sk, 2,
				{make_protocol_message(0x01, {0x00})});
		auto form_result = np::handle_server_datagram(
				ctx, peer, form_dg.data(), form_dg.size(), 101);
		replies.clear();
		if (!expect(
					form_result.outbound.size() == 1 &&
							decode_s2c(
									form_result.outbound.front(), server_scrk, hdr, replies) &&
							replies.size() == 1 && replies.front().tag == 0x02 &&
							replies.front().payload.size() == 512,
					"valid 0x01 receives the 512-byte padding challenge")) {
			return false;
		}
		if (!expect(
					!ctx.np_protocol.connection_list.front().self_id_seen,
					"0x01 challenge still does not make the player spawn-eligible")) {
			return false;
		}

		std::vector<uint8_t> echo(
				replies.front().payload.begin(),
				replies.front().payload.begin() + 256);
		// The third dword is the client's local net-frame counter, not the
		// challenge's padding-length field.
		std::fill(echo.begin() + 8, echo.begin() + 12, 0);
		auto echo_dg = craft_session(
				scrk, server_sk, 3,
				{make_protocol_message(0x02, std::move(echo))});
		auto echo_result = np::handle_server_datagram(
				ctx, peer, echo_dg.data(), echo_dg.size(), 102);
		if (!expect(
					!echo_result.outbound.empty() &&
							np::connection_count(ctx) == 1 &&
							ctx.np_protocol.connection_list.front().self_id_seen &&
							ctx.np_protocol.connection_list.front().reply.roster_pushed,
					"valid 0x02 echo completes admission and emits post-handshake metadata")) {
			return false;
		}
		np::tick_connections(ctx, 16, 103);
		if (!expect(
					ctx.np_protocol.connection_list.front().link.owned_entity.valid(),
					"completed 0x00->0x01->0x02 admission can spawn the player entity")) {
			return false;
		}
	}
	return true;
}

bool run_non_jo_peer_is_ignored() {
	// The join legs validate the complete retail JO identity + HK echo (the @0x6213b0/@0x62b750
	// gates). Any mismatched version field is silently dropped with no reply and no connection.
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
	auto reject_hello = [&](ClientHello hello, uint32_t now, const char *message) {
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		return expect(r.outbound.empty() && np::connection_count(ctx) == 0, message);
	};
	{
		ClientHello hello = make_jointoperations_client_hello(1);
		hello.nvs = "wrong";
		if (!reject_hello(hello, 2, "wrong-NVS 0x41 is dropped")) return false;
		hello = make_jointoperations_client_hello(1);
		hello.pg[0] ^= 0xFFu;
		if (!reject_hello(hello, 3, "wrong-PG 0x41 is dropped")) return false;
		hello = make_jointoperations_client_hello(1);
		hello.pv1 = "wrong";
		if (!reject_hello(hello, 4, "wrong-PV1 0x41 is dropped")) return false;
	}
	// (b) non-JO ClientAuth -> no ServerAuth, no node (the join re-validates PN).
	{
		auto dg = craft_auth("NOVAWORLDUDP", kHostKey, 0xDEADBEEFu, scrk);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 5);
		if (!expect(r.outbound.empty(), "non-JO 0x42 produces no ServerAuth")) return false;
		if (!expect(np::connection_count(ctx) == 0, "non-JO 0x42 registers no connection")) return false;
	}
	auto reject_auth = [&](ClientAuth auth, uint32_t now, const char *message) {
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		return expect(r.outbound.empty() && np::connection_count(ctx) == 0, message);
	};
	{
		ClientAuth auth = make_valid_client_auth(
				1, 0xDEADBEEFu, kHostKey, "TestJoiner", scrk);
		auth.nvs = "wrong";
		if (!reject_auth(auth, 6, "wrong-NVS 0x42 is dropped")) return false;
		auth = make_valid_client_auth(1, 0xDEADBEEFu, kHostKey, "TestJoiner", scrk);
		auth.pg_present = false;
		if (!reject_auth(auth, 7, "missing-PG 0x42 is dropped")) return false;
		auth = make_valid_client_auth(1, 0xDEADBEEFu, kHostKey, "TestJoiner", scrk);
		auth.pv1 = "wrong";
		if (!reject_auth(auth, 8, "wrong-PV1 0x42 is dropped")) return false;
		auth = make_valid_client_auth(1, 0xDEADBEEFu, kHostKey, "TestJoiner", scrk);
		auth.pv2 = "wrong";
		if (!reject_auth(auth, 9, "wrong-PV2 0x42 is dropped")) return false;
		auth = make_valid_client_auth(1, 0xDEADBEEFu, kHostKey, "", scrk);
		if (!reject_auth(auth, 10, "empty-NA 0x42 is dropped")) return false;
	}
	// (c) JO ClientAuth with the WRONG host key -> dropped (HK echo gate).
	{
		auto dg = craft_auth("JOINTOPERATIONS", kHostKey ^ 0x1u, 0xDEADBEEFu, scrk);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 11);
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

	ClientHello hello = make_jointoperations_client_hello(1);
	hello.co = "EarlyBird";
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
	ClientHello hello = make_jointoperations_client_hello(1);
	hello.co = "RemoteJoiner";
	auto h = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = np::handle_server_datagram(ctx, peer, h.data(), h.size(), 1);
	if (!expect(rh.outbound.size() == 1, "remote 0x41 -> one ServerHello")) return false;
	auto a = craft_auth("JOINTOPERATIONS", kHostKey, 0xDEADBEEFu, scrk);
	auto ra = np::handle_server_datagram(ctx, peer, a.data(), a.size(), 2);
	if (!expect(ra.outbound.size() >= 1, "remote 0x42 -> ServerAuth + initial settings")) return false;

	if (!expect(np::connection_count(ctx) == 2, "joiner added alongside the loopback")) return false;
	int loopbacks = 0, remotes = 0;
	for (const auto &c : ctx.np_protocol.connection_list) {
		if (c.type == 2) ++loopbacks;
		else if (c.type == 1) ++remotes;
	}
	if (!expect(loopbacks == 1 && remotes == 1, "exactly one loopback + one remote joiner")) return false;
	return true;
}

// S2C 0x04 precedes the next host-side player-add pass, so byte 17 must be an
// authoritative reservation rather than the default reply-state zero. This also
// proves two admissions drained before spawn cannot advertise the same identity.
bool run_post_handshake_slot_is_reserved_until_spawn() {
	world::World world;
	world::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);

	netsim::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::GameConfig settings;
	settings.max_players = 8;
	np::test::bring_up_host(
			ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
			kHostKey, &loopback, settings);
	ctx.world = &world;
	if (!expect(
				np::Server_ProcessPendingPlayerSpawns(ctx, world) == 1 &&
						ctx.np_protocol.connection_list.front().reply.player_slot == 0,
				"listen host owns roster slot 0 before remote admission")) {
		return false;
	}

	const std::string client_scrk =
			"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const PeerAddr first_peer{0x0100007Fu, 31210};
	const PeerAddr second_peer{0x0100007Fu, 31211};
	std::string first_server_scrk;
	std::string second_server_scrk;
	std::vector<ProtocolMessage> first_burst;
	std::vector<ProtocolMessage> second_burst;
	if (!handshake(
				ctx, first_peer, client_scrk, 0x10101010u,
				first_server_scrk, nullptr, nullptr, true, &first_burst) ||
	    !handshake(
				ctx, second_peer, client_scrk, 0x20202020u,
				second_server_scrk, nullptr, nullptr, true, &second_burst)) {
		return false;
	}

	const ProtocolMessage *first_slot = find_reply(first_burst, 0x04);
	const ProtocolMessage *second_slot = find_reply(second_burst, 0x04);
	if (!expect(
				first_slot != nullptr && first_slot->payload.size() >= 19 &&
						first_slot->payload[17] == 1,
				"first remote 0x04 reserves slot 1 beside the listen host")) {
		return false;
	}
	if (!expect(
				second_slot != nullptr && second_slot->payload.size() >= 19 &&
						second_slot->payload[17] == 2,
				"second admission before either spawn reserves distinct slot 2")) {
		return false;
	}
	bool first_reserved = false;
	bool second_reserved = false;
	for (const np::NapiNPConnection &connection :
	     ctx.np_protocol.connection_list) {
		if (connection.peer == first_peer)
			first_reserved = connection.reply.player_slot_reserved;
		if (connection.peer == second_peer)
			second_reserved = connection.reply.player_slot_reserved;
	}
	if (!expect(
				first_reserved && second_reserved,
				"both advertised slots remain reserved until player-add")) {
		return false;
	}

	if (!expect(
				np::Server_ProcessPendingPlayerSpawns(ctx, world) == 2,
				"both admitted remotes spawn on the next host pass")) {
		return false;
	}
	uint8_t first_spawned_slot = 0xFF;
	uint8_t second_spawned_slot = 0xFF;
	bool spawned_reservation_left = false;
	for (const np::NapiNPConnection &connection :
	     ctx.np_protocol.connection_list) {
		if (connection.peer == first_peer) {
			first_spawned_slot = connection.reply.player_slot;
			spawned_reservation_left |=
					connection.reply.player_slot_reserved;
		}
		if (connection.peer == second_peer) {
			second_spawned_slot = connection.reply.player_slot;
			spawned_reservation_left |=
					connection.reply.player_slot_reserved;
		}
	}
	if (!expect(
				first_spawned_slot == 1 && second_spawned_slot == 2 &&
						!spawned_reservation_left,
				"spawn consumes the exact two slots advertised pre-spawn")) {
		return false;
	}

	if (!expect(
				np::drop_connection(ctx, first_peer),
				"disconnect releases the first remote roster identity")) {
		return false;
	}
	const PeerAddr replacement_peer{0x0100007Fu, 31212};
	std::string replacement_server_scrk;
	std::vector<ProtocolMessage> replacement_burst;
	if (!handshake(
				ctx, replacement_peer, client_scrk, 0x30303030u,
				replacement_server_scrk, nullptr, nullptr, true,
				&replacement_burst)) {
		return false;
	}
	const ProtocolMessage *replacement_slot =
			find_reply(replacement_burst, 0x04);
	if (!expect(
				replacement_slot != nullptr &&
						replacement_slot->payload.size() >= 19 &&
						replacement_slot->payload[17] == 1,
				"replacement admission reuses the released slot 1")) {
		return false;
	}
	if (!expect(
				np::drop_connection(ctx, replacement_peer),
				"pre-spawn teardown releases an advertised reservation")) {
		return false;
	}
	const PeerAddr second_replacement_peer{0x0100007Fu, 31213};
	std::string second_replacement_server_scrk;
	std::vector<ProtocolMessage> second_replacement_burst;
	if (!handshake(
				ctx, second_replacement_peer, client_scrk, 0x40404040u,
				second_replacement_server_scrk, nullptr, nullptr, true,
				&second_replacement_burst)) {
		return false;
	}
	const ProtocolMessage *second_replacement_slot =
			find_reply(second_replacement_burst, 0x04);
	return expect(
			second_replacement_slot != nullptr &&
					second_replacement_slot->payload.size() >= 19 &&
					second_replacement_slot->payload[17] == 1,
			"a later admission reuses the pre-spawn reservation after teardown");
}

// A retransmitted 0x42 ClientAuth for an already-joined connection must re-send
// the cached ServerAuth, not re-mint the server SCRK/SK.
bool run_retransmit_0x42_keeps_keys() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 30800};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t ck = 0x12345678u;

	auto first_auth = [&](uint32_t now, ServerAuth &out_sa) -> bool {
		auto a = craft_auth("JOINTOPERATIONS", kHostKey, ck, scrk); // craft_auth uses CI = 1
		auto r = np::handle_server_datagram(ctx, peer, a.data(), a.size(), now);
		if (!expect(r.outbound.size() >= 1, "0x42 -> ServerAuth + initial settings")) return false;
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
		auto a = craft_auth("JOINTOPERATIONS", kHostKey, ck, scrk);
		return np::handle_server_datagram(ctx, p, a.data(), a.size(), now);
	};

	if (!expect(join(PeerAddr{0x0100007Fu, 32001}, 0x1111u, 1).outbound.size() >= 1,
	            "joiner 1 key-established (ServerAuth + initial settings)")) return false;
	if (!expect(join(PeerAddr{0x0100007Fu, 32002}, 0x2222u, 2).outbound.size() >= 1,
	            "joiner 2 key-established (ServerAuth + initial settings)")) return false;
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
	uint32_t server_sk = 0;
	uint32_t seq = 1;
	if (!handshake(
			ctx, peer, client_scrk, 0xDEADBEE2u, server_scrk,
			&server_sk, &seq)) return false;

	auto send_session = [&](std::vector<ProtocolMessage> msgs, uint32_t now,
	                        std::vector<ProtocolMessage> &out) -> bool {
		auto dg = craft_session(client_scrk, server_sk, seq++, msgs);
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
		auto dg = craft_session(
				client_scrk, server_sk, seq++,
				{make_protocol_message(0x2F, req)});
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

// The C2S 0x2F SUBMISSION ENVELOPE, validated before anything is applied [orig:
// NapiNPServerMsg_HandlePlayerLoadout @0x515790]: the team byte must be 1..4 — above 4 passes only
// in a team-less game type (@0x5158a9) — and a NONZERO class byte must be 5..9 (@0x5158b1, the
// abort @0x515fa5). A failing envelope aborts the handler: retail only re-sends the player's
// CURRENT slot list (Server_SendWeaponSlotListToPlayer @0x502550, whose header byte is the LIVE
// player+89820 class) and writes nothing — no class stamp, no phase-8 gate, no retained body. Class
// 0 is a real apply with an empty soldier-type mask (@0x5159af): it grants no slot and stamps
// entity+660 zero (@0x515ab0). This host is table-less, so an accepted grant echoes its request
// entries and the class-0 EMPTY grant is unambiguous.
bool run_loadout_envelope_gates() {
	np::NapiNPServerCtx ctx;
	np::GameConfig settings;
	settings.max_players = 8;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey,
	                        nullptr, settings);
	world::World world;
	world.registry.configure_pool(0, 16);
	world::AiSystem ai;
	world.ai = &ai; // the §5.2b spawn mounts the infantry motor
	ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 30150};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	std::string server_scrk;
	uint32_t server_sk = 0;
	uint32_t seq = 1;
	if (!handshake(ctx, peer, client_scrk, 0xDEADBEE3u, server_scrk, &server_sk, &seq)) return false;
	// The spawn pump binds the joiner's pool-0 entity, so the handler has a real entity+660 to
	// stamp (or to leave alone). [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]
	if (!expect(np::Server_ProcessPendingPlayerSpawns(ctx, world) == 1,
	            "the admitted joiner spawns its player entity")) return false;

	auto joiner_conn = [&]() -> const np::NapiNPConnection * {
		for (const np::NapiNPConnection &c : ctx.np_protocol.connection_list)
			if (c.peer == peer) return &c;
		return nullptr;
	};
	auto joiner_entity = [&]() -> world::Entity * {
		const np::NapiNPConnection *c = joiner_conn();
		if (c == nullptr || !c->link.owned_entity.valid()) return nullptr;
		return world.registry.get(c->link.owned_entity);
	};
	if (!expect(joiner_entity() != nullptr, "the joiner's player entity resolves")) return false;
	joiner_entity()->player_class = 6; // the live class an aborted submission must not disturb

	// One 0x2F in, the last 0x5A body out (empty when the handler produced none).
	auto submit = [&](const std::vector<uint8_t> &req, uint32_t now) -> std::vector<uint8_t> {
		auto dg = craft_session(client_scrk, server_sk, seq++, {make_protocol_message(0x2F, req)});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		std::vector<uint8_t> body;
		for (const std::vector<uint8_t> &outbound : r.outbound) {
			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> msgs;
			if (!decode_s2c(outbound, server_scrk, hdr, msgs)) continue;
			for (const ProtocolMessage &m : msgs)
				if (m.tag == 0x5A) body = m.payload;
		}
		return body;
	};

	// (1) Team 0 with an otherwise valid class 8 kit, BEFORE any grant exists: the abort re-sends
	// the current (empty) slot list headed by the live class 6 — not the requested 8 — and leaves
	// the whole loadout state untouched.
	{
		const std::vector<uint8_t> body =
				submit({0x00, 0x08, 0xC3, 0x00, 0x00, 0x00, 9, 0xFF, 0xFF, 0xFF, 0xFF}, 200);
		WeaponLoadout lo;
		if (!expect(decode_weapon_loadout(body.data(), body.size(), lo),
		            "team 0 still draws a 0x5A (the current-list re-send)")) return false;
		if (!expect(lo.avatar_class == 6 && lo.slots.empty(),
		            "the re-send carries the LIVE class and the player's current (empty) slots"))
			return false;
		const np::NapiNPConnection *c = joiner_conn();
		if (!expect(c != nullptr && !c->reply.loadout_synced && !c->burst.loadout_received &&
		                    c->reply.last_loadout_reply.empty(),
		            "an out-of-range team opens no gate and retains no body")) return false;
		if (!expect(joiner_entity()->player_class == 6,
		            "an out-of-range team never stamps entity+660")) return false;
	}

	// (2) The same kit with team 1: a real apply — the echoed grant, the class stamp, both gates.
	std::vector<uint8_t> granted;
	{
		granted = submit({0x01, 0x08, 0xC3, 0x00, 0x00, 0x00, 9, 0xFF, 0xFF, 0xFF, 0xFF}, 210);
		WeaponLoadout lo;
		if (!expect(decode_weapon_loadout(granted.data(), granted.size(), lo) &&
		                    lo.avatar_class == 8 && lo.slots.size() == 1 &&
		                    lo.slots[0].type_id == 9,
		            "a valid envelope grants the submitted kit")) return false;
		const np::NapiNPConnection *c = joiner_conn();
		if (!expect(c != nullptr && c->reply.loadout_synced && c->burst.loadout_received &&
		                    c->reply.last_loadout_reply == granted,
		            "the accepted grant opens the gates and is retained")) return false;
		if (!expect(joiner_entity()->player_class == 8,
		            "the accepted class stamps entity+660")) return false;
	}

	// (3) Class 12 (nonzero, outside 5..9): abort. The retained grant is re-sent BYTE-FOR-BYTE —
	// the request's own kit (adm 20) never reaches the reply — and nothing is rewritten.
	{
		const std::vector<uint8_t> body =
				submit({0x01, 0x0C, 0xC3, 0x00, 0x00, 0x00, 20, 0xFF, 0xFF, 0xFF, 0xFF}, 220);
		if (!expect(body == granted, "an out-of-range class re-sends the retained grant verbatim"))
			return false;
		const np::NapiNPConnection *c = joiner_conn();
		if (!expect(c != nullptr && c->reply.last_loadout_reply == granted,
		            "an out-of-range class disturbs no retained body")) return false;
		if (!expect(joiner_entity()->player_class == 8,
		            "an out-of-range class never restamps entity+660")) return false;
	}

	// (4) Team 5 in a TEAM-LESS game type (the default 0): accepted — the `team > 4` leg of the
	// envelope is gated on g_GameType [orig: @0x5158a9].
	{
		granted = submit({0x05, 0x08, 0xC3, 0x00, 0x00, 0x00, 20, 0xFF, 0xFF, 0xFF, 0xFF}, 230);
		WeaponLoadout lo;
		if (!expect(decode_weapon_loadout(granted.data(), granted.size(), lo) &&
		                    lo.slots.size() == 1 && lo.slots[0].type_id == 20,
		            "team 5 applies while the game type is team-less")) return false;
		const np::NapiNPConnection *c = joiner_conn();
		if (!expect(c != nullptr && c->reply.last_loadout_reply == granted,
		            "the team-5 grant is retained")) return false;
	}

	// (5) The same team 5 once the session IS team-based: now the envelope rejects it and the
	// previous grant is re-sent unchanged.
	{
		ctx.config.game_type = 0x10010u; // the golden ASH_I5A team-based gameType
		const std::vector<uint8_t> body =
				submit({0x05, 0x08, 0xC3, 0x00, 0x00, 0x00, 30, 0xFF, 0xFF, 0xFF, 0xFF}, 240);
		if (!expect(body == granted, "team 5 aborts in a team-based game type")) return false;
		ctx.config.game_type = 0;
	}

	// (6) Class 0: a real apply with an EMPTY grant — no row survives a zero soldier-type mask,
	// the reply is the bare header + terminator, and entity+660 is stamped 0.
	{
		const std::vector<uint8_t> body =
				submit({0x01, 0x00, 0xC3, 0x00, 0x00, 0x00, 9, 0xFF, 0xFF, 0xFF, 0xFF}, 250);
		WeaponLoadout lo;
		if (!expect(decode_weapon_loadout(body.data(), body.size(), lo) &&
		                    lo.avatar_class == 0 && lo.slots.empty(),
		            "class 0 grants nothing and heads the reply with class 0")) return false;
		const np::NapiNPConnection *c = joiner_conn();
		if (!expect(c != nullptr && c->reply.last_loadout_reply == body,
		            "the empty class-0 grant REPLACES the retained body")) return false;
		if (!expect(joiner_entity()->player_class == 0,
		            "class 0 stamps entity+660 zero")) return false;
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

	ClientAuth auth = make_valid_client_auth(
			1, 0x0BADF00Du, kHostKey, "TestJoiner",
			"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB");
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
	ok = run_loadout_envelope_gates() && ok;
	ok = run_plain_join_tag29_draws_no_tag51() && ok;
	ok = run_goodbye_despawns_player_entity() && ok;
	ok = run_same_endpoint_reconnect_fully_tears_down_old_session() && ok;
	ok = run_inactive_peer_is_reaped() && ok;
	ok = run_game_environment_and_admission_fsm_are_enforced() && ok;
	ok = run_non_jo_peer_is_ignored() && ok;
	ok = run_handshake_rejected_when_host_down() && ok;
	ok = run_lan_discovery_metadata_is_live_and_stateless() && ok;
	ok = run_listen_host_lifecycle() && ok;
	ok = run_post_handshake_slot_is_reserved_until_spawn() && ok;
	ok = run_retransmit_0x42_keeps_keys() && ok;
	ok = run_capacity_rejects_when_full() && ok;
	ok = run_character_join_vars_parsed() && ok;
	return ok ? 0 : 1;
}
