// P5 — np::ClientRuntime (the headless Client_ProcessNetworkFrame role), always-on:
//
//  (A) Full in-process round-trip — client_runtime <-> the REAL np server legs <-> Server_TickUpdate
//      + the production apply_in_match_c2s consumer + the NetClientView S2C fold:
//        handshake (0x41/0x42) via ClientRuntime.start()/receive()/Client_ProcessNetworkFrame()
//        -> the spawn-gate burst (driven from the per-frame client role) -> PeerSpawned
//        -> the owner binds the joiner connection's transport + streams a NAMED organic-spawn
//        -> client name-matches + receives the deployment release -> InMatch (learns wire handle H)
//        -> each frame: ClientRuntime emits a framed C2S 0x0C -> handle_server_datagram surfaces
//           PeerC2SInMatch -> apply_in_match_c2s deliver_c2s's it onto the connection's transport
//           -> Server_TickUpdate drains+SNAPs the entity + fans an S2C 0x0A
//        -> the owner reframes that 0x0A as a 0x83 -> ClientRuntime folds it into ClientState.
//      Asserts the peer SNAPs to the uplink AND the client's ClientState reflects the server's 0x0A
//      (exactly one SNAP per 0x0C). This is the P5 e2e bar and the FIRST coverage of NetClientView
//      fold + the production PeerC2SInMatch consumer (joiner_connection_test does neither).
//
//  (B) Host-as-client (D-NET-121/122) — the SP listen-server host's OWN loopback view: Server_TickUpdate
//      fans the host loopback a per-frame 0x0A (is_in_match) anchored to the host player's owned_entity
//      (NOT the dvxi5 fallback), and a HostClient ClientRuntime folds it off the loopback. Guards that
//      the host's own local view is no longer starved and anchors correctly.

#include <npruntime/client_runtime.h>
#include <npruntime/host_session.h>
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_protocol.h>
#include <npruntime/server_spawn.h>
#include <npruntime/server_tick.h>

#include "host_test_setup.h"

#include <netsim/connection.h>
#include <netsim/idatagram_socket.h>
#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/vehicle_attach.h>
#include <world/world.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

std::vector<uint8_t> make_organic_spawn(
		uint16_t slot_id, const std::string &name, int32_t x,
		int32_t y, int32_t z, int32_t orient, uint8_t team,
		uint16_t net_id);

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

std::vector<uint8_t> frame_server_session(SessionSequencing &seq,
		const std::string &server_scrk, uint32_t client_key,
		const std::vector<ProtocolMessage> &messages) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(
			seq, SessionCrypto{server_scrk, {}, client_key}, messages, body)) {
		return {};
	}
	return nw_encode_outbound(
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

bool decode_client_session(const std::vector<uint8_t> &datagram,
		const std::string &client_scrk, ProtocolPacketHeader &header,
		std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
			decode_protocol_packet_plaintext(
					body.data(), body.size(), client_scrk, header, messages);
}

const std::vector<uint8_t> &retail_join_request() {
	static const std::vector<uint8_t> body = {
			'V', 'E', 'R', 'S', 'I', 'O', 'N', 'C',
			'R', 'C', 'S', 'T', 'R', 'I', 'N', 'G',
			0x00, 0x02, 0x00, 0x30, 0x00,
	};
	return body;
}

std::vector<uint8_t> retail_expansion_join_request(std::string_view expansion) {
	std::vector<uint8_t> body = {
			'E', 'X', 'P', 0x00,
			static_cast<uint8_t>(expansion.size() + 1), 0x00,
	};
	body.insert(body.end(), expansion.begin(), expansion.end());
	body.push_back(0);
	const std::vector<uint8_t> &crc = retail_join_request();
	body.insert(body.end(), crc.begin(), crc.end());
	return body;
}

const std::vector<uint8_t> &retail_full_player_info() {
	static const std::vector<uint8_t> body = {
			'R', 'e', 't', 'a', 'i', 'l', 'P', 'r', 'e', 'l', 'u', 'd', 'e', 0x00,
			0x00,
			'R', 'e', 't', 'a', 'i', 'l', ' ', 'H', 'o', 's', 't', 0x00,
			'R', 'e', 't', 'a', 'i', 'l', ' ', 'M', 'i', 's', 's', 'i', 'o', 'n', 0x00,
			'R', 'E', 'T', 'A', 'I', 'L', '.', 'B', 'M', 'S', 0x00,
			0x20, 0x00, 0x03, 0x00,
			0x00,
			'J', 'o', 'i', 'n', 't', ' ', 'O', 'p', 'e', 'r', 'a', 't', 'i', 'o', 'n', 's', 0x00,
	};
	return body;
}

std::vector<uint8_t> retail_transfer_chunk(
		uint32_t transfer_id, uint32_t total_size, uint8_t fill) {
	std::vector<uint8_t> body;
	auto append_u32 = [&](uint32_t value) {
		body.push_back(static_cast<uint8_t>(value));
		body.push_back(static_cast<uint8_t>(value >> 8));
		body.push_back(static_cast<uint8_t>(value >> 16));
		body.push_back(static_cast<uint8_t>(value >> 24));
	};
	append_u32(transfer_id);
	append_u32(total_size);
	append_u32(0);
	body.insert(body.end(), total_size, fill);
	return body;
}

bool matches_client_header(const ProtocolPacketHeader &header,
		uint32_t session_id, uint32_t sequence, uint32_t ack) {
	return header.session_id == session_id &&
			header.seq_num == sequence &&
			header.ack_count == ack &&
			header.connection_flags == 0;
}

bool run_seeded_objective_layout_hint() {
	np::ClientRuntime client("Replay");
	client.seed_session(0x1234u, "client-key", "server-key",
	                    7, 6, 0x0001, w::kPlayerInfantryTypeId, 0x30020u);
	if (!expect(client.view().game_type() == 0x30020u,
	            "midstream replay seeds the off-wire objective layout hint")) return false;

	FrameUpdate frame;
	frame.flags2 = 3;
	frame.objective.present = true;
	frame.objective.state[0] = 1;
	frame.objective.state[1] = 2;
	frame.objective.state[2] = 4;
	frame.objective.state[3] = 8;
	frame.mount_handle = 0xFFFF;
	frame.health = 100;
	client.view().apply(0x0A, encode_frame_update(frame));
	return expect(client.state().objective_updates_applied == 1 &&
	                      client.state().objective_won == 1 &&
	                      client.state().local_health == 100,
	              "seeded replay folds objective body before recipient tail");
}

bool run_fire_queue_stamps_runtime_tick() {
	const std::string client_scrk = "CLIENT-FIRE-TICK-SCRK";
	const std::string server_scrk = "SERVER-FIRE-TICK-SCRK";
	constexpr uint16_t self_handle = 0x0002;
	np::ClientRuntime client("Shooter");
	client.seed_session(0x11223344u, client_scrk, server_scrk,
	                    1, 0, self_handle, w::kPlayerInfantryTypeId);

	// Retail's packet producer reads the client network role's currentTick, not
	// the independently-started World::logic_tick. Advance three network frames
	// before queueing so a caller-supplied/world tick cannot pass accidentally.
	for (uint32_t tick = 0; tick < 3; ++tick) {
		if (!expect(client.Client_ProcessNetworkFrame(1000 + tick).empty(),
		            "seeded replay idle frame emits no housekeeping"))
			return false;
	}

	ClientFiredRound fire;
	fire.current_tick = 0xDEADBEEFu; // poison: the runtime owns this wire field
	fire.shooter_handle = self_handle;
	if (!expect(client.queue_fired_round(fire), "in-match self fire queues"))
		return false;

	const std::vector<std::vector<uint8_t>> outbound =
			client.Client_ProcessNetworkFrame(1003);
	if (!expect(outbound.size() == 1, "queued fire emits one seeded-replay datagram"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(nw_decode_inbound(outbound[0].data(), outbound[0].size(), opcode, body) &&
	                    opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
	                    decode_protocol_packet_plaintext(body.data(), body.size(), client_scrk,
	                                                     header, messages),
	            "decode queued C2S fire session packet"))
		return false;
	if (!expect(messages.size() == 1 && messages[0].tag == 0x06,
	            "queued gameplay packet contains C2S 0x06"))
		return false;

	ClientFiredRound decoded;
	size_t consumed = 0;
	if (!expect(decode_client_fired_round(messages[0].payload.data(),
	                                      messages[0].payload.size(), decoded, consumed) &&
	                    consumed == 45,
	            "decode runtime-produced fixed C2S 0x06 body"))
		return false;
	return expect(decoded.current_tick == 3,
	              "C2S 0x06 uses the current runtime tick at queue time");
}

bool run_retail_post_auth_prelude() {
	constexpr uint32_t kServerKey = 0x11223344u;
	constexpr uint32_t kConnectionId = 3;
	const std::string server_scrk = "SERVER-RETAIL-PRELUDE-SCRK";
	np::JoinerConnection joiner("RetailPrelude");

	// Drive the real 0x41/0x42 builders so the post-auth fixture uses this
	// connection's live client key and SCRK.
	const std::vector<uint8_t> hello_datagram = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_HELLO &&
				parse_client_hello(body.data(), body.size(), hello),
			"decode retail-prelude ClientHello")) {
		return false;
	}

	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	server_hello.sus2 = "revx02";
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const np::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	if (!expect(hello_result.outbound.size() == 1,
			"ServerHello emits one ClientAuth")) {
		return false;
	}

	ClientAuth client_auth;
	if (!expect(nw_decode_inbound(
				hello_result.outbound[0].data(), hello_result.outbound[0].size(),
				opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), client_auth),
			"decode retail-prelude ClientAuth")) {
		return false;
	}
	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, server_scrk,
			"", "", "", false);
	server_auth.mi = kConnectionId;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	const np::JoinerConnection::PollResult auth_result = joiner.handle_datagram(
			server_auth_datagram.data(), server_auth_datagram.size());
	if (!expect(auth_result.outbound.empty(),
			"ServerAuth waits for retail's initial sequenced settings packet")) {
		return false;
	}

	// Golden retail frame 6: two settings-update records in S2C sequence 1.
	// The client acknowledges that packet with an empty sequence 1, then sends
	// the exact JOIN request in sequence 2.
	SessionSequencing server_seq = np::make_jo_game_session_sequencing();
	const std::vector<ProtocolMessage> initial_settings = {
			make_protocol_message(
					0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00},
					0xA0),
			make_protocol_message(
					0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00},
					0xA0),
	};
	const std::vector<uint8_t> settings_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck, initial_settings);
	const np::JoinerConnection::PollResult settings_result =
			joiner.handle_datagram(settings_datagram.data(), settings_datagram.size());
	if (!expect(settings_result.outbound.size() == 2,
			"initial settings emit header-only ACK then JOIN")) {
		return false;
	}

	ProtocolPacketHeader client_header;
	std::vector<ProtocolMessage> client_messages;
	if (!expect(decode_client_session(
				settings_result.outbound[0], client_auth.scrk,
				client_header, client_messages) &&
				settings_result.outbound[0].size() == 18 &&
				client_messages.empty() &&
				matches_client_header(client_header, kServerKey, 1, 1),
			"first post-auth C2S packet is retail's header-only ACK")) {
		return false;
	}

	if (!expect(decode_client_session(
				settings_result.outbound[1], client_auth.scrk,
				client_header, client_messages) &&
				settings_result.outbound[1].size() == 55 &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x00 &&
				client_messages[0].full_tag == 0x00 &&
				client_messages[0].flags.raw == 0x20 &&
				client_messages[0].payload ==
						retail_expansion_join_request(server_hello.sus2) &&
				matches_client_header(client_header, kServerKey, 2, 1),
			"second post-auth C2S packet carries retail EXP then VERSIONCRCSTRING")) {
		return false;
	}

	// Golden retail frame 9: the host acknowledges JOIN with an empty S2C
	// 0x00. Retail answers with another header-only ACK and C2S 0x01 {0}.
	server_seq.last_inbound_seq = 2;
	const std::vector<uint8_t> join_ack_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x00, {})});
	const np::JoinerConnection::PollResult join_ack_result =
			joiner.handle_datagram(join_ack_datagram.data(), join_ack_datagram.size());
	if (!expect(join_ack_result.outbound.size() == 2,
			"S2C JOIN ack emits header-only ACK then form post")) {
		return false;
	}
	if (!expect(decode_client_session(
				join_ack_result.outbound[0], client_auth.scrk,
				client_header, client_messages) &&
				join_ack_result.outbound[0].size() == 18 &&
				client_messages.empty() &&
				matches_client_header(client_header, kServerKey, 3, 2),
			"JOIN response receives retail's header-only ACK")) {
		return false;
	}
	if (!expect(decode_client_session(
				join_ack_result.outbound[1], client_auth.scrk,
				client_header, client_messages) &&
				join_ack_result.outbound[1].size() == 22 &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x01 &&
				client_messages[0].full_tag == 0x01 &&
				client_messages[0].flags.raw == 0x20 &&
				client_messages[0].payload == std::vector<uint8_t>{0x00} &&
				matches_client_header(client_header, kServerKey, 4, 2),
			"form post carries retail's required one-byte zero body")) {
		return false;
	}

	// The existing padding response remains the final prelude leg, now reached
	// through the retail ordering rather than OpenNova's former 0x01 shortcut.
	std::vector<uint8_t> padding_probe(512, 0xA5);
	auto write_u32 = [&](std::size_t offset, uint32_t value) {
		padding_probe[offset + 0] = static_cast<uint8_t>(value);
		padding_probe[offset + 1] = static_cast<uint8_t>(value >> 8);
		padding_probe[offset + 2] = static_cast<uint8_t>(value >> 16);
		padding_probe[offset + 3] = static_cast<uint8_t>(value >> 24);
	};
	write_u32(0, 0x0057673Eu);
	write_u32(4, 0x00000002u);
	write_u32(8, 256);
	server_seq.last_inbound_seq = 4;
	const std::vector<uint8_t> padding_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x02, std::move(padding_probe))});
	const np::JoinerConnection::PollResult padding_result =
			joiner.handle_datagram(padding_datagram.data(), padding_datagram.size());
	if (!expect(padding_result.outbound.size() == 1 &&
				decode_client_session(
						padding_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				padding_result.outbound[0].size() == 278 &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x02 &&
				client_messages[0].full_tag == 0x02 &&
				client_messages[0].flags.raw == 0x40 &&
				client_messages[0].payload.size() == 256 &&
				matches_client_header(client_header, kServerKey, 5, 3),
			"retail challenge receives the sequenced 256-byte padding echo")) {
		return false;
	}
	if (!expect(
			client_messages[0].payload[0] == 0x3E &&
				client_messages[0].payload[1] == 0x67 &&
				client_messages[0].payload[2] == 0x57 &&
				client_messages[0].payload[3] == 0x00 &&
				client_messages[0].payload[4] == 0x02 &&
				client_messages[0].payload[5] == 0x00 &&
				client_messages[0].payload[6] == 0x00 &&
				client_messages[0].payload[7] == 0x00,
			"padding echo preserves the retail challenge position")) {
		return false;
	}

	// Retail frame 14 is a post-handshake metadata bundle. While mission loading
	// holds the gameplay drive, the client still sends frame 15's header-only ACK
	// so the reliable server stream can advance.
	joiner.set_world_ready(false);
	server_seq.last_inbound_seq = 5;
	const std::vector<uint8_t> metadata_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x03, {0x01})});
	const np::JoinerConnection::PollResult metadata_result =
			joiner.handle_datagram(metadata_datagram.data(), metadata_datagram.size());
	if (!expect(metadata_result.outbound.empty(),
			"metadata with no semantic response defers its ACK to the send boundary")) {
		return false;
	}
	const std::vector<std::vector<uint8_t>> metadata_ack = joiner.pump(0);
	if (!expect(metadata_ack.size() == 1 &&
				metadata_ack[0].size() == 18 &&
				decode_client_session(
						metadata_ack[0], client_auth.scrk,
						client_header, client_messages) &&
				client_messages.empty() &&
				matches_client_header(client_header, kServerKey, 6, 4),
			"held mission load still ACKs the post-handshake metadata packet")) {
		return false;
	}

	// Golden retail frame 16: the game-start flag is carried after a second
	// settings pair and before the authoritative full-player-info row. Retail
	// reacts immediately, even while the local mission/world remains held.
	server_seq.last_inbound_seq = 6;
	const std::vector<uint8_t> game_start_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(0x03, {0x00}),
					initial_settings[0],
					initial_settings[1],
					make_protocol_message(0x05, {0x01}),
					make_protocol_message(0x04, std::vector<uint8_t>(24, 0)),
					make_protocol_message(0x7B, retail_full_player_info()),
			});
	const np::JoinerConnection::PollResult game_start_result =
			joiner.handle_datagram(
					game_start_datagram.data(), game_start_datagram.size());
	if (!expect(game_start_result.outbound.size() == 1 &&
				decode_client_session(
						game_start_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 7, 5) &&
				client_messages.size() == 5 &&
				client_messages[0].tag == 0x4E &&
				client_messages[0].payload == std::vector<uint8_t>(4, 0) &&
				client_messages[1].tag == 0x03 &&
				client_messages[1].payload == std::vector<uint8_t>(4, 0) &&
				client_messages[2].tag == 0x48 &&
				client_messages[2].payload ==
						std::vector<uint8_t>({0x03, 0x00, 0x00, 0x00}) &&
				client_messages[3].tag == 0x47 &&
				client_messages[3].payload.empty() &&
				client_messages[4].tag == 0x33 &&
				client_messages[4].payload == std::vector<uint8_t>(8, 0),
			"game-start emits retail frame 17's grouped pre-world admission request")) {
		return false;
	}
	if (!expect(joiner.mission_known() && !joiner.world_ready(),
			"full-player-info advertises the mission without releasing the local world hold")) {
		return false;
	}

	// Golden frames 18-20: completion of the server-info transfer emits two
	// distinct packets, never a bundled or empty-body approximation.
	server_seq.last_inbound_seq = 7;
	const std::vector<uint8_t> server_info_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(0x75, {0x00, 0x02}),
					make_protocol_message(
							0x60, retail_transfer_chunk(1, 171, 0xA5)),
			});
	const np::JoinerConnection::PollResult server_info_result =
			joiner.handle_datagram(
					server_info_datagram.data(), server_info_datagram.size());
	if (!expect(server_info_result.outbound.size() == 2,
			"final server-info chunk emits retail's two response packets")) {
		return false;
	}
	if (!expect(decode_client_session(
				server_info_result.outbound[0], client_auth.scrk,
				client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 8, 6) &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x47 &&
				client_messages[0].payload.empty(),
			"retail frame 19 is a standalone empty 0x47")) {
		return false;
	}
	if (!expect(decode_client_session(
				server_info_result.outbound[1], client_auth.scrk,
				client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 9, 6) &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x37 &&
				client_messages[0].payload == std::vector<uint8_t>(8, 0),
			"retail frame 20 is a standalone eight-zero 0x37")) {
		return false;
	}

	// Golden frames 21-23: the final mission-data chunk is followed by a valid
	// player list. The client carries the pending ACK into ONE grouped 0x09+0x22
	// packet instead of manufacturing an intervening header-only sequence.
	server_seq.last_inbound_seq = 9;
	const std::vector<uint8_t> mission_data_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(0x75, {0x00, 0x02}),
					make_protocol_message(
							0x64, retail_transfer_chunk(1, 180, 0x5A)),
			});
	const np::JoinerConnection::PollResult mission_data_result =
			joiner.handle_datagram(
					mission_data_datagram.data(), mission_data_datagram.size());
	if (!expect(mission_data_result.outbound.empty(),
			"final mission-data chunk waits for the player-list boundary")) {
		return false;
	}

	const std::vector<uint8_t> player_list_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(
					0x16, encode_player_list({{0, 1}, {1, 2}}))});
	const np::JoinerConnection::PollResult player_list_result =
			joiner.handle_datagram(
					player_list_datagram.data(), player_list_datagram.size());
	if (!expect(player_list_result.outbound.size() == 1 &&
				decode_client_session(
						player_list_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 10, 8) &&
				client_messages.size() == 2 &&
				client_messages[0].tag == 0x09 &&
				client_messages[0].payload.empty() &&
				client_messages[1].tag == 0x22 &&
				client_messages[1].payload ==
						std::vector<uint8_t>({0x00, 0xF7, 0x1C}),
			"player list emits retail frame 23's grouped 0x09+0x22 packet")) {
		return false;
	}

	// S2C 0x11 is the safe preload boundary. While the binding still holds the
	// advertised mission, the only allowed response is its send-boundary ACK:
	// no spawn-menu 0x0A and no loadout/status packet may escape.
	server_seq.last_inbound_seq = 10;
	const std::vector<uint8_t> sync_tail_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x11, {})});
	const np::JoinerConnection::PollResult sync_tail_result =
			joiner.handle_datagram(
					sync_tail_datagram.data(), sync_tail_datagram.size());
	if (!expect(sync_tail_result.outbound.empty(),
			"terminal sync tail remains held before the local world is ready")) {
		return false;
	}
	const std::vector<std::vector<uint8_t>> sync_tail_ack = joiner.pump(0);
	if (!expect(sync_tail_ack.size() == 1 &&
				decode_client_session(
						sync_tail_ack[0], client_auth.scrk,
						client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 11, 9) &&
				client_messages.empty() &&
				joiner.preload_ready(),
			"terminal 0x11 reaches preload_ready with only a header-only ACK")) {
		return false;
	}
	if (!expect(joiner.pump(0).empty(),
			"preload hold prohibits 0x0A and loadout before world_ready")) {
		return false;
	}

	// Installing the advertised mission releases exactly the empty 0x0A
	// spawn-menu request. Loadout remains reactive to the later world-stream
	// terminator rather than sharing this boundary.
	joiner.set_world_ready(true);
	const std::vector<std::vector<uint8_t>> world_release = joiner.pump(0);
	if (!expect(world_release.size() == 1 &&
				decode_client_session(
						world_release[0], client_auth.scrk,
						client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 12, 9) &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x0A &&
				client_messages[0].payload.empty(),
			"world_ready releases only the empty spawn-menu request")) {
		return false;
	}

	server_seq.last_inbound_seq = 12;
	const std::vector<uint8_t> world_end_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x1A, {})});
	const np::JoinerConnection::PollResult world_end_result =
			joiner.handle_datagram(
					world_end_datagram.data(), world_end_datagram.size());
	if (!expect(world_end_result.outbound.size() == 1 &&
				decode_client_session(
						world_end_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				matches_client_header(client_header, kServerKey, 13, 10) &&
				client_messages.size() == 3 &&
				client_messages[0].tag == 0x2F &&
				!client_messages[0].payload.empty() &&
				client_messages[1].tag == 0x2F &&
				!client_messages[1].payload.empty() &&
				client_messages[0].payload != client_messages[1].payload &&
				client_messages[2].tag == 0x0B &&
				!client_messages[2].payload.empty(),
			"world-stream 0x1A triggers one grouped loadout/status packet")) {
		return false;
	}

	// The player's named organic record arrives during the world stream. Knowing
	// the wire handle is necessary but must not release gameplay while retail's
	// spawn-zone deployment gate is still pending.
	constexpr uint16_t kRetailSelfHandle = 0x00C6;
	server_seq.last_inbound_seq = 13;
	const std::vector<uint8_t> self_spawn_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(
					0x0C,
					make_organic_spawn(
							kRetailSelfHandle, "RetailPrelude",
							0x10000, 0x20000, 0x30000, 0, 1, 7))});
	const np::JoinerConnection::PollResult self_spawn_result =
			joiner.handle_datagram(
					self_spawn_datagram.data(), self_spawn_datagram.size());
	if (!expect(self_spawn_result.outbound.empty() &&
				joiner.has_self_handle() &&
				joiner.self_handle() == kRetailSelfHandle &&
				!joiner.in_match(),
			"self name-match remains hidden before the retail deployment release")) {
		return false;
	}

	// The first 0x5A grants the submitted loadout but cannot decide deployment
	// before 0x0F supplies gameFlags. Split the grant and policy across packets
	// to pin both retail's same-packet ordering and OpenNova's later-0x0F order.
	const std::vector<uint8_t> first_grant_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(
					0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF})});
	const np::JoinerConnection::PollResult first_grant_result =
			joiner.handle_datagram(
					first_grant_datagram.data(), first_grant_datagram.size());
	if (!expect(first_grant_result.outbound.empty() && !joiner.in_match(),
			"loadout grant waits while the deployment policy is unknown")) {
		return false;
	}

	std::vector<uint8_t> zoned_world_state(23, 0);
	zoned_world_state[22] = 0x01;
	const std::vector<uint8_t> deployment_policy_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x0F, std::move(zoned_world_state))});
	const np::JoinerConnection::PollResult granted_loadout_result =
			joiner.handle_datagram(
					deployment_policy_datagram.data(),
					deployment_policy_datagram.size());
	if (!expect(granted_loadout_result.outbound.empty() &&
				!joiner.in_match(),
			"spawn-zone policy still waits for the second profile-side grant")) {
		return false;
	}

	// The second initial grant completes the pair and emits exactly one deploy
	// pick. Record its server sequence so the duplicate-retransmit case below
	// can rebuild that old message with a newer ACK.
	server_seq.last_inbound_seq = 13;
	const uint32_t second_grant_sequence = server_seq.next_outbound_seq;
	const std::vector<uint8_t> split_second_grant_datagram =
			frame_server_session(
					server_seq, server_scrk, client_auth.ck,
					{make_protocol_message(
							0x5A,
							{0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF})});
	const np::JoinerConnection::PollResult split_second_grant_result =
			joiner.handle_datagram(
					split_second_grant_datagram.data(),
					split_second_grant_datagram.size());
	if (!expect(split_second_grant_result.outbound.size() == 1 &&
				decode_client_session(
						split_second_grant_result.outbound[0],
						client_auth.scrk, client_header, client_messages) &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x0E &&
				client_messages[0].payload ==
						std::vector<uint8_t>({0xFF, 0xFF}) &&
				!joiner.in_match(),
			"granted loadout emits one retail deploy pick without entering the match")) {
		return false;
	}

	// A retained retransmit reuses the old server sequence but carries the
	// sender's current ACK, which now covers our 0x0E. Reliable deduplication
	// must suppress that old grant rather than treating it as the release.
	server_seq.last_inbound_seq = client_header.seq_num;
	std::vector<uint8_t> retransmit_body;
	if (!expect(frame_session_packet_for_sequence(
				server_seq,
				SessionCrypto{server_scrk, {}, client_auth.ck},
				second_grant_sequence, retransmit_body),
			"rebuild retained second grant with the post-pick ACK")) {
		return false;
	}
	const std::vector<uint8_t> retransmitted_second_grant =
			nw_encode_outbound(
					SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
					std::move(retransmit_body));
	const np::JoinerConnection::PollResult retransmitted_grant_result =
			joiner.handle_datagram(
					retransmitted_second_grant.data(),
					retransmitted_second_grant.size());
	if (!expect(retransmitted_grant_result.outbound.empty() &&
				!retransmitted_grant_result.reached_in_match &&
				!joiner.in_match(),
			"ACK-updated retransmit of an initial grant cannot release the player")) {
		return false;
	}

	const std::vector<uint8_t> deploy_release_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(
							0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF}),
					make_protocol_message(0x61, {0x00, 0x00, 0xED, 0x00}),
			});
	const np::JoinerConnection::PollResult deploy_release_result =
			joiner.handle_datagram(
					deploy_release_datagram.data(),
					deploy_release_datagram.size());
	if (!expect(deploy_release_result.outbound.empty() &&
				deploy_release_result.reached_in_match &&
				joiner.in_match() &&
				joiner.self_handle() == kRetailSelfHandle,
			"post-pick 0x5A releases the hidden retail player into the match")) {
		return false;
	}
	return true;
}

bool run_duplicate_s2c_session_one_shot_is_not_replayed() {
	const std::string client_scrk = "CLIENT-REPLAY-SCRK";
	const std::string server_scrk = "SERVER-REPLAY-SCRK";
	np::JoinerConnection joiner("Replay");
	joiner.seed_in_match(0x11223344u, client_scrk, server_scrk,
	                     1, 0, 0x0001, w::kPlayerInfantryTypeId);

	WeaponReload reload;
	reload.entity_handle = 0x0001;
	reload.reload_param = 0x00C5;
	SessionSequencing server_tx{1, 0};
	std::vector<uint8_t> first_body;
	if (!expect(frame_session_packet(server_tx, SessionCrypto{server_scrk, {}, 1},
	                                 {make_protocol_message(0x49, encode_weapon_reload(reload))},
	                                 first_body),
	            "frame first S2C 0x49 session packet"))
		return false;
	const std::vector<uint8_t> first =
			nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(first_body));

	np::JoinerConnection::PollResult initial =
			joiner.handle_datagram(first.data(), first.size());
	if (!expect(initial.inbound_gameplay.size() == 1 &&
	                    initial.inbound_gameplay[0].first == 0x49,
	            "first S2C 0x83 surfaces its one-shot reload event"))
		return false;
	np::JoinerConnection::PollResult duplicate =
			joiner.handle_datagram(first.data(), first.size());
	if (!expect(duplicate.inbound_gameplay.empty(),
	            "exact duplicate S2C 0x83 does not replay its one-shot event"))
		return false;

	reload.reload_param = 0x00C6;
	std::vector<uint8_t> second_body;
	if (!expect(frame_session_packet(server_tx, SessionCrypto{server_scrk, {}, 1},
	                                 {make_protocol_message(0x49, encode_weapon_reload(reload))},
	                                 second_body),
	            "frame next contiguous S2C 0x49 session packet"))
		return false;
	const std::vector<uint8_t> second =
			nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(second_body));
	if (!expect(joiner.handle_datagram(second.data(), second.size()).inbound_gameplay.size() == 1,
	            "next contiguous S2C 0x83 dispatches"))
		return false;
	if (!expect(joiner.handle_datagram(first.data(), first.size()).inbound_gameplay.empty(),
	            "older replayed S2C 0x83 stays suppressed"))
		return false;
	if (!expect(joiner.connection().seq.last_inbound_seq == 2,
	            "older replay cannot regress the joiner ACK latch"))
		return false;

	const std::vector<uint8_t> ack = joiner.frame_inner(0x34, {});
	uint8_t opcode = 0;
	std::vector<uint8_t> ack_body;
	ProtocolPacketHeader ack_hdr;
	std::vector<ProtocolMessage> ack_messages;
	if (!expect(nw_decode_inbound(ack.data(), ack.size(), opcode, ack_body) &&
	                    opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
	                    decode_protocol_packet_plaintext(ack_body.data(), ack_body.size(), client_scrk,
	                                                     ack_hdr, ack_messages),
	            "decode joiner ACK-bearing C2S packet"))
		return false;
	return expect(ack_hdr.ack_count == 2, "joiner echoes the highest contiguous S2C sequence");
}

struct NullDatagramSocket final : ns::IDatagramSocket {
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

struct HostPumpHookProbe {
	np::HostOwner *owner = nullptr;
	w::World *world = nullptr;
	w::AiSystem *ai = nullptr;
	PeerAddr peer{};
	int calls = 0;
	w::EntityHandle spawned{};
	bool saw_spawned_connection = false;
	bool saw_live_entity = false;
	bool saw_ai_component = false;
	bool saw_before_logic = false;
	bool saw_before_fan = false;
};

void observe_host_before_server_tick(void *opaque) {
	auto &probe = *static_cast<HostPumpHookProbe *>(opaque);
	++probe.calls;
	for (np::NapiNPConnection &conn : probe.owner->ctx.np_protocol.connection_list) {
		if (!(conn.peer == probe.peer)) continue;
		probe.spawned = conn.link.owned_entity;
		probe.saw_spawned_connection =
				conn.burst.spawned && conn.phase == np::ConnectionPhase::Spawned;
		probe.saw_live_entity = probe.spawned.valid() &&
				probe.world->registry.get(probe.spawned) != nullptr;
		probe.saw_ai_component = probe.spawned.valid() &&
				probe.ai->for_handle(probe.spawned) != nullptr;
		probe.saw_before_logic = probe.world->logic_tick == 0;
		probe.saw_before_fan = conn.link.s2c_phase == 0;
		return;
	}
}

struct FirstLogicTickProbe final : w::ISystem {
	HostPumpHookProbe *hook = nullptr;
	int ticks = 0;
	bool first_tick_saw_hook = false;
	bool first_tick_saw_player = false;

	const char *name() const override { return "host-before-server-tick-order"; }
	void tick(w::World &world, const w::TickContext &) override {
		++ticks;
		if (ticks != 1) return;
		first_tick_saw_hook = hook != nullptr && hook->calls == 1;
		first_tick_saw_player = hook != nullptr && hook->spawned.valid() &&
				world.registry.get(hook->spawned) != nullptr;
	}
};

w::PlayerSpawn player_spawn(w::Vec3 pos, int16_t yaw, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = pos;
	s.yaw = yaw;
	s.net_id = net_id;
	return s;
}

// A 1-record S2C 0x0C organic-spawn body the joiner name-matches (the owner's PeerSpawned reaction,
// mirroring NovaSimulation / joiner_connection_test).
std::vector<uint8_t> make_organic_spawn(uint16_t slot_id, const std::string &name, int32_t x,
                                        int32_t y, int32_t z, int32_t orient, uint8_t team,
                                        uint16_t net_id) {
	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord rec;
	rec.slot_id = slot_id;
	rec.has_body = true;
	rec.item_type_id = 0x14B9;
	rec.entity_name = name;
	rec.pos_x = x;
	rec.pos_y = y;
	rec.pos_z = z;
	rec.orientation = orient;
	rec.team = team;
	rec.net_id = net_id;
	batch.records.push_back(rec);
	return encode_organic_spawn_batch(batch);
}

// ---------------------------------------------------------------------------------------------------
// (A) Full in-process round-trip: a remote joiner via ClientRuntime.
// ---------------------------------------------------------------------------------------------------
bool run_roundtrip() {
	const PeerAddr peer{0x0100007Fu, 30000}; // 127.0.0.1:30000
	const std::string kName = "JoinerOne";

	np::NapiNPServerCtx ctx;
	// HostClient listen host (mode 3). local_client = nullptr: no host loopback connection in this
	// run — the only connection is the joiner, keeping the round-trip focused (the host loopback path
	// is run (B)). The host advertises this HK; ClientRuntime echoes it so the join HK gate passes.
	np::GameConfig host_config;
	host_config.game_type = 0x30020u; // captured Co-op: phase-3 objective layout is active
	host_config.server_name = "Retail Sequence Host";
	host_config.mission_name = "Cooperative Test Mission";
	host_config.mission_file = "COOP_TEST.BMS";
	host_config.expansion = "jox01";
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        0x0FE0E112u, nullptr, host_config);

	// The host's authoritative World. ctx.world set BEFORE the join so tick_connections'
	// Server_ProcessPendingPlayerSpawns spawns the joiner's pool-0 entity (binding conn.link.owned_entity).
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	world.subgoals.won = 0x12u;
	world.subgoals.lost = 0x24u;
	world.subgoals.show_win = 0x48u;
	world.subgoals.show_lose = 0x90u;
	ctx.world = &world;
	// The host's own local player (sets cached.local_player, which apply_player_intent refuses to snap).
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host's own player spawned (cached.local_player)")) return false;

	np::ClientRuntime client(kName);
	client.set_world_ready(false); // discover the authoritative mission before installing its world

	uint32_t tick = 1;
	bool spawned = false;
	bool saw_join_00 = false;
	bool join_00_shape_ok = false;
	bool saw_join_01 = false;
	bool join_01_shape_ok = false;
	bool saw_join_02 = false;
	bool join_02_shape_ok = false;
	bool saw_client_auth = false;
	bool client_auth_has_retail_environment = false;
	bool saw_world_drive_while_held = false;
	std::string spawn_name;
	auto note_event = [&](const np::HostAcceptEvent &e) {
		if (e.kind == np::HostAcceptEvent::Kind::PeerSpawned && !spawned) {
			spawned = true;
			spawn_name = e.peer_name;
		}
	};
	// Dispatch one client->host datagram and feed every host reply (ServerHello/ServerAuth/0x83) back
	// into the client's recv FIFO.
	auto pump_host = [&](std::vector<uint8_t> dg) {
		if (dg.empty()) return;
		// Inspect the real framed client stream before the server consumes it. This pins the witnessed
		// retail post-auth sequence and the 256-byte position/padding response shape.
		uint8_t opcode = 0;
		std::vector<uint8_t> session_body;
		if (nw_decode_inbound(dg.data(), dg.size(), opcode, session_body) &&
		    opcode == SESSION_OPCODE_CLIENT_AUTH) {
			ClientAuth auth;
			if (parse_client_auth(session_body.data(), session_body.size(), auth)) {
				saw_client_auth = true;
				bool saw_vn = false;
				bool saw_bn = false;
				bool saw_mbn = false;
				bool saw_sopd = false;
				for (const std::vector<uint8_t> &blob : auth.cu) {
					uint8_t type = 0;
					std::string name;
					std::string value;
					if (!parse_client_cu_chunk(
							blob.data(), blob.size(), type, name, value) ||
					    type != 2) {
						continue;
					}
					saw_vn |= name == "VN" && value == "2";
					saw_bn |= name == "BN" && value == "1";
					saw_mbn |= name == "MBN" && value == "20042002";
					saw_sopd |= name == "SOPD" && value == "180";
				}
				client_auth_has_retail_environment =
						auth.na == kName && saw_vn && saw_bn &&
						saw_mbn && saw_sopd;
			}
		} else if (opcode == SESSION_OPCODE_PROTOCOL_MESSAGE) {
			for (const np::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!(conn.peer == peer)) continue;
				SessionSequencing seq = conn.seq;
				ProtocolPacketHeader hdr;
				std::vector<ProtocolMessage> messages;
				if (!deframe_session_packet(seq, SessionCrypto{{}, conn.client_scrk, 0},
				                            session_body.data(), session_body.size(), hdr, messages)) break;
				for (const ProtocolMessage &message : messages) {
					if (message.tag == 0x00) {
						saw_join_00 = true;
						join_00_shape_ok =
								message.payload ==
								retail_expansion_join_request(host_config.expansion);
					}
					if (message.tag == 0x01) {
						saw_join_01 = true;
						join_01_shape_ok = message.payload == std::vector<uint8_t>{0x00};
					}
					if (message.tag == 0x02) {
						saw_join_02 = true;
						join_02_shape_ok = message.payload.size() == 256 &&
							message.payload[4] == 0x02 && message.payload[5] == 0 &&
							message.payload[6] == 0 && message.payload[7] == 0;
					}
					if (!client.world_ready() &&
					    (message.tag == 0x0A || message.tag == 0x2F ||
					     message.tag == 0x0B))
						saw_world_drive_while_held = true;
				}
				break;
			}
		}
		np::HandleResult r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), tick++);
		for (const np::HostAcceptEvent &e : r.events) note_event(e);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	};

	// --- 1) Retail post-auth exchange learns the mission while the world/load drive is held. ---
	pump_host(client.start()); // ClientHello -> ServerHello (queued back to the client)
	for (int f = 0; f < 20 && !client.mission_known(); ++f)
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
	if (!expect(client.mission_known(), "joiner learned mission metadata from S2C 0x7B")) return false;
	if (!expect(client.server_name() == host_config.server_name &&
	                    client.mission_name() == host_config.mission_name &&
	                    client.map_file() == host_config.mission_file &&
	                    client.game_type() == host_config.game_type &&
	                    client.expansion() == host_config.expansion,
	            "S2C 0x7B retained authoritative server/mission/gametype/expansion")) return false;
	if (!expect(saw_join_00 && join_00_shape_ok &&
	                    saw_join_01 && join_01_shape_ok &&
	                    saw_join_02 && join_02_shape_ok,
	            "exact C2S 0x00 -> 0x01 -> S2C 0x02 -> 256-byte C2S 0x02 sequence")) return false;
	if (!expect(
			saw_client_auth &&
					client_auth_has_retail_environment,
			"game ClientAuth sends the retail build/environment CU block")) {
		return false;
	}
	if (!expect(client.phase() == np::JoinerConnection::Phase::Driving && !spawned,
	            "mission discovery stays on the same pre-spawn connection")) return false;

	uint32_t held_server_key = 0;
	uint32_t held_connection_id = 0;
	for (const np::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		held_server_key = conn.server_sk;
		held_connection_id = conn.connection_id;
	}
	for (int f = 0; f < 4; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
		for (np::TickOut &t : np::tick_connections(ctx, 300, tick++)) {
			for (const np::HostAcceptEvent &e : t.events) note_event(e);
			for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
		}
	}
	if (!expect(!saw_world_drive_while_held &&
	                    !spawned && !np::connection_spawned(ctx, peer),
	            "0x0A and loadout/status wait for world-ready")) return false;

	// Install the advertised mission, then resume on the exact authenticated session.
	client.set_world_ready(true);

	// --- 2) Spawn-gate burst, driven entirely from the per-frame client role. ---
	for (int f = 0; f < 120 && !spawned; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
		// Several host ticks per frame so entity_batch_count climbs through world streaming (F3) and the
		// spawn gate opens; ship the burst replies back to the client.
		for (int k = 0; k < 6; ++k) {
			for (np::TickOut &t : np::tick_connections(ctx, 300, tick++)) {
				for (const np::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
		}
	}
	if (!expect(spawned, "the spawn-gate burst trips PeerSpawned")) return false;
	if (!expect(spawn_name == kName, "PeerSpawned carries the joiner's ClientAuth.na name")) return false;
	if (!expect(np::connection_spawned(ctx, peer), "host marks the peer spawned")) return false;
	bool resumed_same_session = false;
	for (const np::NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (conn.peer == peer)
			resumed_same_session = conn.server_sk == held_server_key &&
			                       conn.connection_id == held_connection_id;
	if (!expect(resumed_same_session, "world-ready resumes the same authenticated session")) return false;

	// --- 3) Owner's PeerSpawned reaction: bind the connection's transport (the pipeline already bound
	//        owned_entity) + stream a NAMED organic-spawn so the client name-matches. ---
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	w::EntityHandle Hh{};
	for (np::NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) {
			c.link.transport = &udp_host;
			c.link.mode = ns::TransportMode::Client;
			c.link.s2c_phase = 2; // first live frame exercises objective phase 3
			Hh = c.link.owned_entity;
		}
	}
	if (!expect(Hh.valid(), "joiner entity spawned + owned_entity bound by the spawn pipeline")) return false;
	if (!expect(Hh != host_h, "joiner is a distinct entity from the host's own player")) return false;

	const w::Entity *je0 = world.registry.get(Hh);
	const uint16_t net_id = je0 ? je0->net_id : 0;
	const int32_t sx = w::to_fixed(70.0), sy = w::to_fixed(25.0), sz = w::to_fixed(56.0);
	{
		std::vector<uint8_t> body = make_organic_spawn(Hh.packed, spawn_name, sx, sy, sz, 0x40000000, 2, net_id);
		std::vector<uint8_t> sdg;
		if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0C, body, sdg), "host frames the named 0x0C"))
			return false;
		client.receive(sdg.data(), sdg.size());
		// The name-match and the post-0x0E deploy release may arrive on adjacent
		// receive drains. The runtime contract requires the owner to ship every
		// returned datagram; discarding one would manufacture a sequence hole.
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick++))
			pump_host(std::move(d));
	}
	for (int f = 0; f < 4 && !client.in_match(); ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick++))
			pump_host(std::move(d));
	}
	if (!expect(client.in_match() && client.self_handle() == Hh.packed,
	            "client reached InMatch after name-match plus deploy release; H == the host wire handle")) return false;
	if (!expect(client.view().game_type() == host_config.game_type,
	            "joiner learned authoritative g_GameType before live 0x0A frames")) return false;
	if (!expect(client.deployed(), "client is deployed after name-match plus the applicable 0x5A release")) return false;

	// --- 4) In-match per-frame loop: client 0x0C -> apply_in_match_c2s -> Server_TickUpdate -> 0x0A fold ---
	PlayerExtendedUplink up;
	up.carrier_handle = 0xFFFF;
	up.pos_x = w::to_fixed(100.0);
	up.pos_y = w::to_fixed(200.0);
	up.pos_z = w::to_fixed(-50.0);
	up.heading = 0x2000; // -> mission yaw 45
	up.pitch = 0x0100;

	std::size_t staged = 0;
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(up, tick)) {
		np::HandleResult r = np::handle_server_datagram(ctx, peer, d.data(), d.size(), tick++);
		for (const np::HostAcceptEvent &e : r.events) staged += np::apply_in_match_c2s(ctx, e);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	}
	if (!expect(staged == 1, "exactly one C2S 0x0C staged via apply_in_match_c2s")) return false;

	np::Server_TickUpdate(ctx); // drain (SNAP) -> run_logic_tick -> emit per-connection 0x0A

	const w::Entity *je = world.registry.get(Hh);
	if (!expect(je != nullptr, "joiner entity present after the tick")) return false;
	if (!expect(je->position.x == static_cast<float>(w::from_fixed(up.pos_x)) &&
	                    je->position.y == static_cast<float>(w::from_fixed(up.pos_y)) &&
	                    je->position.z == static_cast<float>(w::from_fixed(up.pos_z)),
	            "joiner entity SNAPped to the C2S 0x0C uplink pose")) return false;
	// The host's own player was NOT touched by the joiner's uplink (entity-scoped owner gate).
	const w::Entity *ho = world.registry.get(host_h);
	if (!expect(ho && ho->position.x == 0.0f && ho->position.y == 0.0f && ho->position.z == 0.0f,
	            "host's own player pose unchanged by the peer's 0x0C")) return false;

	// Reframe the host's emitted S2C 0x0A (identity-framed on the transport outbound) as a 0x83 and
	// fold it into the client's ClientState.
	std::vector<uint8_t> raw;
	bool got_0a = false;
	while (udp_host.pop_outbound(raw)) {
		if (raw.empty() || raw[0] != 0x0A) continue;
		std::vector<uint8_t> inner(raw.begin() + 1, raw.end());
		std::vector<uint8_t> dg83;
		if (np::frame_in_match_s2c(ctx, peer, 0x0A, inner, dg83)) {
			client.receive(dg83.data(), dg83.size());
			got_0a = true;
		}
	}
	if (!expect(got_0a, "Server_TickUpdate emitted an S2C 0x0A for the joiner connection")) return false;
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick++))
		pump_host(std::move(d)); // fold the 0x0A and ship any same-frame housekeeping

	// The client's decoded view reflects the server's 0x0A: its anchor IS the joiner's post-SNAP
	// position (emit_connection_s2c anchors to owned_entity), tying 0x0C-in -> 0x0A-out -> ClientState.
	if (!expect(client.state().anchor_x == w::to_fixed(je->position.x) &&
	                    client.state().anchor_y == w::to_fixed(je->position.y) &&
	                    client.state().anchor_z == w::to_fixed(je->position.z),
	            "client ClientState anchor == joiner's post-SNAP position (0x0A folded)")) return false;
	if (!expect(client.state().frames_applied >= 1, "client folded at least one 0x0A frame")) return false;
	if (!expect(client.state().objective_updates_applied == 1 &&
	                    client.state().objective_won == world.subgoals.won &&
	                    client.state().objective_lost == world.subgoals.lost &&
	                    client.state().objective_show_win == world.subgoals.show_win &&
	                    client.state().objective_show_lose == world.subgoals.show_lose,
	            "objective Co-op frame folds all four authoritative subgoal masks")) return false;

	// Partial 0x0A decoding is intentionally useful for entity presentation, but a
	// packet that ends before the recipient tail must not manufacture health zero
	// or close the deployed gate. The tail is after the selected sub-block.
	const int16_t health_before_short_frame = client.state().local_health;
	const uint32_t frames_before_short_frame = client.state().frames_applied;
	std::vector<uint8_t> short_0a(14, 0); // anchor + flags, short before sub-block/tail
	std::vector<uint8_t> short_dg;
	if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0A, short_0a, short_dg),
	            "host frames the deliberately short 0x0A")) return false;
	client.receive(short_dg.data(), short_dg.size());
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick++))
		pump_host(std::move(d));
	if (!expect(client.state().frames_applied == frames_before_short_frame + 1,
	            "partial frame remains available to the lenient view fold")) return false;
	if (!expect(client.state().local_health == health_before_short_frame,
	            "a frame without a decoded recipient tail preserves local health")) return false;
	if (!expect(client.deployed(),
	            "a frame without a decoded recipient tail cannot close the deploy gate")) return false;

	// The 0x0A tail is recipient-specific: once the authoritative owned entity reaches zero health,
	// the same client frame must fold that death before evaluating the deployed 0x0C send gate.
	// This is deliberately death-only. A future respawn/deploy exchange owns re-opening the gate.
	w::Entity *victim = world.registry.get(Hh);
	if (!expect(victim != nullptr, "joiner entity present for the authoritative death frame")) return false;
	victim->health = 0;
	victim->alive = false;
	victim->flags |= 2u;
	const uint32_t frames_before_death = client.state().frames_applied;
	np::Server_TickUpdate(ctx);

	bool got_death_0a = false;
	while (udp_host.pop_outbound(raw)) {
		if (raw.empty() || raw[0] != 0x0A) continue;
		std::vector<uint8_t> inner(raw.begin() + 1, raw.end());
		std::vector<uint8_t> dg83;
		if (np::frame_in_match_s2c(ctx, peer, 0x0A, inner, dg83)) {
			client.receive(dg83.data(), dg83.size());
			got_death_0a = true;
		}
	}
	if (!expect(got_death_0a, "host emitted the authoritative S2C 0x0A death frame")) return false;

	std::size_t staged_after_death = 0;
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(up, tick)) {
		np::HandleResult r = np::handle_server_datagram(ctx, peer, d.data(), d.size(), tick++);
		for (const np::HostAcceptEvent &event : r.events)
			staged_after_death += np::apply_in_match_c2s(ctx, event);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	}
	if (!expect(client.state().frames_applied == frames_before_death + 1,
	            "client folded the fresh authoritative death frame")) return false;
	if (!expect(client.state().local_health == 0,
	            "client stores zero from its recipient-specific 0x0A health tail")) return false;
	if (!expect(!client.deployed(), "authoritative death closes the deployed uplink gate")) return false;
	if (!expect(staged_after_death == 0,
	            "the receive-before-send death frame stages no C2S 0x0C uplink")) return false;

	// Exactly one SNAP per 0x0C: a second tick with no new uplink drained nothing more.
	if (!expect(udp_host.inbound_pending() == 0, "the connection's C2S queue is drained")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (B) Host-as-client (D-NET-121/122): the host's own loopback view anchors to its player, not dvxi5.
// ---------------------------------------------------------------------------------------------------
bool run_host_client_discards_authority_owned_reload_echoes() {
	ns::LoopbackChannel host_loop;
	np::ClientRuntime host_view(host_loop);

	WeaponReload reload;
	reload.entity_handle = 0x0001;
	for (uint16_t i = 0; i < 256; ++i) {
		reload.reload_param = i;
		host_loop.host_send(0x49, encode_weapon_reload(reload));
		host_view.Client_ProcessNetworkFrame(i);
	}

	if (!expect(host_loop.s2c_pending() == 0,
	            "host client pumps every loopback S2C 0x49 notification"))
		return false;
	return expect(host_view.drain_reload_notifications().empty(),
	              "host client does not retain authority-owned reload notifications");
}

bool run_host_as_client() {
	np::NapiNPServerCtx ctx;
	ns::LoopbackChannel host_loop; // the in-process channel the host emits its own S2C onto
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        0x0FE0E112u, &host_loop);

	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	ctx.world = &world;

	// Spawn the host's own player through the real pipeline (type-2 loopback -> spawn_player ->
	// cached.local_player; binds conn.link.owned_entity = the host player, the D-NET-121 anchor).
	const int spawned = np::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(spawned == 1, "the host's own player spawned via the pipeline")) return false;

	// Find the host loopback connection; confirm its owned_entity is the host player + mark it in-match.
	// (In production burst.spawned latches when the host loopback's §5.2a burst completes — covered by
	// npruntime_initial_state_burst; here we set it directly to exercise the per-frame emit path.)
	np::NapiNPConnection *self = nullptr;
	for (np::NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.type == 2) self = &c;
	}
	if (!expect(self != nullptr, "host loopback connection present")) return false;
	if (!expect(self->link.owned_entity.valid(), "host loopback owned_entity bound to its player")) return false;
	if (!expect(self->link.transport == &host_loop, "host loopback transport is the in-process channel"))
		return false;
	self->burst.spawned = true; // in-match (the is_in_match predicate) — §5.2a-complete in production

	// Move the host player to a DISTINCT, non-dvxi5 position so the anchor check is meaningful.
	const w::EntityHandle hp = self->link.owned_entity;
	w::Entity *he = world.registry.get(hp);
	if (!expect(he != nullptr, "host player entity present")) return false;
	he->position = w::Vec3{static_cast<float>(w::from_fixed(w::to_fixed(420.0))),
	                       static_cast<float>(w::from_fixed(w::to_fixed(-37.0))),
	                       static_cast<float>(w::from_fixed(w::to_fixed(910.0)))};

	np::Server_TickUpdate(ctx); // fans a per-frame 0x0A to the host loopback (is_in_match), anchored to hp

	np::ClientRuntime host_view(host_loop); // HostClient role: recv-fold only, 0x0C suppressed
	if (!expect(host_view.is_authority(), "host-as-client runtime is authority (no 0x0C)")) return false;
	std::vector<std::vector<uint8_t>> out = host_view.Client_ProcessNetworkFrame();
	if (!expect(out.empty(), "host-as-client emits no C2S (is_authority gate)")) return false;

	const w::Entity *he2 = world.registry.get(hp);
	if (!expect(host_view.state().frames_applied >= 1, "host-as-client folded its own 0x0A")) return false;
	if (!expect(host_view.state().anchor_x == w::to_fixed(he2->position.x) &&
	                    host_view.state().anchor_y == w::to_fixed(he2->position.y) &&
	                    host_view.state().anchor_z == w::to_fixed(he2->position.z),
	            "host-as-client anchor == host player position (D-NET-121: owned_entity, not dvxi5)"))
		return false;
	// Explicitly assert it is NOT the dvxi5 fallback (the bug D-NET-121 guards).
	if (!expect(host_view.state().anchor_x != static_cast<int32_t>(0xfe56f854u),
	            "host-as-client anchor is not the dvxi5 fallback")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (C) Production HostOwner startup: the host-local client must receive the load-time pool stream
// before its first compact frame. A mounted pool-0 organic can precede its pool-1 ewep carrier in
// registry order, while the no-callback carrier has no live 0x0A body. Its 0x0D spawn is therefore
// the only faithful source for the carrier row used to lift the child's local compact pose.
// ---------------------------------------------------------------------------------------------------
bool run_host_startup_seeds_mounted_no_callback_carrier() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::AiSystem ai;
	world.ai = &ai;

	w::Entity infantry;
	infantry.kind = w::EntityKind::Organic;
	infantry.item_id = 5311; // US02 organic in the production Godot witness
	infantry.net_class_code = static_cast<uint8_t>(EntityClass::Infantry);
	const w::EntityHandle infantry_h = world.registry.spawn(0, infantry);
	if (!expect(infantry_h.valid() && ai.attach(infantry_h) >= 0,
	            "mounted startup fixture spawns pool-0 infantry first")) return false;

	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.item_id = 1294; // B50Cal ewep
	gun.position = {0.0f, 8.0f, 0.0f};
	gun.yaw = 45;
	gun.net_class_code = static_cast<uint8_t>(EntityClass::NoNetworkCallback);
	w::Seat gunner;
	gunner.type = w::SeatType::Gunner;
	gunner.bone_index = 6; // B50Cal.3di USRP row 6 is the witnessed Usegun bone
	gun.seats.push_back(gunner);
	const w::EntityHandle gun_h = world.registry.spawn_from(1, 0, gun);
	if (!expect(gun_h.valid() && gun_h.packed == 0x1000,
	            "B50Cal occupies the first pool-1 carrier handle")) return false;
	if (!expect(w::entity_process_vehicle_attach(world, infantry_h, gun_h, 6),
	            "infantry attaches to the B50Cal gunner bone")) return false;
	w::Entity *infantry_live = world.registry.get(infantry_h);
	const w::Entity *gun_live = world.registry.get(gun_h);
	if (!expect(infantry_live != nullptr && gun_live != nullptr,
	            "mounted startup fixture entities resolve")) return false;
	w::pose_mounted_occupant(world, *infantry_live, *gun_live, gun_live->seats[0]);

	ns::LoopbackChannel host_loop;
	np::HostOwner owner;
	owner.host_loopback = &host_loop;
	owner.ctx.world = &world;
	np::HostConfig cfg;
	cfg.config.server_name = "SINGLEPLAYERGAME";
	cfg.config.max_players = 1;
	cfg.socket_mode = np::SocketMode::Socketless;
	cfg.serve_and_play = true;
	np::start_host_session(owner, cfg);

	// Match NovaSimulation's startup order: construct/install the client view after host bring-up,
	// then fold the queued initial stream and first whole-world compact frame together.
	np::Server_TickUpdate(owner.ctx);
	np::ClientRuntime host_view(host_loop);
	host_view.view().set_item_class_resolver([](uint16_t type_id) {
		if (type_id == 0x14B9u) return EntityClass::Player;
		if (type_id == 5311u) return EntityClass::Infantry;
		if (type_id == 1294u) return EntityClass::NoNetworkCallback;
		return EntityClass::Unknown;
	});
	host_view.Client_ProcessNetworkFrame();

	const ns::ClientEntityState *carrier = host_view.view().state().find(gun_h.packed);
	if (!expect(carrier != nullptr && carrier->type_id == 1294,
	            "production host startup streams the B50Cal 0x0D carrier row")) return false;
	if (!expect(carrier->x == w::to_fixed(gun_live->position.x) &&
	                    carrier->y == w::to_fixed(gun_live->position.y) &&
	                    carrier->z == w::to_fixed(gun_live->position.z),
	            "host-local carrier row retains its absolute spawn pose")) return false;
	const ns::ClientEntityState *child = host_view.view().state().find(infantry_h.packed);
	if (!expect(child != nullptr && child->carrier_handle == gun_h.packed &&
	                    child->mount_bone == 6,
	            "mounted child compact retains its B50Cal carrier and raw Usegun bone"))
		return false;
	if (!expect(child->x == carrier->x && child->y == carrier->y && child->z == carrier->z,
	            "host-local mounted child lifts through the load-time no-callback carrier")) return false;

	const np::NapiNPConnection *self = nullptr;
	for (const np::NapiNPConnection &conn : owner.ctx.np_protocol.connection_list)
		if (conn.type == 2) self = &conn;
	if (!expect(self != nullptr && self->burst.spawned && self->burst.entity_batch_count > 0,
	            "host startup reaches in-match through the real initial-state burst")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (D) Production HostOwner startup: the advertised ClaymorePref mission attribute feeds the
// authoritative throwable rule. This is a host-only simulation setting, not a client-side decode.
// ---------------------------------------------------------------------------------------------------
bool run_host_startup_maps_claymore_preference() {
	{
		w::World world;
		np::HostOwner owner;
		owner.ctx.world = &world;
		np::HostConfig cfg;
		cfg.config.mp_attributes = 0x8000u;
		cfg.socket_mode = np::SocketMode::Socketless;
		np::start_host_session(owner, cfg);
		if (!expect(world.throwables.team_trigger_claymore,
		            "host startup enables same-team claymore triggers for mp_attributes 0x8000"))
			return false;
	}

	{
		w::World world;
		world.throwables.team_trigger_claymore = true;
		np::HostOwner owner;
		owner.ctx.world = &world;
		np::HostConfig cfg;
		cfg.config.mp_attributes = 0x3A06u;
		cfg.socket_mode = np::SocketMode::Socketless;
		np::start_host_session(owner, cfg);
		if (!expect(!world.throwables.team_trigger_claymore,
		            "host startup disables same-team claymore triggers for default mp_attributes 0x3A06"))
			return false;
	}

	return true;
}

// ---------------------------------------------------------------------------------------------------
// (E) Host-owner registration boundary: a player created by tick_connections must be visible to
// adapter registration before its first authoritative body tick and per-connection 0x0A fan.
// ---------------------------------------------------------------------------------------------------
bool run_host_pump_hook_observes_remote_before_first_tick() {
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);

	np::HostOwner owner;
	np::test::bring_up_host(owner.ctx, np::ConnectionMode::HostClient,
			np::SocketMode::Socketless, 0x0FE0E112u);
	owner.ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 31000};
	np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<ns::UdpSessionTransport>(
			ns::UdpSessionTransport::Role::Host);

	np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.connection_id = np::kFirstJoinerDcb;
	conn.player_name = "LateJoiner";
	conn.self_id_seen = true;
	conn.phase = np::ConnectionPhase::PendingSpawn;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = ns::TransportMode::Client;
	conn.reply.roster_pushed = true;
	conn.burst.sync_state = 4;
	conn.burst.world_stream_phase = 8;
	conn.burst.loadout_received = true;
	conn.burst.entity_batch_count = 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	owner.ctx.np_protocol.next_connection_id = np::kFirstJoinerDcb + 1;

	HostPumpHookProbe hook{&owner, &world, &ai, peer};
	FirstLogicTickProbe logic_probe;
	logic_probe.hook = &hook;
	world.add_system(&logic_probe);

	NullDatagramSocket sock;
	np::host_session_pump(owner, sock, &observe_host_before_server_tick, &hook);

	np::NapiNPConnection *remote = nullptr;
	for (np::NapiNPConnection &candidate : owner.ctx.np_protocol.connection_list) {
		if (candidate.peer == peer) remote = &candidate;
	}
	if (!expect(hook.calls == 1, "pre-Server_TickUpdate hook runs exactly once")) return false;
	if (!expect(hook.saw_spawned_connection && hook.saw_live_entity && hook.saw_ai_component,
			"hook observes the remote spawned and AI-attached by tick_connections")) return false;
	if (!expect(hook.saw_before_logic && hook.saw_before_fan,
			"hook runs before the remote's first logic tick and 0x0A fan")) return false;
	if (!expect(logic_probe.ticks == 1 && logic_probe.first_tick_saw_hook &&
				logic_probe.first_tick_saw_player,
			"first authoritative logic tick observes completed registration")) return false;
	if (!expect(world.logic_tick == 1, "host pump advances exactly one logic tick")) return false;
	if (!expect(remote != nullptr && remote->link.s2c_phase == 1,
			"the first authoritative 0x0A fan follows registration")) return false;
	return true;
}

} // namespace

int main() {
	const bool ok = run_seeded_objective_layout_hint() &&
	                run_fire_queue_stamps_runtime_tick() &&
	                run_retail_post_auth_prelude() &&
	                run_duplicate_s2c_session_one_shot_is_not_replayed() &&
	                run_roundtrip() &&
	                run_host_client_discards_authority_owned_reload_echoes() &&
	                run_host_as_client() &&
	                run_host_startup_seeds_mounted_no_callback_carrier() &&
	                run_host_startup_maps_claymore_preference() &&
	                run_host_pump_hook_observes_remote_before_first_tick();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
