// Drives a crafted JointOperations joiner through the npruntime server legs
// (opennova::inmatch::handle_server_datagram / frame_in_match_s2c) end-to-end: the handshake (0x41/0x42),
// the SESSION-framed (0x43) reactive §5.1 reply path (dispatch_session_replies — handshake /
// server-info / mission-metadata / loadout / roster / spawn-confirm), and the join-leg validation
// (non-JO / HK / capacity / retransmit).
//
// This is the P2 port of tests/novaworld/host_session_accept_test.cpp, retargeted onto the promoted np
// free functions over a NapiNPServerCtx (connection state on NapiNPConnection; the reactive replies
// produced by server_message_dispatch off ctx.config, P8). The WORLD-driven spawn / F3-ordering
// flow is covered by client_runtime_test / initial_state_burst_test / two_endpoint_socket_test (which
// wire ctx.world); this World-less test asserts the reactive replies + the handshake legs.
//
// nw_encode_outbound is the client's exact inverse of the server's nw_decode_inbound, so it doubles as
// the joiner-side framer; the server's internally-generated SCRK is recovered from the ServerAuth reply,
// so the encrypted 0x83 replies decode without any test accessor.

#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/world/ammo_table_build.h>   // build_ammo_table / resolve_weapon_round_types
#include <runtime/inmatch/integrity_challenge_profile.h>
#include <net/npwire/lan_discovery.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/world/weapon_table_build.h> // build_weapon_table (the D-NET-141 armory resolve)

#include <formats/def/def.h>
#include <runtime/world/ai.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include "common/test_paths.h"
#include "common/retail_paths.h"
#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h> // LoopbackChannel (run_listen_host_lifecycle's host loopback)
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/ingame_decode.h> // WeaponLoadout / decode_weapon_loadout (the 0x5A reply check)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace opennova::def;

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

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
	       world.vehicles.process_attach(player, emplacement_out, gunner.bone_index);
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

uint32_t le32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) |
	       (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) |
	       (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> indexed_crc_reply(uint8_t index, uint32_t crc,
		std::initializer_list<uint8_t> trailing = {}) {
	std::vector<uint8_t> body = {
			index,
			static_cast<uint8_t>(crc),
			static_cast<uint8_t>(crc >> 8),
			static_cast<uint8_t>(crc >> 16),
			static_cast<uint8_t>(crc >> 24),
	};
	body.insert(body.end(), trailing.begin(), trailing.end());
	return body;
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

std::vector<uint8_t> retail_join_request(std::string_view expansion,
		std::string_view version_crc = "0") {
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
	append_string_tlv("VERSIONCRCSTRING", version_crc);
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

bool disconnect_event_equals(const DisconnectEvent &a, const DisconnectEvent &b) {
	return a.ds == b.ds && a.dc == b.dc && a.dp1 == b.dp1 && a.dp2 == b.dp2 &&
	       a.dstr == b.dstr && a.dpc == b.dpc && a.ddstr == b.ddstr;
}

// The host's teardown burst: `burst[first .. first+4)` must be four identical 0x86
// SERVER_GOODBYE datagrams whose NWU-decoded body is [le32 client_ck] followed by the seven
// disconnect-record TLVs decoding to `expected`. [orig: TeardownActiveConnection @0x6253C0 ->
// SendDisconnectPacket @0x61F2A0; recv_max_per_tick = 4 @0x4cab60]
bool check_server_goodbye_burst(const std::vector<std::vector<uint8_t>> &burst,
		std::size_t first, uint32_t client_ck, const DisconnectEvent &expected,
		const char *message) {
	if (!expect(burst.size() >= first + 4, message)) return false;
	for (std::size_t i = first; i < first + 4; ++i) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		DisconnectEvent decoded;
		if (!expect(nw_decode_inbound(burst[i].data(), burst[i].size(), opcode, body) &&
		                    opcode == SESSION_OPCODE_SERVER_GOODBYE && body.size() >= 4 &&
		                    le32(body.data()) == client_ck && burst[i] == burst[first],
		            "every 0x86 in the burst is the identical datagram keyed by the client CK"))
			return false;
		(void)parse_disconnect_event(body.data() + 4, body.size() - 4, decoded);
		if (!expect(disconnect_event_equals(decoded, expected),
		            "the 0x86 body carries the connection's latched disconnect record"))
			return false;
		if (!expect(body == server_goodbye_to_bytes(client_ck, expected),
		            "the 0x86 body is the from-scratch disconnect-packet writer's bytes"))
			return false;
	}
	return true;
}

// Complete the 0x41/0x42 handshake for `peer` and recover the server SCRK so the test can decode the
// encrypted 0x83 replies.
bool handshake(inmatch::NapiNPServerCtx &ctx, const PeerAddr &peer, std::string_view client_scrk,
               uint32_t client_ck, std::string &out_server_scrk,
               uint32_t *out_server_sk = nullptr,
               uint32_t *out_next_client_seq = nullptr,
               bool complete_game_admission = true,
               std::vector<ProtocolMessage> *out_post_handshake = nullptr) {
	const std::size_t connections_before_hello = inmatch::connection_count(ctx);
	ClientHello hello = make_jointoperations_client_hello(1);
	hello.co = "TestJoiner";
	auto hdg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = inmatch::handle_server_datagram(ctx, peer, hdg.data(), hdg.size(), 1);
	if (!expect(rh.outbound.size() == 1, "0x41 -> one ServerHello")) return false;
	if (!expect(rh.events.empty(), "0x41 is stateless and emits no peer event")) return false;
	if (!expect(inmatch::connection_count(ctx) == connections_before_hello,
	            "0x41 is stateless and creates no connection")) return false;

	ClientAuth auth = make_valid_client_auth(
			1, client_ck, kHostKey, "TestJoiner", client_scrk);
	auto adg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto ra = inmatch::handle_server_datagram(ctx, peer, adg.data(), adg.size(), 2);
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
	auto join_result = inmatch::handle_server_datagram(
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
	auto form_result = inmatch::handle_server_datagram(
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
	auto echo_result = inmatch::handle_server_datagram(
			ctx, peer, echo_dg.data(), echo_dg.size(), 5);
	if (!expect(
				!echo_result.outbound.empty() &&
						inmatch::connection_count(ctx) ==
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
	replication::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	config.server_name = "Configured LAN Host";
	config.game_type = 0x00010020u;
	config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	config.max_players = 11;
	config.spectator_slots = -1;
	config.spectator_password = "watch";
	config.expansion = "jox99";
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Lan,
	                        kHostKey, &loopback, config);

	const std::vector<uint8_t> probe = opennova::build_lan_discovery_probe(0x11223344u);
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
	const std::size_t before_scan = inmatch::connection_count(ctx);
	auto first = inmatch::handle_server_datagram(ctx, scanner, probe.data(), probe.size(), 1);
	if (!expect(first.outbound.size() == 1, "LAN discovery 0x41 receives one 0x81")) return false;
	if (!expect(first.events.empty(), "LAN discovery 0x41 emits no peer event")) return false;
	if (!expect(inmatch::connection_count(ctx) == before_scan,
	            "LAN discovery 0x41 does not register the scanner")) return false;

	opennova::LanDiscoveryServer found;
	if (!expect(opennova::parse_lan_discovery_reply(first.outbound[0].data(), first.outbound[0].size(), found),
	            "LAN discovery parser accepts the host 0x81")) return false;
	if (!expect(found.server_name == config.server_name, "0x81 SN reflects server_name")) return false;
	if (!expect(found.gametype == config.game_type, "0x81 P1 reflects gametype")) return false;
	if (!expect(found.current_players == 1, "0x81 NP counts the host loopback")) return false;
	if (!expect(found.max_players == config.max_players, "0x81 MP reflects max_players")) return false;
	if (!expect(found.server_flags == 0x00006904u,
	            "LAN discovery exposes retail spectator and password BuildFlags")) return false;
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
	if (!expect(foreign.p2 == 0x00006904u,
	            "0x81 P2 carries the live retail BuildFlags value")) return false;
	if (!expect(foreign.sus1.empty(),
	            "LAN 0x81 omits SUS1 when no NovaWorld session user string exists")) return false;
	foreign.pn = "NOVAWORLDUDP";
	const std::vector<uint8_t> foreign_reply = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(foreign));
	opennova::LanDiscoveryServer ignored;
	if (!expect(!opennova::parse_lan_discovery_reply(
	                    foreign_reply.data(), foreign_reply.size(), ignored),
	            "LAN discovery rejects a non-JO 0x81")) return false;

	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const PeerAddr joiner{0x0100007Fu, 32101};
	auto auth = craft_auth("JOINTOPERATIONS", kHostKey, 0x12345678u, scrk);
	auto joined = inmatch::handle_server_datagram(ctx, joiner, auth.data(), auth.size(), 2);
	if (!expect(!joined.outbound.empty(), "validated 0x42 admits the real joiner")) return false;
	if (!expect(inmatch::connection_count(ctx) == before_scan + 1,
	            "only 0x42 adds the remote connection")) return false;

	auto second = inmatch::handle_server_datagram(ctx, scanner, probe.data(), probe.size(), 3);
	opennova::LanDiscoveryServer refreshed;
	if (!expect(second.outbound.size() == 1 &&
	            opennova::parse_lan_discovery_reply(second.outbound[0].data(), second.outbound[0].size(), refreshed),
	            "repeated LAN discovery receives a parseable 0x81")) return false;
	if (!expect(refreshed.current_players == 2, "0x81 NP updates after admission")) return false;
	if (!expect(inmatch::connection_count(ctx) == before_scan + 1,
	            "repeated discovery still creates no connection")) return false;

	// Zero-valued gated fields are absent on the wire. The discovery parser
	// must still report the live zeroes rather than ServerHello builder defaults.
	inmatch::NapiNPServerCtx dedicated;
	inmatch::GameConfig dedicated_config;
	dedicated_config.server_name = "Empty Dedicated Host";
	dedicated_config.game_type = 0;
	dedicated_config.max_players = 4;
	dedicated_config.expansion.clear();
	// The captured install's rules word (TeamChoose spin persisted ON): the
	// P2 pin below reads the LIVE value, not the cfg default (0x3A02).
	dedicated_config.mp_attributes = 0x3A06u;
	inmatch::test::bring_up_host(dedicated, inmatch::ConnectionMode::HostOnly,
	                        inmatch::SocketMode::Lan, kHostKey, nullptr, dedicated_config);
	auto empty_reply = inmatch::handle_server_datagram(
			dedicated, scanner, probe.data(), probe.size(), 4);
	opennova::LanDiscoveryServer empty;
	if (!expect(empty_reply.outbound.size() == 1 &&
	            opennova::parse_lan_discovery_reply(empty_reply.outbound[0].data(),
	                                          empty_reply.outbound[0].size(), empty),
	            "empty dedicated host returns a parseable 0x81")) return false;
	if (!expect(empty.current_players == 0 && empty.gametype == 0 && empty.expansion.empty(),
	            "omitted NP/P1/SUS2 fields parse as live zero/empty values")) return false;
	uint8_t empty_opcode = 0;
	std::vector<uint8_t> empty_body;
	ServerHello empty_hello;
	if (!expect(
				nw_decode_inbound(
						empty_reply.outbound[0].data(), empty_reply.outbound[0].size(),
						empty_opcode, empty_body) &&
						empty_opcode == SESSION_OPCODE_SERVER_HELLO &&
						parse_server_hello(empty_body.data(), empty_body.size(), empty_hello) &&
						empty_hello.p2 == 0x00000904u,
				"Deathmatch LAN 0x81 still carries TeamChoose from live mp_attributes"))
		return false;
	if (!expect(inmatch::connection_count(dedicated) == 0,
	            "dedicated-host discovery remains stateless")) return false;
	return true;
}

// World-less: the reactive §5.1 reply path (dispatch_session_replies). No World wired => no spawn (the
// World-path spawn/F3 flow is covered by client_runtime_test / two_endpoint_socket_test). Asserts the
// handshake / server-info / mission-metadata / loadout / roster / spawn-confirm reactive replies a retail
// joiner expects.
// The PR #403 retail->retail 00TRg witness pins both metadata files consumed
// before the joiner chooses its faction/loadout. For this stock-Co-op selector,
// 0x60 names the map file (not MissionText's display title) and carries the
// configured loading-screen text; the cross-mode selector is pinned separately.
// 0x64 is the original 180-byte CNapiGameSession block: two per-session random
// 32-byte regions around the live cap/game-type/mpattrib fields and three fixed
// 32-byte strings. Re-requests must return the same per-session block.
bool run_mission_transfers_match_retail_lan_contract() {
	replication::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	config.server_name = "Untitled ";
	config.mission_name = "Training: Grenade Launcher";
	config.mission_file = "00TRg.bms";
	config.custom_text = "Put your message here.";
	config.game_type = 0x00010020u;
	config.mp_attributes = 0x00003A06u;
	config.max_players = 4;
	config.expansion = "revx02";
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                        inmatch::SocketMode::Lan, kHostKey, &loopback, config);

	const PeerAddr peer{0x0100007Fu, 30064};
	const std::string client_scrk =
			"MISSIONMETADATASCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
	std::string server_scrk;
	uint32_t server_sk = 0;
	uint32_t seq = 1;
	if (!handshake(ctx, peer, client_scrk, 0x40306400u, server_scrk,
	               &server_sk, &seq)) {
		return false;
	}

	auto request = [&](uint8_t tag, uint32_t now,
	                   ProtocolMessage &out) -> bool {
		const std::vector<uint8_t> dg = craft_session(
				client_scrk, server_sk, seq++, {make_protocol_message(tag, {})});
		const auto result = inmatch::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), now);
		if (result.outbound.empty()) return false;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_s2c(result.outbound.back(), server_scrk, header, messages))
			return false;
		const ProtocolMessage *found = find_reply(
				messages, tag == 0x33 ? uint8_t(0x60) : uint8_t(0x64));
		if (found == nullptr) return false;
		out = *found;
		return true;
	};

	ProtocolMessage info_message;
	if (!expect(request(0x33, 110, info_message),
	            "0x33 returns the retail server-info transfer")) return false;
	FileTransferChunk info;
	if (!expect(decode_file_transfer_chunk(
	                    info_message.payload.data(), info_message.payload.size(), info) &&
	                    info.transfer_id == 1 && info.chunk_offset == 0 && info.is_final(),
	            "0x60 is one complete transfer")) return false;
	std::vector<uint8_t> expected_info;
	auto append_u32 = [&](uint32_t value) {
		for (unsigned shift = 0; shift < 32; shift += 8)
			expected_info.push_back(static_cast<uint8_t>(value >> shift));
	};
	auto append_kv = [&](const char *name, const uint8_t *bytes, std::size_t size) {
		expected_info.insert(expected_info.end(), name,
		                     name + std::char_traits<char>::length(name));
		expected_info.push_back(0);
		append_u32(static_cast<uint32_t>(size));
		expected_info.insert(expected_info.end(), bytes, bytes + size);
	};
	auto append_string_kv = [&](const char *name, const std::string &value) {
		append_kv(name, reinterpret_cast<const uint8_t *>(value.c_str()),
		          value.size() + 1);
	};
	append_string_kv("SERVERNAME", config.server_name);
	append_string_kv("MISSIONNAME", config.mission_file);
	const uint8_t game_type[] = {0x20, 0x00, 0x01, 0x00};
	append_kv("GAMETYPE", game_type, sizeof(game_type));
	append_string_kv("CUSTOMTEXT", config.custom_text);
	append_string_kv("MISSIONFILENAME", config.mission_file);
	const uint8_t fanfare[] = {0, 0};
	append_kv("EXP_FANFARE", fanfare, sizeof(fanfare));
	if (!expect(info.total_size == expected_info.size() &&
	                    std::equal(expected_info.begin(), expected_info.end(), info.chunk_data),
	            "0x60 bytes match the retail 00TRg VarList")) return false;

	ProtocolMessage first_metadata;
	ProtocolMessage repeated_metadata;
	if (!expect(request(0x37, 120, first_metadata) &&
	                    request(0x37, 121, repeated_metadata) &&
	                    first_metadata.payload == repeated_metadata.payload,
	            "0x64 re-request returns one stable per-session block")) return false;
	FileTransferChunk metadata;
	if (!expect(decode_file_transfer_chunk(
	                    first_metadata.payload.data(), first_metadata.payload.size(), metadata) &&
	                    metadata.transfer_id == 1 && metadata.total_size == 180 &&
	                    metadata.chunk_offset == 0 && metadata.chunk_size == 180 &&
	                    metadata.is_final(),
	            "0x64 wraps the exact 180-byte mission block")) return false;
	const uint8_t *blob = metadata.chunk_data;
	const auto fixed_string = [&](std::size_t offset) {
		return std::string(reinterpret_cast<const char *>(blob + offset));
	};
	const bool prefix_nonzero = std::any_of(
			blob, blob + 32, [](uint8_t b) { return b != 0; });
	const bool suffix_nonzero = std::any_of(
			blob + 148, blob + 180, [](uint8_t b) { return b != 0; });
	if (!expect(prefix_nonzero && le32(blob + 32) != 0 && suffix_nonzero,
	            "0x64 carries generated per-session seed/id/token fields")) return false;
	return expect(
			le32(blob + 36) == config.max_players &&
			le32(blob + 40) == config.game_type &&
			le32(blob + 44) == config.mp_attributes && le32(blob + 48) == 1 &&
			fixed_string(52) == config.server_name &&
			fixed_string(84) == config.mission_file &&
			fixed_string(116) == config.mission_file,
			"0x64 live fields and strings match the retail LAN session");
}

bool decode_tag60_mission_strings(const ProtocolMessage &message,
		std::string &mission_name, std::string &mission_filename) {
	FileTransferChunk transfer;
	if (!decode_file_transfer_chunk(
			message.payload.data(), message.payload.size(), transfer) ||
			!transfer.is_final())
		return false;
	const uint8_t *cursor = transfer.chunk_data;
	const uint8_t *end = cursor + transfer.chunk_size;
	while (cursor < end) {
		const uint8_t *name_end = std::find(cursor, end, uint8_t{0});
		if (name_end == end) return false;
		const std::string key(
				reinterpret_cast<const char *>(cursor),
				reinterpret_cast<const char *>(name_end));
		cursor = name_end + 1;
		if (static_cast<std::size_t>(end - cursor) < 4u) return false;
		const uint32_t value_size = le32(cursor);
		cursor += 4;
		if (value_size > static_cast<std::size_t>(end - cursor)) return false;
		if ((key == "MISSIONNAME" || key == "MISSIONFILENAME") &&
				(value_size == 0 || cursor[value_size - 1] != 0))
			return false;
		if (key == "MISSIONNAME") {
			mission_name.assign(
					reinterpret_cast<const char *>(cursor),
					value_size == 0 ? 0u : value_size - 1u);
		} else if (key == "MISSIONFILENAME") {
			mission_filename.assign(
					reinterpret_cast<const char *>(cursor),
					value_size == 0 ? 0u : value_size - 1u);
		}
		cursor += value_size;
	}
	return !mission_name.empty() && !mission_filename.empty();
}

bool run_tag60_mission_name_selects_by_game_type() {
	struct Case {
		uint32_t game_type;
		const char *title;
		const char *file;
		const char *expected_mission_name;
		const char *description;
	};
	const Case cases[] = {
			{0x00010020u, "Stock Co-op Display Title", "COOP_FILE.BMS",
					"COOP_FILE.BMS",
					"stock Co-op 0x60 uses the active mission filename"},
			{0x00030020u, "Objective Co-op Display Title", "OBJECTIVE_FILE.BMS",
					"Objective Co-op Display Title",
					"objective waypoint variant 0x60 uses MissionText title"},
			{0x00010000u, "Retail TDM Display Title", "TDM_FILE.BMS",
					"Retail TDM Display Title",
					"TDM 0x60 uses MissionText title"},
	};

	uint16_t port = 30120;
	uint32_t client_key = 0x60000001u;
	for (const Case &test_case : cases) {
		replication::LoopbackChannel loopback;
		inmatch::NapiNPServerCtx ctx;
		inmatch::GameConfig config;
		config.server_name = "Tag60 Selector Host";
		config.mission_name = test_case.title;
		config.mission_file = test_case.file;
		config.game_type = test_case.game_type;
		config.max_players = 4;
		inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
				inmatch::SocketMode::Lan, kHostKey, &loopback, config);

		const PeerAddr peer{0x0100007Fu, port++};
		const std::string client_scrk =
				"TAG60SELECTORSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567";
		std::string server_scrk;
		uint32_t server_sk = 0;
		uint32_t sequence = 1;
		if (!handshake(ctx, peer, client_scrk, client_key++, server_scrk,
				&server_sk, &sequence))
			return false;

		const std::vector<uint8_t> request = craft_session(
				client_scrk, server_sk, sequence,
				{make_protocol_message(0x33, {})});
		const auto result = inmatch::handle_server_datagram(
				ctx, peer, request.data(), request.size(), 130);
		if (!expect(!result.outbound.empty(),
				"0x33 selector request returns a server-info transfer"))
			return false;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_s2c(result.outbound.back(), server_scrk, header, messages))
			return expect(false, "decode selector 0x60 response");
		const ProtocolMessage *info = find_reply(messages, 0x60);
		if (!expect(info != nullptr, "selector response contains S2C 0x60"))
			return false;
		std::string mission_name;
		std::string mission_filename;
		if (!expect(decode_tag60_mission_strings(
					*info, mission_name, mission_filename),
				"selector response contains both mission string fields"))
			return false;
		if (!expect(mission_name == test_case.expected_mission_name,
				test_case.description))
			return false;
		if (!expect(mission_filename == test_case.file,
				"MISSIONFILENAME remains the exact active map file"))
			return false;
	}
	return true;
}

bool run_reactive_replies() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);

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
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		if (r.outbound.empty()) return false;
		ProtocolPacketHeader hdr;
		return decode_s2c(r.outbound.back(), server_scrk, hdr, out);
	};

	// The helper completed the admission exchange, so a later C2S 0x02 is a no-op.
	// Verify it doesn't crash or double-emit admission metadata.
	{
		std::vector<ProtocolMessage> msgs;
		send_session({make_protocol_message(0x02, std::vector<uint8_t>(8, 0))}, 100, msgs);
		// no assertion on reply content — the burst was already sent with the 0x82 ServerAuth
	}
	// 0x33 -> 0x60 server-info chunk. [orig: NapiNPServerMsg_HandleReplayRequest (0x33) @0x515230]
	{
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x33, {})}, 110, msgs) &&
		            reply_has_tag(msgs, 0x60), "0x33 -> 0x60 server-info")) return false;
	}
	// 0x37 -> 0x64 mission metadata chunk only. Retail's handler calls the
	// circular-buffer sender and does not emit a fresh player-slot state.
	// [orig: NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0]
	{
		std::vector<ProtocolMessage> msgs;
		if (!expect(send_session({make_protocol_message(0x37, {})}, 120, msgs) &&
		            reply_has_tag(msgs, 0x64) && !reply_has_tag(msgs, 0x75),
		            "0x37 -> 0x64 only, without an invented 0x75")) return false;
	}
	// 0x2F -> 0x5A weapon loadout, DERIVED from the request [orig: NapiNPServerMsg_HandlePlayerLoadout
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
		// Delivery policy is sender-owned metadata and is intentionally absent from the wire
		// encoding, so verify one-send behavior against the host's retained queue instead of
		// the decoded message (which necessarily carries ProtocolMessage's default policy).
		bool pong_retained = false;
		for (const inmatch::NapiNPConnection &connection : ctx.np_protocol.connection_list) {
			if (!(connection.peer == peer)) continue;
			for (const auto &[packet_sequence, retained] : connection.seq.retained_outbound) {
				(void)packet_sequence;
				for (const ProtocolMessage &message : retained)
					if (message.tag == 0x57) pong_retained = true;
			}
		}
		if (!expect(body_ok && !pong_retained,
		            "0x57 pong is one-send with echoed timestamp + 0 byte")) return false;
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
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);

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
	inmatch::handle_server_datagram(ctx, peer, mission_dg.data(), mission_dg.size(), 100);
	if (!expect(inmatch::bind_connection_player(ctx, peer, 1, 0x0005),
	            "server binds the connection to its allocated player entity handle")) return false;

	auto spawn_req = craft_session(
			client_scrk, server_sk, seq++,
			{make_protocol_message(0x29, {0x00, 0x00})});
	auto spawn_r = inmatch::handle_server_datagram(ctx, peer, spawn_req.data(), spawn_req.size(), 200);
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
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);

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
	if (!expect(inmatch::bind_connection_player(ctx, peer, 1, h.packed),
	            "connection binds the live world entity")) return false;

	// The goodbye carries the receiver-local server key. Empty, truncated, wrong-key, and stale
	// prior-session packets must not tear down the live connection, and none of them draws
	// the host's 0x86 burst.
	auto malformed_bye = craft(SESSION_OPCODE_CLIENT_GOODBYE, {});
	auto malformed_result = inmatch::handle_server_datagram(
			ctx, peer, malformed_bye.data(), malformed_bye.size(), 398);
	if (!expect(w.registry.get(h) != nullptr && inmatch::connection_count(ctx) == 1 &&
	                    malformed_result.outbound.empty(),
	            "empty goodbye body is ignored")) return false;
	auto wrong_bye = craft(
			SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(server_sk ^ 0x01010101u));
	auto wrong_result = inmatch::handle_server_datagram(
			ctx, peer, wrong_bye.data(), wrong_bye.size(), 399);
	if (!expect(w.registry.get(h) != nullptr && inmatch::connection_count(ctx) == 1 &&
	                    wrong_result.outbound.empty(),
	            "wrong receiver-local goodbye key is ignored")) return false;

	// The retail client's leave record {2, 2, 0, 0, "I.C:CIDEMIS", 0, ""}: the host latches
	// it with the peer role 2 and its answering 0x86 burst echoes it field for field.
	// [orig: Nwu_HandleDisconnect @0x62400d..0x6240ba; TeardownActiveConnection @0x6253ef]
	const DisconnectEvent leave = make_disconnect_event(2, 2, 0, 0, "I.C:CIDEMIS", 0, "");
	auto bye = craft(SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(server_sk, leave));
	auto bye_result = inmatch::handle_server_datagram(ctx, peer, bye.data(), bye.size(), 400);
	if (!expect(bye_result.outbound.size() == 4 &&
	                    check_server_goodbye_burst(bye_result.outbound, 0, 0xC0FFEE01u, leave,
	                            "a keyed goodbye is answered by four 0x86 echoing its record"),
	            "the goodbye reply is exactly the 0x86 burst")) return false;
	if (!expect(bye_result.events.size() == 1 &&
	                    bye_result.events[0].kind == inmatch::HostAcceptEvent::Kind::PeerGoodbye,
	            "the goodbye surfaces owner cleanup")) return false;

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
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
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
	                    inmatch::bind_connection_player(ctx, peer, 1, old_entity.packed),
	            "endpoint-reuse fixture binds the old entity")) return false;
	world::EntityHandle emplacement;
	if (!expect(
				mount_on_test_emplacement(w, old_entity, emplacement) &&
						test_emplacement_is_owned_by(
								w, emplacement, old_entity),
				"endpoint-reuse fixture owns the emplaced-gun seat and control latches")) {
		return false;
	}
	for (inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		conn.phase = inmatch::ConnectionPhase::PlayerAdded;
		conn.burst.spawned = true;
		conn.reply.roster_counted = true;
	}
	const uint32_t roster_before = ctx.np_protocol.roster_generation;

	ClientAuth replacement = make_valid_client_auth(
			2, 0xC0FFEE22u, kHostKey, "ReplacementJoiner",
			"NEWCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD");
	auto replacement_dg = craft(
			SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(replacement));
	auto result = inmatch::handle_server_datagram(
			ctx, peer, replacement_dg.data(), replacement_dg.size(), 500);

	if (!expect(!result.outbound.empty() && inmatch::connection_count(ctx) == 1,
	            "replacement endpoint is admitted as one fresh connection")) return false;
	// The old node is Destroyed first: its 0x86 burst (nothing latched => a zero record) goes
	// to the shared endpoint keyed by the OLD CK, ahead of the replacement's 0x82; the new
	// joiner's own key check drops it. [orig: HandleClientJoin @0x62befb Destroy -> Create]
	if (!expect(result.outbound.size() >= 5 &&
	                    check_server_goodbye_burst(result.outbound, 0, 0xC0FFEE11u,
	                            DisconnectEvent{},
	                            "endpoint replacement bursts 0x86 to the old occupant"),
	            "the old occupant's burst precedes the replacement's admission")) return false;
	{
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(result.outbound[4].data(), result.outbound[4].size(),
		                    op, body) && op == SESSION_OPCODE_SERVER_AUTH,
		            "the replacement's 0x82 follows the old occupant's burst")) return false;
	}
	if (!expect(w.registry.get(old_entity) == nullptr,
	            "endpoint replacement despawns the old authoritative entity")) return false;
	if (!expect(
				test_emplacement_is_released(w, emplacement),
				"endpoint replacement releases the emplaced-gun seat and control latches")) {
		return false;
	}
	if (!expect(ctx.np_protocol.roster_generation == roster_before + 1,
	            "endpoint replacement advances the roster generation")) return false;
	const inmatch::NapiNPConnection &fresh = ctx.np_protocol.connection_list.front();
	if (!expect(fresh.client_ci == replacement.ci && fresh.client_ck == replacement.ck &&
	                    fresh.player_name == replacement.na &&
	                    !fresh.link.owned_entity.valid(),
	            "replacement carries only the new session identity and no stale binding")) return false;
	return true;
}

// Retail's JO connection profile reaps an otherwise-valid peer after 120000 ms of receive
// inactivity. The reap must take the same complete entity/roster teardown path as keyed goodbye.
bool run_inactive_peer_is_reaped() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
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
	if (!expect(entity.valid() && inmatch::bind_connection_player(ctx, peer, 1, entity.packed),
	            "idle-reap fixture binds the player entity")) return false;
	world::EntityHandle emplacement;
	if (!expect(
				mount_on_test_emplacement(w, entity, emplacement) &&
						test_emplacement_is_owned_by(w, emplacement, entity),
				"idle-reap fixture owns the emplaced-gun seat and control latches")) {
		return false;
	}
	for (inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		conn.phase = inmatch::ConnectionPhase::PlayerAdded;
		conn.burst.spawned = true;
		conn.reply.roster_counted = true;
		conn.receive_inactive_ms = 119999;
	}

	// A packet can decrypt with the peer SCRK yet name the wrong receiver-local SK. Deframing
	// classifies it as unadmitted; it must not refresh the live connection's receive clock.
	auto wrong_receiver = craft_session(
			scrk, server_sk ^ 0x01010101u, 4, {});
	auto ignored = inmatch::handle_server_datagram(
			ctx, peer, wrong_receiver.data(), wrong_receiver.size(), 6);
	if (!expect(ignored.outbound.empty(), "wrong receiver-local 0x43 is silently ignored"))
		return false;
	if (!expect(
				ctx.np_protocol.connection_list.front().receive_inactive_ms == 119999,
				"wrong receiver-local 0x43 does not reset the inactivity clock")) {
		return false;
	}
	// The reap clock is stamped ONLY by an in-order delivered session packet (retail's
	// ParseMessages, reached solely for seq == ack+1 or the gap-closing drain) or a ping:
	// a duplicate of an admitted 0x43, a future 0x43, a valid 0x44 resend list and a
	// retransmitted identical 0x42 all leave it alone; an in-order zero-message 0x43 resets it.
	// [orig: HandleSessionPacket @0x626A00 dup @0x626c03 / future @0x626c0c..0x626c3a;
	//  NapiNP_HandleResendList @0x62395f; HandleClientJoin @0x62bee6..0x62bef1;
	//  ParseMessages @0x625d54]
	const auto &live = ctx.np_protocol.connection_list.front();
	const uint32_t admitted_seq = live.seq.last_inbound_seq; // the handshake's last C2S seq
	auto duplicate = craft_session(scrk, server_sk, admitted_seq, {});
	inmatch::handle_server_datagram(ctx, peer, duplicate.data(), duplicate.size(), 6);
	if (!expect(live.receive_inactive_ms == 119999,
	            "a duplicate of an already-admitted 0x43 does not reset the inactivity clock"))
		return false;
	auto future = craft_session(scrk, server_sk, admitted_seq + 2, {});
	inmatch::handle_server_datagram(ctx, peer, future.data(), future.size(), 6);
	if (!expect(live.receive_inactive_ms == 119999 && live.seq.queued_inbound.size() == 1,
	            "a queued future 0x43 does not reset the inactivity clock"))
		return false;
	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(server_sk, {1}, resend_body),
	            "idle-reap fixture encodes a keyed 0x44")) return false;
	auto resend = craft(SESSION_OPCODE_CLIENT_RESEND_LIST, std::move(resend_body));
	auto resend_result = inmatch::handle_server_datagram(
			ctx, peer, resend.data(), resend.size(), 6);
	if (!expect(!resend_result.outbound.empty() && live.receive_inactive_ms == 119999,
	            "a valid 0x44 resend list is answered but does not reset the inactivity clock"))
		return false;
	{
		ClientAuth retransmit = make_valid_client_auth(
				1, 0xC0FFEE33u, kHostKey, "TestJoiner", scrk);
		auto retransmit_dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(retransmit));
		auto retransmit_result = inmatch::handle_server_datagram(
				ctx, peer, retransmit_dg.data(), retransmit_dg.size(), 6);
		if (!expect(!retransmit_result.outbound.empty() && inmatch::connection_count(ctx) == 1 &&
		                    ctx.np_protocol.connection_list.front().receive_inactive_ms == 119999,
		            "a retransmitted identical 0x42 re-sends the 0x82 but does not reset the clock"))
			return false;
	}
	auto in_order = craft_session(scrk, server_sk, admitted_seq + 1, {});
	inmatch::handle_server_datagram(ctx, peer, in_order.data(), in_order.size(), 6);
	if (!expect(ctx.np_protocol.connection_list.front().receive_inactive_ms == 0 &&
	                    ctx.np_protocol.connection_list.front().seq.queued_inbound.empty(),
	            "an in-order zero-message 0x43 resets the inactivity clock (and drains the gap)"))
		return false;
	ctx.np_protocol.connection_list.front().receive_inactive_ms = 119999;

	const std::vector<inmatch::TickOut> outs =
			inmatch::tick_connections(ctx, /*elapsed_ms=*/2, /*now_tick=*/7);
	bool saw_goodbye = false;
	bool saw_burst = false;
	for (const inmatch::TickOut &out : outs) {
		for (const inmatch::HostAcceptEvent &event : out.events)
			if (event.kind == inmatch::HostAcceptEvent::Kind::PeerGoodbye &&
			    event.peer == peer)
				saw_goodbye = true;
		// The reap latches {1, 3, elapsed, timeout, "", 0, "NP.C:PT:SERTMOUT"} and the
		// destroy bursts it to the dead endpoint before the player teardown.
		// [orig: PumpStateMachine @0x6293b7..0x6293e6 -> RequestDisconnect @0x62940a;
		//  NapiNPProtocol_Pump @0x62a793 -> Destroy -> TeardownActiveConnection @0x6253ef]
		if (out.peer == peer && out.outbound.size() == 4 &&
		    check_server_goodbye_burst(out.outbound, 0, 0xC0FFEE33u,
		            make_disconnect_event(1, 3, 120001, 120000, "", 0, "NP.C:PT:SERTMOUT"),
		            "the reap bursts four 0x86 carrying the SERTMOUT record"))
			saw_burst = true;
	}
	if (!expect(saw_goodbye, "inactivity reap surfaces owner cleanup for the dead endpoint"))
		return false;
	if (!expect(saw_burst, "inactivity reap sends the 0x86 SERTMOUT burst to the dead endpoint"))
		return false;
	if (!expect(inmatch::connection_count(ctx) == 0,
	            "120000 ms inactive peer is removed from the connection list")) return false;
	if (!expect(w.registry.get(entity) == nullptr,
	            "inactivity reap despawns the authoritative player entity")) return false;
	return expect(
			test_emplacement_is_released(w, emplacement),
			"inactivity reap releases the emplaced-gun seat and control latches");
}

// The disconnect teardown drops the leaver from every spawn-wave row it was queued in, before
// the entity goes: a stale row entry would restart that row's countdown on release and, because
// pool-0 slots are reused, force-deploy the slot's next occupant at the leaver's zone.
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 -> SpawnWaveList_RemovePlayer @0x52A410 @0x51b809]
bool run_disconnect_removes_leaver_from_spawn_waves() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
	world::World w;
	w.registry.configure_pool(0, 8);
	w.registry.configure_pool(2, 8);
	world::Entity player;
	player.kind = world::EntityKind::Organic;
	player.team = 1;
	player.alive = false;
	player.health = 0;
	player.health_max = 100;
	const world::EntityHandle h = w.registry.spawn(0, player);
	world::Entity zone;
	zone.kind = world::EntityKind::Item;
	zone.team = 1;
	zone.alive = true;
	zone.is_spawn_point = true;
	zone.zone_number = 1;
	zone.zone_control = 0x10000;
	const world::EntityHandle zone_handle = w.registry.spawn(2, zone);
	w.zones.spawn_waves.build_from_mission(w, 0, 2);
	if (!expect(h.valid() && w.zones.spawn_waves.has_entry(zone_handle),
	            "spawn-wave fixture builds one numbered-zone row")) return false;
	ctx.world = &w;

	const PeerAddr peer{0x0100007Fu, 30730};
	const std::string client_scrk = "WAVECLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABC";
	std::string server_scrk;
	uint32_t server_sk = 0;
	if (!handshake(ctx, peer, client_scrk, 0xC0FFEE44u, server_scrk, &server_sk)) return false;
	if (!expect(inmatch::bind_connection_player(ctx, peer, 1, h.packed),
	            "spawn-wave fixture binds the player entity")) return false;
	if (!expect(w.zones.spawn_waves.try_queue(w, zone_handle, h) &&
	                    w.zones.spawn_waves.entries()[0].queued.size() == 1 &&
	                    w.zones.spawn_waves.entries()[0].queued[0] == h,
	            "the player queues into the zone's spawn-wave row")) return false;

	auto bye = craft(SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(server_sk));
	inmatch::handle_server_datagram(ctx, peer, bye.data(), bye.size(), 400);
	if (!expect(inmatch::connection_count(ctx) == 0 && w.registry.get(h) == nullptr,
	            "the goodbye tears the player down")) return false;
	for (const world::SpawnWaveEntry &entry : w.zones.spawn_waves.entries()) {
		if (!expect(std::find(entry.queued.begin(), entry.queued.end(), h) == entry.queued.end(),
		            "the leaver is removed from every spawn-wave row")) return false;
	}
	// The freed pool-0 slot's next occupant does not inherit the queue position.
	const world::EntityHandle reused = w.registry.spawn(0, player);
	if (!expect(reused.valid() && reused.slot() == h.slot(),
	            "the registry reuses the freed pool-0 slot")) return false;
	return expect(w.zones.spawn_waves.entries()[0].queued.empty() &&
	                      w.zones.spawn_waves.tick(w).empty(),
	              "a later occupant of the slot is not released by the stale row entry");
}

// A `_NSTMOUT.TXT` NEVER on the host sets the connection template to -1/-1: the 0x82 advertises
// both (CS field 0 / field 11 as 0xFFFFFFFF), the created node copies them, the receive reap
// never fires and the outbound pool is unbounded. [orig: CNapiNetwork_Init @0x4caa13..0x4caa22
//  -> cs_dir stores @0x4caa81/@0x4cab20/@0x4cab54/@0x4cabf0; SendSessionInit @0x620ef0 emits the
//  live blocks; PumpStateMachine `timeout_ms < 0 -> skip` @0x62934c; NapiNPMessage_Create
//  `msg_out_max >= 0` @0x628048]
bool run_never_template_disables_reap_and_is_advertised() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
	inmatch::parse_nstmout("never please", ctx.np_protocol.connection_template);
	if (!expect(ctx.np_protocol.connection_template.timeout_ms == -1 &&
	                    ctx.np_protocol.connection_template.msg_out_max == -1,
	            "NEVER sets both template values to -1")) return false;

	const PeerAddr peer{0x0100007Fu, 30740};
	const std::string client_scrk = "NEVRCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABC";
	ClientHello hello = make_jointoperations_client_hello(1);
	auto hdg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	(void)inmatch::handle_server_datagram(ctx, peer, hdg.data(), hdg.size(), 1);
	ClientAuth auth = make_valid_client_auth(1, 0xC0FFEE55u, kHostKey, "TestJoiner", client_scrk);
	auto adg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto ra = inmatch::handle_server_datagram(ctx, peer, adg.data(), adg.size(), 2);
	uint8_t op = 0;
	std::vector<uint8_t> body;
	ServerAuth sa;
	if (!expect(!ra.outbound.empty() &&
	                    nw_decode_inbound(ra.outbound[0].data(), ra.outbound[0].size(), op, body) &&
	                    op == SESSION_OPCODE_SERVER_AUTH && parse_server_auth(body.data(), body.size(), sa),
	            "NEVER host still admits the join")) return false;
	bool saw_timeout = false, saw_pool = false;
	for (const std::vector<CsField> *block : {&sa.client_cs, &sa.server_cs}) {
		for (const CsField &field : *block) {
			if (field.field_index == 0) saw_timeout = field.value == 0xFFFFFFFFu;
			if (field.field_index == 11) saw_pool = field.value == 0xFFFFFFFFu;
		}
	}
	if (!expect(saw_timeout && saw_pool,
	            "the 0x82 CS block advertises the -1 timeout and pool bound in both directions"))
		return false;
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	if (!expect(conn.timeouts.timeout_ms == -1 && conn.timeouts.msg_out_max == -1 &&
	                    conn.seq.outbound_message_limit == 0,
	            "the created node copies the template and runs an unbounded pool")) return false;
	conn.receive_inactive_ms = 10u * 60u * 1000u;
	const std::vector<inmatch::TickOut> outs = inmatch::tick_connections(ctx, 2, 7);
	for (const inmatch::TickOut &out : outs)
		for (const inmatch::HostAcceptEvent &event : out.events)
			if (!expect(event.kind != inmatch::HostAcceptEvent::Kind::PeerGoodbye,
			            "a -1 timeout never reaps")) return false;
	return expect(inmatch::connection_count(ctx) == 1,
	              "the silent peer survives ten minutes under a NEVER template");
}

bool run_game_environment_and_admission_fsm_are_enforced() {
	const std::string scrk =
			"FSMCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABC";

	// These literals are checked at game JOIN, after the NP connection exists.
	struct BadField { const char *name; const char *value; uint32_t reason; };
	for (const BadField bad : {
			BadField{"BN", "0", 2}, {"VN", "3", 3}, {"MBN", "20042001", 4},
			{"SOPD", "179", 8}, {"BT", "1", 6}, {"BT", "2", 7}, {"", "", 2}}) {
		inmatch::NapiNPServerCtx ctx;
		inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Lan, kHostKey);
		ClientAuth auth = make_valid_client_auth(1, 0xA0000000u,
				kHostKey, "BadEnvironment", scrk);
		if (*bad.name) auth.cu.push_back(make_client_cu_chunk(2, bad.name, bad.value));
		else auth.cu.clear();
		const PeerAddr peer{0x0100007Fu, 31300};
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		auto result = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
		if (!expect(result.outbound.size() == 2 && inmatch::connection_count(ctx) == 1,
				"CU compatibility values do not reject the 0x42 handshake")) return false;
		replication::UdpSessionTransport transport(replication::UdpSessionTransport::Role::Host);
		auto &conn = ctx.np_protocol.connection_list.front();
		conn.link.transport = &transport;
		dg = craft_session(scrk, conn.server_sk, 1,
				{make_protocol_message(0x00, retail_join_request(ctx.config.expansion))});
		result = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 2);
		replication::Datagram staged;
		DisconnectEvent event;
		if (!expect(result.outbound.empty() && inmatch::connection_count(ctx) == 1 &&
				conn.admission_stage == inmatch::GameAdmissionStage::Rejected &&
				transport.pop_outbound(staged) && parse_disconnect_event(staged.body.data(), staged.body.size(), event) &&
				event.ds == 1 && event.dc == 2 && event.dpc == bad.reason,
				"game JOIN retains the connection and stages the exact compatibility DPC")) return false;
		conn.link.transport = nullptr;
	}

	// A form post cannot skip the JOIN request. The protocol violation tears
	// down the pending node, so it occupies neither a player slot nor an entity.
	{
		inmatch::NapiNPServerCtx ctx;
		inmatch::test::bring_up_host(
				ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
		world::World w;
		world::AiSystem &ai = w.ai;
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
		inmatch::tick_connections(ctx, 16, 10);
		if (!expect(
					!ctx.np_protocol.connection_list.front().link.owned_entity.valid(),
					"0x42 alone cannot spawn an authoritative entity")) {
			return false;
		}

		auto dg = craft_session(
				scrk, server_sk, 1,
				{make_protocol_message(0x01, {0x00})});
		auto result = inmatch::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), 11);
		if (!expect(
					result.outbound.empty() && inmatch::connection_count(ctx) == 0,
					"out-of-order 0x01 is dropped and releases the pending admission")) {
			return false;
		}
	}

	// The JOIN request itself is structural: the golden base-game request is
	// VERSIONCRCSTRING="0". A malformed body cannot advance the FSM.
	{
		inmatch::NapiNPServerCtx ctx;
		inmatch::test::bring_up_host(
				ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
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
		auto result = inmatch::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), 12);
		if (!expect(
					result.outbound.empty() && inmatch::connection_count(ctx) == 0,
					"malformed 0x00 JOIN is dropped and releases the pending admission")) {
			return false;
		}
	}

	// D-NET-166: the expansion version-checksum gate. An EXPANSION host compares
	// atol(VERSIONCRCSTRING) against its own g_expansion_checksum and rejects a
	// mismatch; a matching nonzero (and negative — "%ld" of a bit-31 CRC) value
	// admits [orig: Server_ValidatePlayerJoinRequest — gate @0x51231e, compare
	// @0x512331, reject DPC=48 @0x512341].
	{
		inmatch::NapiNPServerCtx ctx;
		inmatch::test::bring_up_host(
				ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
		ctx.config.expansion_version_checksum = -559038737; // 0xDEADBEEF as i32
		const PeerAddr peer{0x0100007Fu, 31335};
		std::string server_scrk;
		uint32_t server_sk = 0;
		if (!handshake(
					ctx, peer, scrk, 0xA0000015u, server_scrk,
					&server_sk, nullptr, false)) {
			return false;
		}
		// The stale-checksum join ("0" against a host that carries a real CRC).
		auto dg = craft_session(
				scrk, server_sk, 1,
				{make_protocol_message(
						0x00, retail_join_request(ctx.config.expansion))});
		auto result = inmatch::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), 12);
		if (!expect(
					result.outbound.empty() && inmatch::connection_count(ctx) == 0,
					"a mismatched expansion version checksum is rejected [orig: @0x512341]")) {
			return false;
		}
		// The matching signed-decimal value admits (a fresh handshake — the
		// mismatch above tore the pending node down).
		std::string server_scrk2;
		uint32_t server_sk2 = 0;
		if (!handshake(
					ctx, peer, scrk, 0xA0000016u, server_scrk2,
					&server_sk2, nullptr, false)) {
			return false;
		}
		auto ok_dg = craft_session(
				scrk, server_sk2, 1,
				{make_protocol_message(
						0x00, retail_join_request(
								ctx.config.expansion, "-559038737"))});
		auto ok_result = inmatch::handle_server_datagram(
				ctx, peer, ok_dg.data(), ok_dg.size(), 13);
		ProtocolPacketHeader crc_hdr;
		std::vector<ProtocolMessage> crc_replies;
		if (!expect(
					ok_result.outbound.size() == 1 &&
							decode_s2c(ok_result.outbound.front(), server_scrk2,
									crc_hdr, crc_replies) &&
							crc_replies.size() == 1 && crc_replies.front().tag == 0x00,
					"the matching signed-decimal checksum is acknowledged [orig: @0x512331]")) {
			return false;
		}
	}

	// D-NET-166: a BASE-GAME host (no active expansion) never runs the compare —
	// retail stores the TLV without reading it [orig: the g_ExpansionName[0]
	// gate @0x51231e].
	{
		inmatch::NapiNPServerCtx ctx;
		inmatch::test::bring_up_host(
				ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
		ctx.config.expansion.clear();
		ctx.config.expansion_version_checksum = 0;
		const PeerAddr peer{0x0100007Fu, 31336};
		std::string server_scrk;
		uint32_t server_sk = 0;
		if (!handshake(
					ctx, peer, scrk, 0xA0000017u, server_scrk,
					&server_sk, nullptr, false)) {
			return false;
		}
		auto dg = craft_session(
				scrk, server_sk, 1,
				{make_protocol_message(
						0x00, retail_join_request("", "12345"))});
		auto result = inmatch::handle_server_datagram(
				ctx, peer, dg.data(), dg.size(), 12);
		ProtocolPacketHeader base_hdr;
		std::vector<ProtocolMessage> base_replies;
		if (!expect(
					result.outbound.size() == 1 &&
							decode_s2c(result.outbound.front(), server_scrk,
									base_hdr, base_replies) &&
							base_replies.size() == 1 && base_replies.front().tag == 0x00,
					"a base-game host stores VERSIONCRCSTRING without comparing it [orig: @0x51231e]")) {
			return false;
		}
	}

	// Happy path: each request advances exactly one turn, and the authoritative
	// player becomes spawn-eligible only after the 256-byte challenge echo.
	{
		inmatch::NapiNPServerCtx ctx;
		inmatch::test::bring_up_host(
				ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
		world::World w;
		world::AiSystem &ai = w.ai;
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
		auto join_result = inmatch::handle_server_datagram(
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
		auto form_result = inmatch::handle_server_datagram(
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
		auto echo_result = inmatch::handle_server_datagram(
				ctx, peer, echo_dg.data(), echo_dg.size(), 102);
		if (!expect(
					!echo_result.outbound.empty() &&
							inmatch::connection_count(ctx) == 1 &&
							ctx.np_protocol.connection_list.front().self_id_seen &&
							ctx.np_protocol.connection_list.front().reply.admission_metadata_pushed &&
							!ctx.np_protocol.connection_list.front().reply.spawn_metadata_pushed &&
							!ctx.np_protocol.connection_list.front().reply.roster_pushed,
					"valid 0x02 echo completes admission and emits post-handshake metadata")) {
			return false;
		}
		inmatch::tick_connections(ctx, 16, 103);
		if (!expect(
					ctx.np_protocol.connection_list.front().link.owned_entity.valid() &&
							ctx.np_protocol.connection_list.front().reply.spawn_metadata_pushed &&
							!ctx.np_protocol.connection_list.front().reply.roster_pushed,
					"next tick spawns the player and emits only spawn metadata")) {
			return false;
		}
		const std::vector<inmatch::TickOut> pre_metadata_tick =
				inmatch::tick_connections(ctx, 16, 104);
		if (!expect(
					pre_metadata_tick.empty() &&
					!ctx.np_protocol.connection_list.front().reply.roster_pushed,
					"following tick waits for the mission-metadata boundary")) {
			return false;
		}
		inmatch::NapiNPConnection &connection =
				ctx.np_protocol.connection_list.front();
		const std::vector<ProtocolMessage> join_tail =
				inmatch::dispatch_session_replies(
						ctx.config, connection,
						{make_protocol_message(0x37, {})}, 105,
						ctx.np_protocol.connection_list, &w);
		if (!expect(
					connection.reply.roster_pushed &&
					join_tail.size() == 2 && join_tail[0].tag == 0x64 &&
					join_tail[1].tag == 0x16,
					"0x37 publishes mission metadata and the first roster together")) {
			return false;
		}
	}
	return true;
}

bool run_expansion_join_reasons_and_tlv_order() {
	inmatch::GameConfig config;
	config.expansion = "jox01";
	config.expansion_version_checksum = 55;
	auto drive = [&](std::vector<uint8_t> payload, uint32_t reason) {
		std::vector<inmatch::NapiNPConnection> roster(1);
		auto &conn = roster.front();
		conn.type = inmatch::NapiNPConnection::kTypeServerSide;
		conn.admission_stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
		conn.join_environment = {0, 2, 1, 20042002, 180};
		replication::UdpSessionTransport transport(replication::UdpSessionTransport::Role::Host);
		conn.link.transport = &transport;
		auto replies = inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(0x00, std::move(payload))}, 1, roster, nullptr);
		conn.link.transport = nullptr;
		if (reason == 0) return expect(replies.size() == 1 && replies[0].tag == 0,
				"ordered JOIN fields acknowledge the final compatible expansion and CRC");
		replication::Datagram staged;
		DisconnectEvent event;
		return expect(replies.empty() && transport.pop_outbound(staged) &&
				parse_disconnect_event(staged.body.data(), staged.body.size(), event) &&
				event.dpc == reason && event.ds == 1 && event.dc == 2,
				"expansion mismatch returns its exact game-layer rejection description");
	};
	if (!drive(retail_join_request("wrong", "wrong"), 47)) return false;
	if (!drive(retail_join_request("jox01", "0"), 48)) return false;
	if (!drive(retail_join_request("jox01", "55"), 0)) return false;
	std::vector<uint8_t> ordered = retail_join_request("wrong", "1");
	const auto last = retail_join_request("jox01", "55suffix");
	ordered.insert(ordered.end(), last.begin(), last.end());
	ordered.insert(ordered.end(), {'C', 'D', 0, 3, 0, 4, 5, 6}); // unknown binary value
	ordered.insert(ordered.end(), {'s', 'h', 'o', 'r', 't'}); // stop, retain preceding fields
	return drive(std::move(ordered), 0);
}

bool run_non_jo_peer_is_ignored() {
	// The join legs validate the complete retail JO identity + HK echo (the @0x6213b0/@0x62b750
	// gates). The four identity fields fail silently; server-side rejects have their own test.
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 31000};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

	// (a) non-JO ClientHello -> no ServerHello, no node.
	{
		ClientHello hello;
		hello.pn = "NOVAWORLDUDP";
		hello.ci = 1;
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
		if (!expect(r.outbound.empty(), "lobby PN produces no JO ServerHello")) return false;
		if (!expect(inmatch::connection_count(ctx) == 0, "lobby PN registers no JO connection")) return false;
	}
	auto reject_hello = [&](ClientHello hello, uint32_t now, const char *message) {
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		return expect(r.outbound.empty() && inmatch::connection_count(ctx) == 0, message);
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
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 5);
		if (!expect(r.outbound.empty(), "non-JO 0x42 produces no ServerAuth")) return false;
		if (!expect(inmatch::connection_count(ctx) == 0, "non-JO 0x42 registers no connection")) return false;
	}
	auto reject_auth = [&](ClientAuth auth, uint32_t now, const char *message) {
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		return expect(r.outbound.empty() && inmatch::connection_count(ctx) == 0, message);
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
	}
	return true;
}

// A live handshake against a host that was NOT brought up (host_running == 0) is rejected.
bool run_client_join_rejects_match_retail() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	config.max_players = 4;
	config.server_password = "Secret";
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly,
			inmatch::SocketMode::Lan, kHostKey, nullptr, config);
	const PeerAddr peer{0x0100007Fu, 31340};
	ClientAuth auth = make_valid_client_auth(1, 0x98761234u, kHostKey,
			"PasswordJoiner", "CLIENTPASSWORDSCRK");
	auth.pw = "sEcReT";
	auto send = [&](const ClientAuth &request) {
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(request));
		return inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	};
	auto rejected = [&](const ClientAuth &request, uint32_t family, uint32_t reason = 0) {
		auto result = send(request);
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ServerAuth reply;
		return expect(result.outbound.size() == 1 &&
				nw_decode_inbound(result.outbound[0].data(), result.outbound[0].size(), opcode, body) &&
				opcode == SESSION_OPCODE_SERVER_AUTH &&
				parse_server_auth(body.data(), body.size(), reply) && reply.cr == 0 &&
				reply.ci == request.ci && reply.ck == request.ck &&
				reply.jfc == family && reply.jfp == reason && reply.sk == 0 && reply.scrk.empty(),
				"rejected 0x42 returns the retail family/reason and echoes client identity");
	};
	ClientAuth bad = auth;
	bad.hk ^= 1;
	bad.pw.clear();
	if (!rejected(bad, 3)) return false; // HK precedes password.
	bad = auth;
	bad.pw.clear();
	bad.pv2 = "wrong";
	if (!rejected(bad, 4)) return false; // Password precedes PV2.
	bad = auth;
	bad.pv2 = "wrong";
	bad.na.clear();
	if (!rejected(bad, 7)) return false; // PV2 precedes name.
	bad = auth;
	bad.na.clear();
	ctx.np_protocol.reject_new_connections = true;
	if (!rejected(bad, 5) || !rejected(auth, 6)) return false;
	ctx.np_protocol.reject_new_connections = false;
	bad = auth;
	bad.cu = {std::vector<uint8_t>(2048)};
	bad.pn.clear();
	if (!rejected(bad, 9)) return false; // CU bounds precede identity.
	bad.cu.assign(65, std::vector<uint8_t>{1});
	if (!rejected(bad, 10)) return false;
	ctx.join_locked = true;
	ctx.banned_join_addresses.push_back(peer.ip);
	if (!rejected(auth, 14, 2)) return false;
	ctx.join_locked = false;
	// The ban operand is the datagram source (conn+0x30), never the reported
	// SIP: an omitted or forged SIP cannot evade it.
	auth.sip = 0;
	if (!rejected(auth, 14, 3)) return false;
	auth.sip = 0x88776655u;
	if (!rejected(auth, 14, 3)) return false;
	// A banned value that only matches the reported SIP admits the join.
	ctx.banned_join_addresses.assign(1, 0x88776655u);
	if (!expect(inmatch::connection_count(ctx) == 0, "rejected joins allocate no connection")) return false;
	const auto accepted = send(auth);
	if (!expect(accepted.outbound.size() == 2 && inmatch::connection_count(ctx) == 1,
			"case-insensitive server password admits the join")) return false;
	const uint32_t saved_key = ctx.np_protocol.connection_list.front().server_sk;
	bad = auth;
	bad.ci += 1;
	bad.pw = "wrong";
	if (!rejected(bad, 4)) return false;
	if (!expect(inmatch::connection_count(ctx) == 1 &&
			ctx.np_protocol.connection_list.front().server_sk == saved_key,
			"failed password cannot tear down a live same-address connection")) return false;
	ctx.join_locked = true;
	const auto retry = send(auth);
	return expect(retry.outbound.size() == 2 &&
			ctx.np_protocol.connection_list.front().server_sk == saved_key,
			"an authenticated retry precedes the callback lock and retains its keys");
}

bool run_handshake_rejected_when_host_down() {
	inmatch::NapiNPServerCtx ctx;
	if (!expect(ctx.np_protocol.host_running == 0, "host not running before create_session")) return false;
	const PeerAddr peer{0x0100007Fu, 31100};

	ClientHello hello = make_jointoperations_client_hello(1);
	hello.co = "EarlyBird";
	auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	if (!expect(r.outbound.empty(), "0x41 to a down host produces no ServerHello")) return false;
	if (!expect(inmatch::connection_count(ctx) == 0, "0x41 to a down host registers no connection")) return false;
	return true;
}

// The full listen-host bring-up installs the host's own type-2 loopback, and a remote joiner is added
// alongside it (not in place of it).
bool run_listen_host_lifecycle() {
	replication::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.max_players = 8; // co-op listen host: host loopback + up to 7 joiners (capacity gate)
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
	                        kHostKey, &loopback, settings);

	if (!expect(ctx.is_in_session == 1, "listen host is in session")) return false;
	if (!expect(ctx.is_authority == 1 && ctx.is_mp_session_peer == 1, "HostClient = host + client")) return false;
	if (!expect(ctx.np_protocol.host_running == 1, "listen host is running")) return false;
	if (!expect(ctx.np_protocol.host_key == kHostKey, "host key seeded")) return false;
	if (!expect(inmatch::connection_count(ctx) == 1, "listen-host session installs one loopback")) return false;
	if (!expect(ctx.np_protocol.connection_list[0].type == 2, "preserved node is the type-2 loopback")) return false;

	const PeerAddr peer{0x0100007Fu, 31200};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	ClientHello hello = make_jointoperations_client_hello(1);
	hello.co = "RemoteJoiner";
	auto h = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = inmatch::handle_server_datagram(ctx, peer, h.data(), h.size(), 1);
	if (!expect(rh.outbound.size() == 1, "remote 0x41 -> one ServerHello")) return false;
	auto a = craft_auth("JOINTOPERATIONS", kHostKey, 0xDEADBEEFu, scrk);
	auto ra = inmatch::handle_server_datagram(ctx, peer, a.data(), a.size(), 2);
	if (!expect(ra.outbound.size() >= 1, "remote 0x42 -> ServerAuth + initial settings")) return false;

	if (!expect(inmatch::connection_count(ctx) == 2, "joiner added alongside the loopback")) return false;
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
	world::AiSystem &ai = world.ai;
	world.registry.configure_pool(0, 16);

	replication::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.max_players = 8;
	inmatch::test::bring_up_host(
			ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
			kHostKey, &loopback, settings);
	ctx.world = &world;
	if (!expect(
				inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 1 &&
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
	if (!handshake(
				ctx, first_peer, client_scrk, 0x10101010u,
				first_server_scrk) ||
	    !handshake(
				ctx, second_peer, client_scrk, 0x20202020u,
				second_server_scrk)) {
		return false;
	}
	bool first_reserved = false;
	bool second_reserved = false;
	for (const inmatch::NapiNPConnection &connection :
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

	const std::vector<inmatch::TickOut> spawn_out =
			inmatch::tick_connections(ctx, 16, 6);
	auto slot_message_for = [&](const PeerAddr &peer, std::string_view server_scrk,
	                            ProtocolMessage &slot) {
		for (const inmatch::TickOut &tick : spawn_out) {
			if (!(tick.peer == peer)) continue;
			for (const std::vector<uint8_t> &datagram : tick.outbound) {
				ProtocolPacketHeader header;
				std::vector<ProtocolMessage> messages;
				if (!decode_s2c(datagram, server_scrk, header, messages)) continue;
				if (const ProtocolMessage *found = find_reply(messages, 0x04)) {
					slot = *found;
					return true;
				}
			}
		}
		return false;
	};
	ProtocolMessage first_slot;
	ProtocolMessage second_slot;
	if (!expect(
				slot_message_for(first_peer, first_server_scrk, first_slot) &&
						first_slot.payload.size() >= 19 && first_slot.payload[17] == 1 &&
				slot_message_for(second_peer, second_server_scrk, second_slot) &&
						second_slot.payload.size() >= 19 && second_slot.payload[17] == 2,
				"spawn-pump 0x04 publishes the two distinct reserved slots")) {
		return false;
	}
	uint8_t first_spawned_slot = 0xFF;
	uint8_t second_spawned_slot = 0xFF;
	bool spawned_reservation_left = false;
	for (const inmatch::NapiNPConnection &connection :
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
				inmatch::drop_connection(ctx, first_peer),
				"disconnect releases the first remote roster identity")) {
		return false;
	}
	const PeerAddr replacement_peer{0x0100007Fu, 31212};
	std::string replacement_server_scrk;
	if (!handshake(
				ctx, replacement_peer, client_scrk, 0x30303030u,
				replacement_server_scrk)) {
		return false;
	}
	const inmatch::NapiNPConnection *replacement = nullptr;
	for (const inmatch::NapiNPConnection &connection : ctx.np_protocol.connection_list)
		if (connection.peer == replacement_peer) replacement = &connection;
	if (!expect(
				replacement != nullptr && replacement->reply.player_slot_reserved &&
						replacement->reply.player_slot == 1,
				"replacement admission reserves the released slot 1")) {
		return false;
	}
	if (!expect(
				inmatch::drop_connection(ctx, replacement_peer),
				"pre-spawn teardown releases an advertised reservation")) {
		return false;
	}
	const PeerAddr second_replacement_peer{0x0100007Fu, 31213};
	std::string second_replacement_server_scrk;
	if (!handshake(
				ctx, second_replacement_peer, client_scrk, 0x40404040u,
				second_replacement_server_scrk)) {
		return false;
	}
	const inmatch::NapiNPConnection *second_replacement = nullptr;
	for (const inmatch::NapiNPConnection &connection : ctx.np_protocol.connection_list)
		if (connection.peer == second_replacement_peer)
			second_replacement = &connection;
	return expect(
			second_replacement != nullptr &&
					second_replacement->reply.player_slot_reserved &&
					second_replacement->reply.player_slot == 1,
			"a later admission reuses the pre-spawn reservation after teardown");
}

// Retail does not coalesce the C2S 0x02 handler and the pending-player spawn
// pump. The first S2C datagram completes admission; only a later host pump
// replays the connection settings and writes 0x04, after client-side join
// initialization has finished. The first mission-metadata response then shares
// its send boundary with the live-slot reply and initial roster. 00TRg
// retail->retail frames 13/15/20 pin these as three distinct boundaries:
//
//   admission:  settings x2, 0x01, 0x7A, 0x7B, 0x03(restrictions)
//   spawn pump: 0x03(reset), settings x2, 0x05, 0x04, 0x7B
//   join tail:  0x75, 0x64, 0x16
//
// Coalescing 0x04 into admission looks harmless in a tag histogram but retail
// subsequently resets its selected team to side B and submits the wrong 0x2F
// kit. This regression therefore treats UDP boundaries as protocol semantics.
bool run_admission_spawn_and_roster_keep_retail_packet_boundaries() {
	world::World world;
	world::AiSystem &ai = world.ai;
	world.registry.configure_pool(0, 16);

	replication::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	config.game_type = 0x00010020u;
	config.max_players = 4;
	config.expansion = "revx02";
	inmatch::test::bring_up_host(
			ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
			kHostKey, &loopback, config);
	ctx.world = &world;
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 1,
	            "listen host exists before the retail join sequence")) {
		return false;
	}

	const PeerAddr peer{0x0100007Fu, 31209};
	const std::string client_scrk =
			"BOUNDARYTESTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567";
	std::string server_scrk;
	std::vector<ProtocolMessage> admission;
	if (!handshake(
				ctx, peer, client_scrk, 0x40302010u, server_scrk,
				nullptr, nullptr, true, &admission)) {
		return false;
	}
	const auto is_settings = [](const ProtocolMessage &m) {
		return m.flags.settings_update && m.tag == hightag::CS_CONFIG_UPDATE;
	};
	if (!expect(
			admission.size() == 6 && is_settings(admission[0]) &&
			is_settings(admission[1]) && admission[2].tag == 0x01 &&
			admission[3].tag == 0x7A && admission[4].tag == 0x7B &&
			admission[5].tag == 0x03 && admission[5].payload.size() == 5 &&
			!reply_has_tag(admission, 0x04) && !reply_has_tag(admission, 0x05) &&
			!reply_has_tag(admission, 0x16),
			"C2S 0x02 reply contains only witnessed admission metadata")) {
		return false;
	}

	auto messages_for_peer = [&](const std::vector<inmatch::TickOut> &outs,
	                             std::vector<ProtocolMessage> &messages) {
		for (const inmatch::TickOut &tick : outs) {
			if (!(tick.peer == peer)) continue;
			for (const std::vector<uint8_t> &datagram : tick.outbound) {
				ProtocolPacketHeader header;
				std::vector<ProtocolMessage> decoded;
				if (!decode_s2c(datagram, server_scrk, header, decoded)) continue;
				messages.insert(messages.end(), decoded.begin(), decoded.end());
			}
		}
		return !messages.empty();
	};

	std::vector<ProtocolMessage> spawn_pump;
	if (!expect(
			messages_for_peer(inmatch::tick_connections(ctx, 16, 6), spawn_pump) &&
			spawn_pump.size() == 6 && spawn_pump[0].tag == 0x03 &&
			spawn_pump[0].payload == std::vector<uint8_t>{0x00} &&
			is_settings(spawn_pump[1]) && is_settings(spawn_pump[2]) &&
			spawn_pump[3].tag == 0x05 && spawn_pump[3].payload == std::vector<uint8_t>{0x01} &&
			spawn_pump[4].tag == 0x04 && spawn_pump[4].payload.size() == 24 &&
			spawn_pump[4].payload[17] == 1 && spawn_pump[4].payload[18] == 4 &&
			spawn_pump[4].payload[23] == 1 && spawn_pump[5].tag == 0x7B &&
			!reply_has_tag(spawn_pump, 0x16),
			"next host tick emits the witnessed spawn-pump replay with team 1")) {
		return false;
	}

	inmatch::NapiNPConnection *remote = nullptr;
	for (inmatch::NapiNPConnection &connection : ctx.np_protocol.connection_list)
		if (connection.peer == peer) remote = &connection;
	if (!expect(remote != nullptr, "spawn-pump fixture retains the remote connection"))
		return false;
	std::vector<ProtocolMessage> premature_roster;
	if (!expect(
			!messages_for_peer(inmatch::tick_connections(ctx, 16, 7), premature_roster) &&
			premature_roster.empty(),
			"the post-spawn tick does not emit an early standalone roster")) {
		return false;
	}

	const std::vector<ProtocolMessage> join_tail = inmatch::dispatch_session_replies(
			ctx.config, *remote,
			{make_protocol_message(0x47, {}), make_protocol_message(0x37, {})},
			7, ctx.np_protocol.connection_list, &world);
	const ProtocolMessage *team_state = find_reply(join_tail, 0x75);
	if (!expect(
			team_state != nullptr &&
			team_state->payload == std::vector<uint8_t>({0x00, 0x01}),
			"0x75 serializes the live team-1 slot instead of a team-2 fixture")) {
		return false;
	}

	if (!expect(
			join_tail.size() == 3 && join_tail[0].tag == 0x75 &&
			join_tail[1].tag == 0x64 && join_tail[2].tag == 0x16,
			"the first 0x37 boundary is exactly 0x75 + 0x64 + 0x16")) {
		return false;
	}
	if (!expect(
			inmatch::Server_SetPlayerSpectator(ctx, *remote, world, true),
			"authority can move the live player into spectator state")) {
		return false;
	}
	const std::vector<ProtocolMessage> spectator_state =
			inmatch::dispatch_session_replies(
					ctx.config, *remote, {make_protocol_message(0x47, {})},
					7, ctx.np_protocol.connection_list, &world);
	const ProtocolMessage *spectator_slot = find_reply(spectator_state, 0x75);
	if (!expect(
			spectator_slot != nullptr &&
			spectator_slot->payload == std::vector<uint8_t>({0x01, 0x00}),
			"0x75 bit 0 advertises free-fly spectator mode on team 0")) {
		return false;
	}
	if (!expect(
			inmatch::Server_SetPlayerSpectator(ctx, *remote, world, false),
			"authority can return the spectator to ordinary play")) {
		return false;
	}

	std::vector<ProtocolMessage> same_tick_stream;
	if (!expect(
			!messages_for_peer(inmatch::tick_connections(ctx, 16, 7), same_tick_stream) &&
			same_tick_stream.empty(),
			"world streaming waits until after the roster send boundary")) {
		return false;
	}
	std::vector<ProtocolMessage> next_tick_stream;
	return expect(
			messages_for_peer(inmatch::tick_connections(ctx, 16, 8), next_tick_stream) &&
			!next_tick_stream.empty() && !reply_has_tag(next_tick_stream, 0x16),
			"world streaming begins on the following tick without another roster");
}

// The PR #403 retail-to-retail LAN witness sends the joining client's ClientAuth.NA
// in FULL_PLAYER_INFO field 1 and the map filename in both mission fields:
//
//   RetailJoin403\0\0Untitled \000TRg.bms\000TRg.bms\0...
//
// This message is recipient-scoped. Advertising the host player's callsign here
// makes the retail joiner build the wrong local identity before its loadout submit.
bool run_full_player_info_is_recipient_scoped_lan_metadata() {
	world::World world;
	world.registry.configure_pool(0, 8);

	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	config.player_name = "RetailHost403";
	config.server_name = "Untitled ";
	config.mission_name = "Training: Grenade Launcher";
	config.mission_file = "00TRg.bms";
	config.game_type = 0x00010020u;
	config.expansion = "revx02";
	inmatch::test::bring_up_host(
			ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			kHostKey, nullptr, config);
	ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 31212};
	const std::string client_scrk =
			"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	std::string server_scrk;
	std::vector<ProtocolMessage> burst;
	if (!handshake(
				ctx, peer, client_scrk, 0x40340340u,
				server_scrk, nullptr, nullptr, true, &burst)) {
		return false;
	}

	const ProtocolMessage *message = find_reply(burst, 0x7B);
	FullPlayerInfo info;
	if (!expect(
				message != nullptr &&
				decode_full_player_info(
						message->payload.data(), message->payload.size(), info),
				"post-handshake burst carries a decodable S2C 0x7B")) {
		return false;
	}
	return expect(
			info.player_name == "TestJoiner" && info.player_id.empty() &&
					info.server_name == config.server_name &&
					info.mission_name == config.mission_file &&
					info.map_file == config.mission_file &&
					info.extra == config.game_type && info.game_name == config.expansion,
			"S2C 0x7B matches the recipient-scoped retail LAN witness");
}

// Retail does not always duplicate the map filename into FULL_PLAYER_INFO field
// 4. NapiNPMsg_0x7B_BuildPayload @0x507822 selects the active filename for the
// waypoint family (the objective bit is ignored), and the resolved MissionText
// title for Deathmatch/TDM. Field 5 remains the map filename on both arms.
bool run_full_player_info_selects_retail_mission_title_branch() {
	struct MissionIdentityCase {
		uint32_t game_type;
		const char *mission_title;
		const char *expected_mission;
		uint16_t port;
		const char *message;
	};
	const std::array<MissionIdentityCase, 3> cases = {{
			{0x00000000u, "DM - Awan Atoll", "DM - Awan Atoll", 31213,
			 "Deathmatch 0x7B advertises the mission title and exact map filename"},
			{0x00010000u, "TDM - Awan Atoll", "TDM - Awan Atoll", 31214,
			 "TDM 0x7B advertises the mission title and exact map filename"},
			{0x00030020u, "COOP - Awan Atoll", "TDH_I3A.bms", 31215,
			 "objective waypoint 0x7B advertises the filename in both mission fields"},
	}};

	for (const MissionIdentityCase &test_case : cases) {
		world::World world;
		world.registry.configure_pool(0, 8);

		inmatch::NapiNPServerCtx ctx;
		inmatch::GameConfig config;
		config.player_name = "RetailHost403";
		config.server_name = "Untitled ";
		config.mission_name = test_case.mission_title;
		config.mission_file = "TDH_I3A.bms";
		config.game_type = test_case.game_type;
		config.expansion = "revx02";
		inmatch::test::bring_up_host(
				ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
				kHostKey, nullptr, config);
		ctx.world = &world;

		const PeerAddr peer{0x0100007Fu, test_case.port};
		const std::string client_scrk =
				"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
		std::string server_scrk;
		std::vector<ProtocolMessage> burst;
		if (!handshake(
					ctx, peer, client_scrk, 0x40340340u,
					server_scrk, nullptr, nullptr, true, &burst)) {
			return false;
		}

		const ProtocolMessage *message = find_reply(burst, 0x7B);
		FullPlayerInfo info;
		if (!expect(
					message != nullptr &&
					decode_full_player_info(
							message->payload.data(), message->payload.size(), info),
					"mission-identity branch emits a decodable S2C 0x7B")) {
			return false;
		}
		if (!expect(
					info.mission_name == test_case.expected_mission &&
					info.map_file == config.mission_file &&
					info.extra == test_case.game_type,
					test_case.message)) {
			return false;
		}
	}
	return true;
}

// A retransmitted 0x42 ClientAuth for an already-joined connection must re-send
// the cached ServerAuth, not re-mint the server SCRK/SK.
bool run_retransmit_0x42_keeps_keys() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 30800};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t ck = 0x12345678u;

	auto first_auth = [&](uint32_t now, ServerAuth &out_sa) -> bool {
		auto a = craft_auth("JOINTOPERATIONS", kHostKey, ck, scrk); // craft_auth uses CI = 1
		auto r = inmatch::handle_server_datagram(ctx, peer, a.data(), a.size(), now);
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
	if (!expect(inmatch::connection_count(ctx) == 1, "retransmit does not create a second node")) return false;
	return true;
}

// The join leg enforces capacity. A dedicated host with max_players == 2 admits two joiners; the third
// 0x42 is rejected. [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0 — current_player_count >= max]
// ClientAuth JSP is the submitted side/squad password; TR is the signed team
// choice. Drive the real auth loader, JOIN validator, reject vehicle and team
// reservation so a codec-only change cannot make this regression pass.
// [orig: NapiNetConfig_SetJsp @0x4C26BE; Server_ValidatePlayerJoinRequest
// @0x5124A2..0x5125D4; Server_AssignPlayerTeam @0x4FE424..0x4FE519]
bool run_side_password_admission() {
	struct Case {
		const char *side_a;
		const char *side_b;
		const char *password;
		int team_request;
		bool spectator;
		uint32_t reject;
		uint8_t team;
	};
	const Case cases[] = {
		{"Blue", "Red", "wrong", -1, false, 18, 0},
		{"Blue", "Red", "wrong", 0, false, 19, 0},
		{"Blue", "Red", "wrong", 1, false, 20, 0},
		{"Blue", "Red", "bLuE", -1, false, 0, 1},
		{"Blue", "Red", "rEd", -1, false, 0, 2},
		{"Blue", "Red", "Blue", 1, false, 20, 0},
		{"Blue", "", "", -1, false, 0, 2},
		{"", "Red", "", -1, false, 0, 1},
		{"", "", "", 1, false, 0, 2},
		{"Blue", "Red", "", 0, true, 0, 0},
	};
	for (const Case &test : cases) {
		inmatch::NapiNPServerCtx ctx;
		inmatch::GameConfig config;
		config.game_type = 0x10000u;
		config.mp_attributes = inmatch::GameConfig::kMpAttribTeamChoose;
		config.spectator_slots = -1;
		config.side_a_password = test.side_a;
		config.side_b_password = test.side_b;
		inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Lan, kHostKey, nullptr, config);
		auto auth = make_valid_client_auth(12, 0x1212u, kHostKey, "SideJoiner",
				"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB");
		auth.cu.push_back(make_client_cu_chunk(2, "JSP", "overwritten"));
		auth.cu.push_back(make_client_cu_chunk(2, "jSp", test.password));
		auth.cu.push_back(make_client_cu_chunk(1, "JSP", "ignored-wrong-type"));
		auth.cu.push_back(make_client_cu_chunk(2, "TR", std::to_string(test.team_request)));
		if (test.spectator) auth.cu.push_back(make_client_cu_chunk(2, "JSR", "1"));
		const auto datagram = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		const PeerAddr peer{0x0100007Fu, 32012};
		inmatch::handle_server_datagram(ctx, peer, datagram.data(), datagram.size(), 3);
		if (!expect(ctx.np_protocol.connection_list.size() == 1,
				"side password is validated at JOIN, after ClientAuth")) return false;
		auto &roster = ctx.np_protocol.connection_list;
		auto &conn = roster.front();
		replication::UdpSessionTransport transport(replication::UdpSessionTransport::Role::Host);
		conn.link.transport = &transport;
		const auto join = retail_join_request(config.expansion);
		const auto replies = inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(0x00, join)}, 5, roster, nullptr);
		if (test.reject) {
			replication::Datagram staged;
			DisconnectEvent event;
			const bool has_punt = transport.pop_outbound(staged) &&
					parse_disconnect_event(staged.body.data(), staged.body.size(), event);
			if (!expect(replies.empty() &&
					conn.admission_stage == inmatch::GameAdmissionStage::Rejected &&
					has_punt && event.ds == 1 && event.dc == 2 && event.dpc == test.reject,
					"wrong side password emits the exact retail description reject")) {
				std::fprintf(stderr, "expected DPC %u, observed %u (punt %d, replies %zu)\n",
						test.reject, event.dpc, has_punt, replies.size());
				return false;
			}
		} else {
			if (!expect(!replies.empty() && replies.front().tag == s2c::INIT,
					"valid side credential admits JOIN")) return false;
			auto world = std::make_unique<opennova::world::World>();
			if (!expect(inmatch::Server_ReservePlayerTeam(config, true, roster, conn, *world) ==
					test.team, "the submitted password selects the matching side")) return false;
		}
		conn.link.transport = nullptr;
	}
	return true;
}

bool run_spectator_admission_codes_match_retail() {
	const std::string scrk =
			"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	auto spectator_auth = [&](uint32_t ci, uint32_t ck, std::string_view password) {
		ClientAuth auth = make_valid_client_auth(
				ci, ck, kHostKey, "TestSpectator", scrk);
		auth.cu.push_back(make_client_cu_chunk(2, "JSR", "1"));
		if (!password.empty())
			auth.cu.push_back(make_client_cu_chunk(2, "JSPP", password));
		return craft(
				SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	};
	auto decode_auth = [&](const inmatch::HandleResult &result, ServerAuth &auth,
	                       const char *message) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		return expect(
				!result.outbound.empty() &&
				nw_decode_inbound(
						result.outbound.front().data(),
						result.outbound.front().size(), opcode, body) &&
				opcode == SESSION_OPCODE_SERVER_AUTH &&
				parse_server_auth(body.data(), body.size(), auth),
				message);
	};

	// --- The 0x42 leg stores the request; it never carries spectator codes. ---
	// [orig: NapiNetConfig_LoadFromConnTags @0x4c7260 stores JSR/JSPP on the
	// connection; NapiNPProtocol_HandleClientJoin @0x62b750 has no spectator
	// leg of its own]
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.max_players = 1;
	settings.spectator_slots = 1;
	settings.spectator_password = "watch";
	inmatch::test::bring_up_host(
			ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			kHostKey, nullptr, settings);

	const PeerAddr admitted_peer{0x0100007Fu, 32012};
	const std::vector<uint8_t> admitted_datagram =
			spectator_auth(12, 0x1212u, "WATCH");
	const inmatch::HandleResult admitted = inmatch::handle_server_datagram(
			ctx, admitted_peer, admitted_datagram.data(),
			admitted_datagram.size(), 3);
	ServerAuth admitted_reply;
	if (!decode_auth(
				admitted, admitted_reply,
				"a spectator ClientAuth key-establishes like any other join")) {
		return false;
	}
	if (!expect(
			admitted_reply.cr == 1 && inmatch::connection_count(ctx) == 1 &&
			ctx.np_protocol.connection_list.front().join_spectator_request == 1 &&
			ctx.np_protocol.connection_list.front().join_spectator_password ==
					"WATCH" &&
			!ctx.np_protocol.connection_list.front().link.spectator,
			"the 0x42 stores JSR/JSPP without latching the live spectator bit")) {
		return false;
	}

	// The 0x42 gate is ONE shared count against max_players + positive slots:
	// an ordinary player may occupy the spectator headroom (retail has no
	// separate ordinary cap), and the join past the shared total rejects with
	// the CR=0 0x82 carrying JFC=14 and JFP=5 (the positive-spectator-slots
	// reason). [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0 reasons 4/5;
	// NapiNPProtocol_SendJoinRejection @0x620cd0]
	{
		const std::string player_scrk =
				"TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
		auto player_auth = craft_auth(
				"JOINTOPERATIONS", kHostKey, 0x1414u, player_scrk);
		const PeerAddr player_peer{0x0100007Fu, 32014};
		inmatch::HandleResult player_result = inmatch::handle_server_datagram(
				ctx, player_peer, player_auth.data(), player_auth.size(), 4);
		if (!expect(
				!player_result.outbound.empty() &&
				inmatch::connection_count(ctx) == 2,
				"an ordinary player occupies the spectator headroom (shared count)")) {
			return false;
		}
		const PeerAddr third{0x0100007Fu, 32013};
		const std::vector<uint8_t> datagram =
				spectator_auth(13, 0x1313u, "watch");
		inmatch::HandleResult result = inmatch::handle_server_datagram(
				ctx, third, datagram.data(), datagram.size(), 5);
		ServerAuth reply;
		if (!decode_auth(result, reply, "join beyond shared capacity answers")) {
			return false;
		}
		if (!expect(
				reply.cr == 0 && reply.jfc == 14 && reply.jfp == 5 &&
				inmatch::connection_count(ctx) == 2,
				"shared capacity rejects with JFC 14 / JFP 5 and no node")) {
			return false;
		}
	}

	// --- The game-layer 0x00 join gate answers the spectator codes as DPC
	// punts through the connection-description record, after admission. ---
	// [orig: Server_ValidatePlayerJoinRequest @0x512100 codes 14/15/16 via
	// CNapiNPConnection_SendChatMessage @0x4c7ef0]
	const std::vector<uint8_t> join_payload = [] {
		std::vector<uint8_t> payload;
		const char name[] = "VERSIONCRCSTRING";
		payload.insert(payload.end(), name, name + sizeof(name));
		payload.push_back(2);
		payload.push_back(0);
		payload.push_back('0');
		payload.push_back(0);
		return payload;
	}();
	struct JoinGate {
		bool valid = false;
		bool got_init = false;
		bool rejected = false;
		bool punt_staged = false;
		bool spectator_latched = false;
		DisconnectEvent event;
	};
	auto drive_join_gate = [&](const inmatch::GameConfig &config,
			std::vector<inmatch::NapiNPConnection> &roster, size_t index) {
		JoinGate out;
		replication::UdpSessionTransport transport(
				replication::UdpSessionTransport::Role::Host);
		inmatch::NapiNPConnection &conn = roster[index];
		conn.link.transport = &transport;
		const std::vector<ProtocolMessage> replies =
				inmatch::dispatch_session_replies(config, conn,
						{make_protocol_message(0x00, join_payload)},
						5, roster, nullptr);
		out.valid = true;
		for (const ProtocolMessage &reply : replies)
			if (reply.tag == s2c::INIT) out.got_init = true;
		out.rejected =
				conn.admission_stage == inmatch::GameAdmissionStage::Rejected;
		replication::Datagram staged;
		if (transport.pop_outbound(staged)) {
			out.punt_staged = parse_disconnect_event(
					staged.body.data(), staged.body.size(), out.event);
		}
		out.spectator_latched = conn.link.spectator;
		conn.link.transport = nullptr;
		return out;
	};
	auto make_gate_conn = [](uint8_t spectator_request,
			std::string password) {
		inmatch::NapiNPConnection conn;
		conn.type = inmatch::NapiNPConnection::kTypeServerSide;
		conn.phase = inmatch::ConnectionPhase::Joined;
		conn.admission_stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
		conn.join_environment = {0, 2, 1, 20042002, 180};
		conn.join_spectator_request = spectator_request;
		conn.join_spectator_password = std::move(password);
		return conn;
	};

	{
		// Spectating disabled: the gate latches, then punts DPC 14. (An empty
		// expansion keeps the 0x00 payload to the bare VERSIONCRCSTRING TLV.)
		inmatch::GameConfig disabled;
		disabled.expansion.clear();
		std::vector<inmatch::NapiNPConnection> roster;
		roster.push_back(make_gate_conn(1, ""));
		const JoinGate gate = drive_join_gate(disabled, roster, 0);
		if (!expect(gate.rejected && !gate.got_init,
				"a disabled-spectator join gate rejects without INIT")) {
			return false;
		}
		if (!expect(gate.punt_staged,
				"the disabled-spectator reject stages a description punt")) {
			return false;
		}
		if (!expect(
				gate.event.dc == 2 && gate.event.dpc == 14 &&
				gate.event.dstr.empty() && gate.event.ddstr.empty(),
				"retail DPC 14 punts a spectator when spectating is disabled")) {
			return false;
		}
	}

	inmatch::GameConfig gate_config;
	gate_config.expansion.clear();
	gate_config.spectator_slots = 1;
	gate_config.spectator_password = "watch";
	{
		// Wrong password: DPC 16. The compare is case-insensitive, so the
		// mixed-case password must NOT reject.
		std::vector<inmatch::NapiNPConnection> roster;
		roster.push_back(make_gate_conn(1, "wrong"));
		const JoinGate gate = drive_join_gate(gate_config, roster, 0);
		if (!expect(
				gate.valid && gate.rejected && gate.punt_staged &&
				gate.event.dc == 2 && gate.event.dpc == 16,
				"retail DPC 16 punts a bad spectator password")) {
			return false;
		}
	}
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_gate_conn(1, "WATCH"));
	{
		const JoinGate gate = drive_join_gate(gate_config, roster, 0);
		if (!expect(
				gate.valid && !gate.rejected && gate.got_init &&
				!gate.punt_staged && gate.spectator_latched,
				"a case-insensitive JSPP match admits and latches the spectator")) {
			return false;
		}
	}
	{
		// A second spectator past the positive slot count: the count includes
		// the candidate's freshly latched flag, so it rejects on
		// strictly-greater with DPC 15.
		roster.push_back(make_gate_conn(1, "watch"));
		const JoinGate gate = drive_join_gate(gate_config, roster, 1);
		return expect(
				gate.valid && gate.rejected && gate.punt_staged &&
				gate.event.dc == 2 && gate.event.dpc == 15,
				"retail DPC 15 punts a spectator past the slot count");
	}
}

bool run_capacity_rejects_when_full() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.max_players = 2; // dedicated host: two joiner slots, no host loopback
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey,
	                        nullptr, settings);
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

	auto join = [&](const PeerAddr &p, uint32_t ck, uint32_t now) {
		auto a = craft_auth("JOINTOPERATIONS", kHostKey, ck, scrk);
		return inmatch::handle_server_datagram(ctx, p, a.data(), a.size(), now);
	};

	if (!expect(join(PeerAddr{0x0100007Fu, 32001}, 0x1111u, 1).outbound.size() >= 1,
	            "joiner 1 key-established (ServerAuth + initial settings)")) return false;
	if (!expect(join(PeerAddr{0x0100007Fu, 32002}, 0x2222u, 2).outbound.size() >= 1,
	            "joiner 2 key-established (ServerAuth + initial settings)")) return false;
	if (!expect(inmatch::connection_count(ctx) == 2, "two joiners fill the server")) return false;

	auto r3 = join(PeerAddr{0x0100007Fu, 32003}, 0x3333u, 3);
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ServerAuth rejected;
	if (!expect(
			!r3.outbound.empty() &&
			nw_decode_inbound(
					r3.outbound.front().data(), r3.outbound.front().size(),
					opcode, body) &&
			opcode == SESSION_OPCODE_SERVER_AUTH &&
			parse_server_auth(body.data(), body.size(), rejected) &&
			rejected.cr == 0 && rejected.jfc == 14 && rejected.jfp == 4,
			"over-capacity join returns the retail CR=0 0x82 with JFC 14 / JFP 4")) {
		return false;
	}
	if (!expect(inmatch::connection_count(ctx) == 2, "over-capacity join creates no node")) return false;
	return true;
}

// Armory-fed loadout resolve (D-NET-141): with world.tables.weapons built from the committed fixture,
// the 0x5A reply resolves REAL ammo counts through the witnessed rules instead of echoing —
// filters drop unfiltered (emplaced) request entries, counts come from startrounds/clipsize
// (min(req,maxclips) on an explicit request), the alt byte carries the first different-ammoclass
// sub-variant, and the reply sorts by weapon-slot combo (category*65+rank).
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 / WeaponSlot_GetTotalClips @0x5425F0]
// NOTE fixture truth ≠ live-install truth: a real JO:CA root resolves a larger weapon.def whose
// indices reproduce the golden bytes end-to-end — that equality is the live v16 wire gate.
bool run_loadout_resolve_with_armory() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
	// Every accepted 0x2F re-arms the player's armory-reuse cooldown (playerSlot+356 =
	// armoryReuseTime) and a later nonzero-class submit is refused until it expires, so a
	// host that lets this joiner resubmit back to back runs with ArmoryTimer 0.
	// [orig: NapiNPServerMsg_HandlePlayerLoadout — the accept store @0x515ba6, the
	//  `slot[89] <= 0 || preround` gate @0x5158d0 -> the re-send @0x515fa5]
	ctx.config.armory_reuse_time = 0;

	// The armory: the shipped weapon.def from the reference fixture set -> the
	// witnessed table (null@0 + file order). A SKIP-LEG retail leg without it.
	const std::string def_path = retail::reference_fixture("def/weapon.def");
	const std::string ammo_path = retail::reference_fixture("def/ammo.def");
	if (def_path.empty() || ammo_path.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/weapon.def + ammo.def (the shipped armory the loadout resolve keys on)");
		return true;
	}
	DefWeaponsFile wf{};
	if (!expect(def_parse_weapons(def_path.c_str(), &wf) == 0, "fixture weapon.def parses")) return false;
	world::World world;
	world.tables.weapons = world::build_weapon_table(wf);
	def_free_weapons(&wf);
	DefAmmoFile af{};
	if (!expect(def_parse_ammo(ammo_path.c_str(), &af) == 0, "fixture ammo.def parses")) return false;
	world.tables.ammo = world::build_ammo_table(af);
	def_free_ammo(&af);
	world::resolve_weapon_round_types(world.tables.weapons, world.tables.ammo);
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
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
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
		auto response = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
		bool saw_5a = false;
		for (const std::vector<uint8_t> &outbound : response.outbound) {
			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> messages;
			if (!decode_s2c(outbound, server_scrk, hdr, messages)) continue;
			if (reply_has_tag(messages, 0x5A)) saw_5a = true;
		}
		if (!expect(!saw_5a, label)) return false;
		const inmatch::NapiNPConnection *connection = nullptr;
		for (const inmatch::NapiNPConnection &candidate : ctx.np_protocol.connection_list)
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
		const world::WeaponTableEntry *m4 = world.tables.weapons.by_index(9);
		const inmatch::NapiNPConnection *connection = nullptr;
		for (const inmatch::NapiNPConnection &candidate : ctx.np_protocol.connection_list)
			if (candidate.peer == peer) connection = &candidate;
		const int pool_id = m4 != nullptr ? m4->ammo_class_id : -1;
		// The accepted count seeds the carried pool, then the rebuilt slot draws
		// its initial clip before S2C 0x0F copies player+88664. Thus an explicit
		// four-clip M4 request retains three magazines (90), not all four (120).
		int32_t expected_pool = m4 != nullptr
				? 4 * m4->clipsize - m4->clipsize * m4->ammo_class_count
				: 0;
		if (m4 != nullptr && pool_id >= 0 &&
		    pool_id < static_cast<int>(world.tables.weapons.ammo_class_caps.size())) {
			expected_pool = std::min(
					expected_pool,
					world.tables.weapons.ammo_class_caps[static_cast<size_t>(pool_id)]);
		}
		if (!expect(
				connection != nullptr && pool_id >= 0 && pool_id < 128 &&
						connection->reply.ammo_pools[static_cast<size_t>(pool_id)] ==
								expected_pool,
				"accepted 0x2F retains the authority ammo pool for S2C 0x0F")) {
			return false;
		}
	}

	const int m4_auto = world.tables.weapons.index_of("WPN_M4AUTO");
	const int m4_m203_auto = world.tables.weapons.index_of("WPN_M4M203AUTO");
	if (!expect(m4_auto > 0 && m4_m203_auto > 0,
	            "shared-ammo loadout fixtures resolve")) return false;
	const int16_t m4_ammo = world.tables.weapons.entries[static_cast<size_t>(m4_auto)].ammo_index;
	if (!expect(m4_ammo >= 0 &&
	                    world.tables.weapons.entries[static_cast<size_t>(m4_m203_auto)].ammo_index == m4_ammo,
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
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.max_players = 8;
	// ArmoryTimer 0: the accepted grants below re-arm playerSlot+356 with 0, so the
	// next submit is judged by the envelope alone rather than refused by the cooldown.
	// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x5158d0 / @0x515ba6]
	settings.armory_reuse_time = 0;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey,
	                        nullptr, settings);
	world::World world;
	world.registry.configure_pool(0, 16);
	world::AiSystem &ai = world.ai; // the §5.2b spawn mounts the infantry motor
	ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 30150};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	std::string server_scrk;
	uint32_t server_sk = 0;
	uint32_t seq = 1;
	if (!handshake(ctx, peer, client_scrk, 0xDEADBEE3u, server_scrk, &server_sk, &seq)) return false;
	// The spawn pump binds the joiner's pool-0 entity, so the handler has a real entity+660 to
	// stamp (or to leave alone). [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 1,
	            "the admitted joiner spawns its player entity")) return false;

	auto joiner_conn = [&]() -> const inmatch::NapiNPConnection * {
		for (const inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list)
			if (c.peer == peer) return &c;
		return nullptr;
	};
	auto joiner_entity = [&]() -> world::Entity * {
		const inmatch::NapiNPConnection *c = joiner_conn();
		if (c == nullptr || !c->link.owned_entity.valid()) return nullptr;
		return world.registry.get(c->link.owned_entity);
	};
	if (!expect(joiner_entity() != nullptr, "the joiner's player entity resolves")) return false;
	joiner_entity()->player_class = 6; // the live class an aborted submission must not disturb

	// One 0x2F in, the last 0x5A body out (empty when the handler produced none).
	auto submit = [&](const std::vector<uint8_t> &req, uint32_t now) -> std::vector<uint8_t> {
		auto dg = craft_session(client_scrk, server_sk, seq++, {make_protocol_message(0x2F, req)});
		auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), now);
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
		const inmatch::NapiNPConnection *c = joiner_conn();
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
		const inmatch::NapiNPConnection *c = joiner_conn();
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
		const inmatch::NapiNPConnection *c = joiner_conn();
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
		const inmatch::NapiNPConnection *c = joiner_conn();
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
		const inmatch::NapiNPConnection *c = joiner_conn();
		if (!expect(c != nullptr && c->reply.last_loadout_reply == body,
		            "the empty class-0 grant REPLACES the retained body")) return false;
		if (!expect(joiner_entity()->player_class == 0,
		            "class 0 stamps entity+660 zero")) return false;
	}

	// (7) A valid requested class whose host-policy bit is clear is remapped to the first
	// allowed Soldier Class in retail's 5..9 scan. Both the S2C 0x5A grant header and the owned
	// Person carry that effective class [orig: @0x5158d6..@0x515915, stamp @0x515ab0].
	{
		ctx.config.class_allow_mask = 1u << 6;
		const std::vector<uint8_t> body =
				submit({0x01, 0x08, 0xC3, 0x00, 0x00, 0x00, 9, 0xFF, 0xFF, 0xFF, 0xFF}, 260);
		WeaponLoadout lo;
		if (!expect(decode_weapon_loadout(body.data(), body.size(), lo) &&
		                    lo.avatar_class == 6,
		            "a denied class is remapped in the S2C 0x5A grant")) return false;
		if (!expect(joiner_entity()->player_class == 6,
		            "the same remapped class stamps the owned Person")) return false;
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
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
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
	auto r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	if (!expect(r.outbound.size() >= 1, "0x42 with CU vars admitted")) return false;

	const inmatch::NapiNPConnection *conn = nullptr;
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
	inmatch::NapiNPServerCtx ctx2;
	inmatch::test::bring_up_host(ctx2, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan, kHostKey);
	const PeerAddr peer2{0x0100007Fu, 30901};
	ClientAuth auth2 = auth;
	auth2.cu.push_back(make_client_cu_chunk(2, "TR", "7"));
	auto dg2 = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth2));
	inmatch::handle_server_datagram(ctx2, peer2, dg2.data(), dg2.size(), 1);
	for (const auto &c : ctx2.np_protocol.connection_list) {
		if (c.peer == peer2 &&
		    !expect(c.char_vars.team_request == 0xFF, "TR=7 clamps to 0xFF")) return false;
	}
	return true;
}

// Host-side validation of the two integrity reply families is profile-scoped:
// only CRC sources covered by the explicitly selected retail resource corpus
// may disconnect a peer. The mismatch record is the exact high-table 0x103
// connection description consumed by an unmodified retail client.
bool run_integrity_replies_validate_registered_profile() {
	struct Observation {
		bool replies_empty = false;
		bool disconnect_latched = false;
		bool staged = false;
		bool extra_staged = false;
		bool parsed = false;
		replication::Datagram datagram;
		DisconnectEvent event;
	};

	auto observe = [](std::string_view profile_id, uint8_t tag,
			std::vector<uint8_t> body, uint32_t weapon_salt = 0,
			uint32_t ammo_salt = 0, bool append_ping = false) {
		inmatch::GameConfig config;
		config.integrity_profile = std::string(profile_id);
		replication::UdpSessionTransport transport(
				replication::UdpSessionTransport::Role::Host);
		std::vector<inmatch::NapiNPConnection> roster(1);
		inmatch::NapiNPConnection &conn = roster.front();
		conn.type = 1;
		conn.phase = inmatch::ConnectionPhase::InMatch;
		conn.burst.spawned = true;
		conn.link.mode = replication::TransportMode::Client;
		conn.link.transport = &transport;
		conn.reply.integrity_weapon_crc_salt = weapon_salt;
		conn.reply.integrity_ammo_crc_salt = ammo_salt;

		std::vector<ProtocolMessage> messages;
		messages.push_back(make_protocol_message(tag, std::move(body)));
		if (append_ping)
			messages.push_back(make_protocol_message(c2s::PING, {}));
		Observation result;
		result.replies_empty = inmatch::dispatch_session_replies(
				config, conn, messages,
				100, roster, nullptr).empty();
		if (append_ping) {
			result.replies_empty = result.replies_empty &&
					inmatch::dispatch_session_replies(
							config, conn,
							{make_protocol_message(c2s::PING, {})},
							101, roster, nullptr).empty();
		}
		result.disconnect_latched = conn.host_disconnect_sent;
		result.staged = transport.pop_outbound(result.datagram);
		replication::Datagram extra;
		result.extra_staged = transport.pop_outbound(extra);
		if (result.staged) {
			result.parsed = parse_disconnect_event(
					result.datagram.body.data(), result.datagram.body.size(),
					result.event);
		}
		return result;
	};

	auto expect_silent = [](const Observation &result, const char *message) {
		return expect(result.replies_empty && !result.disconnect_latched &&
					!result.staged && !result.extra_staged,
				message);
	};
	auto expect_crc_punt = [](const Observation &result,
			std::string_view detail, const char *message) {
		return expect(result.replies_empty && result.disconnect_latched &&
					result.staged && !result.extra_staged &&
					result.datagram.tag == hightag::DESCRIPTION_PACKET &&
					result.datagram.reliable &&
					result.datagram.protocol_flags_raw == 0xA0 && result.parsed &&
					result.event.ds == 1 && result.event.dc == 2 &&
					result.event.dp1 == 0 && result.event.dp2 == 0 &&
					result.event.dstr.empty() && result.event.dpc == 46 &&
					result.event.ddstr == detail,
				message);
	};

	constexpr uint32_t kWeaponTableCrc = 0x024F56F2u;
	constexpr uint32_t kAmmoRow18Crc = 0x2D087374u;
	constexpr uint32_t kWeaponSalt = 0x11223344u;
	constexpr uint32_t kAmmoSalt = 0x55667788u;
	const std::string_view profile = inmatch::kRetailRevx02IntegrityProfileId;

	if (!expect(!inmatch::weapon_integrity_reply_matches(
					0xFF, /*source_crc=*/42, /*salt=*/0,
					/*received_crc=*/41),
			"a profile source CRC of 42 does not bypass a non-42 C2S 0x20 mismatch"))
		return false;
	if (!expect_silent(
			observe(profile, c2s::ENTITY_CHECKSUM_REPLY,
					indexed_crc_reply(0xFF, 42), kWeaponSalt),
			"C2S 0x20 id 0xFF bypasses on received checksum 42 regardless of profile CRC and salt"))
		return false;
	if (!expect_silent(
			observe(profile, c2s::ENTITY_CHECKSUM_REPLY,
					indexed_crc_reply(0xFF, kWeaponTableCrc ^ kWeaponSalt,
							{0xDE, 0xAD}),
					kWeaponSalt),
			"C2S 0x20 accepts the salted whole-table CRC and ignores trailing bytes"))
		return false;
	if (!expect_crc_punt(
			observe(profile, c2s::ENTITY_CHECKSUM_REPLY,
					indexed_crc_reply(0xFF, 0), 0, 0,
					/*append_ping=*/true),
			"PUNT WCRC",
			"a wrong C2S 0x20 stages exact WCRC and suppresses later batch replies"))
		return false;
	if (!expect_crc_punt(
			observe(profile, c2s::ENTITY_CHECKSUM_REPLY, {0xFF}),
			"PUNT WCRC",
			"a short C2S 0x20 zero-fills its missing checksum before validation"))
		return false;
	if (!expect_silent(
			observe({}, c2s::ENTITY_CHECKSUM_REPLY,
					indexed_crc_reply(0xFF, 0)),
			"C2S 0x20 is ignored when no integrity profile is selected"))
		return false;
	if (!expect_silent(
			observe(profile, c2s::ENTITY_CHECKSUM_REPLY,
					indexed_crc_reply(0x07, 0)),
			"C2S 0x20 cannot validate an individual ADM row absent from the profile"))
		return false;

	if (!expect_silent(
			observe(profile, c2s::CHECKSUM_REPLY,
					indexed_crc_reply(0x18, kAmmoRow18Crc ^ kAmmoSalt,
							{0xCA, 0xFE, 0xBA, 0xBE}),
					0, kAmmoSalt),
			"C2S 0x21 accepts a salted covered ammo CRC and ignores trailing bytes"))
		return false;
	if (!expect_crc_punt(
			observe(profile, c2s::CHECKSUM_REPLY,
					indexed_crc_reply(0x18, 0)),
			"PUNT ACRC",
			"a wrong C2S 0x21 stages the exact retail ACRC description"))
		return false;
	if (!expect_crc_punt(
			observe(profile, c2s::CHECKSUM_REPLY, {}),
			"PUNT ACRC",
			"an empty C2S 0x21 leniently reads index zero and checksum zero"))
		return false;
	if (!expect_silent(
			observe(profile, c2s::CHECKSUM_REPLY,
					indexed_crc_reply(0xFF, 0)),
			"C2S 0x21 ignores a negative signed row index"))
		return false;
	if (!expect_silent(
			observe(profile, c2s::CHECKSUM_REPLY,
					indexed_crc_reply(0x66, 0)),
			"C2S 0x21 ignores a positive row index at the table capacity"))
		return false;
	return expect_silent(
			observe({}, c2s::CHECKSUM_REPLY,
					indexed_crc_reply(0x18, 0)),
			"C2S 0x21 is ignored when no integrity profile is selected");
}

// A retail client drops a 0x16 row until that slot has been introduced by 0x46. The
// retail host repairs the scoreboard at its 311-tick broadcast boundary: in the PR
// #403 oracle the joining client learns slot 1 via 0x46, then receives a two-row 0x16.
// Drive that public reply/tick sequence so a premature one-shot list cannot leave the
// HUD's "Number of players" pinned at one.
bool run_periodic_scoreboard_repairs_pre_sync_dropped_row() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	config.max_players = 4;
	config.player_name = "OpenNovaHost403";
	inmatch::test::bring_up_host(
			ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			kHostKey, nullptr, config);

	world::World world;
	world.registry.configure_pool(0, 4);
	world::Entity host_entity;
	host_entity.kind = world::EntityKind::Organic;
	host_entity.item_id = 0x14B9;
	host_entity.team = 1;
	const world::EntityHandle host_handle = world.registry.spawn(0, host_entity);
	world::Entity joiner_entity = host_entity;
	const world::EntityHandle joiner_handle = world.registry.spawn(0, joiner_entity);
	if (!expect(host_handle.packed == 0x0000 && joiner_handle.packed == 0x0001,
	            "scoreboard fixture owns retail slots 0 and 1")) return false;
	ctx.world = &world;

	inmatch::NapiNPConnection host;
	host.type = 2;
	host.phase = inmatch::ConnectionPhase::PlayerAdded;
	host.burst.spawned = true;
	host.link.owned_entity = host_handle;
	host.reply.player_slot = 0;
	host.reply.player_name = "OpenNovaHost403";
	host.reply.roster_counted = true;
	ctx.np_protocol.connection_list.push_back(std::move(host));

	const PeerAddr peer{0x0100007Fu, 32786};
	replication::UdpSessionTransport transport(
			replication::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection joiner;
	joiner.peer = peer;
	joiner.type = 1;
	joiner.phase = inmatch::ConnectionPhase::PlayerAdded;
	joiner.admission_stage = inmatch::GameAdmissionStage::Complete;
	joiner.burst.spawned = true;
	joiner.link.owned_entity = joiner_handle;
	joiner.link.mode = replication::TransportMode::Client;
	joiner.link.transport = &transport;
	joiner.reply.player_slot = 1;
	joiner.reply.player_name = "RetailJoin403";
	joiner.reply.roster_pushed = true;
	joiner.reply.roster_counted = true;
	joiner.reply.roster_seen_gen = ctx.np_protocol.roster_generation;
	joiner.server_scrk =
			"PERIODICSCOREBOARDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
	joiner.client_ck = 0x40340340u;
	ctx.np_protocol.connection_list.push_back(std::move(joiner));

	// The global mission counter—not a per-connection epoch—owns the repair.
	// Counter 309 advances to 310 without a list; the next call crosses 311.
	ctx.scoreboard_broadcast_timer = 309;
	inmatch::Server_TickUpdate(ctx);
	replication::Datagram staged;
	bool premature_list = false;
	while (transport.pop_outbound(staged))
		premature_list = premature_list || staged.tag == 0x16;
	if (!expect(!premature_list, "no scoreboard broadcast before boundary 311"))
		return false;

	inmatch::NapiNPConnection &remote = ctx.np_protocol.connection_list.back();
	const std::vector<ProtocolMessage> sync = inmatch::dispatch_session_replies(
			ctx.config, remote,
			{make_protocol_message(0x22, {0x01, 0xF7, 0x5C})},
			310, ctx.np_protocol.connection_list, &world);
	PlayerSync sync_row;
	bool learned_slot_one = false;
	for (const ProtocolMessage &message : sync) {
		if (message.tag != 0x46) continue;
		learned_slot_one = decode_player_sync(
				message.payload.data(), message.payload.size(), sync_row) &&
				sync_row.slot_id == 1 && (sync_row.field_bitmask & 0x4000u) != 0;
	}
	if (!expect(learned_slot_one, "retail slot walk learns slot 1 before the list repair"))
		return false;

	inmatch::Server_TickUpdate(ctx);
	PlayerList list;
	bool saw_repaired_list = false;
	while (transport.pop_outbound(staged)) {
		if (staged.tag != 0x16 ||
		    !decode_player_list(staged.body.data(), staged.body.size(), list))
			continue;
		saw_repaired_list = list.players.size() == 2 &&
				list.players[0].slot_id == 0 && list.players[1].slot_id == 1 &&
				list.in_game_count == 2 && list.spectator_count == 0;
	}
	if (expect(
			saw_repaired_list,
			"tick 311 re-broadcasts the two-row list after retail learns slot 1") == false)
		return false;

	// A spectator remains an in-game roster row, with flags bit 0 set and the
	// trailer count incremented. Retail uses both values to move the row into
	// the spectator column and subtract it from the HUD's player count.
	remote.link.spectator = true;
	world.registry.get(joiner_handle)->team = 0;
	ctx.scoreboard_broadcast_timer = 310;
	inmatch::Server_TickUpdate(ctx);
	bool saw_spectator_list = false;
	while (transport.pop_outbound(staged)) {
		if (staged.tag == 0x16 &&
		    decode_player_list(staged.body.data(), staged.body.size(), list)) {
			saw_spectator_list = list.players.size() == 2 &&
				list.players[1].slot_id == 1 && list.players[1].flags == 0x01 &&
				list.in_game_count == 2 && list.spectator_count == 1;
		}
	}
	return expect(saw_spectator_list,
			"periodic 0x16 marks and counts the spectator roster row");
}

} // namespace

int main() {
	bool ok = run_side_password_admission();
	ok = run_mission_transfers_match_retail_lan_contract() && ok;
	ok = run_tag60_mission_name_selects_by_game_type() && ok;
	ok = run_reactive_replies() && ok;
	ok = run_loadout_resolve_with_armory() && ok;
	ok = run_loadout_envelope_gates() && ok;
	ok = run_plain_join_tag29_draws_no_tag51() && ok;
	ok = run_goodbye_despawns_player_entity() && ok;
	ok = run_same_endpoint_reconnect_fully_tears_down_old_session() && ok;
	ok = run_inactive_peer_is_reaped() && ok;
	ok = run_disconnect_removes_leaver_from_spawn_waves() && ok;
	ok = run_never_template_disables_reap_and_is_advertised() && ok;
	ok = run_game_environment_and_admission_fsm_are_enforced() && ok;
	ok = run_non_jo_peer_is_ignored() && ok;
	ok = run_expansion_join_reasons_and_tlv_order() && ok;
	ok = run_client_join_rejects_match_retail() && ok;
	ok = run_handshake_rejected_when_host_down() && ok;
	ok = run_lan_discovery_metadata_is_live_and_stateless() && ok;
	ok = run_listen_host_lifecycle() && ok;
	ok = run_post_handshake_slot_is_reserved_until_spawn() && ok;
	ok = run_admission_spawn_and_roster_keep_retail_packet_boundaries() && ok;
	ok = run_full_player_info_is_recipient_scoped_lan_metadata() && ok;
	ok = run_full_player_info_selects_retail_mission_title_branch() && ok;
	ok = run_retransmit_0x42_keeps_keys() && ok;
	ok = run_spectator_admission_codes_match_retail() && ok;
	ok = run_capacity_rejects_when_full() && ok;
	ok = run_character_join_vars_parsed() && ok;
	ok = run_integrity_replies_validate_registered_profile() && ok;
	ok = run_periodic_scoreboard_repairs_pre_sync_dropped_row() && ok;
	return ok ? 0 : 1;
}
