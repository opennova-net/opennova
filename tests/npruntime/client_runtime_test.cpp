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

#include <npruntime/charattr_challenge.h>
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
#include <novacrypto/crc32.h>

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/vehicle_attach.h>
#include <world/world.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
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
		uint16_t net_id, uint8_t anim_slot = 0);

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

// The S2C 0x04 slot assignment in its witnessed 24-byte shape: the tail byte is the
// joiner's server-assigned team — the client's byte_A85B48 latch, and therefore the
// 0x2F loadout-submit header byte 0. [orig: NetPacket_WriteSlotAssignment @0x502b30;
// NapiNPClientMsg_0x004 @0x425410 -> byte_A85B48 @0x425499]
std::vector<uint8_t> retail_slot_assignment(uint8_t team = 0x02) {
	std::vector<uint8_t> body(24, 0);
	body[17] = 0x01; // the joiner's slot index
	body[18] = 0x18; // host slot capacity
	body[23] = team;
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

std::vector<uint8_t> zone_timer_value_body(
		uint16_t handle, uint8_t mode, int32_t value_s,
		int32_t limit_s, int16_t rate,
		uint8_t byte544 = 0, uint8_t byte545 = 0) {
	std::vector<uint8_t> body;
	body.reserve(15);
	auto append_u16 = [&](uint16_t value) {
		body.push_back(static_cast<uint8_t>(value));
		body.push_back(static_cast<uint8_t>(value >> 8));
	};
	auto append_u32 = [&](uint32_t value) {
		body.push_back(static_cast<uint8_t>(value));
		body.push_back(static_cast<uint8_t>(value >> 8));
		body.push_back(static_cast<uint8_t>(value >> 16));
		body.push_back(static_cast<uint8_t>(value >> 24));
	};
	append_u16(handle);
	body.push_back(mode);
	append_u32(static_cast<uint32_t>(value_s));
	append_u32(static_cast<uint32_t>(limit_s));
	append_u16(static_cast<uint16_t>(rate));
	body.push_back(byte544);
	body.push_back(byte545);
	return body;
}

std::vector<uint8_t> zone_timer_window_body(
		uint16_t handle, uint8_t mode_a, uint8_t mode_b,
		uint16_t start_s, uint16_t end_s, uint8_t rate) {
	return {
			static_cast<uint8_t>(handle),
			static_cast<uint8_t>(handle >> 8),
			mode_a,
			mode_b,
			static_cast<uint8_t>(start_s),
			static_cast<uint8_t>(start_s >> 8),
			static_cast<uint8_t>(end_s),
			static_cast<uint8_t>(end_s >> 8),
			rate,
	};
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
	client.seed_session(0x1234u, 0u, "client-key", "server-key",
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
	// The network-role clock is ANCHORED to the host's tick seed, never free-run from
	// zero: a replay seeds it the way the capture's S2C 0x61 did.
	constexpr uint32_t kTickSeed = 0x00110000u;
	np::ClientRuntime client("Shooter");
	client.seed_session(0x11223344u, 0u, client_scrk, server_scrk,
	                    1, 0, self_handle, w::kPlayerInfantryTypeId, 0u, kTickSeed);

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
	return expect(decoded.current_tick == kTickSeed + 3,
	              "C2S 0x06 uses the seeded runtime tick at queue time");
}

// The host anchors the client's whole network-role clock with S2C 0x61 and then rejects
// any fire whose tick is zero or not past the seed it stamped into that player's slot. An
// unseeded client must NOT free-run a tick (retail skips the increment while it is zero),
// but it DOES still transmit: the fire action's freshness predicate lives on the AUTHORITY
// arm only, so a joiner emits the doomed 0x06 and lets the host discard it. Refusing it
// client-side would be our own invention.
// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 (both globals @0x4297f8/@0x4297fd,
//  short body -> 0 @0x4297eb); the zero-skip Client_ProcessNetworkFrame @0x42c193; the
//  is_authority split Entity_FireWeaponAndSendPacket @0x42bdfd (client arm @0x42bf46,
//  authority-only gate @0x42be3a); the host gate PlayerSlot_IsActive @0x4fc760 via
//  NapiNPServerMsg_0x006 @0x51358d; Server_SendRandomSeedToPlayer @0x5101a0
//  (value @0x5101d4, disarm @0x510237)]
bool run_tick_seed_anchors_the_client_clock() {
	const std::string client_scrk = "CLIENT-TICK-SEED-SCRK";
	const std::string server_scrk = "SERVER-TICK-SEED-SCRK";
	constexpr uint16_t self_handle = 0x0002;
	constexpr uint32_t kSeed = 0x00110000u;

	// (a) UNSEEDED: the tick stays parked at zero across frames — five Client frames must
	// not advance it — and the shot is STILL queued, stamped with that zero, exactly as
	// retail's client arm does. The zero is what the host's own gate rejects.
	np::ClientRuntime unseeded("Unseeded");
	unseeded.seed_session(0x33445566u, 0u, client_scrk, server_scrk,
	                      1, 0, self_handle, w::kPlayerInfantryTypeId, 0u, /*tick_seed=*/0u);
	for (int i = 0; i < 5; ++i) (void)unseeded.Client_ProcessNetworkFrame(100 + i);
	ClientFiredRound fire;
	fire.current_tick = 0xDEADBEEFu; // poison: the runtime owns this wire field
	fire.shooter_handle = self_handle;
	if (!expect(unseeded.queue_fired_round(fire),
			"an unseeded clock still queues the shot (the gate is authority-side in retail)")) {
		return false;
	}
	{
		const std::vector<std::vector<uint8_t>> doomed =
				unseeded.Client_ProcessNetworkFrame(105);
		uint8_t op = 0;
		std::vector<uint8_t> bd;
		ProtocolPacketHeader hd;
		std::vector<ProtocolMessage> msgs;
		if (!expect(doomed.size() == 1 &&
				nw_decode_inbound(doomed[0].data(), doomed[0].size(), op, bd) &&
				op == SESSION_OPCODE_PROTOCOL_MESSAGE &&
				decode_protocol_packet_plaintext(bd.data(), bd.size(), client_scrk, hd, msgs) &&
				msgs.size() == 1 && msgs[0].tag == 0x06,
				"the unseeded shot reaches the wire as a C2S 0x06")) {
			return false;
		}
		ClientFiredRound doomed_round;
		size_t doomed_consumed = 0;
		if (!expect(decode_client_fired_round(msgs[0].payload.data(), msgs[0].payload.size(),
				doomed_round, doomed_consumed) && doomed_round.current_tick == 0u,
				"the unseeded 0x06 carries tick 0 — the value the host's gate discards")) {
			return false;
		}
	}

	// (b) SEEDED via the wire: an S2C 0x61 re-bases the clock, and the very next frame's
	// shot stamps seed+1 — comfortably past the freshness floor the host stamped.
	np::JoinerConnection joiner("TickSeed", [] { return uint64_t(0); });
	joiner.seed_in_match(0x44556677u, 1u, client_scrk, server_scrk,
	                     1, 0, self_handle, 0x14B9);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> seed_dg = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x61, {0x00, 0x00, 0x11, 0x00})});
	const np::JoinerConnection::PollResult seeded =
			joiner.handle_datagram(seed_dg.data(), seed_dg.size());
	if (!expect(seeded.tick_seed_set && seeded.tick_seed == kSeed,
			"S2C 0x61 surfaces the per-player tick seed")) {
		return false;
	}

	// (c) A SHORT body seeds zero, not garbage, and the four-zero-byte disarm form is a
	// real seed that parks the clock again.
	const std::vector<uint8_t> short_dg = frame_server_session(
			server_tx, server_scrk, 1u, {make_protocol_message(0x61, {0x01, 0x02})});
	const np::JoinerConnection::PollResult short_seed =
			joiner.handle_datagram(short_dg.data(), short_dg.size());
	if (!expect(short_seed.tick_seed_set && short_seed.tick_seed == 0u,
			"a short 0x61 body seeds zero rather than reading past the payload")) {
		return false;
	}
	const std::vector<uint8_t> disarm_dg = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x61, {0x00, 0x00, 0x00, 0x00})});
	const np::JoinerConnection::PollResult disarm =
			joiner.handle_datagram(disarm_dg.data(), disarm_dg.size());
	return expect(disarm.tick_seed_set && disarm.tick_seed == 0u,
			"the round-end disarm form is a witnessed seed of zero");
}

// The capture-default C2S 0x2F body, re-derived independently of the production encoder:
// [u8 team][u8 class 8][u32 slot] + the golden seven ADM rows ("-1" ammo/flags -> 0xFF) + 0xFF.
// [wire: retail-lan-host-join-session f317-318; orig: NetPacket_SendLoadoutSubmit @0x42cdc0]
std::vector<uint8_t> canned_loadout_body(uint8_t team, uint8_t slot_low_byte) {
	std::vector<uint8_t> body = {team, 0x08, slot_low_byte, 0x00, 0x00, 0x00};
	for (uint8_t adm : {uint8_t{0x18}, uint8_t{0x03}, uint8_t{0x2C}, uint8_t{0x28},
	                    uint8_t{0x29}, uint8_t{0x2A}, uint8_t{0x02}}) {
		body.push_back(adm);
		body.push_back(0xFF);
		body.push_back(0xFF);
		body.push_back(0xFF);
	}
	body.push_back(0xFF);
	return body;
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
					make_protocol_message(0x04, retail_slot_assignment()),
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
				client_messages[0].payload ==
						canned_loadout_body(0x02, 0xC3) &&
				client_messages[1].tag == 0x2F &&
				client_messages[1].payload ==
						canned_loadout_body(0x02, 0xD4) &&
				client_messages[2].tag == 0x0B &&
				!client_messages[2].payload.empty(),
			"world-stream 0x1A triggers the capture-default loadout pair "
			"(team from the 0x04 tail, slots 195/212)")) {
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

// The host emits the terminal pre-world S2C 0x11 exactly ONCE, paced behind its C2S 0x02
// reply regardless of joiner progress (§5.5). A joiner still mid-0x60/0x64 must LATCH an
// early arrival and complete the stage when it reaches the sync tail — dropping it parked
// both sides forever (joiner never preload_ready, host waiting in sync state 3).
bool run_early_sync_tail_latch() {
	constexpr uint32_t kServerKey = 0x22334455u;
	constexpr uint32_t kConnectionId = 5;
	np::JoinerConnection joiner("EarlyTail");

	const std::vector<uint8_t> hello_datagram = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_HELLO &&
				parse_client_hello(body.data(), body.size(), hello),
			"early-tail: decode ClientHello")) {
		return false;
	}
	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x99AA0011u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const np::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	if (!expect(hello_result.outbound.size() == 1, "early-tail: ClientAuth emitted")) {
		return false;
	}
	ClientAuth client_auth;
	if (!expect(nw_decode_inbound(
				hello_result.outbound[0].data(), hello_result.outbound[0].size(),
				opcode, body) &&
				parse_client_auth(body.data(), body.size(), client_auth),
			"early-tail: decode ClientAuth")) {
		return false;
	}
	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, "SERVER-EARLY-TAIL-SCRK",
			"", "", "", false);
	server_auth.mi = kConnectionId;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	(void)joiner.handle_datagram(server_auth_datagram.data(), server_auth_datagram.size());
	const std::string server_scrk = "SERVER-EARLY-TAIL-SCRK";

	// Settings -> [header ACK, JOIN]; S2C 0x00 -> [header ACK, 0x01 {0}]; probe -> echo;
	// game-start bundle -> the grouped admission packet. Same legs the prelude pins —
	// here they only position the FSM at AwaitServerInfo.
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
	if (!expect(joiner.handle_datagram(
				settings_datagram.data(), settings_datagram.size()).outbound.size() == 2,
			"early-tail: settings emit ACK + JOIN")) {
		return false;
	}
	server_seq.last_inbound_seq = 2;
	const std::vector<uint8_t> join_ack_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x00, {})});
	if (!expect(joiner.handle_datagram(
				join_ack_datagram.data(), join_ack_datagram.size()).outbound.size() == 2,
			"early-tail: JOIN ack emits ACK + form post")) {
		return false;
	}
	std::vector<uint8_t> padding_probe(512, 0xA5);
	padding_probe[0] = 0x3E;
	padding_probe[1] = 0x67;
	padding_probe[2] = 0x57;
	padding_probe[3] = 0x00;
	padding_probe[4] = 0x02;
	padding_probe[5] = 0x00;
	padding_probe[6] = 0x00;
	padding_probe[7] = 0x00;
	padding_probe[8] = 0x00;
	padding_probe[9] = 0x01;
	padding_probe[10] = 0x00;
	padding_probe[11] = 0x00;
	server_seq.last_inbound_seq = 4;
	const std::vector<uint8_t> padding_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x02, std::move(padding_probe))});
	if (!expect(joiner.handle_datagram(
				padding_datagram.data(), padding_datagram.size()).outbound.size() == 1,
			"early-tail: probe echoed")) {
		return false;
	}
	server_seq.last_inbound_seq = 5;
	const std::vector<uint8_t> game_start_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(0x05, {0x01}),
					make_protocol_message(0x7B, retail_full_player_info()),
			});
	if (!expect(joiner.handle_datagram(
				game_start_datagram.data(), game_start_datagram.size()).outbound.size() == 1,
			"early-tail: game-start emits the grouped admission packet")) {
		return false;
	}

	// EARLY 0x11 — the FSM is at AwaitServerInfo (mid-transfer). It must defer, not drop:
	// no semantic reply, no preload_ready, only the send-boundary ACK.
	server_seq.last_inbound_seq = 6;
	const std::vector<uint8_t> early_tail_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x11, {})});
	if (!expect(joiner.handle_datagram(
				early_tail_datagram.data(), early_tail_datagram.size()).outbound.empty() &&
				!joiner.preload_ready(),
			"early-tail: a mid-transfer 0x11 is latched, not applied")) {
		return false;
	}
	ProtocolPacketHeader client_header;
	std::vector<ProtocolMessage> client_messages;
	const std::vector<std::vector<uint8_t>> early_ack = joiner.pump(0);
	if (!expect(early_ack.size() == 1 &&
				decode_client_session(
						early_ack[0], client_auth.scrk, client_header, client_messages) &&
				client_messages.empty() &&
				matches_client_header(client_header, kServerKey, 7, 5) &&
				!joiner.preload_ready(),
			"early-tail: the early 0x11 draws only a header ACK")) {
		return false;
	}

	// Completing the transfers + player list applies the latch: the grouped 0x09+0x22
	// releases, preload_ready trips, and (world_ready default) pump releases the 0x0A.
	server_seq.last_inbound_seq = 7;
	const std::vector<uint8_t> server_info_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x60, retail_transfer_chunk(1, 64, 0xA5))});
	if (!expect(joiner.handle_datagram(
				server_info_datagram.data(), server_info_datagram.size()).outbound.size() == 2,
			"early-tail: server-info final emits 0x47 + 0x37")) {
		return false;
	}
	const std::vector<uint8_t> mission_data_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x64, retail_transfer_chunk(1, 64, 0x5A))});
	if (!expect(joiner.handle_datagram(
				mission_data_datagram.data(), mission_data_datagram.size()).outbound.empty(),
			"early-tail: mission-data final waits for the player list")) {
		return false;
	}
	const std::vector<uint8_t> player_list_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x16, encode_player_list({{0, 1}, {1, 2}}))});
	const np::JoinerConnection::PollResult player_list_result = joiner.handle_datagram(
			player_list_datagram.data(), player_list_datagram.size());
	if (!expect(player_list_result.outbound.size() == 1 &&
				decode_client_session(
						player_list_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				client_messages.size() == 2 &&
				client_messages[0].tag == 0x09 &&
				client_messages[1].tag == 0x22 &&
				joiner.preload_ready(),
			"early-tail: reaching the sync tail applies the latched 0x11")) {
		return false;
	}
	const std::vector<std::vector<uint8_t>> world_release = joiner.pump(0);
	if (!expect(world_release.size() == 1 &&
				decode_client_session(
						world_release[0], client_auth.scrk, client_header, client_messages) &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x0A &&
				client_messages[0].payload.empty(),
			"early-tail: the latched sync tail releases the spawn-menu request")) {
		return false;
	}

	// Continuity past the latch: the world-stream terminator still triggers the
	// grouped loadout/status packet.
	server_seq.last_inbound_seq = 11;
	const std::vector<uint8_t> world_end_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x1A, {})});
	const np::JoinerConnection::PollResult world_end_result = joiner.handle_datagram(
			world_end_datagram.data(), world_end_datagram.size());
	return expect(world_end_result.outbound.size() == 1 &&
				decode_client_session(
						world_end_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				client_messages.size() == 3 &&
				client_messages[0].tag == 0x2F &&
				client_messages[1].tag == 0x2F &&
				client_messages[2].tag == 0x0B,
			"early-tail: the FSM continues normally past the latched stage");
}

bool run_duplicate_s2c_session_one_shot_is_not_replayed() {
	const std::string client_scrk = "CLIENT-REPLAY-SCRK";
	const std::string server_scrk = "SERVER-REPLAY-SCRK";
	np::JoinerConnection joiner("Replay");
	joiner.seed_in_match(0x11223344u, 1u, client_scrk, server_scrk,
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
                                        uint16_t net_id, uint8_t anim_slot) {
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
	rec.anim_slot = anim_slot;
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
	np::CharacterJoinVars join_profile;
	join_profile.char_id[0] = 0x0400; // nat 0, div 0, combo 2, good
	join_profile.char_id[1] = 0x8407; // nat 7, div 0, combo 2, evil
	join_profile.team_request = 0xFF;
	join_profile.char_class[0] = 6;
	join_profile.char_class[1] = 6;
	join_profile.avatar[0] = 1;
	join_profile.avatar[1] = 10;
	client.set_character_join_vars(join_profile);

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
	bool client_auth_has_retail_character_profile = false;
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
				uint16_t char_id[2] = {0, 0};
				uint8_t char_class[2] = {0, 0};
				uint8_t avatar[2] = {0, 0};
				bool saw_auto_team = false;
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
					if (name == "CI0")
						char_id[0] = static_cast<uint16_t>(std::strtol(value.c_str(), nullptr, 10));
					else if (name == "CI1")
						char_id[1] = static_cast<uint16_t>(std::strtol(value.c_str(), nullptr, 10));
					else if (name == "TR")
						saw_auto_team = std::strtol(value.c_str(), nullptr, 10) == -1;
					else if (name == "CTA")
						char_class[0] = static_cast<uint8_t>(std::strtol(value.c_str(), nullptr, 10));
					else if (name == "CTB")
						char_class[1] = static_cast<uint8_t>(std::strtol(value.c_str(), nullptr, 10));
					else if (name == "VCA")
						avatar[0] = static_cast<uint8_t>(std::strtol(value.c_str(), nullptr, 10));
					else if (name == "VCB")
						avatar[1] = static_cast<uint8_t>(std::strtol(value.c_str(), nullptr, 10));
				}
				client_auth_has_retail_environment =
						auth.na == kName && saw_vn && saw_bn &&
						saw_mbn && saw_sopd;
				// Pin the binding-supplied selection, not merely constructor
				// fallbacks. Zero/omitted values leave the server-stamped
				// animSlot at zero and break retail's character registry, which
				// was witnessed as the DBuggy1 shadow attached to the joiner.
				// [orig: PlayerProfile_InitDefaults @0x54bbe0..0x54bc24;
				//  CNapiServerInfo_SerializeToSession @0x4c3a1b..0x4c3c4c]
				client_auth_has_retail_character_profile =
						char_id[0] == join_profile.char_id[0] &&
						char_id[1] == join_profile.char_id[1] &&
						saw_auto_team &&
						char_class[0] == join_profile.char_class[0] &&
						char_class[1] == join_profile.char_class[1] &&
						avatar[0] == join_profile.avatar[0] &&
						avatar[1] == join_profile.avatar[1];
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
	if (!expect(
			saw_client_auth &&
					client_auth_has_retail_character_profile,
			"game ClientAuth sends a complete retail character-profile CU block")) {
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
	if (!expect(je0 != nullptr, "host resolves the admitted joiner's authoritative entity"))
		return false;
	const int join_side =
			(je0->team == 1 || je0->team == 3 ||
					(host_config.game_type & 0x10000u) == 0)
			? 0
			: 1;
	if (!expect(
			je0->anim_slot == join_profile.avatar[join_side] &&
					je0->minimap_net_id == join_profile.char_id[join_side],
			"host stamps the uploaded side's avatar and character id onto the joiner"))
		return false;
	const int32_t sx = w::to_fixed(70.0), sy = w::to_fixed(25.0), sz = w::to_fixed(56.0);
	{
		std::vector<uint8_t> body = make_organic_spawn(
				Hh.packed, spawn_name, sx, sy, sz, 0x40000000, 2,
				je0->minimap_net_id, je0->anim_slot);
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
	if (!expect(
			client.spawn_pose().anim_slot == je0->anim_slot &&
					client.spawn_pose().net_id == je0->minimap_net_id,
			"the name-matched self spawn retains the host-stamped character selector and id"))
		return false;
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

	// The host's final connection-settings update can hold the send block shut
	// for a bounded number of client frames. Drain that authoritative gate
	// before testing the live uplink producer.
	for (int frame = 0;
	     frame < 8 && client.send_holdoff_countdown() != 0; ++frame) {
		const auto held = client.Client_ProcessNetworkFrame(tick++);
		if (!expect(held.empty(),
		            "settings send-holdoff suppresses the whole deployed send block"))
			return false;
	}
	if (!expect(client.send_holdoff_countdown() == 0,
	            "settings send-holdoff reaches zero before the uplink frame"))
		return false;

	std::size_t staged = 0;
	std::vector<std::vector<uint8_t>> client_frame =
			client.Client_ProcessNetworkFrame(up, tick);
	if (!expect(client_frame.size() == 1,
	            "one deployed client tick batches RTT and uplink into one session datagram"))
		return false;
	for (std::vector<uint8_t> &d : client_frame) {
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
	if (!expect(client.deployment_pick_pending(),
	            "authoritative death re-enters the deployment FSM so the player can respawn"))
		return false;
	if (!expect(staged_after_death == 0,
	            "the receive-before-send death frame stages no C2S 0x0C uplink")) return false;

	// Exactly one SNAP per 0x0C: a second tick with no new uplink drained nothing more.
	if (!expect(udp_host.inbound_pending() == 0, "the connection's C2S queue is drained")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (B) Host-as-client (D-NET-121/122): the host's own loopback view anchors to its player, not dvxi5.
// ---------------------------------------------------------------------------------------------------
// The integrated ZONES join — the leg run_roundtrip's zone-less world never exercises:
// world_has_spawn_zone -> 0x0F gameFlags bit0=1 + join-time respawn_pending (D-NET-156)
// -> the joiner answers the initial 0x5A grant pair with a C2S 0x0E pick -> the host
// deploys (clearing pending/hidden) and its covering-ack 0x5A release reaches InMatch.
// Two modes pin the deployment seam:
//   headless (player_paced=false): the auto parameter-0 pick {FF FF}, exactly once —
//     today's ctest/nw_replay behavior, unchanged.
//   player-paced (player_paced=true): NO auto pick; the join parks pick-pending
//     (the shell's DEATH deploy screen), an INVALID pick is silently dropped by the
//     host and the screen-side state stays pick-pending for the re-pick (net-re
//     §5.61 — retail's input case 12 has no re-entry gate), then the default pick
//     releases. Both picks reach the wire.
bool run_roundtrip_with_spawn_zones(bool player_paced) {
	const PeerAddr peer{0x0100007Fu, 30001}; // 127.0.0.1:30001
	const std::string kName = "ZonesJoiner";

	np::NapiNPServerCtx ctx;
	np::GameConfig host_config;
	host_config.game_type = 0x30020u;
	host_config.server_name = "Zones Host";
	host_config.mission_name = "Zones Test Mission";
	host_config.mission_file = "ZONES_TEST.BMS";
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        0x0FE0E112u, nullptr, host_config);

	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(2, 16);
	world.registry.configure_pool(3, 16);
	w::AiSystem ai;
	world.ai = &ai;
	ctx.world = &world;
	{
		// A 6002 start marker: the 0xFFFF parameter-0 pick resolves through the
		// per-team marker chain, never an NPC position.
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {50.0f, 60.0f, 1.0f};
		world.registry.spawn(3, start);
	}
	{
		// The deploy-selectable spawn zone: an alive pool-2 def-attrib-0x40000 entity
		// flips world_has_spawn_zone -> 0x0F bit0 = 1 AND the join-time pending hold.
		w::Entity zone;
		zone.kind = w::EntityKind::Building;
		zone.item_id = 0x0500;
		zone.position = {10.0f, 10.0f, 0.0f};
		zone.is_spawn_point = true;
		zone.alive = true;
		zone.team = 1;
		world.registry.spawn(2, zone);
	}
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "zones: host player spawned")) return false;

	np::ClientRuntime client(kName);
	client.set_world_ready(false);
	if (player_paced) client.set_player_paced_deployment(true);
	// The binding seam under test alongside the zones flow: the shell's applied kit
	// replaces the capture-default 0x2F pair content (D-NET-168). The wire team byte
	// stays host-owned — this co-op joiner latches the same team 1 reservation
	// the later player-add pass consumes from the host's S2C 0x04.
	{
		np::JoinerConnection::LoadoutKit kit;
		kit.player_class = 6;
		kit.equipped_combo = 212;
		kit.rows.push_back(LoadoutSubmitEntry{0x18, 0x03, 0xFF, 0xFF});
		kit.rows.push_back(LoadoutSubmitEntry{0x2C, 0xFF, 0x02, 0x01});
		client.set_loadout_kit(kit);
	}

	uint32_t tick = 1;
	bool spawned = false;
	std::string spawn_name;
	bool saw_deploy_pick = false;
	bool deploy_pick_shape_ok = false;
	bool pending_at_pick_time = false;
	int deploy_pick_count = 0;
	std::vector<std::vector<uint8_t>> deploy_picks;
	std::vector<std::vector<uint8_t>> submitted_loadouts;
	auto note_event = [&](const np::HostAcceptEvent &e) {
		if (e.kind == np::HostAcceptEvent::Kind::PeerSpawned && !spawned) {
			spawned = true;
			spawn_name = e.peer_name;
		}
	};
	auto pump_host = [&](std::vector<uint8_t> dg) {
		if (dg.empty()) return;
		uint8_t opcode = 0;
		std::vector<uint8_t> session_body;
		if (nw_decode_inbound(dg.data(), dg.size(), opcode, session_body) &&
		    opcode == SESSION_OPCODE_PROTOCOL_MESSAGE) {
			for (const np::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!(conn.peer == peer)) continue;
				SessionSequencing seq = conn.seq;
				ProtocolPacketHeader hdr;
				std::vector<ProtocolMessage> messages;
				if (!deframe_session_packet(seq, SessionCrypto{{}, conn.client_scrk, 0},
				                            session_body.data(), session_body.size(), hdr,
				                            messages)) {
					break;
				}
				for (const ProtocolMessage &message : messages) {
					if (message.tag == 0x2F) {
						submitted_loadouts.push_back(message.payload);
						continue;
					}
					if (message.tag != 0x0E) continue;
					++deploy_pick_count;
					deploy_picks.push_back(message.payload);
					if (!saw_deploy_pick) {
						saw_deploy_pick = true;
						deploy_pick_shape_ok = message.payload ==
								std::vector<uint8_t>({0xFF, 0xFF});
						// The tap runs before the host consumes this datagram: the
						// joiner must still be held respawn-pending at pick time.
						pending_at_pick_time = conn.link.respawn_pending;
					}
				}
				break;
			}
		}
		np::HandleResult r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), tick++);
		for (const np::HostAcceptEvent &e : r.events) note_event(e);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	};

	pump_host(client.start());
	for (int f = 0; f < 20 && !client.mission_known(); ++f)
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
	if (!expect(client.mission_known(), "zones: joiner learned the mission")) return false;
	client.set_world_ready(true);

	for (int f = 0; f < 120 && !spawned; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
		for (int k = 0; k < 6; ++k) {
			for (np::TickOut &t : np::tick_connections(ctx, 300, tick++)) {
				for (const np::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
		}
	}
	if (!expect(spawned && spawn_name == kName, "zones: spawn gate tripped for the joiner")) return false;

	// The owner's PeerSpawned reaction ships the joiner's NAMED organic record (the burst
	// streamed pool 0 before this entity existed) — same leg run_roundtrip pins.
	w::EntityHandle Hh{};
	for (np::NapiNPConnection &c : ctx.np_protocol.connection_list)
		if (c.peer == peer) Hh = c.link.owned_entity;
	if (!expect(Hh.valid(), "zones: owned_entity bound by the spawn pipeline")) return false;
	const w::Entity *je0 = world.registry.get(Hh);
	{
		std::vector<uint8_t> named = make_organic_spawn(
				Hh.packed, spawn_name, w::to_fixed(50.0), w::to_fixed(60.0),
				w::to_fixed(1.0), 0x40000000, 1, je0 ? je0->net_id : 0);
		std::vector<uint8_t> sdg;
		if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0C, named, sdg),
				"zones: host frames the named 0x0C")) {
			return false;
		}
		client.receive(sdg.data(), sdg.size());
	}

	// Drive the deployment exchange: phase-8 bundle ships 0x0F (bit0=1) -> the pick
	// (auto parameter-0, or the binding's player-paced picks) -> the host's
	// deploy-release 0x5A whose packet ack covers it.
	auto drive_frames = [&](int frames, auto until) {
		for (int f = 0; f < frames && !until(); ++f) {
			for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick))
				pump_host(std::move(d));
			for (np::TickOut &t : np::tick_connections(ctx, 300, tick++)) {
				for (const np::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
		}
	};
	if (player_paced) {
		// The join parks at the player's pick; nothing is auto-sent.
		drive_frames(60, [&] { return client.deployment_pick_pending(); });
		if (!expect(client.deployment_pick_pending(),
				"zones-paced: join parked awaiting the player's deployment pick")) return false;
		if (!expect(deploy_pick_count == 0 && !client.in_match(),
				"zones-paced: no auto pick was sent")) return false;
		// An INVALID pick: the host silently drops it (the resolve-miss break) and the
		// player stays pick-pending for a re-pick [orig: @0x519c88 / net-re §5.61].
		client.queue_deployment_pick(0x2FFE); // pool-2 slot 0xFFE: resolves no entity
		drive_frames(20, [&] { return client.in_match(); });
		if (!expect(deploy_pick_count == 1 && !client.in_match() &&
					client.deployment_pick_pending(),
				"zones-paced: invalid pick silently dropped, still pick-pending")) return false;
		bool still_pending_host_side = false;
		for (const np::NapiNPConnection &conn : ctx.np_protocol.connection_list)
			if (conn.peer == peer) still_pending_host_side = conn.link.respawn_pending;
		if (!expect(still_pending_host_side,
				"zones-paced: host still holds respawn-pending after the invalid pick")) return false;
		// The re-pick: the Default Spawn row (parameter-0 0xFFFF) releases.
		client.queue_deployment_pick(0xFFFF);
		drive_frames(60, [&] { return client.in_match(); });
		if (!expect(deploy_pick_count == 2 &&
					deploy_picks[0] == std::vector<uint8_t>({0xFE, 0x2F}) &&
					deploy_picks[1] == std::vector<uint8_t>({0xFF, 0xFF}),
				"zones-paced: both player picks reached the wire in order")) return false;
	} else {
		drive_frames(60, [&] { return client.in_match(); });
		if (!expect(saw_deploy_pick && deploy_pick_shape_ok,
				"zones: joiner sent the parameter-0 deploy pick 0x0E {FF FF}")) return false;
		if (!expect(deploy_pick_count == 1,
				"zones: exactly one deploy pick on the wire")) return false;
	}
	if (!expect(pending_at_pick_time,
			"zones: host held the joiner respawn-pending until the pick")) return false;
	if (!expect(client.in_match() && client.self_handle() == Hh.packed,
			"zones: deployment release reached InMatch with the host wire handle")) return false;
	if (!expect(client.assigned_team() == 1 && je0 != nullptr && je0->team == 1,
			"zones: pre-spawn 0x04 and the later co-op entity share team 1"))
		return false;
	if (!expect(client.deployed(), "zones: client deployed after the post-pick release")) return false;
	bool pending_cleared = false;
	bool entity_unhidden = false;
	for (const np::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		pending_cleared = !conn.link.respawn_pending;
		if (const w::Entity *je = world.registry.get(conn.link.owned_entity))
			entity_unhidden = (je->flags & 1u) == 0 && je->alive;
	}
	if (!expect(pending_cleared, "zones: successful deploy cleared respawn_pending")) return false;
	if (!expect(entity_unhidden, "zones: successful deploy cleared the hidden bit")) return false;

	// The injected kit rode the wire: the SAME rows/class twice under the 0x04-latched
	// team, first with the fixed pre-init slot 195, then the injected equipped combo.
	// [orig: Game_StartMission @0x525836/@0x525c2e -> NetPacket_SendLoadoutSubmit @0x42cdc0]
	const std::vector<uint8_t> expected_rows = {
			0x18, 0x03, 0xFF, 0xFF, 0x2C, 0xFF, 0x02, 0x01, 0xFF};
	std::vector<uint8_t> expected_first = {0x01, 0x06, 0xC3, 0x00, 0x00, 0x00};
	expected_first.insert(expected_first.end(), expected_rows.begin(), expected_rows.end());
	std::vector<uint8_t> expected_second = expected_first;
	expected_second[2] = 0xD4;
	if (!expect(submitted_loadouts.size() == 2,
			"zones: exactly one loadout-submission pair on the wire")) return false;
	if (!expect(submitted_loadouts[0] == expected_first,
			"zones: first 0x2F carries the injected kit at the fixed slot 195")) return false;
	if (!expect(submitted_loadouts[1] == expected_second,
			"zones: second 0x2F carries the injected kit at the equipped combo")) return false;

	// The mid-session re-submission (the armory ACCEPT leg): ONE more 0x2F with the
	// same kit at the live equipped combo, host re-grants without disturbing the
	// deployed player. [orig: WeaponLoadout_ApplyFromBuffer @0x565d94]
	client.queue_loadout_resubmit();
	drive_frames(10, [&] { return submitted_loadouts.size() >= 3; });
	return expect(submitted_loadouts.size() == 3 && submitted_loadouts[2] == expected_second,
			"zones: armory re-submission rode the wire with the equipped combo");
}

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

bool run_host_zone_timer_value_matches_retail_entry() {
	ns::LoopbackChannel host_loop;
	np::ClientRuntime host_view(host_loop);
	constexpr uint16_t kZone = 0x3001;

	int32_t percent = -1;
	if (!expect(!host_view.lfp_cam_percent(kZone, percent),
	            "zone value: absent entry has no LFP_CAMPPERCENT writer"))
		return false;

	// A new 0x6F seeds current from value, then the one per-client-frame
	// ZoneTimerList advance applies rate before presentation reads the entry.
	host_loop.host_send(
			0x6F, zone_timer_value_body(kZone, 2, 10, 20, 2, 0xAA, 0xBB));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.lfp_cam_percent(kZone, percent) && percent == 32873,
	            "zone value: new entry seeds 620, advances to 622, and scales to 16.16"))
		return false;

	// An existing entry is re-targeted without snapping current to the new wire
	// value. Only the replacement rate advances it this frame: 622 - 3 = 619.
	host_loop.host_send(
			0x6F, zone_timer_value_body(kZone, 4, 19, 20, -3, 0xCC, 0xDD));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.lfp_cam_percent(kZone, percent) && percent == 32715,
	            "zone value: later 0x6F re-targets without replacing current"))
		return false;

	// Two messages in one receive pump apply in FIFO order, then the list advances
	// exactly once with the final message's rate: 619 - 9 = 610.
	host_loop.host_send(0x6F, zone_timer_value_body(kZone, 5, 1, 20, 7));
	host_loop.host_send(0x6F, zone_timer_value_body(kZone, 6, 3, 20, -9));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.lfp_cam_percent(kZone, percent) && percent == 32239,
	            "zone value: receive burst advances once after the final ordered update"))
		return false;

	const auto live = host_view.zone_states().find(kZone);
	return expect(live != host_view.zone_states().end() &&
	                      live->second.has_value &&
	                      live->second.value.mode == 6 &&
	                      live->second.value.value_s == 3 &&
	                      live->second.value.byte544 == 0 &&
	                      live->second.value.byte545 == 0,
	              "zone value: legacy latest-wire record remains available to UI callers");
}

bool run_zone_timer_channels_share_one_retail_entry() {
	ns::LoopbackChannel host_loop;
	np::ClientRuntime host_view(host_loop);
	constexpr uint16_t kZone = 0x3002;

	host_loop.host_send(
			0x53, zone_timer_window_body(kZone, 1, 4, 10, 40, 2));
	host_view.Client_ProcessNetworkFrame();
	const auto first = host_view.zone_states().find(kZone);
	if (!expect(first != host_view.zone_states().end() &&
	                    first->second.entry.mode_a == 1 &&
	                    first->second.entry.mode_b == 4 &&
	                    first->second.entry.window_current == 622 &&
	                    first->second.entry.window_target == 620 &&
	                    first->second.entry.window_limit == 2480 &&
	                    first->second.entry.window_rate == 2 &&
	                    first->second.entry.window_active &&
	                    first->second.entry.value_current == 0 &&
	                    !first->second.entry.value_active,
	            "zone channels: new 0x53 seeds and advances the shared entry window"))
		return false;

	// Because 0x53 already created the entry, this first 0x6F must not seed
	// DWORD 8 from its wire value. It activates value, disables window, and the
	// frame advance changes zero to one.
	host_loop.host_send(
			0x6F, zone_timer_value_body(kZone, 7, 30, 60, 1));
	host_view.Client_ProcessNetworkFrame();
	const auto &after_value = host_view.zone_states().at(kZone);
	int32_t percent = -1;
	if (!expect(after_value.entry.mode_a == 7 &&
	                    after_value.entry.mode_b == 7 &&
	                    !after_value.entry.window_active &&
	                    after_value.entry.value_current == 1 &&
	                    after_value.entry.value_target == 1860 &&
	                    after_value.entry.value_limit == 3720 &&
	                    after_value.entry.value_rate == 1 &&
	                    after_value.entry.value_active &&
	                    host_view.lfp_cam_percent(kZone, percent) &&
	                    percent == 17,
	            "zone channels: window-first entry prevents later 0x6F current seeding"))
		return false;

	// Same mode_b leaves the prior window current alone; 0x53 clears the value
	// target/limit and disables its channel without clearing DWORD 8.
	host_loop.host_send(
			0x53, zone_timer_window_body(kZone, 2, 7, 20, 40, 3));
	host_view.Client_ProcessNetworkFrame();
	const auto &same_mode = host_view.zone_states().at(kZone);
	if (!expect(same_mode.entry.window_current == 625 &&
	                    same_mode.entry.window_target == 1240 &&
	                    same_mode.entry.window_active &&
	                    same_mode.entry.value_current == 1 &&
	                    same_mode.entry.value_target == 0 &&
	                    same_mode.entry.value_limit == 0 &&
	                    !same_mode.entry.value_active &&
	                    host_view.lfp_cam_percent(kZone, percent) &&
	                    percent == 0x10000,
	            "zone channels: 0x53 preserves currents, clears value program, and advances window"))
		return false;

	// A changed mode_b resets DWORD 3 to the new start before this frame's tick.
	host_loop.host_send(
			0x53, zone_timer_window_body(kZone, 2, 8, 20, 40, 4));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.zone_states().at(kZone).entry.window_current == 1244,
	            "zone channels: changed window mode resets current before advance"))
		return false;

	// Retail only deactivates the window channel under its peculiar
	// target==limit && current==limit && positive-rate condition.
	host_loop.host_send(
			0x53, zone_timer_window_body(kZone, 2, 9, 40, 40, 2));
	host_view.Client_ProcessNetworkFrame();
	const auto &finished = host_view.zone_states().at(kZone);
	return expect(finished.entry.window_current == 2480 &&
	                      !finished.entry.window_active &&
	                      finished.has_value && finished.has_window,
	              "zone channels: exact window completion condition deactivates DWORD 7");
}

bool run_zone_timer_uses_wrapping_dword_arithmetic_and_signed_clamps() {
	ns::LoopbackChannel host_loop;
	np::ClientRuntime host_view(host_loop);
	int32_t percent = -1;

	// 34,636,833 * 62 = INT32_MAX-1. Adding two wraps to INT32_MIN,
	// then the signed low clamp sets current to zero.
	host_loop.host_send(
			0x6F,
			zone_timer_value_body(
					0x3003, 1, 34636833, 34636833, 2));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.zone_states().at(0x3003).entry.value_current == 0 &&
	                    host_view.lfp_cam_percent(0x3003, percent) &&
	                    percent == 0,
	            "zone arithmetic: per-frame ADD wraps as a retail signed DWORD"))
		return false;

	// 0x40000000 * 62 wraps to INT32_MIN before the signed low clamp.
	host_loop.host_send(
			0x6F,
			zone_timer_value_body(
					0x3004, 1, 0x40000000, 1, 0));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.zone_states().at(0x3004).entry.value_current == 0,
	            "zone arithmetic: wire-to-tick multiply wraps at 32 bits"))
		return false;

	host_loop.host_send(
			0x6F, zone_timer_value_body(0x3005, 1, 0, 20, 32767));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.lfp_cam_percent(0x3005, percent) &&
	                    percent == 0x10000,
	            "zone arithmetic: positive overflow past limit high-clamps"))
		return false;
	host_loop.host_send(
			0x6F, zone_timer_value_body(0x3005, 1, 19, 20, -32768));
	host_view.Client_ProcessNetworkFrame();
	return expect(host_view.lfp_cam_percent(0x3005, percent) && percent == 0,
	              "zone arithmetic: signed negative rate low-clamps");
}

bool run_joiner_zone_timer_preserves_mixed_wire_order() {
	const std::string client_scrk = "CLIENT-ZONE-TIMER-SCRK";
	const std::string server_scrk = "SERVER-ZONE-TIMER-SCRK";
	np::ClientRuntime client("ZoneJoiner");
	client.seed_session(
			0x66778899u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/true);

	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> mixed = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					// Window then value: the value sees an existing entry, does
					// not seed current, and is the sole active channel at tick.
					make_protocol_message(
							0x53, zone_timer_window_body(0x3006, 1, 4, 1, 10, 3)),
					make_protocol_message(
							0x6F, zone_timer_value_body(0x3006, 7, 8, 10, 5)),
					// Reverse order on another entry: window wins and value
					// current is retained but inactive.
					make_protocol_message(
							0x6F, zone_timer_value_body(0x3007, 2, 10, 20, 9)),
					make_protocol_message(
							0x53, zone_timer_window_body(0x3007, 3, 6, 1, 10, 4)),
			});
	client.receive(mixed.data(), mixed.size());
	(void)client.Client_ProcessNetworkFrame();

	const auto &window_then_value = client.zone_states().at(0x3006).entry;
	const auto &value_then_window = client.zone_states().at(0x3007).entry;
	int32_t percent = -1;
	if (!expect(window_then_value.value_current == 5 &&
	                    window_then_value.value_active &&
	                    !window_then_value.window_active &&
	                    client.lfp_cam_percent(0x3006, percent) &&
	                    percent == 528,
	            "zone joiner: 0x53 then 0x6F remains in packet order and ticks once"))
		return false;
	return expect(value_then_window.value_current == 620 &&
	                      !value_then_window.value_active &&
	                      value_then_window.window_current == 66 &&
	                      value_then_window.window_active &&
	                      client.lfp_cam_percent(0x3007, percent) &&
	                      percent == 0x10000,
	              "zone joiner: 0x6F then 0x53 remains in packet order and ticks once");
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

// Retail's periodic control quartet (S2C 0x39 / 0x42 / 0x43 / 0x68) must be dispatched, and
// its three challenge members (0x39/0x43/0x68) must be answered. The host counts unanswered
// challenges per player, and a live retail host was observed cutting
// the joiner's entire entity-record stream after eight silent rounds while the transport
// stayed healthy (no gap, still deployed) — the "disconnect" that is not a disconnect.
// Each reply's CONTENT is ignored by the host (the 0x1C handler discards its own checksum
// and only zeroes the counter; the 0x3D handler never parses the body), so this pins the
// witnessed SHAPES and, above all, that the replies exist.
// [orig: challenge senders NapiNPClientMsg_HandleChecksumChallenge @0x42E6D0 (-> 0x1C),
//  NapiNPClientMsg_0x043 @0x42FA90 (-> 0x08), NapiNPClientMsg_0x068 @0x42DAA0 (-> 0x3D);
//  host side NapiNPServerMsg_0x01C @0x501D40 (discard @0x501d71 + counter
//  reset @0x501d79), NapiNPServerMsg_0x03D @0x500EC0, validate_time_sync @0x502210]
bool build_retail_class8_charattr_table(
		np::CharAttrChallengeTable &table) {
	// Sections 1..7 only need to exist: CharAttr_LoadFromDef stops at the
	// first missing CHARACTER section. CHARACTER8 uses the authoritative JO
	// resource.pff/localres.pff text and therefore produces the captured row.
	static constexpr std::string_view source = R"CHARATTR(
[CHARACTER1]
[CHARACTER2]
[CHARACTER3]
[CHARACTER4]
[CHARACTER5]
[CHARACTER6]
[CHARACTER7]
[CHARACTER8]
STEALTH         = 25
HPBONUS         = 2
RECOIL_MUTE     = 0.75
XHAIR_MUTE      = 2.5
XHAIRDX_MUTE    = 2.5
SCOPE_MUTE      = 0.0
RELOAD_MUTE     = 0.5
JUNGLE_CAMMO    = 5305
DESERT_CAMMO    = 5305
ARCTIC_CAMMO    = 5305
RUN_MODIFIER    = 0
ATTRIBUTES      = KnifeBonus
)CHARATTR";
	return np::parse_charattr_challenge_table(
			reinterpret_cast<const uint8_t *>(source.data()),
			source.size(), table);
}

// All sixteen sections present, every property S2C 0x41 can clear nonzero and distinct per
// row, so a missed id or a wrong offset leaves a surviving dword the assertions can see.
bool build_full_charattr_table(np::CharAttrChallengeTable &table) {
	std::string source;
	for (int section = 1; section <= 16; ++section) {
		const std::string n = std::to_string(section);
		source += "[CHARACTER" + n + "]\n";
		source += "STEALTH      = " + n + "1\n";
		source += "HPBONUS      = " + n + "2\n";
		source += "MANABONUS    = " + n + "3\n";
		source += "RECOIL_MUTE  = " + n + "4\n";
		source += "RELOAD_MUTE  = " + n + "5\n";
		source += "XHAIR_MUTE   = " + n + "6\n";
		source += "XHAIRDX_MUTE = " + n + "7\n";
		source += "SCOPE_MUTE   = " + n + "8\n";
		source += "ATTRIBUTES   = AutoScope, Medic\n";
	}
	return np::parse_charattr_challenge_table(
			reinterpret_cast<const uint8_t *>(source.data()),
			source.size(), table);
}

bool run_charattr_challenge_table_matches_retail() {
	np::CharAttrChallengeTable table;
	if (!expect(build_retail_class8_charattr_table(table),
			"charattr parser accepts the authoritative CHARACTER syntax")) {
		return false;
	}

	np::CharAttrChallengeRow expected{};
	auto put_u32 = [&](std::size_t offset, uint32_t value) {
		expected[offset + 0] = static_cast<uint8_t>(value);
		expected[offset + 1] = static_cast<uint8_t>(value >> 8);
		expected[offset + 2] = static_cast<uint8_t>(value >> 16);
		expected[offset + 3] = static_cast<uint8_t>(value >> 24);
	};
	put_u32(0, 1);            // active
	expected[4] = 8;          // embedded class
	put_u32(8, 0x41C80000u);  // STEALTH 25.0f
	put_u32(12, 0x40000000u); // HPBONUS 2.0f
	put_u32(16, 0);           // absent MANABONUS
	put_u32(20, 0x3F400000u); // RECOIL_MUTE .75f
	put_u32(24, 0x3F000000u); // RELOAD_MUTE .5f
	put_u32(28, 0x40200000u); // XHAIR_MUTE 2.5f
	put_u32(32, 0x40200000u); // XHAIRDX_MUTE 2.5f
	put_u32(36, 0);           // SCOPE_MUTE 0.0f
	put_u32(40, 0x04);        // KnifeBonus
	put_u32(44, 5305);        // JUNGLE_CAMMO
	put_u32(48, 5305);        // DESERT_CAMMO
	put_u32(52, 5305);        // ARCTIC_CAMMO
	put_u32(56, 0);           // RUN_MODIFIER; +60..123 remain zero

	const np::CharAttrChallengeRow *row =
			np::find_charattr_challenge_row(table, 8);
	if (!expect(row != nullptr && *row == expected,
			"CHARACTER8 is the exact 124-byte g_CharAttr row")) {
		return false;
	}
	if (!expect(crc32_napi(row->data(), row->size()) == 0x22A25E01u,
			"JO CHARACTER8 row has the capture-derived CRC 0x22A25E01")) {
		return false;
	}
	if (!expect((0x0000F7EDu ^ 0x22A25E01u) == 0x22A2A9ECu &&
	                    (0x0000B380u ^ 0x22A25E01u) == 0x22A2ED81u,
			"the row reproduces both independent retail 0x1C replies")) {
		return false;
	}
	if (!expect(np::find_charattr_challenge_row(table, 0) == nullptr &&
	                    np::find_charattr_challenge_row(table, 17) == nullptr &&
	                    np::find_charattr_challenge_row(table, 255) == nullptr,
			"class 0 and wrapped out-of-range classes fail the embedded-id check")) {
		return false;
	}

	// Enumeration stops on the first missing section even if a later section
	// exists in the file.
	static constexpr std::string_view skipped =
			"[CHARACTER1]\nSTEALTH=1\n[CHARACTER3]\nSTEALTH=3\n";
	np::CharAttrChallengeTable gap_table;
	if (!expect(np::parse_charattr_challenge_table(
				reinterpret_cast<const uint8_t *>(skipped.data()),
				skipped.size(), gap_table) &&
	                    np::find_charattr_challenge_row(gap_table, 1) != nullptr &&
	                    np::find_charattr_challenge_row(gap_table, 3) == nullptr,
			"first missing CHARACTER section terminates retail enumeration")) {
		return false;
	}

	np::CharAttrChallengeTable empty_table;
	empty_table.rows[0].fill(0xFF);
	const np::CharAttrChallengeTable all_zero{};
	if (!expect(!np::parse_charattr_challenge_table(
				nullptr, 0, empty_table) &&
	                    empty_table.rows == all_zero.rows,
			"missing/empty source clears the table and leaves every slot inactive")) {
		return false;
	}

	np::CharAttrChallengeTable cleared = table;
	np::clear_charattr_challenge_property(cleared, 5);
	np::CharAttrChallengeRow expected_cleared = expected;
	std::fill(expected_cleared.begin() + 20, expected_cleared.begin() + 24, 0);
	if (!expect(*np::find_charattr_challenge_row(cleared, 8) == expected_cleared,
			"property 5 clears exactly RECOIL_MUTE across the checksum row")) {
		return false;
	}
	// The nine live property ids and the row offsets they zero.
	// [orig: CharAttr_SetSlotProperty @0x412890 -- cases 0/2/3/4/5/6/7/8/9]
	struct ClearCase {
		uint8_t property_id;
		std::size_t offset;
	};
	static constexpr ClearCase kClearMap[] = {
			{0, 40}, // ATTRIBUTES
			{2, 8},  // STEALTH
			{3, 12}, // HPBONUS
			{4, 16}, // MANABONUS
			{5, 20}, // RECOIL_MUTE
			{6, 28}, // XHAIR_MUTE
			{7, 36}, // SCOPE_MUTE
			{8, 32}, // XHAIRDX_MUTE
			{9, 24}, // RELOAD_MUTE
	};
	np::CharAttrChallengeTable full;
	if (!expect(build_full_charattr_table(full),
			"sixteen CHARACTER sections with every clearable property set")) {
		return false;
	}
	constexpr uint32_t kClearSeed = 0x3D5D0000u;
	for (const ClearCase &c : kClearMap) {
		np::CharAttrChallengeTable mutated = full;
		np::clear_charattr_challenge_property(mutated, c.property_id);
		bool rows_match = true;
		const auto field = static_cast<std::ptrdiff_t>(c.offset);
		for (std::size_t r = 0; r < np::kCharAttrChallengeRowCount; ++r) {
			np::CharAttrChallengeRow expected_row = full.rows[r];
			// Falsifiability guard: the fixture must have made this dword nonzero.
			rows_match = rows_match &&
					std::any_of(expected_row.begin() + field,
							expected_row.begin() + field + 4,
							[](uint8_t b) { return b != 0; });
			std::fill(expected_row.begin() + field,
					expected_row.begin() + field + 4, 0);
			rows_match = rows_match && mutated.rows[r] == expected_row;
		}
		const std::string rows_label = "S2C 0x41 property " +
				std::to_string(c.property_id) +
				" zeroes exactly its witnessed dword in all sixteen rows";
		if (!expect(rows_match, rows_label.c_str())) return false;
		const np::CharAttrChallengeRow *before_row =
				np::find_charattr_challenge_row(full, 8);
		const np::CharAttrChallengeRow *after_row =
				np::find_charattr_challenge_row(mutated, 8);
		if (!expect(before_row != nullptr && after_row != nullptr,
				"the class-8 checksum row stays selectable across the clear")) {
			return false;
		}
		const uint32_t before_reply =
				kClearSeed ^ crc32_napi(before_row->data(), before_row->size());
		const uint32_t after_reply =
				kClearSeed ^ crc32_napi(after_row->data(), after_row->size());
		const std::string crc_label = "S2C 0x41 property " +
				std::to_string(c.property_id) + " moves the C2S 0x1C checksum";
		if (!expect(before_reply != after_reply, crc_label.c_str())) return false;
	}
	// Only id 1 and ids >= 10 reach the original's default arm.
	for (const uint8_t no_op_id : {uint8_t{1}, uint8_t{10}, uint8_t{255}}) {
		np::CharAttrChallengeTable untouched = full;
		np::clear_charattr_challenge_property(untouched, no_op_id);
		const std::string label = "S2C 0x41 property " +
				std::to_string(no_op_id) + " is an exact no-op";
		if (!expect(untouched.rows == full.rows, label.c_str())) return false;
	}
	return true;
}

bool run_periodic_request_quartet_is_answered() {
	const std::string client_scrk = "CLIENT-QUARTET-SCRK";
	const std::string server_scrk = "SERVER-QUARTET-SCRK";
	uint64_t now_ms = 4242;
	np::JoinerConnection joiner("Quartet", [&now_ms] { return now_ms; });
	joiner.seed_in_match(0x77889900u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, 0x14B9);

	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	// The host fires all four together in ONE datagram, exactly as captured.
	const std::vector<uint8_t> quartet = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x39, {0x5D, 0x3D, 0x00, 0x00}), // CRC seed
					make_protocol_message(0x42, {0x00, 0x00}),             // input-state flags
					make_protocol_message(0x43, {0x11, 0x22, 0x33, 0x44}), // server stamp
					make_protocol_message(0x68, {0x32, 0x00, 0x00, 0x00}), // page start 50
			});
	const np::JoinerConnection::PollResult r =
			joiner.handle_datagram(quartet.data(), quartet.size());
	if (!expect(r.outbound.empty() && r.queued_send_messages.size() == 3,
			"periodic replies remain semantic until the client send boundary"))
		return false;
	const std::vector<std::vector<uint8_t>> framed_replies =
			joiner.frame_messages(r.queued_send_messages);
	if (!expect(framed_replies.size() == 1,
			"periodic replies from one receive boundary batch into one session packet"))
		return false;

	// Collect every inner message the joiner replied with across its datagrams.
	std::vector<ProtocolMessage> replies;
	for (const std::vector<uint8_t> &dg : framed_replies) {
		ProtocolPacketHeader h;
		std::vector<ProtocolMessage> msgs;
		if (decode_client_session(dg, client_scrk, h, msgs))
			for (ProtocolMessage &m : msgs) replies.push_back(std::move(m));
	}
	const ProtocolMessage *crc = nullptr;
	const ProtocolMessage *sync = nullptr;
	const ProtocolMessage *page = nullptr;
	for (const ProtocolMessage &m : replies) {
		if (m.tag == 0x1C) crc = &m;
		else if (m.tag == 0x08) sync = &m;
		else if (m.tag == 0x3D) page = &m;
	}
	if (!expect(crc != nullptr && sync != nullptr && page != nullptr,
			"the quartet draws all three replies (0x1C anti-cheat, 0x08 time-sync, "
			"0x3D loaded-model page) — silence is what stops the host's entity stream")) {
		return false;
	}
	// 0x1C: four bytes, the witnessed inactive-charattr-row value.
	if (!expect(crc->payload == std::vector<uint8_t>({0x00, 0x00, 0x00, 0x00}),
			"0x1C is the witnessed 4-byte inactive-charattr-row checksum")) {
		return false;
	}
	// 0x08: [u32 echoed server stamp][u32 local clock] — the host validates the deltas.
	if (!expect(sync->payload.size() == 8 && sync->payload[0] == 0x11 &&
				sync->payload[1] == 0x22 && sync->payload[2] == 0x33 &&
				sync->payload[3] == 0x44,
			"0x08 echoes the server stamp then appends our own clock")) {
		return false;
	}
	// 0x3D: the header-only page form, echoing the requested cursor.
	if (!expect(page->payload == std::vector<uint8_t>({0x32, 0x00, 0x00, 0x00}),
			"0x3D carries the requested page cursor"))
		return false;

	// Activate both modeled data terms: the byte-exact charattr CHARACTER table and a
	// deliberately ordered/duplicated renderer-definition snapshot. The response
	// must use the challenge seed and page the frozen values VERBATIM, at most fifty
	// dwords after the echoed cursor. This is not a live network-entity census.
	np::CharAttrChallengeTable charattr;
	if (!expect(build_retail_class8_charattr_table(charattr),
			"build production charattr challenge table")) {
		return false;
	}
	const np::CharAttrChallengeRow class8_row =
			*np::find_charattr_challenge_row(charattr, 8);
	joiner.set_charattr_challenge_table(charattr);
	std::vector<uint32_t> model_snapshot;
	for (uint32_t i = 0; i < 60; ++i)
		model_snapshot.push_back(0xC0000000u + i);
	model_snapshot[1] = 0x11223344u;
	model_snapshot[2] = 0x11223344u; // duplicate must survive
	model_snapshot[3] = 0x55667788u; // order must survive
	joiner.set_loaded_model_challenge_snapshot(std::move(model_snapshot));

	constexpr uint32_t kChallenge = 0x01020304u;
	constexpr uint32_t kStart = 1u;
	const std::vector<uint8_t> active = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x5A, {0x08, 0xFF}),
					make_protocol_message(0x39, {0x04, 0x03, 0x02, 0x01}),
					make_protocol_message(0x68, {0x01, 0x00, 0x00, 0x00}),
			});
	const np::JoinerConnection::PollResult active_result =
			joiner.handle_datagram(active.data(), active.size());
	if (!expect(active_result.outbound.empty() &&
	                    active_result.queued_send_messages.size() == 2,
			"active challenge pair remains queued for one send boundary"))
		return false;
	const std::vector<std::vector<uint8_t>> active_framed =
			joiner.frame_messages(active_result.queued_send_messages);
	ProtocolPacketHeader active_header;
	std::vector<ProtocolMessage> active_messages;
	if (!expect(decode_client_session(
				active_framed.size() == 1 ? active_framed[0] : std::vector<uint8_t>{},
				client_scrk, active_header, active_messages) &&
	                    active_messages.size() == 2,
			"decode active charattr/model challenge response"))
		return false;
	const uint32_t expected_crc =
			kChallenge ^ crc32_napi(class8_row.data(), class8_row.size());
	const std::vector<uint8_t> expected_crc_body = {
			static_cast<uint8_t>(expected_crc),
			static_cast<uint8_t>(expected_crc >> 8),
			static_cast<uint8_t>(expected_crc >> 16),
			static_cast<uint8_t>(expected_crc >> 24),
	};
	if (!expect(active_messages[0].tag == 0x1C &&
	                    active_messages[0].payload == expected_crc_body,
			"active 0x1C is seed XOR CRC over the exact 124-byte CHARACTER row"))
		return false;
	if (!expect(active_messages[1].tag == 0x3D &&
	                    active_messages[1].payload.size() == 4 + 50 * sizeof(uint32_t),
			"active 0x3D contains the cursor plus at most fifty model rows"))
		return false;
	if (!expect(active_messages[1].payload[0] == kStart &&
	                    active_messages[1].payload[4] == 0x44 &&
	                    active_messages[1].payload[5] == 0x33 &&
	                    active_messages[1].payload[6] == 0x22 &&
	                    active_messages[1].payload[7] == 0x11 &&
	                    active_messages[1].payload[8] == 0x44 &&
	                    active_messages[1].payload[9] == 0x33 &&
	                    active_messages[1].payload[10] == 0x22 &&
	                    active_messages[1].payload[11] == 0x11 &&
	                    active_messages[1].payload[12] == 0x88 &&
	                    active_messages[1].payload[13] == 0x77 &&
	                    active_messages[1].payload[14] == 0x66 &&
	                    active_messages[1].payload[15] == 0x55,
			"model page preserves producer order and duplicate rows verbatim"))
		return false;

	// S2C 0x41 mutates the table synchronously. Its first byte selects the
	// property and trailing bytes are ignored, so the following same-packet
	// challenge must hash RECOIL_MUTE as four zero bytes.
	const std::vector<uint8_t> clear_then_challenge = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x41, {0x05, 0xA5}),
					make_protocol_message(0x39, {0x04, 0x03, 0x02, 0x01}),
			});
	const np::JoinerConnection::PollResult cleared_result =
			joiner.handle_datagram(
					clear_then_challenge.data(), clear_then_challenge.size());
	const std::vector<std::vector<uint8_t>> cleared_framed =
			joiner.frame_messages(cleared_result.queued_send_messages);
	ProtocolPacketHeader cleared_header;
	std::vector<ProtocolMessage> cleared_messages;
	if (!expect(cleared_framed.size() == 1 &&
	                    decode_client_session(
							    cleared_framed[0], client_scrk,
							    cleared_header, cleared_messages) &&
	                    cleared_messages.size() == 1 &&
	                    cleared_messages[0].tag == 0x1C,
			"same-packet 0x41 then 0x39 emits one ordered checksum reply")) {
		return false;
	}
	np::CharAttrChallengeRow cleared_row = class8_row;
	std::fill(cleared_row.begin() + 20, cleared_row.begin() + 24, 0);
	const uint32_t cleared_crc =
			kChallenge ^ crc32_napi(cleared_row.data(), cleared_row.size());
	const std::vector<uint8_t> cleared_crc_body = {
			static_cast<uint8_t>(cleared_crc),
			static_cast<uint8_t>(cleared_crc >> 8),
			static_cast<uint8_t>(cleared_crc >> 16),
			static_cast<uint8_t>(cleared_crc >> 24),
	};
	if (!expect(cleared_messages[0].payload == cleared_crc_body,
			"S2C 0x41 property 5 clears RECOIL_MUTE before the next 0x39")) {
		return false;
	}

	// Class changes are ordered too. The wrapped class 17 cannot alias
	// CHARACTER1 because the embedded class byte must match; restoring class 8
	// within the same packet makes only the second challenge active.
	const std::vector<uint8_t> ordered_classes = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x5A, {0x11, 0xFF}),
					make_protocol_message(0x39, {0x04, 0x03, 0x02, 0x01}),
					make_protocol_message(0x5A, {0x08, 0xFF}),
					make_protocol_message(0x39, {0x04, 0x03, 0x02, 0x01}),
			});
	const np::JoinerConnection::PollResult ordered_result =
			joiner.handle_datagram(ordered_classes.data(), ordered_classes.size());
	const std::vector<std::vector<uint8_t>> ordered_framed =
			joiner.frame_messages(ordered_result.queued_send_messages);
	ProtocolPacketHeader ordered_header;
	std::vector<ProtocolMessage> ordered_messages;
	if (!expect(ordered_framed.size() == 1 &&
	                    decode_client_session(
							    ordered_framed[0], client_scrk,
							    ordered_header, ordered_messages) &&
	                    ordered_messages.size() == 2,
			"ordered class changes yield two challenge replies")) {
		return false;
	}
	return expect(ordered_messages[0].payload ==
	                      std::vector<uint8_t>({0, 0, 0, 0}) &&
	                      ordered_messages[1].payload == cleared_crc_body,
			"class 17 is inactive and later class 8 observes the retained 0x41 clear");
}

bool run_reverse_rtt_probe_is_echoed() {
	const std::string client_scrk = "CLIENT-REVERSE-RTT-SCRK";
	const std::string server_scrk = "SERVER-REVERSE-RTT-SCRK";
	np::JoinerConnection joiner("ReverseRtt", [] { return uint64_t{0x55667788u}; });
	joiner.seed_in_match(0x10203040u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> probe = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x57, {0x44, 0x33, 0x22, 0x11, 0x01})});
	const np::JoinerConnection::PollResult result =
			joiner.handle_datagram(probe.data(), probe.size());
	if (!expect(result.outbound.empty() && result.queued_send_messages.size() == 1,
			"S2C 0x57 flag=1 queues one C2S response"))
		return false;
	const std::vector<std::vector<uint8_t>> framed =
			joiner.frame_messages(result.queued_send_messages);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(framed.size() == 1 && decode_client_session(
				framed[0], client_scrk, header, messages),
			"decode reverse RTT response"))
		return false;
	return expect(messages.size() == 1 && messages[0].tag == 0x2C &&
	                    messages[0].payload ==
	                            std::vector<uint8_t>({0x44, 0x33, 0x22, 0x11, 0x00}),
			"reverse RTT response echoes the timestamp and clears the flag");
}

bool run_network_spawn_does_not_mutate_loaded_model_snapshot() {
	const std::string client_scrk = "CLIENT-MODEL-SNAPSHOT-SCRK";
	const std::string server_scrk = "SERVER-MODEL-SNAPSHOT-SCRK";
	np::ClientRuntime client("ModelSnapshot", [] { return uint64_t{1000}; });
	client.seed_session(
			0x20304050u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	client.set_loaded_model_challenge_snapshot(
			{0xA1B2C3D4u, 0u, 0u});

	constexpr uint16_t kFreshHandle = 0x0123;
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> receive_boundary = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(
							0x0C,
							make_organic_spawn(
									kFreshHandle, "OtherPlayer",
									0x10000, 0x20000, 0x30000, 0, 2, 9)),
					make_protocol_message(0x68, {0x00, 0x00, 0x00, 0x00}),
			});
	client.receive(receive_boundary.data(), receive_boundary.size());
	const std::vector<std::vector<uint8_t>> outbound =
			client.Client_ProcessNetworkFrame(1);
	if (!expect(outbound.size() == 1,
			"model challenge reply batches at the same send boundary as a spawn"))
		return false;

	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(decode_client_session(
				outbound[0], client_scrk, header, messages),
			"decode frozen model challenge response"))
		return false;
	for (const ProtocolMessage &message : messages) {
		if (message.tag != 0x3D) continue;
		return expect(
				message.payload.size() == 16 &&
				        message.payload[0] == 0 &&
				        message.payload[4] == 0xD4 &&
				        message.payload[5] == 0xC3 &&
				        message.payload[6] == 0xB2 &&
				        message.payload[7] == 0xA1 &&
				        message.payload[8] == 0 &&
				        message.payload[9] == 0 &&
				        message.payload[10] == 0 &&
				        message.payload[11] == 0 &&
				        message.payload[12] == 0 &&
				        message.payload[13] == 0 &&
				        message.payload[14] == 0 &&
				        message.payload[15] == 0,
				"same-packet S2C spawn cannot enter or reorder the frozen model page");
	}
	return expect(false, "same-boundary 0x68 produced a C2S 0x3D reply");
}

bool run_split_batch_keeps_deployment_pick_ack_causal() {
	const std::string client_scrk = "CLIENT-PICK-SPLIT-SCRK";
	const std::string server_scrk = "SERVER-PICK-SPLIT-SCRK";
	np::ClientRuntime client("PickSplit", [] { return uint64_t{2000}; });
	client.seed_session(
			0x30405060u, 1u, client_scrk, server_scrk,
			10, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	// Death re-enters AwaitDeployPick during the receive fold. Fill the same
	// receive boundary with enough 0x43 requests to make their C2S 0x08 replies
	// split across the 1300-byte send ceiling.
	FrameUpdate death;
	death.mount_handle = 0xFFFF;
	death.health = 0;
	std::vector<ProtocolMessage> inbound = {
			make_protocol_message(0x0A, encode_frame_update(death)),
	};
	for (uint32_t i = 0; i < 180; ++i) {
		inbound.push_back(make_protocol_message(
				0x43,
				{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), 0, 0}));
	}
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> challenge_burst =
			frame_server_session(server_tx, server_scrk, 1u, inbound);
	client.receive(challenge_burst.data(), challenge_burst.size());
	client.queue_deployment_pick(0xFFFF);
	const std::vector<std::vector<uint8_t>> outbound =
			client.Client_ProcessNetworkFrame(1);
	if (!expect(outbound.size() >= 2 && !client.deployed(),
			"challenge replies force a split while the deploy release remains pending"))
		return false;

	uint32_t first_sequence = 0;
	uint32_t pick_sequence = 0;
	bool saw_pick = false;
	for (std::size_t i = 0; i < outbound.size(); ++i) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!expect(decode_client_session(
					outbound[i], client_scrk, header, messages),
				"decode split deployment batch"))
			return false;
		if (i == 0) first_sequence = header.seq_num;
		for (const ProtocolMessage &message : messages) {
			if (message.tag == 0x0E) {
				saw_pick = true;
				pick_sequence = header.seq_num;
			}
		}
	}
	if (!expect(saw_pick && pick_sequence == first_sequence,
			"deployment pick occupies the sequence recorded before split framing"))
		return false;

	// An ACK covering exactly that first packet is sufficient and causal: it
	// cannot be an ACK of a prefix packet that preceded the actual 0x0E.
	server_tx.last_inbound_seq = first_sequence;
	const std::vector<uint8_t> release = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x5A, {})});
	client.receive(release.data(), release.size());
	(void)client.Client_ProcessNetworkFrame(2);
	return expect(client.deployed(),
			"0x5A covering the pick's actual packet releases redeployment");
}

bool run_live_frame_uses_wall_clock_and_batches_mount_requests() {
	const std::string client_scrk = "CLIENT-LIVE-BATCH-SCRK";
	const std::string server_scrk = "SERVER-LIVE-BATCH-SCRK";
	uint64_t now_ms = 0x11223344u;
	np::ClientRuntime client("LiveBatch", [&now_ms] { return now_ms; });
	client.seed_session(
			0x55667788u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	if (!expect(client.queue_vehicle_attach(0x1007, 6),
			"a deployed joiner can queue C2S 0x26 attach"))
		return false;
	client.queue_loadout_resubmit();
	const std::vector<std::vector<uint8_t>> attach_frame =
			client.Client_ProcessNetworkFrame(999u);
	if (!expect(attach_frame.size() == 1,
			"loadout, attach, and the per-frame RTT probe share one outer datagram"))
		return false;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(decode_client_session(
				attach_frame[0], client_scrk, header, messages) &&
	                    messages.size() == 3,
			"decode batched attach frame"))
		return false;
	if (!expect(messages[0].tag == 0x2F,
			"armory ACCEPT loadout re-submit shares the live send boundary"))
		return false;
	if (!expect(messages[1].tag == 0x26 &&
	                    messages[1].payload ==
	                            std::vector<uint8_t>({0x02, 0x00, 0x07, 0x10, 0x06, 0x00}),
			"C2S 0x26 carries self, vehicle, one-based model bone and pad"))
		return false;
	if (!expect(messages[2].tag == 0x2C &&
	                    messages[2].payload ==
	                            std::vector<uint8_t>({0x44, 0x33, 0x22, 0x11, 0x01}),
			"outbound RTT uses monotonic milliseconds rather than simulation tick"))
		return false;

	now_ms = 0x55667788u;
	if (!expect(client.queue_vehicle_detach(0x1007),
			"a deployed joiner can queue C2S 0x27 detach"))
		return false;
	const std::vector<std::vector<uint8_t>> detach_frame =
			client.Client_ProcessNetworkFrame(1000u);
	if (!expect(detach_frame.size() == 1 &&
	                    decode_client_session(
	                            detach_frame[0], client_scrk, header, messages) &&
	                    messages.size() == 2 &&
	                    messages[0].tag == 0x27 &&
	                    messages[0].payload ==
	                            std::vector<uint8_t>({0x02, 0x00, 0x07, 0x10, 0x00, 0x00}) &&
	                    messages[1].tag == 0x2C &&
	                    messages[1].payload ==
	                            std::vector<uint8_t>({0x88, 0x77, 0x66, 0x55, 0x01}),
			"C2S 0x27 batches with a fresh RTT ping on the immediately consecutive "
			"deployed frame (the retail 62 counter is not a send gate)"))
		return false;
	return true;
}

bool run_same_packet_holdoff_defers_admission_replies() {
	constexpr uint32_t kServerKey = 0x31415926u;
	constexpr uint32_t kConnectionId = 7;
	const std::string server_scrk = "SERVER-HOLDOFF-ADMISSION-SCRK";
	np::ClientRuntime client("HoldoffAdmission");

	const std::vector<uint8_t> hello_datagram = client.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
	                    opcode == SESSION_OPCODE_CLIENT_HELLO &&
	                    parse_client_hello(body.data(), body.size(), hello),
			"holdoff-admission decodes ClientHello"))
		return false;

	ServerHello server_hello = build_server_hello(
			hello, 0x7F000001u, 32769);
	server_hello.hk = 0x27182818u;
	server_hello.sus2 = "revx02";
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO,
			server_hello_to_bytes(server_hello));
	client.receive(
			server_hello_datagram.data(), server_hello_datagram.size());
	const std::vector<std::vector<uint8_t>> auth_frame =
			client.Client_ProcessNetworkFrame(0);
	ClientAuth client_auth;
	if (!expect(auth_frame.size() == 1 &&
	                    nw_decode_inbound(
			                    auth_frame[0].data(), auth_frame[0].size(),
			                    opcode, body) &&
	                    opcode == SESSION_OPCODE_CLIENT_AUTH &&
	                    parse_client_auth(body.data(), body.size(), client_auth),
			"holdoff-admission reaches ClientAuth"))
		return false;

	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey,
			server_scrk, "", "", "", false);
	server_auth.mi = kConnectionId;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH,
			server_auth_to_bytes(server_auth));
	client.receive(
			server_auth_datagram.data(), server_auth_datagram.size());
	if (!expect(client.Client_ProcessNetworkFrame(1).empty(),
			"ServerAuth waits for connection settings"))
		return false;

	// One admitted S2C session packet both completes the initial settings leg
	// (which reactively builds the ACK + JOIN pair) and installs a direction-1
	// field-3 holdoff. The gate is evaluated after the receive pump, so neither
	// reply may escape this same frame.
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> settings = frame_server_session(
			server_tx, server_scrk, client_auth.ck,
			{
					make_protocol_message(
							0x00,
							{0x00, 0x00, 0x20, 0x00, 0x00,
							 0x14, 0x05, 0x00, 0x00},
							0xA0),
					make_protocol_message(
							0x00,
							{0x01, 0x00, 0x20, 0x00, 0x00,
							 0x14, 0x05, 0x00, 0x00},
							0xA0),
					make_protocol_message(
							0x00,
							{0x01, 0x08, 0x00, 0x00, 0x00,
							 0x02, 0x00, 0x00, 0x00},
							0xA0),
			});
	client.receive(settings.data(), settings.size());
	const std::vector<std::vector<uint8_t>> first_held =
			client.Client_ProcessNetworkFrame(2);
	if (!expect(first_held.empty() &&
	                    client.send_holdoff_countdown() == 1,
			"same-packet field-3 holds the reactive ACK + JOIN pair"))
		return false;
	const std::vector<std::vector<uint8_t>> second_held =
			client.Client_ProcessNetworkFrame(3);
	if (!expect(second_held.empty() &&
	                    client.send_holdoff_countdown() == 0,
			"admission replies remain queued through the final held frame"))
		return false;

	const std::vector<std::vector<uint8_t>> released =
			client.Client_ProcessNetworkFrame(4);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(released.size() == 2 &&
	                    decode_client_session(
			                    released[0], client_auth.scrk,
			                    header, messages) &&
	                    matches_client_header(
			                    header, kServerKey, 1, 1) &&
	                    messages.empty(),
			"first open pump preserves the admission ACK packet"))
		return false;
	if (!expect(decode_client_session(
				released[1], client_auth.scrk, header, messages) &&
	                    matches_client_header(
			                    header, kServerKey, 2, 1) &&
	                    messages.size() == 1 &&
	                    messages[0].tag == 0x00 &&
	                    messages[0].payload ==
			                    retail_expansion_join_request(
					                    server_hello.sus2),
			"first open pump preserves the following JOIN packet and sequence"))
		return false;
	return true;
}

bool run_holdoff_defers_retained_session_reconstruction() {
	const std::string client_scrk = "CLIENT-HOLDOFF-RETAINED-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-RETAINED-SCRK";
	uint64_t now_ms = 0x10203040u;
	np::ClientRuntime client(
			"HoldoffRetained", [&now_ms] { return now_ms; });
	client.seed_session(
			0x55667788u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	const std::vector<std::vector<uint8_t>> original =
			client.Client_ProcessNetworkFrame(0);
	ProtocolPacketHeader original_header;
	std::vector<ProtocolMessage> original_messages;
	if (!expect(original.size() == 1 &&
	                    decode_client_session(
			                    original[0], client_scrk,
			                    original_header, original_messages) &&
	                    original_header.seq_num == 1 &&
	                    original_messages.size() == 1 &&
	                    original_messages[0].tag == 0x2C,
			"holdoff-retained seeds one retained C2S packet"))
		return false;

	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> hold = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(
					0x00,
					{0x01, 0x08, 0x00, 0x00, 0x00,
					 0x02, 0x00, 0x00, 0x00},
					0xA0)});
	client.receive(hold.data(), hold.size());
	if (!expect(client.Client_ProcessNetworkFrame(1).empty() &&
	                    client.send_holdoff_countdown() == 1,
			"field-3 closes the pump before the resend request"))
		return false;

	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(
				1u, {original_header.seq_num}, resend_body),
			"holdoff-retained builds ServerResendList"))
		return false;
	const std::vector<uint8_t> resend = nw_encode_outbound(
			SESSION_OPCODE_SERVER_RESEND_LIST, std::move(resend_body));
	client.receive(resend.data(), resend.size());
	const std::vector<std::vector<uint8_t>> final_held =
			client.Client_ProcessNetworkFrame(2);
	if (!expect(final_held.empty() &&
	                    client.send_holdoff_countdown() == 0,
			"retained reconstruction cannot bypass the final held frame"))
		return false;

	const std::vector<std::vector<uint8_t>> released =
			client.Client_ProcessNetworkFrame(3);
	ProtocolPacketHeader resent_header;
	std::vector<ProtocolMessage> resent_messages;
	if (!expect(released.size() == 2 &&
	                    decode_client_session(
			                    released[0], client_scrk,
			                    resent_header, resent_messages) &&
	                    resent_header.seq_num == original_header.seq_num &&
	                    resent_header.ack_count == 1 &&
	                    resent_messages.size() == 1 &&
	                    resent_messages[0].tag == 0x2C &&
	                    resent_messages[0].payload ==
			                    original_messages[0].payload,
			"first open pump sends the exact retained sequence with current ACK"))
		return false;
	ProtocolPacketHeader live_header;
	std::vector<ProtocolMessage> live_messages;
	return expect(decode_client_session(
				released[1], client_scrk,
				live_header, live_messages) &&
	                      live_header.seq_num == 2 &&
	                      live_messages.size() == 1 &&
	                      live_messages[0].tag == 0x2C,
			"retained reconstruction stays ordered before the fresh live send");
}

bool run_settings_send_holdoff_blocks_exact_frame_count() {
	const std::string client_scrk = "CLIENT-HOLDOFF-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-SCRK";
	uint64_t now_ms = 1000;
	np::ClientRuntime client("Holdoff", [&now_ms] { return now_ms; });
	client.seed_session(
			0x66778899u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> update = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(
							0x00, {0x01, 0x08, 0x00, 0x00, 0x00,
							       0x02, 0x00, 0x00, 0x00}, 0xA0),
					make_protocol_message(
							0x57, {0x44, 0x33, 0x22, 0x11, 0x01}),
					make_protocol_message(
							0x39, {0x5D, 0x3D, 0x00, 0x00}),
					make_protocol_message(
							0x43, {0x11, 0x22, 0x33, 0x44}),
					make_protocol_message(
							0x68, {0x00, 0x00, 0x00, 0x00}),
			});
	client.receive(update.data(), update.size());

	auto semantic_tags = [&](const std::vector<std::vector<uint8_t>> &datagrams) {
		std::vector<uint8_t> tags;
		for (const std::vector<uint8_t> &datagram : datagrams) {
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> decoded;
			if (!decode_client_session(datagram, client_scrk, header, decoded)) continue;
			for (const ProtocolMessage &message : decoded)
				tags.push_back(message.tag);
		}
		return tags;
	};
	const std::vector<std::vector<uint8_t>> first_frame =
			client.Client_ProcessNetworkFrame(10);
	if (!expect(first_frame.empty() && client.send_holdoff_countdown() == 1,
			"field-3 holdoff suppresses every first-frame datagram and decrements once"))
		return false;
	++now_ms;
	const std::vector<std::vector<uint8_t>> second_frame =
			client.Client_ProcessNetworkFrame(11);
	if (!expect(second_frame.empty() && client.send_holdoff_countdown() == 0,
			"field-3 holdoff suppresses every datagram for exactly its second frame"))
		return false;
	++now_ms;
	const std::vector<std::vector<uint8_t>> third_frame =
			client.Client_ProcessNetworkFrame(12);
	const std::vector<uint8_t> third_tags = semantic_tags(third_frame);
	return expect(
			third_frame.size() == 1 &&
			        third_tags ==
			                std::vector<uint8_t>(
						{0x2C, 0x1C, 0x08, 0x3D, 0x2C}),
			"send block reopens by batching held receive replies with the live RTT");
}

bool run_send_holdoff_defers_due_housekeeping() {
	const std::string client_scrk = "CLIENT-HOLDOFF-DUE-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-DUE-SCRK";
	uint64_t now_ms = 2000;
	np::ClientRuntime client("HoldoffDue", [&now_ms] { return now_ms; });
	client.seed_session(
			0x778899AAu, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	auto semantic_tags = [&](const std::vector<std::vector<uint8_t>> &datagrams) {
		std::vector<uint8_t> tags;
		for (const std::vector<uint8_t> &datagram : datagrams) {
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> decoded;
			if (!decode_client_session(datagram, client_scrk, header, decoded)) continue;
			for (const ProtocolMessage &message : decoded)
				tags.push_back(message.tag);
		}
		return tags;
	};

	// Prime the strict `++timer > 310` cadence to exactly 310. The next
	// frame is the due 0x4C frame.
	for (uint32_t frame = 0; frame < 310; ++frame) {
		(void)client.Client_ProcessNetworkFrame(frame);
		++now_ms;
	}
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> hold = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(
					0x00, {0x01, 0x08, 0x00, 0x00, 0x00,
					       0x02, 0x00, 0x00, 0x00}, 0xA0)});
	client.receive(hold.data(), hold.size());

	const std::vector<uint8_t> first_held =
			semantic_tags(client.Client_ProcessNetworkFrame(310));
	if (!expect(first_held.empty() &&
	                    client.send_holdoff_countdown() == 1,
			"a due 0x4C remains queued while holdoff closes the send pump"))
		return false;
	++now_ms;
	const std::vector<uint8_t> second_held =
			semantic_tags(client.Client_ProcessNetworkFrame(311));
	if (!expect(second_held.empty() &&
	                    client.send_holdoff_countdown() == 0,
			"queued housekeeping cannot leak on the final held frame"))
		return false;
	++now_ms;
	const std::vector<uint8_t> released =
			semantic_tags(client.Client_ProcessNetworkFrame(312));
	return expect(released == std::vector<uint8_t>({0x4C, 0x2C}),
			"the first open send pump flushes deferred 0x4C before live RTT");
}

bool run_start_resets_reusable_runtime_state() {
	const std::string client_scrk = "CLIENT-REUSE-SCRK";
	const std::string server_scrk = "SERVER-REUSE-SCRK";
	uint64_t now_ms = 0x01020304u;
	np::ClientRuntime client("Reusable", [&now_ms] { return now_ms; });
	client.seed_session(
			0x44556677u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	if (!expect(client.queue_vehicle_attach(0x1007, 3),
			"reuse fixture queues session-bound gameplay traffic"))
		return false;
	client.queue_loadout_resubmit();
	client.queue_deployment_pick(0xFFFFu);

	SessionSequencing first_server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> authoritative = frame_server_session(
			first_server_tx, server_scrk, 1u,
			{
					make_protocol_message(
							0x00, {0x01, 0x08, 0x00, 0x00, 0x00,
							       0x04, 0x00, 0x00, 0x00}, 0xA0),
					make_protocol_message(0x5A, {0x08, 0xFF}),
					make_protocol_message(
							0x6F,
							{
									0x01, 0x30, 0x02,
									0x1E, 0x00, 0x00, 0x00,
									0x3C, 0x00, 0x00, 0x00,
									0x01, 0x00, 0xAA, 0xBB,
							}),
			});
	client.receive(authoritative.data(), authoritative.size());
	(void)client.Client_ProcessNetworkFrame(10);
	if (!expect(client.authoritative_loadout_revision() == 1 &&
	                    client.zone_states().count(0x3001u) == 1 &&
	                    client.send_holdoff_countdown() == 3,
			"reuse fixture populates authoritative and cadence state"))
		return false;

	client.view().state().anchor_x = 0x12345678;
	client.view().apply(0x49, {0x02, 0x00, 0x07, 0x10});

	// Leave a sequence-one packet queued at the receive boundary. If start()
	// fails to discard the old FIFO, it is valid under the freshly seeded
	// session below and reinstalls a nine-frame holdoff.
	SessionSequencing stale_server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> stale_inbound = frame_server_session(
			stale_server_tx, server_scrk, 1u,
			{make_protocol_message(
					0x00, {0x01, 0x08, 0x00, 0x00, 0x00,
					       0x09, 0x00, 0x00, 0x00}, 0xA0)});
	client.receive(stale_inbound.data(), stale_inbound.size());

	const std::vector<uint8_t> hello = client.start();
	if (!expect(!hello.empty() &&
	                    client.phase() == np::JoinerConnection::Phase::Hello,
			"reused runtime starts a fresh handshake"))
		return false;
	if (!expect(client.authoritative_loadout_revision() == 0 &&
	                    client.zone_states().empty() &&
	                    client.send_holdoff_countdown() == 0 &&
	                    !client.deployed(),
			"start clears prior authoritative, cadence, and deploy state"))
		return false;
	if (!expect(client.state().anchor_x == 0 &&
	                    client.drain_reload_notifications().empty(),
			"start clears decoded view state and pending notifications"))
		return false;

	client.seed_session(
			0x44556677u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	const std::vector<std::vector<uint8_t>> fresh_frame =
			client.Client_ProcessNetworkFrame(11);
	std::vector<uint8_t> tags;
	for (const std::vector<uint8_t> &datagram : fresh_frame) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(
					datagram, client_scrk, header, messages))
			continue;
		for (const ProtocolMessage &message : messages)
			tags.push_back(message.tag);
	}
	return expect(tags == std::vector<uint8_t>({0x2C}) &&
	                      client.send_holdoff_countdown() == 0,
			"fresh session emits no stale FIFO, action, loadout, or deployment work");
}

bool run_joiner_correlates_handshake_echoes() {
	np::JoinerConnection joiner("EchoGuard");
	const std::vector<uint8_t> hello_datagram = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
	                    parse_client_hello(body.data(), body.size(), hello),
			"echo-guard decodes ClientHello"))
		return false;
	ServerHello wrong_hello = build_server_hello(hello, 0x7F000001u, 32769);
	wrong_hello.ci ^= 0x100u;
	const std::vector<uint8_t> wrong_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(wrong_hello));
	const np::JoinerConnection::PollResult ignored_hello =
			joiner.handle_datagram(wrong_hello_datagram.data(), wrong_hello_datagram.size());
	if (!expect(ignored_hello.outbound.empty() &&
	                    joiner.phase() == np::JoinerConnection::Phase::Hello,
			"a ServerHello for another CI is ignored without advancing"))
		return false;

	ServerHello valid_hello = build_server_hello(hello, 0x7F000001u, 32769);
	const std::vector<uint8_t> valid_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(valid_hello));
	const np::JoinerConnection::PollResult auth_request =
			joiner.handle_datagram(valid_hello_datagram.data(), valid_hello_datagram.size());
	if (!expect(auth_request.outbound.size() == 1 &&
	                    joiner.phase() == np::JoinerConnection::Phase::Auth,
			"matching ServerHello advances to Auth"))
		return false;
	ClientAuth client_auth;
	if (!expect(nw_decode_inbound(
				auth_request.outbound[0].data(), auth_request.outbound[0].size(),
				opcode, body) &&
	                    parse_client_auth(body.data(), body.size(), client_auth),
			"echo-guard decodes ClientAuth"))
		return false;
	ServerAuth wrong_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, 0x11223344u,
			"SERVER-ECHO-GUARD-SCRK", "", "", "", false);
	wrong_auth.ck ^= 0x200u;
	const std::vector<uint8_t> wrong_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(wrong_auth));
	(void)joiner.handle_datagram(wrong_auth_datagram.data(), wrong_auth_datagram.size());
	return expect(joiner.phase() == np::JoinerConnection::Phase::Auth &&
	                      joiner.server_key() == 0,
			"a ServerAuth with mismatched CK cannot install server keys");
}

// --- Round-5 hardening ---------------------------------------------------------------------

// The 0x04 team latch must be falsifiable: every value in the old pin chain was the same
// hardcoded 2 (test helper default, joiner default, host fallback), so deleting the latch kept
// the suite green. Read the wire team byte back through frame_loadout_resubmit — the same
// assigned_team_ source the 0x1A pair uses — across the zero-init, a team-3 latch, and a
// short-body 0x04 that must NOT take the latch.
// [orig: NapiNPClientMsg_0x004 @0x425410 -> byte_A85B48 @0x425499 (24-byte tail read)]
bool run_team_latch_is_falsifiable() {
	const std::string client_scrk = "CLIENT-TEAM-LATCH-SCRK";
	const std::string server_scrk = "SERVER-TEAM-LATCH-SCRK";
	np::JoinerConnection joiner("TeamLatch");
	joiner.seed_in_match(0x33445566u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0001, w::kPlayerInfantryTypeId);

	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	auto resubmit_team = [&](uint8_t &team_out) {
		const std::vector<uint8_t> datagram = joiner.frame_loadout_resubmit();
		if (datagram.empty() ||
		    !decode_client_session(datagram, client_scrk, header, messages) ||
		    messages.size() != 1 || messages[0].tag != 0x2F || messages[0].payload.empty())
			return false;
		team_out = messages[0].payload[0];
		return true;
	};

	uint8_t team = 0xEE;
	if (!expect(resubmit_team(team) && team == 0x00,
			"pre-0x04 the wire team byte is retail's zero-init (byte_A85B48)")) {
		return false;
	}

	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> assign = frame_server_session(server_tx, server_scrk, 1u,
			{make_protocol_message(0x04, retail_slot_assignment(0x03))});
	(void)joiner.handle_datagram(assign.data(), assign.size());
	if (!expect(resubmit_team(team) && team == 0x03,
			"the 0x04 tail byte latches and drives the 0x2F team byte")) {
		return false;
	}

	std::vector<uint8_t> short_body(20, 0);
	short_body[19] = 0x04; // a would-be team in a body too short for the tail read
	const std::vector<uint8_t> short_assign = frame_server_session(server_tx, server_scrk, 1u,
			{make_protocol_message(0x04, std::move(short_body))});
	(void)joiner.handle_datagram(short_assign.data(), short_assign.size());
	return expect(resubmit_team(team) && team == 0x03,
			"a short (<24 B) 0x04 body does not take the latch");
}

// The padding echo must follow the retail clamp — body = max(12, min(padding_len, 512)) with
// the [x][y][net-frame-counter] prefix — and a tiny request must still echo AND advance the
// FSM (the pre-fix builder returned nothing below 8 bytes, silently wedging the join at
// AwaitPaddingProbe). [orig: NetPacket_WritePositionWithPadding @0x42A360; server consumer
// NapiNPServerMsg_0x002 @0x512fd0 reads dwords 0 and 2]
bool run_padding_echo_retail_clamp(uint32_t requested_len, std::size_t expected_body) {
	constexpr uint32_t kServerKey = 0x66778899u;
	np::JoinerConnection joiner("PadClamp");

	const std::vector<uint8_t> hello_datagram = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
				parse_client_hello(body.data(), body.size(), hello),
			"pad-clamp: decode ClientHello")) {
		return false;
	}
	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x44556677u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const np::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	if (!expect(hello_result.outbound.size() == 1, "pad-clamp: ClientAuth emitted")) {
		return false;
	}
	ClientAuth client_auth;
	if (!expect(nw_decode_inbound(
				hello_result.outbound[0].data(), hello_result.outbound[0].size(),
				opcode, body) &&
				parse_client_auth(body.data(), body.size(), client_auth),
			"pad-clamp: decode ClientAuth")) {
		return false;
	}
	const std::string server_scrk = "SERVER-PAD-CLAMP-SCRK";
	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, server_scrk, "", "", "", false);
	server_auth.mi = 6;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	(void)joiner.handle_datagram(server_auth_datagram.data(), server_auth_datagram.size());

	SessionSequencing server_seq = np::make_jo_game_session_sequencing();
	const std::vector<uint8_t> settings_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(
							0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00},
							0xA0),
					make_protocol_message(
							0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00},
							0xA0),
			});
	if (!expect(joiner.handle_datagram(
				settings_datagram.data(), settings_datagram.size()).outbound.size() == 2,
			"pad-clamp: settings emit ACK + JOIN")) {
		return false;
	}
	server_seq.last_inbound_seq = 2;
	const std::vector<uint8_t> join_ack_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x00, {})});
	if (!expect(joiner.handle_datagram(
				join_ack_datagram.data(), join_ack_datagram.size()).outbound.size() == 2,
			"pad-clamp: JOIN ack emits ACK + form post")) {
		return false;
	}

	std::vector<uint8_t> probe(512, 0xA5);
	auto write_u32 = [&](std::size_t offset, uint32_t value) {
		probe[offset + 0] = static_cast<uint8_t>(value);
		probe[offset + 1] = static_cast<uint8_t>(value >> 8);
		probe[offset + 2] = static_cast<uint8_t>(value >> 16);
		probe[offset + 3] = static_cast<uint8_t>(value >> 24);
	};
	write_u32(0, 0x11223344u);
	write_u32(4, 0x00000002u);
	write_u32(8, requested_len);
	server_seq.last_inbound_seq = 4;
	const std::vector<uint8_t> padding_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x02, std::move(probe))});
	ProtocolPacketHeader client_header;
	std::vector<ProtocolMessage> client_messages;
	const np::JoinerConnection::PollResult padding_result =
			joiner.handle_datagram(padding_datagram.data(), padding_datagram.size());
	if (!expect(padding_result.outbound.size() == 1 &&
				decode_client_session(
						padding_result.outbound[0], client_auth.scrk,
						client_header, client_messages) &&
				client_messages.size() == 1 &&
				client_messages[0].tag == 0x02 &&
				client_messages[0].payload.size() == expected_body &&
				client_messages[0].payload[0] == 0x44 &&
				client_messages[0].payload[1] == 0x33 &&
				client_messages[0].payload[2] == 0x22 &&
				client_messages[0].payload[3] == 0x11 &&
				client_messages[0].payload[4] == 0x02 &&
				// dword 2 = the client net-frame counter (no pump ran here -> 0),
				// the consumed stats sample — not random filler.
				client_messages[0].payload[8] == 0x00 &&
				client_messages[0].payload[9] == 0x00 &&
				client_messages[0].payload[10] == 0x00 &&
				client_messages[0].payload[11] == 0x00,
			"pad-clamp: echo body follows max(12, min(len, 512)) with the 3-dword prefix")) {
		return false;
	}

	// The stage must have advanced: the game-start flag now yields the grouped admission
	// packet (the old <8-byte reject left AwaitPaddingProbe wedged and this emitted nothing).
	server_seq.last_inbound_seq = 5;
	const std::vector<uint8_t> game_start_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(0x05, {0x01})});
	return expect(joiner.handle_datagram(
				game_start_datagram.data(), game_start_datagram.size()).outbound.size() == 1,
			"pad-clamp: the echo advanced the FSM to AwaitGameStart");
}

// The leave leg: an authenticated joiner's disconnect() is the witnessed 4-packet 0x46 burst,
// one-shot, and the host's goodbye handler tears the connection down on receipt.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0; Nwu_HandleClientGoodbye @0x624250]
bool run_joiner_goodbye_tears_down_host() {
	const PeerAddr peer{0x0100007Fu, 32771};
	np::NapiNPServerCtx host;
	np::test::bring_up_host(host, np::ConnectionMode::HostClient,
			np::SocketMode::Socketless, 0x0FE0E112u);
	np::ClientRuntime client("GoodbyeJoiner");
	if (!expect(client.disconnect().empty(),
			"goodbye: nothing to send before ServerAuth assigns the session key")) {
		return false;
	}
	const std::vector<uint8_t> hello = client.start();
	const np::HandleResult hello_result = np::handle_server_datagram(
			host, peer, hello.data(), hello.size(), 0);
	if (!expect(hello_result.outbound.size() == 1, "goodbye: host answers ServerHello")) {
		return false;
	}
	client.receive(hello_result.outbound[0].data(), hello_result.outbound[0].size());
	const std::vector<std::vector<uint8_t>> auth = client.Client_ProcessNetworkFrame(0);
	if (!expect(auth.size() == 1, "goodbye: client emits ClientAuth")) return false;
	const np::HandleResult auth_result = np::handle_server_datagram(
			host, peer, auth[0].data(), auth[0].size(), 0);
	if (!expect(!auth_result.outbound.empty() &&
				host.np_protocol.connection_list.size() == 1,
			"goodbye: host holds the authenticated connection")) {
		return false;
	}
	for (const std::vector<uint8_t> &reply : auth_result.outbound)
		client.receive(reply.data(), reply.size());
	(void)client.Client_ProcessNetworkFrame(0);

	const std::vector<std::vector<uint8_t>> burst = client.disconnect();
	if (!expect(burst.size() == 4, "goodbye: the leave is the 4-packet burst "
			"(cs_dir0.recv_max_per_tick)")) {
		return false;
	}
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!expect(nw_decode_inbound(burst[0].data(), burst[0].size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_GOODBYE,
			"goodbye: burst datagrams carry 0x46")) {
		return false;
	}
	for (std::size_t i = 1; i < burst.size(); ++i) {
		if (!expect(burst[i] == burst[0], "goodbye: burst datagrams are identical")) {
			return false;
		}
	}
	for (const std::vector<uint8_t> &dg : burst)
		(void)np::handle_server_datagram(host, peer, dg.data(), dg.size(), 1);
	if (!expect(host.np_protocol.connection_list.empty(),
			"goodbye: the host tears the connection down (idempotent across the burst)")) {
		return false;
	}
	return expect(client.disconnect().empty(), "goodbye: the burst is one-shot");
}

int main() {
	const bool ok = run_charattr_challenge_table_matches_retail() &&
	                run_seeded_objective_layout_hint() &&
	                run_fire_queue_stamps_runtime_tick() &&
	                run_retail_post_auth_prelude() &&
	                run_early_sync_tail_latch() &&
	                run_duplicate_s2c_session_one_shot_is_not_replayed() &&
	                run_roundtrip() &&
	                run_roundtrip_with_spawn_zones(/*player_paced=*/false) &&
	                run_roundtrip_with_spawn_zones(/*player_paced=*/true) &&
	                run_host_client_discards_authority_owned_reload_echoes() &&
	                run_host_zone_timer_value_matches_retail_entry() &&
	                run_zone_timer_channels_share_one_retail_entry() &&
	                run_zone_timer_uses_wrapping_dword_arithmetic_and_signed_clamps() &&
	                run_joiner_zone_timer_preserves_mixed_wire_order() &&
	                run_host_as_client() &&
	                run_host_startup_seeds_mounted_no_callback_carrier() &&
	                run_host_startup_maps_claymore_preference() &&
	                run_host_pump_hook_observes_remote_before_first_tick() &&
	                run_periodic_request_quartet_is_answered() &&
	                run_reverse_rtt_probe_is_echoed() &&
	                run_network_spawn_does_not_mutate_loaded_model_snapshot() &&
	                run_split_batch_keeps_deployment_pick_ack_causal() &&
	                run_live_frame_uses_wall_clock_and_batches_mount_requests() &&
	                run_same_packet_holdoff_defers_admission_replies() &&
	                run_holdoff_defers_retained_session_reconstruction() &&
	                run_settings_send_holdoff_blocks_exact_frame_count() &&
	                run_send_holdoff_defers_due_housekeeping() &&
	                run_start_resets_reusable_runtime_state() &&
	                run_joiner_correlates_handshake_echoes() &&
	                run_tick_seed_anchors_the_client_clock() &&
	                run_team_latch_is_falsifiable() &&
	                run_padding_echo_retail_clamp(/*requested_len=*/0, /*expected_body=*/12) &&
	                run_padding_echo_retail_clamp(/*requested_len=*/300, /*expected_body=*/300) &&
	                run_padding_echo_retail_clamp(/*requested_len=*/2000, /*expected_body=*/512) &&
	                run_joiner_goodbye_tears_down_host();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
