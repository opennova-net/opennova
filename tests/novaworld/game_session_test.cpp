#include <novaworld/game_session.h>
#include <novaworld/ingame_decode.h> // decode_frame_update (validate the field-driven 0x0A)
#include <novaworld/retail_loading_blobs.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

int find_tag(const std::vector<opennova::ProtocolMessage> &messages, uint8_t tag, int start = 0) {
	for (size_t i = static_cast<size_t>(start); i < messages.size(); ++i) {
		if (messages[i].tag == tag) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int count_tag(const std::vector<opennova::ProtocolMessage> &messages, uint8_t tag) {
	int count = 0;
	for (const opennova::ProtocolMessage &message : messages) {
		if (message.tag == tag) {
			++count;
		}
	}
	return count;
}

bool payload_contains(const std::vector<uint8_t> &payload, const char *needle) {
	const std::string text(needle);
	return std::search(payload.begin(), payload.end(), text.begin(), text.end()) != payload.end();
}

std::string read_cstr(const std::vector<uint8_t> &payload, size_t offset = 0) {
	std::string out;
	for (size_t i = offset; i < payload.size() && payload[i] != 0; ++i) {
		out.push_back(static_cast<char>(payload[i]));
	}
	return out;
}

void push_u8(std::vector<uint8_t> &buf, uint8_t v) {
	buf.push_back(v);
}

void push_u16(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void push_u32(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

std::vector<opennova::ProtocolMessage> drain_queued(opennova::GameSession &session,
                                                    opennova::GameSessionState &state) {
	std::vector<opennova::ProtocolMessage> out;
	for (int i = 0; i < 64 && !state.queued_replies.empty(); ++i) {
		auto tick = session.tick(state, 16, static_cast<uint32_t>(1000 + i));
		out.insert(out.end(),
				std::make_move_iterator(tick.replies.begin()),
				std::make_move_iterator(tick.replies.end()));
	}
	return out;
}

opennova::GameEntitySnapshot replicated_entity(uint8_t pool, uint16_t slot,
                                               uint16_t type_id,
                                               opennova::EntityClass cls,
                                               int32_t x, int32_t y, int32_t z) {
	opennova::GameEntitySnapshot e;
	e.pool = pool;
	e.slot = slot;
	e.type_id = type_id;
	e.team = 1;
	e.x = x;
	e.y = y;
	e.z = z;
	e.entity_class = cls;
	return e;
}

bool check_post_handshake_burst() {
	opennova::GameSessionConfig config;
	config.player_name = "FooPlayer";
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	std::vector<opennova::ProtocolMessage> incoming = {
			opennova::make_protocol_message(0x02, std::vector<uint8_t>(256, 0)),
	};
	const auto result = session.handle_messages(state, incoming, 0x12345678u);
	if (!expect(result.replies.size() == 8, "0x02 produces eight replies")) return false;
	if (!expect(result.replies[0].tag == 0x00 && result.replies[0].flags.raw == 0xA0,
			"first reply is custom tag=0x00 ack")) return false;
	if (!expect(result.replies[1].tag == 0x00 && result.replies[1].payload[0] == 1,
			"second custom ack has idx=1")) return false;
	const int tag7a = find_tag(result.replies, 0x7A);
	if (!expect(tag7a >= 0, "player handle tag=0x7A present")) return false;
	if (!expect(read_cstr(result.replies[static_cast<size_t>(tag7a)].payload) == "FooPlayer",
			"player handle tag=0x7A carries configured player name")) return false;
	const int tag7b = find_tag(result.replies, 0x7B);
	if (!expect(tag7b >= 0, "session summary tag=0x7B present")) return false;
	if (!expect(read_cstr(result.replies[static_cast<size_t>(tag7b)].payload) == "FooPlayer",
			"session summary tag=0x7B starts with configured player name")) return false;
	return true;
}

bool check_mission_burst_order_and_quarantine() {
	opennova::GameSession session;
	opennova::GameSessionState state;
	std::vector<opennova::ProtocolMessage> incoming = {
			opennova::make_protocol_message(0x37, {}),
	};
	const auto result = session.handle_messages(state, incoming, 100);
	if (!expect(!state.spawned, "0x37 does not mark session spawned")) return false;
	if (!expect(state.phase == opennova::GameSessionPhase::MissionReady,
			"0x37 advances phase to mission_ready")) return false;
	if (!expect(state.ida_initial_state == 2, "0x37 enters IDA initial state 2")) return false;
	if (!expect(state.queued_replies.size() == 14, "0x37 queues IDA mission bootstrap")) return false;
	if (!expect(find_tag(result.replies, 0x75) >= 0, "mission metadata includes retail tag=0x75")) return false;
	if (!expect(find_tag(result.replies, 0x64) >= 0, "mission metadata present")) return false;
	if (!expect(find_tag(result.replies, 0x0B) < 0, "mission state blob is queued after metadata")) return false;
	if (!expect(find_tag(result.replies, 0x46) < 0, "roster is not in immediate mission bootstrap")) return false;
	if (!expect(find_tag(result.replies, 0x16) < 0, "player list is not in immediate mission bootstrap")) return false;
	if (!expect(find_tag(result.replies, 0x0D) < 0, "spawn entity not active on mission bootstrap")) return false;
	if (!expect(find_tag(result.replies, 0x0F) < 0, "game-start not active on mission bootstrap")) return false;
	if (!expect(find_tag(result.replies, 0x40) < 0, "tag=0x40 remains inactive")) return false;

	auto queued = drain_queued(session, state);
	if (!expect(find_tag(queued, 0x2C) == 0, "queued mission bootstrap starts with tag=0x2C")) return false;
	const int tag08 = find_tag(queued, 0x08);
	if (!expect(tag08 == 1, "queued mission bootstrap then emits tag=0x08")) return false;
	if (!expect(queued[static_cast<size_t>(tag08)].payload.size() == 51,
			"tag=0x08 payload length matches retail")) return false;
	if (!expect(queued[static_cast<size_t>(tag08)].payload[48] == 0x04,
			"tag=0x08 retail fixture byte[48] matches capture")) return false;
	if (!expect(count_tag(queued, 0x2A) == 6, "queued mission bootstrap emits six retail tag=0x2A entries")) return false;
	if (!expect(find_tag(queued, 0x1C) >= 0, "queued mission bootstrap emits tag=0x1C")) return false;
	if (!expect(find_tag(queued, 0x0B) >= 0, "queued mission bootstrap emits tag=0x0B")) return false;
	if (!expect(find_tag(queued, 0x66) >= 0, "queued mission bootstrap emits tag=0x66")) return false;
	if (!expect(find_tag(queued, 0x76) >= 0, "queued mission bootstrap emits tag=0x76")) return false;
	if (!expect(find_tag(queued, 0x11) >= 0, "queued mission bootstrap emits tag=0x11")) return false;
	if (!expect(find_tag(queued, 0x19) >= 0, "queued mission bootstrap emits tag=0x19")) return false;
	if (!expect(count_tag(queued, 0x11) == 1, "queued mission bootstrap emits exactly one tag=0x11")) return false;
	if (!expect(find_tag(queued, 0x1C) < find_tag(queued, 0x0B) &&
			find_tag(queued, 0x0B) < find_tag(queued, 0x66) &&
			find_tag(queued, 0x66) < find_tag(queued, 0x76) &&
			find_tag(queued, 0x76) < find_tag(queued, 0x11) &&
			find_tag(queued, 0x11) < find_tag(queued, 0x19),
			"queued mission bootstrap orders tag=0x11 before tag=0x19")) return false;
	if (!expect(find_tag(queued, 0x0F) < 0, "queued mission bootstrap does not start the game")) return false;
	if (!expect(state.ida_initial_state == 4, "drained mission bootstrap advances to IDA state 4")) return false;
	if (!expect(state.phase == opennova::GameSessionPhase::WorldStreaming,
			"drained mission bootstrap enters world_streaming")) return false;
	if (!expect(state.initial_sync_complete, "IDA state 4 marks initial sync complete")) return false;
	if (!expect(state.world_streaming_armed, "IDA state 4 arms server-driven world streaming")) return false;
	if (!expect(state.world_streaming_ack_count == 0,
			"server-driven state 4 does not count as a client ack")) return false;

	opennova::GameSessionState marker_state;
	session.handle_messages(marker_state, {opennova::make_protocol_message(0x37, {})}, 100);
	auto tick = session.tick(marker_state, 16, 101);
	for (int i = 0; i < 16 && find_tag(tick.replies, 0x1C) < 0; ++i) {
		tick = session.tick(marker_state, 16, static_cast<uint32_t>(102 + i));
	}
	if (!expect(find_tag(tick.replies, 0x1C) >= 0, "retail final mission bundle includes tag=0x1C")) return false;
	if (!expect(find_tag(tick.replies, 0x0B) >= 0, "retail final mission bundle includes tag=0x0B")) return false;
	if (!expect(find_tag(tick.replies, 0x66) >= 0, "retail final mission bundle includes tag=0x66")) return false;
	if (!expect(find_tag(tick.replies, 0x76) >= 0, "retail final mission bundle includes tag=0x76")) return false;
	if (!expect(find_tag(tick.replies, 0x11) >= 0, "retail final mission bundle includes tag=0x11")) return false;
	if (!expect(find_tag(tick.replies, 0x76) < find_tag(tick.replies, 0x11),
			"retail final mission bundle puts tag=0x11 after tag=0x76")) return false;
	if (!expect(find_tag(tick.replies, 0x19) < 0,
			"retail final mission bundle leaves tag=0x19 for a later tick")) return false;

	const auto marker = session.handle_messages(
			marker_state, {opennova::make_protocol_message(0x09, {})}, 120);
	// Per IDA: client's NapiClient_WaitForDisconnect @ 0x42CB20 sends
	// tag=0x09 then loops on `dword_A82358 != 0`; the only writer is the
	// 1-instruction stub NapiNPClientMsg_0x011 @ 0x4226E0. So tag=0x09
	// MUST be answered with tag=0x11 to advance the loading bar past the
	// pre-mission-load wait. Per `notes/tag_cross_capture_diff.md` retail
	// emits tag=0x11 ONLY in the bundle [0x1C, 0x0B, 0x66, 0x76, 0x11] -
	// never standalone - so tag=0x09 must not append a duplicate once the
	// capture7 bootstrap ack has already gone out.
	if (!expect(find_tag(marker.replies, 0x11) < 0,
			"tag=0x09 no longer emits standalone tag=0x11 in immediate reply")) return false;
	auto post_marker_drain = drain_queued(session, marker_state);
	if (!expect(find_tag(post_marker_drain, 0x19) >= 0,
			"queue still had tag=0x19 from bootstrap")) return false;
	if (!expect(find_tag(post_marker_drain, 0x11) < 0,
			"tag=0x09 does not queue a duplicate tag=0x11 after the bootstrap bundle")) return false;
	return true;
}

bool check_spawn_request_acceptance_sequence() {
	opennova::GameSession session;
	opennova::GameSessionState state;
	session.handle_messages(state, {opennova::make_protocol_message(0x37, {})}, 100);
	std::vector<opennova::ProtocolMessage> incoming = {
			opennova::make_protocol_message(0x0E, {0xFE, 0xFF}),
	};
	const auto result = session.handle_messages(state, incoming, 200);
	if (!expect(!state.spawned, "0x0E FE FF does not spawn before IDA readiness")) return false;
	if (!expect(!state.spawn_acceptance_sent, "0x0E FE FF does not record acceptance before readiness")) return false;
	if (!expect(state.phase == opennova::GameSessionPhase::SpawnRequested,
			"0x0E FE FF records a deferred spawn request")) return false;
	if (!expect(find_tag(result.replies, 0x0D) < 0, "deferred spawn request emits no local-player 0x0D")) return false;
	if (!expect(find_tag(result.replies, 0x0F) < 0, "deferred spawn request emits no game-start")) return false;
	if (!expect(find_tag(result.replies, 0x5A) < 0, "deferred spawn request emits no loadout")) return false;
	return true;
}

bool check_observed_loading_flow_enters_world_streaming() {
	opennova::GameSessionConfig config;
	config.replicated_entities = {
		replicated_entity(2, 3, 0x14B9, opennova::EntityClass::Player, 100, 200, 300),
		replicated_entity(2, 4, 0x14BF, opennova::EntityClass::Infantry, 400, 500, 600),
	};
	config.spawn_points = {
		{1, 49, 1359, 1, 1000, 2000, 3000},
	};
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	session.handle_messages(state, {opennova::make_protocol_message(0x37, {})}, 100);
	drain_queued(session, state);
	if (!expect(state.phase == opennova::GameSessionPhase::WorldStreaming,
			"mission bootstrap drain enters retail state 4 world streaming")) return false;
	if (!expect(state.world_streaming_armed, "state 4 streaming is server-driven")) return false;
	if (!expect(state.initial_sync_complete, "state 4 marks initial sync complete")) return false;
	const auto transition = session.handle_messages(
			state, {opennova::make_protocol_message(0x09, {})}, 120);
	// Same IDA-witnessed contract as above: tag=0x09 unblocks through
	// tag=0x11. capture7 bundles tag=0x11 with tag=0x0B, so drain_queued
	// already sent the bootstrap ack and tag=0x09 must not append a duplicate.
	if (!expect(find_tag(transition.replies, 0x11) < 0,
			"0x09 transition marker no longer emits standalone tag=0x11")) return false;
	const auto disconnect_ack_tick = session.tick(state, 16, 130);
	if (!expect(find_tag(disconnect_ack_tick.replies, 0x11) < 0,
			"0x09 transition marker does not queue duplicate tag=0x11")) return false;

	const auto first_stream = session.tick(state, 300, 150);
	if (!expect(find_tag(first_stream.replies, 0x10) >= 0,
			"state 4 tick emits tag=0x10 without waiting for local-player ack")) return false;
	if (!expect(find_tag(first_stream.replies, 0x0A) < 0,
			"pre-game state 4 does not emit early world-reference 0x0A")) return false;
	if (!expect(find_tag(first_stream.replies, 0x57) < 0,
			"pre-game state 4 does not emit early RTT 0x57")) return false;
	if (!expect(state.state4_loading_gate_queued,
			"first state 4 stream queues the retail loading gate")) return false;

	// GameSession owns the retail state-4 timing/loading gate only. Mission/player
	// entity snapshots are live world data streamed by the host bridge; replaying
	// captured world blobs here overwrites the joiner's DCB-bearing 0x0C record.
	std::vector<opennova::ProtocolMessage> wait_phase;
	for (int i = 0; i < 64 && !state.queued_replies.empty(); ++i) {
		auto tick = session.tick(state, 300, static_cast<uint32_t>(400 + i));
		wait_phase.insert(wait_phase.end(),
				std::make_move_iterator(tick.replies.begin()),
				std::make_move_iterator(tick.replies.end()));
	}
	if (!expect(find_tag(wait_phase, 0x0D) < 0,
			"WAIT does not emit canned state-4 0x0D world data")) return false;
	if (!expect(find_tag(wait_phase, 0x0C) < 0,
			"WAIT does not emit canned state-4 0x0C player/DCB data")) return false;
	if (!expect(find_tag(wait_phase, 0x20) < 0,
			"WAIT does not emit canned state-4 0x20 spawn-marker data")) return false;
	if (!expect(find_tag(wait_phase, 0x45) >= 0,
			"WAIT emits the retail state-4 0x45 loading gate")) return false;
	if (!expect(find_tag(wait_phase, 0x7E) >= 0,
			"WAIT emits the retail state-4 0x7E loading gate")) return false;
	if (!expect(find_tag(wait_phase, 0x1A) >= 0,
			"WAIT emits the retail state-4 0x1A loading tick")) return false;
	if (!expect(find_tag(wait_phase, 0x0F) < 0,
			"WAIT does not emit game-start before loadout/status readiness")) return false;
	if (!expect(state.state4_loading_gate_complete,
			"state 4 loading gate completes after queued replies drain")) return false;
	if (!expect(!state.spawned, "tag=0x10 stream does not imply spawned")) return false;
	if (!expect(state.entity_batch_count > 0, "entity batch counter advances")) return false;
	return true;
}

bool check_observed_spawn_readiness_accepts_once() {
	opennova::GameSessionConfig config;
	config.replicated_entities = {
		replicated_entity(2, 3, 0x14B9, opennova::EntityClass::Player, 100, 200, 300),
	};
	config.spawn_points = {
		{1, 49, 1359, 1, 1000, 2000, 3000},
	};
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	session.handle_messages(state, {opennova::make_protocol_message(0x37, {})}, 100);
	drain_queued(session, state);
	session.handle_messages(state, {opennova::make_protocol_message(0x09, {})}, 120);
	session.tick(state, 300, 200);
	drain_queued(session, state);
	if (!expect(state.state4_loading_gate_complete,
			"readiness waits for the state-4 loading gate to drain")) return false;

	const auto loadout = session.handle_messages(state, {
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
	}, 450);
	if (!expect(count_tag(loadout.replies, 0x5A) == 2,
			"retail loadout/status packet emits exactly two loadout syncs")) return false;
	if (!expect(count_tag(loadout.replies, 0x0D) == 0,
			"loadout/status packet does not emit unsupported local-player 0x0D")) return false;
	const int tag5a = find_tag(loadout.replies, 0x5A);
	const int tag42 = find_tag(loadout.replies, 0x42);
	const int tag0a = find_tag(loadout.replies, 0x0A);
	const int tag0f = find_tag(loadout.replies, 0x0F);
	const int tag4d = find_tag(loadout.replies, 0x4D);
	const int tag61 = find_tag(loadout.replies, 0x61);
	const int tag3e = find_tag(loadout.replies, 0x3E);
	if (!expect(tag5a >= 0 && tag42 >= 0 && tag0a >= 0 && tag0f >= 0 &&
			tag4d >= 0 && tag61 >= 0 && tag3e >= 0,
			"loadout/status packet emits reference-backed game-start tags")) return false;
	if (!expect(tag5a < tag42 && tag42 < tag0a && tag0a < tag0f &&
			tag0f < tag4d && tag4d < tag61 && tag61 < tag3e,
			"loadout/status packet follows retail game-start order")) return false;
	if (!expect(count_tag(loadout.replies, 0x0A) == 1,
			"game-start emits only the live-scoped world-reference 0x0A")) return false;
	{
		const auto &frame = loadout.replies[static_cast<size_t>(tag0a)].payload;
		auto class_of = [](uint16_t) {
			return opennova::EntityClass::Unknown;
		};
		opennova::FrameUpdate fu;
		if (!expect(opennova::decode_frame_update(frame.data(), frame.size(), class_of, fu) &&
		            fu.complete && fu.consumed == frame.size(),
		            "game-start world-reference 0x0A decodes cleanly")) return false;
		if (!expect(fu.records.empty(),
		            "game-start world-reference 0x0A does not replay captured compact records")) return false;
	}
	// tag=0x1D MUST NOT be in the bundle — it's the round-end signal,
	// not round-start. See `notes/spawn_gate_24C1928.md` 2026-04-26
	// addendum: emitting tag=0x1D unblocks the WaitForGameStart gate
	// AS A SIDE EFFECT of writing dword_C8D820 = 0x7FFFFFFF (= round
	// ended at +∞), which the client UI shows as "game has ended" and
	// then disconnects. Confirmed in 2026-04-26 live test.
	if (!expect(find_tag(loadout.replies, 0x1D) < 0,
			"loadout/status packet does NOT emit tag=0x1D (round-end side effect)")) return false;
	// tag=0x25 RESET_AND_START MUST NOT be in the bundle. Its handler
	// NapiNPClientMsg_GameReset @0x422800 SETS the spawn gate
	// g_spawn_success_gate (dword_24C1928)=1 [@0x422849], which blocks the
	// joiner's auto-deploy C2S 0x0C (Client_ProcessNetworkFrame @0x42c46d) and
	// arms the reason=4 auto-kick. The joiner already cleared the gate via
	// Game_StartMission @0x524a1f at Game-Loop entry, so a 0x25 here re-arms it
	// with nothing left to clear it. The working retail initial join
	// (host_and_join_lan.pcapng) sends NO 0x25 before deploy. net-re §5.38c /
	// D-NET-99.
	if (!expect(find_tag(loadout.replies, 0x25) < 0,
			"loadout/status packet does NOT emit tag=0x25 (re-arms spawn gate, blocks deploy)")) return false;
	if (!expect(find_tag(loadout.replies, 0x1E) < 0,
			"loadout/status packet does not emit speculative tag=0x1E")) return false;
	if (!expect(state.spawned, "loadout/status packet marks session spawned")) return false;
	if (!expect(state.spawn_acceptance_sent, "loadout/status packet records spawn acceptance")) return false;
	if (!expect(state.phase == opennova::GameSessionPhase::Spawned,
			"loadout/status packet advances phase to spawned")) return false;
	if (!expect(state.game_start_bundle_sent, "loadout/status packet records game-start bundle")) return false;
	if (!expect(state.spawn_query_count == 0,
			"retail game-start does not require pre-game 0x0F queries")) return false;

	std::vector<opennova::ProtocolMessage> readiness = {
			opennova::make_protocol_message(0x0F, {0x01, 0x00}),
			opennova::make_protocol_message(0x0F, {0x0E, 0x10}),
	};
	const auto result = session.handle_messages(state, readiness, 500);
	if (!expect(count_tag(result.replies, 0x0D) == 0,
			"readiness packet does not emit unsupported local-player 0x0D")) return false;
	if (!expect(state.spawn_query_count == 2, "readiness packet counts spawn queries")) return false;
	if (!expect(count_tag(result.replies, 0x0F) == 0,
			"post-spawn readiness does not emit another game-start")) return false;

	const auto duplicate = session.handle_messages(state, {
			opennova::make_protocol_message(0x0F, {0x01, 0x00}),
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
	}, 600);
	if (!expect(count_tag(duplicate.replies, 0x5A) == 1,
			"duplicate readiness can still refresh loadout")) return false;
	if (!expect(count_tag(duplicate.replies, 0x0D) == 0,
			"duplicate readiness does not emit unsupported local-player 0x0D")) return false;
	if (!expect(count_tag(duplicate.replies, 0x0F) == 0,
			"duplicate readiness does not emit another game-start")) return false;
	if (!expect(count_tag(duplicate.replies, 0x1E) == 0,
			"duplicate readiness does not emit another post-spawn event")) return false;
	return true;
}

bool check_game_start_spawn_names_use_configured_mission() {
	opennova::GameSessionConfig config;
	config.mission_name = "Custom Island Test";
	config.mission_file = "CUSTOM_A1.BMS";
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	session.handle_messages(state, {opennova::make_protocol_message(0x37, {})}, 100);
	drain_queued(session, state);
	session.handle_messages(state, {opennova::make_protocol_message(0x09, {})}, 120);
	session.tick(state, 300, 200);
	drain_queued(session, state);

	const auto loadout = session.handle_messages(state, {
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
	}, 450);
	const int tag0f = find_tag(loadout.replies, 0x0F);
	if (!expect(tag0f >= 0, "configured mission game-start emits tag=0x0F")) return false;
	const auto &payload = loadout.replies[static_cast<size_t>(tag0f)].payload;
	if (!expect(payload_contains(payload, "Custom Island Test"),
			"tag=0x0F spawn-name list uses the configured mission label")) return false;
	if (!expect(!payload_contains(payload, "North Sea Village"),
			"tag=0x0F no longer leaks the default ASH spawn-name list")) return false;
	return true;
}

bool check_mission_bootstrap_header_uses_configured_mission() {
	opennova::GameSessionConfig config;
	config.mission_name = "Custom Island Test";
	config.mission_file = "CUSTOM_A1.BMS";
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	session.handle_messages(state, {opennova::make_protocol_message(0x37, {})}, 100);
	const auto queued = drain_queued(session, state);
	const int tag0b = find_tag(queued, 0x0B);
	if (!expect(tag0b >= 0, "configured mission bootstrap emits tag=0x0B")) return false;
	const auto &payload = queued[static_cast<size_t>(tag0b)].payload;
	if (!expect(payload.size() == 616, "tag=0x0B remains a 616-byte BMS header")) return false;
	if (!expect(payload_contains(payload, "Custom Island Test"),
			"tag=0x0B BMS header uses the configured mission label")) return false;
	if (!expect(!payload_contains(payload, "AS - Dormant Volcano Isle"),
			"tag=0x0B BMS header does not leak the default ASH mission label")) return false;
	return true;
}

bool check_frame749_mission_refresh_uses_configured_mission() {
	opennova::GameSessionConfig config;
	config.server_name = "Custom Host";
	config.mission_name = "Custom Island Test";
	config.mission_file = "CUSTOM_A1.BMS";
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	session.handle_messages(state, {opennova::make_protocol_message(0x37, {})}, 100);
	drain_queued(session, state);
	session.handle_messages(state, {opennova::make_protocol_message(0x09, {})}, 120);
	session.tick(state, 300, 200);
	drain_queued(session, state);

	const auto loadout = session.handle_messages(state, {
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
	}, 450);
	const int tag58 = find_tag(loadout.replies, 0x58);
	if (!expect(tag58 >= 0, "configured mission game-start emits tag=0x58")) return false;
	const auto &payload = loadout.replies[static_cast<size_t>(tag58)].payload;
	if (!expect(payload_contains(payload, "Custom Host"),
			"tag=0x58 mission refresh uses the configured server name")) return false;
	if (!expect(payload_contains(payload, "Custom Island Test"),
			"tag=0x58 mission refresh uses the configured mission label")) return false;
	if (!expect(!payload_contains(payload, "AS - Dormant Volcano Isle"),
			"tag=0x58 mission refresh does not leak the default ASH mission label")) return false;
	return true;
}

bool check_tag_0f_query_silently_consumed() {
	// Per `notes/spawn_flow_capture7.md`: capture7's retail co-host session
	// shows ZERO C2S `tag=0x0F len=2` queries and ZERO S2C tag=0x18 emissions.
	// The IDA path at NapiNPServerMsg_0x00F @ 0x514180 line 0x514241 (the
	// `NapiNPServer_SendFiltered(&ctx, 0x18u, ...)` call) is real but never
	// triggered in normal multiplayer play. We previously emitted tag=0x18
	// here speculatively; cap7 ground truth proves that's wrong, so we
	// silent-consume and just bump the diagnostic counter.
	opennova::GameSessionConfig config;
	config.spawn_points = {
		{1, 49, 1359, 0, 1000, 2000, 3000},
		{1, 51, 1359, 1, 4000, 5000, 6000},
	};
	opennova::GameSession session(config);
	opennova::GameSessionState state;

	const auto match = session.handle_messages(state, {
			opennova::make_protocol_message(0x0F, {0x31, 0x10}),
	}, 100);
	if (!expect(match.replies.empty(),
			"tag=0x0F query produces no replies (retail also ignores)")) return false;
	if (!expect(find_tag(match.replies, 0x18) < 0,
			"tag=0x0F query does not emit speculative tag=0x18")) return false;
	if (!expect(state.spawn_query_count == 1,
			"tag=0x0F query bumps the spawn_query_count diagnostic")) return false;
	if (!expect(match.label ==
			"in-game 0x0F spawn-point query consumed (retail also ignores)",
			"tag=0x0F query records the silent-consume label")) return false;

	const auto miss = session.handle_messages(state, {
			opennova::make_protocol_message(0x0F, {0x0E, 0x10}),
	}, 200);
	if (!expect(miss.replies.empty(),
			"non-matching tag=0x0F query also stays silent")) return false;
	if (!expect(state.spawn_query_count == 2,
			"second tag=0x0F query advances spawn_query_count")) return false;
	return true;
}

bool check_tag_29_emits_tag_51_spawn_confirm() {
	// Per IDA NapiNPServerMsg_0x029 @ 0x514F10, server emits tag=0x51 in
	// response to client's tag=0x29 spawn-slot request. The client sends
	// tag=0x29 with payload 00 00 from NapiNPClientMsg_0x00F line 0x42e61e
	// after receiving our tag=0x0F WORLD-STATE-LOAD. Without the tag=0x51
	// reply the spawn-select menu stays open and the player can't enter
	// the world (verified 2026-04-25 live test).
	opennova::GameSessionConfig config;
	opennova::GameSession session(config);
	opennova::GameSessionState state;

	const auto first = session.handle_messages(state, {
			opennova::make_protocol_message(0x29, {0x00, 0x00}),
	}, 100);
	const int tag51 = find_tag(first.replies, 0x51);
	if (!expect(tag51 >= 0,
			"first tag=0x29 emits tag=0x51 player-spawn confirm")) return false;
	const auto &payload = first.replies[static_cast<size_t>(tag51)].payload;
	if (!expect(payload.size() == 8,
			"tag=0x51 payload is 8 bytes (team, slot, team_byte, weapon, camera)")) return false;
	const auto u16_at = [&](size_t off) -> uint16_t {
		return static_cast<uint16_t>(payload[off]) |
				(static_cast<uint16_t>(payload[off + 1]) << 8);
	};
	if (!expect(u16_at(0) == 1, "tag=0x51 team_u16 matches PlayerReplicationState default (1)")) return false;
	if (!expect(u16_at(2) == 0x0000u, "tag=0x51 entity_slot encodes pool=0 player_slot=0")) return false;
	if (!expect(payload[4] == 1, "tag=0x51 team_byte matches team")) return false;
	if (!expect(u16_at(5) == 0u, "tag=0x51 weapon_index = 0")) return false;
	if (!expect(payload[7] == 0, "tag=0x51 camera_byte = 0 (first person)")) return false;
	if (!expect(state.player_spawn_confirmed,
			"tag=0x29 marks player_spawn_confirmed in state")) return false;

	// HandlePlayerSpawn echoes back tag=0x29 with payload (team+1) after
	// receiving tag=0x51. We must NOT re-emit tag=0x51 or we loop forever.
	const auto echo = session.handle_messages(state, {
			opennova::make_protocol_message(0x29, {0x02, 0x00}),
	}, 200);
	if (!expect(find_tag(echo.replies, 0x51) < 0,
			"echo tag=0x29 (post-spawn) does not re-emit tag=0x51")) return false;
	return true;
}

bool check_tick_emits_entity_batch_and_world_reference() {
	opennova::GameSessionConfig config;
	config.replicated_entities = {
		replicated_entity(2, 3, 0x14B9, opennova::EntityClass::Player, 100, 200, 300),
		replicated_entity(2, 4, 0x14BF, opennova::EntityClass::Infantry, 400, 500, 600),
	};
	opennova::GameSession session(config);
	opennova::GameSessionState state;
	state.spawned = false;
	state.phase = opennova::GameSessionPhase::WorldStreaming;
	state.world_streaming_armed = true;
	const auto result = session.tick(state, 1000, 0xCAFEBABEu);
	if (!expect(find_tag(result.replies, 0x10) >= 0, "tick emits tag=0x10")) return false;
	const int tag10 = find_tag(result.replies, 0x10);
	if (!expect(result.replies[static_cast<size_t>(tag10)].payload.size() > 4,
			"tag=0x10 carries replicated entities when configured")) return false;
	if (!expect(find_tag(result.replies, 0x0A) < 0, "pre-game tick does not emit tag=0x0A")) return false;
	if (!expect(find_tag(result.replies, 0x57) < 0, "pre-game tick does not emit tag=0x57")) return false;
	if (!expect(find_tag(result.replies, 0x40) < 0, "tick does not emit tag=0x40")) return false;
	if (!expect(!state.spawned, "world streaming tick does not mark spawned")) return false;

	state.spawned = true;
	state.phase = opennova::GameSessionPhase::Spawned;
	state.queued_replies.clear();
	const auto spawned_tick = session.tick(state, 1000, 0xCAFEBABFu);
	if (!expect(find_tag(spawned_tick.replies, 0x10) >= 0, "spawned tick emits tag=0x10")) return false;
	if (!expect(find_tag(spawned_tick.replies, 0x0A) >= 0, "spawned tick emits tag=0x0A world reference")) return false;
	{
		// D-NET-50: the field-driven 0x0A the tick now emits must decode cleanly via
		// the §5.9 walk (no leftover) — the end-to-end wiring check.
		const int s0a = find_tag(spawned_tick.replies, 0x0A);
		const auto &frame = spawned_tick.replies[static_cast<size_t>(s0a)].payload;
		auto class_of = [](uint16_t t) {
			if (t == 0x14B9) return opennova::EntityClass::Player;
			if (t == 0x14BF) return opennova::EntityClass::Infantry;
			return opennova::EntityClass::Unknown;
		};
		opennova::FrameUpdate fu;
		if (!expect(opennova::decode_frame_update(frame.data(), frame.size(), class_of, fu) &&
		            fu.complete && fu.consumed == frame.size(),
		            "field-driven 0x0A decodes cleanly")) return false;
		if (!expect(fu.records.size() == 2,
		            "field-driven 0x0A carries configured compact records")) return false;
		if (!expect(fu.records[0].cls == opennova::EntityClass::Player &&
		            fu.records[0].handle == 0x2003,
		            "first configured compact record is the player-class entity")) return false;
		if (!expect(fu.records[1].cls == opennova::EntityClass::Infantry &&
		            fu.records[1].handle == 0x2004,
		            "second configured compact record is the infantry-class entity")) return false;
	}
	if (!expect(find_tag(spawned_tick.replies, 0x57) >= 0, "spawned tick emits tag=0x57")) return false;
	return true;
}

bool check_client_position_is_state_only() {
	opennova::GameSession session;
	opennova::GameSessionState state;
	std::vector<uint8_t> payload;
	push_u16(payload, 0x0003);     // entity handle
	push_u16(payload, 0x14B9);     // player item type
	push_u8(payload, 0x0A);        // extended uplink
	push_u16(payload, 0x2222);     // vehicle handle
	push_u32(payload, 0x11223344); // x
	push_u32(payload, 0x55667788); // y
	push_u32(payload, 0x99AABBCC); // z
	push_u16(payload, 0x1234);     // heading
	push_u16(payload, 0x5678);     // pitch
	for (int i = 0; i < 25; ++i) push_u8(payload, 0);
	std::vector<opennova::ProtocolMessage> incoming = {
			opennova::make_protocol_message(0x0C, payload),
	};
	const auto result = session.handle_messages(state, incoming, 0);
	if (!expect(result.replies.empty(), "client input has no immediate reply")) return false;
	if (!expect(state.client_pos_valid, "client position marked valid")) return false;
	if (!expect(state.client_pos_x == 0x11223344u, "client X cached")) return false;
	if (!expect(state.client_pos_y == 0x55667788u, "client Y cached")) return false;
	if (!expect(state.client_pos_z == 0x99AABBCCu, "client Z cached")) return false;
	if (!expect(state.client_entity_handle == 0x0003u, "client entity handle cached")) return false;
	if (!expect(state.client_item_type_id == 0x14B9u, "client item type cached")) return false;
	if (!expect(state.client_vehicle_handle == 0x2222u, "client vehicle handle cached")) return false;
	if (!expect(state.client_heading == 0x1234, "client heading cached")) return false;
	if (!expect(state.client_pitch == 0x5678, "client pitch cached")) return false;

	opennova::GameSessionState compact_state;
	payload[4] = 0x0B; // compact sub-op; do not treat as extended position.
	session.handle_messages(compact_state, {opennova::make_protocol_message(0x0C, payload)}, 0);
	if (!expect(!compact_state.client_pos_valid,
			"compact 0x0C body is not misparsed as extended uplink")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_post_handshake_burst() && ok;
	ok = check_mission_burst_order_and_quarantine() && ok;
	ok = check_spawn_request_acceptance_sequence() && ok;
	ok = check_observed_loading_flow_enters_world_streaming() && ok;
	ok = check_observed_spawn_readiness_accepts_once() && ok;
	ok = check_game_start_spawn_names_use_configured_mission() && ok;
	ok = check_mission_bootstrap_header_uses_configured_mission() && ok;
	ok = check_frame749_mission_refresh_uses_configured_mission() && ok;
	ok = check_tag_0f_query_silently_consumed() && ok;
	ok = check_tag_29_emits_tag_51_spawn_confirm() && ok;
	ok = check_tick_emits_entity_batch_and_world_reference() && ok;
	ok = check_client_position_is_state_only() && ok;
	return ok ? 0 : 1;
}
