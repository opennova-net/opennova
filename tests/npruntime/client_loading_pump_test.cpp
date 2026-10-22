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

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/joiner_role.h> // the release frame through the role (D-NET-526)
#include <runtime/inmatch/remote_admin.h> // the admin's GET over the joiner's clock
#include <runtime/mission/mission_kernel.h>
#include <base/io/le.h>
#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h> // AdminSession
#include <net/title/title_identity.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

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
	inmatch::ClientRuntime client{"LoadingPump", [this] { return now_ms; }};
	std::string server_scrk = "SERVER-LOADING-PUMP-SCRK";
	ClientAuth auth;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	bool uplink = false; // the frame builds a C2S 0x0C body (the game loop's uplink)

	Harness() { client.set_title(&kJo); }

	// One client frame on the 16 ms clock, with `incoming` delivered first.
	std::vector<std::vector<uint8_t>> frame(
			const std::vector<ProtocolMessage> *incoming = nullptr) {
		now_ms += 16;
		if (incoming != nullptr) {
			const std::vector<uint8_t> dg =
					frame_server_session(server_tx, server_scrk, auth.ck, *incoming);
			client.receive(dg.data(), dg.size());
		}
		if (uplink) return client.Client_ProcessNetworkFrame(PlayerExtendedUplink{}, tick++);
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
		// state; the frame after it, past that start, sends the 0x42 (D-NET-NEW-L2-25).
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
	// wait (D-NET-NEW-L2-6).
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
// the wait: its completion burst leads, the first 0x2C and 0x0C ride the first game-loop
// boundary behind it, and from there the frame's countdown gate paces the block, one boundary
// per dictated period, the frames that build exactly the input pack's; under no holdoff
// every frame opens and a one-shot queued before a frame leaves in it.
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
		std::vector<uint8_t> world_state(23, 0);
		std::vector<std::vector<uint8_t>> sent =
				h.frame({make_protocol_message(0x0F, world_state)});
		for (int i = 0; sent.empty() && i < 8; ++i) sent = h.frame();
		if (!expect(h.packets(sent, {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23, 0x2C, 0x0C}}),
				"the 0x0F ends the wait: its burst leads, the first 0x2C and 0x0C behind it"))
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


// D-NET-NEW-L2-26 (V-L2-s9 FOUND 1): an ESC in the mission start's final wait for the S2C
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
bool drive_into_the_world_stream(Harness &h, uint8_t period) {
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
//  latch @0x4319DB, the submit @0x431A9E; Game_StartMission @0x524360 - the first 0x2F
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
// 0x0C, whose build reads the body, stays unsent.
// [orig: Client_ProcessNetworkFrame @0x42c3ee..0x42c410 (the 0x2C's gate) and
//  @0x42c44f..0x42c46d -> Player_BuildTag0CInputBody @0x42A550; NapiNPClientMsg_0x00F
//  @0x42e391 (the hold's clear)]
bool run_a_bodyless_joiner_pings() {
	Harness h;
	h.uplink = true;
	if (!expect(drive_into_the_world_stream(h, 0), "bodyless: the world stream runs"))
		return false;
	(void)h.frame({make_protocol_message(0x1A, {})});
	std::vector<uint8_t> world_state(23, 0);
	std::vector<std::vector<uint8_t>> sent = h.frame({make_protocol_message(0x0F, world_state)});
	if (!expect(!h.client.has_self_handle() &&
	                    h.packets(sent, {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23, 0x2C}}),
			"bodyless: the 0x0F's burst, then the frame's 0x2C and no 0x0C"))
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
	inmatch::JoinerRole role;
	KeepingSocket socket;
	inmatch::TickInput input;
	std::string server_scrk = "SERVER-LOADING-PUMP-SCRK";
	ClientAuth auth;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	inmatch::ClientRuntime &client;

	RoleHarness() : client(install()) {}

	inmatch::ClientRuntime &install() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.load_systems();
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_title(&kJo);
		role.create_runtime("LoadingPump", inmatch::JoinRole::Player, "", "");
		// The role's runtime on the virtual clock, configured as create_runtime does.
		role.runtime = std::make_unique<inmatch::ClientRuntime>(
				"LoadingPump", [this] { return now_ms; });
		role.runtime->set_title(&kJo);
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

// D-NET-526: the joiner's own body is born where retail's handlers write it, in the
// receive. The release (a 0x5A) and the S2C 0x0F reach a joiner whose self record is
// bound in one packet: the role's after-receive leg spawns L, folds its record and lands
// the 0x0F's pose ahead of the send block, so that frame's one datagram carries the 0x0F's
// completion burst, then the 0x2C ping and the first 0x0C, built from the 0x0F's position
// (RR's 01tr / cp04 / ash joiners: [28 29 2D 32 22 23 2C 2C 0C], the extra 0x2C a 0x57's
// pong). Before, L was born after the net frame and the first 0x0C left a boundary later.
// [orig: Client_ProcessNetworkFrame @0x42C180 - the receive @0x42C228 ahead of the send
//  block @0x42C3DD, the 0x0C's build from g_LocalPlayerEntity @0x42C46F..0x42C482;
//  NapiNPClientMsg_0x00F @0x42E200 - the pose stores @0x42E3B9..0x42E3CE;
//  Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC]
bool run_the_release_frame_bears_l_and_ships_its_uplink() {
	RoleHarness h;
	// The self record rides the world stream, ahead of the mission start.
	if (!expect(drive_to_the_final_wait(h, 0, {self_spawn_record()}),
			"release frame: the final wait runs"))
		return false;
	if (!expect(h.client.has_self_handle() && !h.client.in_match() && !h.role.local_spawned(),
			"release frame: the self record is bound, no release yet, no L"))
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
	const std::vector<std::vector<uint8_t>> sent =
			h.frame({first_grant(), make_protocol_message(0x0F, world_state)});
	if (!expect(h.client.in_match() && h.role.local_spawned(),
			"release frame: L is born in the frame whose receive released it"))
		return false;
	if (!expect(h.packets(sent, {{0x28, 0x29, 0x2D, 0x32, 0x22, 0x23, 0x2C, 0x0C}}),
			"release frame: the burst, the 0x2C and the first 0x0C in one datagram"))
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
	h.client.set_mission_start_loading(true);
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

// The joiner's round clock at its mission start: retail never seeds it. Game_StartMission's
// round-clock seed (g_RoundTimeRemaining = g_RoundTimeLimitMinutes x 3720 when in session,
// outside the Co-op waypoint family, with a nonzero limit) sits on the authority / single-player
// arm; the joiner's arm runs its two waits and jumps past the seed and the pump that follows it,
// so its clock is whatever the common path's -1 or the waits' 0x0A folds left, until the next
// phase-1 0x0A (the host's seed is Match::configure's). A timed team game whose joiner's own cfg
// limit is 7 keeps a folded 100 s past the 0x1A, and the remote admin's GET reads it as
// 6200 / 60 / 62 = 1 over the joiner's own limit (D-NET-NEW-L8-23, NOT-A-DIVERGENCE).
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

// The 0x11's 0x0A joins the connection's one queue (D-NET-NEW-L2-19): the game-start wait
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

int main() {
	bool ok = true;
	ok = run_admission_follows_the_retail_loading_loops() && ok;
	ok = run_timed_loops_pace_an_undictated_connection() && ok;
	ok = run_an_early_release_stays_in_the_final_wait() && ok;
	ok = run_an_esc_leaves_the_final_wait_for_the_game_loop() && ok;
	ok = run_a_self_team_assign_waits_for_the_mission_starts_player_init() && ok;
	ok = run_a_reset_before_the_mission_start_is_cleared() && ok;
	ok = run_a_bodyless_joiner_pings() && ok;
	ok = run_the_release_frame_bears_l_and_ships_its_uplink() && ok;
	ok = run_the_quality_fold_starts_with_the_game_loop() && ok;
	ok = run_the_net_readout_records_each_frame() && ok;
	ok = run_the_sync_request_needs_no_player_list() && ok;
	ok = run_the_sync_tail_0x0a_joins_the_queue() && ok;
	ok = run_the_mission_start_submissions_follow_the_countdown() && ok;
	ok = run_the_joiner_mission_start_keeps_the_folded_round_clock() && ok;
	ok = run_the_net_frame_counter_steps_in_the_verification_wait() && ok;
	ok = run_the_join_screen_follows_the_admission() && ok;
	ok = run_the_roster_outlives_a_reload() && ok;
	if (ok) std::printf("client_loading_pump_test: OK\n");
	return ok ? 0 : 1;
}
