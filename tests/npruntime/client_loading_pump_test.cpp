// The joiner's send cadence through the retail loading loops (D-NET-254).
//
// A stock client does not reach its in-match Client_ProcessNetworkFrame until
// mission loading ends. Before that, each admission stage runs inside its own
// retail loop, and each loop calls the client send pump its own way:
//
//   join state machine states 4..6   one receive + one send per UI frame (the
//                                    build still waits for the holdoff
//                                    countdown, so a frame cadence like the
//                                    main loop);
//   SaveFile_SendAndWaitForServerAck  the send pump only when MORE than 50 ms
//                                    passed since its last call;
//   InitRandomSeedOrRequest          a busy spin: receive + send every pass;
//   NapiClient_SendPlayerSyncAndWaitForSyncTail     the send pump only after MORE than 100 ms;
//   NapiClient_WaitForGameStart      a busy spin;
//   Game_StartMission's 0x0F wait    a busy spin;
//   Client_ProcessNetworkFrame       the holdoff-gated per-frame send block.
//
// A spin pass costs microseconds, so the countdown a NovaWorld host dictates
// (12) runs out between datagrams and every reply leaves on the frame that
// produced it; the timed loops send on their own wall-clock pace whatever the
// countdown says. This drives a ClientRuntime through the whole admission
// against a scripted host on a virtual 16 ms clock and pins the frame on which
// each admission packet leaves.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56a320 (state 5 @0x56a5bf..
//  0x56a5dd, state 6 -> MultiPlayer_PumpJoinVerificationFrame @0x424740); SaveFile_SendAndWaitForServerAck
//  @0x5204b0; CNapiGameSession_InitRandomSeedOrRequest @0x51e8f0;
//  NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42cb20; NapiClient_WaitForGameStart @0x42cc10;
//  Game_StartMission's final loop @0x52628d..0x5262df; the main-loop gate
//  Client_ProcessNetworkFrame @0x42c3dd]

#include "joiner_mission_start.h" // the harness's end of a released mission start

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/joiner_role.h> // the release frame through the role (D-NET-526)
#include <runtime/inmatch/remote_admin.h> // the admin's GET over the joiner's clock
#include <runtime/mission/mission_kernel.h>
#include <base/io/le.h>
#include <base/gameprofile/game_type.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h> // AdminSession
#include <net/protocol/protocol.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/session_ping.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../common/test_expect.h"
#include "../common/title_fixture.h"

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

using test_expect::expect;

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

// A 0x60 / 0x64 chunk: [u32 id][u32 total][u32 offset][size bytes].
std::vector<uint8_t> transfer_chunk(uint32_t transfer_id, uint32_t total_size,
		uint32_t offset, uint32_t size, uint8_t fill) {
	std::vector<uint8_t> body;
	for (uint32_t value : {transfer_id, total_size, offset})
		for (int shift = 0; shift < 32; shift += 8)
			body.push_back(static_cast<uint8_t>(value >> shift));
	body.insert(body.end(), size, fill);
	return body;
}

std::vector<uint8_t> slot_assignment(uint8_t team) {
	std::vector<uint8_t> body(24, 0);
	body[17] = 0x01;
	body[18] = 0x18;
	body[23] = team;
	return body;
}

std::vector<uint8_t> full_player_info() {
	const std::string fields[] = {
			"LoadingPump", "", "Cadence Host", "Cadence Mission", "CADENCE.BMS"};
	std::vector<uint8_t> body;
	for (const std::string &field : fields) {
		body.insert(body.end(), field.begin(), field.end());
		body.push_back(0);
	}
	for (uint8_t b : {0x20, 0x00, 0x03, 0x00, 0x00}) body.push_back(b);
	const std::string game = "Joint Operations";
	body.insert(body.end(), game.begin(), game.end());
	body.push_back(0);
	return body;
}

std::vector<uint8_t> player_list(std::initializer_list<PlayerListEntry> players) {
	PlayerListFrame frame;
	frame.players.assign(players.begin(), players.end());
	frame.teams.resize(size_t(frame.team_count) + 1);
	frame.in_game_count = static_cast<uint8_t>(frame.players.size());
	return encode_player_list(frame);
}

// The CS update records: the two template-shaped admission settings and a
// host's direction-1 field-3 dictation.
ProtocolMessage settings_record(uint8_t direction) {
	return make_protocol_message(
			0x00, {direction, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0);
}
ProtocolMessage holdoff_record(uint8_t period) {
	return make_protocol_message(
			0x00, {0x01, 0x08, 0x00, 0x00, 0x00, period, 0x00, 0x00, 0x00}, 0xA0);
}

struct Harness {
	static constexpr uint32_t kServerKey = 0x0C0FFEE1u;
	uint64_t now_ms = 100000;
	uint32_t tick = 1;
	inmatch::ClientRuntime client{"LoadingPump", [this] { return now_ms; }};
	bool client_jo = test_titles::jo_folds(client);
	std::string server_scrk = "SERVER-LOADING-PUMP-SCRK";
	ClientAuth auth;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	bool uplink = false; // the frame builds a C2S 0x0C body (the game loop's uplink)
	// The test runs the mission start's load itself after the S2C 0x1A releases it, ending
	// it with push_mission_start_end; without it the harness has no load to run and ends the
	// released mission start right after the 0x1A's frame (push_released_mission_start).
	bool load_declared = false;

	Harness() { client.set_protocol(&kJo); }

	// One client frame on the 16 ms clock, with `incoming` delivered first.
	std::vector<std::vector<uint8_t>> frame(
			const std::vector<ProtocolMessage> *incoming = nullptr) {
		now_ms += 16;
		if (incoming != nullptr) {
			const std::vector<uint8_t> dg =
					frame_server_session(server_tx, server_scrk, auth.ck, *incoming);
			client.receive(dg.data(), dg.size());
		}
		std::vector<std::vector<uint8_t>> sent =
				uplink ? client.Client_ProcessNetworkFrame(PlayerExtendedUplink{}, tick++)
				       : client.Client_ProcessNetworkFrame(tick++);
		if (!load_declared)
			push_released_mission_start(client, [&sent](std::vector<uint8_t> datagram) {
				sent.push_back(std::move(datagram));
			});
		return sent;
	}
	std::vector<std::vector<uint8_t>> frame(std::vector<ProtocolMessage> incoming) {
		return frame(&incoming);
	}

	// The tags of every message in one sent datagram (header-only = empty).
	bool tags_of(const std::vector<uint8_t> &datagram, std::vector<uint8_t> &tags) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(datagram, auth.scrk, header, messages)) return false;
		tags.clear();
		for (const ProtocolMessage &m : messages) tags.push_back(m.tag);
		return true;
	}
	// Each sent datagram's tags against `want`, one list per datagram, in order.
	bool packets(const std::vector<std::vector<uint8_t>> &sent,
			const std::vector<std::vector<uint8_t>> &want) {
		std::vector<std::vector<uint8_t>> got;
		for (const std::vector<uint8_t> &dg : sent) {
			std::vector<uint8_t> tags;
			if (!tags_of(dg, tags)) tags = {0xFF, 0xFF};
			got.push_back(tags);
		}
		if (got == want) return true;
		std::fprintf(stderr, "  sent %zu datagram(s):", got.size());
		for (const std::vector<uint8_t> &tags : got) {
			std::fprintf(stderr, " [");
			for (uint8_t tag : tags) std::fprintf(stderr, " %02X", tag);
			std::fprintf(stderr, " ]");
		}
		std::fprintf(stderr, "\n");
		return false;
	}
	bool one_packet(const std::vector<std::vector<uint8_t>> &sent,
			std::vector<uint8_t> want) {
		std::vector<uint8_t> tags;
		if (sent.size() == 1 && tags_of(sent[0], tags) && tags == want) return true;
		std::fprintf(stderr, "  sent %zu datagram(s):", sent.size());
		for (const std::vector<uint8_t> &dg : sent) {
			std::fprintf(stderr, " [");
			if (tags_of(dg, tags))
				for (uint8_t tag : tags) std::fprintf(stderr, " %02X", tag);
			else
				std::fprintf(stderr, " ?");
			std::fprintf(stderr, " ]");
		}
		std::fprintf(stderr, "\n");
		return false;
	}

	bool handshake() {
		const std::vector<uint8_t> hello_dg = client.start();
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ClientHello hello;
		if (!nw_decode_inbound(hello_dg.data(), hello_dg.size(), opcode, body) ||
				!parse_client_hello(body.data(), body.size(), hello))
			return false;
		ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
		server_hello.hk = 0x13572468u;
		const std::vector<uint8_t> server_hello_dg = nw_encode_outbound(
				SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
		client.receive(server_hello_dg.data(), server_hello_dg.size());
		// The ServerHello's frame records the session; the next frame starts the connect
		// state; the frame after it, past that start, sends the 0x42 (D-NET-851).
		if (!frame().empty() || !frame().empty()) return false;
		const std::vector<std::vector<uint8_t>> auth_frame = frame();
		if (auth_frame.size() != 1 ||
				!nw_decode_inbound(auth_frame[0].data(), auth_frame[0].size(), opcode, body) ||
				!parse_client_auth(body.data(), body.size(), auth))
			return false;
		ServerAuth server_auth = build_server_auth(
				auth, 0x7F000001u, 32769, kServerKey, server_scrk, "", "", "", false);
		server_auth.mi = 4;
		const std::vector<uint8_t> server_auth_dg = nw_encode_outbound(
				SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
		client.receive(server_auth_dg.data(), server_auth_dg.size());
		return frame().empty();
	}
};

bool run_admission_follows_the_retail_loading_loops() {
	Harness h;
	if (!expect(h.handshake(), "handshake reaches the post-auth settings leg")) return false;

	// States 4..6 of the join state machine run one receive and one send per UI
	// frame. The template holdoff is 0, so every frame builds.
	if (!expect(h.frame({settings_record(0), settings_record(1)}).size() == 2,
			"the settings packet's ACK and the JOIN leave on their frame"))
		return false;
	if (!expect(h.frame().empty(), "an idle state-5 frame sends nothing"))
		return false;
	std::vector<std::vector<uint8_t>> sent = h.frame({make_protocol_message(0x00, {})});
	if (!expect(sent.size() == 2, "the JOIN ack's ACK and the 0x01 leave on their frame"))
		return false;
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x02, probe)}), {0x02}),
			"the padding echo leaves on its frame"))
		return false;

	// The 0x02 reply dictates the NovaWorld holdoff. State 6 is still a
	// per-frame loop: the first boundary opens (counter 0), the next is twelve
	// frames out.
	if (!expect(h.one_packet(h.frame({holdoff_record(12), make_protocol_message(0x03, {0x01})}),
	                    {}) &&
	                    h.client.send_holdoff_countdown() == 12,
			"the dictation's packet is ACKed at the open state-6 boundary and arms 12"))
		return false;
	if (!expect(h.frame({make_protocol_message(0x03, {0x00})}).empty(),
			"state 6 holds its next ACK behind the countdown, a frame loop like the main one"))
		return false;

	// The game-start flag ends state 6. Game_StartMission's prologue pump and
	// SaveFile's entry pump find the countdown running, so nothing builds; the
	// loop then sends only once MORE than 50 ms passed since its entry.
	sent = h.frame({settings_record(0), settings_record(1),
			make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	if (!expect(sent.empty(), "SaveFile's entry pump builds nothing while the countdown runs"))
		return false;
	for (int held = 1; held <= 3; ++held) {
		if (!expect(h.frame().empty(), "SaveFile holds its send pump for 50 ms (16/32/48)"))
			return false;
	}
	if (!expect(h.one_packet(h.frame(), {0x4E, 0x03, 0x48, 0x47, 0x33}),
			"the first send pump past 50 ms carries the grouped admission request"))
		return false;

	// A non-final server-info chunk's 0x33 re-request waits for the next pace.
	if (!expect(h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 100, 0xA5))})
	                    .empty() &&
	                    h.frame().empty() && h.frame().empty(),
			"the 0x33 re-request waits inside SaveFile's 50 ms pace"))
		return false;
	if (!expect(h.one_packet(h.frame(), {0x33}),
			"the 0x33 re-request leaves at the next pace, not at the holdoff"))
		return false;

	// The final chunk ends SaveFile: the standalone 0x47 builds at once and
	// InitRandomSeedOrRequest's spin builds the 0x37 within microseconds.
	sent = h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 100, 71, 0xA5))});
	std::vector<uint8_t> tags0, tags1;
	if (!expect(sent.size() == 2 && h.tags_of(sent[0], tags0) && h.tags_of(sent[1], tags1) &&
	                    tags0 == std::vector<uint8_t>({0x47}) &&
	                    tags1 == std::vector<uint8_t>({0x37}),
			"the final server-info chunk's 0x47 and 0x37 leave on its frame"))
		return false;
	if (!expect(h.one_packet(h.frame({make_protocol_message(
	                                 0x64, transfer_chunk(1, 180, 0, 100, 0x5A))}),
	                    {0x37}),
			"the mission-data spin sends each 0x37 re-request on its own frame"))
		return false;

	// The final mission chunk ends the spin; NapiClient_SendPlayerSyncAndWaitForSyncTail then
	// queues the 0x09, finds the countdown running at its entry pump, and sends
	// only after MORE than 100 ms. A list landing inside that pace queues its one
	// 0x22 per unbound row behind the 0x09, so both leave in one packet: the retail
	// corpus run_20260426_113242's [0x09][0x22 00 F7 1C] (D-NET-526)
	// [orig: NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42CB20 (queue @0x42cb43, pace @0x42cbb5);
	//  NapiNPClientMsg_PlayerList @0x42fc35].
	if (!expect(h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 100, 80, 0x5A))})
	                    .empty(),
			"the final mission chunk enters WaitForDisconnect without building"))
		return false;
	if (!expect(h.frame({make_protocol_message(0x16, player_list({{0, 1}}))}).empty(),
			"the player list's 0x22 waits inside WaitForDisconnect's pace"))
		return false;
	for (int held = 2; held <= 6; ++held) {
		if (!expect(h.frame().empty(), "WaitForDisconnect holds its send pump for 100 ms"))
			return false;
	}
	sent = h.frame();
	ProtocolPacketHeader sync_header;
	std::vector<ProtocolMessage> sync_messages;
	if (!expect(sent.size() == 1 &&
	                    decode_client_session(sent[0], h.auth.scrk, sync_header, sync_messages) &&
	                    sync_messages.size() == 2 && sync_messages[0].tag == 0x09 &&
	                    sync_messages[0].payload.empty() && sync_messages[1].tag == 0x22 &&
	                    sync_messages[1].payload == std::vector<uint8_t>({0x00, 0xF7, 0x1C}),
			"the first send pump past 100 ms carries the 0x09, then the row's one 0x22"))
		return false;

	// S2C 0x11 ends WaitForDisconnect; NapiClient_WaitForGameStart queues the
	// 0x0A and spins: it leaves on the 0x11's frame, and every world-stream
	// packet is ACKed on its own frame.
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x11, {})}), {0x0A}),
			"the WaitForGameStart spin sends the 0x0A on the sync tail's frame"))
		return false;
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x03, {0x00})}), {}),
			"the world stream is ACKed on each packet's own frame"))
		return false;
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x03, {0x00})}), {}),
			"and again on the next world-stream frame, whatever the countdown"))
		return false;

	// S2C 0x1A ends the world stream. Its receive spent the countdown, so the wait's
	// last send ACKs it at once (header-only); the mission start's own pumps then find
	// the reloaded countdown and hold the loadout pair and the 0x0B for the final 0x0F
	// wait (D-NET-832).
	if (!expect(h.packets(h.frame({make_protocol_message(0x1A, {})}), {{}}) &&
	                    h.client.send_holdoff_countdown() == 10,
			"the 0x1A's immediate ACK; the load's pumps hold the submissions at c = 10"))
		return false;

	// The S2C 0x0F already waits at the final wait's first pass: its receive ends
	// Game_StartMission before its send, which the countdown holds. From here
	// Client_ProcessNetworkFrame's gate paces the send block again: the held submissions
	// and the completion burst leave together once the countdown is spent.
	std::vector<uint8_t> world_state(23, 0);
	sent = h.frame({make_protocol_message(0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF}),
			make_protocol_message(0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF}),
			make_protocol_message(0x19, {0x40, 0x30, 0x20, 0x10}),
			make_protocol_message(0x0F, world_state)});
	if (!expect(sent.empty(), "the 0x0F wait's pass receives the 0x0F; its send is held"))
		return false;
	int held_frames = 1;
	for (; held_frames < 12; ++held_frames) {
		sent = h.frame();
		if (!sent.empty()) break;
	}
	std::vector<uint8_t> tags;
	if (!expect(held_frames == 9 && !sent.empty() && h.tags_of(sent[0], tags) &&
	                    tags.size() > 3 && tags[0] == 0x2F && tags[1] == 0x2F &&
	                    tags[2] == 0x0B && tags[3] == 0x28,
			"the submissions lead the completion burst at the main loop's ninth frame"))
		return false;
	return true;
}

// With no dictated holdoff (the template's 0) the timed loops still send only on
// their wall-clock pace: SaveFile's entry pump builds at once, its re-requests wait.
bool run_timed_loops_pace_an_undictated_connection() {
	Harness h;
	if (!expect(h.handshake(), "zero-holdoff handshake")) return false;
	(void)h.frame({settings_record(0), settings_record(1)});
	(void)h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	(void)h.frame({make_protocol_message(0x02, probe)});
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x05, {0x01}),
	                                 make_protocol_message(0x04, slot_assignment(0x02)),
	                                 make_protocol_message(0x7B, full_player_info())}),
	                    {0x4E, 0x03, 0x48, 0x47, 0x33}),
			"with a zero countdown SaveFile's entry pump builds the grouped request at once"))
		return false;
	if (!expect(h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 100, 0xA5))})
	                    .empty() &&
	                    h.frame().empty() && h.frame().empty(),
			"a zero countdown does not let SaveFile send before its 50 ms pace"))
		return false;
	return expect(h.one_packet(h.frame(), {0x33}), "the 0x33 leaves at the pace");
}

// Run frames until one sends something (at most `limit`); the sent datagrams.
template <class H>
std::vector<std::vector<uint8_t>> frame_until_sent(H &h, int limit) {
	for (int i = 0; i < limit; ++i) {
		std::vector<std::vector<uint8_t>> sent = h.frame();
		if (!sent.empty()) return sent;
	}
	return {};
}

// Drive the admission under a dictated `period` through the S2C 0x1A: the mission start ran,
// the joiner sits in its final wait for the S2C 0x0F. `stream` (none: no self record yet) is
// the world stream's packet ahead of the 0x1A.
template <class H>
bool drive_to_the_final_wait(H &h, uint8_t period,
		const std::vector<ProtocolMessage> &stream = {}) {
	if (!h.handshake()) return false;
	(void)h.frame({settings_record(0), settings_record(1)});
	(void)h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	(void)h.frame({make_protocol_message(0x02, probe)});
	(void)h.frame({holdoff_record(period), make_protocol_message(0x03, {0x01})});
	(void)h.frame({make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
	(void)h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 180, 0x5A))});
	(void)h.frame({make_protocol_message(0x16, player_list({{0, 1}, {1, 2}}))});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x11, {})});
	if (!stream.empty()) (void)h.frame(stream);
	(void)h.frame({make_protocol_message(0x1A, {})});
	return std::string(h.client.admission_stage_name()) ==
			std::string("awaiting the world state (S2C 0x0F)");
}

// The joiner's own S2C 0x0C organic spawn record: slot 0xC6, owned by the 0x82's MI.
ProtocolMessage self_spawn_record() {
	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord self;
	self.slot_id = 0x00C6;
	self.def_type = 3; // ItemDefType person
	self.item_type_id = 0x14B9;
	self.owner_connection_id = 4; // the 0x82's MI
	self.minimap_flags = 0x100;
	self.entity_name = "LoadingPump";
	self.team = 2;
	self.net_id = 7;
	batch.records.push_back(self);
	return make_protocol_message(0x0C, encode_organic_spawn_batch(batch));
}

// A first 0x5A grant (a host's answer to the load's first 0x2F).
ProtocolMessage first_grant() {
	return make_protocol_message(0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF});
}

// Drive the admission under a dictated `period` to the final wait for S2C 0x0F, then let the
// host release the player early: its self record and a first 0x5A grant (a host's answer to
// the load's first 0x2F, which lands inside the final wait, ahead of the 0x0F).
bool drive_to_an_early_in_match_release(Harness &h, uint8_t period) {
	if (!drive_to_the_final_wait(h, period)) return false;
	(void)h.frame({self_spawn_record(), first_grant()});
	return expect(h.client.in_match() &&
	                      std::string(h.client.admission_stage_name()) ==
	                              std::string("awaiting the world state (S2C 0x0F)"),
			"early release: the joiner is in the match inside the final wait for the 0x0F");
}

// The tags of every message the joiner sent, one list per datagram, from `sent`.
std::vector<std::vector<uint8_t>> sent_tags(Harness &h,
		const std::vector<std::vector<uint8_t>> &sent) {
	std::vector<std::vector<uint8_t>> out;
	for (const std::vector<uint8_t> &dg : sent) {
		std::vector<uint8_t> tags;
		if (!h.tags_of(dg, tags)) tags = {0xFF, 0xFF};
		out.push_back(tags);
	}
	return out;
}

bool carries(const std::vector<std::vector<uint8_t>> &packets, uint8_t tag) {
	for (const std::vector<uint8_t> &tags : packets)
		for (uint8_t t : tags)
			if (t == tag) return true;
	return false;
}

// A joiner the host released early (its self record and a first 0x5A inside the final wait
// for the 0x0F, as a host's answer to the load's first 0x2F lands there) is still in that
// wait: the loop spins, building at every pass, and runs no client net frame, so neither the
// frame's 0x2C ping, its 0x0C uplink nor its 0x4C report leaves before the 0x0F (checkpoint
// c1: our joiner's 0x2C and 0x0C rode the 0x0B, ahead of the load requests). The 0x0F ends
// the wait, whose pass it is: that pass sends its completion burst alone when its countdown
// is spent (the early form), else nothing; the first 0x2C and 0x0C ride the game loop's first
// open boundary (the late form's behind the burst), and from there the frame's countdown gate
// paces the block, one boundary per dictated period, the frames that build exactly the input
// pack's; under no holdoff every frame opens and a one-shot queued before a frame leaves in it.
// [orig: Game_StartMission's final wait @0x526295..0x5262e5 (the send @0x5262da, the exit
//  @0x5262df on g_ClientWorldStateLoaded, set only by NapiNPClientMsg_0x00F @0x42e2cd; its
//  burst @0x42e5af..0x42e6ab); Client_ProcessNetworkFrame @0x42c3dd ->
//  Player_PackInputStateToEntity @0x42c3e9, the 0x2C @0x42c3fa..0x42c44a, the 0x0C
//  @0x42c46f..0x42c4a3, the 0x4C @0x42c23e..0x42c279 -> PumpClientProtocolSend @0x42c4bc]
bool run_an_early_release_stays_in_the_final_wait() {
	{
		Harness h;
		h.uplink = true;
		if (!drive_to_an_early_in_match_release(h, 6)) return false;
		// Past one 0x4C period (310 frames): the spin never reaches the frame's producers.
		for (int i = 0; i < 320; ++i) {
			const uint32_t flushes = h.client.send_flush_counter();
			const std::vector<std::vector<uint8_t>> sent = sent_tags(h, h.frame());
			if (!expect(h.client.send_flush_counter() == flushes + 1,
					"final wait: the spin builds at every pass, released or not"))
				return false;
			if (!expect(!carries(sent, 0x2C) && !carries(sent, 0x0C) && !carries(sent, 0x4C),
					"final wait: no 0x2C, 0x0C or 0x4C leaves before the 0x0F"))
				return false;
		}
		// The late form (13 of 18 RR joins): the 0x0F lands on a pass whose countdown is not
		// spent, so that pass (the wait's) sends nothing and the burst rides the game loop's
		// first open boundary, the frame's 0x2C and 0x0C behind it.
		while (h.client.send_holdoff_countdown() <= 1) (void)h.frame();
		std::vector<uint8_t> world_state(23, 0);
		std::vector<std::vector<uint8_t>> sent =
				h.frame({make_protocol_message(0x0F, world_state)});
		if (!expect(sent.empty(), "late form: the 0x0F's pass holds its burst behind the countdown"))
			return false;
		for (int i = 0; sent.empty() && i < 8; ++i) sent = h.frame();
		if (!expect(h.packets(sent, {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23, 0x2C, 0x0C}}),
				"late form: the burst leads the game loop's first boundary, its 0x2C and 0x0C "
				"behind it"))
			return false;
		int opened_frames = 0;
		int last_opened = -1;
		for (int i = 0; i < 18; ++i) {
			const bool pack_frame = h.client.send_block_opens_this_frame();
			const uint32_t flushes = h.client.send_flush_counter();
			(void)h.frame();
			const bool opened = h.client.send_flush_counter() != flushes;
			if (!expect(opened == pack_frame,
					"game loop: the send block opens exactly on the input pack's frames"))
				return false;
			if (opened && !expect(last_opened < 0 || i - last_opened == 6,
					"game loop: the boundaries are one dictated period apart"))
				return false;
			if (opened) {
				last_opened = i;
				++opened_frames;
			}
		}
		if (!expect(opened_frames == 3, "game loop: one send boundary per dictated period of 6"))
			return false;
	}
	{
		// The early form (5 of 18 RR joins): the 0x0F lands on a pass whose countdown is spent,
		// so that pass's send ships the burst alone; the first 0x2C and 0x0C ride the game loop's
		// first open boundary, a dictated period on (D-NET-1110).
		Harness h;
		h.uplink = true;
		if (!drive_to_an_early_in_match_release(h, 6)) return false;
		while (h.client.send_holdoff_countdown() > 1) (void)h.frame();
		std::vector<uint8_t> world_state(23, 0);
		if (!expect(h.packets(h.frame({make_protocol_message(0x0F, world_state)}),
		                    {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23}}),
				"early form: the 0x0F's pass (the wait's) sends its burst alone"))
			return false;
		int frames = 0;
		std::vector<std::vector<uint8_t>> sent;
		for (; sent.empty() && frames < 8; ++frames) sent = h.frame();
		if (!expect(frames == 6 && h.packets(sent, {{0x2C, 0x0C}}),
				"early form: the game loop's first open boundary, a period on, carries the "
				"first 0x2C and 0x0C"))
			return false;
	}
	Harness h;
	if (!drive_to_an_early_in_match_release(h, 0)) return false;
	// The final wait resets the key queue and reads ESC alone: an input producer
	// queues nothing there, the early release notwithstanding
	// [orig: Game_StartMission @0x524360 - Input_ResetKeyQueue @0x526288, ESC
	//  @0x5262a2..0x5262ad].
	if (!expect(h.client.in_match() && !h.client.in_game_loop() &&
				!h.client.queue_stance_change(0xA9),
			"final wait: the stance key queues nothing before the 0x0F"))
		return false;
	// The session-gated senders too: the input frame, the HUD and the in-game screens
	// that call them run in the main frame alone, which the wait does not run
	// (ClientRuntime::in_game_loop) [orig: Game_ProcessMainFrame @0x5263F0 -
	// Input_ProcessFrame @0x52661D].
	if (!expect(h.client.in_session() && !h.client.queue_medic_request() &&
				!h.client.queue_respawn_request() && !h.client.queue_spectator_convert() &&
				!h.client.queue_deployment_pick(0) &&
				!h.client.queue_voice_menu_pick(opennova::c2s::RADIO_CALL_REQUEST, 1) &&
				!h.client.queue_team_change_request() &&
				!h.client.queue_vehicle_spawn_availability_request() &&
				!h.client.queue_vehicle_spawn_request(0x1001, 0),
			"final wait: no session sender queues before the 0x0F"))
		return false;
	std::vector<uint8_t> world_state(23, 0);
	(void)h.frame({make_protocol_message(0x0F, world_state)});
	for (int i = 0; i < 4; ++i) {
		if (!expect(h.client.send_block_opens_this_frame(),
				"game loop, no holdoff: every frame opens"))
			return false;
		if (!expect(h.client.queue_stance_change(0xA9), "game loop: the stance key queues a 0x1D"))
			return false;
		if (!expect(carries(sent_tags(h, h.frame()), 0x1D),
				"game loop, no holdoff: a one-shot queued before a frame leaves in it"))
			return false;
	}
	return true;
}


// D-NET-852 (V-L2-s9 FOUND 1): an ESC in the mission start's final wait for the S2C
// 0x0F stores the loading cancel flag and leaves the wait, and nothing else: the game loop
// runs (its frame's 0x2C, a key's 0x1D), the session stays (nothing torn down, no 0x46), a
// second ESC finds no wait, and the 0x0F, when it comes, still completes the admission.
// Outside the wait (the load) the ESC takes nothing here.
// [orig: Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5, its ESC
//  @0x5262AB -> g_LoadingCancelFlag = 1 @0x5262E9 -> @0x5262F3; the flag's clear at the head
//  @0x524575; NapiNPClientMsg_0x00F @0x42E2CD]
bool run_an_esc_leaves_the_final_wait_for_the_game_loop() {
	Harness h;
	if (!drive_to_an_early_in_match_release(h, 0)) return false;
	if (!expect(!h.client.in_game_loop() && h.client.cancel_final_wait() &&
				h.client.in_game_loop() && h.client.in_session() &&
				!h.client.initial_admission_complete(),
			"final wait ESC: the game loop runs with the 0x0F still owed"))
		return false;
	if (!expect(!h.client.cancel_final_wait(), "final wait ESC: a second ESC finds no wait"))
		return false;
	if (!expect(h.client.queue_stance_change(0xA9),
			"final wait ESC: the game loop's input queues its 0x1D"))
		return false;
	const std::vector<std::vector<uint8_t>> sent = sent_tags(h, h.frame());
	if (!expect(carries(sent, 0x1D) && carries(sent, 0x2C) && !carries(sent, 0x46) &&
				h.client.in_session() && !h.client.session_lost(),
			"final wait ESC: the game loop's frame sends, nothing is torn down"))
		return false;
	std::vector<uint8_t> world_state(23, 0);
	(void)h.frame({make_protocol_message(0x0F, world_state)});
	return expect(h.client.initial_admission_complete() && h.client.in_game_loop(),
			"final wait ESC: the later 0x0F still completes the admission");
}

} // namespace

// A host's round reset that lands before the mission data is done does not
// survive the mission start: Game_StartMission clears the round-over gate
// after the mission-data spin, so the joiner goes on into the match instead of
// running the end-round linger out.
// [orig: NapiNPClientMsg_GameReset @0x422849 (the gate); Game_StartMission
//  @0x524a1f (the clear) after InitRandomSeedOrRequest @0x5248a7]
bool run_a_reset_before_the_mission_start_is_cleared() {
	Harness h;
	if (!expect(h.handshake(), "reset: handshake")) return false;
	h.frame({settings_record(0), settings_record(1)});
	h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	h.frame({make_protocol_message(0x02, probe)});
	h.frame({make_protocol_message(0x03, {0x01})});
	h.frame({make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	for (int i = 0; i < 4; ++i) h.frame();
	h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
	h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 100, 0x5A))});
	// The host resets its round while the mission data is still arriving.
	h.frame({make_protocol_message(0x25, {})});
	if (!expect(h.client.state().spawn_success_gate,
			"reset: the 0x25 raises the round-over gate"))
		return false;
	h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 100, 80, 0x5A))});
	if (!expect(!h.client.state().spawn_success_gate,
			"reset: the mission start clears the gate"))
		return false;
	for (int i = 0; i < 8; ++i) h.frame();
	return expect(h.client.mission_exit_reason() == inmatch::kMissionExitNone,
			"reset: the joiner does not leave the match it is loading");
}

// Drive the admission under a dictated `period` into NapiClient_WaitForGameStart: the
// transfers, the 0x09, the 0x11 and its 0x0A, the world stream running.
template <class H>
bool drive_into_the_world_stream(H &h, uint8_t period) {
	if (!h.handshake()) return false;
	(void)h.frame({settings_record(0), settings_record(1)});
	(void)h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	(void)h.frame({make_protocol_message(0x02, probe)});
	(void)h.frame({holdoff_record(period), make_protocol_message(0x03, {0x01})});
	(void)h.frame({make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
	(void)h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 180, 0x5A))});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x11, {})});
	return std::string(h.client.admission_stage_name()) == std::string("awaiting the world stream");
}

// D-NET-572: a self S2C 0x50 ahead of the process's first mission start latches nothing
// and re-submits nothing: the handler's latch and its local arm compare the entity with
// g_LocalPlayerEntity, which only the mission start's Player_InitPlayer binds, so the
// mission start's two 0x2F carry the older team. After the mission start the same 0x50
// latches the team and re-submits a 0x2F on it.
// [orig: NapiNPClientMsg_TeamAssign @0x431910 - the compares @0x4319CF / @0x431A1B, the
//  latch @0x4319DB, the submit @0x431AAD; Game_StartMission @0x524360 - the first 0x2F
//  @0x525836, Player_InitPlayer @0x525BBC (its store @0x4E17EB), the second @0x525C2E]
bool run_a_self_team_assign_waits_for_the_mission_starts_player_init() {
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 0), "team assign: the world stream runs"))
		return false;
	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord self;
	self.slot_id = 0x00C6;
	self.def_type = 3;
	self.item_type_id = 0x14B9;
	self.owner_connection_id = 4; // the 0x82's MI
	self.minimap_flags = 0x100;
	self.entity_name = "LoadingPump";
	self.team = 2;
	self.net_id = 7;
	batch.records.push_back(self);
	(void)h.frame({make_protocol_message(0x0C, encode_organic_spawn_batch(batch))});
	if (!expect(h.client.has_self_handle() && h.client.self_handle() == 0x00C6 &&
				h.client.assigned_team() == 2,
			"team assign: the self entity is bound; the 0x04 latched team 2"))
		return false;
	const std::vector<uint8_t> to_team_1 = {0xC6, 0x00, 0x01, 0x00, 0x00, 0x00};
	const auto loadout_teams = [&](const std::vector<std::vector<uint8_t>> &sent) {
		std::vector<uint8_t> teams;
		for (const std::vector<uint8_t> &datagram : sent) {
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!decode_client_session(datagram, h.auth.scrk, header, messages)) continue;
			for (const ProtocolMessage &m : messages)
				if (m.tag == 0x2F && !m.payload.empty()) teams.push_back(m.payload[0]);
		}
		return teams;
	};
	std::vector<std::vector<uint8_t>> sent =
			h.frame({make_protocol_message(0x50, to_team_1)});
	for (int i = 0; i < 4; ++i) {
		const std::vector<std::vector<uint8_t>> more = h.frame();
		sent.insert(sent.end(), more.begin(), more.end());
	}
	if (!expect(h.client.assigned_team() == 2 && loadout_teams(sent).empty(),
			"before the mission start: no latch and no 0x2F"))
		return false;
	sent = h.frame({make_protocol_message(0x1A, {})});
	if (!expect(loadout_teams(sent) == std::vector<uint8_t>({2, 2}),
			"the mission start's two 0x2F carry the older team"))
		return false;
	sent = h.frame({make_protocol_message(0x50, to_team_1)});
	for (int i = 0; i < 4 && loadout_teams(sent).empty(); ++i) sent = h.frame();
	return expect(h.client.assigned_team() == 1 &&
						loadout_teams(sent) == std::vector<uint8_t>({1}),
			"after the mission start: the 0x50 latches team 1 and re-submits a 0x2F on it");
}

// A joiner the 0x0F admits with no self row (a spectator's release: the 0x0F clears the
// deploy hold whatever the role) pings from its first game-loop boundary: the 0x2C's gate
// reads the session, the role, the deploy hold and the round-over gate, never a body. Its
// 0x0C, whose build reads the body, stays unsent. The 0x0F's own pass is the final wait's,
// which sends its burst alone (D-NET-1110).
// [orig: Client_ProcessNetworkFrame @0x42c3ee..0x42c410 (the 0x2C's gate) and
//  @0x42c44f..0x42c46d -> Player_BuildTag0CInputBody @0x42A550; NapiNPClientMsg_0x00F
//  @0x42e391 (the hold's clear); Game_StartMission @0x524360 - the final wait's send
//  @0x5262DA ahead of its exit test @0x5262DF]
bool run_a_bodyless_joiner_pings() {
	Harness h;
	h.uplink = true;
	if (!expect(drive_into_the_world_stream(h, 0), "bodyless: the world stream runs"))
		return false;
	(void)h.frame({make_protocol_message(0x1A, {})});
	std::vector<uint8_t> world_state(23, 0);
	std::vector<std::vector<uint8_t>> sent = h.frame({make_protocol_message(0x0F, world_state)});
	if (!expect(!h.client.has_self_handle() &&
	                    h.packets(sent, {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23}}),
			"bodyless: the 0x0F's pass (the wait's) sends its burst alone"))
		return false;
	for (int i = 0; i < 3; ++i) {
		if (!expect(h.packets(h.frame(), {{0x2C}}),
				"bodyless: one 0x2C at every open boundary, no 0x0C"))
			return false;
	}
	return true;
}

// The same admission driven through a JoinerRole on the virtual clock: each frame is the
// role's tick (its receive, the after-receive leg on the role's own world, its send block),
// and what it ships lands on the socket.
struct RoleHarness {
	struct KeepingSocket final : public IDatagramSocket {
		std::vector<std::vector<uint8_t>> sent;
		int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
		void send_to(const PeerAddr &, const uint8_t *data, std::size_t size) override {
			sent.emplace_back(data, data + size);
		}
	};
	uint64_t now_ms = 100000;
	// The kernel lives on the heap: a World-carrying frame is megabytes.
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	bool kernel_jo = test_titles::jo_world(kernel->world);
	inmatch::JoinerRole role;
	bool role_jo = test_titles::jo_joiner(role);
	KeepingSocket socket;
	inmatch::TickInput input;
	std::string server_scrk = "SERVER-LOADING-PUMP-SCRK";
	ClientAuth auth;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	inmatch::ClientRuntime &client;
	// The test runs the mission start's call itself (poll_mission_start); without it the
	// harness has no load to run and ends the released mission start after the 0x1A's tick.
	bool load_declared = false;

	RoleHarness() : client(install()) {}

	inmatch::ClientRuntime &install() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.load_systems();
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_protocol(&kJo);
		role.set_rules(opennova::world::title_rules("jo"));
		role.create_runtime("LoadingPump", inmatch::JoinRole::Player, "", "");
		// The role's runtime on the virtual clock, configured as create_runtime does.
		role.runtime = std::make_unique<inmatch::ClientRuntime>(
				"LoadingPump", [this] { return now_ms; });
		test_titles::jo_folds(*role.runtime);
		role.runtime->set_protocol(&kJo);
		role.runtime->set_weapon_table(&kernel->world.tables.weapons);
		return *role.runtime;
	}

	std::vector<std::vector<uint8_t>> frame(
			const std::vector<ProtocolMessage> *incoming = nullptr) {
		now_ms += 16;
		if (incoming != nullptr) {
			const std::vector<uint8_t> dg =
					frame_server_session(server_tx, server_scrk, auth.ck, *incoming);
			client.receive(dg.data(), dg.size());
		}
		const std::size_t from = socket.sent.size();
		role.run_tick(input);
		// The embedder's end of the mission start the tick's receive released, its load done
		// (poll_mission_start's): the push and the final wait's first pass, shipped here.
		if (!load_declared && client.mission_start_released()) role.push_mission_start_end();
		return std::vector<std::vector<uint8_t>>(socket.sent.begin() +
						static_cast<std::ptrdiff_t>(from), socket.sent.end());
	}
	std::vector<std::vector<uint8_t>> frame(std::vector<ProtocolMessage> incoming) {
		return frame(&incoming);
	}

	// Each sent datagram's message tags against `want`, one list per datagram, in order.
	bool packets(const std::vector<std::vector<uint8_t>> &sent,
			const std::vector<std::vector<uint8_t>> &want) {
		std::vector<std::vector<uint8_t>> got;
		for (const std::vector<uint8_t> &dg : sent) {
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			std::vector<uint8_t> tags;
			if (decode_client_session(dg, auth.scrk, header, messages))
				for (const ProtocolMessage &m : messages) tags.push_back(m.tag);
			else
				tags = {0xFF, 0xFF};
			got.push_back(tags);
		}
		if (got == want) return true;
		std::fprintf(stderr, "  sent %zu datagram(s):", got.size());
		for (const std::vector<uint8_t> &tags : got) {
			std::fprintf(stderr, " [");
			for (uint8_t tag : tags) std::fprintf(stderr, " %02X", tag);
			std::fprintf(stderr, " ]");
		}
		std::fprintf(stderr, "\n");
		return false;
	}

	// The role's first tick ships the ClientHello; the handshake then runs as Harness's.
	bool handshake() {
		const std::vector<std::vector<uint8_t>> first = frame();
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ClientHello hello;
		if (first.empty() || !nw_decode_inbound(first[0].data(), first[0].size(), opcode, body) ||
				!parse_client_hello(body.data(), body.size(), hello))
			return false;
		ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
		server_hello.hk = 0x13572468u;
		const std::vector<uint8_t> server_hello_dg = nw_encode_outbound(
				SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
		client.receive(server_hello_dg.data(), server_hello_dg.size());
		std::vector<std::vector<uint8_t>> auth_frame;
		for (int i = 0; i < 4 && auth_frame.empty(); ++i) auth_frame = frame();
		if (auth_frame.size() != 1 ||
				!nw_decode_inbound(auth_frame[0].data(), auth_frame[0].size(), opcode, body) ||
				!parse_client_auth(body.data(), body.size(), auth))
			return false;
		ServerAuth server_auth = build_server_auth(auth, 0x7F000001u, 32769,
				Harness::kServerKey, server_scrk, "", "", "", false);
		server_auth.mi = 4;
		const std::vector<uint8_t> server_auth_dg = nw_encode_outbound(
				SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
		client.receive(server_auth_dg.data(), server_auth_dg.size());
		return frame().empty();
	}
};

// D-NET-526, D-NET-1183: the joiner's own body is born at its mission start, where
// retail's Player_InitPlayer binds the self entity the world stream's 0x0C built, ahead of the
// 0x0B and the final wait; it leaves the mission start hidden. The release (a 0x5A) and the S2C
// 0x0F then reach it in one packet: the role's after-receive leg writes the handlers' stores on
// L, folds its record and lands the 0x0F's pose, and the release is L's admission, no
// redeploy's hold. That pass is the final wait's (D-NET-1110): under no holdoff its
// send ships the 0x0F's completion burst alone (the early form: RR's kick / aikill / 03tr-dm
// joiners), and it runs no entity update or World tick, so the game loop's first frame
// builds the 0x2C ping and the first 0x0C from L as the 0x0F left it, at the 0x0F's position.
// [orig: Client_ProcessNetworkFrame @0x42C180 - the receive @0x42C228 ahead of the send
//  block @0x42C3DD, the 0x0C's build from g_LocalPlayerEntity @0x42C46F..0x42C482;
//  NapiNPClientMsg_0x00F @0x42E200 - the pose stores @0x42E3B9..0x42E3CE;
//  Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC, `or [eax+24h], edi` @0x525C9E,
//  the 0x0B @0x52624B, the final wait's send @0x5262DA ahead of its exit test @0x5262DF;
//  Player_InitPlayer @0x4E15F0 - the store @0x4E17EB; Game_ProcessMainFrame @0x5263F0 -
//  Client_ProcessNetworkFrame @0x526692 ahead of Entity_UpdateAllEntities @0x52674B]
bool run_the_mission_start_bears_l_and_the_first_frame_ships_its_uplink() {
	RoleHarness h;
	// The self record rides the world stream, ahead of the mission start.
	if (!expect(drive_to_the_final_wait(h, 0, {self_spawn_record()}),
			"release frame: the final wait runs"))
		return false;
	const world::Entity *born = h.kernel->local.player();
	if (!expect(h.client.has_self_handle() && !h.client.in_match() && h.role.local_spawned() &&
	                    born != nullptr && (born->flags & world::kEntityFlagCarried) != 0u,
			"release frame: the self record is bound, no release yet; the mission start bore L, "
			"hidden"))
		return false;
	// 23-byte header [u32 tick][i32 x][i32 y][i32 z][i16 yaw][i16 pitch][i16 roll][u8 flags],
	// the 128 pool dwords, zero waypoint / location counts.
	std::vector<uint8_t> world_state(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	const auto put_i32 = [&world_state](std::size_t at, int32_t v) {
		for (int b = 0; b < 4; ++b)
			world_state[at + static_cast<std::size_t>(b)] = static_cast<uint8_t>(v >> (8 * b));
	};
	put_i32(4, 100 << 16);
	put_i32(8, 200 << 16);
	put_i32(12, 42 << 16);
	if (!expect(h.packets(h.frame({first_grant(), make_protocol_message(0x0F, world_state)}),
	                    {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23}}),
			"release frame: the 0x0F's pass (the wait's) sends its burst alone"))
		return false;
	if (!expect(h.client.in_match() && h.role.local_spawned() &&
	                    h.kernel->local.player() == born,
			"release frame: the mission start's L takes the release in the pass"))
		return false;
	const std::vector<std::vector<uint8_t>> sent = h.frame();
	if (!expect(h.packets(sent, {{0x2C, 0x0C}}),
			"release frame: the game loop's first frame sends the 0x2C and the first 0x0C"))
		return false;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!decode_client_session(sent[0], h.auth.scrk, header, messages) || messages.empty())
		return expect(false, "release frame: the datagram decodes");
	const ProtocolMessage &uplink_message = messages.back();
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	std::size_t header_bytes = 0, body_bytes = 0;
	if (!expect(decode_entity_packet_sub_header(uplink_message.payload.data(),
						uplink_message.payload.size(), sub, header_bytes) &&
					decode_player_extended_uplink(uplink_message.payload.data() + header_bytes,
						uplink_message.payload.size() - header_bytes, uplink, body_bytes),
			"release frame: the 0x0C decodes"))
		return false;
	if (!expect(sub.handle == 0x00C6, "release frame: the 0x0C names the self handle"))
		return false;
	return expect(uplink.pos_x == (100 << 16) && uplink.pos_y == (200 << 16) &&
					uplink.pos_z == (42 << 16) && uplink.carrier_handle == 0xFFFF,
			"release frame: the first 0x0C carries the 0x0F's position");
}

// D-NET-862: the final wait for the S2C 0x0F is the mission start's last loop, inside
// the Game Loop mode's initialize; the role's tick that leaves it reports the initialize's end
// once (Role::take_final_wait_end), so the session's bank and re-based frames start there.
// [orig: Game_StartMission @0x524360 - g_MissionStartBlankFrameCount = 3 @0x525E1F, the final
//  wait @0x526295..0x5262E5; GameLoop_Init @0x526370 (the call @0x526377); Game_MainLoop
//  @0x52B70F..0x52B757, @0x52B766]
bool run_the_final_wait_end_ends_the_mode_init() {
	RoleHarness h;
	if (!expect(drive_to_the_final_wait(h, 0, {self_spawn_record()}),
			"final wait end: the final wait runs"))
		return false;
	(void)h.frame();
	if (!expect(!h.role.take_final_wait_end(), "final wait end: the wait's passes end nothing"))
		return false;
	std::vector<uint8_t> world_state(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	(void)h.frame({first_grant(), make_protocol_message(0x0F, world_state)});
	if (!expect(h.client.in_game_loop() && h.role.take_final_wait_end(),
			"final wait end: the 0x0F's tick ends the initialize"))
		return false;
	if (!expect(!h.role.take_final_wait_end(), "final wait end: reported once"))
		return false;
	(void)h.frame();
	return expect(!h.role.take_final_wait_end(),
			"final wait end: the game loop's ticks end nothing");
}

// D-NET-1148: the push's pass is the final wait's first, so the first tick after the
// embedder's mission start call (poll_mission_start, which pushes between ticks) whose receive
// takes the S2C 0x0F ends the Game Loop mode's initialize, once, as a later wait pass's does.
// [orig: Game_StartMission @0x524360 - the send @0x526255, the final wait @0x526295..0x5262E5
//  (CNapiNetwork_PumpManagerReceive @0x5262B4, CNapiNetwork_PumpClientProtocolRecv @0x5262BE,
//  CNapiNetwork_PumpClientProtocolSend @0x5262DA, the exit test @0x5262DF); GameLoop_Init
//  @0x526370 (the call @0x526377)]
bool run_a_0x0f_after_the_mission_start_call_ends_the_mode_init() {
	RoleHarness h;
	if (!expect(drive_into_the_world_stream(h, 0), "call end: the world stream runs")) return false;
	h.load_declared = true;
	(void)h.frame({self_spawn_record()});
	const std::vector<uint8_t> release = frame_server_session(h.server_tx, h.server_scrk,
			h.auth.ck, {make_protocol_message(0x1A, {})});
	h.client.receive(release.data(), release.size());
	inmatch::JoinerRole::MissionStartLegs legs;
	legs.load = [] { return true; };
	if (!expect(h.role.poll_mission_start(legs) == inmatch::JoinerRole::MissionStartPass::Started &&
	                    h.client.in_final_wait() && !h.role.take_final_wait_end(),
			"call end: the mission start's call ends in the final wait"))
		return false;
	std::vector<uint8_t> world_state(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	(void)h.frame({first_grant(), make_protocol_message(0x0F, world_state)});
	if (!expect(h.client.in_game_loop() && h.role.take_final_wait_end(),
			"call end: the first tick's 0x0F ends the initialize"))
		return false;
	return expect(!h.role.take_final_wait_end(), "call end: reported once");
}

// D-NET-1110 (V-ROLL-AP's order note): the pass whose receive takes the S2C 0x0F is the
// final wait's, and what its receive's handlers write lands in that pass, as retail's
// handlers write it inside the wait's receive; the game loop's first frame (its input pass
// ahead of its net frame) then reads it. A 0x0F carrying the cease-fire flag stores
// g_InCeaseFire, the fire row's gate; a 0x34 behind it plays its sound; a 0x25 raises
// g_RoundOver. The first game-loop frame applies none of them a second time.
// [orig: Game_StartMission @0x524360 - CNapiNetwork_PumpClientProtocolRecv @0x5262BE ahead
//  of the send @0x5262DA; NapiNPClientMsg_0x00F @0x42E200 - `test cl, 8` @0x42E311,
//  g_InCeaseFire @0x42E316; NapiNPClientMsg_PlaySoundByName @0x4283A0;
//  NapiNPClientMsg_GameReset @0x422800 - g_RoundOver @0x422849; Game_ProcessMainFrame
//  @0x5263F0 - Input_ProcessFrame @0x52661D ahead of Client_ProcessNetworkFrame @0x526692;
//  Input_HandleActionBinding_0 @0x4E0420 - g_InCeaseFire @0x4E07C2]
bool run_the_0x0f_pass_lands_its_handler_writes() {
	const auto pin_sounds = [](const mission::MissionKernel &kernel) {
		std::size_t n = 0;
		for (const world::ScriptSoundEvent &event : kernel.world.out.script_sounds)
			if (event.name == "PIN_SOUND") ++n;
		return n;
	};
	PlaySoundCommand sound;
	sound.sound_name = "PIN_SOUND"; // flag 0: the flat play
	std::vector<uint8_t> world_state(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	world_state[22] = 0x08; // the game flags' cease-fire bit
	{
		RoleHarness h;
		if (!expect(drive_to_the_final_wait(h, 0, {self_spawn_record()}),
				"0x0F pass writes: the final wait runs"))
			return false;
		(void)h.frame({first_grant(), make_protocol_message(0x0F, world_state),
				make_protocol_message(s2c::PLAY_SOUND, encode_play_sound(sound))});
		if (!expect(h.client.in_game_loop() && h.role.take_final_wait_end(),
				"0x0F pass writes: the pass is the final wait's"))
			return false;
		if (!expect(h.kernel->world.rules.cease_fire,
				"0x0F pass writes: the 0x0F's cease fire stands for the first frame's fire gate"))
			return false;
		if (!expect(pin_sounds(*h.kernel) == 1, "0x0F pass writes: the 0x34 plays in the pass"))
			return false;
		(void)h.frame();
		if (!expect(pin_sounds(*h.kernel) == 1,
				"0x0F pass writes: the first game-loop frame plays it no second time"))
			return false;
	}
	RoleHarness h;
	if (!expect(drive_to_the_final_wait(h, 0, {self_spawn_record()}),
			"0x0F pass writes: the final wait runs (the 0x25)"))
		return false;
	(void)h.frame({first_grant(), make_protocol_message(0x0F, world_state),
			make_protocol_message(s2c::GAME_RESET, {})});
	return expect(h.client.in_game_loop() && h.kernel->world.match.outcome().ended,
			"0x0F pass writes: the 0x25's round-over latch lands in the pass");
}

// The main frame's 62-frame quality fold steps in the game loop's frames alone and starts
// from the mission start's zero: no fold runs in the join's waits (a host scalar landing
// there moves no level), the first game-loop frame folds at once, the next fold is 62
// frames on, and a reload's mission start zeroes the countdown and resets the level, the
// host scalar and the receive window.
// [orig: Game_ProcessMainFrame @0x526537..0x526555 (g_NetQualityUpdateCountdown - 1, > 0 skips, else
//  62) -> CNetQuality_UpdateMetrics @0x4C52C0; Game_StartMission's head @0x52439e (the
//  zero), CNetStats_Reset @0x5243aa and CNetQuality_Reset @0x5243b4]
bool run_the_quality_fold_starts_with_the_game_loop() {
	Harness h;
	h.client.set_observed_frame_rate(62); // the client window samples its floor 1
	if (!expect(drive_into_the_world_stream(h, 0), "fold: the world stream runs"))
		return false;
	(void)h.frame({make_protocol_message(0x79, {0xAB})}); // 171: level 3 once folded
	for (int i = 0; i < 130; ++i) {
		(void)h.frame();
		if (!expect(h.client.net_quality_level() == 0, "fold: no fold in the join's waits"))
			return false;
	}
	(void)h.frame({make_protocol_message(0x1A, {})});
	std::vector<uint8_t> world_state(23, 0);
	(void)h.frame({make_protocol_message(0x0F, world_state)});
	if (!expect(h.client.net_quality_level() == 0,
			"fold: the 0x0F's own frame is the final wait's pass, no fold"))
		return false;
	(void)h.frame();
	if (!expect(h.client.net_quality_level() == 3,
			"fold: the first game-loop frame folds max(171, 1) at once: level 3"))
		return false;
	(void)h.frame({make_protocol_message(0x79, {0x60})}); // 96: level 2 at the next fold
	for (int frame = 3; frame <= 62; ++frame) {
		(void)h.frame();
		if (!expect(h.client.net_quality_level() == 3, "fold: the level holds for 62 frames"))
			return false;
	}
	(void)h.frame();
	if (!expect(h.client.net_quality_level() == 2, "fold: the 63rd game-loop frame folds again"))
		return false;
	if (!expect(h.client.client_quality_window().ping[1] != 0,
			"fold: the receive window holds the game loop's samples"))
		return false;
	const replication::NetQualityWindow empty;
	const replication::NetQualityWindow &window = h.client.client_quality_window();
	return expect(h.client.begin_mission_reload() && h.client.net_quality_level() == 0 &&
	                      window.cursor == empty.cursor && window.ping[1] == 0 &&
	                      window.quality == 0 && h.client.state().net.host_metric == 0,
			"fold: a reload's mission start resets the level, the host scalar and the "
			"receive window");
}

// D-NET-1031 (L2-s26 FOUND 2): the connection indicators and the client net frame's
// link-error leg are the main frame's, which none of the join's waits runs. A silence past two
// whole seconds in the game-start wait raises no incoming link error; a link error the wait's
// receive raises (its missing-sequence request's) holds its countdown through the waits; the
// game loop's frames raise the silence's flag (once the 0x0F's ten-second hold-off ran out)
// and step its countdown. An abandoned mission start's game loop is a main frame too: its
// frames step the 0x4C report's timer and report.
// [orig: g_GameModeGameLoop's frame Game_ProcessMainFrame @0x5263F0 - the one caller of
//  CNetQuality_UpdateIndicators @0x4C3090 (@0x52668D) and Client_ProcessNetworkFrame
//  @0x42C180 (@0x526692); Client_ProcessNetworkFrame - the silence leg @0x42C1F8..0x42C21E,
//  the 0x4C @0x42C22D..0x42C279; NapiClient_WaitForGameStart @0x42CC10 - its pumps
//  @0x42CC7E / @0x42CC88 / @0x42CC92 and no net frame; Game_StartMission @0x524360 -
//  "Mission loading aborted 1" pushes g_GameModeGameLoop @0x524DB4]
bool run_the_link_indicators_step_in_the_main_frame_alone() {
	{
		Harness h;
		if (!expect(drive_into_the_world_stream(h, 0), "indicators: the world stream runs"))
			return false;
		const hud::NetQualityIndicators &q = h.client.net_quality_indicators();
		h.now_ms += 3500;
		(void)h.frame();
		if (!expect(q.link_error_bits == 0 && q.link_error_countdown == 0,
				"indicators: the game-start wait's silence raises no link error"))
			return false;
		// A packet lost on the way: the next one is a future, whose missing-sequence request
		// raises the incoming link error inside the wait.
		const std::vector<uint8_t> lost = frame_server_session(h.server_tx, h.server_scrk,
				h.auth.ck, {make_protocol_message(0x79, {0x10})});
		(void)h.frame({make_protocol_message(0x79, {0x10})});
		if (!expect(q.link_error_bits == static_cast<uint32_t>(hud::kNetLinkErrorIncoming) &&
						q.link_error_countdown == hud::kNetLinkErrorFrames,
				"indicators: the wait's missing-sequence request raises the incoming flag"))
			return false;
		for (int i = 0; i < 8; ++i) (void)h.frame();
		h.client.receive(lost.data(), lost.size());
		(void)h.frame();
		(void)h.frame({make_protocol_message(0x1A, {})});
		if (!expect(std::string(h.client.admission_stage_name()) ==
						std::string("awaiting the world state (S2C 0x0F)"),
				"indicators: the 0x1A's mission start ran into the final wait"))
			return false;
		for (int i = 0; i < 4; ++i) (void)h.frame();
		if (!expect(q.link_error_countdown == hud::kNetLinkErrorFrames &&
						q.link_error_alpha == 0,
				"indicators: the waits step no countdown and ramp no alpha"))
			return false;
		std::vector<uint8_t> world_state(23, 0);
		(void)h.frame({make_protocol_message(0x0F, world_state)});
		if (!expect(h.client.in_game_loop() && q.link_error_bits == 0,
				"indicators: the 0x0F clears the flags and the game loop runs"))
			return false;
		// Silent game-loop frames: the leg raises the flag once the hold-off has run out.
		bool raised = false;
		for (int i = 0; i < 800 && !raised; ++i) {
			(void)h.frame();
			raised = (q.link_error_bits & static_cast<uint32_t>(hud::kNetLinkErrorIncoming)) != 0;
		}
		if (!expect(raised, "indicators: the game loop's silence raises the incoming flag"))
			return false;
		// A packet ends the silence (its frame's leg still reads the old stamp); the next
		// main frame steps the countdown and raises nothing.
		(void)h.frame({make_protocol_message(0x79, {0x10})});
		(void)h.frame();
		if (!expect(q.link_error_countdown == hud::kNetLinkErrorFrames - 1,
				"indicators: the game loop's frame steps the countdown"))
			return false;
	}
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 0), "abandoned: the world stream runs"))
		return false;
	(void)h.frame({make_protocol_message(0x1A, {}), make_protocol_message(0x1D, {})});
	if (!expect(h.client.mission_start_aborted() && !h.client.in_game_loop() &&
					h.client.in_main_frame(),
			"abandoned: a round over in the 0x1A's pass abandons the start into the game loop"))
		return false;
	bool reported = false;
	for (int i = 0; i < 320 && !reported; ++i)
		reported = carries(sent_tags(h, h.frame()), 0x4C);
	return expect(reported, "abandoned: the game loop's frames step the 0x4C timer and report");
}

// Game_StartMission's pumps from the S2C 0x1A to the final wait for the 0x0F, by where the
// send-holdoff countdown c stood. The 0x1A's iteration of the game-start wait receives (a
// step) and sends (a build only at a spent countdown, the one immediate header-only ACK,
// reloading c to the dictated period N); the load receives twice (@0x525784, @0x525bd9) and
// builds only at c = 0 at its send pumps (@0x5258a2 after the first 0x2F, @0x526255 after
// the second and the 0x0B). What c still holds there leaves from the final 0x0F wait, whose
// every pass reads the socket and runs the receive handlers before its send: our next frame,
// its receive then its build. So c = 1 after the wait sends the first 0x2F alone and the rest
// from the 0x0F wait; c = 2 sends all three at @0x526255; c >= 3 (N after an immediate ACK)
// all three from the 0x0F wait; N = 0 builds at every pump. Nothing else leaves between the
// 0x1A and the submissions, and a mission start whose load runs after the release neither
// receives nor sends until its load ends: what arrived meanwhile is folded ahead of the held
// submissions' build, its ACK in their header and its replies behind them.
// [orig: NapiClient_WaitForGameStart @0x42cc10 - @0x42cc88, @0x42cc92, @0x42ccbf;
//  Game_StartMission @0x524360 - @0x5253ef, @0x525784, @0x525836, @0x5258a2..@0x525aca,
//  @0x525bd9, @0x525c2e, @0x52624b, @0x526255, the 0x0F wait @0x5262b4 / @0x5262be /
//  @0x5262da; CNapiNPConnection_PumpFlags @0x629780 (@0x6297ac, @0x629802);
//  CNapiNPConnection_PumpEnumeratorAndSend @0x62927b; NapiNPClientMsg_PlayerList @0x42fc35
//  (the 0x22); LP's 2026-10-11 captures: one immediate header-only ACK on 03TR, none on
//  01TR, then nothing until the submissions]
bool run_the_mission_start_submissions_follow_the_countdown() {
	struct Case {
		uint32_t countdown_before; // c ahead of the 0x1A's frame
		std::vector<std::vector<uint8_t>> at_release; // the 0x1A's frame
		uint32_t countdown_after;                     // c at the load's last send
		std::vector<std::vector<uint8_t>> next;       // the 0x0F wait's frame
		const char *what;
	};
	const Case cases[] = {
			{1, {{}}, 10, {{0x2F, 0x2F, 0x0B}},
					"spent by the 0x1A: its ACK, then all three from the 0x0F wait"},
			{2, {{0x2F}}, 11, {{0x2F, 0x0B}},
					"c = 1 at the exit: the first 0x2F alone, then the rest from the 0x0F wait"},
			{3, {{0x2F, 0x2F, 0x0B}}, 12, {},
					"c = 2 at the exit: all three at the load's last send"},
			{4, {}, 1, {{0x2F, 0x2F, 0x0B}}, "c = 3 at the exit: all three from the 0x0F wait"},
	};
	for (const Case &c : cases) {
		Harness h;
		if (!expect(drive_into_the_world_stream(h, 12), "submissions: the world stream runs"))
			return false;
		for (int i = 0; i < 24 && h.client.send_holdoff_countdown() != c.countdown_before; ++i)
			(void)h.frame();
		if (!expect(h.client.send_holdoff_countdown() == c.countdown_before,
				"submissions: the countdown stands where the case needs it"))
			return false;
		if (!expect(h.packets(h.frame({make_protocol_message(0x1A, {})}), c.at_release) &&
							h.client.send_holdoff_countdown() == c.countdown_after,
					c.what))
			return false;
		if (!expect(h.packets(h.frame(), c.next), c.what)) return false;
	}
	{
		Harness h;
		if (!expect(drive_into_the_world_stream(h, 0), "submissions: N = 0 world stream"))
			return false;
		if (!expect(h.packets(h.frame({make_protocol_message(0x1A, {})}),
							{{}, {0x2F}, {0x2F, 0x0B}}),
				"N = 0: every pump builds: the ACK, the first 0x2F, the second with the 0x0B"))
			return false;
	}
	// A shell whose load runs after the release, in the common case: the 0x1A's pass spent
	// the countdown, so the load's pumps hold all three. No frame receives or sends while it
	// loads, the lists that arrive wait, and the 0x0F wait folds them before it sends: the
	// held submissions leave first in one packet that ACKs the last list and carries the
	// lists' 0x22s behind them.
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 12), "loading: the world stream runs"))
		return false;
	h.load_declared = true;
	for (int i = 0; i < 24 && h.client.send_holdoff_countdown() != 1; ++i) (void)h.frame();
	if (!expect(h.packets(h.frame({make_protocol_message(0x1A, {})}), {{}}) &&
						h.client.mission_start_released(),
			"loading: the 0x1A's immediate ACK at c = 1; the 0x1A releases the mission start"))
		return false;
	const uint32_t flushes = h.client.send_flush_counter();
	for (int i = 0; i < 6; ++i) {
		if (!expect(h.frame({make_protocol_message(0x16, player_list({{0, 1}}))}).empty() &&
							h.client.send_flush_counter() == flushes,
				"loading: the frames neither receive nor send while the load runs"))
			return false;
	}
	// The load zeroes the frame tick, g_CurrentTick's stand-in (D-NET-690), and the 0x0F
	// wait that follows is no main frame: it stays 0 [orig: Game_StartMission @0x524360 -
	// `mov g_CurrentTick, ebx` @0x525B9F; Game_StartMission's final wait @0x52628d..0x5262df].
	h.client.view().step_frame_tick();
	const bool ticked = h.client.view().frame_tick() != 0;
	if (!expect(h.client.push_mission_start_end().empty() && !h.client.mission_start_released(),
			"loading: the load's pumps hold all three (c = 12 at the exit)"))
		return false;
	if (!expect(ticked && h.client.view().frame_tick() == 0, "loading: the load zeroes the frame tick"))
		return false;
	const std::vector<std::vector<uint8_t>> after = h.frame();
	if (!expect(h.client.view().frame_tick() == 0, "loading: the 0x0F wait is no main frame"))
		return false;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	bool replies_behind = after.size() == 1 &&
			decode_client_session(after[0], h.auth.scrk, header, messages) &&
			messages.size() > 3 && messages[0].tag == 0x2F && messages[1].tag == 0x2F &&
			messages[2].tag == 0x0B && header.ack_count == h.server_tx.next_outbound_seq - 1;
	for (std::size_t i = 3; replies_behind && i < messages.size(); ++i)
		replies_behind = messages[i].tag == 0x22;
	return expect(replies_behind,
			"loading: the 0x0F wait folds the held lists first: the submissions leave ACKing "
			"the last list, its 0x22s behind them");
}

// D-NET-861: the load's two legs. A shell whose load runs after the release calls the
// first leg once its definitions and models are in. With the countdown at 1 at the wait's exit
// (c = 2 ahead of the 0x1A's frame), the leg's receive spends it and the pump after the first
// 0x2F's queue sends that 0x2F alone, mid-load, while the load's frames still neither receive
// nor send; the end leg holds the second 0x2F and the 0x0B, which leave from the 0x0F wait.
// Retail's x7 wire shows the same split (03tr-dm-rr: the first 0x2F at +1.18 s after the 0x1A,
// the host's 0x5A at +1.34 s, the second 0x2F with the 0x0B at +3.86 s).
// [orig: Game_StartMission @0x524360 - @0x5253ef, the loads @0x525495..0x525762, @0x525784,
//  @0x525836, @0x5258a2 / @0x5258ea / @0x5259f1 / @0x525a61, the freeze @0x525a6e, @0x525aca,
//  @0x525bd9, @0x525c2e, @0x52624b, @0x526255]
bool run_the_mission_start_first_leg_runs_mid_load() {
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 12), "first leg: the world stream runs"))
		return false;
	h.load_declared = true;
	for (int i = 0; i < 24 && h.client.send_holdoff_countdown() != 2; ++i) (void)h.frame();
	if (!expect(h.client.send_holdoff_countdown() == 2, "first leg: c = 2 ahead of the 0x1A"))
		return false;
	if (!expect(h.frame({make_protocol_message(0x1A, {})}).empty() &&
	                    h.client.mission_start_released() &&
	                    h.client.send_holdoff_countdown() == 1,
			"first leg: the 0x1A's frame sends nothing at c = 2 and releases the mission start"))
		return false;
	if (!expect(h.frame().empty() && h.client.send_holdoff_countdown() == 1,
			"first leg: the load's frames neither receive nor send"))
		return false;
	if (!expect(h.packets(h.client.push_mission_start_first_loadout(), {{0x2F}}) &&
	                    h.client.mission_start_released(),
			"first leg: its receive spends the countdown, the first 0x2F leaves alone mid-load"))
		return false;
	if (!expect(h.client.push_mission_start_first_loadout().empty() && h.frame().empty(),
			"first leg: it runs once, and the load's frames stay quiet"))
		return false;
	if (!expect(h.client.push_mission_start_end().empty() &&
	                    !h.client.mission_start_released() &&
	                    h.client.send_holdoff_countdown() == 11,
			"end leg: the second 0x2F and the 0x0B wait in the queue at c = 11"))
		return false;
	return expect(h.packets(h.frame(), {{0x2F, 0x0B}}),
			"end leg: the 0x0F wait sends the second 0x2F with the 0x0B");
}

// D-NET-861: the mission start's call has no frame between its legs. The game-start
// wait's pass whose receive takes the S2C 0x1A goes straight on into the load (the first leg
// inside it), the settle, the push and the final wait's first pass, all in that one call
// (JoinerRole::poll_mission_start); a pass that takes no 0x1A runs none of them. The push's
// countdown holds the second 0x2F and the 0x0B (c = 11, as above), and the final wait's first
// pass, inside the same call, ships them. The embedder's 100 % loading present runs inside the
// push, after the settle and ahead of anything the push sends (D-NET-1160).
// [orig: NapiClient_WaitForGameStart @0x42CC10 - the receive @0x42CC88, the send @0x42CC92,
//  the flag test @0x42CCB9..0x42CCBF, the return @0x42CCC1; Game_StartMission @0x524360 -
//  the call @0x524E29, the first 0x2F @0x525836, the second 0x2F @0x525C2E, `push 64h`
//  @0x525D22, LoadingScreen_UpdateAndPresent @0x525D29, the 0x0B @0x52624B, the send
//  @0x526255, the final wait's first pass @0x5262B4 / @0x5262BE / @0x5262DA]
bool run_the_mission_start_runs_in_the_0x1a_pass() {
	RoleHarness h;
	if (!expect(drive_into_the_world_stream(h, 12), "mission start call: the world stream runs"))
		return false;
	h.load_declared = true;
	for (int i = 0; i < 24 && h.client.send_holdoff_countdown() != 3; ++i) (void)h.frame();
	std::vector<std::string> order;
	std::vector<std::vector<uint8_t>> first_leg;
	std::size_t load_from = 0;
	std::size_t settle_from = 0;
	std::size_t present_from = 0;
	inmatch::JoinerRole::MissionStartLegs legs;
	legs.after_pass = [&] {
		order.push_back(h.client.mission_start_released() ? "pass 0x1A" : "pass");
		return true;
	};
	legs.load = [&] {
		order.push_back("load");
		load_from = h.socket.sent.size();
		h.role.push_mission_start_first_loadout();
		first_leg.assign(h.socket.sent.begin() + static_cast<std::ptrdiff_t>(load_from),
				h.socket.sent.end());
		return true;
	};
	legs.settle = [&] {
		order.push_back("settle");
		settle_from = h.socket.sent.size();
		return true;
	};
	legs.present = [&] {
		order.push_back("present");
		present_from = h.socket.sent.size();
	};
	if (!expect(h.client.send_holdoff_countdown() == 3 &&
	                    h.role.poll_mission_start(legs) ==
	                            inmatch::JoinerRole::MissionStartPass::Waiting &&
	                    order == std::vector<std::string>{"pass"} &&
	                    h.client.send_holdoff_countdown() == 2,
			"mission start call: a pass with no 0x1A runs no leg"))
		return false;
	order.clear();
	const std::size_t call_from = h.socket.sent.size();
	const std::vector<uint8_t> release =
			frame_server_session(h.server_tx, h.server_scrk, h.auth.ck,
					{make_protocol_message(0x1A, {})});
	h.client.receive(release.data(), release.size());
	if (!expect(h.role.poll_mission_start(legs) ==
	                    inmatch::JoinerRole::MissionStartPass::Started &&
	                    order == std::vector<std::string>{"pass 0x1A", "load", "settle", "present"},
			"mission start call: the 0x1A's pass runs the load, then the settle, then the push's "
			"100 % present, in its call"))
		return false;
	if (!expect(present_from == settle_from,
			"mission start call: the 100 % present runs inside the push, ahead of its sends"))
		return false;
	if (!expect(load_from == call_from && h.packets(first_leg, {{0x2F}}) &&
	                    settle_from == load_from + first_leg.size(),
			"mission start call: the 0x1A's pass sends nothing at c = 2, the load's first leg "
			"the first 0x2F alone"))
		return false;
	const std::vector<std::vector<uint8_t>> after_settle(
			h.socket.sent.begin() + static_cast<std::ptrdiff_t>(settle_from), h.socket.sent.end());
	return expect(h.packets(after_settle, {{0x2F, 0x0B}}) && h.client.in_final_wait(),
			"mission start call: the final wait's first pass, inside the push, ships the "
			"held second 0x2F with the 0x0B");
}

// D-NET-1183: the mission start's role legs sit at retail's points in the push.
// Player_InitPlayer runs right after the submissions' take binds the local entity, ahead of
// the receive and the second 0x2F's queue; the null-tested block with the proximity rebuild
// runs past that queue; then the 100 % present and the Attack & Defend latch, ahead of the
// 0x0B's queue. Under a dictated period of 12 with the countdown at 1 at the wait's exit, the
// push's first leg sends the first 0x2F alone and its end holds the second 0x2F and the 0x0B
// for the 0x0F wait.
// [orig: Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC (Player_InitPlayer
//  @0x4E15F0 - the store @0x4E17EB), the receive @0x525BD9, the second 0x2F @0x525C2E,
//  Entity_BuildAllProximityLists @0x525C90, LoadingScreen_UpdateAndPresent @0x525D29,
//  Game_LatchAttackDefendRole @0x5260C1, the 0x0B @0x52624B, the send @0x526255]
bool run_the_mission_start_legs_keep_retails_order() {
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 12), "legs: the world stream runs")) return false;
	h.load_declared = true;
	(void)h.frame({self_spawn_record()});
	for (int i = 0; i < 24 && h.client.send_holdoff_countdown() != 2; ++i) (void)h.frame();
	(void)h.frame({make_protocol_message(0x1A, {})});
	if (!expect(h.client.mission_start_released() && h.client.has_self_handle() &&
	                    !h.client.local_entity_bound(),
			"legs: the 0x1A released the mission start, the self entity not yet bound"))
		return false;
	std::vector<std::string> order;
	std::vector<std::size_t> queued;
	bool bound_at_init = false;
	const auto leg = [&](const char *name) {
		order.push_back(name);
		queued.push_back(h.client.queued_send_count());
	};
	inmatch::ClientRuntime::MissionStartEndLegs legs;
	legs.init_player = [&] {
		leg("init_player");
		bound_at_init = h.client.local_entity_bound();
	};
	legs.start_block = [&] { leg("start_block"); };
	legs.present = [&] { leg("present"); };
	legs.latch = [&] { leg("latch"); };
	const std::vector<std::vector<uint8_t>> pushed = h.client.push_mission_start_end(legs);
	if (!expect(order == std::vector<std::string>{"init_player", "start_block", "present",
	                                              "latch"},
			"legs: the player init, the block, the present, the latch, in that order"))
		return false;
	if (!expect(bound_at_init, "legs: the player init runs on the local entity its take bound"))
		return false;
	if (!expect(queued[1] == queued[0] + 1 && queued[2] == queued[1] && queued[3] == queued[1],
			"legs: the second 0x2F is queued between the player init and the block; the "
			"present and the latch run ahead of the 0x0B's queue"))
		return false;
	if (!expect(h.packets(pushed, {{0x2F}}) && h.client.send_holdoff_countdown() == 11,
			"legs: the push sends the first 0x2F alone and holds the rest at c = 11"))
		return false;
	return expect(h.packets(h.frame(), {{0x2F, 0x0B}}),
			"legs: the 0x0F wait sends the second 0x2F with the 0x0B behind the legs");
}

// D-NET-1183: a join's L is born in its mission start, as retail's Player_InitPlayer binds
// and inits the self entity the world stream's 0x0C built: not in the 0x1A's pass or the load,
// but in the push, so the embedder's 100 % present already finds L, which the block left
// hidden, and the call ends in the final wait with L ahead of any release.
// [orig: Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC, `or [eax+24h], edi`
//  @0x525C9E, LoadingScreen_UpdateAndPresent @0x525D29, the 0x0B @0x52624B;
//  Player_InitPlayer @0x4E15F0 - Player_FindLocalPlayerEntity @0x4E17E4, the store @0x4E17EB]
bool run_the_mission_start_push_bears_l() {
	RoleHarness h;
	if (!expect(drive_into_the_world_stream(h, 0), "birth: the world stream runs")) return false;
	h.load_declared = true;
	(void)h.frame({self_spawn_record()});
	if (!expect(h.client.has_self_handle() && !h.role.local_spawned(),
			"birth: the world stream's self record bears no L"))
		return false;
	const std::vector<uint8_t> release = frame_server_session(h.server_tx, h.server_scrk,
			h.auth.ck, {make_protocol_message(0x1A, {})});
	h.client.receive(release.data(), release.size());
	bool spawned_at_load = true;
	bool spawned_at_present = false;
	bool hidden_at_present = false;
	inmatch::JoinerRole::MissionStartLegs legs;
	legs.load = [&] {
		spawned_at_load = h.role.local_spawned();
		return true;
	};
	legs.present = [&] {
		spawned_at_present = h.role.local_spawned();
		const world::Entity *L = h.kernel->local.player();
		hidden_at_present = L != nullptr && (L->flags & world::kEntityFlagCarried) != 0u;
	};
	if (!expect(h.role.poll_mission_start(legs) == inmatch::JoinerRole::MissionStartPass::Started,
			"birth: the mission start's call runs"))
		return false;
	if (!expect(!spawned_at_load, "birth: no L in the 0x1A's pass or the load")) return false;
	if (!expect(spawned_at_present && hidden_at_present,
			"birth: the push bears L ahead of the 100 % present, hidden by the block"))
		return false;
	return expect(h.client.in_final_wait() && !h.client.in_match() && h.role.local_spawned(),
			"birth: the call ends in the final wait with L, ahead of any release");
}

// D-NET-1184: the mission start's second 0x2F reads its words at its build, past
// Player_InitPlayer and the receive: the init stores g_CurrentWeaponSlot (195, then its weapon
// leg's select), which the second 0x2F reads @0x525C1A, its team @0x525BE8. A slot the init leg
// moves goes out in the second 0x2F; the first 0x2F, queued ahead of the init, keeps 195.
// [orig: Game_StartMission @0x524360 - the first 0x2F @0x525836, Player_InitPlayer @0x525BBC,
//  the receive @0x525BD9, g_LocalTeam @0x525BE8, g_CurrentWeaponSlot @0x525C1A,
//  NetPacket_SendLoadoutSubmit @0x525C2E; Player_InitPlayer @0x4E15F0 - g_CurrentWeaponSlot = 195
//  @0x4E17FF, Player_SelectWeaponSlot @0x4E1995]
bool run_the_second_0x2f_reads_its_words_past_the_init() {
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 0), "second 0x2F: the world stream runs"))
		return false;
	h.load_declared = true;
	(void)h.frame({self_spawn_record()});
	(void)h.frame({make_protocol_message(0x1A, {})});
	if (!expect(h.client.mission_start_released(),
			"second 0x2F: the 0x1A released the mission start"))
		return false;
	inmatch::ClientRuntime::MissionStartEndLegs legs;
	legs.init_player = [&] {
		inmatch::JoinerConnection::LoadoutKit kit;
		kit.equipped_combo = 7;
		h.client.set_loadout_kit(std::move(kit));
	};
	std::vector<std::vector<uint8_t>> sent = h.client.push_mission_start_end(legs);
	const std::vector<std::vector<uint8_t>> wait_pass = h.frame();
	sent.insert(sent.end(), wait_pass.begin(), wait_pass.end());
	std::vector<uint32_t> slots;
	for (const std::vector<uint8_t> &datagram : sent) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(datagram, h.auth.scrk, header, messages)) continue;
		for (const ProtocolMessage &m : messages) {
			LoadoutSubmit submit;
			if (m.tag == 0x2F && decode_loadout_submit(m.payload.data(), m.payload.size(), submit))
				slots.push_back(submit.weapon_slot_index);
		}
	}
	return expect(slots == std::vector<uint32_t>({195u, 7u}),
			"second 0x2F: the first keeps 195, the second carries the slot the init left");
}

// Each sent C2S 0x2F in `sent`, in order, with the ACK of the packet that carried it.
struct SentSubmit {
	LoadoutSubmit submit;
	uint32_t ack = 0;
};
template <class H>
std::vector<SentSubmit> sent_loadout_submits(H &h, const std::vector<std::vector<uint8_t>> &sent) {
	std::vector<SentSubmit> out;
	for (const std::vector<uint8_t> &datagram : sent) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_client_session(datagram, h.auth.scrk, header, messages)) continue;
		for (const ProtocolMessage &m : messages) {
			SentSubmit s;
			s.ack = header.ack_count;
			if (m.tag == 0x2F && decode_loadout_submit(m.payload.data(), m.payload.size(), s.submit))
				out.push_back(s);
		}
	}
	return out;
}

std::vector<uint8_t> submit_teams(const std::vector<SentSubmit> &submits) {
	std::vector<uint8_t> teams;
	for (const SentSubmit &s : submits) teams.push_back(s.submit.team);
	return teams;
}

// The self S2C 0x50 moving the joiner's own entity (slot 0xC6) to team 1.
ProtocolMessage self_team_1() {
	return make_protocol_message(0x50, {0xC6, 0x00, 0x01, 0x00, 0x00, 0x00});
}

// D-NET-1209: a NovaWorld join's mission start reads the socket at its loading steps: its
// session's periodic update runs a manager pump on the game's one socket, so what the host sends
// during the load (its 0x5A answer to the first 0x2F, a 0x50) waits on the client queue, and the
// receive past Player_InitPlayer runs its handler ahead of the second 0x2F's build: the 0x5A's
// slot selection (the embedder's kit fold, g_CurrentWeaponSlot) and the 0x50's team latch move
// the second 0x2F's words, the 0x50's own re-submission queues ahead of it, and the packet that
// carries it ACKs the answer. A LAN session's update returns at its head: the datagram waits on
// the socket for the final wait's first manager receive, past the second 0x2F, whose words keep
// the old team and slot 195.
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - @0x4D4403 / @0x4D4411,
//  NapiNPManager_Pump(flags 4) @0x4D441F; Game_StartMission @0x524360 - the updates @0x525A8E /
//  @0x525B09, Player_InitPlayer @0x525BBC, the receive @0x525BD9, g_LocalTeam @0x525BE8,
//  g_CurrentWeaponSlot @0x525C1A, the second 0x2F @0x525C2E, the present @0x525D29, the final
//  wait's PumpManagerReceive @0x5262B4; NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - the
//  selection @0x4296E3; NapiNPClientMsg_TeamAssign @0x431910 - the latch @0x4319DB, the submit
//  @0x431AAD]
bool run_a_novaworld_load_runs_its_handlers_ahead_of_the_second_0x2f() {
	for (const bool team_assign : {false, true}) {
		for (const bool novaworld : {true, false}) {
			RoleHarness h;
			h.role.set_join_novaworld(novaworld);
			// The embedder's kit fold: a grant's selection moves the equipped slot.
			uint64_t kit_revision = 0;
			h.role.kit_seams.apply_authoritative = [&h, &kit_revision] {
				if (h.client.authoritative_loadout_revision() == kit_revision) return;
				kit_revision = h.client.authoritative_loadout_revision();
				inmatch::JoinerConnection::LoadoutKit kit;
				kit.equipped_combo = 7;
				h.client.set_loadout_kit(std::move(kit));
			};
			if (!expect(drive_into_the_world_stream(h, 0), "in-load answer: the world stream runs"))
				return false;
			h.load_declared = true;
			(void)h.frame({self_spawn_record()});
			(void)h.frame({make_protocol_message(0x1A, {})});
			if (!expect(h.client.mission_start_released(),
					"in-load answer: the 0x1A released the mission start"))
				return false;
			const std::size_t from = h.socket.sent.size();
			h.role.push_mission_start_first_loadout();
			// The host's answer lands on the socket during the load.
			const std::vector<uint8_t> answer = frame_server_session(h.server_tx, h.server_scrk,
					h.auth.ck, {team_assign ? self_team_1() : first_grant()});
			const uint32_t answer_seq = h.server_tx.next_outbound_seq - 1;
			h.client.receive(answer.data(), answer.size());
			bool handled_by_present = false;
			h.role.push_mission_start_end([&] {
				handled_by_present = team_assign ? h.client.assigned_team() == 1
				                                 : h.client.authoritative_loadout_revision() != 0;
			});
			const std::vector<std::vector<uint8_t>> sent(
					h.socket.sent.begin() + static_cast<std::ptrdiff_t>(from), h.socket.sent.end());
			const std::vector<SentSubmit> submits = sent_loadout_submits(h, sent);
			const char *what = novaworld ? "NovaWorld: the receive past the init runs the answer"
			                             : "LAN: the answer waits for the final wait";
			if (!expect(handled_by_present == novaworld, what)) return false;
			if (team_assign) {
				// The first 0x2F; on NovaWorld the 0x50's re-submission, then the second on team 1;
				// on LAN the second on team 2, the re-submission from the final wait's pass.
				const std::vector<uint8_t> want = novaworld ? std::vector<uint8_t>{2, 1, 1}
				                                            : std::vector<uint8_t>{2, 2, 1};
				if (!expect(submit_teams(submits) == want, what)) return false;
				continue;
			}
			if (!expect(submits.size() == 2 && submits[0].submit.weapon_slot_index == 195u &&
			                    submits[1].submit.weapon_slot_index == (novaworld ? 7u : 195u),
					what))
				return false;
			if (!expect((submits[1].ack >= answer_seq) == novaworld,
					novaworld ? "NovaWorld: the second 0x2F's packet ACKs the in-load 0x5A"
					          : "LAN: the second 0x2F's packet leaves ahead of the 0x5A's ACK"))
				return false;
		}
	}
	return true;
}

// D-NET-1209 (V-ROLL-AY N1): the update past the receive reads the socket but runs no
// handler ahead of the second 0x2F: a self 0x50 that lands after the update ahead of the init
// (@0x525B09) is read at @0x525BE3 and waits on the client queue, so the second 0x2F keeps team
// 2 and the final wait's first pass latches the team and re-submits; one that lands during the
// load is run at the receive @0x525BD9 and moves the second 0x2F to team 1. The load's receive
// pumps read no socket themselves: on a LAN session (no periodic deposit) the in-time 0x50 waits
// too.
// [orig: Game_StartMission @0x524360 - the update @0x525B09, Player_InitPlayer @0x525BBC, the
//  receive @0x525BD9, the update @0x525BE3, g_LocalTeam @0x525BE8, the second 0x2F @0x525C2E;
//  CNapiNetwork_PumpClientProtocolRecv @0x4C4FE0 (flags 26)]
bool run_the_update_past_the_receive_runs_no_handler() {
	enum class Lands { InTime, AtTheInit, LanInTime };
	for (const Lands lands : {Lands::InTime, Lands::AtTheInit, Lands::LanInTime}) {
		Harness h;
		if (!expect(drive_into_the_world_stream(h, 0), "update past the receive: the world stream"))
			return false;
		h.load_declared = true;
		(void)h.frame({self_spawn_record()});
		(void)h.frame({make_protocol_message(0x1A, {})});
		const auto land = [&h] {
			const std::vector<uint8_t> dg =
					frame_server_session(h.server_tx, h.server_scrk, h.auth.ck, {self_team_1()});
			h.client.receive(dg.data(), dg.size());
		};
		inmatch::ClientRuntime::MissionStartEndLegs legs;
		if (lands != Lands::LanInTime) legs.periodic_deposit = [] {}; // the socket is the queue
		std::vector<std::vector<uint8_t>> sent = h.client.push_mission_start_first_loadout(legs);
		if (lands != Lands::AtTheInit) land();
		if (lands == Lands::AtTheInit) legs.init_player = land;
		const std::vector<std::vector<uint8_t>> pushed = h.client.push_mission_start_end(legs);
		sent.insert(sent.end(), pushed.begin(), pushed.end());
		const std::vector<uint8_t> at_push = submit_teams(sent_loadout_submits(h, sent));
		const std::vector<uint8_t> at_wait = submit_teams(sent_loadout_submits(h, h.frame()));
		const bool in_time = lands == Lands::InTime;
		if (!expect(at_push == (in_time ? std::vector<uint8_t>{2, 1, 1} : std::vector<uint8_t>{2, 2}) &&
		                    at_wait == (in_time ? std::vector<uint8_t>{} : std::vector<uint8_t>{1}),
				in_time ? "in time: the receive runs the 0x50 ahead of the second 0x2F"
				        : "late or LAN: the 0x50 waits for the final wait's first pass"))
			return false;
	}
	return true;
}

// D-NET-1209: the embedder's loading steps run the session's periodic update between the
// load's pumps. On a NovaWorld join its manager pump answers a host's WR ping at once, mid-load
// (the 0x85's 0x45 pong from inside the receive); on a LAN join the update reads nothing and the
// ping waits for the final wait's first manager receive. Outside a declared load the step does
// nothing (the frames read the socket themselves). The S2C 0x11's declared terrain load runs
// the same updates: its manager receive pongs at once, and once the load ends the step reads
// nothing.
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - @0x4D4403 / @0x4D4411,
//  NapiNPManager_Pump(flags 4) @0x4D441F; Nwu_HandlePing @0x623A70; Game_StartMission @0x524360 -
//  the updates @0x525405..0x52571F, the final wait's PumpManagerReceive @0x5262B4;
//  Game_LoadTerrainDuringConnect @0x520710 - the updates @0x52072C / @0x5207E7]
bool run_a_loading_step_answers_a_ping_on_novaworld() {
	{
		Harness h;
		h.client.set_sync_tail_loading(true);
		(void)drive_into_the_world_stream(h, 0); // the 0x11's load holds the 0x0A
		const std::vector<uint8_t> ping = nw_encode_outbound(
				SESSION_OPCODE_SERVER_PING, build_session_ping_body(h.auth.ck, true, 0x55667788u));
		h.client.receive(ping.data(), ping.size());
		if (!expect(h.client.declared_load_running() &&
		                    h.client.pump_mission_start_manager_receive().size() == 1,
				"terrain load: the 0x11's declared load's update pongs at once"))
			return false;
		h.client.set_sync_tail_loading(false);
		(void)h.frame(); // the 0x0A: the load is over
		h.client.receive(ping.data(), ping.size());
		if (!expect(!h.client.declared_load_running() &&
		                    h.client.pump_mission_start_manager_receive().empty(),
				"terrain load: past the load the step reads nothing (the frames do)"))
			return false;
	}
	for (const bool novaworld : {true, false}) {
		RoleHarness h;
		h.role.set_join_novaworld(novaworld);
		if (!expect(drive_into_the_world_stream(h, 0), "loading step: the world stream runs"))
			return false;
		h.load_declared = true;
		const std::vector<uint8_t> early = nw_encode_outbound(
				SESSION_OPCODE_SERVER_PING, build_session_ping_body(h.auth.ck, true, 0x11223344u));
		h.client.receive(early.data(), early.size());
		std::size_t from = h.socket.sent.size();
		h.role.pump_mission_start_periodic_update();
		if (!expect(h.socket.sent.size() == from,
				"loading step: nothing outside a declared load"))
			return false;
		(void)h.frame({make_protocol_message(0x1A, {})});
		const std::vector<uint8_t> ping = nw_encode_outbound(
				SESSION_OPCODE_SERVER_PING, build_session_ping_body(h.auth.ck, true, 0xAABBCCDDu));
		h.client.receive(ping.data(), ping.size());
		from = h.socket.sent.size();
		h.role.pump_mission_start_periodic_update();
		uint8_t opcode = 0;
		std::vector<uint8_t> pong_body;
		SessionPingBody parsed;
		const bool ponged = h.socket.sent.size() == from + 1 &&
				nw_decode_inbound(h.socket.sent[from].data(), h.socket.sent[from].size(), opcode,
						pong_body) &&
				opcode == SESSION_OPCODE_CLIENT_PING &&
				parse_session_ping_body(pong_body.data(), pong_body.size(), parsed) &&
				parsed.timestamp_ms == 0xAABBCCDDu;
		if (!expect(ponged == novaworld && (novaworld || h.socket.sent.size() == from),
				novaworld ? "NovaWorld: the loading step's update pongs the WR ping at once"
				          : "LAN: the loading step's update reads nothing"))
			return false;
	}
	return true;
}

// D-NET-1238: each periodic update of a mission load runs the process's NovaWorld session's
// own half (its NOVAWORLDUDP protocol's pump, its state and GLSVSS legs), whatever the role. A
// host's loading step (no join) runs it alone, once, and nothing without a held session. A
// NovaWorld join's runs it past the update's manager receive: the WR ping's pong is already on
// the socket when the session's half runs; nothing runs outside a declared load or on a LAN
// join. In the mission start's legs the session's half runs once per periodic update: the first
// leg's six (one ahead of its receive, one past it, one per send pump of the four) and the push's
// four.
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - NapiNPManager_Pump(flags 4) @0x4D441F
//  ahead of NapiNPProtocol_Pump(-1) @0x4D442E; Game_StartMission @0x524360 - the head's update
//  @0x5243CA (no role test), the updates @0x525405..0x52571F, @0x52578E, @0x525889, @0x5258BC,
//  @0x525962, @0x525A57, @0x525A8E, @0x525B09, @0x525BE3, @0x525D33]
bool run_a_loading_step_runs_the_novaworld_sessions_update() {
	{
		// A host's load: no join to pump, the held session's half alone.
		inmatch::ClientProcessState process;
		int updates = 0;
		inmatch::run_loading_step(nullptr, process); // no NovaWorld session held: nothing
		process.nwu_periodic_update = [&updates] { ++updates; };
		inmatch::run_loading_step(nullptr, process);
		if (!expect(updates == 1, "host: a loading step runs the held session's half alone"))
			return false;
	}
	for (const bool novaworld : {true, false}) {
		// The process outlives the harness whose runtime points at it.
		inmatch::ClientProcessState process;
		std::vector<std::size_t> sent_at_update;
		RoleHarness h;
		process.nwu_periodic_update = [&h, &sent_at_update] {
			sent_at_update.push_back(h.socket.sent.size());
		};
		h.role.use_process_state(&process);
		h.role.set_join_novaworld(novaworld);
		if (!expect(drive_into_the_world_stream(h, 0), "session update: the world stream runs"))
			return false;
		h.load_declared = true;
		sent_at_update.clear();
		inmatch::run_loading_step(&h.role, process);
		if (!expect(sent_at_update.empty(), "session update: nothing outside a declared load"))
			return false;
		(void)h.frame({make_protocol_message(0x1A, {})});
		const std::vector<uint8_t> ping = nw_encode_outbound(
				SESSION_OPCODE_SERVER_PING, build_session_ping_body(h.auth.ck, true, 0x0BADF00Du));
		h.client.receive(ping.data(), ping.size());
		sent_at_update.clear();
		const std::size_t from = h.socket.sent.size();
		inmatch::run_loading_step(&h.role, process);
		if (!expect(novaworld ? sent_at_update == std::vector<std::size_t>{from + 1}
		                      : sent_at_update.empty(),
				novaworld ? "NovaWorld: the loading step runs the session's half past the pong"
				          : "LAN: the loading step runs no session update"))
			return false;
	}
	for (const bool novaworld : {true, false}) {
		inmatch::ClientProcessState process;
		int updates = 0;
		Harness h;
		process.nwu_periodic_update = [&updates] { ++updates; };
		h.client.use_process_state(&process);
		if (!expect(drive_into_the_world_stream(h, 0), "session update: the world stream (legs)"))
			return false;
		h.load_declared = true;
		(void)h.frame({self_spawn_record()});
		(void)h.frame({make_protocol_message(0x1A, {})});
		inmatch::ClientRuntime::MissionStartEndLegs legs;
		if (novaworld) legs.periodic_deposit = [] {}; // the socket is the queue
		updates = 0;
		(void)h.client.push_mission_start_first_loadout(legs);
		const int first_leg = updates;
		(void)h.client.push_mission_start_end(legs);
		if (!expect(first_leg == (novaworld ? 6 : 0) && updates == (novaworld ? 10 : 0),
				novaworld ? "NovaWorld: each of the legs' periodic updates runs the session's half"
				          : "LAN: the legs' updates run no session update"))
			return false;
	}
	return true;
}

// The joiner's round clock at its mission start: retail never seeds it. Game_StartMission's
// round-clock seed (g_RoundTimeRemaining = g_RoundTimeLimitMinutes x 3720 when in session,
// outside the Co-op waypoint family, with a nonzero limit) sits on the authority / single-player
// arm; the joiner's arm runs its two waits and jumps past the seed and the pump that follows it,
// so its clock is whatever the common path's -1 or the waits' 0x0A folds left, until the next
// phase-1 0x0A (the host's seed is Match::configure's). A timed team game whose joiner's own cfg
// limit is 7 keeps a folded 100 s past the 0x1A, and the remote admin's GET reads it as
// 6200 / 60 / 62 = 1 over the joiner's own limit (D-NET-1451, NOT-A-DIVERGENCE).
// [orig: Game_StartMission @0x524360 - the in-session split @0x524D7D..0x524D8F (`jnz
//  loc_524F2B` @0x524D8F, the authority arm); the joiner's arm NapiClient_SendPlayerSyncAndWait-
//  ForSyncTail @0x524DAB, NapiClient_WaitForGameStart @0x524E29, `jmp loc_52526C` @0x524E7D,
//  past the seed @0x525229..0x525251 (`imul eax, 0E88h` @0x52524B; reached only from
//  @0x52520D..0x525224 and @0x525025..0x5251A5) and Server_PumpNetworkTransport @0x525256;
//  the -1 @0x524A5D;
//  CAdminServer_HandleGet @0x403390 (@0x403888..0x4038B8)]
bool run_the_joiner_mission_start_keeps_the_folded_round_clock() {
	const int32_t folded = 62 * 100; // a phase-1 0x0A's 100 s, folded in the waits
	Harness h;
	gamecfg::GameCfg cfg = gamecfg::defaults();
	cfg.time_limit = 7;
	if (!expect(drive_into_the_world_stream(h, 0), "clock: the world stream runs")) return false;
	std::vector<uint8_t> config(51, 0); // S2C 0x08: field 3 = g_GameType
	for (int i = 0; i < 4; ++i)
		config[12 + i] = static_cast<uint8_t>(game_type::kTeamDeathmatch >> (8 * i));
	(void)h.frame({make_protocol_message(s2c::SESSION_CONFIG, config)});
	h.client.state().round_time_remaining_ticks = folded;
	(void)h.frame({make_protocol_message(0x1A, {})});
	if (!expect(h.client.game_type() == game_type::kTeamDeathmatch &&
					h.client.state().round_time_remaining_ticks == folded,
			"clock: a timed team game's joiner keeps the folded clock past its mission start"))
		return false;
	inmatch::RemoteAdminConsoles consoles;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &cfg;
	seams.config_title = opennova::title_by_code("jo");
	inmatch::RemoteAdminBinding joined;
	joined.client = &h.client;
	joined.session = 1;
	consoles.bind(joined, seams);
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	std::vector<std::string> replies;
	(void)consoles.dispatch(AdminSession{"boss", 0xFFFFFFFFu}, "GET GAMESETTINGS", replies);
	return expect(replies.size() == 1 &&
					replies[0].find("GameTime             = 1/7\n") != std::string::npos,
			"clock: the admin's GET reads the folded clock over the joiner's own limit (6200 / 60 / 62 = 1)");
}

// The client's net-frame counter, the C2S 0x02 echo's third dword: one per process, stepped
// once per pass of the join screen's state-6 wait (the S2C 0x00 answered, the verification
// value 0) after that pass's receive and send, never reset (D-NET-733).
// [orig: MultiPlayer_PumpJoinVerificationFrame @0x424740 - the value test @0x424747, ++
//  g_ClientNetFrameCounter @0x424776; MultiPlayer_JoinSessionStateMachine @0x56A685;
//  NapiNPClientMsg_HandleJoinResponse - the read @0x42e12d, NetPacket_WritePositionWith-
//  Padding @0x42e140]
bool run_the_net_frame_counter_steps_in_the_verification_wait() {
	Harness h;
	if (!expect(h.handshake(), "counter: handshake")) return false;
	(void)h.frame({settings_record(0), settings_record(1)});
	const uint32_t before = inmatch::JoinerConnection::client_net_frame_counter();
	(void)h.frame({make_protocol_message(0x00, {})});
	if (!expect(inmatch::JoinerConnection::client_net_frame_counter() == before,
			"counter: the frame that answered the S2C 0x00 was no state-6 pass"))
		return false;
	for (int i = 0; i < 3; ++i) (void)h.frame();
	if (!expect(inmatch::JoinerConnection::client_net_frame_counter() == before + 3,
			"counter: each state-6 frame steps it once"))
		return false;
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	const std::vector<std::vector<uint8_t>> sent = h.frame({make_protocol_message(0x02, probe)});
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(sent.size() == 1 && decode_client_session(sent[0], h.auth.scrk, header, messages) &&
						messages.size() == 1 && messages[0].tag == 0x02 &&
						messages[0].payload.size() >= 12 &&
						io::read_u32_le(messages[0].payload.data() + 8) == before + 3,
			"counter: the echo carries the count before the probe's own pass steps it"))
		return false;
	(void)h.frame({make_protocol_message(0x01, {0x01, 0x00, 0x00, 0x00})});
	const uint32_t at_verified = inmatch::JoinerConnection::client_net_frame_counter();
	(void)h.frame();
	(void)h.frame();
	return expect(at_verified == before + 5 &&
						inmatch::JoinerConnection::client_net_frame_counter() == at_verified,
			"counter: the verified frames are no state-6 passes");
}

// The C2S 0x09 needs no S2C 0x16: with no list yet it leaves alone at
// WaitForDisconnect's first pace, the 0x11 then releases the 0x0A, and a list
// arriving afterwards draws its one 0x22 per unbound row in a later packet: the
// retail corpus run_20260426_120102 ([0x09], [0x0A], then [0x22 00 F7 1C]); there
// is no second 0x22 for slot 0 (D-NET-526).
// [orig: Game_StartMission @0x524dab -> NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42CB20
//  (queue @0x42cb43, pace @0x42cbb5); NapiNPClientMsg_PlayerList @0x42fc35]
bool run_the_sync_request_needs_no_player_list() {
	Harness h;
	if (!expect(h.handshake(), "sync: handshake")) return false;
	h.frame({settings_record(0), settings_record(1)});
	h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	h.frame({make_protocol_message(0x02, probe)});
	h.frame({holdoff_record(12), make_protocol_message(0x03, {0x01})});
	h.frame({make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
	if (!expect(h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 180, 0x5A))})
	                    .empty(),
			"sync: the final mission chunk builds nothing on its frame"))
		return false;
	if (!expect(h.one_packet(frame_until_sent(h, 16), {0x09}),
			"sync: the 0x09 leaves alone at the pace, no player list seen"))
		return false;
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x11, {})}), {0x0A}),
			"sync: the sync tail releases the 0x0A"))
		return false;
	// The world-stream wait builds on every frame, so the 0x22 leaves on the list's own.
	const std::vector<std::vector<uint8_t>> sent =
			h.frame({make_protocol_message(0x16, player_list({{0, 1}}))});
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	return expect(sent.size() == 1 &&
	                      decode_client_session(sent[0], h.auth.scrk, header, messages) &&
	                      messages.size() == 1 && messages[0].tag == 0x22 &&
	                      messages[0].payload == std::vector<uint8_t>({0x00, 0xF7, 0x1C}),
			"sync: the later list draws exactly one 0x22 for its unbound row");
}

// The 0x11's 0x0A joins the connection's one queue (D-NET-845): the game-start wait
// queues it at its head, behind everything the sync-tail pass queued, so a player list that
// rode the 0x11's packet sends its 0x22 in the same packet, ahead of the 0x0A (the retail pair's
// [0x22, 0x0A] after a [0x16 .. 0x11] packet; ours sent the 0x0A alone first). Under a declared
// 0x11 load the joiner pumps nothing until the load has ended, and the frame after it queues
// the 0x0A at its head, ahead of what its receive folds: a list that arrived during the load
// sends its 0x22 behind the 0x0A, in one packet.
// [orig: Game_StartMission @0x524360 - the sync-tail wait (the call @0x524DAB), the loads
//  @0x524DF2..0x524E24, NapiClient_WaitForGameStart (the call @0x524E29);
//  NapiClient_WaitForGameStart @0x42CC10 - the 0x0A @0x42CC3F ahead of the receive
//  @0x42CC7E / @0x42CC88 and the send @0x42CC92; NapiNPClientMsg_PlayerList - the 0x22
//  @0x42FC35]
bool run_the_sync_tail_0x0a_joins_the_queue() {
	for (const bool declared_load : {false, true}) {
		Harness h;
		if (!expect(h.handshake(), "0x0A queue: handshake")) return false;
		h.frame({settings_record(0), settings_record(1)});
		h.frame({make_protocol_message(0x00, {})});
		std::vector<uint8_t> probe(64, 0);
		probe[8] = 16;
		h.frame({make_protocol_message(0x02, probe)});
		h.frame({holdoff_record(12), make_protocol_message(0x03, {0x01})});
		h.frame({make_protocol_message(0x05, {0x01}),
				make_protocol_message(0x04, slot_assignment(0x02)),
				make_protocol_message(0x7B, full_player_info())});
		(void)frame_until_sent(h, 16);
		(void)h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
		(void)h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 180, 0x5A))});
		if (!expect(h.one_packet(frame_until_sent(h, 16), {0x09}),
				"0x0A queue: the 0x09 leaves at the sync-tail wait's pace"))
			return false;
		if (!declared_load) {
			if (!expect(h.one_packet(h.frame({make_protocol_message(0x16, player_list({{0, 1}})),
						 make_protocol_message(0x11, {})}),
						{0x22, 0x0A}),
					"0x0A queue: the list's 0x22 and the 0x0A leave in one packet, the 0x22 first"))
				return false;
			continue;
		}
		h.client.set_sync_tail_loading(true);
		if (!expect(h.frame({make_protocol_message(0x11, {})}).empty() &&
						h.frame({make_protocol_message(0x16, player_list({{0, 1}}))}).empty() &&
						h.frame().empty(),
				"0x0A queue: the declared load pumps nothing, a list arriving meanwhile waits"))
			return false;
		h.client.set_sync_tail_loading(false);
		if (!expect(h.one_packet(h.frame(), {0x0A, 0x22}),
				"0x0A queue: after the load the 0x0A leads, the waiting list's 0x22 behind it"))
			return false;
	}
	return true;
}

// The final S2C 0x64's 0x09 waits for the mission start's effect system load (D-NET-1054):
// the mission data's spin ends on the final chunk's pass, the mission start parses the effects
// and registers their materials, and only then does the sync-tail wait queue the 0x09 at its
// head. Under a declared mission-data load the chunk's frame and every frame while the load runs
// send nothing, a list arriving meanwhile waits, and the frame after the load's end queues the
// 0x09 at its head, the waiting list's 0x22 behind it. Without the declaration the 0x09 leaves at
// the sync-tail wait's pace (run_the_sync_request_needs_no_player_list).
// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 - the send @0x51EA57, the exit
//  @0x51EA62; Game_StartMission @0x524360 - CEffectSystem_Init @0x524980,
//  CEffectManager_RegisterAllMaterials @0x52499E, the sync-tail wait (the call @0x524DAB);
//  NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42CB20 - the 0x09 queued @0x42CB43]
bool run_the_mission_data_load_holds_the_0x09() {
	Harness h;
	if (!expect(h.handshake(), "0x09 hold: handshake")) return false;
	h.frame({settings_record(0), settings_record(1)});
	h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	h.frame({make_protocol_message(0x02, probe)});
	h.frame({holdoff_record(12), make_protocol_message(0x03, {0x01})});
	h.frame({make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
	h.client.set_mission_data_loading(true);
	if (!expect(h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 180, 0x5A))})
	                    .empty() &&
	                    h.client.mission_data_load_running(),
			"0x09 hold: the final chunk's frame builds nothing and the declared load runs"))
		return false;
	bool quiet = h.frame({make_protocol_message(0x16, player_list({{0, 1}}))}).empty();
	for (int i = 0; i < 8; ++i) quiet = h.frame().empty() && quiet;
	if (!expect(quiet, "0x09 hold: the declared load pumps nothing, a list arriving meanwhile waits"))
		return false;
	h.client.end_mission_data_load();
	if (!expect(!h.client.mission_data_load_running(), "0x09 hold: the end ends the load"))
		return false;
	return expect(h.one_packet(h.frame(), {0x09, 0x22}),
			"0x09 hold: after the load the 0x09 leads, the waiting list's 0x22 behind it");
}

// The join screen reads where the join stands: the dial until ServerAuth
// accepts, the connect until the host's S2C 0x00, the verification until its
// S2C 0x01 reads 1, the host's S2C 0x03 queue record, and the game start at
// S2C 0x05, when the loading screen takes over.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56a320 states 4..7;
//  NapiNPClientMsg_0x001 @0x425360; NapiNPClientMsg_0x003 @0x425390]
bool run_the_join_screen_follows_the_admission() {
	Harness h;
	if (!expect(h.client.join_screen_stage() == inmatch::JoinScreenStage::Joining,
			"screen: the dial reads Joining"))
		return false;
	if (!expect(h.handshake(), "screen: handshake")) return false;
	if (!expect(h.client.join_screen_stage() == inmatch::JoinScreenStage::Connecting,
			"screen: an accepted ServerAuth reads Connecting"))
		return false;
	h.frame({settings_record(0), settings_record(1)});
	h.frame({make_protocol_message(0x00, {})});
	if (!expect(h.client.join_screen_stage() == inmatch::JoinScreenStage::Verifying,
			"screen: the host's S2C 0x00 starts the verification"))
		return false;
	h.frame({make_protocol_message(0x01, {0x01, 0x00, 0x00, 0x00})});
	if (!expect(h.client.join_screen_stage() == inmatch::JoinScreenStage::Queued,
			"screen: an S2C 0x01 of 1 moves to the queue line"))
		return false;
	h.frame({make_protocol_message(0x03, {0x01, 0x03, 0x00, 0x04, 0x00})});
	const inmatch::JoinQueueRecord queue = h.client.join_queue();
	if (!expect(queue.queued && queue.position == 3 && queue.length == 4 &&
					queue.queued_since_ms == h.now_ms,
			"screen: the S2C 0x03 record is kept with its queued-since time"))
		return false;
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	h.frame({make_protocol_message(0x02, probe)});
	h.frame({make_protocol_message(0x05, {0x01})});
	return expect(h.client.join_screen_stage() == inmatch::JoinScreenStage::Starting,
			"screen: the S2C 0x05 game start hands over to the loading screen");
}

// The connection-slot roster is the session's: a map change's reload keeps it (only the
// S2C 0x04's slot-table reallocation renews it, and a map change sends none), so the next
// mission's first 0x16 joins names from the slots the first mission bound and drops only a
// row whose slot is unbound, the one that takes a C2S 0x22 {slot, 0x1CF7} re-request.
// [orig: NapiNPClientMsg_SessionSlotConfig @0x425410 - PlayerSlotTable_Reallocate called
//  @0x4254B4; NapiNPClientMsg_PlayerList @0x42FAE0 - the re-request @0x42fc05..0x42fc3a]
bool run_the_roster_outlives_a_reload() {
	Harness h;
	if (!expect(drive_into_the_world_stream(h, 0), "roster: the world stream runs"))
		return false;
	PlayerReplicationState host_slot;
	host_slot.player_slot = 0;
	host_slot.entity_handle = 0x0001;
	host_slot.player_name = "RosterHost";
	(void)h.frame({make_protocol_message(0x46, encode_player_sync(host_slot))});
	if (!expect(h.client.state().roster[0].bound, "roster: the 0x46 binds slot 0"))
		return false;
	(void)h.frame({make_protocol_message(0x1A, {})});
	std::vector<uint8_t> world_state(23, 0);
	(void)h.frame({make_protocol_message(0x0F, world_state)});
	(void)h.frame();
	if (!expect(h.client.begin_mission_reload(), "roster: the reload begins")) return false;
	if (!expect(h.client.state().roster[0].bound &&
	                    h.client.state().roster[0].name == "RosterHost",
			"roster: slot 0 outlives the reload"))
		return false;
	(void)h.frame({make_protocol_message(0x16, player_list({{0, 1}, {1, 2}}))});
	const auto &board = h.client.state().scoreboard;
	return expect(board.rows.size() == 1 && board.rows[0].slot_id == 0 &&
	                      board.rows[0].name == "RosterHost" &&
	                      board.rows_dropped_unknown_slot == 1,
			"roster: the next mission's list joins slot 0 and drops only the unbound slot 1");
}

// The S2C 0x80 / 0x82 score tracker across a map change's reload: its words are the session's,
// which no mission start clears, and its entries the process's, which only the 0x82 writes, so
// the reload keeps both and the process store holds the entries.
// [orig: Client_ResetGameSessionState @0x42C9F0 - +0..+0x18 @0x42CA12..0x42CA36, called from
//  Game_InitSubsystems @0x4A6D7B and CNapiGameSession_FullDestroy @0x4C9785;
//  CineTracker_RecordTimeEntry @0x422410]
bool run_the_score_tracker_outlives_a_reload() {
	Harness h;
	inmatch::ClientProcessState process;
	h.client.use_process_state(&process);
	if (!expect(drive_into_the_world_stream(h, 0), "tracker: the world stream runs"))
		return false;
	(void)h.frame({make_protocol_message(0x80, {0x07, 0x00, 0x00, 0x00}),
			make_protocol_message(0x82, {0x64, 0x00, 0x00, 0x00})});
	if (!expect(h.client.state().score_tracker.valid && h.client.state().score_tracker.time == 100 &&
	                    process.score_tracker_entries.size() == 1,
			"tracker: the 0x82 records node 7's entry in the process store"))
		return false;
	(void)h.frame({make_protocol_message(0x1A, {})});
	std::vector<uint8_t> world_state(23, 0);
	(void)h.frame({make_protocol_message(0x0F, world_state)});
	(void)h.frame();
	if (!expect(h.client.begin_mission_reload(), "tracker: the reload begins")) return false;
	const replication::ClientScoreTracker &t = h.client.state().score_tracker;
	return expect(t.valid && t.node == 7 && t.start == 100 && t.time == 100 &&
	                      process.score_tracker_entries.size() == 1 &&
	                      h.client.view().score_tracker_entries().size() == 1,
			"tracker: the reload keeps the words and the process's entries");
}

// The joiner net readout (inmatch/joiner_net_trace.h, an instrument with no behaviour): one
// record per client net frame run to its end. Each holds the send countdown ahead of the
// frame's step, whether the block opened and the 0x0C left in it, the dictated period, the
// replica's applied 0x0A count, the frame's clocks and its loop. The 62-frame fold's record
// carries its inputs and the terms it pushed, the 0x4C's its level. The frames send what
// run_an_early_release_stays_in_the_final_wait pins.
// [orig: Client_ProcessNetworkFrame @0x42C180 - the gate @0x42C3DD, the 0x0C
//  @0x42C46F..0x42C4A3, the 0x4C @0x42c23e..0x42c279; Game_ProcessMainFrame @0x5263F0 - the
//  fold's countdown @0x526537..0x526555; CNetQuality_UpdateMetrics @0x4C52C0 - the holds
//  @0x4c5606..0x4c5630]
bool run_the_net_readout_records_each_frame() {
	Harness h;
	h.uplink = true;
	h.client.set_observed_frame_rate(0); // the game mode's init value: the frame-rate term 255
	if (!drive_to_an_early_in_match_release(h, 6)) return false;
	std::vector<uint8_t> world_state(23, 0);
	(void)h.frame({make_protocol_message(0x0F, world_state)});
	const inmatch::JoinerNetTrace &trace = h.client.net_trace();
	int folds = 0;
	int uplinks = 0;
	for (int i = 0; i < 70; ++i) {
		const uint32_t seq = trace.next_seq();
		const uint32_t countdown = h.client.send_holdoff_countdown();
		const bool pack_frame = h.client.send_block_opens_this_frame();
		const uint32_t frames_before = h.client.state().frames_applied;
		// A short 0x0A (anchor and flags only) still counts as an applied frame.
		const std::vector<std::vector<uint8_t>> sent = i == 10
				? h.frame({make_protocol_message(0x0A, std::vector<uint8_t>(14, 0))})
				: h.frame();
		const std::vector<inmatch::JoinerNetTraceRecord> rows = trace.since(seq);
		if (!expect(rows.size() == 1 && rows[0].seq == seq && trace.next_seq() == seq + 1,
				"readout: one record per client net frame"))
			return false;
		const inmatch::JoinerNetTraceRecord &r = rows[0];
		if (!expect(r.countdown_before == countdown && r.block_open == pack_frame &&
						r.holdoff_ticks == 6,
				"readout: the countdown ahead of the step, the block it opened, the period"))
			return false;
		if (!expect(r.uplink_sent == carries(sent_tags(h, sent), 0x0C),
				"readout: uplink_sent is the frame's C2S 0x0C"))
			return false;
		if (!expect(r.frames_applied == h.client.state().frames_applied &&
						(i == 10) == (r.frames_applied == frames_before + 1),
				"readout: the applied 0x0A count, stepping in the frame whose receive took one"))
			return false;
		if (!expect(r.pump_loop == inmatch::JoinerConnection::SendPumpLoop::NetworkFrame &&
						r.wall_ms == static_cast<uint32_t>(h.now_ms) &&
						r.frame_tick == h.client.view().frame_tick(),
				"readout: the game loop's frame, its clock and its frame tick"))
			return false;
		if (r.uplink_sent) ++uplinks;
		if (r.report_queued &&
				!expect(r.report_level == h.client.net_quality_level(),
						"readout: the 0x4C carries the level the fold left"))
			return false;
		if (!r.quality_folded) continue;
		++folds;
		const bool held = r.fold_deploy_hold || r.fold_spawn_gate || r.fold_preround;
		if (!expect(r.fold_fps == 0 && r.fold_bandwidth == (held ? 0 : 255) &&
						r.window_quality == h.client.client_quality_window().quality &&
						r.host_metric == h.client.state().net.host_metric &&
						r.level == h.client.net_quality_level(),
				"readout: the fold's frame rate, the term it pushed, the window and the level"))
			return false;
	}
	return expect(folds == 2, "readout: the first game-loop frame folds, then every 62") &&
			expect(uplinks > 0, "readout: the game loop's boundaries carry the 0x0C");
}

// D-NET-1111 (LX-s25's 03tr-dm row: our joiner's first C2S 0x4C carried 02 where retail's
// carries 01, so our host's S2C 0x46 relayed an extra 0x0400 field): the S2C 0x0F's pass is the
// final wait's, which steps no 0x4C timer. A fresh process's first report is then the game loop's
// 311th frame, the frame of its sixth quality fold, which runs first and has pushed the first
// fold's sample out of the five-sample window. The witnessed inputs: the frame rate reads 0 from
// the mode's init until the FR counter's first two-second window closes (the folds of frames 1
// and 63 score 255), then any rate of 16 or more (the floor 1); no pong (ping term 1), no loss, no
// S2C 0x79 (host scalar 0). The window then holds 255, 1, 1, 1, 1: 51, level 1, the report's
// byte. A pass that stepped the timer put the report on frame 310, ahead of the sixth fold:
// 255, 255, 1, 1, 1, 102, level 2.
// [orig: Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5 (no
//  Client_ProcessNetworkFrame); Game_ProcessMainFrame @0x5263F0 - the countdown
//  @0x526537..0x526555, CNetQuality_UpdateMetrics @0x52658B, Client_ProcessNetworkFrame
//  @0x526692; Client_ProcessNetworkFrame @0x42C180 - the timer @0x42C22D..0x42C23E, the level
//  read @0x42C256, the queue @0x42C279; CNetQuality_UpdateMetrics @0x4C52C0 - the receive window
//  @0x4C5636..0x4C584E, the level @0x4C586A..0x4C58B0; Game_MainLoop @0x52B630 - the mode's init
//  @0x52B70D, then g_StatsAvgFps = 0 @0x52B727]
bool run_the_first_report_after_a_join_follows_the_sixth_fold() {
	Harness h;
	h.client.set_observed_frame_rate(0);
	if (!expect(drive_to_the_final_wait(h, 0), "first report: the final wait runs")) return false;
	std::vector<uint8_t> world_state(23, 0);
	if (!expect(!carries(sent_tags(h, h.frame({make_protocol_message(0x0F, world_state)})), 0x4C),
			"first report: the 0x0F's pass reports nothing"))
		return false;
	int report_frame = 0;
	std::vector<uint8_t> report;
	for (int frame = 1; frame <= 320 && report_frame == 0; ++frame) {
		// The FR counter's first window has closed by the third fold.
		if (frame == 125) h.client.set_observed_frame_rate(62);
		for (const std::vector<uint8_t> &dg : h.frame()) {
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!decode_client_session(dg, h.auth.scrk, header, messages)) continue;
			for (const ProtocolMessage &m : messages) {
				if (m.tag != 0x4C || report_frame != 0) continue;
				report_frame = frame;
				report = m.payload;
			}
		}
	}
	if (!expect(report_frame == 311, "first report: the game loop's 311th frame reports"))
		return false;
	return expect(report == std::vector<uint8_t>({0x01}) && h.client.net_quality_level() == 1,
			"first report: the sixth fold's window (255, 1, 1, 1, 1) reports level 1");
}

int main() {
	bool ok = true;
	ok = run_admission_follows_the_retail_loading_loops() && ok;
	ok = run_timed_loops_pace_an_undictated_connection() && ok;
	ok = run_an_early_release_stays_in_the_final_wait() && ok;
	ok = run_an_esc_leaves_the_final_wait_for_the_game_loop() && ok;
	ok = run_a_self_team_assign_waits_for_the_mission_starts_player_init() && ok;
	ok = run_a_reset_before_the_mission_start_is_cleared() && ok;
	ok = run_a_bodyless_joiner_pings() && ok;
	ok = run_the_mission_start_bears_l_and_the_first_frame_ships_its_uplink() && ok;
	ok = run_the_final_wait_end_ends_the_mode_init() && ok;
	ok = run_a_0x0f_after_the_mission_start_call_ends_the_mode_init() && ok;
	ok = run_the_0x0f_pass_lands_its_handler_writes() && ok;
	ok = run_the_quality_fold_starts_with_the_game_loop() && ok;
	ok = run_the_link_indicators_step_in_the_main_frame_alone() && ok;
	ok = run_the_net_readout_records_each_frame() && ok;
	ok = run_the_first_report_after_a_join_follows_the_sixth_fold() && ok;
	ok = run_the_sync_request_needs_no_player_list() && ok;
	ok = run_the_sync_tail_0x0a_joins_the_queue() && ok;
	ok = run_the_mission_data_load_holds_the_0x09() && ok;
	ok = run_the_mission_start_submissions_follow_the_countdown() && ok;
	ok = run_the_mission_start_first_leg_runs_mid_load() && ok;
	ok = run_the_mission_start_runs_in_the_0x1a_pass() && ok;
	ok = run_the_mission_start_legs_keep_retails_order() && ok;
	ok = run_the_mission_start_push_bears_l() && ok;
	ok = run_the_second_0x2f_reads_its_words_past_the_init() && ok;
	ok = run_a_novaworld_load_runs_its_handlers_ahead_of_the_second_0x2f() && ok;
	ok = run_the_update_past_the_receive_runs_no_handler() && ok;
	ok = run_a_loading_step_answers_a_ping_on_novaworld() && ok;
	ok = run_a_loading_step_runs_the_novaworld_sessions_update() && ok;
	ok = run_the_joiner_mission_start_keeps_the_folded_round_clock() && ok;
	ok = run_the_net_frame_counter_steps_in_the_verification_wait() && ok;
	ok = run_the_join_screen_follows_the_admission() && ok;
	ok = run_the_roster_outlives_a_reload() && ok;
	ok = run_the_score_tracker_outlives_a_reload() && ok;
	if (ok) std::printf("client_loading_pump_test: OK\n");
	return ok ? 0 : 1;
}
