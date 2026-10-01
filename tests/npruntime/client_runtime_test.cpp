#include <runtime/world/radio_call.h>
// P5 — inmatch::ClientRuntime (the headless Client_ProcessNetworkFrame role), always-on:
//
//  (A) Full in-process round-trip — client_runtime <-> the REAL np server legs <-> Server_TickUpdate
//      + the production apply_in_match_c2s consumer + the ClientReplicaPipeline S2C fold:
//        handshake (0x41/0x42) via ClientRuntime.start()/receive()/Client_ProcessNetworkFrame()
//        -> the spawn-gate burst (driven from the per-frame client role) -> PeerSpawned
//        -> the owner binds the joiner connection's transport + streams a NAMED organic-spawn
//        -> client matches the owner ID + receives the deployment release -> InMatch (learns wire handle H)
//        -> each frame: ClientRuntime emits a framed C2S 0x0C -> handle_server_datagram surfaces
//           PeerC2SInMatch -> apply_in_match_c2s deliver_c2s's it onto the connection's transport
//           -> Server_TickUpdate drains+SNAPs the entity + fans an S2C 0x0A
//        -> the owner reframes that 0x0A as a 0x83 -> ClientRuntime folds it into ClientState.
//      Asserts the peer SNAPs to the uplink AND the client's ClientState reflects the server's 0x0A
//      (exactly one SNAP per 0x0C). This is the P5 e2e bar and the FIRST coverage of ClientReplicaPipeline
//      fold + the production PeerC2SInMatch consumer.
//
//  (B) Host-as-client (D-NET-121/122) — the SP listen-server host's OWN loopback view: Server_TickUpdate
//      fans the host loopback a per-frame 0x0A (is_in_match) anchored to the host player's owned_entity
//      (NOT the dvxi5 fallback), and a HostClient ClientRuntime folds it off the loopback. Guards that
//      the host's own local view is no longer starved and anchors correctly.

#include <runtime/inmatch/charattr_challenge.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/novaworld_link.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>

#include "host_test_setup.h"
#include "conn_fixture.h"
#include <runtime/inmatch/server_message_dispatch.h>

#include <runtime/replication/connection.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/session_ping.h>
#include <net/npwire/session_vars.h>
#include <net/novacrypto/crc32.h>

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
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
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

std::vector<uint8_t> make_organic_spawn(
		uint16_t slot_id, const std::string &name, int32_t x,
		int32_t y, int32_t z, int32_t orient, uint8_t team,
		uint16_t net_id, uint8_t anim_slot = 0, uint32_t owner = 0);

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

std::vector<uint8_t> encode_test_player_list(
		std::initializer_list<PlayerListEntry> players) {
	PlayerListFrame frame;
	frame.players.assign(players.begin(), players.end());
	frame.teams.resize(size_t(frame.team_count) + 1);
	frame.in_game_count = static_cast<uint8_t>(frame.players.size());
	return encode_player_list(frame);
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
// NapiNPClientMsg_SessionSlotConfig (0x04) @0x425410 -> byte_A85B48 @0x425499]
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

std::vector<uint8_t> zone_presence_body(uint16_t handle, uint8_t count) {
	return {
			static_cast<uint8_t>(handle),
			static_cast<uint8_t>(handle >> 8),
			count,
	};
}

bool matches_client_header(const ProtocolPacketHeader &header,
		uint32_t session_id, uint32_t sequence, uint32_t ack) {
	return header.session_id == session_id &&
			header.seq_num == sequence &&
			header.ack_count == ack &&
			header.connection_flags == 0;
}

bool build_joiner_client_auth(inmatch::JoinRole role,
		std::string password, ClientAuth &out,
		std::string server_password = {}, bool password_required = false) {
	inmatch::JoinerConnection joiner("SpectatorWire");
	joiner.set_join_request(role, std::move(password), std::move(server_password));
	const std::vector<uint8_t> hello_datagram = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) ||
			opcode != SESSION_OPCODE_CLIENT_HELLO ||
			!parse_client_hello(body.data(), body.size(), hello)) {
		return false;
	}
	ServerHello server_hello =
			build_server_hello(hello, 0x7F000001u, 32768);
	server_hello.hk = 0xAABBCCDDu;
	server_hello.sf = password_required ? 1u : 0u;
	const std::vector<uint8_t> reply = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO,
			server_hello_to_bytes(server_hello));
	const inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(reply.data(), reply.size());
	return result.outbound.size() == 1 &&
			nw_decode_inbound(
					result.outbound[0].data(), result.outbound[0].size(),
					opcode, body) &&
			opcode == SESSION_OPCODE_CLIENT_AUTH &&
			parse_client_auth(body.data(), body.size(), out);
}

bool auth_has_cu(const ClientAuth &auth,
		std::string_view wanted_name, std::string_view wanted_value) {
	for (const std::vector<uint8_t> &blob : auth.cu) {
		uint8_t type = 0;
		std::string name;
		std::string value;
		if (parse_client_cu_chunk(
					blob.data(), blob.size(), type, name, value) &&
				type == 2 && name == wanted_name && value == wanted_value) {
			return true;
		}
	}
	return false;
}

bool run_spectator_clientauth_and_state_latch() {
	ClientAuth protected_join;
	for (const auto role : {inmatch::JoinRole::Player, inmatch::JoinRole::Spectator}) {
		if (!expect(build_joiner_client_auth(role, "watch", protected_join, "Secret", true) &&
				protected_join.pw == "Secret", "SF=1 publishes the server PW for either role")) return false;
		if (!expect(build_joiner_client_auth(role, "watch", protected_join, "Secret", false) &&
				protected_join.pw.empty(), "unprotected servers receive no stored PW")) return false;
	}

	ClientAuth player;
	if (!expect(build_joiner_client_auth(
				inmatch::JoinRole::Player, "", player),
			"ordinary player ClientAuth builds")) {
		return false;
	}
	if (!expect(!auth_has_cu(player, "JSR", "1") &&
				!auth_has_cu(player, "JSPP", "watch"),
			"ordinary player ClientAuth remains byte-shape compatible: no spectator CUs")) {
		return false;
	}

	ClientAuth spectator;
	if (!expect(build_joiner_client_auth(
				inmatch::JoinRole::Spectator, "watch", spectator),
			"spectator ClientAuth builds")) {
		return false;
	}
	if (!expect(auth_has_cu(spectator, "JSR", "1") &&
				auth_has_cu(spectator, "JSPP", "watch"),
			"spectator ClientAuth carries retail JSR=1 and JSPP")) {
		return false;
	}

	const std::string client_scrk = "CLIENT-SPECTATOR-STATE-SCRK";
	const std::string server_scrk = "SERVER-SPECTATOR-STATE-SCRK";
	constexpr uint32_t kSession = 0x10203040u;
	constexpr uint32_t kClientKey = 0x50607080u;
	inmatch::ClientRuntime runtime("SpectatorState");
	runtime.seed_session(
			kSession, kClientKey, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> state = frame_server_session(
			server_seq, server_scrk, kClientKey,
			{make_protocol_message(0x75, {0x01, 0x00})});
	runtime.receive(state.data(), state.size());
	(void)runtime.Client_ProcessNetworkFrame(1);
	if (!expect(runtime.is_spectator(),
			"S2C 0x75 bit 0 latches spectator mode")) {
		return false;
	}
	state = frame_server_session(
			server_seq, server_scrk, kClientKey,
			{make_protocol_message(0x75, {0x00, 0x02})});
	runtime.receive(state.data(), state.size());
	(void)runtime.Client_ProcessNetworkFrame(2);
	return expect(!runtime.is_spectator(),
			"cleared S2C 0x75 bit returns the client to player mode");
}

bool run_seeded_objective_layout_hint() {
	inmatch::ClientRuntime client("Replay");
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

// The runtime surfaces the joiner's anti-cheat challenge counters for the live
// join diagnostics: a 0x30 with no integrity profile counts as seen-not-answered
// (the deliberate-silence policy, D-NET-181), and a non-joiner runtime reports
// zeroed defaults.
bool run_challenge_diagnostics_pass_through_the_runtime() {
	const std::string client_scrk = "CLIENT-CHALLENGE-DIAG-SCRK";
	const std::string server_scrk = "SERVER-CHALLENGE-DIAG-SCRK";
	constexpr uint32_t kChallengeClientKey = 0x0BADCAFEu;
	inmatch::ClientRuntime runtime("ChallengeDiag");
	runtime.seed_session(
			0x11223344u, kChallengeClientKey, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> challenge = frame_server_session(
			server_seq, server_scrk, kChallengeClientKey,
			{make_protocol_message(0x30, {0xFF, 0x00, 0x00})});
	runtime.receive(challenge.data(), challenge.size());
	(void)runtime.Client_ProcessNetworkFrame(1);
	const inmatch::JoinerConnection::ChallengeDiagnostics challenges =
			runtime.challenge_diagnostics();
	if (!expect(challenges.entity_checksum_seen == 1 &&
	                    challenges.entity_checksum_answered == 0,
	            "the runtime surfaces the joiner's seen-but-silent 0x30 counter"))
		return false;
	if (!expect(!runtime.last_join_reject().set && !runtime.has_disconnect_event(),
	            "a healthy session carries no reject or disconnect record"))
		return false;
	inmatch::ClientRuntime unseeded("ChallengeDiagDefaults");
	return expect(unseeded.challenge_diagnostics().entity_checksum_seen == 0 &&
	                      !unseeded.last_join_reject().set &&
	                      !unseeded.has_disconnect_event(),
	              "a runtime without a joiner session reports zeroed diagnostics");
}

bool run_fire_queue_stamps_runtime_tick() {
	const std::string client_scrk = "CLIENT-FIRE-TICK-SCRK";
	const std::string server_scrk = "SERVER-FIRE-TICK-SCRK";
	constexpr uint16_t self_handle = 0x0002;
	// The network-role clock is ANCHORED to the host's tick seed, never free-run from
	// zero: a replay seeds it the way the capture's S2C 0x61 did.
	constexpr uint32_t kTickSeed = 0x00110000u;
	inmatch::ClientRuntime client("Shooter");
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
	inmatch::ClientRuntime unseeded("Unseeded");
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
	inmatch::JoinerConnection joiner("TickSeed", [] { return uint64_t(0); });
	joiner.seed_in_match(0x44556677u, 1u, client_scrk, server_scrk,
	                     1, 0, self_handle, 0x14B9);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> seed_dg = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x61, {0x00, 0x00, 0x11, 0x00})});
	const inmatch::JoinerConnection::PollResult seeded =
			joiner.handle_datagram(seed_dg.data(), seed_dg.size());
	if (!expect(seeded.tick_seed_set && seeded.tick_seed == kSeed,
			"S2C 0x61 surfaces the per-player tick seed")) {
		return false;
	}

	// (c) A SHORT body seeds zero, not garbage, and the four-zero-byte disarm form is a
	// real seed that parks the clock again.
	const std::vector<uint8_t> short_dg = frame_server_session(
			server_tx, server_scrk, 1u, {make_protocol_message(0x61, {0x01, 0x02})});
	const inmatch::JoinerConnection::PollResult short_seed =
			joiner.handle_datagram(short_dg.data(), short_dg.size());
	if (!expect(short_seed.tick_seed_set && short_seed.tick_seed == 0u,
			"a short 0x61 body seeds zero rather than reading past the payload")) {
		return false;
	}
	const std::vector<uint8_t> disarm_dg = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x61, {0x00, 0x00, 0x00, 0x00})});
	const inmatch::JoinerConnection::PollResult disarm =
			joiner.handle_datagram(disarm_dg.data(), disarm_dg.size());
	return expect(disarm.tick_seed_set && disarm.tick_seed == 0u,
			"the round-end disarm form is a witnessed seed of zero");
}

bool run_end_round_header_pulls_complete_board() {
	const std::string client_scrk = "CLIENT-END-ROUND-SCRK";
	const std::string server_scrk = "SERVER-END-ROUND-SCRK";
	inmatch::JoinerConnection joiner("RoundPull");
	// A team game type: the 0x1D form is the session-state pick, so the
	// joiner must know g_GameType before the header arrives.
	joiner.seed_in_match(0x10203040u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId, 0x10000u);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	auto replica_owned = std::make_unique<ns::ClientReplicaPipeline>();
	ns::ClientReplicaPipeline &replica = *replica_owned;
	replica.set_game_type(0x10000u);
	replica.set_mp_session(true);

	EndRoundHeader header;
	header.winner_team = 2;
	header.team_score_0 = 3;
	header.team_score_1 = 8;
	header.player_index = 4;
	const std::vector<uint8_t> header_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(s2c::END_ROUND_HEADER,
					encode_end_round_header(header, /*non_team_form=*/false))});
	const inmatch::JoinerConnection::PollResult header_result =
			joiner.handle_datagram(header_datagram.data(), header_datagram.size());
	if (!expect(header_result.queued_send_messages.size() == 1 &&
			header_result.queued_send_messages[0].tag ==
					c2s::END_ROUND_STATS_REQUEST &&
			header_result.queued_send_messages[0].payload ==
					std::vector<uint8_t>({0, 0}),
			"S2C 0x1D immediately queues reliable C2S 0x2B offset zero")) {
		return false;
	}
	for (const auto &message : header_result.inbound_reducer)
		replica.apply(message.first, message.second);
	if (!expect(replica.state().end_round.header_known &&
			replica.state().end_round.header.player_index == 4,
			"the canonical client reducer retains the recipient 0x1D header")) {
		return false;
	}

	EndRoundStats board;
	board.winner_team = 2;
	board.team_score_0 = 3;
	board.team_score_1 = 8;
	for (uint8_t slot = 0; slot < 12; ++slot) {
		EndRoundPlayerRow row;
		row.slot = slot;
		row.name = "RetailPeer" + std::to_string(slot);
		row.team = static_cast<uint8_t>((slot & 1u) + 1u);
		row.kills = slot;
		board.players.push_back(std::move(row));
	}
	board.team_rows.resize(3);
	const std::vector<uint8_t> board_wire = encode_end_round_stats(board);
	if (!expect(board_wire.size() > 200 && board_wire.size() < 400,
			"end-round pull fixture crosses exactly one 200-byte boundary")) {
		return false;
	}
	const std::vector<uint8_t> first_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(s2c::END_ROUND_STATS,
					encode_end_round_stats_chunk(board_wire, 0))});
	const inmatch::JoinerConnection::PollResult first =
			joiner.handle_datagram(first_datagram.data(), first_datagram.size());
	if (!expect(first.queued_send_messages.size() == 1 &&
			first.queued_send_messages[0].tag ==
					c2s::END_ROUND_STATS_REQUEST &&
			first.queued_send_messages[0].payload ==
					std::vector<uint8_t>({200, 0}),
			"incomplete S2C 0x56 requests the next running offset")) {
		return false;
	}
	for (const auto &message : first.inbound_reducer)
		replica.apply(message.first, message.second);
	if (!expect(!replica.state().end_round.known,
			"the first 200-byte chunk does not publish a partial board")) {
		return false;
	}

	const std::vector<uint8_t> final_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(s2c::END_ROUND_STATS,
					encode_end_round_stats_chunk(board_wire, 200))});
	const inmatch::JoinerConnection::PollResult final =
			joiner.handle_datagram(final_datagram.data(), final_datagram.size());
	if (!expect(final.queued_send_messages.empty(),
			"the completing S2C 0x56 queues no further pull")) {
		return false;
	}
	for (const auto &message : final.inbound_reducer)
		replica.apply(message.first, message.second);
	return expect(replica.state().end_round.known &&
			replica.state().end_round.board.players.size() == 12 &&
			replica.state().end_round.board.players[11].name == "RetailPeer11",
			"the requested chunks publish the complete retail board");
}

// The DM/KOTH-family 0x1D: a non-team session decodes the named form (three
// top-row names + i16 primary scores) with the same 0x2B kick and reducer
// retention. [orig: NapiNPClientMsg_0x01D form pick @0x43086c..0x430883,
// named parse @0x430889..0x4309af]
bool run_end_round_named_header_kicks_and_folds() {
	const std::string client_scrk = "CLIENT-END-ROUND-DM-SCRK";
	const std::string server_scrk = "SERVER-END-ROUND-DM-SCRK";
	inmatch::JoinerConnection joiner("RoundPullDM");
	joiner.seed_in_match(0x10203041u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId, /*game_type=*/0u);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	ns::ClientReplicaPipeline replica;
	replica.set_game_type(0u);
	replica.set_mp_session(true);

	EndRoundHeader header;
	header.player_names[0] = "Ace";
	header.player_names[1] = "Bee";
	header.player_scores[0] = 12;
	header.player_scores[1] = -3;
	header.player_index = 1;
	const std::vector<uint8_t> header_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(s2c::END_ROUND_HEADER,
					encode_end_round_header(header, /*non_team_form=*/true))});
	const inmatch::JoinerConnection::PollResult header_result =
			joiner.handle_datagram(header_datagram.data(), header_datagram.size());
	if (!expect(header_result.queued_send_messages.size() == 1 &&
			header_result.queued_send_messages[0].tag ==
					c2s::END_ROUND_STATS_REQUEST &&
			header_result.queued_send_messages[0].payload ==
					std::vector<uint8_t>({0, 0}),
			"the named 0x1D form still queues the C2S 0x2B offset-zero kick")) {
		return false;
	}
	for (const auto &message : header_result.inbound_reducer)
		replica.apply(message.first, message.second);
	const ns::ClientEndRoundStats &er = replica.state().end_round;
	return expect(er.header_known &&
			er.header.player_names[0] == "Ace" &&
			er.header.player_names[1] == "Bee" &&
			er.header.player_names[2].empty() &&
			er.header.player_scores[0] == 12 &&
			er.header.player_scores[1] == -3 &&
			er.header.player_index == 1,
			"the reducer retains the named header's rows and index");
}

// A 0x16 row for a connection slot the roster has not bound yet is dropped by
// the reducer AND re-requested on the wire: one reliable C2S 0x22 {slot,
// 0x1CF7} per dropped row, leaving through the session framing at the next
// send boundary; a bound slot's row folds and asks for nothing
// [orig: NapiNPClientMsg_PlayerList @0x42fc05..0x42fc3a ->
//  CNapiNetwork_QueueReliableMessage(ctx, 0x22, 1, 0, {slot, 0xF7, 0x1C}, 3)].
bool run_unknown_scoreboard_row_requests_player_sync() {
	const std::string client_scrk = "CLIENT-SYNC-RETRY-SCRK";
	const std::string server_scrk = "SERVER-SYNC-RETRY-SCRK";
	inmatch::ClientRuntime client("SyncRetryRuntime");
	client.seed_session(0x63748596u, 1u, client_scrk, server_scrk,
	                    1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	PlayerReplicationState known;
	known.player_slot = 3;
	known.player_name = "Known";
	const std::vector<uint8_t> datagram = frame_server_session(
			server_tx, server_scrk, 1u, {
					make_protocol_message(
							0x46, encode_player_sync(known, kPlayerSyncHasName)),
					make_protocol_message(
							0x16, encode_test_player_list({{3, 1}, {9, 2}})),
			});
	client.receive(datagram.data(), datagram.size());
	std::vector<std::vector<uint8_t>> sync_requests;
	for (const std::vector<uint8_t> &out : client.Client_ProcessNetworkFrame(1)) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(out, client_scrk, header, messages)) continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == c2s::PLAYER_SYNC_REQUEST) sync_requests.push_back(m.payload);
	}
	const ns::ClientScoreboard &board = client.state().scoreboard;
	if (!expect(board.rows.size() == 1 && board.rows[0].slot_id == 3 &&
	                    board.rows_dropped_unknown_slot == 1 &&
	                    board.pending_sync_requests.empty(),
	            "the bound slot's row folds, the unknown slot's row drops, the retry queue drains"))
		return false;
	std::size_t for_unknown = 0;
	std::size_t for_known = 0;
	for (const std::vector<uint8_t> &payload : sync_requests) {
		if (payload == std::vector<uint8_t>({9, 0xF7, 0x1C})) ++for_unknown;
		if (!payload.empty() && payload[0] == 3) ++for_known;
	}
	return expect(for_unknown == 1 && for_known == 0,
	              "exactly one C2S 0x22 {slot, 0x1CF7} re-request for the dropped row, none for the bound slot");
}

// Retail S2C 0x76 replaces the client-global class availability word. It is
// not a clock: a short body explicitly clears the word to zero.
// [orig: NapiNPClientMsg_HandleClassAllowMask @0x42d540]
bool run_class_allow_mask_follows_retail_host() {
	const std::string client_scrk = "CLIENT-CLASS-MASK-SCRK";
	const std::string server_scrk = "SERVER-CLASS-MASK-SCRK";
	inmatch::JoinerConnection joiner("ClassMask");
	joiner.seed_in_match(0x52637485u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	if (!expect(joiner.class_allow_mask() == 0x03FFu,
	            "joiner starts with retail's all-ten-classes default")) {
		return false;
	}

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> configured = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x76, {0x55, 0x01})});
	(void)joiner.handle_datagram(configured.data(), configured.size());
	if (!expect(joiner.class_allow_mask() == 0x0155u,
	            "S2C 0x76 installs the retail host's configured u16 mask")) {
		return false;
	}

	const std::vector<uint8_t> short_body = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x76, {0xAA})});
	(void)joiner.handle_datagram(short_body.data(), short_body.size());
	if (!expect(joiner.class_allow_mask() == 0u,
	            "a short S2C 0x76 clears the class mask like retail")) {
		return false;
	}

	// The embedding runtime must expose the same receive-side state to its UI
	// adapter, not merely consume it inside JoinerConnection.
	inmatch::ClientRuntime client("ClassMaskRuntime");
	client.seed_session(0x63748596u, 1u, client_scrk, server_scrk,
	                    1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing runtime_server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> runtime_mask = frame_server_session(
			runtime_server_tx, server_scrk, 1u,
			{make_protocol_message(0x76, {0xAA, 0x02})});
	client.receive(runtime_mask.data(), runtime_mask.size());
	(void)client.Client_ProcessNetworkFrame(1);
	return expect(client.class_allow_mask() == 0x02AAu,
	              "ClientRuntime exposes the retail host's received class mask");
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

// The two NovaWorld-only join tokens on the wire, decoded back off the framed
// packets: the ClientAuth carries the .joi CK decimal as the APPID CU right
// after COUNTRYCODE (a stock host reads the APPID tag into net_cfg.bt and punts
// code 9 on a mismatch), and the 0x00 JOIN carries the CD identity cookie as a
// binary TLV after VERSIONCRCSTRING (codes 23/24/25/28). A LAN joiner (no
// APPID, no cookie) emits neither. [orig: NapiNetConfig_LoadFromConnTags
// @0x4c7260; NapiNP_WriteClientAuthPayload @0x42a180;
// Server_ValidatePlayerJoinRequest @0x512100 @0x5122c5]
bool run_novaworld_join_tokens_ride_the_wire() {
	constexpr uint32_t kServerKey = 0x11223344u;
	const std::string server_scrk = "SERVER-JOIN-TOKENS-SCRK";
	const std::vector<uint8_t> cookie = {'P', 'U', 'B', '1', 0, 'v', 'a', 'l', 0};

	auto hello_to_client_auth = [&](inmatch::JoinerConnection &joiner,
	                                ServerHello &server_hello,
	                                ClientAuth &client_auth) -> bool {
		const std::vector<uint8_t> hello_datagram = joiner.start();
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ClientHello hello;
		if (!expect(nw_decode_inbound(hello_datagram.data(), hello_datagram.size(),
		                              opcode, body) &&
		                    opcode == SESSION_OPCODE_CLIENT_HELLO &&
		                    parse_client_hello(body.data(), body.size(), hello),
		            "join-tokens: decode ClientHello")) {
			return false;
		}
		server_hello = build_server_hello(hello, 0x7F000001u, 32769);
		server_hello.hk = 0x55667788u;
		server_hello.sus2 = "revx02";
		const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
				SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
		const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
				server_hello_datagram.data(), server_hello_datagram.size());
		if (!expect(hello_result.outbound.size() == 1,
		            "join-tokens: ServerHello emits one ClientAuth")) {
			return false;
		}
		return expect(nw_decode_inbound(hello_result.outbound[0].data(),
		                                hello_result.outbound[0].size(), opcode, body) &&
		                      opcode == SESSION_OPCODE_CLIENT_AUTH &&
		                      parse_client_auth(body.data(), body.size(), client_auth),
		              "join-tokens: decode ClientAuth");
	};
	// The CU chunks as (name, value) in wire order.
	auto cu_fields = [](const ClientAuth &auth) {
		std::vector<std::pair<std::string, std::string>> out;
		for (const std::vector<uint8_t> &chunk : auth.cu) {
			uint8_t type = 0;
			std::string name, value;
			if (parse_client_cu_chunk(chunk.data(), chunk.size(), type, name, value))
				out.emplace_back(name, value);
		}
		return out;
	};
	auto index_of = [](const std::vector<std::pair<std::string, std::string>> &cu,
	                   const char *name) -> int {
		for (size_t i = 0; i < cu.size(); ++i)
			if (cu[i].first == name) return static_cast<int>(i);
		return -1;
	};

	// --- the NovaWorld joiner: APPID after COUNTRYCODE, CD after VERSIONCRCSTRING
	inmatch::JoinerConnection joiner("JoinTokens");
	joiner.set_join_request(inmatch::JoinRole::Player, "", "", "SideSecret");
	inmatch::CharacterJoinVars side_profile;
	side_profile.char_id[0] = 0x2101;
	side_profile.team_request = 1;
	joiner.set_character_join_vars(side_profile);
	joiner.set_app_id("3225");
	joiner.set_cd_cookie(cookie);
	ServerHello server_hello;
	ClientAuth client_auth;
	if (!hello_to_client_auth(joiner, server_hello, client_auth)) return false;
	const auto cu = cu_fields(client_auth);
	const int jsp_at = index_of(cu, "JSP");
	if (!expect(jsp_at >= 0 && cu[static_cast<size_t>(jsp_at)].second == "SideSecret",
			"side/squad credential rides JSP in ClientAuth")) return false;
	if (!expect(jsp_at < index_of(cu, "CI0"), "JSP precedes the profile character tags"))
		return false;
	const int appid_at = index_of(cu, "APPID");
	const int country_at = index_of(cu, "COUNTRYCODE");
	const int bt_at = index_of(cu, "BT");
	if (!expect(appid_at >= 0 && cu[appid_at].second == "3225",
	            "the ClientAuth carries CU APPID = the .joi CK decimal")) {
		return false;
	}
	if (!expect(country_at >= 0 && appid_at == country_at + 1,
	            "APPID rides right after COUNTRYCODE")) {
		return false;
	}
	if (!expect(bt_at >= 0 && cu[bt_at].second == "0",
	            "BT stays the LAN default 0 (the ban-type gate, not the token)")) {
		return false;
	}

	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, server_scrk, "", "", "", false);
	server_auth.mi = 3;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	(void)joiner.handle_datagram(server_auth_datagram.data(), server_auth_datagram.size());
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
	const std::vector<ProtocolMessage> initial_settings = {
			make_protocol_message(
					0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
			make_protocol_message(
					0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
	};
	const std::vector<uint8_t> settings_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck, initial_settings);
	const inmatch::JoinerConnection::PollResult settings_result =
			joiner.handle_datagram(settings_datagram.data(), settings_datagram.size());
	if (!expect(settings_result.outbound.size() == 2,
	            "join-tokens: initial settings emit ACK then JOIN")) {
		return false;
	}
	ProtocolPacketHeader client_header;
	std::vector<ProtocolMessage> client_messages;
	if (!expect(decode_client_session(settings_result.outbound[1], client_auth.scrk,
	                                  client_header, client_messages) &&
	                    client_messages.size() == 1 && client_messages[0].tag == 0x00,
	            "join-tokens: decode the JOIN")) {
		return false;
	}
	std::vector<uint8_t> expected_join = retail_expansion_join_request(server_hello.sus2);
	const uint8_t cd_tlv_head[] = {'C', 'D', 0x00, static_cast<uint8_t>(cookie.size()), 0x00};
	expected_join.insert(expected_join.end(), std::begin(cd_tlv_head), std::end(cd_tlv_head));
	expected_join.insert(expected_join.end(), cookie.begin(), cookie.end());
	if (!expect(client_messages[0].payload == expected_join,
	            "the JOIN carries EXP, VERSIONCRCSTRING, then the CD cookie as a binary "
	            "TLV (raw size, no appended NUL)")) {
		return false;
	}

	// --- the LAN joiner: no APPID chunk
	inmatch::JoinerConnection lan("JoinTokensLan");
	ServerHello lan_hello;
	ClientAuth lan_auth;
	if (!hello_to_client_auth(lan, lan_hello, lan_auth)) return false;
	const auto lan_cu = cu_fields(lan_auth);
	if (!expect(index_of(lan_cu, "JSP") < 0, "empty join credentials omit JSP")) return false;
	if (!expect(index_of(lan_cu, "APPID") < 0, "a LAN joiner sends no APPID")) return false;
	return expect(index_of(lan_cu, "COUNTRYCODE") >= 0,
	              "the LAN ClientAuth keeps its fixed fields through COUNTRYCODE");
}

bool run_retail_post_auth_prelude() {
	constexpr uint32_t kServerKey = 0x11223344u;
	constexpr uint32_t kConnectionId = 3;
	const std::string server_scrk = "SERVER-RETAIL-PRELUDE-SCRK";
	inmatch::JoinerConnection joiner("RetailPrelude");

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
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
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
	const inmatch::JoinerConnection::PollResult auth_result = joiner.handle_datagram(
			server_auth_datagram.data(), server_auth_datagram.size());
	if (!expect(auth_result.outbound.empty(),
			"ServerAuth waits for retail's initial sequenced settings packet")) {
		return false;
	}

	// Golden retail frame 6: two settings-update records in S2C sequence 1.
	// The client acknowledges that packet with an empty sequence 1, then sends
	// the exact JOIN request in sequence 2.
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
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
	const inmatch::JoinerConnection::PollResult settings_result =
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
	const inmatch::JoinerConnection::PollResult join_ack_result =
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
	const inmatch::JoinerConnection::PollResult padding_result =
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
	const inmatch::JoinerConnection::PollResult metadata_result =
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
	const inmatch::JoinerConnection::PollResult game_start_result =
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
	const inmatch::JoinerConnection::PollResult server_info_result =
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
	constexpr uint32_t kMissionMpAttributes = 0x80003A06u;
	std::vector<uint8_t> mission_data_chunk =
			retail_transfer_chunk(1, 180, 0x5A);
	for (int shift = 0; shift < 32; shift += 8) {
		mission_data_chunk[12 + 44 + static_cast<std::size_t>(shift / 8)] =
				static_cast<uint8_t>(kMissionMpAttributes >> shift);
	}
	constexpr uint32_t kMissionMaxPlayers = 24u; // the fixed block's dword @36
	for (int shift = 0; shift < 32; shift += 8) {
		mission_data_chunk[12 + 36 + static_cast<std::size_t>(shift / 8)] =
				static_cast<uint8_t>(kMissionMaxPlayers >> shift);
	}
	const std::vector<uint8_t> mission_data_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(0x75, {0x00, 0x02}),
					make_protocol_message(
							0x64, std::move(mission_data_chunk)),
			});
	const inmatch::JoinerConnection::PollResult mission_data_result =
			joiner.handle_datagram(
					mission_data_datagram.data(), mission_data_datagram.size());
	if (!expect(mission_data_result.outbound.empty() &&
			joiner.mp_attributes() == kMissionMpAttributes &&
			joiner.session_max_players() == kMissionMaxPlayers,
			"final mission-data chunk waits for the player-list boundary")) {
		return false;
	}

	const std::vector<uint8_t> player_list_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(
					0x16, encode_test_player_list({{0, 1}, {1, 2}}))});
	const inmatch::JoinerConnection::PollResult player_list_result =
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
	const inmatch::JoinerConnection::PollResult sync_tail_result =
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
	const inmatch::JoinerConnection::PollResult world_end_result =
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
							0x10000, 0x20000, 0x30000, 0, 1, 7, 0, kConnectionId))});
	const inmatch::JoinerConnection::PollResult self_spawn_result =
			joiner.handle_datagram(
					self_spawn_datagram.data(), self_spawn_datagram.size());
	if (!expect(self_spawn_result.outbound.empty() &&
				joiner.has_self_handle() &&
				joiner.self_handle() == kRetailSelfHandle &&
				!joiner.in_match(),
			"self owner-ID match remains hidden before the retail deployment release")) {
		return false;
	}

	// Every retail 0x5A apply clears dword_81474C immediately. Split the first
	// grant from both the second grant and 0x0F policy to prove that gameplay
	// release is independent from the later deploy-UI decision.
	const std::vector<uint8_t> first_grant_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{make_protocol_message(
					0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF})});
	const inmatch::JoinerConnection::PollResult first_grant_result =
			joiner.handle_datagram(
					first_grant_datagram.data(), first_grant_datagram.size());
	if (!expect(first_grant_result.outbound.empty() &&
				first_grant_result.gameplay_release_applied &&
				first_grant_result.reached_in_match && joiner.in_match() &&
				!joiner.deployment_pick_pending() &&
				!joiner.initial_admission_complete(),
			"the first loadout grant opens gameplay without completing deploy UI readiness")) {
		return false;
	}

	// Retail's world-state handler carries two witnessed clocks into its completion
	// burst: S2C 0x19 supplies the first C2S 0x28 dword, while S2C 0x0F's
	// session tick supplies the second. The rest of the burst is fixed and ordered
	// (NapiNPClientMsg_0x00F @0x42e5bd..0x42e6ab).
	constexpr uint32_t kSpawnAckTimestamp = 0x10203040u;
	constexpr uint32_t kWorldStateTick = 0x50607080u;
	std::vector<uint8_t> zoned_world_state(23, 0);
	zoned_world_state[0] = 0x80;
	zoned_world_state[1] = 0x70;
	zoned_world_state[2] = 0x60;
	zoned_world_state[3] = 0x50;
	zoned_world_state[22] = 0x01;
	const std::vector<uint8_t> deployment_policy_datagram = frame_server_session(
			server_seq, server_scrk, client_auth.ck,
			{
					make_protocol_message(0x19, {0x40, 0x30, 0x20, 0x10}),
					make_protocol_message(0x0F, std::move(zoned_world_state)),
			});
	const inmatch::JoinerConnection::PollResult granted_loadout_result =
			joiner.handle_datagram(
					deployment_policy_datagram.data(),
					deployment_policy_datagram.size());
	const std::vector<uint8_t> expected_loadout_request = {
			0x40, 0x30, 0x20, 0x10,
			0x80, 0x70, 0x60, 0x50,
			0x00, 0x00,
	};
	const auto &completion = granted_loadout_result.queued_send_messages;
	if (!expect(granted_loadout_result.outbound.empty() &&
				completion.size() == 6 &&
				completion[0].tag == 0x28 &&
				completion[0].payload == expected_loadout_request &&
				completion[1].tag == 0x29 &&
				completion[1].payload == std::vector<uint8_t>({0x00, 0x00}) &&
				completion[2].tag == 0x2D && completion[2].payload.empty() &&
				completion[3].tag == 0x32 && completion[3].payload.empty() &&
				completion[4].tag == 0x22 &&
				completion[4].payload == std::vector<uint8_t>({0x00, 0xF7, 0x5C}) &&
				completion[5].tag == 0x23 && completion[5].payload.empty() &&
				joiner.in_match() && !joiner.deployment_pick_pending() &&
				!joiner.initial_admission_complete(),
			"world-state completion preserves the first-grant gameplay release "
			"without making the deploy UI ready before both grants")) {
		return false;
	}

	// The second grant completes admission without forcing a pick. An explicit
	// selection on the host-driven overlay remains a valid C2S 0x0E afterward.
	server_seq.last_inbound_seq = 13;
	const std::vector<uint8_t> split_second_grant_datagram =
			frame_server_session(
					server_seq, server_scrk, client_auth.ck,
					{make_protocol_message(
							0x5A,
							{0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF})});
	const inmatch::JoinerConnection::PollResult split_second_grant_result =
			joiner.handle_datagram(
					split_second_grant_datagram.data(),
					split_second_grant_datagram.size());
	if (!expect(split_second_grant_result.outbound.empty() &&
				split_second_grant_result.gameplay_release_applied &&
				joiner.in_match() && !joiner.deployment_pick_pending() &&
				joiner.initial_admission_complete() &&
				joiner.self_handle() == kRetailSelfHandle,
			"the second grant completes admission and deploys the player, no pick owed")) {
		return false;
	}
	if (!expect(!joiner.frame_deployment_pick(0xFFFFu).empty() &&
				joiner.deployment_pick_pending(),
			"the initial overlay selection waits for the host's spawn release")) {
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
	inmatch::JoinerConnection joiner("EarlyTail");

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
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
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
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
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
			{make_protocol_message(0x16, encode_test_player_list({{0, 1}, {1, 2}}))});
	const inmatch::JoinerConnection::PollResult player_list_result = joiner.handle_datagram(
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
	const inmatch::JoinerConnection::PollResult world_end_result = joiner.handle_datagram(
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
	inmatch::JoinerConnection joiner("Replay");
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

	inmatch::JoinerConnection::PollResult initial =
			joiner.handle_datagram(first.data(), first.size());
	if (!expect(initial.inbound_gameplay.size() == 1 &&
	                    initial.inbound_gameplay[0].first == 0x49,
	            "first S2C 0x83 surfaces its one-shot reload event"))
		return false;
	inmatch::JoinerConnection::PollResult duplicate =
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

bool run_client_reducer_preserves_packet_message_order() {
	const std::string client_scrk = "CLIENT-REDUCER-ORDER";
	const std::string server_scrk = "SERVER-REDUCER-ORDER";
	inmatch::JoinerConnection joiner("ReducerOrder");
	joiner.seed_in_match(0x10203040u, 1u, client_scrk, server_scrk,
			1, 0, 0x0001, w::kPlayerInfantryTypeId);

	WeaponReload reload;
	reload.entity_handle = 0x0001;
	reload.reload_param = 3;
	FrameUpdate frame;
	frame.mount_handle = 0xFFFF;
	frame.health = 100;
	// The Tab-board lanes, the feed lane, and the guided lane ride the same
	// canonical stream (D-HUD-23/D-HUD-24/D-NET-64): 0x16 and 0x46 fold beyond
	// their connection-level bookkeeping, and 0x1E/0x44 fold at all (before
	// these pins, only the host's loopback view — which applies every tag —
	// ever saw those bodies).
	PlayerReplicationState sync_rep;
	sync_rep.player_slot = 3;
	sync_rep.player_name = "ReducerName";
	const std::vector<uint8_t> game_event{6, 0x01, 0x02, 0xFF, 0, 0, 0, 0};
	// 0x44 sub-header [u16 shooter][i16 netId][u8 subtype] — the 5-byte
	// minimum the decoder accepts (§5.36).
	const std::vector<uint8_t> entity_routed{0x02, 0x00, 0x05, 0x00, 0x01};
	const std::vector<uint8_t> spawn_wave{
			1, 0x01, 0x20, 0x03, 0x00, 1, 0x0A, 0x00, 0x01, 0x00};
	const std::vector<uint8_t> score_feedback{5, 0, 0, 0};
	SessionSequencing server_tx{1, 0};
	const std::vector<uint8_t> datagram = frame_server_session(
			server_tx, server_scrk, 1u, {
					make_protocol_message(0x49, encode_weapon_reload(reload)),
					make_protocol_message(0x0A, encode_frame_update(frame)),
					make_protocol_message(0x16, encode_test_player_list({{3, 1}})),
					make_protocol_message(
							0x46, encode_player_sync(sync_rep, kPlayerSyncHasName)),
					make_protocol_message(0x1E, game_event),
					make_protocol_message(0x44, entity_routed),
					make_protocol_message(0x6E, spawn_wave),
					make_protocol_message(0x81, score_feedback),
					make_protocol_message(0x40, {0}),
					make_protocol_message(0x6B, {0}),
			});
	if (!expect(!datagram.empty(), "frame mixed reducer-order packet"))
		return false;
	const inmatch::JoinerConnection::PollResult poll =
			joiner.handle_datagram(datagram.data(), datagram.size());
	const std::array<uint8_t, 10> expected{
			{0x49, 0x0A, 0x16, 0x46, 0x1E, 0x44, 0x6E, 0x81, 0x40, 0x6B}};
	if (!expect(poll.inbound_reducer.size() == expected.size(),
			"every validated reducer message enters the canonical stream"))
		return false;
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (!expect(poll.inbound_reducer[i].first == expected[i],
				"canonical reducer stream preserves inner packet order"))
			return false;
	}
	return expect(poll.inbound_0a.size() == 1 &&
				poll.inbound_gameplay.size() == 9,
			"legacy family vectors remain diagnostic views of the same packet");
}

using opennova::inmatch::NullDatagramSocket;

struct HostPumpHookProbe {
	inmatch::HostOwner *owner = nullptr;
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
	for (inmatch::NapiNPConnection &conn : probe.owner->ctx.np_protocol.connection_list) {
		if (!(conn.peer == probe.peer)) continue;
		probe.spawned = conn.link.owned_entity;
		probe.saw_spawned_connection =
				conn.burst.spawned && conn.phase == inmatch::ConnectionPhase::Spawned;
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

// A 1-record S2C 0x0C organic-spawn body the joiner self-identifies from by its owner
// connection ID (the owner's PeerSpawned reaction, mirroring Simulation).
std::vector<uint8_t> make_organic_spawn(uint16_t slot_id, const std::string &name, int32_t x,
                                        int32_t y, int32_t z, int32_t orient, uint8_t team,
                                        uint16_t net_id, uint8_t anim_slot, uint32_t owner) {
	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord rec;
	rec.slot_id = slot_id;
	rec.has_body = true;
	rec.item_type_id = 0x14B9;
	rec.owner_connection_id = owner;
	rec.minimap_flags = owner ? 0x100 : 0;
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

	inmatch::NapiNPServerCtx ctx;
	// HostClient listen host (mode 3). local_client = nullptr: no host loopback connection in this
	// run — the only connection is the joiner, keeping the round-trip focused (the host loopback path
	// is run (B)). The host advertises this HK; ClientRuntime echoes it so the join HK gate passes.
	inmatch::GameConfig host_config;
	host_config.game_type = 0x30020u; // captured Co-op: phase-3 objective layout is active
	host_config.server_name = "Retail Sequence Host";
	host_config.mission_name = "Cooperative Test Mission";
	host_config.mission_file = "COOP_TEST.BMS";
	host_config.expansion = "jox01";
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
	                        0x0FE0E112u, nullptr, host_config);

	// The host's authoritative World. ctx.world set BEFORE the join so tick_connections'
	// Server_ProcessPendingPlayerSpawns spawns the joiner's pool-0 entity (binding conn.link.owned_entity).
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem &ai = world.ai;
	world.script.subgoals.won = 0x12u;
	world.script.subgoals.lost = 0x24u;
	world.script.subgoals.show_win = 0x48u;
	world.script.subgoals.show_lose = 0x90u;
	ctx.world = &world;
	// The host's own local player (sets cached.local_player, which apply_player_intent refuses to snap).
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host's own player spawned (cached.local_player)")) return false;

	inmatch::ClientRuntime client(kName);
	client.set_world_ready(false); // discover the authoritative mission before installing its world
	inmatch::CharacterJoinVars join_profile;
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
	auto note_event = [&](const inmatch::HostAcceptEvent &e) {
		if (e.kind == inmatch::HostAcceptEvent::Kind::PeerSpawned && !spawned) {
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
			for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
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
		inmatch::HandleResult r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), tick++);
		for (const inmatch::HostAcceptEvent &e : r.events) note_event(e);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	};

	// --- 1) Retail post-auth exchange learns the mission while the world/load drive is held. ---
	pump_host(client.start()); // ClientHello -> ServerHello (queued back to the client)
	for (int f = 0; f < 20 && !client.mission_known(); ++f)
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
	if (!expect(client.mission_known(), "joiner learned mission metadata from S2C 0x7B")) return false;
	if (!expect(client.server_name() == host_config.server_name &&
	                    client.mission_name() == host_config.mission_file &&
	                    client.map_file() == host_config.mission_file &&
	                    client.game_type() == host_config.game_type &&
	                    client.expansion() == host_config.expansion,
	            "S2C 0x7B retained retail LAN server/map/gametype/expansion fields")) return false;
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
	if (!expect(client.phase() == inmatch::JoinerConnection::Phase::Driving && !spawned,
	            "mission discovery stays on the same pre-spawn connection")) return false;

	uint32_t held_server_key = 0;
	uint32_t held_connection_id = 0;
	for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!(conn.peer == peer)) continue;
		held_server_key = conn.server_sk;
		held_connection_id = conn.connection_id;
	}
	for (int f = 0; f < 4; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
		for (inmatch::TickOut &t : inmatch::tick_connections(ctx, 300, tick++)) {
			for (const inmatch::HostAcceptEvent &e : t.events) note_event(e);
			for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
		}
	}
	if (!expect(!saw_world_drive_while_held &&
	                    !spawned && !inmatch::connection_spawned(ctx, peer),
	            "0x0A and loadout/status wait for world-ready")) return false;

	// Install the advertised mission, then resume on the exact authenticated session.
	client.set_world_ready(true);

	// --- 2) Spawn-gate burst, driven entirely from the per-frame client role. ---
	for (int f = 0; f < 120 && !spawned; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
		// Several host ticks per frame so entity_batch_count climbs through world streaming (F3) and the
		// spawn gate opens; ship the burst replies back to the client. The match service advances
		// with each host tick: the pending-player spawn pump admits only on its periodic second
		// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8DC0, called from Server_TickUpdate
		//  @0x51DBFD inside the reload-62 block].
		for (int k = 0; k < 6; ++k) {
			world.match.advance_tick(world);
			for (inmatch::TickOut &t : inmatch::tick_connections(ctx, 300, tick++)) {
				for (const inmatch::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
		}
	}
	if (!expect(spawned, "the spawn-gate burst trips PeerSpawned")) return false;
	if (!expect(spawn_name == kName, "PeerSpawned carries the joiner's ClientAuth.na name")) return false;
	if (!expect(inmatch::connection_spawned(ctx, peer), "host marks the peer spawned")) return false;
	bool resumed_same_session = false;
	for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (conn.peer == peer)
			resumed_same_session = conn.server_sk == held_server_key &&
			                       conn.connection_id == held_connection_id;
	if (!expect(resumed_same_session, "world-ready resumes the same authenticated session")) return false;

	// --- 3) Owner's PeerSpawned reaction: bind the connection's transport (the pipeline already bound
	//        owned_entity) + stream a NAMED organic-spawn so the client matches the owner ID. ---
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	w::EntityHandle Hh{};
	for (inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list) {
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
				je0->minimap_net_id, je0->anim_slot, je0->owner_connection_id);
		std::vector<uint8_t> sdg;
		if (!expect(inmatch::frame_in_match_s2c(ctx, peer, 0x0C, body, sdg), "host frames the named 0x0C"))
			return false;
		client.receive(sdg.data(), sdg.size());
		// The owner-ID match and the post-0x0E deploy release may arrive on adjacent
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
	            "client reached InMatch after owner-ID match plus deploy release; H == the host wire handle")) return false;
	if (!expect(
			client.spawn_pose().anim_slot == je0->anim_slot &&
					client.spawn_pose().net_id == je0->minimap_net_id,
			"the owner-ID matched self spawn retains the host-stamped character selector and id"))
		return false;
	if (!expect(client.view().game_type() == host_config.game_type,
	            "joiner learned authoritative g_GameType before live 0x0A frames")) return false;
	if (!expect(client.is_deployed(), "client is deployed after owner-ID match plus the applicable 0x5A release")) return false;

	// --- 4) In-match per-frame loop: client 0x0C -> apply_in_match_c2s -> Server_TickUpdate -> 0x0A fold ---
	PlayerExtendedUplink up;
	up.carrier_handle = 0xFFFF;
	up.pos_x = w::to_fixed(100.0);
	up.pos_y = w::to_fixed(200.0);
	up.pos_z = w::to_fixed(-50.0);
	up.heading = 0x2000; // -> mission yaw 45
	up.pitch = 0x0100;

	// The host's settings update dictates the send-holdoff period. An OpenNova
	// host dictates the engine-max period 1 (D-NET-197), so the client uplinks
	// every tick; drain any residual countdown (retail cycle: dec-then-check —
	// a frame is open when the countdown is <= 1 at its start).
	for (int frame = 0;
	     frame < 8 && client.send_holdoff_countdown() > 1; ++frame) {
		const auto held = client.Client_ProcessNetworkFrame(tick++);
		if (!expect(held.empty(),
		            "settings send-holdoff suppresses the whole deployed send block"))
			return false;
	}
	if (!expect(client.send_holdoff_ticks() == 1 &&
	                    client.send_holdoff_countdown() <= 1,
	            "the OpenNova host dictates the engine-max period 1"))
		return false;

	std::size_t staged = 0;
	std::vector<std::vector<uint8_t>> client_frame =
			client.Client_ProcessNetworkFrame(up, tick);
	if (!expect(client_frame.size() == 1,
	            "one deployed client tick batches RTT and uplink into one session datagram"))
		return false;
	for (std::vector<uint8_t> &d : client_frame) {
		inmatch::HandleResult r = inmatch::handle_server_datagram(ctx, peer, d.data(), d.size(), tick++);
		for (const inmatch::HostAcceptEvent &e : r.events) staged += inmatch::apply_in_match_c2s(ctx, e);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	}
	// With a World bound the host read-applies the 0x0C inside the dispatch walk, in
	// wire order (retail's NapiNPServerMsg_0x00C @0x501C30); nothing is surfaced for
	// the tick-time FIFO, so the joiner entity already sits at the uplink pose here.
	if (!expect(staged == 0, "no C2S 0x0C is staged for the tick drain once a World applies it inline")) return false;
	{
		const w::Entity *snapped = world.registry.get(Hh);
		if (!expect(snapped != nullptr &&
		                    snapped->position.x == static_cast<float>(w::from_fixed(up.pos_x)),
		            "the dispatch walk SNAPped the joiner before the authoritative tick")) return false;
	}

	// This focused test invokes Server_TickUpdate without HostOwner's pump; open
	// the remote peer's per-connection send boundary exactly as the owner does
	// before the authoritative tick.
	for (inmatch::NapiNPConnection &connection : ctx.np_protocol.connection_list)
		if (connection.peer == peer) connection.s2c_send_boundary_open = true;
	inmatch::Server_TickUpdate(ctx); // drain (SNAP) -> run_logic_tick -> emit per-connection 0x0A

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
		if (inmatch::frame_in_match_s2c(ctx, peer, 0x0A, inner, dg83)) {
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
	                    client.state().objective_won == world.script.subgoals.won &&
	                    client.state().objective_lost == world.script.subgoals.lost &&
	                    client.state().objective_show_win == world.script.subgoals.show_win &&
	                    client.state().objective_show_lose == world.script.subgoals.show_lose,
	            "objective Co-op frame folds all four authoritative subgoal masks")) return false;

	// Partial 0x0A decoding is intentionally useful for entity presentation, but a
	// packet that ends before the recipient tail must not manufacture health zero
	// or close the deployed gate. The tail is after the selected sub-block.
	const int16_t health_before_short_frame = client.state().local_health;
	const uint32_t frames_before_short_frame = client.state().frames_applied;
	std::vector<uint8_t> short_0a(14, 0); // anchor + flags, short before sub-block/tail
	std::vector<uint8_t> short_dg;
	if (!expect(inmatch::frame_in_match_s2c(ctx, peer, 0x0A, short_0a, short_dg),
	            "host frames the deliberately short 0x0A")) return false;
	client.receive(short_dg.data(), short_dg.size());
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick++))
		pump_host(std::move(d));
	if (!expect(client.state().frames_applied == frames_before_short_frame + 1,
	            "partial frame remains available to the lenient view fold")) return false;
	if (!expect(client.state().local_health == health_before_short_frame,
	            "a frame without a decoded recipient tail preserves local health")) return false;
	if (!expect(client.is_deployed(),
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
	inmatch::Server_TickUpdate(ctx);

	bool got_death_0a = false;
	while (udp_host.pop_outbound(raw)) {
		if (raw.empty() || raw[0] != 0x0A) continue;
		std::vector<uint8_t> inner(raw.begin() + 1, raw.end());
		std::vector<uint8_t> dg83;
		if (inmatch::frame_in_match_s2c(ctx, peer, 0x0A, inner, dg83)) {
			client.receive(dg83.data(), dg83.size());
			got_death_0a = true;
		}
	}
	if (!expect(got_death_0a, "host emitted the authoritative S2C 0x0A death frame")) return false;

	std::size_t staged_after_death = 0;
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(up, tick)) {
		inmatch::HandleResult r = inmatch::handle_server_datagram(ctx, peer, d.data(), d.size(), tick++);
		for (const inmatch::HostAcceptEvent &event : r.events)
			staged_after_death += inmatch::apply_in_match_c2s(ctx, event);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	}
	if (!expect(client.state().frames_applied == frames_before_death + 1,
	            "client folded the fresh authoritative death frame")) return false;
	if (!expect(client.state().local_health == 0,
	            "client stores zero from its recipient-specific 0x0A health tail")) return false;
	if (!expect(!client.is_deployed(), "authoritative death closes the deployed uplink gate")) return false;
	if (!expect(client.deployment_pick_pending(),
	            "authoritative death re-enters the deployment FSM so the player can respawn"))
		return false;
	if (!expect(staged_after_death == 0,
	            "the receive-before-send death frame stages no C2S 0x0C uplink")) return false;

	// Exactly one SNAP per 0x0C: a second tick with no new uplink drained nothing more.
	if (!expect(udp_host.inbound_pending() == 0, "the connection's C2S queue is drained")) return false;

	// A deployment release and a fresh dead 0x0A tail can share ONE datagram
	// (the same-tick spawn-kill / resend window). The wire-position death
	// edge must hold the spawn latch closed against the poll's batched
	// release flags — only the deploy flow reopens it.
	{
		// Capture a dead 0x0A inner while the victim is still down.
		inmatch::Server_TickUpdate(ctx);
		std::vector<uint8_t> dead_inner;
		while (udp_host.pop_outbound(raw)) {
			if (!raw.empty() && raw[0] == 0x0A)
				dead_inner.assign(raw.begin() + 1, raw.end());
		}
		if (!expect(!dead_inner.empty(),
				"host emitted a dead 0x0A inner for the crafted datagram")) return false;
		// Queue and ship an INVALID re-pick (pool-2 slot 0xFFE resolves no
		// entity — the host's resolve-miss break drops it silently, zones
		// leg above): the client reaches AwaitDeployRelease with the pick
		// acked while NO natural release ever races the crafted datagram,
		// and every host reply is delivered so the session stream stays
		// gap-free.
		if (!expect(client.queue_deployment_pick(0x2FFE),
				"the dead client queues the invalid re-pick")) return false;
		int repick_on_wire = 0;
		for (int frame = 0; frame < 60 && repick_on_wire == 0; ++frame) {
			for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) {
				uint8_t opcode = 0;
				std::vector<uint8_t> session_body;
				if (nw_decode_inbound(d.data(), d.size(), opcode, session_body) &&
						opcode == SESSION_OPCODE_PROTOCOL_MESSAGE) {
					for (const inmatch::NapiNPConnection &conn :
							ctx.np_protocol.connection_list) {
						if (!(conn.peer == peer)) continue;
						SessionSequencing seq = conn.seq;
						ProtocolPacketHeader hdr;
						std::vector<ProtocolMessage> messages;
						if (deframe_session_packet(seq,
								SessionCrypto{{}, conn.client_scrk, 0},
								session_body.data(), session_body.size(), hdr,
								messages)) {
							for (const ProtocolMessage &message : messages)
								if (message.tag == 0x0E) ++repick_on_wire;
						}
						break;
					}
				}
				inmatch::HandleResult r = inmatch::handle_server_datagram(
						ctx, peer, d.data(), d.size(), tick++);
				for (const std::vector<uint8_t> &o : r.outbound)
					client.receive(o.data(), o.size());
			}
			// The client's send boundary opens on receive: keep the host's
			// keepalive stream flowing (the invalid pick produces no release
			// on this path — the resolve-miss break drops it host-side).
			for (inmatch::TickOut &t : inmatch::tick_connections(ctx, 300, tick++)) {
				for (const std::vector<uint8_t> &o : t.outbound)
					client.receive(o.data(), o.size());
			}
		}
		if (!expect(repick_on_wire == 1,
				"the invalid re-pick reached the wire exactly once")) return false;
		const inmatch::NapiNPConnection *jc = nullptr;
		for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
			if (conn.peer == peer) jc = &conn;
		if (!expect(jc != nullptr && !jc->reply.last_loadout_reply.empty(),
				"host retains the last 0x5A grant body")) return false;
		if (!expect(!client.in_match(),
				"the dead client sits outside InMatch before the crafted release")) return false;
		const uint64_t release_revision_before =
				client.deployment_release_revision();
		std::vector<uint8_t> dg;
		if (!expect(inmatch::frame_in_match_s2c_batch(ctx, peer,
				{make_protocol_message(0x5A, jc->reply.last_loadout_reply),
				 make_protocol_message(0x0A, dead_inner)}, dg),
				"host frames the release + dead tail into one datagram")) return false;
		client.receive(dg.data(), dg.size());
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick++))
			(void)d; // receive-only: nothing may race the assertions below
		// The release leg ran (the revision edge fired) AND the same
		// datagram's dead tail re-entered redeployment at its wire position —
		// so the spawn latch must end CLOSED, not re-opened by the batched
		// release flags.
		if (!expect(client.deployment_release_revision() ==
						release_revision_before + 1,
				"the crafted release fired its revision edge")) return false;
		if (!expect(!client.in_match() && client.deployment_pick_pending(),
				"the same-datagram death edge re-entered the deploy flow")) return false;
		if (!expect(!client.authoritative_spawn_released(),
				"a same-datagram death edge outlasts the batched release latch"))
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (B) Host-as-client (D-NET-121/122): the host's own loopback view anchors to its player, not dvxi5.
// ---------------------------------------------------------------------------------------------------
// The integrated spawn-zone join: admission completes before a manual pick,
// but the host keeps its respawn-pending hold until the player selects a spawn.
// Exercise the initial overlay selection over real framing, host dispatch and
// the ACK-qualified release, including the dictated twelve-tick send boundary.
// [orig: Server_OnPlayerJoin @0x51A680, hold @0x51A6F2; Input_HandleActionBinding @0x49AD40, case 12 @0x49B0C5..0x49B17B]
bool run_roundtrip_with_spawn_zones(bool under_send_holdoff) {
	const PeerAddr peer{0x0100007Fu, 30001}; // 127.0.0.1:30001
	const std::string kName = "ZonesJoiner";

	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig host_config;
	host_config.game_type = 0x30020u;
	host_config.server_name = "Zones Host";
	host_config.mission_name = "Zones Test Mission";
	host_config.mission_file = "ZONES_TEST.BMS";
	// With the dictated send boundary closed, framing the joiner's uplinks waits
	// for the next twelve-tick boundary.
	if (under_send_holdoff) host_config.send_holdoff_ticks = 12;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
	                        0x0FE0E112u, nullptr, host_config);

	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(2, 16);
	world.registry.configure_pool(3, 16);
	w::AiSystem &ai = world.ai;
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
	w::EntityHandle zone_h{};
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
		zone_h = world.registry.spawn(2, zone);
	}
	if (!expect(zone_h.valid(), "zones: selectable non-default spawn zone spawned")) return false;
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "zones: host player spawned")) return false;

	inmatch::ClientRuntime client(kName);
	client.set_world_ready(false);
	// The binding seam under test alongside the zones flow: the shell's applied kit
	// replaces the capture-default 0x2F pair content (D-NET-168). The wire team byte
	// stays host-owned — this co-op joiner latches the same team 1 reservation
	// the later player-add pass consumes from the host's S2C 0x04.
	{
		inmatch::JoinerConnection::LoadoutKit kit;
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
	int gameplay_uplink_count = 0;
	int net_quality_count = 0;
	std::vector<std::vector<uint8_t>> deploy_picks;
	std::vector<std::vector<uint8_t>> submitted_loadouts;
	auto note_event = [&](const inmatch::HostAcceptEvent &e) {
		if (e.kind == inmatch::HostAcceptEvent::Kind::PeerSpawned && !spawned) {
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
			for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
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
					if (message.tag == 0x0C && !message.payload.empty())
						++gameplay_uplink_count;
					if (message.tag == 0x4C)
						++net_quality_count;
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
		inmatch::HandleResult r = inmatch::handle_server_datagram(ctx, peer, dg.data(), dg.size(), tick++);
		for (const inmatch::HostAcceptEvent &e : r.events) {
			note_event(e);
			inmatch::apply_in_match_c2s(ctx, e);
		}
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
			// The spawn pump admits on the match's periodic second [orig: @0x51DBFD].
			world.match.advance_tick(world);
			for (inmatch::TickOut &t : inmatch::tick_connections(ctx, 300, tick++)) {
				for (const inmatch::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
		}
	}
	if (!expect(spawned && spawn_name == kName, "zones: spawn gate tripped for the joiner")) return false;

	// The owner's PeerSpawned reaction ships the joiner's NAMED organic record (the burst
	// streamed pool 0 before this entity existed) — same leg run_roundtrip pins.
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	w::EntityHandle Hh{};
	for (inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!(c.peer == peer)) continue;
		c.link.transport = &udp_host;
		c.link.mode = ns::TransportMode::Client;
		Hh = c.link.owned_entity;
	}
	if (!expect(Hh.valid(), "zones: owned_entity bound by the spawn pipeline")) return false;
	const w::Entity *je0 = world.registry.get(Hh);
	{
		std::vector<uint8_t> named = make_organic_spawn(
				Hh.packed, spawn_name, w::to_fixed(50.0), w::to_fixed(60.0),
				w::to_fixed(1.0), 0x40000000, 1, je0 ? je0->net_id : 0, 0, je0 ? je0->owner_connection_id : 0);
		std::vector<uint8_t> sdg;
		if (!expect(inmatch::frame_in_match_s2c(ctx, peer, 0x0C, named, sdg),
				"zones: host frames the named 0x0C")) {
			return false;
		}
		client.receive(sdg.data(), sdg.size());
	}

	// Drive the deployment exchange: the phase-8 bundle ships 0x0F, both initial 0x5A
	// grants complete admission, and the joiner enters the match through the host's spawn.
	auto drive_frames = [&](int frames, auto until) {
		for (int f = 0; f < frames && !until(); ++f) {
			for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick))
				pump_host(std::move(d));
			for (inmatch::TickOut &t : inmatch::tick_connections(ctx, 300, tick++)) {
				for (const inmatch::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
			for (inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
				if (conn.peer == peer) conn.s2c_send_boundary_open = true;
			inmatch::Server_TickUpdate(ctx);
			std::vector<uint8_t> raw;
			while (udp_host.pop_outbound(raw)) {
				if (raw.empty()) continue;
				std::vector<uint8_t> datagram;
				if (inmatch::frame_in_match_s2c(ctx, peer, raw[0],
						std::vector<uint8_t>(raw.begin() + 1, raw.end()), datagram))
					client.receive(datagram.data(), datagram.size());
			}
		}
	};
	// Admission itself emits no pick: the user chooses through the deploy map.
	// The initial grant pair does not clear the host's spawn-zone hold.
	(void)zone_h;
	drive_frames(120, [&] {
		return client.in_match() && client.initial_admission_complete();
	});
	if (!expect(client.in_match() && client.initial_admission_complete() &&
				deploy_pick_count == 0,
			"zones: joiner completes admission and enters the match with no forced 0x0E"))
		return false;
	if (!expect(client.self_handle() == Hh.packed,
			"zones: the joiner bound the host's self wire handle")) return false;
	if (!expect(client.assigned_team() == 1 && je0 != nullptr && je0->team == 1,
			"zones: pre-spawn 0x04 and the co-op entity share team 1")) return false;
	// The authority holds deployment until the initial map selection arrives.
	bool host_still_pending = false;
	for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (conn.peer == peer) host_still_pending = conn.link.respawn_pending;
	if (!expect(host_still_pending,
			"zones: host keeps its spawn-zone hold until a selection"))
		return false;

	// AAS retail keeps this alive player in its spawn-zone hold until the
	// deploy-map selection arrives. Closing only the local overlay leaves the
	// authority pending and eventually produces the witnessed t35 idle punt.
	if (!expect(client.state().deploy_overlay_active &&
			client.queue_deployment_pick(0xFFFFu),
			"zones: the initial deploy-map selection queues a real C2S 0x0E")) return false;
	if (!expect(!client.is_deployed(),
			"zones: selection holds gameplay until the authority's release")) return false;
	auto authority_pending = [&] {
		for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
			if (conn.peer == peer) return conn.link.respawn_pending;
		return true;
	};
	drive_frames(240, [&] {
		return !authority_pending() && client.is_deployed() &&
				!client.deployment_pick_pending() && !client.state().deploy_overlay_active;
	});
	if (authority_pending() || !client.is_deployed() || client.state().deploy_overlay_active)
		std::fprintf(stderr, "zones release: picks=%d host_pending=%d deployed=%d pick_pending=%d overlay=%d\n",
				deploy_pick_count, authority_pending(), client.is_deployed(),
				client.deployment_pick_pending(), client.state().deploy_overlay_active);
	if (!expect(deploy_pick_count == 1 && deploy_pick_shape_ok && pending_at_pick_time &&
			!authority_pending() && client.is_deployed() &&
			!client.deployment_pick_pending() && !client.state().deploy_overlay_active,
			"zones: the real selection releases both host and client deployment state")) return false;
	if (!expect(!client.queue_deployment_pick(0xFFFFu),
			"zones: normal gameplay without the deploy UI cannot queue another pick")) return false;

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
	// Allow two of the dictated twelve-tick boundaries for the queued submit.
	drive_frames(24, [&] { return submitted_loadouts.size() >= 3; });
	return expect(submitted_loadouts.size() == 3 && submitted_loadouts[2] == expected_second,
			"zones: armory re-submission rode the wire with the equipped combo");
}

bool run_joiner_remote_reload_stamps_before_same_frame_body_tick() {
	const std::string client_scrk = "CLIENT-RELOAD-ORDER-SCRK";
	const std::string server_scrk = "SERVER-RELOAD-ORDER-SCRK";
	constexpr uint16_t kSelfHandle = 0x0002;
	constexpr uint16_t kRemoteHandle = 0x0007;
	constexpr uint16_t kNonPersonHandle = 0x1003;

	inmatch::ClientRuntime client("ReloadOrder");
	client.seed_session(
			0x10203040u, 1u, client_scrk, server_scrk,
			1, 0, kSelfHandle, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/true);
	client.view().set_item_class_resolver([](uint16_t type_id) {
		if (type_id == 0x14B9u) return EntityClass::Player;
		return EntityClass::Unknown;
	});

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> spawn = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(
					0x0C,
					make_organic_spawn(
							kRemoteHandle, "RemoteReload",
							0x10000, 0x20000, 0x30000, 0, 2, 9))});
	client.receive(spawn.data(), spawn.size());
	(void)client.Client_ProcessNetworkFrame(1);
	replication::ClientEntityState &non_person =
			client.state().upsert(kNonPersonHandle);
	non_person.cls = EntityClass::Vehicle;

	const WeaponReload reload{kRemoteHandle, 0x00C5};
	const WeaponReload non_person_reload{kNonPersonHandle, 0x00C6};
	const std::vector<uint8_t> reload_echo = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x49, encode_weapon_reload(reload)),
					make_protocol_message(
							0x49, encode_weapon_reload(non_person_reload)),
			});
	client.receive(reload_echo.data(), reload_echo.size());
	(void)client.Client_ProcessNetworkFrame(2);

	const replication::ClientEntityState *remote = client.state().find(kRemoteHandle);
	if (!expect(remote != nullptr && remote->cls == EntityClass::Player,
			"reload order: the addressed remote Person row exists"))
		return false;
	if (!expect(remote->arms_dip_ticks == 78,
			"reload order: S2C 0x49 stamps 80 before the same frame's two decrements"))
		return false;
	if (!expect(remote->pitch_kick_accum == -0x02300000,
			"reload order: the same frame applies the first retail arms-dip step"))
		return false;
	if (!expect(non_person.arms_dip_ticks == 0 &&
				non_person.pitch_kick_accum == 0,
			"reload order: the runtime does not invent the unmodeled non-Person refill"))
		return false;

	const std::vector<WeaponReload> notifications =
			client.drain_reload_notifications();
	return expect(notifications.size() == 2 &&
					notifications[0].entity_handle == kRemoteHandle &&
					notifications[0].reload_param == reload.reload_param &&
					notifications[1].entity_handle == kNonPersonHandle &&
					notifications[1].reload_param == non_person_reload.reload_param,
			"reload order: the runtime preserves the notification for simulation consumers");
}

bool run_host_client_discards_authority_owned_reload_echoes() {
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host_view(host_loop);

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
	inmatch::ClientRuntime host_view(host_loop);
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
	// The two contest bytes ride the same message onto the live entry — the AAS
	// status panel's in-radius counts [orig: @0x428e79/@0x428e7f].
	if (!expect(host_view.zone_states().at(kZone).entry.contest_owner == 0xCC &&
	                    host_view.zone_states().at(kZone).entry.contest_other == 0xDD,
	            "zone value: the 0x6F contest bytes are retained on the entry"))
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
	inmatch::ClientRuntime host_view(host_loop);
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

bool run_zone_presence_updates_only_a_tracked_window() {
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host_view(host_loop);
	constexpr uint16_t kZone = 0x1003;

	host_loop.host_send(
			s2c::ZONE_TIMER_WINDOW,
			zone_timer_window_body(kZone, 0, 1, 0, 15, 1));
	host_view.Client_ProcessNetworkFrame();
	host_loop.host_send(
			s2c::ZONE_PRESENCE_COUNT, zone_presence_body(kZone, 3));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.zone_states().at(kZone).has_presence &&
	                    host_view.zone_states().at(kZone).presence_count == 3,
	            "zone presence: 0x6C replaces the tracked active-window count"))
		return false;

	// The retail handler only changes dword_A85BA0 when the handle resolves to
	// its currently tracked timed-capture entity. Unknown and malformed rows do
	// not create timer entries. [orig: NapiNPClientMsg_0x06C @0x428FC0]
	host_loop.host_send(
			s2c::ZONE_PRESENCE_COUNT, zone_presence_body(0x1004, 9));
	host_loop.host_send(s2c::ZONE_PRESENCE_COUNT, {0x03, 0x10});
	host_view.Client_ProcessNetworkFrame();
	if (!expect(host_view.zone_states().size() == 1 &&
	                    host_view.zone_states().at(kZone).presence_count == 3,
	            "zone presence: untracked/short 0x6C rows fail closed"))
		return false;

	// The tracked-window cluster image [orig: dword_A85B88..A85BA0]: the 0x53
	// seeded progress = target = 62*start, limit = 62*end, rate = byte; the
	// 0x6C on the tracked entity re-rated it to 3; the per-frame pump adds the
	// rate and clamps progress <= target for a positive rate — so the count
	// never moves it (the inert positive-rate clamp) — and the untracked 0x6C
	// left the rate alone.
	const inmatch::ClientRuntime::TrackedCaptureWindow &w = host_view.tracked_capture_window();
	if (!expect(w.tracked() && w.zone == kZone && w.rate == 3 && w.target == 0 &&
	                    w.limit == 62 * 15 && w.progress == 0 && w.mode_a == 0 && w.mode_b == 1,
	            "tracked window: the 0x53 seed + the 0x6C re-rate + the inert clamp"))
		return false;
	for (int i = 0; i < 10; ++i) host_view.Client_ProcessNetworkFrame();
	if (!expect(w.progress == 0, "tracked window: a positive rate never passes the target"))
		return false;
	// A changed modeB on the same entity re-seeds progress at the new start;
	// a start at or past the end then drops the cluster (rate 0, no entity)
	// but the parked progress survives [orig: @0x428d09 / @0x428d40..0x428d60].
	host_loop.host_send(
			s2c::ZONE_TIMER_WINDOW, zone_timer_window_body(kZone, 0, 2, 7, 7, 4));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(!w.tracked() && w.rate == 0 && w.limit == 0 && w.progress == 62 * 7,
	            "tracked window: start >= end zeroes the cluster, progress survives"))
		return false;
	// With nothing tracked the next window is adopted WITHOUT a progress
	// re-seed (the `!dword_A85B88` arm jumps past LABEL_35), and the same
	// frame's pump clamps the stale 434 down to the new target under the
	// positive rate [orig: @0x428c5b -> LABEL_36; the clamp @0x42c32f].
	host_loop.host_send(
			s2c::ZONE_TIMER_WINDOW, zone_timer_window_body(kZone, 0, 2, 5, 6, 1));
	host_view.Client_ProcessNetworkFrame();
	return expect(w.tracked() && w.target == 310 && w.limit == 372 && w.progress == 310 &&
	                      w.rate == 1,
	              "tracked window: an adoption from nothing keeps the stale progress and the pump clamps it to the target");
}

// The client-side 1 Hz revive countdown: every 63rd client frame each active
// S2C 0x4C table slot with an entity and a nonzero window loses one second; the
// medic-request latch survives [orig: Client_ProcessNetworkFrame
// @0x42C27E..0x42C2DA -> PlayerSlot_SetDownedState @0x4348D0]. Both the
// S2C 0x54 seed and the 0x46 bit-0x0008 seed feed the same slot bytes.
bool run_roster_revive_countdown_ticks_once_per_63_frames() {
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host_view(host_loop);

	PlayerReplicationState rep;
	rep.player_slot = 3;
	rep.player_name = "Downed";
	rep.entity_handle = 0x0007;
	host_loop.host_send(0x46, encode_player_sync(rep, kPlayerSyncHasName));
	PlayerDownedState downed;
	downed.entity_handle = 0x0007;
	downed.revive_seconds = 120;
	downed.medic_request_active = true;
	host_loop.host_send(s2c::PLAYER_DOWNED_STATE, encode_player_downed_state(downed));
	// The countdown walks the S2C 0x4C table: slot 3 is listed.
	host_loop.host_send(s2c::VISIBLE_PLAYERS, {0x01, 0x03, 0x07, 0x00});
	host_view.Client_ProcessNetworkFrame();
	const ns::ClientRosterSlot &slot = host_view.state().roster[3];
	if (!expect(slot.bound && slot.entity_slot == 7 &&
	                    slot.downed_revive_seconds == 120 && slot.medic_request_active,
	            "revive countdown: the 0x54 seed lands on the slot the 0x46 bound"))
		return false;
	// Frames 2..62 leave the window alone; frame 63 is the first decrement.
	for (int i = 0; i < 61; ++i) host_view.Client_ProcessNetworkFrame();
	if (!expect(slot.downed_revive_seconds == 120,
	            "revive countdown: 62 frames do not tick the window"))
		return false;
	host_view.Client_ProcessNetworkFrame();
	if (!expect(slot.downed_revive_seconds == 119 && slot.medic_request_active,
	            "revive countdown: the 63rd frame takes one second and keeps the request latch"))
		return false;
	for (int i = 0; i < 63; ++i) host_view.Client_ProcessNetworkFrame();
	if (!expect(slot.downed_revive_seconds == 118,
	            "revive countdown: the timer resets and fires again 63 frames later"))
		return false;

	// The 0x46 bit-0x0008 path seeds the same byte and counts down the same
	// way [orig: NapiNPClientMsg_PlayerSync 0x0008 -> PlayerSlot_SetDownedState].
	rep.downed_state = 0x05;
	host_loop.host_send(0x46, encode_player_sync(rep, kPlayerSyncHasDownedState));
	host_view.Client_ProcessNetworkFrame();
	if (!expect(slot.downed_revive_seconds == 5 && !slot.medic_request_active,
	            "revive countdown: the 0x46 bit-0x0008 field re-seeds the window"))
		return false;
	for (int i = 0; i < 62; ++i) host_view.Client_ProcessNetworkFrame();
	if (!expect(slot.downed_revive_seconds == 4,
	            "revive countdown: the 0x46 seed counts down on the same cadence"))
		return false;
	// A window at zero stays at zero (the > 0 gate @0x42C2B7).
	rep.downed_state = 0x00;
	host_loop.host_send(0x46, encode_player_sync(rep, kPlayerSyncHasDownedState));
	for (int i = 0; i < 130; ++i) host_view.Client_ProcessNetworkFrame();
	return expect(slot.downed_revive_seconds == 0,
	              "revive countdown: an exhausted window never wraps");
}

// The dead player's medic call: a deployed joiner queues one reliable C2S
// 0x2E carrying its packed entity index; a HostClient view never sends
// [orig: Input_HandleActionBinding case 217 @0x49b4b4..0x49b51b].
bool run_medic_request_queues_one_reliable_0x2e() {
	const std::string client_scrk = "CLIENT-MEDIC-SCRK";
	const std::string server_scrk = "SERVER-MEDIC-SCRK";
	inmatch::ClientRuntime client("MedicJoiner", [] { return uint64_t{0x10203040}; });
	if (!expect(!client.queue_medic_request(),
	            "medic request: an unconnected joiner cannot queue"))
		return false;
	client.seed_session(
			0x55667799u, 1u, client_scrk, server_scrk,
			1, 0, 0x0007, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	if (!expect(client.queue_medic_request(),
	            "medic request: a deployed joiner queues the call"))
		return false;
	const std::vector<std::vector<uint8_t>> frame =
			client.Client_ProcessNetworkFrame(1);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(frame.size() == 1 &&
			decode_client_session(frame[0], client_scrk, header, messages) &&
			messages.size() == 2 && messages[0].tag == c2s::MEDIC_REQUEST &&
			messages[0].payload == std::vector<uint8_t>({0x07, 0x00, 0x00, 0x00}),
			"medic request: exact 4-B packed entity index on C2S 0x2E"))
		return false;
	if (!expect(client.retained_outbound_depth() == 1,
	            "medic request: the call is reliable"))
		return false;
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host_view(host_loop);
	return expect(!host_view.queue_medic_request(),
	              "medic request: the host's own view never uplinks");
}

// The server-info VarList walk lands EXP_FANFARE as the u16 the 0x81 tone
// ladder reads [orig: Client_ParseServerSessionVariables @0x5202f0 -> @0x520478].
bool run_session_vars_exp_fanfare_walk() {
	auto kv = [](std::vector<uint8_t> &out, const char *key, std::vector<uint8_t> value) {
		for (const char *p = key; *p; ++p) out.push_back(uint8_t(*p));
		out.push_back(0);
		const uint32_t n = uint32_t(value.size());
		out.push_back(uint8_t(n)); out.push_back(uint8_t(n >> 8));
		out.push_back(uint8_t(n >> 16)); out.push_back(uint8_t(n >> 24));
		out.insert(out.end(), value.begin(), value.end());
	};
	const auto fanfare = [](const std::vector<uint8_t> &stream) {
		SessionVars vars;
		decode_session_vars(stream.data(), stream.size(), vars);
		return vars.exp_fanfare;
	};
	std::vector<uint8_t> body;
	kv(body, "SERVERNAME", {'b', 'i', 'g', 'g', 'y', 0});
	kv(body, "GAMETYPE", {0x20, 0x00, 0x03, 0x00});
	kv(body, "EXP_FANFARE", {5, 20});
	kv(body, "MISSIONFILENAME", {'x', 0});
	if (!expect(fanfare(body) == 0x1405,
	            "exp_fanfare: lo byte 5 / hi byte 20 land as the u16"))
		return false;
	std::vector<uint8_t> absent;
	kv(absent, "SERVERNAME", {'b', 0});
	if (!expect(fanfare(absent) == 0, "exp_fanfare: an absent key reads 0"))
		return false;
	std::vector<uint8_t> truncated(body.begin(), body.begin() + 20);
	return expect(fanfare(truncated) == 0, "exp_fanfare: a truncated stream fails closed");
}

bool run_zone_timer_uses_wrapping_dword_arithmetic_and_signed_clamps() {
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host_view(host_loop);
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
	inmatch::ClientRuntime client("ZoneJoiner");
	client.seed_session(
			0x66778899u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/true);

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
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
	inmatch::NapiNPServerCtx ctx;
	ns::LoopbackChannel host_loop; // the in-process channel the host emits its own S2C onto
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
	                        0x0FE0E112u, &host_loop);

	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 128);
	world.registry.configure_pool(2, 16);
	w::AiSystem &ai = world.ai;
	ctx.world = &world;

	// Spawn the host's own player through the real pipeline (type-2 loopback -> spawn_player ->
	// cached.local_player; binds conn.link.owned_entity = the host player, the D-NET-121 anchor).
	const int spawned = inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(spawned == 1, "the host's own player spawned via the pipeline")) return false;

	// Find the host loopback connection; confirm its owned_entity is the host player + mark it in-match.
	// (In production burst.spawned latches when the host loopback's §5.2a burst completes — covered by
	// npruntime_initial_state_burst; here we set it directly to exercise the per-frame emit path.)
	inmatch::NapiNPConnection *self = nullptr;
	for (inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list) {
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
	w::Entity map_entity;
	map_entity.kind = w::EntityKind::Building;
	map_entity.item_type = 5;
	map_entity.has_item_def = true;
	map_entity.has_minimap_model_marker = true;
	map_entity.position = {64.0f, 96.0f, 0.0f};
	const w::EntityHandle map_handle = world.registry.spawn_from(2, 0, map_entity);
	if (!expect(map_handle.valid(), "host map fixture occupies a pool-2 slot"))
		return false;
	w::Entity late_vehicle;
	late_vehicle.kind = w::EntityKind::Item;
	late_vehicle.item_type = 1;
	late_vehicle.item_unit_type = 3;
	late_vehicle.has_item_def = true;
	late_vehicle.team = he->team;
	late_vehicle.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
	late_vehicle.position = {128.0f, 96.0f, 0.0f};
	const w::EntityHandle vehicle_handle =
			world.registry.spawn_from(1, 127, late_vehicle);
	if (!expect(vehicle_handle.valid() && vehicle_handle.packed == 0x107Fu,
			"vehicle fixture occupies the last retail minimap phase"))
		return false;

	inmatch::ClientRuntime host_view(host_loop); // HostClient recv-fold only
	ns::ClientEntityState &decoded_map = host_view.state().upsert(map_handle.packed);
	decoded_map.x = w::to_fixed(map_entity.position.x);
	decoded_map.y = w::to_fixed(map_entity.position.y);
	decoded_map.z = w::to_fixed(map_entity.position.z);

	// The frame is built ahead of the tick's entity update, so it anchors on
	// the pose the host player holds when Server_TickUpdate starts; the body
	// update may move it afterwards.
	const w::Vec3 framed = he->position;
	inmatch::Server_TickUpdate(ctx); // fans a per-frame 0x0A to the host loopback (is_in_match), anchored to hp

	if (!expect(host_view.is_authority(), "host-as-client runtime is authority (no 0x0C)")) return false;
	std::vector<std::vector<uint8_t>> out = host_view.Client_ProcessNetworkFrame();
	if (!expect(out.empty(), "host-as-client emits no C2S (is_authority gate)")) return false;

	if (!expect(host_view.state().frames_applied >= 1, "host-as-client folded its own 0x0A")) return false;
	if (!expect(host_view.state().anchor_x == w::to_fixed(framed.x) &&
	                    host_view.state().anchor_y == w::to_fixed(framed.y) &&
	                    host_view.state().anchor_z == w::to_fixed(framed.z),
	            "host-as-client anchor == host player position (D-NET-121: owned_entity, not dvxi5)"))
		return false;
	// Explicitly assert it is NOT the dvxi5 fallback (the bug D-NET-121 guards).
	if (!expect(host_view.state().anchor_x != static_cast<int32_t>(0xfe56f854u),
	            "host-as-client anchor is not the dvxi5 fallback")) return false;
	bool found_map_overlay = false;
	for (const ns::ClientMinimapOverlaySlot &slot :
			host_view.state().minimap.persistent) {
		if (slot.active && slot.handle == map_handle.packed) {
			found_map_overlay = true;
			break;
		}
	}
	if (!expect(found_map_overlay,
			"host loopback receives and retains the same initial minimap stream"))
		return false;

	// Pool-1 markers bubble into retail one phase every fourteen server ticks.
	// Slot 127 therefore must remain absent through tick 1778, then arrive on
	// tick 1779 (phase 0 ran on the first tick above). This keeps screenshot
	// readiness honest without changing the witnessed 14x128 cadence.
	const auto has_vehicle_overlay = [&] {
		for (const ns::ClientMinimapOverlaySlot &slot :
				host_view.state().minimap.transient) {
			if (slot.active && slot.handle == vehicle_handle.packed &&
					slot.param == 11)
				return true;
		}
		return false;
	};
	if (!expect(!has_vehicle_overlay(),
			"late-phase vehicle is absent before its dynamic scan"))
		return false;
	for (int tick = 2; tick <= 1778; ++tick) {
		inmatch::Server_TickUpdate(ctx);
		(void)host_view.Client_ProcessNetworkFrame();
	}
	if (!expect(!has_vehicle_overlay(),
			"late-phase vehicle remains absent through tick 1778"))
		return false;
	inmatch::Server_TickUpdate(ctx);
	(void)host_view.Client_ProcessNetworkFrame();
	if (!expect(has_vehicle_overlay(),
			"late-phase vehicle arrives and is retained on phase-127 tick 1779"))
		return false;

	// A mission restart recreates the client view with EMPTY retained banks
	// while the server connections persist; the host re-arms each
	// connection's one-shot initial scan so the producer re-sends the
	// persistent pool-2 markers to the fresh epoch (without the re-arm the
	// building/zone markers would be missing for the rest of the session).
	inmatch::ClientRuntime restart_view(host_loop);
	inmatch::Server_RearmMinimapInitialScan(ctx);
	bool restart_map_overlay = false;
	for (int tick = 0; tick < 16 && !restart_map_overlay; ++tick) {
		inmatch::Server_TickUpdate(ctx);
		(void)restart_view.Client_ProcessNetworkFrame();
		for (const ns::ClientMinimapOverlaySlot &slot :
				restart_view.state().minimap.persistent) {
			if (slot.active && slot.handle == map_handle.packed) {
				restart_map_overlay = true;
				break;
			}
		}
	}
	if (!expect(restart_map_overlay,
			"the re-armed initial scan repopulates a fresh client view"))
		return false;
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
	w::AiSystem &ai = world.ai;

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
	if (!expect(world.vehicles.process_attach(infantry_h, gun_h, 6),
	            "infantry attaches to the B50Cal gunner bone")) return false;
	w::Entity *infantry_live = world.registry.get(infantry_h);
	const w::Entity *gun_live = world.registry.get(gun_h);
	if (!expect(infantry_live != nullptr && gun_live != nullptr,
	            "mounted startup fixture entities resolve")) return false;
	world.vehicles.pose_mounted_occupant(*infantry_live, *gun_live, gun_live->seats[0]);

	ns::LoopbackChannel host_loop;
	inmatch::HostOwner owner;
	owner.host_loopback = &host_loop;
	owner.ctx.world = &world;
	inmatch::HostConfig cfg;
	cfg.config.server_name = "SINGLEPLAYERGAME";
	cfg.config.max_players = 1;
	cfg.socket_mode = inmatch::SocketMode::Socketless;
	cfg.serve_and_play = true;
	inmatch::start_host_session(owner, cfg);

	// Match Simulation's startup order: construct/install the replica pipeline after host bring-up,
	// then fold the queued initial stream and first whole-world compact frame together.
	inmatch::Server_TickUpdate(owner.ctx);
	inmatch::ClientRuntime host_view(host_loop);
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
	// The host's own client takes the header-only 0x0A (D-NET-140 closed): the
	// startup 0x0C organic row streams, but no compact record ever folds — the
	// host presents the mounted child from its own pools, where the attach
	// already lifted it through the load-time no-callback carrier
	// [orig: NetPacket_SerializeEntityStatesToPacket @0x50f07e].
	const ns::ClientEntityState *child = host_view.view().state().find(infantry_h.packed);
	if (!expect(child != nullptr && child->type_id == 5311,
	            "production host startup streams the infantry 0x0C row")) return false;
	if (!expect(host_view.state().compact_records_applied == 0,
	            "the host's own view folds no compact records")) return false;
	if (!expect(infantry_live->mounted && infantry_live->mount_target == gun_h &&
	                    infantry_live->mount_bone == 6,
	            "the host's pool row retains its B50Cal carrier and raw Usegun bone"))
		return false;

	const inmatch::NapiNPConnection *self = nullptr;
	for (const inmatch::NapiNPConnection &conn : owner.ctx.np_protocol.connection_list)
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
		inmatch::HostOwner owner;
		owner.ctx.world = &world;
		inmatch::HostConfig cfg;
		cfg.config.mp_attributes = 0x8000u;
		cfg.socket_mode = inmatch::SocketMode::Socketless;
		inmatch::start_host_session(owner, cfg);
		if (!expect(world.throwables.team_trigger_claymore,
		            "host startup enables same-team claymore triggers for mp_attributes 0x8000"))
			return false;
	}

	{
		w::World world;
		world.throwables.team_trigger_claymore = true;
		inmatch::HostOwner owner;
		owner.ctx.world = &world;
		inmatch::HostConfig cfg;
		cfg.config.mp_attributes = 0x3A06u;
		cfg.socket_mode = inmatch::SocketMode::Socketless;
		inmatch::start_host_session(owner, cfg);
		if (!expect(!world.throwables.team_trigger_claymore,
		            "host startup disables same-team claymore triggers for default mp_attributes 0x3A06"))
			return false;
	}

	return true;
}

// The advertised NoTracers mission attribute (rules word bit 0) feeds the authoritative
// tracer-visual gate the same way [orig: g_RulesFlags @ 0x24D1E34 & 1 @ 0x4ec41f].
bool run_host_startup_maps_no_tracers_rule() {
	{
		w::World world;
		inmatch::HostOwner owner;
		owner.ctx.world = &world;
		inmatch::HostConfig cfg;
		cfg.config.mp_attributes = 0x0001u;
		cfg.socket_mode = inmatch::SocketMode::Socketless;
		inmatch::start_host_session(owner, cfg);
		if (!expect(world.round_sim.no_tracers_rule,
		            "host startup arms the NoTracers rule for mp_attributes 0x0001"))
			return false;
	}

	{
		w::World world;
		world.round_sim.no_tracers_rule = true;
		inmatch::HostOwner owner;
		owner.ctx.world = &world;
		inmatch::HostConfig cfg;
		cfg.config.mp_attributes = 0x3A06u;
		cfg.socket_mode = inmatch::SocketMode::Socketless;
		inmatch::start_host_session(owner, cfg);
		if (!expect(!world.round_sim.no_tracers_rule,
		            "host startup clears the NoTracers rule for default mp_attributes 0x3A06"))
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
	w::AiSystem &ai = world.ai;
	world.registry.configure_pool(0, 16);

	inmatch::HostOwner owner;
	inmatch::test::bring_up_host(owner.ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E112u);
	owner.ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 31000};
	inmatch::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<ns::UdpSessionTransport>(
			ns::UdpSessionTransport::Role::Host);

	inmatch::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.connection_id = inmatch::kFirstJoinerDcb;
	conn.player_name = "LateJoiner";
	conn.self_id_seen = true;
	conn.phase = inmatch::ConnectionPhase::PendingSpawn;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = ns::TransportMode::Client;
	conn.reply.roster_pushed = true;
	conn.reply.roster_completed_tick = 0;
	conn.burst.sync_state = 4;
	conn.burst.world_stream_phase = 8;
	conn.burst.loadout_received = true;
	conn.reply.mission_status_received = true; // the stock client's 0x0B, the bundle trigger
	conn.burst.entity_batch_count = 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	owner.ctx.np_protocol.next_connection_id = inmatch::kFirstJoinerDcb + 1;
	// The fixture starts on the boundary after its already-pushed roster. Initial
	// world records intentionally cannot share the roster's send tick.
	owner.now_tick = 1;

	HostPumpHookProbe hook{&owner, &world, &ai, peer};
	FirstLogicTickProbe logic_probe;
	logic_probe.hook = &hook;
	world.add_system(&logic_probe);

	// The pending-player spawn pump admits on the match's periodic second: this
	// pump is the one that follows it. [orig: CNapiServer_ProcessPendingPlayerSpawns
	//  @0x4C8DC0, called from Server_TickUpdate @0x51DBFD inside the reload-62 block]
	world.match.advance_tick(world);
	NullDatagramSocket sock;
	inmatch::host_session_pump(owner, sock, &observe_host_before_server_tick, &hook);

	inmatch::NapiNPConnection *remote = nullptr;
	for (inmatch::NapiNPConnection &candidate : owner.ctx.np_protocol.connection_list) {
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
//  host side NapiNPServerMsg_AnimChecksumRequest @0x501D40 (discard @0x501d71 + counter
//  reset @0x501d79), NapiNPServerMsg_0x03D @0x500EC0, NapiNPServerMsg_ValidateTimeSync @0x502210]
bool build_retail_class8_charattr_table(
		inmatch::CharAttrChallengeTable &table) {
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
	return inmatch::parse_charattr_challenge_table(
			reinterpret_cast<const uint8_t *>(source.data()),
			source.size(), table);
}

// All sixteen sections present, every property S2C 0x41 can clear nonzero and distinct per
// row, so a missed id or a wrong offset leaves a surviving dword the assertions can see.
bool build_full_charattr_table(inmatch::CharAttrChallengeTable &table) {
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
	return inmatch::parse_charattr_challenge_table(
			reinterpret_cast<const uint8_t *>(source.data()),
			source.size(), table);
}

bool run_charattr_challenge_table_matches_retail() {
	inmatch::CharAttrChallengeTable table;
	if (!expect(build_retail_class8_charattr_table(table),
			"charattr parser accepts the authoritative CHARACTER syntax")) {
		return false;
	}

	inmatch::CharAttrChallengeRow expected{};
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

	const inmatch::CharAttrChallengeRow *row =
			inmatch::find_charattr_challenge_row(table, 8);
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
	if (!expect(inmatch::find_charattr_challenge_row(table, 0) == nullptr &&
	                    inmatch::find_charattr_challenge_row(table, 17) == nullptr &&
	                    inmatch::find_charattr_challenge_row(table, 255) == nullptr,
			"class 0 and wrapped out-of-range classes fail the embedded-id check")) {
		return false;
	}

	// Enumeration stops on the first missing section even if a later section
	// exists in the file.
	static constexpr std::string_view skipped =
			"[CHARACTER1]\nSTEALTH=1\n[CHARACTER3]\nSTEALTH=3\n";
	inmatch::CharAttrChallengeTable gap_table;
	if (!expect(inmatch::parse_charattr_challenge_table(
				reinterpret_cast<const uint8_t *>(skipped.data()),
				skipped.size(), gap_table) &&
	                    inmatch::find_charattr_challenge_row(gap_table, 1) != nullptr &&
	                    inmatch::find_charattr_challenge_row(gap_table, 3) == nullptr,
			"first missing CHARACTER section terminates retail enumeration")) {
		return false;
	}

	inmatch::CharAttrChallengeTable empty_table;
	empty_table.rows[0].fill(0xFF);
	const inmatch::CharAttrChallengeTable all_zero{};
	if (!expect(!inmatch::parse_charattr_challenge_table(
				nullptr, 0, empty_table) &&
	                    empty_table.rows == all_zero.rows,
			"missing/empty source clears the table and leaves every slot inactive")) {
		return false;
	}

	inmatch::CharAttrChallengeTable cleared = table;
	inmatch::clear_charattr_challenge_property(cleared, 5);
	inmatch::CharAttrChallengeRow expected_cleared = expected;
	std::fill(expected_cleared.begin() + 20, expected_cleared.begin() + 24, 0);
	if (!expect(*inmatch::find_charattr_challenge_row(cleared, 8) == expected_cleared,
			"property 5 clears exactly RECOIL_MUTE across the checksum row")) {
		return false;
	}
	// The nine live property ids and the row offsets they zero.
	// [orig: AnimMap_SetSlotProperty @0x412890 -- cases 0/2/3/4/5/6/7/8/9]
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
	inmatch::CharAttrChallengeTable full;
	if (!expect(build_full_charattr_table(full),
			"sixteen CHARACTER sections with every clearable property set")) {
		return false;
	}
	constexpr uint32_t kClearSeed = 0x3D5D0000u;
	for (const ClearCase &c : kClearMap) {
		inmatch::CharAttrChallengeTable mutated = full;
		inmatch::clear_charattr_challenge_property(mutated, c.property_id);
		bool rows_match = true;
		const auto field = static_cast<std::ptrdiff_t>(c.offset);
		for (std::size_t r = 0; r < inmatch::kCharAttrChallengeRowCount; ++r) {
			inmatch::CharAttrChallengeRow expected_row = full.rows[r];
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
		const inmatch::CharAttrChallengeRow *before_row =
				inmatch::find_charattr_challenge_row(full, 8);
		const inmatch::CharAttrChallengeRow *after_row =
				inmatch::find_charattr_challenge_row(mutated, 8);
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
		inmatch::CharAttrChallengeTable untouched = full;
		inmatch::clear_charattr_challenge_property(untouched, no_op_id);
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
	inmatch::JoinerConnection joiner("Quartet", [&now_ms] { return now_ms; });
	joiner.seed_in_match(0x77889900u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, 0x14B9);

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	// The host fires all four together in ONE datagram, exactly as captured.
	const std::vector<uint8_t> quartet = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x39, {0x5D, 0x3D, 0x00, 0x00}), // CRC seed
					make_protocol_message(0x42, {0x00, 0x00}),             // input-state flags
					make_protocol_message(0x43, {0x11, 0x22, 0x33, 0x44}), // server stamp
					make_protocol_message(0x68, {0x32, 0x00, 0x00, 0x00}), // page start 50
			});
	const inmatch::JoinerConnection::PollResult r =
			joiner.handle_datagram(quartet.data(), quartet.size());
	if (!expect(r.outbound.empty() && r.queued_send_messages.size() == 3,
			"periodic replies remain semantic until the client send boundary"))
		return false;
	const auto queued_reliability = [&](uint8_t tag) {
		for (const ProtocolMessage &message : r.queued_send_messages) {
			if (message.tag == tag) return message.reliable;
		}
		return false;
	};
	if (!expect(queued_reliability(0x1C) && queued_reliability(0x08) &&
				!queued_reliability(0x3D),
			"quartet replies retain 0x1C/0x08 but send loaded-model 0x3D once"))
		return false;
	const std::vector<std::vector<uint8_t>> framed_replies =
			joiner.frame_messages(r.queued_send_messages);
	if (!expect(framed_replies.size() == 1,
			"periodic replies from one receive boundary batch into one session packet"))
		return false;
	if (!expect(joiner.retained_outbound_depth() == 2,
			"mixed quartet reply retains only reliable C2S 0x1C/0x08 siblings"))
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
	inmatch::CharAttrChallengeTable charattr;
	if (!expect(build_retail_class8_charattr_table(charattr),
			"build production charattr challenge table")) {
		return false;
	}
	const inmatch::CharAttrChallengeRow class8_row =
			*inmatch::find_charattr_challenge_row(charattr, 8);
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
	const inmatch::JoinerConnection::PollResult active_result =
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
	const inmatch::JoinerConnection::PollResult cleared_result =
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
	inmatch::CharAttrChallengeRow cleared_row = class8_row;
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
	const inmatch::JoinerConnection::PollResult ordered_result =
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
	inmatch::JoinerConnection joiner("ReverseRtt", [] { return uint64_t{0x55667788u}; });
	joiner.seed_in_match(0x10203040u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> probe = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x57, {0x44, 0x33, 0x22, 0x11, 0x01})});
	const inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(probe.data(), probe.size());
	if (!expect(result.outbound.empty() && result.queued_send_messages.size() == 1,
			"S2C 0x57 flag=1 queues one C2S response"))
		return false;
	if (!expect(!result.queued_send_messages[0].reliable,
			"reactive C2S 0x2C uses retail's one-send queue parameter"))
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
			"reverse RTT response echoes the timestamp and clears the flag") &&
			expect(joiner.retained_outbound_depth() == 0,
					"reactive C2S 0x2C is absent from NACK retention");
}

// The pong of our own C2S 0x2C (S2C 0x57 with the echo flag CLEAR) lands the
// completed round trip in the ten-entry ring and the current-ping word; the
// ring's mean counts every slot, zero-initialized ones included, and nothing
// is queued back [orig: NapiNPClientMsg_0x057_RTT @0x432280;
// CNetStats_GetAveragePing @0x4C2750].
bool run_rtt_pong_fills_the_client_ring() {
	const std::string client_scrk = "CLIENT-PONG-RTT-SCRK";
	const std::string server_scrk = "SERVER-PONG-RTT-SCRK";
	inmatch::JoinerConnection joiner("PongRtt", [] { return uint64_t{0x55667788u}; });
	joiner.seed_in_match(0x10203040u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	if (!expect(joiner.client_ping_ms() == 0 && joiner.client_average_ping_ms() == 0,
			"a fresh connection has measured nothing"))
		return false;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	// Our ping's stamp was 0x55667000: the round trip is 0x788 ms.
	const std::vector<uint8_t> pong = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x57, {0x00, 0x70, 0x66, 0x55, 0x00})});
	const inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(pong.data(), pong.size());
	if (!expect(result.outbound.empty() && result.queued_send_messages.empty(),
			"a flag=0 0x57 queues nothing back"))
		return false;
	if (!expect(joiner.client_ping_ms() == 0x788u &&
					joiner.client_average_ping_ms() == 0x788u / 10u,
			"the pong lands the round trip and the ten-slot mean"))
		return false;
	const std::vector<uint8_t> second = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x57, {0x00, 0x77, 0x66, 0x55, 0x00})});
	(void)joiner.handle_datagram(second.data(), second.size());
	return expect(joiner.client_ping_ms() == 0x88u &&
					joiner.client_average_ping_ms() == (0x788u + 0x88u) / 10u,
			"the second pong advances the ring index and the mean");
}

// The outer connection ping (S2C 0x85, `[u32 CK][WR][MS]` under the session
// NWU key): WR set answers with a 0x45 keyed by the server's SK carrying the
// same MS and WR clear; WR clear lands the round trip; a foreign key is
// dropped [orig: Nwu_HandlePing @0x623A70 -> CNapiNPConnection_SendPing
// @0x61DF00].
bool run_server_ping_answers_and_measures() {
	const std::string client_scrk = "CLIENT-PING-SCRK";
	const std::string server_scrk = "SERVER-PING-SCRK";
	uint64_t now_ms = 5000;
	inmatch::JoinerConnection joiner("OuterPing", [&now_ms] { return now_ms; });
	joiner.seed_in_match(0x10203040u, 7u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	const auto ping_body = [](uint32_t key, uint8_t wr, uint32_t ms) {
		std::vector<uint8_t> body = {
				static_cast<uint8_t>(key), static_cast<uint8_t>(key >> 8),
				static_cast<uint8_t>(key >> 16), static_cast<uint8_t>(key >> 24),
				'W', 'R', 0, 1, 0, wr,
				'M', 'S', 0, 4, 0,
				static_cast<uint8_t>(ms), static_cast<uint8_t>(ms >> 8),
				static_cast<uint8_t>(ms >> 16), static_cast<uint8_t>(ms >> 24),
		};
		return body;
	};
	// A request keyed by our CK (7): the pong goes out and the reap clock is
	// re-stamped at the receive.
	now_ms = 5500;
	const std::vector<uint8_t> request =
			nw_encode_outbound(0x85, ping_body(7u, 1, 0xAABBCCDDu));
	inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(request.data(), request.size());
	// SendPing writes the pong to the socket from inside the receive pump
	// [orig: Nwu_HandlePing -> CNapiNPConnection_SendPing @0x623c6f].
	if (!expect(result.immediate_outbound.size() == 1 && result.outbound.empty(),
			"a keyed WR ping draws one reply datagram, sent at once"))
		return false;
	uint8_t opcode = 0;
	std::vector<uint8_t> reply;
	if (!expect(nw_decode_inbound(result.immediate_outbound[0].data(),
						result.immediate_outbound[0].size(), opcode, reply) &&
					opcode == 0x45 && reply == ping_body(0x10203040u, 0, 0xAABBCCDDu),
			"the reply is a 0x45 keyed by the server SK, WR clear, the same MS"))
		return false;
	if (!expect(joiner.milliseconds_since_last_receive() == 0,
			"the ping refreshed the reap clock"))
		return false;
	// The host's pong of our ping: rtt = now - MS.
	now_ms = 6000;
	const std::vector<uint8_t> pong = nw_encode_outbound(0x85, ping_body(7u, 0, 5750u));
	result = joiner.handle_datagram(pong.data(), pong.size());
	if (!expect(result.outbound.empty() && result.immediate_outbound.empty() &&
					joiner.session_ping_ms() == 250u,
			"a WR-clear ping lands the outer round trip"))
		return false;
	// A foreign receiver key is dropped silently.
	const std::vector<uint8_t> foreign = nw_encode_outbound(0x85, ping_body(9u, 1, 1u));
	result = joiner.handle_datagram(foreign.data(), foreign.size());
	return expect(result.outbound.empty() && result.immediate_outbound.empty() &&
					joiner.session_ping_ms() == 250u,
			"a ping keyed by another connection is ignored");
}

// Every decoded S2C 0x0F re-queues the six-message completion burst; there is
// no once-per-session latch [orig: NapiNPClientMsg_0x00F @0x42e5af..0x42e6ab].
bool run_world_state_load_bursts_on_every_0x0f() {
	const std::string client_scrk = "CLIENT-0F-BURST-SCRK";
	const std::string server_scrk = "SERVER-0F-BURST-SCRK";
	inmatch::JoinerConnection joiner("Burst");
	joiner.seed_in_match(0x10203040u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	// 23-byte fixed header, the 128 pool dwords, zero waypoint/location counts.
	const std::vector<uint8_t> body(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	const auto burst_count = [](const inmatch::JoinerConnection::PollResult &r) {
		int loadout_requests = 0;
		for (const ProtocolMessage &m : r.queued_send_messages)
			if (m.tag == c2s::LOADOUT_REQUEST) ++loadout_requests;
		return loadout_requests;
	};
	const std::vector<uint8_t> first = frame_server_session(
			server_tx, server_scrk, 1u, {make_protocol_message(0x0F, body)});
	const inmatch::JoinerConnection::PollResult r1 =
			joiner.handle_datagram(first.data(), first.size());
	if (!expect(burst_count(r1) == 1, "the first 0x0F queues its completion burst"))
		return false;
	const std::vector<uint8_t> second = frame_server_session(
			server_tx, server_scrk, 1u, {make_protocol_message(0x0F, body)});
	const inmatch::JoinerConnection::PollResult r2 =
			joiner.handle_datagram(second.data(), second.size());
	return expect(burst_count(r2) == 1, "a second 0x0F queues the burst again");
}

// The connection indicators on a joiner (D-HUD-38): the frame step under the
// session, the three-second receive silence's incoming flag, a received 0x84
// that named a sequence (outgoing), and the S2C 0x0F clear with its 10 s
// hold-off [orig: Game_ProcessMainFrame @0x52668d; Client_ProcessNetworkFrame
// @0x42c1f8..0x42c21e; NapiNP_HandleResendList cb_client_3 @0x623a24;
// NapiNPClientMsg_0x00F @0x42e660; CNetQuality_SetLinkErrorFlag @0x4c34f0].
bool run_connection_indicators_on_a_joiner() {
	const std::string client_scrk = "CLIENT-NETQ-SCRK";
	const std::string server_scrk = "SERVER-NETQ-SCRK";
	uint64_t now_ms = 50000;
	inmatch::ClientRuntime client("NetQ", [&now_ms] { return now_ms; });
	client.seed_session(0x10203040u, 1u, client_scrk, server_scrk,
	                    1, 0, 0x0002, w::kPlayerInfantryTypeId,
	                    0, 0x00100000u, /*replay_mode=*/false);
	const hud::NetQualityIndicators &q = client.net_quality_indicators();
	(void)client.Client_ProcessNetworkFrame(1);
	if (!expect(q.link_error_bits == 0 && q.link_error_countdown == 0,
			"a freshly seeded joiner holds no link error"))
		return false;
	// Two whole seconds (2999 ms) of silence raise nothing; the third does.
	now_ms += 2999;
	(void)client.Client_ProcessNetworkFrame(2);
	if (!expect(q.link_error_bits == 0, "2999 ms of receive silence raise no flag"))
		return false;
	now_ms += 1;
	(void)client.Client_ProcessNetworkFrame(3);
	if (!expect(q.link_error_bits == hud::kNetLinkErrorIncoming &&
					q.link_error_countdown == hud::kNetLinkErrorFrames && q.link_error_alpha == 0,
			"three seconds of receive silence raise the incoming flag after the step"))
		return false;
	// The next frame steps first (alpha 64), then the silence re-raises.
	(void)client.Client_ProcessNetworkFrame(4);
	if (!expect(q.link_error_alpha == 64 && q.link_error_countdown == hud::kNetLinkErrorFrames,
			"the step runs ahead of the re-raised flag"))
		return false;
	// A 0x84 naming a sequence is the outgoing flag, at its datagram.
	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(1u, {1}, resend_body), "encode a 0x84 body"))
		return false;
	const std::vector<uint8_t> resend =
			nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(resend_body));
	client.receive(resend.data(), resend.size());
	(void)client.Client_ProcessNetworkFrame(5);
	if (!expect(q.link_error_bits == (hud::kNetLinkErrorIncoming | hud::kNetLinkErrorOutgoing),
			"a 0x84 that names a sequence raises the outgoing flag"))
		return false;
	// Any S2C 0x0F clears both and holds new flags off for 10 s; its receive
	// also ends the silence.
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> world_state(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	const std::vector<uint8_t> load = frame_server_session(
			server_tx, server_scrk, 1u, {make_protocol_message(0x0F, world_state)});
	client.receive(load.data(), load.size());
	(void)client.Client_ProcessNetworkFrame(6);
	const uint32_t cleared_at = client.state().local_clock_ms;
	if (!expect(q.link_error_bits == 0 && q.link_error_countdown == 0 &&
					q.link_error_alpha == 0 &&
					q.flag_cooldown_until_ms == cleared_at + hud::kNetLinkErrorCooldownMs,
			"a 0x0F clears the link errors and arms the 10 s hold-off"))
		return false;
	client.receive(resend.data(), resend.size());
	(void)client.Client_ProcessNetworkFrame(7);
	if (!expect(q.link_error_bits == 0, "inside the hold-off a 0x84 raises nothing"))
		return false;
	uint32_t tick = 8;
	while (client.state().local_clock_ms < q.flag_cooldown_until_ms)
		(void)client.Client_ProcessNetworkFrame(tick++);
	client.receive(resend.data(), resend.size());
	(void)client.Client_ProcessNetworkFrame(tick++);
	return expect(q.link_error_bits == hud::kNetLinkErrorOutgoing,
			"past the hold-off the next 0x84 raises the outgoing flag again");
}

// The listen host's own client (D-HUD-38): the host role's level lands before
// the step, its server protocol's link errors after it, and nothing steps off
// a networked session [orig: CNetQuality_SetLevel @0x52659b ahead of
// CNetQuality_UpdateIndicators @0x52668d under `is_in_session` @0x526686].
bool run_connection_indicators_on_the_host_client() {
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host_view(host_loop);
	const hud::NetQualityIndicators &q = host_view.net_quality_indicators();
	host_view.set_net_quality_level(3);
	(void)host_view.Client_ProcessNetworkFrame(1);
	if (!expect(q.level == 3 && q.level3_alpha == 0,
			"single player (no session) never steps the indicators"))
		return false;
	host_view.view().set_mp_session(true);
	(void)host_view.Client_ProcessNetworkFrame(2);
	if (!expect(q.level3_alpha == 4 && host_view.net_quality_level() == 3,
			"a networked host's own client steps toward its level"))
		return false;
	host_view.raise_net_quality_link_errors(inmatch::kNetQualityLinkErrorOutgoing |
			inmatch::kNetQualityLinkErrorIncoming);
	return expect(q.link_error_bits == 3 && q.link_error_countdown == hud::kNetLinkErrorFrames,
			"the server protocol's callbacks raise both flags");
}

// The C2S 0x0D chat producer: `[u8 channel][cstr]` with the `<...>` strip and
// the 59-character cut, `Flooded` for a repeat within 0x500 main frames of
// its entry, and nothing for the non-peer channels 4/5 [orig:
// Chat_SendGlobalMessage @0x49A6B0; Chat_CheckFloodControl @0x498F60 —
// `dword_A8705C - entry <= 0x500` @0x499028; Chat_StripHtmlTags @0x4983F0].
bool run_chat_uplink_api() {
	using Result = hud::ChatSendResult;
	const std::string client_scrk = "CLIENT-CHAT-SCRK";
	const std::string server_scrk = "SERVER-CHAT-SCRK";
	uint64_t now_ms = 10000;
	inmatch::ClientRuntime client("Chatter", [&now_ms] { return now_ms; });
	client.seed_session(0x10203040u, 1u, client_scrk, server_scrk,
	                    1, 0, 0x0002, w::kPlayerInfantryTypeId,
	                    0, 0x00100000u, /*replay_mode=*/false);
	const auto send = [&client](uint8_t channel, std::string text, uint32_t frame) {
		return client.queue_chat_message(channel, text, frame);
	};
	uint32_t frame = 1000;
	if (!expect(send(2, "hi <b>there</b>", frame) == Result::Sent, "a team line is accepted"))
		return false;
	// The window counts main frames, not milliseconds: wall time alone never
	// reopens it.
	now_ms += 100000;
	if (!expect(send(2, "hi <b>there</b>", frame + 0x500) == Result::Flooded,
			"the same line inside the flood window is flooded"))
		return false;
	if (!expect(send(4, "all", frame) == Result::Refused,
			"the non-peer red channel sends nothing from a joiner"))
		return false;
	if (!expect(send(2, "", frame) == Result::Refused, "an empty line sends nothing"))
		return false;
	const std::vector<std::vector<uint8_t>> outbound = client.Client_ProcessNetworkFrame(1);
	std::vector<uint8_t> chat_body;
	for (const std::vector<uint8_t> &datagram : outbound) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(datagram, client_scrk, header, messages)) continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == c2s::CHAT_MESSAGE) chat_body = m.payload;
	}
	const std::vector<uint8_t> expected = {2, 'h', 'i', ' ', 't', 'h', 'e', 'r', 'e', 0};
	if (!expect(chat_body == expected, "the line rides out as [channel][stripped cstr]"))
		return false;
	// Past the window the same line goes again; a long line is cut to 59 first.
	frame += 0x501;
	if (!expect(send(2, "hi <b>there</b>", frame) == Result::Sent,
			"the same line past the flood window is accepted"))
		return false;
	std::string long_line(70, 'x');
	if (!expect(client.queue_chat_message(1, long_line, frame) == Result::Sent &&
					long_line.size() == 59,
			"a long global line is accepted, cut to 59 in place"))
		return false;
	const std::vector<std::vector<uint8_t>> again = client.Client_ProcessNetworkFrame(2);
	std::size_t long_body = 0;
	for (const std::vector<uint8_t> &datagram : again) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(datagram, client_scrk, header, messages)) continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == c2s::CHAT_MESSAGE && !m.payload.empty() && m.payload[0] == 1)
				long_body = m.payload.size();
	}
	return expect(long_body == 1 + 59 + 1, "a long line is cut to 59 characters");
}

// The listen host's own client is a session peer too: its talk line rides
// its loopback to its own server as the same C2S 0x0D [orig: the senders'
// QueueReliableMessage(0xD) over transport mode 1]; the local and crew keys
// refuse on the death screen alone.
bool run_host_chat_uplink() {
	using Result = hud::ChatSendResult;
	ns::LoopbackChannel host_loop;
	inmatch::ClientRuntime host(host_loop);
	std::string line = "hello <i>all</i>";
	if (!expect(host.queue_chat_message(13, line, 50) == Result::Sent,
			"the host's local line is sent"))
		return false;
	ns::Datagram dg;
	if (!expect(host_loop.host_recv(dg) && dg.tag == c2s::CHAT_MESSAGE, "it lands on the loopback"))
		return false;
	const std::vector<uint8_t> want = {13, 'h', 'e', 'l', 'l', 'o', ' ', 'a', 'l', 'l', 0};
	return expect(dg.body == want, "the host's body is [13][stripped cstr]");
}

// The client window of CNetQuality: a ring of 400 ms round trips scores the
// ping term 102, and once five 62-frame samples fill the window the combined
// scalar folds to level 2 [orig: CNetQuality_UpdateMetrics @0x4C52C0;
// the Game_ProcessMainFrame countdown; CNetQuality_SetLevel @0x4C3060].
bool run_client_quality_level_folds_the_ping_ring() {
	const std::string client_scrk = "CLIENT-QUALITY-SCRK";
	const std::string server_scrk = "SERVER-QUALITY-SCRK";
	inmatch::ClientRuntime client("Quality", [] { return uint64_t{100000}; });
	client.seed_session(0x10203040u, 1u, client_scrk, server_scrk,
	                    1, 0, 0x0002, w::kPlayerInfantryTypeId,
	                    0, 0x00100000u, /*replay_mode=*/false);
	// A healthy measured frame rate (the session's FR counter): the
	// frame-pressure term scores its floor 1, so the ping term decides.
	client.set_observed_frame_rate(62);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	std::vector<ProtocolMessage> pongs;
	for (int i = 0; i < 10; ++i) {
		// stamp = now - 400 -> a 400 ms round trip.
		const uint32_t stamp = 100000u - 400u;
		pongs.push_back(make_protocol_message(0x57,
				{static_cast<uint8_t>(stamp), static_cast<uint8_t>(stamp >> 8),
				 static_cast<uint8_t>(stamp >> 16), static_cast<uint8_t>(stamp >> 24), 0x00}));
	}
	const std::vector<uint8_t> datagram =
			frame_server_session(server_tx, server_scrk, 1u, pongs);
	client.receive(datagram.data(), datagram.size());
	(void)client.Client_ProcessNetworkFrame(1);
	if (!expect(client.client_average_ping_ms() == 400 && client.net_quality_level() == 0,
			"ten pongs fill the ring; the level waits for the 62-frame fold"))
		return false;
	for (uint32_t tick = 2; tick <= 62; ++tick) (void)client.Client_ProcessNetworkFrame(tick);
	if (!expect(client.net_quality_level() == 1,
			"the first sample truncates to 102/5 -> level 1"))
		return false;
	for (uint32_t tick = 63; tick <= 62 * 5; ++tick) (void)client.Client_ProcessNetworkFrame(tick);
	return expect(client.net_quality_level() == 2 && client.state().net.level == 2 &&
					client.state().net.ping_ms == 400,
			"five samples of 102 fold to level 2 on the state as well");
}

// The client window's frame-pressure term reads the main loop's FR counter
// [orig: CNetQuality_UpdateMetrics @0x4C5643 g_StatsAvgFps]: with no ping
// samples (term 1) an 8 fps rate scores 256 - 128 = 128 per sample (five
// samples: level 2), and the unmeasured 0 of the first 2 s window (retail's
// mode-init value, the default) scores the ceiling 255 (level 3).
bool run_client_quality_frame_pressure_follows_the_frame_rate() {
	const auto fold = [](int32_t fps, bool set_rate) {
		inmatch::ClientRuntime client("QualityFps", [] { return uint64_t{100000}; });
		client.seed_session(0x10203040u, 1u, "CLIENT-QUALITY-FPS-SCRK", "SERVER-QUALITY-FPS-SCRK",
		                    1, 0, 0x0002, w::kPlayerInfantryTypeId,
		                    0, 0x00100000u, /*replay_mode=*/false);
		if (set_rate) client.set_observed_frame_rate(fps);
		for (uint32_t tick = 1; tick <= 62 * 5; ++tick) (void)client.Client_ProcessNetworkFrame(tick);
		return client.net_quality_level();
	};
	if (!expect(fold(8, true) == 2, "an 8 fps rate samples frame pressure 128 -> level 2"))
		return false;
	if (!expect(fold(62, true) == 1, "a healthy rate leaves every term at the floor -> level 1"))
		return false;
	return expect(fold(0, false) == 3, "an unmeasured rate samples the ceiling 255 -> level 3");
}

// The NovaWorld exit leads the joiner's 62-frame block (D-NET-220): a NovaWorld
// network type with its NWU session in use exits the mission with reason 12 once
// the session's hosting/playing word holds neither 2 nor 3; a playing or hosting
// word, a LAN join and an unused session never do, and the session's own exit
// store (a stop-playing or a punt) lands as is. JoinerRole reports it as a lost
// session [orig: Game_ProcessMainFrame @0x52655d..0x52657c].
bool run_novaworld_exit_on_a_joiner() {
	const auto run = [](const hud::NovaWorldLinkFacts &nw, uint32_t frames) {
		inmatch::ClientRuntime client("NovaWorldExit", [] { return uint64_t{100000}; });
		client.seed_session(0x10203040u, 1u, "CLIENT-NWU-EXIT-SCRK", "SERVER-NWU-EXIT-SCRK",
		                    1, 0, 0x0002, w::kPlayerInfantryTypeId,
		                    0, 0x00100000u, /*replay_mode=*/false);
		client.set_novaworld_link(nw);
		for (uint32_t tick = 1; tick <= frames; ++tick) (void)client.Client_ProcessNetworkFrame(tick);
		return client.mission_exit_reason();
	};
	hud::NovaWorldLinkFacts verified;
	verified.novaworld = true;
	verified.nwu_in_use = true;
	verified.nwu_session_flags = 0x1A;
	verified.nwu_session_role = 1;
	if (!expect(run(verified, 61) == 0, "the exit waits for the 62-frame block"))
		return false;
	if (!expect(run(verified, 62) == inmatch::kMissionExitNovaWorld,
			"a verified-only word exits with 12 on the block"))
		return false;
	hud::NovaWorldLinkFacts playing = verified;
	playing.nwu_session_flags = 0x0A;
	playing.nwu_session_role = 3;
	if (!expect(run(playing, 62 * 3) == 0, "a playing word holds the match"))
		return false;
	hud::NovaWorldLinkFacts hosting = verified;
	hosting.nwu_session_role = 2;
	if (!expect(run(hosting, 62 * 3) == 0, "a hosting word holds the match"))
		return false;
	hud::NovaWorldLinkFacts lan = verified;
	lan.novaworld = false;
	if (!expect(run(lan, 62 * 3) == 0, "a LAN join never takes the NovaWorld exit"))
		return false;
	hud::NovaWorldLinkFacts unused = verified;
	unused.nwu_in_use = false;
	if (!expect(run(unused, 62 * 3) == 0, "an unused NWU session never takes it"))
		return false;
	inmatch::ClientRuntime stored("NovaWorldExitStore", [] { return uint64_t{100000}; });
	stored.set_mission_exit_reason(inmatch::kMissionExitNovaWorld);
	return expect(stored.mission_exit_reason() == inmatch::kMissionExitNovaWorld,
			"the NWU session's own exit store lands as is");
}

// The exit reason an in-match disconnect stores and the post-mission router's verdict on it
// [orig: CNapiNetwork_OnDisconnectedFromServer @0x4c63d0 (the dc == 2 DPC switch);
//  PostMenu_RouteMissionExit @0x568460]: class-2 records map their DPC (1 / 33 / an unlisted
// code quit, 34 -> 5, 35 -> 7, 36..45 -> 9..18, 46 / 49 -> 19), any other class or a zero DPC
// stores nothing; reasons 2, 5..7, 9..20 drop the NovaWorld session (5..7 / 20 with a gameerr
// text, 9..19 with the disconnect text), every other reason keeps it on a NovaWorld session.
bool run_mission_exit_routes() {
	const auto dpc = [](uint32_t dc, uint32_t code) {
		DisconnectEvent event;
		event.dc = dc;
		event.dpc = code;
		return inmatch::mission_exit_reason_for_disconnect(event);
	};
	if (!expect(dpc(2, 0) == inmatch::kMissionExitNone, "a zero DPC stores nothing") ||
			!expect(dpc(3, 40) == inmatch::kMissionExitNone, "a non-description class stores nothing") ||
			!expect(dpc(2, 1) == inmatch::kMissionExitQuit, "DPC 1 queues the quit") ||
			!expect(dpc(2, 33) == inmatch::kMissionExitQuit, "DPC 33 (a punt) queues the quit") ||
			!expect(dpc(2, 50) == inmatch::kMissionExitQuit, "an unlisted DPC queues the quit") ||
			!expect(dpc(2, 34) == inmatch::kMissionExitCdTrouble, "DPC 34 -> 5") ||
			!expect(dpc(2, 35) == inmatch::kMissionExitPirate, "DPC 35 -> 7") ||
			!expect(dpc(2, 36) == 9 && dpc(2, 39) == inmatch::kMissionExitNovaWorld &&
							dpc(2, 45) == 18,
					"DPC 36..45 -> 9..18") ||
			!expect(dpc(2, 46) == 19 && dpc(2, 49) == 19, "DPC 46 / 49 -> 19"))
		return false;
	const auto route = [](int32_t reason, bool novaworld) {
		return inmatch::route_mission_exit(reason, novaworld);
	};
	using E = inmatch::PostMissionError;
	if (!expect(route(1, true).keep_session && route(1, true).error == E::None,
			"a quit keeps the NovaWorld session for the NovaWorld menu") ||
			!expect(route(3, true).keep_session && route(4, true).keep_session,
					"a map cycle and a round-over keep it too") ||
			!expect(!route(1, false).keep_session, "a LAN session has nothing to keep") ||
			!expect(!route(2, true).keep_session && route(2, true).error == E::None,
					"a reset drops it without a text") ||
			!expect(route(5, true).error == E::CdTrouble && route(6, true).error == E::System &&
							route(7, true).error == E::Pirate &&
							route(20, true).error == E::BadMission && !route(20, true).keep_session,
					"5 / 6 / 7 / 20 store their gameerr text and drop the session") ||
			!expect(std::string(inmatch::post_mission_error_key(E::Pirate)) == "STRE_PIRATE",
					"the gameerr key") ||
			!expect(route(12, true).error == E::DisconnectReason && !route(12, true).keep_session &&
							route(9, false).error == E::DisconnectReason &&
							route(19, true).error == E::DisconnectReason,
					"9..19 show the disconnect text and drop the session"))
		return false;
	// A joiner's latched class-2 record reads as its mapped reason.
	inmatch::ClientRuntime stored("MissionExitStore", [] { return uint64_t{100000}; });
	return expect(stored.mission_exit_reason() == inmatch::kMissionExitNone,
			"a healthy joiner has stored nothing");
}

bool run_direct_uplink_framing_is_transient() {
	const std::string client_scrk = "CLIENT-DIRECT-UPLINK-SCRK";
	inmatch::JoinerConnection joiner("DirectUplink");
	joiner.seed_in_match(
			0x31415926u, 1u, client_scrk, "SERVER-DIRECT-UPLINK-SCRK",
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	PlayerExtendedUplink uplink;
	uplink.pos_x = 0x00100000u;
	uplink.pos_y = 0x00200000u;
	uplink.pos_z = 0x00300000u;
	const std::vector<uint8_t> datagram = joiner.frame_c2s_uplink(
			0x0002, w::kPlayerInfantryTypeId, uplink);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(decode_client_session(datagram, client_scrk, header, messages) &&
	                    messages.size() == 1 && messages[0].tag == 0x0C,
	            "public direct-uplink seam still emits the retail C2S 0x0C wire"))
		return false;
	return expect(joiner.retained_outbound_depth() == 0 &&
	                      joiner.send_flush_counter() == 1,
	            "public direct-uplink C2S 0x0C is one-send and completes one boundary");
}

bool run_network_spawn_does_not_mutate_loaded_model_snapshot() {
	const std::string client_scrk = "CLIENT-MODEL-SNAPSHOT-SCRK";
	const std::string server_scrk = "SERVER-MODEL-SNAPSHOT-SCRK";
	inmatch::ClientRuntime client("ModelSnapshot", [] { return uint64_t{1000}; });
	client.seed_session(
			0x20304050u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	client.set_loaded_model_challenge_snapshot(
			{0xA1B2C3D4u, 0u, 0u});

	constexpr uint16_t kFreshHandle = 0x0123;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
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
	inmatch::ClientRuntime client("PickSplit", [] { return uint64_t{2000}; });
	client.seed_session(
			0x30405060u, 1u, client_scrk, server_scrk,
			10, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	if (!expect(client.initial_admission_complete(),
			"a seeded in-match replay starts beyond initial admission"))
		return false;

	// Death first re-enters AwaitDeployPick, matching the UI state that can
	// accept input case 12. Then fill the pick's receive boundary with enough
	// 0x43 requests to make their C2S 0x08 replies split across the 1300-byte
	// send ceiling.
	FrameUpdate death;
	death.mount_handle = 0xFFFF;
	death.health = 0;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> death_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x0A, encode_frame_update(death))});
	client.receive(death_datagram.data(), death_datagram.size());
	(void)client.Client_ProcessNetworkFrame(0);
	if (!expect(client.deployment_pick_pending() &&
				client.initial_admission_complete(),
			"redeployment keeps the initial-admission boundary monotonic"))
		return false;

	std::vector<ProtocolMessage> inbound;
	for (uint32_t i = 0; i < 180; ++i) {
		inbound.push_back(make_protocol_message(
				0x43,
				{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), 0, 0}));
	}
	const std::vector<uint8_t> challenge_burst =
			frame_server_session(server_tx, server_scrk, 1u, inbound);
	client.receive(challenge_burst.data(), challenge_burst.size());
	if (!expect(client.queue_deployment_pick(0xFFFF),
			"split deployment fixture accepts the queued pick"))
		return false;
	const std::vector<std::vector<uint8_t>> outbound =
			client.Client_ProcessNetworkFrame(1);
	if (!expect(outbound.size() >= 2 && !client.is_deployed(),
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
			{make_protocol_message(0x5A, {0x08, 0xFF})});
	client.receive(release.data(), release.size());
	(void)client.Client_ProcessNetworkFrame(2);
	return expect(client.is_deployed(),
			"0x5A covering the pick's actual packet releases redeployment");
}

bool run_unrelated_loadout_cannot_revive_dead_client() {
	const std::string client_scrk = "CLIENT-DEAD-LOADOUT-SCRK";
	const std::string server_scrk = "SERVER-DEAD-LOADOUT-SCRK";
	inmatch::ClientRuntime client("DeadLoadout", [] { return uint64_t{3000}; });
	client.seed_session(
			0x40506070u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	FrameUpdate death;
	death.mount_handle = 0xFFFF;
	death.health = 0;
	const std::vector<uint8_t> death_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x0A, encode_frame_update(death))});
	client.receive(death_datagram.data(), death_datagram.size());
	(void)client.Client_ProcessNetworkFrame(1);
	if (!expect(client.gameplay_gate_open() &&
				!client.authoritative_spawn_released() && !client.is_deployed() &&
				client.deployment_pick_pending(),
			"death closes only authoritative spawn and re-enters the picker"))
		return false;

	// WeaponLoadout_ApplyFromBuffer still performs its unconditional
	// dword_81474C clear. That literal edge must remain incapable of reopening
	// authoritative spawn or accepting the stale positive tail beside it.
	FrameUpdate stale_positive;
	stale_positive.mount_handle = 0xFFFF;
	stale_positive.health = 100;
	const std::vector<uint8_t> unrelated_grant = frame_server_session(
			server_tx, server_scrk, 1u,
			{
					make_protocol_message(0x5A, {0x08, 0xFF}),
					make_protocol_message(
							0x0A, encode_frame_update(stale_positive)),
			});
	client.receive(unrelated_grant.data(), unrelated_grant.size());
	(void)client.Client_ProcessNetworkFrame(2);
	if (!expect(client.gameplay_gate_open() &&
				!client.authoritative_spawn_released() && !client.is_deployed() &&
				client.state().local_health == 100 &&
				client.deployment_release_revision() == 0 &&
				client.authoritative_spawn_release_revision() == 0,
			"unrelated valid 0x5A clears dword without reviving dead gameplay"))
		return false;

	ClientFiredRound shot;
	shot.shooter_handle = 0x0002;
	return expect(!client.queue_fired_round(shot),
			"the effective send predicate rejects dead gameplay after the unrelated grant");
}

// D-NET-235: a queued producer's message leaves at the next open send boundary even when the
// player died in between. Retail's QueueReliableMessage puts it on the connection's list, and
// PumpClientProtocolSend builds whatever is queued; only the 0x2C ping and the 0x0C uplink are
// built behind the deploy gate, and nothing drains the list on death.
// [orig: Client_ProcessNetworkFrame @0x42c3ee..0x42c4a3 (the 0x2C / 0x0C builds behind
//  is_in_session && !is_authority && !dword_81474C && !g_SpawnSuccessGate), the tail
//  @0x42c4b1 -> PumpClientProtocolSend @0x42c4bc in every branch;
//  CNapiNPConnection_DrainMessageQueues @0x625600 runs only at join @0x629dfc / @0x62c26f
//  and destroy @0x62a50a]
bool run_queued_gameplay_survives_a_death_before_the_boundary() {
	const std::string client_scrk = "CLIENT-QUEUED-DEATH-SCRK";
	const std::string server_scrk = "SERVER-QUEUED-DEATH-SCRK";
	inmatch::ClientRuntime client("QueuedDeath", [] { return uint64_t{4000}; });
	client.seed_session(
			0x41516171u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	if (!expect(client.queue_vehicle_detach(0x1007),
			"a deployed joiner queues C2S 0x27 detach"))
		return false;

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	FrameUpdate death;
	death.mount_handle = 0xFFFF;
	death.health = 0;
	const std::vector<uint8_t> death_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x0A, encode_frame_update(death))});
	client.receive(death_datagram.data(), death_datagram.size());
	const std::vector<std::vector<uint8_t>> boundary = client.Client_ProcessNetworkFrame(1);
	if (!expect(!client.is_deployed(), "the death closes the deploy gate this frame"))
		return false;
	bool saw_detach = false;
	bool saw_ping_or_uplink = false;
	for (const std::vector<uint8_t> &datagram : boundary) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(datagram, client_scrk, header, messages)) continue;
		for (const ProtocolMessage &message : messages) {
			saw_detach = saw_detach || message.tag == 0x27;
			saw_ping_or_uplink = saw_ping_or_uplink || message.tag == 0x2C || message.tag == 0x0C;
		}
	}
	return expect(saw_detach && !saw_ping_or_uplink,
			"the queued 0x27 leaves at the boundary; only the 0x2C/0x0C builds sit behind the gate");
}

bool run_live_frame_uses_wall_clock_and_batches_mount_requests() {
	const std::string client_scrk = "CLIENT-LIVE-BATCH-SCRK";
	const std::string server_scrk = "SERVER-LIVE-BATCH-SCRK";
	uint64_t now_ms = 0x11223344u;
	inmatch::ClientRuntime client("LiveBatch", [&now_ms] { return now_ms; });
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
	if (!expect(client.retained_outbound_depth() == 2,
			"first mixed C2S packet retains loadout/attach but prunes RTT"))
		return false;

	// Consecutive live frames remain inside the 120-second receive-silence
	// window; a billion-millisecond clock jump would correctly reap the session
	// before this second producer can run.
	now_ms = 0x11223345u;
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
	                            std::vector<uint8_t>({0x45, 0x33, 0x22, 0x11, 0x01}),
			"C2S 0x27 batches with a fresh RTT ping on the immediately consecutive "
			"deployed frame (the retail 62 counter is not a send gate)"))
		return false;
	if (!expect(client.retained_outbound_depth() == 3,
			"second mixed packet retains detach but prunes its fresh RTT"))
		return false;

	PlayerExtendedUplink uplink;
	uplink.carrier_handle = 0xFFFF;
	const std::vector<std::vector<uint8_t>> uplink_frame =
			client.Client_ProcessNetworkFrame(uplink, 1001u);
	if (!expect(uplink_frame.size() == 1 &&
	                    decode_client_session(
	                            uplink_frame[0], client_scrk, header, messages) &&
	                    messages.size() == 2 && messages[0].tag == 0x2C &&
	                    messages[1].tag == 0x0C,
			"live pose frame carries transient RTT and transient entity uplink"))
		return false;
	return expect(client.retained_outbound_depth() == 3,
	              "C2S 0x2C/0x0C first-send records do not grow NACK retention");
}

bool run_mounted_slot_select_and_reload_producers() {
	const std::string client_scrk = "CLIENT-MOUNTED-SLOT-SCRK";
	const std::string server_scrk = "SERVER-MOUNTED-SLOT-SCRK";
	inmatch::ClientRuntime client("MountedSlot", [] { return uint64_t{0x10203040}; });
	client.seed_session(
			0x55667788u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	if (!expect(client.queue_mounted_weapon_slot_selection(true),
			"a deployed joiner can queue the action-6 parent-slot selector"))
		return false;
	WeaponReload mounted_reload;
	mounted_reload.entity_handle = 0x1007;
	mounted_reload.reload_param = 0xBEEF;
	if (!expect(client.queue_reload_request(mounted_reload),
			"a deployed joiner can address a mounted EWeap reload"))
		return false;

	const std::vector<std::vector<uint8_t>> frame =
			client.Client_ProcessNetworkFrame(1);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(frame.size() == 1 &&
			decode_client_session(frame[0], client_scrk, header, messages) &&
			messages.size() == 3,
			"mounted selector, reload, and RTT share one client frame"))
		return false;
	if (!expect(messages[0].tag == c2s::MOUNTED_WEAPON_SLOT_SELECT &&
			messages[0].payload == std::vector<uint8_t>({1, 0}),
			"action-6 parent selection is exact C2S 0x16 bool-as-i16"))
		return false;
	if (!expect(messages[1].tag == c2s::WEAPON_RELOAD_REQUEST &&
			messages[1].payload ==
					std::vector<uint8_t>({0x07, 0x10, 0xEF, 0xBE}),
			"mounted reload preserves the addressed EWeap handle and combo"))
		return false;
	return expect(messages[2].tag == c2s::RTT_CONSUMED &&
			client.retained_outbound_depth() == 2,
			"both mounted gameplay messages are reliable while RTT is transient");
}

bool run_same_packet_holdoff_keeps_first_admission_boundary_open() {
	constexpr uint32_t kServerKey = 0x31415926u;
	constexpr uint32_t kConnectionId = 7;
	const std::string server_scrk = "SERVER-HOLDOFF-ADMISSION-SCRK";
	inmatch::ClientRuntime client("HoldoffAdmission");

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
	// field-3 period. HandleCSConfigUpdate stores that period without loading
	// the active counter: the earlier join-response reset left the first
	// post-handshake boundary open, and only this open send reloads the period.
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
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
	const std::vector<std::vector<uint8_t>> released =
			client.Client_ProcessNetworkFrame(2);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(released.size() == 2 &&
	                    decode_client_session(
			                    released[0], client_auth.scrk,
			                    header, messages) &&
	                    matches_client_header(
			                    header, kServerKey, 1, 1) &&
	                    messages.empty(),
			"the immediately open post-handshake boundary preserves the admission ACK packet"))
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
			"the immediately open boundary preserves the following JOIN packet and sequence"))
		return false;
	if (!expect(client.send_holdoff_countdown() == 2,
			"the first open boundary re-arms the dictated field-3 period"))
		return false;

	// Decrement-before-gate gives a period N exactly N-1 held frames between
	// open sends. For N=2, frame 3 is the sole held frame and frame 4 opens.
	if (!expect(client.Client_ProcessNetworkFrame(3).empty() &&
	                    client.send_holdoff_countdown() == 1,
			"period two holds exactly one frame after the first open boundary"))
		return false;
	(void)client.Client_ProcessNetworkFrame(4);
	return expect(client.send_holdoff_countdown() == 2,
			"the next boundary opens after exactly N-1 held frames");
}

// The host's CS update dictating send-holdoff field 3 (direction 1, mask 8).
ProtocolMessage cs_send_holdoff_update(uint8_t period) {
	return make_protocol_message(
			0x00, {0x01, 0x08, 0x00, 0x00, 0x00, period, 0x00, 0x00, 0x00}, 0xA0);
}

bool decode_client_resend_list(const std::vector<uint8_t> &datagram, uint32_t server_key,
		std::vector<uint32_t> &requested) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == SESSION_OPCODE_CLIENT_RESEND_LIST &&
			decode_session_resend_list(body.data(), body.size(), server_key, requested);
}

// Under a NovaWorld host's twelve-tick send holdoff the C2S 0x44 still leaves
// on the frame the gap is seen: retail sends it from the client receive pump,
// which runs every frame, while the holdoff gates only PumpClientProtocolSend.
// The latch clears at that pump, so a frame with no new future packet sends
// nothing more; a later future packet behind the same gap asks again.
// [orig: Client_ProcessNetworkFrame — CNapiNetwork_PumpClientProtocolRecv
//  @0x42c228 ahead of the holdoff gate @0x42c3dd; NapiNPProtocol_PumpRecvQueues
//  @0x6269bb..0x6269d6 -> CNapiNPConnection_SendMissingSeqList @0x6269ce;
//  the latch NapiNPProtocol_HandleSessionPacket @0x626c3a]
bool run_missing_sequence_request_leaves_from_the_receive_pump() {
	constexpr uint32_t kServerKey = 0x4E41434Bu;
	const std::string client_scrk = "CLIENT-NACK-HOLDOFF-SCRK";
	const std::string server_scrk = "SERVER-NACK-HOLDOFF-SCRK";
	inmatch::ClientRuntime client("NackHoldoff", [] { return uint64_t{0x55667788u}; });
	client.seed_session(kServerKey, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId, 0, 0x00100000u, /*replay_mode=*/false);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> settings =
			frame_server_session(server_tx, server_scrk, 1u, {cs_send_holdoff_update(12)});
	client.receive(settings.data(), settings.size());
	if (!expect(!client.Client_ProcessNetworkFrame(1).empty() &&
	                    client.send_holdoff_ticks() == 12 &&
	                    client.send_holdoff_countdown() == 12,
			"nack-holdoff: the first open boundary arms the NovaWorld twelve-tick period"))
		return false;
	const uint32_t flushes = client.send_flush_counter();
	const uint32_t next_seq = client.outbound_seq();

	// S2C seq 2 (the pong of our ping: flag 0, stamp 0x55667000) is lost on the
	// wire; seq 3 arrives three ticks into the held period.
	const std::vector<uint8_t> lost = frame_server_session(server_tx, server_scrk, 1u,
			{make_protocol_message(0x57, {0x00, 0x70, 0x66, 0x55, 0x00})});
	const std::vector<uint8_t> after_gap = frame_server_session(server_tx, server_scrk, 1u, {});
	if (!expect(client.Client_ProcessNetworkFrame(2).empty() &&
	                    client.Client_ProcessNetworkFrame(3).empty(),
			"nack-holdoff: the held frames before the gap send nothing"))
		return false;
	client.receive(after_gap.data(), after_gap.size());
	const std::vector<std::vector<uint8_t>> gap_frame = client.Client_ProcessNetworkFrame(4);
	std::vector<uint32_t> requested;
	if (!expect(gap_frame.size() == 1 &&
	                    decode_client_resend_list(gap_frame[0], kServerKey, requested) &&
	                    requested == std::vector<uint32_t>({2}),
			"nack-holdoff: the frame that sees the gap sends one 0x44 naming the lost sequence"))
		return false;
	if (!expect(client.send_holdoff_countdown() == 9 &&
	                    client.send_flush_counter() == flushes &&
	                    client.outbound_seq() == next_seq,
			"nack-holdoff: the 0x44 leaves mid-holdoff without opening the send boundary"))
		return false;
	if (!expect((client.net_quality_indicators().link_error_bits &
	                    static_cast<uint32_t>(hud::kNetLinkErrorIncoming)) != 0,
			"nack-holdoff: the sent request raises the incoming link error"))
		return false;
	if (!expect(client.Client_ProcessNetworkFrame(5).empty(),
			"nack-holdoff: the receive pump cleared its latch; no repeat without a new packet"))
		return false;

	// A further future packet behind the same gap asks again on its own frame.
	const std::vector<uint8_t> later = frame_server_session(server_tx, server_scrk, 1u, {});
	client.receive(later.data(), later.size());
	const std::vector<std::vector<uint8_t>> again = client.Client_ProcessNetworkFrame(6);
	requested.clear();
	if (!expect(again.size() == 1 &&
	                    decode_client_resend_list(again[0], kServerKey, requested) &&
	                    requested == std::vector<uint32_t>({2}) &&
	                    client.inbound_gap_depth() == 2,
			"nack-holdoff: a later future packet behind the gap re-requests it at once"))
		return false;

	// The host's retransmit of seq 2 closes the gap and drains 3 and 4; the ACK
	// itself waits for the next open boundary.
	client.receive(lost.data(), lost.size());
	if (!expect(client.Client_ProcessNetworkFrame(7).empty() &&
	                    client.inbound_frontier_seq() == 4 &&
	                    client.inbound_gap_depth() == 0 &&
	                    client.client_ping_ms() == 0x788u,
			"nack-holdoff: the recovered sequence dispatches and drains the queue"))
		return false;
	for (uint32_t tick = 8; tick < 13; ++tick) {
		if (!expect(client.Client_ProcessNetworkFrame(tick).empty(),
				"nack-holdoff: the rest of the period stays held"))
			return false;
	}
	const std::vector<std::vector<uint8_t>> boundary = client.Client_ProcessNetworkFrame(13);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	return expect(boundary.size() == 1 &&
	                      decode_client_session(boundary[0], client_scrk, header, messages) &&
	                      matches_client_header(header, kServerKey, next_seq, 4) &&
	                      client.send_flush_counter() == flushes + 1,
			"nack-holdoff: the next open boundary carries the recovered frontier's ACK");
}

// A host's 0x84 is answered from the receive pump too: NapiNP_HandleResendList
// rebuilds each requested sequence from its retained records with the current
// ACK and writes it to the socket itself, so the reconstruction leaves on the
// frame the request arrives, mid-holdoff, without the transient records the
// original carried and without touching the flush counter. A WR-flagged 0x85
// is answered the same way by its pong.
// [orig: NapiNP_HandleResendList @0x623800 -> CNapiNPConnection_SendSessionPacket
//  @0x6239b6 -> CNapiNPManager_SendTo @0x61f039; Nwu_HandlePing @0x623a70 ->
//  CNapiNPConnection_SendPing @0x623c6f -> CNapiNPManager_SendTo @0x61f261]
bool run_resend_answer_and_pong_leave_from_the_receive_pump() {
	constexpr uint32_t kServerKey = 0x55667788u;
	const std::string client_scrk = "CLIENT-HOLDOFF-RETAINED-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-RETAINED-SCRK";
	inmatch::ClientRuntime client("HoldoffRetained", [] { return uint64_t{0x10203040u}; });
	client.seed_session(kServerKey, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId, 0, 0x00100000u, /*replay_mode=*/false);

	// Seq 1: a reliable stance change beside the transient RTT ping.
	if (!expect(client.queue_stance_change(0xA9), "resend-holdoff: the stance change queues"))
		return false;
	const std::vector<std::vector<uint8_t>> original = client.Client_ProcessNetworkFrame(0);
	ProtocolPacketHeader original_header;
	std::vector<ProtocolMessage> original_messages;
	if (!expect(original.size() == 1 &&
	                    decode_client_session(original[0], client_scrk, original_header,
	                            original_messages) &&
	                    original_header.seq_num == 1 && original_messages.size() == 2 &&
	                    original_messages[0].tag == c2s::STANCE_CHANGE &&
	                    original_messages[1].tag == c2s::RTT_CONSUMED &&
	                    client.retained_outbound_depth() == 1,
			"resend-holdoff: seq 1 carries the retained 0x1D and the one-send 0x2C"))
		return false;

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> settings =
			frame_server_session(server_tx, server_scrk, 1u, {cs_send_holdoff_update(12)});
	client.receive(settings.data(), settings.size());
	if (!expect(!client.Client_ProcessNetworkFrame(1).empty() &&
	                    client.send_holdoff_countdown() == 12,
			"resend-holdoff: the initial CS update keeps the boundary open, then holds twelve"))
		return false;
	const uint32_t flushes = client.send_flush_counter();
	const uint32_t next_seq = client.outbound_seq();
	if (!expect(client.Client_ProcessNetworkFrame(2).empty(),
			"resend-holdoff: the period holds"))
		return false;

	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(1u, {original_header.seq_num}, resend_body),
			"resend-holdoff: build the ServerResendList"))
		return false;
	const std::vector<uint8_t> resend =
			nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(resend_body));
	client.receive(resend.data(), resend.size());
	const std::vector<std::vector<uint8_t>> answered = client.Client_ProcessNetworkFrame(3);
	ProtocolPacketHeader resent_header;
	std::vector<ProtocolMessage> resent_messages;
	if (!expect(answered.size() == 1 &&
	                    decode_client_session(answered[0], client_scrk, resent_header,
	                            resent_messages) &&
	                    matches_client_header(resent_header, kServerKey, 1, 1) &&
	                    resent_messages.size() == 1 &&
	                    resent_messages[0].tag == c2s::STANCE_CHANGE &&
	                    resent_messages[0].payload == std::vector<uint8_t>({0xA9, 0x00}),
			"resend-holdoff: the 0x84 frame rebuilds seq 1 (retained 0x1D only, current ACK)"))
		return false;
	if (!expect(client.send_holdoff_countdown() == 10 &&
	                    client.send_flush_counter() == flushes &&
	                    client.outbound_seq() == next_seq,
			"resend-holdoff: the reconstruction neither opens the boundary nor ages it"))
		return false;

	const std::vector<uint8_t> ping = nw_encode_outbound(
			SESSION_OPCODE_SERVER_PING, build_session_ping_body(1u, true, 0xAABBCCDDu));
	client.receive(ping.data(), ping.size());
	const std::vector<std::vector<uint8_t>> pong = client.Client_ProcessNetworkFrame(4);
	uint8_t opcode = 0;
	std::vector<uint8_t> pong_body;
	SessionPingBody parsed;
	if (!expect(pong.size() == 1 &&
	                    nw_decode_inbound(pong[0].data(), pong[0].size(), opcode, pong_body) &&
	                    opcode == SESSION_OPCODE_CLIENT_PING &&
	                    parse_session_ping_body(pong_body.data(), pong_body.size(), parsed) &&
	                    parsed.receiver_local_key == kServerKey && !parsed.wants_reply &&
	                    parsed.timestamp_ms == 0xAABBCCDDu &&
	                    client.send_holdoff_countdown() == 9,
			"resend-holdoff: a WR ping's 0x45 pong leaves on its own receive frame"))
		return false;

	for (uint32_t tick = 5; tick < 13; ++tick) {
		if (!expect(client.Client_ProcessNetworkFrame(tick).empty(),
				"resend-holdoff: the rest of the period stays held"))
			return false;
	}
	const std::vector<std::vector<uint8_t>> live = client.Client_ProcessNetworkFrame(13);
	ProtocolPacketHeader live_header;
	std::vector<ProtocolMessage> live_messages;
	return expect(live.size() == 1 &&
	                      decode_client_session(live[0], client_scrk, live_header,
	                              live_messages) &&
	                      live_header.seq_num == next_seq && live_messages.size() == 1 &&
	                      live_messages[0].tag == c2s::RTT_CONSUMED &&
	                      client.send_flush_counter() == flushes + 1,
			"resend-holdoff: the next open boundary sends only the fresh live packet");
}

// A stance change pressed between boundaries is QUEUED like every other
// reliable C2S: it mints no sequence and ages nothing until the next open
// boundary, which carries it in queue order ahead of what was queued after it
// (here the receive pump's C2S 0x22 re-request of an unbound 0x16 row, also
// queued at its receive) and the frame's live 0x2C. That boundary advances the
// flush counter exactly once.
// [orig: Input_HandleActionBinding_0 cases 169/170/172 ->
//  CNapiNetwork_QueueReliableMessage(0x1D, 1, 0, .., 2) @0x4e0de7;
//  NapiNPClientMsg_PlayerList -> QueueReliableMessage(0x22, ..) @0x42fc35;
//  CNapiNPConnection_QueueMessage @0x628640 -> NapiNPMessage_Create @0x627fc0;
//  CNapiNPConnection_PumpFlags 0x80 @0x6297d5, only in PumpClientProtocolSend]
bool run_queued_stance_waits_for_the_send_boundary() {
	constexpr uint32_t kServerKey = 0x53544E43u;
	const std::string client_scrk = "CLIENT-STANCE-QUEUE-SCRK";
	const std::string server_scrk = "SERVER-STANCE-QUEUE-SCRK";
	inmatch::ClientRuntime client("StanceQueue", [] { return uint64_t{0x31323334u}; });
	client.seed_session(kServerKey, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId, 0, 0x00100000u, /*replay_mode=*/false);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> settings =
			frame_server_session(server_tx, server_scrk, 1u, {cs_send_holdoff_update(12)});
	client.receive(settings.data(), settings.size());
	if (!expect(!client.Client_ProcessNetworkFrame(1).empty() &&
	                    client.send_holdoff_countdown() == 12,
			"stance-queue: the first open boundary arms the twelve-tick period"))
		return false;
	const uint32_t flushes = client.send_flush_counter();
	const uint32_t next_seq = client.outbound_seq();
	if (!expect(client.Client_ProcessNetworkFrame(2).empty(), "stance-queue: the period holds"))
		return false;

	if (!expect(client.queue_stance_change(0xAA), "stance-queue: prone queues between boundaries"))
		return false;
	if (!expect(client.Client_ProcessNetworkFrame(3).empty() &&
	                    client.outbound_seq() == next_seq &&
	                    client.send_flush_counter() == flushes,
			"stance-queue: the queued 0x1D mints no sequence and ages no flush while held"))
		return false;
	const std::vector<uint8_t> roster = frame_server_session(server_tx, server_scrk, 1u,
			{make_protocol_message(0x16, encode_test_player_list({{9, 2}}))});
	client.receive(roster.data(), roster.size());
	if (!expect(client.Client_ProcessNetworkFrame(4).empty() &&
	                    client.outbound_seq() == next_seq &&
	                    client.send_flush_counter() == flushes,
			"stance-queue: the receive-side 0x22 re-request queues the same way"))
		return false;
	for (uint32_t tick = 5; tick < 13; ++tick) {
		if (!expect(client.Client_ProcessNetworkFrame(tick).empty() &&
		                    client.send_flush_counter() == flushes,
				"stance-queue: held frames never age the flush counter"))
			return false;
	}

	const std::vector<std::vector<uint8_t>> boundary = client.Client_ProcessNetworkFrame(13);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(boundary.size() == 1 &&
	                    decode_client_session(boundary[0], client_scrk, header, messages) &&
	                    matches_client_header(header, kServerKey, next_seq, 2) &&
	                    messages.size() == 3,
			"stance-queue: the next open boundary frames one packet at the next sequence"))
		return false;
	if (!expect(messages[0].tag == c2s::STANCE_CHANGE &&
	                    messages[0].payload == std::vector<uint8_t>({0xAA, 0x00}) &&
	                    messages[1].tag == c2s::PLAYER_SYNC_REQUEST &&
	                    messages[1].payload == std::vector<uint8_t>({9, 0xF7, 0x1C}) &&
	                    messages[2].tag == c2s::RTT_CONSUMED,
			"stance-queue: the boundary carries the 0x1D, then the 0x22, then the live 0x2C"))
		return false;
	return expect(client.send_flush_counter() == flushes + 1 &&
	                      client.retained_outbound_depth() == 2 &&
	                      client.send_holdoff_countdown() == 12,
			"stance-queue: the boundary ages once and retains both reliable records");
}

bool run_settings_update_preserves_active_holdoff_countdown() {
	const std::string client_scrk = "CLIENT-HOLDOFF-UPDATE-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-UPDATE-SCRK";
	inmatch::ClientRuntime client("HoldoffUpdate");
	client.seed_session(
			0x61728394u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	auto settings_update = [&](uint32_t period) {
		return frame_server_session(
				server_tx, server_scrk, 1u,
				{make_protocol_message(
						0x00,
						{0x01, 0x08, 0x00, 0x00, 0x00,
						 static_cast<uint8_t>(period), 0x00, 0x00, 0x00},
						0xA0)});
	};

	const std::vector<uint8_t> initial = settings_update(3);
	client.receive(initial.data(), initial.size());
	(void)client.Client_ProcessNetworkFrame(1);
	if (!expect(client.send_holdoff_ticks() == 3 &&
	                    client.send_holdoff_countdown() == 3,
			"initial CS update keeps the open boundary and rearms period three"))
		return false;

	// A later CS update changes only the stored field. The active three-tick
	// cycle still decrements 3 -> 2 instead of being overwritten with five.
	const std::vector<uint8_t> replacement = settings_update(5);
	client.receive(replacement.data(), replacement.size());
	if (!expect(client.Client_ProcessNetworkFrame(2).empty() &&
	                    client.send_holdoff_ticks() == 5 &&
	                    client.send_holdoff_countdown() == 2,
			"later CS update preserves the active countdown while replacing the period"))
		return false;
	if (!expect(client.Client_ProcessNetworkFrame(3).empty() &&
	                    client.send_holdoff_countdown() == 1,
			"the original cycle still holds exactly N-1 frames"))
		return false;
	(void)client.Client_ProcessNetworkFrame(4);
	return expect(client.send_holdoff_countdown() == 5,
			"the replacement period applies when the unchanged cycle next opens");
}

bool run_settings_send_holdoff_blocks_exact_frame_count() {
	const std::string client_scrk = "CLIENT-HOLDOFF-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-SCRK";
	uint64_t now_ms = 1000;
	inmatch::ClientRuntime client("Holdoff", [&now_ms] { return now_ms; });
	client.seed_session(
			0x66778899u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
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
	const std::vector<uint8_t> first_tags = semantic_tags(first_frame);
	if (!expect(first_frame.size() == 1 &&
	                    first_tags ==
	                            std::vector<uint8_t>(
						{0x2C, 0x1C, 0x08, 0x3D, 0x2C}),
			"the first boundary stays open and batches receive replies with the live RTT"))
		return false;
	if (!expect(client.send_holdoff_countdown() == 2,
			"the immediate boundary re-arms the stored field-3 period"))
		return false;
	++now_ms;
	// The re-armed period gates exactly N-1 subsequent frames. For N=2 the
	// next frame is held, then the following frame opens and reloads.
	const std::vector<std::vector<uint8_t>> second_frame =
			client.Client_ProcessNetworkFrame(11);
	if (!expect(second_frame.empty() && client.send_holdoff_countdown() == 1,
			"period two holds exactly one frame after the immediate boundary"))
		return false;
	++now_ms;
	const std::vector<std::vector<uint8_t>> third_frame =
			client.Client_ProcessNetworkFrame(12);
	if (!expect(!third_frame.empty() && client.send_holdoff_countdown() == 2,
			"the periodic boundary reopens every dictated interval"))
		return false;
	++now_ms;
	return expect(client.Client_ProcessNetworkFrame(13).empty() &&
	                      client.send_holdoff_countdown() == 1,
			"the reloaded period continues to hold exactly N-1 frames");
}

bool run_send_holdoff_defers_due_housekeeping() {
	const std::string client_scrk = "CLIENT-HOLDOFF-DUE-SCRK";
	const std::string server_scrk = "SERVER-HOLDOFF-DUE-SCRK";
	uint64_t now_ms = 2000;
	inmatch::ClientRuntime client("HoldoffDue", [&now_ms] { return now_ms; });
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
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> hold = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(
					0x00, {0x01, 0x08, 0x00, 0x00, 0x00,
					       0x02, 0x00, 0x00, 0x00}, 0xA0)});
	client.receive(hold.data(), hold.size());

	const std::vector<uint8_t> first_open =
			semantic_tags(client.Client_ProcessNetworkFrame(310));
	if (!expect(first_open == std::vector<uint8_t>({0x4C, 0x2C}) &&
	                    client.send_holdoff_countdown() == 2 &&
	                    client.retained_outbound_depth() == 1 &&
	                    client.send_flush_counter() == 311,
			"the initial open boundary flushes due 0x4C before the live RTT"))
		return false;
	++now_ms;
	const std::vector<uint8_t> held =
			semantic_tags(client.Client_ProcessNetworkFrame(311));
	if (!expect(held.empty() && client.send_holdoff_countdown() == 1 &&
	                    client.send_flush_counter() == 311,
			"period two holds exactly one frame without aging finite retention"))
		return false;
	++now_ms;
	(void)client.Client_ProcessNetworkFrame(312);
	return expect(client.send_holdoff_countdown() == 2 &&
	                      client.send_flush_counter() == 312,
			"the next housekeeping boundary opens, ages once, and rearms");
}

bool run_finite_quality_retention_expires_on_flush_310() {
	const std::string client_scrk = "CLIENT-QUALITY-TTL-SCRK";
	const std::string server_scrk = "SERVER-QUALITY-TTL-SCRK";
	inmatch::ClientRuntime client("QualityTtl", [] { return uint64_t{9000}; });
	client.seed_session(
			0x66778899u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	for (uint32_t frame = 0; frame < 310; ++frame)
		(void)client.Client_ProcessNetworkFrame(frame);
	if (!expect(client.send_flush_counter() == 310 &&
	                    client.retained_outbound_depth() == 0,
	            "quality TTL fixture reaches producer boundary with no retained node"))
		return false;
	(void)client.Client_ProcessNetworkFrame(310); // 0x4C first inclusion at C=310
	if (!expect(client.send_flush_counter() == 311 &&
	                    client.retained_outbound_depth() == 1,
	            "C2S 0x4C is retained after its first send at counter C"))
		return false;

	// Boundaries C+1 through C+308 retain the node. The following boundary is
	// C+309 and removes it before incrementing, exactly as retail does.
	for (uint32_t frame = 311; frame <= 618; ++frame)
		(void)client.Client_ProcessNetworkFrame(frame);
	if (!expect(client.send_flush_counter() == 619 &&
	                    client.retained_outbound_depth() == 1,
	            "C2S 0x4C survives through finite boundary C+308"))
		return false;
	(void)client.Client_ProcessNetworkFrame(619);
	return expect(client.send_flush_counter() == 620 &&
	                      client.retained_outbound_depth() == 0,
	            "C2S 0x4C expires individually at finite boundary C+309");
}

// S2C 0x0F carries the authority's 128-dword ammo-pool image (serverPlayer+88664 ->
// client g_LocalAmmoPools) at the fixed body offset 23; the runtime retains it with
// a revision the embedder applies after the 0x5A slot rebuild, and start() clears
// it with the other authoritative state. [orig: NapiNPClientMsg_0x00F
// @0x42e324..0x42e34a -> WeaponSlots_RecalculateAmmoFromCapacity @0x42e424]
bool run_world_state_load_pools_reach_the_runtime() {
	const std::string client_scrk = "CLIENT-POOLS-SCRK";
	const std::string server_scrk = "SERVER-POOLS-SCRK";
	uint64_t now_ms = 0x01020304u;
	inmatch::ClientRuntime client("Pools", [&now_ms] { return now_ms; });
	client.seed_session(
			0x44556677u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);
	if (!expect(client.authoritative_ammo_pools_revision() == 0,
			"a fresh runtime holds no authority pool image"))
		return false;

	// 23-byte fixed header, the 128 pool dwords, zero waypoint/location counts.
	std::vector<uint8_t> body(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	const auto put_pool = [&body](size_t index, uint32_t value) {
		const size_t at = 23 + index * 4;
		body[at] = uint8_t(value);
		body[at + 1] = uint8_t(value >> 8);
		body[at + 2] = uint8_t(value >> 16);
		body[at + 3] = uint8_t(value >> 24);
	};
	put_pool(3, 270);  // the golden team-1 image: M16 300 -> 270 after the clip draw
	put_pool(21, 63);  // .45 70 -> 63
	put_pool(76, 2);   // AT4 3 -> 2
	put_pool(127, 0xFFFFFFF7u); // a negative pool rides as is (shipped -1 startrounds)
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> framed = frame_server_session(
			server_tx, server_scrk, 1u, {make_protocol_message(0x0F, body)});
	client.receive(framed.data(), framed.size());
	(void)client.Client_ProcessNetworkFrame(10);
	const std::array<int32_t, kWorldStateAmmoPoolCount> &pools =
			client.authoritative_ammo_pools();
	if (!expect(client.authoritative_ammo_pools_revision() == 1 &&
	                    pools[3] == 270 && pools[21] == 63 && pools[76] == 2 &&
	                    pools[127] == -9 && pools[0] == 0 && pools[4] == 0,
			"the 0x0F pool image is retained verbatim with one revision"))
		return false;

	(void)client.start();
	return expect(client.authoritative_ammo_pools_revision() == 0 &&
	                      client.authoritative_ammo_pools()[3] == 0,
			"start clears the authority pool image with the other authoritative state");
}

bool run_start_resets_reusable_runtime_state() {
	const std::string client_scrk = "CLIENT-REUSE-SCRK";
	const std::string server_scrk = "SERVER-REUSE-SCRK";
	uint64_t now_ms = 0x01020304u;
	inmatch::ClientRuntime client("Reusable", [&now_ms] { return now_ms; });
	client.seed_session(
			0x44556677u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId,
			0, 0x00100000u, /*replay_mode=*/false);

	if (!expect(client.queue_vehicle_attach(0x1007, 3),
			"reuse fixture queues session-bound gameplay traffic"))
		return false;
	client.queue_loadout_resubmit();
	client.queue_deployment_pick(0xFFFFu);

	SessionSequencing first_server_tx = inmatch::make_jo_game_session_sequencing();
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
	                    client.send_holdoff_countdown() == 4,
			"reuse fixture preserves the initial open boundary then rearms cadence state"))
		return false;

	client.view().state().anchor_x = 0x12345678;
	client.view().apply(0x49, {0x02, 0x00, 0x07, 0x10});

	// Leave a sequence-one packet queued at the receive boundary. If start()
	// fails to discard the old FIFO, it is valid under the freshly seeded
	// session below and reinstalls a nine-frame holdoff.
	SessionSequencing stale_server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> stale_inbound = frame_server_session(
			stale_server_tx, server_scrk, 1u,
			{make_protocol_message(
					0x00, {0x01, 0x08, 0x00, 0x00, 0x00,
					       0x09, 0x00, 0x00, 0x00}, 0xA0)});
	client.receive(stale_inbound.data(), stale_inbound.size());

	const std::vector<uint8_t> hello = client.start();
	if (!expect(!hello.empty() &&
	                    client.phase() == inmatch::JoinerConnection::Phase::Hello,
			"reused runtime starts a fresh handshake"))
		return false;
	if (!expect(client.authoritative_loadout_revision() == 0 &&
	                    client.zone_states().empty() &&
	                    client.send_holdoff_countdown() == 0 &&
	                    !client.is_deployed(),
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
	inmatch::JoinerConnection joiner("EchoGuard");
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
	const inmatch::JoinerConnection::PollResult ignored_hello =
			joiner.handle_datagram(wrong_hello_datagram.data(), wrong_hello_datagram.size());
	if (!expect(ignored_hello.outbound.empty() &&
	                    joiner.phase() == inmatch::JoinerConnection::Phase::Hello,
			"a ServerHello for another CI is ignored without advancing"))
		return false;

	ServerHello valid_hello = build_server_hello(hello, 0x7F000001u, 32769);
	const std::vector<uint8_t> valid_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(valid_hello));
	const inmatch::JoinerConnection::PollResult auth_request =
			joiner.handle_datagram(valid_hello_datagram.data(), valid_hello_datagram.size());
	if (!expect(auth_request.outbound.size() == 1 &&
	                    joiner.phase() == inmatch::JoinerConnection::Phase::Auth,
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
	return expect(joiner.phase() == inmatch::JoinerConnection::Phase::Auth &&
	                      joiner.server_key() == 0,
			"a ServerAuth with mismatched CK cannot install server keys");
}

// --- Round-5 hardening ---------------------------------------------------------------------

// Both witnessed writers of byte_A85B48 must be falsifiable. S2C 0x04 installs
// the admission team; S2C 0x75 byte 1 refreshes it when the server broadcasts
// this player's live slot state. Read the result through frame_loadout_resubmit
// — the same assigned_team_ source the 0x1A pair uses.
// [orig: NapiNPClientMsg_SessionSlotConfig (0x04) @0x425410 -> byte_A85B48 @0x425499;
// NapiNPClientMsg_SetSpectatorMode @0x4259E0 -> byte_A85B48 @0x425A32]
bool run_team_latch_is_falsifiable() {
	const std::string client_scrk = "CLIENT-TEAM-LATCH-SCRK";
	const std::string server_scrk = "SERVER-TEAM-LATCH-SCRK";
	inmatch::JoinerConnection joiner("TeamLatch");
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

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> assign = frame_server_session(server_tx, server_scrk, 1u,
			{make_protocol_message(0x04, retail_slot_assignment(0x03))});
	(void)joiner.handle_datagram(assign.data(), assign.size());
	if (!expect(resubmit_team(team) && team == 0x03,
			"the 0x04 tail byte latches and drives the 0x2F team byte")) {
		return false;
	}

	const std::vector<uint8_t> live_state = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x75, {0x00, 0x01})});
	(void)joiner.handle_datagram(live_state.data(), live_state.size());
	if (!expect(resubmit_team(team) && team == 0x01,
			"0x75 byte 1 refreshes the same 0x2F team latch")) {
		return false;
	}

	std::vector<uint8_t> short_body(20, 0);
	short_body[19] = 0x04; // a would-be team in a body too short for the tail read
	const std::vector<uint8_t> short_assign = frame_server_session(server_tx, server_scrk, 1u,
			{make_protocol_message(0x04, std::move(short_body))});
	(void)joiner.handle_datagram(short_assign.data(), short_assign.size());
	return expect(resubmit_team(team) && team == 0x01,
			"a short (<24 B) 0x04 body does not take the latch");
}

// The padding echo must follow the retail clamp — body = max(12, min(padding_len, 512)) with
// the [x][y][net-frame-counter] prefix — and a tiny request must still echo AND advance the
// FSM (the pre-fix builder returned nothing below 8 bytes, silently wedging the join at
// AwaitPaddingProbe). [orig: NetPacket_WritePositionWithPadding @0x42A360; server consumer
// NapiNPServerMsg_0x002 @0x512fd0 reads dwords 0 and 2]
// S2C 0x46 bit 0x4000 is a client-driven roster cursor, not a passive marker.
// Retail requests slot+1 with the fixed 0x5CF7 mask until it has processed the
// inclusive max-slot byte from S2C 0x04. Empty/removal rows advance the same
// cursor; the reply for the maximum slot terminates it.
// [orig: NapiNPClientMsg_PlayerSync @0x431370 tail; golden retail-to-retail
//  frames 190/193/196/198/200 walk slots 0..4]
bool run_player_sync_ack_walks_inclusive_roster_capacity() {
	const std::string client_scrk = "CLIENT-PLAYER-SYNC-WALK-SCRK";
	const std::string server_scrk = "SERVER-PLAYER-SYNC-WALK-SCRK";
	inmatch::JoinerConnection joiner("RosterWalk");
	joiner.seed_in_match(0x10293847u, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0002, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	std::vector<uint8_t> slot_config = retail_slot_assignment();
	slot_config[18] = 4; // inclusive maximum roster slot: walk 0,1,2,3,4
	const std::vector<uint8_t> config_datagram = frame_server_session(
			server_tx, server_scrk, 1u,
			{make_protocol_message(0x04, std::move(slot_config))});
	(void)joiner.handle_datagram(config_datagram.data(), config_datagram.size());

	auto apply_sync = [&](std::vector<uint8_t> body) {
		const std::vector<uint8_t> datagram = frame_server_session(
				server_tx, server_scrk, 1u,
				{make_protocol_message(0x46, std::move(body))});
		return joiner.handle_datagram(datagram.data(), datagram.size());
	};
	const inmatch::JoinerConnection::PollResult slot_zero = apply_sync({
			0x00, 0x05, 0x40, // slot 0, name|team|queue-ack
			0x34, 'H', 0x00, 0x01,
	});
	if (!expect(slot_zero.queued_send_messages.size() == 1 &&
				slot_zero.queued_send_messages[0].tag == 0x22 &&
				slot_zero.queued_send_messages[0].payload ==
						std::vector<uint8_t>({0x01, 0xF7, 0x5C}),
			"player-sync ack advances the roster request from slot 0 to slot 1")) {
		return false;
	}

	const inmatch::JoinerConnection::PollResult empty_slot_one = apply_sync({
			0x01, 0x00, 0xC0, // removal|queue-ack
	});
	if (!expect(empty_slot_one.cleared_player_slots == std::vector<uint8_t>({0x01}) &&
				empty_slot_one.queued_send_messages.size() == 1 &&
				empty_slot_one.queued_send_messages[0].payload ==
						std::vector<uint8_t>({0x02, 0xF7, 0x5C}),
			"an empty player-sync row still advances the roster cursor")) {
		return false;
	}

	const inmatch::JoinerConnection::PollResult max_slot = apply_sync({
			0x04, 0x05, 0x40, // maximum slot, name|team|queue-ack
			0x40, 'T', 0x00, 0x02,
	});
	return expect(max_slot.queued_send_messages.empty(),
			"the inclusive maximum roster slot terminates the ack walk");
}

bool run_padding_echo_retail_clamp(uint32_t requested_len, std::size_t expected_body) {
	constexpr uint32_t kServerKey = 0x66778899u;
	inmatch::JoinerConnection joiner("PadClamp");

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
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
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

	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
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
	const inmatch::JoinerConnection::PollResult padding_result =
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
	inmatch::NapiNPServerCtx host;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E112u);
	inmatch::ClientRuntime client("GoodbyeJoiner");
	if (!expect(client.disconnect().empty(),
			"goodbye: nothing to send before ServerAuth assigns the session key")) {
		return false;
	}
	const std::vector<uint8_t> hello = client.start();
	const inmatch::HandleResult hello_result = inmatch::handle_server_datagram(
			host, peer, hello.data(), hello.size(), 0);
	if (!expect(hello_result.outbound.size() == 1, "goodbye: host answers ServerHello")) {
		return false;
	}
	client.receive(hello_result.outbound[0].data(), hello_result.outbound[0].size());
	const std::vector<std::vector<uint8_t>> auth = client.Client_ProcessNetworkFrame(0);
	if (!expect(auth.size() == 1, "goodbye: client emits ClientAuth")) return false;
	const inmatch::HandleResult auth_result = inmatch::handle_server_datagram(
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
		(void)inmatch::handle_server_datagram(host, peer, dg.data(), dg.size(), 1);
	if (!expect(host.np_protocol.connection_list.empty(),
			"goodbye: the host tears the connection down (idempotent across the burst)")) {
		return false;
	}
	return expect(client.disconnect().empty(), "goodbye: the burst is one-shot");
}

bool run_joiner_admits_exact_retail_message_prefix() {
	const std::string client_scrk = "CLIENT-MSG-OUT-MAX-SCRK";
	const std::string server_scrk = "SERVER-MSG-OUT-MAX-SCRK";
	inmatch::JoinerConnection reliable_joiner("ReliablePrefix");
	reliable_joiner.seed_in_match(
			0x12345678u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);

	std::vector<ProtocolMessage> retained(
			JO_SESSION_OUTBOUND_MESSAGE_MAX - 1,
			make_protocol_message(0x60, {}));
	if (!expect(!reliable_joiner.frame_messages(retained).empty() &&
	                    reliable_joiner.retained_outbound_depth() ==
	                            JO_SESSION_OUTBOUND_MESSAGE_MAX - 1,
			"joiner fixture retains 1,199 reliable nodes across MTU packets"))
		return false;
	reliable_joiner.complete_send_flush();

	// The 0x61 takes the last free node; the 0x62 is the pool overflow. A
	// client-side connection's RequestDisconnect tears it down INLINE, so
	// nothing queued this boundary is built: the result is the 4x 0x46 burst
	// carrying {2, 4, count, max, "", 0, "NP.C:MSGCRE"} (count = 1,199 retained
	// + 1 admitted + 1), the retained depth stays 1,199 and the session is
	// terminal. [orig: NapiNPMessage_Create @0x628099..0x628112 ->
	//  RequestDisconnect @0x61e0fa -> TeardownActiveConnection @0x62549e]
	const std::vector<std::vector<uint8_t>> capped =
			reliable_joiner.frame_messages({
					make_protocol_message(0x61, {0xA1}),
					make_protocol_message(0x62, {0xB2}),
			});
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(capped.size() == 4 &&
	                    reliable_joiner.retained_outbound_depth() ==
	                            JO_SESSION_OUTBOUND_MESSAGE_MAX - 1 &&
	                    reliable_joiner.phase() ==
	                            inmatch::JoinerConnection::Phase::Error &&
	                    reliable_joiner.has_disconnect_event() &&
	                    reliable_joiner.last_disconnect_event().ddstr == "NP.C:MSGCRE",
			"at 1,199 retained nodes the 1,201st queued node is the MSGCRE overflow: "
			"the goodbye burst, nothing built, the session terminal"))
		return false;
	const std::vector<uint8_t> expected_goodbye = client_goodbye_to_bytes(
			0x12345678u,
			make_disconnect_event(2, 4, JO_SESSION_OUTBOUND_MESSAGE_MAX + 1,
					JO_SESSION_OUTBOUND_MESSAGE_MAX, "", 0, "NP.C:MSGCRE"));
	for (const std::vector<uint8_t> &datagram : capped) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
		                    opcode == SESSION_OPCODE_CLIENT_GOODBYE &&
		                    body == expected_goodbye,
				"every 0x46 of the overflow burst carries the MSGCRE record keyed by the SK"))
			return false;
	}

	inmatch::JoinerConnection failing_joiner("FailedSuffix");
	failing_joiner.seed_in_match(
			0xAABBCCDDu, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	ProtocolMessage invalid = make_protocol_message(
			0x64, std::vector<uint8_t>(256u, 0xCC), 0x20);
	const inmatch::JoinerConnection::FrameMessagesResult failed =
			failing_joiner.frame_messages_detailed({
					make_protocol_message(0x65, {0x01}),
					std::move(invalid),
					make_protocol_message(0x66, {0x02}),
			});
	if (!expect(failed.frame_failed && failed.admitted_count == 3 &&
	                    failed.framed_count == 1 &&
	                    failed.datagrams.size() == 1,
			"joiner batching reports the exact unframed owner-queue suffix"))
		return false;

	// Transient/userParam=1 nodes do not enter the resend map, but every node
	// already framed in this OPEN boundary still occupies msg_out_max. A single
	// semantic queue of exactly 1,200 nodes spanning multiple MTU packets fills
	// the pool without overflowing it, like retail's per-node Create loop (a
	// 1,201st would be the MSGCRE overflow above).
	inmatch::JoinerConnection transient_joiner("TransientPrefix");
	transient_joiner.seed_in_match(
			0x87654321u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	ProtocolMessage transient = make_protocol_message(0x63, {});
	transient.reliable = false;
	std::vector<ProtocolMessage> transient_nodes(
			JO_SESSION_OUTBOUND_MESSAGE_MAX, transient);
	const std::vector<std::vector<uint8_t>> transient_datagrams =
			transient_joiner.frame_messages(transient_nodes);
	std::size_t admitted = 0;
	for (const std::vector<uint8_t> &datagram : transient_datagrams) {
		messages.clear();
		if (!decode_client_session(
					datagram, client_scrk, header, messages))
			return expect(false, "decode MTU-split transient prefix");
		admitted += messages.size();
	}
	return expect(admitted == JO_SESSION_OUTBOUND_MESSAGE_MAX &&
	                      transient_joiner.retained_outbound_depth() == 0 &&
	                      !transient_joiner.has_disconnect_event() &&
	                      transient_joiner.phase() !=
	                              inmatch::JoinerConnection::Phase::Error,
			"an MTU-split transient queue of exactly 1,200 nodes ships whole without an overflow");
}

// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430;
// NapiNPMessage_SplitAtLength @0x628350]
bool run_joiner_splits_to_fill_remaining_packet_space() {
	const std::string client_scrk = "CLIENTSPLITFILL0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	const std::string server_scrk = "SERVERSPLITFILL0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	inmatch::JoinerConnection joiner("SplitFill");
	joiner.seed_in_match(0x12345678u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	auto first = make_protocol_message(0x60, std::vector<uint8_t>(10, 0xA1));
	auto second = make_protocol_message(0x61, std::vector<uint8_t>(20, 0xB2));
	second.reliable = false;
	second.retention_flushes = 1;
	const auto framed = joiner.frame_messages_detailed({first, second}, 48);
	if (!expect(!framed.frame_failed && framed.framed_count == 2 &&
			framed.datagrams.size() == 2 && framed.datagrams[0].size() == 48,
			"joiner fills first datagram instead of deferring a record that fits alone")) return false;
	ProtocolReassemblyState reassembly;
	std::vector<std::vector<uint8_t>> bodies;
	std::vector<ProtocolMessage> records;
	for (const auto &datagram : framed.datagrams) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> packet;
		if (!decode_client_session(datagram, client_scrk, header, packet)) return false;
		for (const auto &message : packet) {
			records.push_back(message);
			std::vector<uint8_t> payload;
			if (reassemble_protocol_payload(reassembly, message, payload))
				bodies.push_back(std::move(payload));
		}
	}
	if (!expect(records.size() == 3 && records[1].payload.size() == 14 &&
			records[1].flags.frag_cont && records[2].payload.size() == 6 &&
			records[2].flags.frag_end && bodies.size() == 2 &&
			bodies[0] == first.payload && bodies[1] == second.payload,
			"split prefix and continuation reassemble in semantic order")) return false;
	joiner.complete_send_flush();
	joiner.complete_send_flush();
	return expect(joiner.retained_outbound_depth() == 3,
			"splitting a transient record retains both pieces until acknowledgement");
}


bool run_flag_event_audio_and_feed_drain_independently() {
    inmatch::ClientRuntime runtime("FlagAudio");
    w::World world;
    world.registry.configure_pool(0, 4);
    w::Entity person;
    person.item_id = 11; person.has_item_def = true; person.team = 1;
    world.cached.local_player = world.registry.spawn(0, person);
    const auto remote = w::EntityHandle::make(0, 1);
    runtime.view().apply(s2c::ENTITY_SPAWN_BATCH,
            make_organic_spawn(remote.packed, "Remote", 12*65536, 34*65536, 5*65536, 0, 1, 1));
    world.out.fire_sounds.set_listener({});
    opennova::lwf::File bank;
    for (const char *name : {"FLAG_PU_P", "FLAG_SV_OT", "FLAG_VXSV_OT"}) {
        opennova::lwf::Multi set; set.name = name; bank.multis.push_back(set);
    }
    opennova::audio::SoundSetIndex sets;
    sets.add_bank(0, bank); world.tables.sound_sets = &sets;
    runtime.view().apply(s2c::GAME_EVENT, {20, 0, 255, 255, 0, 0, 0, 0});
    runtime.view().apply(s2c::GAME_EVENT, {21, uint8_t(remote.slot()), 255, 255, 0xFE, 0xFF, 3, 0});
    if (!expect(runtime.drain_game_events().size() == 2,
            "text feed drains independently of flag audio")) return false;
    runtime.apply_received_effects(world);
    auto ready = world.out.fire_sounds.drain();
    if (!expect(world.out.script_sounds.size() == 1 &&
            world.out.script_sounds[0].name == "FLAG_PU_P" &&
            ready.size() == 1 && ready[0].set_name == "FLAG_SV_OT" &&
            ready[0].pos.x == -2 && ready[0].pos.y == 3 &&
            world.out.fire_sounds.pending_count() == 1,
            "ordered decoded events reach local and remote sound routes")) return false;
    runtime.apply_received_effects(world);
    if (!expect(world.out.script_sounds.size() == 1 && world.out.fire_sounds.drain().empty(),
            "flag audio consumes each receive edge once")) return false;
    for (int i = 0; i < 62; ++i) world.out.fire_sounds.tick();
    ready = world.out.fire_sounds.drain();
    return expect(ready.size() == 1 && ready[0].set_name == "FLAG_VXSV_OT" && ready[0].interface_set,
            "flag voice uses the shared pending slot after 62 ticks");
}

bool run_medic_reviving_plays_both_receive_cues() {
	inmatch::ClientRuntime runtime("MedicAudio");
	runtime.view().set_mp_session(true);
	w::World world;
	world.registry.configure_pool(0, 4);
	w::Entity person;
	person.position = {12.0f, 34.0f, 5.0f};
	person.item_id = 11; person.has_item_def = true;
	world.cached.local_player = world.registry.spawn(0, person);
	world.rules.mp_session = true;
	std::vector<std::string> voices;
	world.script.voice.set_set_resolver([&](const std::string &name, uint8_t, bool)
			-> std::optional<w::ScriptVoiceChannel::SetSelection> {
		voices.push_back(name); return std::nullopt;
	});
	runtime.view().apply(s2c::MEDIC_REVIVING, {});
	runtime.apply_received_effects(world);
	if (!expect(world.out.slot_sounds.size() == 1 && voices == std::vector<std::string>{"MEDIC_VOICE"},
			"0x3A produces positional kit audio and the medic radio voice")) return false;
	const auto &kit = world.out.slot_sounds.front();
	if (!expect(std::string(kit.set_name) == "MEDIC_KIT_USE" && kit.pos[0] == 12 * 65536 &&
			kit.pos[1] == 34 * 65536 && kit.pos[2] == 5 * 65536,
			"revive kit sound uses the local body's current position")) return false;
	runtime.apply_received_effects(world);
	if (!expect(voices.size() == 1, "sound drain is consumed once")) return false;
	runtime.view().apply(s2c::MEDIC_REVIVING, {});
	runtime.apply_received_effects(world);
	return expect(voices.size() == 2 && world.out.slot_sounds.size() == 2,
			"a new revive message attempts both cues even while already latched");
}

bool run_radio_events_preserve_order_chat_and_mute_state(bool replica_only) {
    inmatch::ClientRuntime runtime("RadioAudio");
    runtime.view().set_mp_session(true);
    w::World world;
    world.registry.configure_pool(0, 4);
    w::Entity person;
    person.item_id = 11; person.has_item_def = true; person.team = 1;
    world.cached.local_player = world.registry.spawn(0, person);
    person.position = {12.0f, 34.0f, 5.0f};
    const auto remote = replica_only ? w::EntityHandle::make(0, 1) : world.registry.spawn(0, person);
    if (replica_only)
        runtime.view().apply(s2c::ENTITY_SPAWN_BATCH,
                make_organic_spawn(remote.packed, "Medic", 12*65536, 34*65536, 5*65536, 0, 1, 1));
    const auto request = [&]() {
        return replica_only ? runtime.state().find(remote.packed)->radio_request :
                world.registry.get(remote)->radio_request;
    };
    world.rules.mp_session = true;
    auto &roster = runtime.view().state().roster[3];
    roster.bound = true; roster.entity_slot = int16_t(remote.slot()); roster.name = "Medic";
    runtime.view().state().location_names = {"Hill"};
    world.tables.voice_macros.sections.push_back({"macrotext", 1});
    world.tables.voice_macros.entries.push_back({"RAD_6", "Need a lift", {}, 0});
    std::vector<std::string> voices;
    world.script.voice.set_set_resolver([&](const std::string &name, uint8_t, bool)
            -> std::optional<w::ScriptVoiceChannel::SetSelection> {
        voices.push_back(name); return std::nullopt;
    });
    const std::vector<uint8_t> body{6, uint8_t(remote.slot()), 0, 0};
    runtime.view().apply(s2c::TRACKED_PLAYER_VOICE, body);
    runtime.view().apply(s2c::MEDIC_REVIVING, {});
    runtime.apply_received_effects(world);
    if (!expect(voices == std::vector<std::string>{"BM1_RAD_6", "MEDIC_VOICE"},
            "radio and revive sounds keep packet receive order")) return false;
    const auto lines = runtime.view().drain_chat_lines();
    if (!expect(lines.size() == 1 && lines[0].text == "Medic:[Hill]: Need a lift" &&
            lines[0].channel == 2 && lines[0].sender_slot == remote.slot(),
            "radio chat resolves macrotext and the 0x0F location-name table")) return false;
    if (!expect(request() == 1 &&
            runtime.state().tracked_target.handle == remote.packed &&
            runtime.state().tracked_target.ticks_remaining == 1860 &&
            runtime.state().tracked_target.color == 0xFF80A0FFu,
            "event six arms the ride request and 30-second tracking target")) return false;
    roster.radio_mute_flags = 1;
    runtime.view().apply(s2c::TRACKED_PLAYER_VOICE, {7, uint8_t(remote.slot()), 255, 255});
    runtime.apply_received_effects(world);
    if (!expect(voices.size() == 2 && request() == 1 &&
            runtime.view().drain_chat_lines()[0].text == "Medic: RAD_7",
            "voice mute gates the request latch, independently of chat")) return false;
    roster.radio_mute_flags = 2;
    runtime.view().apply(s2c::TRACKED_PLAYER_VOICE, {7, uint8_t(remote.slot()), 255, 255});
    runtime.apply_received_effects(world);
    return expect(voices.back() == "BM1_RAD_7" && runtime.view().drain_chat_lines().empty() &&
            request() == 0, "chat mute permits radio playback");
}


// S2C 0x2D and a channel-13 chat line set the map's tracked target through
// HUD_SetTrackedEntityTarget: 496 ticks, white, the snapshot position; the
// slot's voice-mute bit gates the emote leg, its chat-mute bit the chat leg.
// [orig: NapiNPClientMsg_HandleEmote @0x427E90 (@0x427f39, @0x427f5b);
//  Chat_DispatchToChannel @0x42B9CD..0x42BA09; HUD_SetTrackedEntityTarget
//  @0x59D050]
bool run_emote_and_local_chat_track_the_speaker(bool replica_only) {
    inmatch::ClientRuntime runtime("EmoteTrack");
    runtime.view().set_mp_session(true);
    w::World world;
    world.registry.configure_pool(0, 4);
    w::Entity person;
    person.item_id = 11; person.has_item_def = true; person.item_type = 3; person.team = 1;
    world.cached.local_player = world.registry.spawn(0, person);
    person.position = {12.0f, 34.0f, 5.0f};
    const auto remote = replica_only ? w::EntityHandle::make(0, 1) : world.registry.spawn(0, person);
    if (replica_only)
        runtime.view().apply(s2c::ENTITY_SPAWN_BATCH,
                make_organic_spawn(remote.packed, "Mate", 12*65536, 34*65536, 5*65536, 0, 1, 1));
    world.rules.mp_session = true;
    auto &roster = runtime.view().state().roster[3];
    roster.bound = true; roster.entity_slot = int16_t(remote.slot()); roster.name = "Mate";
    auto &target = runtime.view().state().tracked_target;

    runtime.view().apply(s2c::EMOTE_BROADCAST, {4, uint8_t(remote.slot()), 0, 0});
    runtime.apply_received_effects(world);
    if (!expect(target.handle == remote.packed && target.ticks_remaining == 496 &&
            target.color == 0xFFFFFFFFu && target.friendly &&
            target.position[0] == 12 * 65536 && target.serial == 1,
            "an emote tracks its speaker for 496 ticks in white")) return false;
    roster.radio_mute_flags = 1;
    target = {};
    runtime.view().apply(s2c::EMOTE_BROADCAST, {4, uint8_t(remote.slot()), 0, 0});
    runtime.apply_received_effects(world);
    if (!expect(target.ticks_remaining == 0, "the voice-mute bit gates the emote")) return false;
    // The local player's own emote never tracks itself.
    roster.radio_mute_flags = 0;
    runtime.view().apply(s2c::EMOTE_BROADCAST,
            {4, uint8_t(world.cached.local_player.slot()), 0, 0});
    runtime.apply_received_effects(world);
    if (!expect(target.ticks_remaining == 0, "the local player is never tracked")) return false;

    ChatBroadcast chat;
    chat.channel = 13;
    chat.sender_slot = 3;
    chat.text = "Mate: here";
    runtime.view().apply(s2c::CHAT_BROADCAST, encode_chat_broadcast(chat));
    runtime.apply_received_effects(world);
    if (!expect(target.handle == remote.packed && target.ticks_remaining == 496,
            "a local chat line tracks its sender's person")) return false;
    target = {};
    roster.radio_mute_flags = 2;
    runtime.view().apply(s2c::CHAT_BROADCAST, encode_chat_broadcast(chat));
    runtime.apply_received_effects(world);
    if (!expect(target.ticks_remaining == 0, "a chat-muted slot tracks nothing")) return false;
    roster.radio_mute_flags = 0;
    chat.channel = 1;
    runtime.view().apply(s2c::CHAT_BROADCAST, encode_chat_broadcast(chat));
    runtime.apply_received_effects(world);
    return expect(target.ticks_remaining == 0, "only channel 13 tracks the sender");
}

// The listen client's visible-players refreshes ride its loopback: the 0x0F
// burst member pair {0, 0x5CF7} + 0x23, another slot's 0x4D pair
// {slot, 0x1CF7} + 0x23, nothing for its own slot; the 0x4C snapshot lands
// in the table, and the 1 Hz revive countdown walks the table only.
// [orig: NapiNPClientMsg_0x00F @0x42e66c..0x42e6ab; NapiNPClientMsg_HandleSpawnSlot
//  @0x4317B0; Client_ProcessNetworkFrame @0x42C27E..0x42C2DA]
// The Emotes / Radio menu picks leave the listen client over its loopback as
// C2S 0x14 / 0x13 [i16 value]; a joiner with no session queues nothing.
// [orig: NetPacket_SendEmoteRequest @0x42C120; NetPacket_SendRadioCallRequest
//  @0x42C150]
bool run_voice_menu_picks_ride_the_session() {
    ns::LoopbackChannel host_loop;
    inmatch::ClientRuntime host_view(host_loop);
    if (!expect(host_view.queue_voice_menu_pick(c2s::EMOTE_REQUEST, 3) &&
            host_view.queue_voice_menu_pick(c2s::RADIO_CALL_REQUEST, 10),
            "the listen client queues both picks")) return false;
    std::vector<ns::Datagram> out;
    ns::Datagram dg;
    while (host_loop.host_recv(dg)) out.push_back(dg);
    if (!expect(out.size() == 2 && out[0].tag == c2s::EMOTE_REQUEST &&
            out[0].body == std::vector<uint8_t>({3, 0}) &&
            out[1].tag == c2s::RADIO_CALL_REQUEST &&
            out[1].body == std::vector<uint8_t>({10, 0}),
            "the picks ride the loopback as [i16 value]")) return false;
    if (!expect(!host_view.queue_voice_menu_pick(c2s::CHAT_MESSAGE, 1),
            "only the two menu tags are picks")) return false;
    inmatch::ClientRuntime unjoined("Unjoined");
    if (!expect(!unjoined.queue_voice_menu_pick(c2s::EMOTE_REQUEST, 3),
            "a joiner outside a session queues nothing")) return false;
    // A joiner in session frames the pick on its one-send held queue.
    const std::string client_scrk = "CLIENT-PICK-SCRK";
    inmatch::ClientRuntime joiner("Picker", [] { return uint64_t{0x10203040}; });
    joiner.seed_session(0x55667799u, 1u, client_scrk, "SERVER-PICK-SCRK", 1, 0, 0x0007,
            w::kPlayerInfantryTypeId, 0, 0x00100000u, /*replay_mode=*/false);
    if (!expect(joiner.queue_voice_menu_pick(c2s::RADIO_CALL_REQUEST, 6),
            "a joiner in session queues the pick")) return false;
    std::vector<uint8_t> radio_body;
    for (const std::vector<uint8_t> &datagram : joiner.Client_ProcessNetworkFrame(1)) {
        ProtocolPacketHeader header;
        std::vector<ProtocolMessage> messages;
        if (!decode_client_session(datagram, client_scrk, header, messages)) continue;
        for (const ProtocolMessage &m : messages)
            if (m.tag == c2s::RADIO_CALL_REQUEST) radio_body = m.payload;
    }
    return expect(radio_body == std::vector<uint8_t>({6, 0}),
            "the joiner's radio pick is C2S 0x13 [i16 6] on its session");
}

// The emote clip source: emote_1..emote_10 are 20-tick one-shots, emote_4 is
// unauthored (its slot backfilled with RESET); every other state a 62-tick loop.
struct EmoteSource final : w::IRootMotionSource {
    bool has_clip(int, int state) const override { return state != 118; }
    bool advance(int, int, int32_t &phase, w::RootMotionFrame &out) override {
        ++phase;
        out = {};
        return true;
    }
    int32_t clip_length_ticks(int, int state, int) const override {
        return state >= 115 && state <= 124 ? 20 : 62;
    }
    bool clip_loops(int, int state, int) const override { return state < 115 || state > 124; }
};

// S2C 0x2D on the listen host: the speaker's world body takes emote_N on its
// secondary channel when its map authors it, and the EMO_ voice (flags 9, the
// body prefix) asks the bank for its member at the speaker.
// [orig: NapiNPClientMsg_HandleEmote @0x427efb..0x427f55]
bool run_emote_stamps_the_speaker_body_and_voices_it() {
    ns::LoopbackChannel host_loop;
    inmatch::ClientRuntime host_view(host_loop);
    host_view.view().set_mp_session(true);
    w::World world;
    world.registry.configure_pool(0, 4);
    world.rules.mp_session = true;
    EmoteSource source;
    world.ai.root_motion = &source;
    w::PlayerSpawn spawn;
    spawn.team = 1;
    world.cached.local_player = w::spawn_remote_player(world, spawn);
    spawn.position = {5.0f, 0.0f, 0.0f};
    const w::EntityHandle remote = w::spawn_remote_player(world, spawn);
    w::AiEntity *body = world.ai.for_handle(remote);
    if (!expect(body != nullptr, "the remote player has a world body")) return false;
    std::vector<std::pair<std::string, bool>> voices;
    world.script.voice.set_set_resolver([&](const std::string &name, uint8_t, bool bank)
            -> std::optional<w::ScriptVoiceChannel::SetSelection> {
        voices.emplace_back(name, bank);
        return std::nullopt;
    });
    host_view.view().apply(s2c::EMOTE_BROADCAST, {3, uint8_t(remote.slot()), 0, 0});
    host_view.apply_received_effects(world);
    if (!expect(body->inf.wpn_state == 117 && body->inf.wpn_deferred == 0,
            "emote 3 stamps emote_3 (state 117) with no deferred")) return false;
    if (!expect(voices.size() == 1 && voices[0].first == "BM1_EMO_3" && voices[0].second,
            "the voice asks the bank member of the body's EMO_ set")) return false;
    body->inf.wpn_state = 43;
    host_view.view().apply(s2c::EMOTE_BROADCAST, {4, uint8_t(remote.slot()), 0, 0});
    host_view.apply_received_effects(world);
    return expect(body->inf.wpn_state == 43 && voices.size() == 2,
            "an unauthored emote leaves the channel and still voices");
}

// opennova <-> opennova: a joiner's Emotes / Radio picks leave framed on its
// session as [i16 value], the host's handlers answer them (the sender hears
// its own), and the joiner's fold plays the answer: the emote on its own
// body, the radio line in its chat ring.
// [orig: NetPacket_SendEmoteRequest @0x42C120 -> NapiNPServerMsg_HandleEmoteRequest
//  @0x501E00 -> NapiNPClientMsg_HandleEmote @0x427E90; NetPacket_SendRadioCallRequest
//  @0x42C150 -> NapiNPServerMsg_HandleRadioCall @0x514330 ->
//  NapiNPClientMsg_HandleEntityDeath @0x430C50]
bool run_voice_menu_picks_round_trip_through_a_host() {
    inmatch::NapiNPServerCtx ctx;
    inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
    ctx.is_in_session = 1;
    w::World host_world;
    host_world.registry.configure_pool(0, 8);
    host_world.rules.mp_session = true;
    ctx.world = &host_world;
    w::PlayerSpawn spawn;
    spawn.team = 1;
    const w::EntityHandle joined = w::spawn_remote_player(host_world, spawn);
    ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
    ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
            inmatch::kFirstJoinerDcb, 1, &transport, ns::TransportMode::Client, joined, true));

    const std::string client_scrk = "CLIENT-VOICE-SCRK";
    const std::string server_scrk = "SERVER-VOICE-SCRK";
    inmatch::ClientRuntime joiner("Voice", [] { return uint64_t{0x10203040}; });
    joiner.seed_session(0x55667799u, 1u, client_scrk, server_scrk, 1, 0, joined.packed,
            w::kPlayerInfantryTypeId, 0, 0x00100000u, /*replay_mode=*/false);
    joiner.view().set_mp_session(true);
    w::World world;
    world.registry.configure_pool(0, 8);
    world.rules.mp_session = true;
    EmoteSource source;
    world.ai.root_motion = &source;
    world.cached.local_player = w::spawn_remote_player(world, spawn);
    auto &slot = joiner.view().state().roster[1];
    slot.bound = true;
    slot.entity_slot = int16_t(joined.slot());
    slot.name = "Voice";

    uint32_t frame = 1;
    const auto round_trip = [&](uint8_t tag, int16_t value) {
        std::vector<ProtocolMessage> sent;
        if (!joiner.queue_voice_menu_pick(tag, value)) return sent;
        for (const std::vector<uint8_t> &datagram : joiner.Client_ProcessNetworkFrame(frame++)) {
            ProtocolPacketHeader header;
            std::vector<ProtocolMessage> messages;
            if (!decode_client_session(datagram, client_scrk, header, messages)) continue;
            for (ProtocolMessage &m : messages)
                if (m.tag == tag) sent.push_back(m);
        }
        inmatch::ServerDispatchInputs inputs;
        inputs.server_ctx = &ctx;
        for (const ProtocolMessage &reply : inmatch::dispatch_session_replies(ctx.config,
                     ctx.np_protocol.connection_list[0], sent, 100,
                     ctx.np_protocol.connection_list, &host_world, inputs))
            joiner.view().apply(reply.tag, reply.payload);
        joiner.apply_received_effects(world);
        return sent;
    };
    std::vector<ProtocolMessage> sent = round_trip(c2s::EMOTE_REQUEST, 3);
    if (!expect(sent.size() == 1 && sent[0].payload == std::vector<uint8_t>({3, 0}),
            "the emote pick leaves as C2S 0x14 [i16 3]")) return false;
    const w::AiEntity *own = world.ai.for_handle(world.cached.local_player);
    if (!expect(own != nullptr && own->inf.wpn_state == 117,
            "the host's own-copy 0x2D plays emote_3 on the joiner's body")) return false;
    sent = round_trip(c2s::RADIO_CALL_REQUEST, 7);
    if (!expect(sent.size() == 1 && sent[0].payload == std::vector<uint8_t>({7, 0}),
            "the radio pick leaves as C2S 0x13 [i16 7]")) return false;
    const std::vector<ns::ClientChatLine> lines = joiner.view().drain_chat_lines();
    return expect(lines.size() == 1 && lines[0].channel == 2 && lines[0].text == "Voice: RAD_7",
            "the host's own-copy 0x6D posts the joiner's radio line");
}

bool run_host_client_refreshes_visible_players() {
    ns::LoopbackChannel host_loop;
    inmatch::ClientRuntime host_view(host_loop);
    auto pull = [&]() {
        std::vector<ns::Datagram> out;
        ns::Datagram dg;
        while (host_loop.host_recv(dg)) out.push_back(dg);
        return out;
    };
    host_loop.host_send(s2c::WORLD_STATE_LOAD, {});
    host_view.Client_ProcessNetworkFrame();
    std::vector<ns::Datagram> c2s_out = pull();
    if (!expect(c2s_out.size() == 2 && c2s_out[0].tag == c2s::PLAYER_SYNC_REQUEST &&
            c2s_out[0].body == std::vector<uint8_t>({0x00, 0xF7, 0x5C}) &&
            c2s_out[1].tag == c2s::VISIBLE_PLAYERS_REQUEST && c2s_out[1].body.empty(),
            "the world-state load queues the 0x22 {0, 0x5CF7} + 0x23 pair")) return false;
    host_loop.host_send(s2c::SPAWN_SLOT_NOTICE, {3});
    host_view.Client_ProcessNetworkFrame();
    c2s_out = pull();
    if (!expect(c2s_out.size() == 2 && c2s_out[0].tag == c2s::PLAYER_SYNC_REQUEST &&
            c2s_out[0].body == std::vector<uint8_t>({0x03, 0xF7, 0x1C}) &&
            c2s_out[1].tag == c2s::VISIBLE_PLAYERS_REQUEST,
            "another slot's join notice queues its refresh pair")) return false;
    host_loop.host_send(s2c::SPAWN_SLOT_NOTICE, {0});
    host_view.Client_ProcessNetworkFrame();
    if (!expect(pull().empty(), "the own slot's notice queues nothing")) return false;

    auto &state = host_view.view().state();
    state.roster[3].bound = true;
    state.roster[3].entity_slot = 4;
    state.roster[3].downed_revive_seconds = 30;
    state.roster[5].bound = true;
    state.roster[5].entity_slot = 6;
    state.roster[5].downed_revive_seconds = 30;
    host_loop.host_send(s2c::VISIBLE_PLAYERS, {0x01, 0x03, 0x04, 0x00});
    for (int frame = 0; frame < 63; ++frame) host_view.Client_ProcessNetworkFrame();
    if (!expect(state.visible_players.size() == 1 && state.visible_players[0].slot == 3,
            "the snapshot lands in the pointer table")) return false;
    return expect(state.roster[3].downed_revive_seconds == 29 &&
            state.roster[5].downed_revive_seconds == 30,
            "the 1 Hz revive countdown walks the table's slots only");
}

bool run_contextual_radio_keys_match_retail() {
    w::World world;
    world.registry.configure_pool(1, 4);
    w::Entity speaker; speaker.player_class = 5;
    if (!expect(w::radio_call_key(world, speaker, 9, 3, 0x30020, false) == "BM1_RAD_MEDIC1",
            "on-foot contextual call resolves the body and class")) return false;
    if (!expect(w::radio_call_key(world, speaker, 10, 6, 0x10004, false) == "RAD_MP_CTF2",
            "mode context precedes class context")) return false;
    world.registry.configure_pool(3, 1);
    w::Entity zone; zone.item_id = 6006; zone.has_item_def = true; zone.bound_radius = 100.0f;
    const auto zone_handle = world.registry.spawn(3, zone);
    if (!expect(w::radio_call_key(world, speaker, 9, 6, 0x10001, false) == "RAD_MP_TKTH1",
            "an interior neutral zone supplies the hill context")) return false;
    // The probe's value itself (the HUD's "In the Zone" gate reads it too).
    if (!expect(w::capture_zone_max_coverage(world, speaker) == 100,
            "the hill centre covers 100 percent")) return false;
    speaker.position.x = 50.0;
    if (!expect(w::capture_zone_max_coverage(world, speaker) == 50,
            "halfway out covers 50 percent")) return false;
    speaker.position.x = 99.5;
    if (!expect(w::capture_zone_max_coverage(world, speaker) == 0,
            "the outermost ring scores zero")) return false;
    if (!expect(w::radio_call_key(world, speaker, 9, 6, 0x10001, false) == "RAD_MEDIC1",
            "a sub-one-percent coverage ring falls back to the class context")) return false;
    speaker.position.x = 0;
    world.registry.get(zone_handle)->bound_radius = 500.0f;
    if (!expect(w::radio_call_key(world, speaker, 9, 6, 0x10001, false) == "RAD_MEDIC1",
            "coverage percentage multiplies in signed 32 bits before dividing")) return false;
    world.registry.get(zone_handle)->bound_radius = 200.0f;
    speaker.position.x = 199.5;
    if (!expect(w::radio_call_key(world, speaker, 9, 6, 0x10001, false) == "RAD_MP_TKTH1",
            "overflowed squared distance preserves the x87 integer-indefinite shift")) return false;
    w::Entity hull; hull.item_id = 100; hull.has_item_def = true; hull.item_unit_type = 12;
    speaker.mount_target = world.registry.spawn(1, hull);
    speaker.mount_type = w::SeatType::Passenger;
    if (!expect(w::radio_call_key(world, speaker, 9, 3, 0, false) == "BM1_RAD_WL_1",
            "dirt-bike passenger radio suffix is remapped")) return false;
    auto *mount = world.registry.get(speaker.mount_target);
    mount->item_unit_type = 2;
    speaker.mount_type = w::SeatType::Driver;
    return expect(w::radio_call_key(world, speaker, 10, 3, 0, false) == "BM1_RAD_WL_2",
            "tank driver's second radio uses the wheeled-vehicle remap");
}

// The 0x21 source is the credited attacker; a pool-1..3 source resolves to the
// joiner's materialized twin, so the explosion's row-5 impact sound reads its
// +0x26C damage ammo and bms id rather than the row snapshot's zero defaults.
bool run_explosion_sound_reads_the_pool_twin_damage_ammo() {
    inmatch::ClientRuntime runtime("ExplosionAudio");
    w::World world;
    world.rules.logic_authority = false;
    world.rules.mp_session = true;
    world.registry.configure_pool(1, 8);
    w::Entity attacker;
    attacker.item_id = 1291; attacker.has_item_def = true;
    attacker.bms_id = 4242;
    attacker.squib.damage_ammo_index = 2;
    const auto source = world.registry.spawn(1, attacker);
    ns::ClientEntityState &row = runtime.state().upsert(source.packed);
    row.type_id = 1291; row.cls = EntityClass::Vehicle;
    world.tables.ammo.entries.resize(3);
    world.tables.ammo.entries[0].valid = true;
    world.tables.ammo.entries[0].impact_effects[5].sound = "DEFAULT_BOOM";
    world.tables.ammo.entries[0].impact_effects[5].authored = true;
    world.tables.ammo.entries[2].valid = true;
    world.tables.ammo.entries[2].impact_effects[5].sound = "ITEM_BOOM";
    world.tables.ammo.entries[2].impact_effects[5].authored = true;
    world.out.fire_sounds.set_listener({});
    ExplosionEffectRecord effect;
    effect.source = source.packed; effect.count = 4;
    runtime.view().apply(s2c::EXPLOSION_EFFECT, encode_explosion_effect(effect));
    runtime.apply_received_effects(world);
    const auto ready = world.out.fire_sounds.drain();
    return expect(ready.size() == 1 && ready[0].set_name == "ITEM_BOOM" && ready[0].source_bms_id == 4242,
            "a pool-1 explosion source resolves through its twin's damage ammo and bms id");
}

// The explosion sound seeds from ammo def 0's static bank row 5 (the table's
// baked default_explosion_sound) and is overwritten only by an AUTHORED tag-5
// row of the credited ammo: an absent row plays the fallback, an authored
// 'none' row plays nothing. [orig: AmmoDef_GetExplosionRadius @0x409770 —
// `radius = dword_A2EB80` @0x40978c, the tag-5 overwrite @0x4097ab]
bool run_explosion_sound_falls_back_to_ammo_zero_bank_row_five() {
    const auto play = [](bool authored_none) -> std::string {
        inmatch::ClientRuntime runtime("ExplosionAudioFallback");
        w::World world;
        world.rules.logic_authority = false;
        world.rules.mp_session = true;
        world.registry.configure_pool(1, 8);
        w::Entity attacker;
        attacker.item_id = 1291; attacker.has_item_def = true;
        attacker.squib.damage_ammo_index = 2;
        const auto source = world.registry.spawn(1, attacker);
        ns::ClientEntityState &row = runtime.state().upsert(source.packed);
        row.type_id = 1291; row.cls = EntityClass::Vehicle;
        world.tables.ammo.entries.resize(3);
        world.tables.ammo.entries[0].valid = true;
        world.tables.ammo.default_explosion_sound = "DEFAULT_BOOM";
        world.tables.ammo.entries[2].valid = true;
        world.tables.ammo.entries[2].impact_effects[5].authored = authored_none;
        world.out.fire_sounds.set_listener({});
        ExplosionEffectRecord effect;
        effect.source = source.packed; effect.count = 4;
        runtime.view().apply(s2c::EXPLOSION_EFFECT, encode_explosion_effect(effect));
        runtime.apply_received_effects(world);
        const auto ready = world.out.fire_sounds.drain();
        return ready.size() == 1 ? ready[0].set_name : std::string("<none>");
    };
    return expect(play(false) == "DEFAULT_BOOM",
                   "an ammo without a tag-5 row plays ammo def 0's bank row-5 sound") &&
           expect(play(true) != "DEFAULT_BOOM",
                   "an authored 'none' tag-5 row overwrites the seeded default with silence");
}

// A remote player's death edge screams on the client itself: the body-model
// composite "<prefix>_DEATH" from its anim slot, "_DEATH_K" on a night
// mission, at the body origin; the self row's edge stays silent (the local
// body screams through its own motor). [orig: Entity_UpdateInfantryPlayerBody
// @0x4b4c4a..0x4b4c54 -> SoundProfile_FindByEntityAndType @0x528180]
bool run_remote_death_edge_screams() {
    const auto scream_of = [](bool night, uint16_t self) {
        inmatch::ClientRuntime runtime("DeathScream");
        w::World world;
        if (night)
            world.tables.mission_attrib_flags = w::MissionTables::kMissionAttribEnableNVG;
        runtime.view().set_remote_motion_mode(true);
        ns::ClientEntityState &row = runtime.state().upsert(0x0011u);
        row.type_id = w::kPlayerInfantryTypeId;
        row.cls = EntityClass::Player;
        row.spawn_anim_slot = 4;
        row.net_has_compact = true;
        row.state_flags_known = true;
        row.state_flags = 0x02;
        row.net_health_zero = true;
        row.x = 7 << 16;
        row.net_smooth_target[0] = row.x; // the staged corpse pose: no chase step
        runtime.view().tick_remote_motion(self);
        runtime.tick_remote_stance_sounds(world);
        const auto &sounds = world.out.slot_sounds;
        return sounds.size() == 1 && sounds[0].source_handle == 0x0011u &&
                        sounds[0].pos[0] == (7 << 16)
                ? std::string(sounds[0].set_name)
                : std::string("<none>");
    };
    return expect(scream_of(false, 0xFFFF) == "BM4_DEATH",
                   "a remote player's death edge plays its body-model scream") &&
           expect(scream_of(true, 0xFFFF) == "BM4_DEATH_K",
                   "a night mission plays the _DEATH_K composite") &&
           expect(scream_of(false, 0x0011) == "<none>",
                   "the self row's edge stays silent");
}

// The remote stance latch's prone clear reads only a MOUNT parent's def: a
// deck-standing remote (carrier = the ground link, mount_bone 0) keeps TO_PRONE.
bool run_remote_stance_sound_parent_is_the_mount_only() {
    inmatch::ClientRuntime runtime("StanceAudio");
    w::World world;
    world.registry.configure_pool(1, 4);
    w::Entity deck; deck.item_id = 1291; deck.has_item_def = true;
    const auto carrier = world.registry.spawn(1, deck);
    const auto make_row = [&](uint16_t handle, uint8_t mount_bone) {
        ns::ClientEntityState &row = runtime.state().upsert(handle);
        row.type_id = w::kPlayerInfantryTypeId; row.cls = EntityClass::Player;
        row.carrier_handle = carrier.packed; row.mount_bone = mount_bone;
        row.net_stance_bits = 1; // prone
    };
    const auto standing = w::EntityHandle::make(0, 1);
    make_row(standing.packed, 0);
    make_row(w::EntityHandle::make(0, 2).packed, 3);
    runtime.tick_remote_stance_sounds(world);
    const auto &sounds = world.out.slot_sounds;
    return expect(sounds.size() == 1 && std::string(sounds[0].set_name) == "TO_PRONE" &&
            sounds[0].source_handle == standing.packed,
            "only the deck-standing remote plays TO_PRONE; a mount parent's def suppresses it");
}

// MP A&S radio context: the NEAREST qualifying capture entry decides, and its
// Q16 coverage is zero on the radius itself even when a farther entry covers.
bool run_radio_zone_context_uses_the_nearest_entry_coverage() {
    ns::LoopbackChannel loop;
    inmatch::ClientRuntime runtime(loop);
    runtime.view().set_mp_session(true);
    std::vector<uint8_t> config(51, 0);
    config[12] = 0x10; config[14] = 0x01; // fields[3] = 0x10010 (MP A&S)
    loop.host_send(s2c::SESSION_CONFIG, config); // the joiner learns g_GameType on its receive path
    runtime.Client_ProcessNetworkFrame();
    w::World world;
    world.rules.mp_session = true;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(3, 4);
    w::Entity person; person.item_id = 11; person.has_item_def = true;
    world.cached.local_player = world.registry.spawn(0, person);
    const auto remote = w::EntityHandle::make(0, 1);
    runtime.view().apply(s2c::ENTITY_SPAWN_BATCH,
            make_organic_spawn(remote.packed, "Caller", 0, 0, 0, 0, 1, 1));
    const auto spawn_zone = [&](float x, uint16_t radius) {
        w::Entity zone; zone.item_id = 6006; zone.has_item_def = true;
        zone.position = {x, 0.0f, 0.0f}; zone.zone_radius = radius;
        return world.registry.spawn(3, zone);
    };
    const auto edge = spawn_zone(100.0f, 100);  // dist == radius: coverage 0
    const auto inner = spawn_zone(150.0f, 200); // farther, but covered
    for (const auto handle : {edge, inner}) {
        loop.host_send(s2c::ZONE_TIMER_WINDOW, zone_timer_window_body(handle.packed, 1, 4, 10, 40, 2));
        auto &slot = runtime.state().minimap.transient[handle.slot()];
        slot.active = true; slot.handle = handle.packed;
        slot.remaining_ticks = 1000; // the transient bank clears a slot at zero each frame
    }
    runtime.Client_ProcessNetworkFrame();
    std::vector<std::string> voices;
    world.script.voice.set_set_resolver([&](const std::string &name, uint8_t, bool)
            -> std::optional<w::ScriptVoiceChannel::SetSelection> {
        voices.push_back(name); return std::nullopt;
    });
    const auto call = [&] {
        loop.host_send(s2c::TRACKED_PLAYER_VOICE, {9, uint8_t(remote.slot()), 255, 255});
        runtime.Client_ProcessNetworkFrame();
        runtime.apply_received_effects(world);
    };
    call();
    if (!expect(voices == std::vector<std::string>{"BM1_RAD_ENG1"},
            "the nearest entry sits on its radius: zero coverage, class context")) return false;
    runtime.state().find(remote.packed)->x = 50 << 16;
    call();
    return expect(voices.size() == 2 && voices.back() == "BM1_RAD_MP_A&S1",
            "inside the nearest entry the A&S context applies");
}

// D-NET-64 zero-write leg: retail's read side stores every decoded coordinate
// into entity+700/704/708 unconditionally, zero included, so an origin steer
// point (a group-5 decoy at 0/0/0, or a group-3 lock whose steer point is the
// origin) is a real steer point that seeds and re-aims the flight. The port
// used to skip all-zero writes. Drives ClientReplicaPipeline::apply(0x44, ...)
// through the public 0x44 fold with the 5-B §5.36 sub-header + the real §5.15 encoder.
// [orig: Entity_SerializeGuidedMissileState @0x447C50 read-full groups 3/4/5]
bool run_guided_zero_steer_point_is_stored() {
	ns::ClientReplicaPipeline view([](uint16_t) { return EntityClass::Player; });
	auto make_routed = [](uint16_t shooter, int16_t net_id, GuidedFieldGroup group,
			const GuidedRecord &rec) {
		const auto id = static_cast<uint16_t>(net_id);
		std::vector<uint8_t> body{static_cast<uint8_t>(shooter), static_cast<uint8_t>(shooter >> 8),
				static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
				static_cast<uint8_t>(group)};
		const std::vector<uint8_t> payload =
				encode_guided_field_group(GuidedMode::WriteFull, group, rec);
		body.insert(body.end(), payload.begin(), payload.end());
		return body;
	};
	GuidedRecord origin;
	origin.target_slot = 0xFFFF;
	origin.pos_x = origin.pos_y = origin.pos_z = 0;
	view.apply(0x44, make_routed(0x0002, 5, GuidedFieldGroup::Pos, origin));
    auto round = std::make_unique<opennova::world::LiveRound>();
    round->active = true; round->shot_seq = 5; round->max_age_ticks = 620;
    round->guided_family = opennova::world::GuidedFamily::Stinger;
    round->guided.steer[0] = 123;
    view.set_guided_round_resolver([&](int16_t id) { return id == 5 ? round.get() : nullptr; });
    view.apply(0x44, make_routed(0x0002, 4, GuidedFieldGroup::Pos, origin));
    if (!expect(round->guided.steer[0] == 123, "guidance cannot create a missing missile")) return false;
    view.apply(0x44, make_routed(0x0002, 5, GuidedFieldGroup::Pos, origin));
    auto *m = &round->guided;
    if (!expect(m->steer[0] == 0 && m->steer[1] == 0 && m->steer[2] == 0 && m->target == 0xFFFF,
            "guidance stores the origin on an existing round")) return false;
	// A non-zero lock moves the steer point; a following zero lock moves it
	// back to the origin instead of leaving the stale point in place.
	GuidedRecord lock;
	lock.target_slot = 0x1003;
	lock.pos_x = 0x10000;
	lock.pos_y = 0x20000;
	lock.pos_z = 0x30000;
	view.apply(0x44, make_routed(0x0002, 5, GuidedFieldGroup::TargetPos, lock));
	if (!expect(m->steer[0] == 0x10000 && m->steer[1] == 0x20000 && m->steer[2] == 0x30000 &&
					m->target == 0x1003,
			"guided: a non-zero group-3 lock stores its steer point and target")) return false;
	GuidedRecord zero_lock = lock;
	zero_lock.pos_x = zero_lock.pos_y = zero_lock.pos_z = 0;
	view.apply(0x44, make_routed(0x0002, 5, GuidedFieldGroup::TargetPos, zero_lock));
    if (!expect(m->steer[0] == 0 && m->steer[1] == 0 && m->steer[2] == 0 && m->target == 0x1003,
            "zero coordinates overwrite the old steer point")) return false;
    view.apply(0x44, make_routed(0, 5, GuidedFieldGroup::Status, origin));
    view.apply(0x44, make_routed(0, 5, GuidedFieldGroup::TargetPos, lock));
    if (!expect((m->flags & 1) && round->det_at_expiry && m->steer[0] == 0,
            "guidance cannot revive a terminated round")) return false;
    round->guided = {}; round->det_at_expiry = false;
    view.apply(0x44, make_routed(0, 5, GuidedFieldGroup::TargetPos, lock));
    return expect(m->steer[0] == 65536 && !(m->flags & 1), "a fresh lifetime admits guidance");
}

int main() {
	const bool ok = run_guided_zero_steer_point_is_stored() &&
	                run_connection_indicators_on_a_joiner() &&
	                run_connection_indicators_on_the_host_client() &&
	                run_radio_events_preserve_order_chat_and_mute_state(false) &&
                    run_radio_events_preserve_order_chat_and_mute_state(true) &&
                    run_emote_and_local_chat_track_the_speaker(false) &&
                    run_emote_and_local_chat_track_the_speaker(true) &&
                    run_voice_menu_picks_ride_the_session() &&
                    run_emote_stamps_the_speaker_body_and_voices_it() &&
                    run_voice_menu_picks_round_trip_through_a_host() &&
                    run_host_client_refreshes_visible_players() &&
                    run_contextual_radio_keys_match_retail() &&
                    run_charattr_challenge_table_matches_retail() &&
	                run_spectator_clientauth_and_state_latch() &&
	                run_seeded_objective_layout_hint() &&
	                run_challenge_diagnostics_pass_through_the_runtime() &&
	                run_fire_queue_stamps_runtime_tick() &&
	                run_retail_post_auth_prelude() &&
	                run_novaworld_join_tokens_ride_the_wire() &&
	                run_early_sync_tail_latch() &&
	                run_client_reducer_preserves_packet_message_order() &&
	                run_duplicate_s2c_session_one_shot_is_not_replayed() &&
	                run_roundtrip() &&
	                run_roundtrip_with_spawn_zones(/*under_send_holdoff=*/false) &&
	                run_roundtrip_with_spawn_zones(/*under_send_holdoff=*/true) &&
	                run_joiner_remote_reload_stamps_before_same_frame_body_tick() &&
	                run_host_client_discards_authority_owned_reload_echoes() &&
	                run_host_zone_timer_value_matches_retail_entry() &&
	                run_zone_timer_channels_share_one_retail_entry() &&
	                run_zone_presence_updates_only_a_tracked_window() &&
	                run_roster_revive_countdown_ticks_once_per_63_frames() &&
	                run_medic_request_queues_one_reliable_0x2e() &&
	                run_session_vars_exp_fanfare_walk() &&
	                run_zone_timer_uses_wrapping_dword_arithmetic_and_signed_clamps() &&
	                run_joiner_zone_timer_preserves_mixed_wire_order() &&
	                run_host_as_client() &&
	                run_host_startup_seeds_mounted_no_callback_carrier() &&
	                run_host_startup_maps_claymore_preference() &&
	                run_host_startup_maps_no_tracers_rule() &&
	                run_host_pump_hook_observes_remote_before_first_tick() &&
	                run_periodic_request_quartet_is_answered() &&
	                run_reverse_rtt_probe_is_echoed() &&
	                run_medic_reviving_plays_both_receive_cues() &&
                    run_flag_event_audio_and_feed_drain_independently() &&
                    run_explosion_sound_reads_the_pool_twin_damage_ammo() &&
                    run_explosion_sound_falls_back_to_ammo_zero_bank_row_five() &&
                    run_remote_stance_sound_parent_is_the_mount_only() &&
                    run_remote_death_edge_screams() &&
                    run_radio_zone_context_uses_the_nearest_entry_coverage() &&
	                run_direct_uplink_framing_is_transient() &&
	                run_network_spawn_does_not_mutate_loaded_model_snapshot() &&
	                run_split_batch_keeps_deployment_pick_ack_causal() &&
	                run_unrelated_loadout_cannot_revive_dead_client() &&
	                run_live_frame_uses_wall_clock_and_batches_mount_requests() &&
	                run_queued_gameplay_survives_a_death_before_the_boundary() &&
	                run_mounted_slot_select_and_reload_producers() &&
	                run_same_packet_holdoff_keeps_first_admission_boundary_open() &&
	                run_missing_sequence_request_leaves_from_the_receive_pump() &&
	                run_resend_answer_and_pong_leave_from_the_receive_pump() &&
	                run_queued_stance_waits_for_the_send_boundary() &&
	                run_settings_update_preserves_active_holdoff_countdown() &&
	                run_settings_send_holdoff_blocks_exact_frame_count() &&
	                run_send_holdoff_defers_due_housekeeping() &&
	                run_finite_quality_retention_expires_on_flush_310() &&
	                run_world_state_load_pools_reach_the_runtime() &&
	                run_start_resets_reusable_runtime_state() &&
	                run_joiner_correlates_handshake_echoes() &&
	                run_tick_seed_anchors_the_client_clock() &&
	                run_end_round_header_pulls_complete_board() &&
	                run_end_round_named_header_kicks_and_folds() &&
	                run_class_allow_mask_follows_retail_host() &&
	                run_unknown_scoreboard_row_requests_player_sync() &&
	                run_team_latch_is_falsifiable() &&
	                run_player_sync_ack_walks_inclusive_roster_capacity() &&
	                run_padding_echo_retail_clamp(/*requested_len=*/0, /*expected_body=*/12) &&
	                run_padding_echo_retail_clamp(/*requested_len=*/300, /*expected_body=*/300) &&
	                run_padding_echo_retail_clamp(/*requested_len=*/2000, /*expected_body=*/512) &&
	                run_joiner_admits_exact_retail_message_prefix() &&
	                run_joiner_splits_to_fill_remaining_packet_space() &&
	                run_rtt_pong_fills_the_client_ring() &&
	                run_server_ping_answers_and_measures() &&
	                run_world_state_load_bursts_on_every_0x0f() &&
	                run_chat_uplink_api() && run_host_chat_uplink() &&
	                run_client_quality_level_folds_the_ping_ring() &&
	                run_client_quality_frame_pressure_follows_the_frame_rate() &&
	                run_novaworld_exit_on_a_joiner() &&
	                run_mission_exit_routes() &&
	                run_joiner_goodbye_tears_down_host();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
