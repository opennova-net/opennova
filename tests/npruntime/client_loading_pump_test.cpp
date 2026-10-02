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
//   NapiClient_WaitForDisconnect     the send pump only after MORE than 100 ms;
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
//  0x56a5dd, state 6 -> sub_424740 @0x424740); SaveFile_SendAndWaitForServerAck
//  @0x5204b0; CNapiGameSession_InitRandomSeedOrRequest @0x51e8f0;
//  NapiClient_WaitForDisconnect @0x42cb20; NapiClient_WaitForGameStart @0x42cc10;
//  Game_StartMission's final loop @0x52628d..0x5262df; the main-loop gate
//  Client_ProcessNetworkFrame @0x42c3dd]

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

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
	uint32_t dictation_tick = 0; // the frame that received the holdoff dictation
	inmatch::ClientRuntime client{"LoadingPump", [this] { return now_ms; }};
	std::string server_scrk = "SERVER-LOADING-PUMP-SCRK";
	ClientAuth auth;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	// One client frame on the 16 ms clock, with `incoming` delivered first.
	std::vector<std::vector<uint8_t>> frame(
			const std::vector<ProtocolMessage> *incoming = nullptr) {
		now_ms += 16;
		if (incoming != nullptr) {
			const std::vector<uint8_t> dg =
					frame_server_session(server_tx, server_scrk, auth.ck, *incoming);
			client.receive(dg.data(), dg.size());
		}
		return client.Client_ProcessNetworkFrame(tick++);
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

	// The final mission chunk ends the spin; NapiClient_WaitForDisconnect then
	// queues the 0x09, finds the countdown running at its entry pump, and sends
	// only after MORE than 100 ms.
	if (!expect(h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 100, 80, 0x5A))})
	                    .empty(),
			"the final mission chunk enters WaitForDisconnect without building"))
		return false;
	if (!expect(h.frame({make_protocol_message(0x16, player_list({{0, 1}, {1, 2}}))}).empty(),
			"the player list's 0x09+0x22 waits inside WaitForDisconnect's pace"))
		return false;
	for (int held = 2; held <= 6; ++held) {
		if (!expect(h.frame().empty(), "WaitForDisconnect holds its send pump for 100 ms"))
			return false;
	}
	// (The reducer's per-row 0x22 re-requests for the list's unbound slots ride
	// the same boundary in a later packet.)
	sent = h.frame();
	if (!expect(!sent.empty() && h.tags_of(sent[0], tags0) &&
	                    tags0 == std::vector<uint8_t>({0x09, 0x22}),
			"the first send pump past 100 ms carries the 0x09+0x22"))
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

	// S2C 0x1A ends the world stream; the loadout pair and the 0x0B ride the
	// final 0x0F wait's spin.
	if (!expect(h.one_packet(h.frame({make_protocol_message(0x1A, {})}), {0x2F, 0x2F, 0x0B}),
			"the loadout pair and status leave on the 0x1A's frame"))
		return false;

	// S2C 0x0F ends Game_StartMission. From here Client_ProcessNetworkFrame's
	// gate paces the send block again: the completion burst waits for the
	// countdown, re-armed to 12 by the last spin build.
	std::vector<uint8_t> world_state(23, 0);
	sent = h.frame({make_protocol_message(0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF}),
			make_protocol_message(0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF}),
			make_protocol_message(0x19, {0x40, 0x30, 0x20, 0x10}),
			make_protocol_message(0x0F, world_state)});
	if (!expect(sent.empty(), "the in-match gate holds the world-state completion burst"))
		return false;
	int held_frames = 1;
	for (; held_frames < 12; ++held_frames) {
		sent = h.frame();
		if (!sent.empty()) break;
	}
	std::vector<uint8_t> tags;
	if (!expect(held_frames == 11 && !sent.empty() && h.tags_of(sent[0], tags) &&
	                    !tags.empty() && tags[0] == 0x28,
			"the completion burst leaves at the main loop's twelfth frame"))
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
std::vector<std::vector<uint8_t>> frame_until_sent(Harness &h, int limit) {
	for (int i = 0; i < limit; ++i) {
		std::vector<std::vector<uint8_t>> sent = h.frame();
		if (!sent.empty()) return sent;
	}
	return {};
}

// Drive the admission under a dictated `period` to the final wait for S2C 0x0F, then let the
// host release the player early: its self record and a first 0x5A grant (the in-match release
// that precedes the 0x0F on an OpenNova host).
bool drive_to_an_early_in_match_release(Harness &h, uint8_t period) {
	if (!h.handshake()) return false;
	(void)h.frame({settings_record(0), settings_record(1)});
	(void)h.frame({make_protocol_message(0x00, {})});
	std::vector<uint8_t> probe(64, 0);
	probe[8] = 16;
	(void)h.frame({make_protocol_message(0x02, probe)});
	(void)h.frame({holdoff_record(period), make_protocol_message(0x03, {0x01})});
	h.dictation_tick = h.tick - 1;
	(void)h.frame({make_protocol_message(0x05, {0x01}),
			make_protocol_message(0x04, slot_assignment(0x02)),
			make_protocol_message(0x7B, full_player_info())});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x60, transfer_chunk(1, 171, 0, 171, 0xA5))});
	(void)h.frame({make_protocol_message(0x64, transfer_chunk(1, 180, 0, 180, 0x5A))});
	(void)h.frame({make_protocol_message(0x16, player_list({{0, 1}, {1, 2}}))});
	(void)frame_until_sent(h, 16);
	(void)h.frame({make_protocol_message(0x11, {})});
	(void)h.frame({make_protocol_message(0x1A, {})});
	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord self;
	self.slot_id = 0x00C6;
	self.has_body = true;
	self.item_type_id = 0x14B9;
	self.owner_connection_id = 4; // the 0x82's MI
	self.minimap_flags = 0x100;
	self.entity_name = "LoadingPump";
	self.team = 2;
	self.net_id = 7;
	batch.records.push_back(self);
	(void)h.frame({make_protocol_message(0x0C, encode_organic_spawn_batch(batch)),
			make_protocol_message(0x5A, {0x08, 0x03, 0x0A, 0xFF, 0x00, 0xFF})});
	return expect(h.client.in_match() &&
	                      std::string(h.client.admission_stage_name()) ==
	                              std::string("awaiting the loadout grants"),
			"early release: the joiner is in the match inside the final wait for the 0x0F");
}

// A joiner the host released into the match early (its self record and a first 0x5A inside
// the final wait for the 0x0F) already runs our in-match frame: its uplink and input pack ride
// that frame's send gate. Its sends therefore keep the per-frame countdown gate, so the frames
// whose pack precondition holds are exactly the frames that build, once per dictated period,
// and under no holdoff every frame builds and a one-shot queued before a frame leaves in it.
// [orig: Client_ProcessNetworkFrame @0x42c3dd -> Player_PackInputStateToEntity @0x42c3e9 ->
//  PumpClientProtocolSend @0x42c4bc]
bool run_an_in_match_joiner_keeps_the_frame_gate() {
	{
		Harness h;
		if (!drive_to_an_early_in_match_release(h, 6)) return false;
		int opened_frames = 0;
		for (int i = 0; i < 18; ++i) {
			const bool pack_frame = h.client.send_block_opens_this_frame();
			const uint32_t flushes = h.client.send_flush_counter();
			(void)h.frame();
			const bool opened = h.client.send_flush_counter() != flushes;
			if (!expect(opened == pack_frame,
					"in match: the send block opens exactly on the input pack's frames"))
				return false;
			// The countdown kept its frame cycle through the loading loops: the in-match
			// boundaries fall on the dictation frame's cycle.
			if (!expect(opened == ((h.tick - 1 - h.dictation_tick) % 6 == 0),
					"in match: the boundaries continue the countdown's cycle from the dictation"))
				return false;
			if (opened) ++opened_frames;
		}
		if (!expect(opened_frames == 3, "in match: one send boundary per dictated period of 6"))
			return false;
	}
	Harness h;
	if (!drive_to_an_early_in_match_release(h, 0)) return false;
	for (int i = 0; i < 4; ++i) {
		if (!expect(h.client.send_block_opens_this_frame(),
				"in match, no holdoff: every frame opens"))
			return false;
		if (!expect(h.client.queue_stance_change(0xA9), "in match: the stance key queues a 0x1D"))
			return false;
		std::vector<std::vector<uint8_t>> sent = h.frame();
		bool carried = false;
		std::vector<uint8_t> tags;
		for (const std::vector<uint8_t> &dg : sent)
			if (h.tags_of(dg, tags))
				for (uint8_t tag : tags) carried = carried || tag == 0x1D;
		if (!expect(carried, "in match, no holdoff: a one-shot queued before a frame leaves in it"))
			return false;
	}
	return true;
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

int main() {
	bool ok = true;
	ok = run_admission_follows_the_retail_loading_loops() && ok;
	ok = run_timed_loops_pace_an_undictated_connection() && ok;
	ok = run_an_in_match_joiner_keeps_the_frame_gate() && ok;
	ok = run_a_reset_before_the_mission_start_is_cleared() && ok;
	ok = run_the_join_screen_follows_the_admission() && ok;
	if (ok) std::printf("client_loading_pump_test: OK\n");
	return ok ? 0 : 1;
}
