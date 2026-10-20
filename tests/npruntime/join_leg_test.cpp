// npruntime_join_leg: the host's join leg against retail's (net parity lane L1). A remote joiner
// is added by the spawn pump, then driven through its C2S 0x09 / 0x0A / 0x0B legs with the
// producer (tick_connections) between them, the way a stock client's Game_StartMission sends
// them (net-re §5.70.7). Each case pins one witnessed rule:
//   - D-NET-500: the player-sync track (its S2C 0x2C first) waits for the joiner's C2S 0x09,
//     which enters slot state 2 [orig: NapiNPServerMsg_0x009 @0x513200 - game state 9
//     @0x51322D, slot state 2 @0x513249; Server_SendInitialGameStateToPlayer @0x51BBA0 - the
//     switch @0x51BBB4..0x51BBBB, the track @0x51BEFF, the 0x2C call @0x51BF5D];
//   - D-NET-504: no S2C 0x16 rides the C2S 0x37 reply or a spawn: retail's one 0x16 serializer
//     call is the scoreboard clock's [orig: Server_BuildAndBroadcastScoreboard @0x50D960
//     (@0x50DDE2), called from Server_TickUpdate @0x51D90D].
//   - D-NET-501: the join's bundle carries the slot's first 0x0A between the 0x42 and the 0x0F,
//     and the tail's cease-fire line and round-over legs [orig: Server_OnPlayerJoin @0x51A680 -
//     0x42 @0x51A81F, Server_SendEntityStateToPlayer @0x51A82D, 0x0F @0x51A869, 0x4D @0x51A97A,
//     0x61 @0x51A982, 0x14 @0x51A9F3, the round-over legs @0x51A9F8..0x51AA5C, 0x3E @0x51AAEE];
//   - D-NET-NEW-L1-7: the producer waits while the round is over [orig:
//     Server_SendInitialGameStateToPlayer @0x51BBA3..0x51BBA9, @0x51BEFF..0x51BF05];
//   - D-NET-401: the player-slot table is the slot limit alone [orig: Server_InitNewRoundState
//     @0x51C948..0x51C97B];
//   - D-NET-351: a remote add takes the row free the longest [orig: PlayerSlot_PickFreeSlot
//     @0x4FD7B0; Server_TickUpdate @0x51E457..0x51E467];
//   - D-NET-715: the spawn pump unlinks a joiner before its add, so a failed add leaves it
//     bodyless and never walked again [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8FF6];
//   - D-NET-NEW-L1-8: the join arms the round-event watermark at the frame clock less one
//     [orig: CNetPlayer_SetGameState @0x4C422D; Server_OnPlayerJoin @0x51A74C;
//     Server_BuildRoundEventListForPlayer @0x4FFEE8, @0x5001A8..0x5001AE].

#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/replication/entity_wire_bridge.h> // class_for_type_id

#include "host_test_setup.h"

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h> // to_fixed
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// One remote joiner on a Serve Only host, admitted (its C2S 0x00 / 0x01 / 0x02 done) and
// waiting for the spawn pump. Heap-held: a World and a server context overflow the 1 MB
// test-main stack under MSVC.
struct JoinFixture {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	opennova::bms::File mission;
	ns::UdpSessionTransport joiner_transport{ns::UdpSessionTransport::Role::Host};
	ns::UdpSessionTransport existing_transport{ns::UdpSessionTransport::Role::Host};
	uint32_t tick = 1;

	// `with_existing`: an already-joined client ahead of the joiner on the connection list.
	explicit JoinFixture(bool with_existing) {
		world->registry.configure_pool(0, 16);
		world->registry.configure_pool(1, 8);
		world->registry.configure_pool(2, 8);
		world->registry.configure_pool(3, 16);
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {10.0f, 20.0f, 1.0f};
		start.item_type_index = 1;
		start.has_item_def = true;
		world->registry.spawn(3, start);
		opennova::mission::make_default(mission);

		inmatch::GameConfig config;
		config.max_players = 4;
		inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Socketless, /*host_key=*/0, nullptr, config);
		ctx->world = world.get();
		ctx->mission = &mission;
		inmatch::Server_InitNewRoundState(*ctx);

		if (with_existing) {
			inmatch::NapiNPConnection &existing = ctx->np_protocol.connection_list.emplace_back();
			existing.type = inmatch::NapiNPConnection::kTypeServerSide;
			existing.peer = opennova::PeerAddr{0x0100007Fu, 30001};
			existing.connection_id = inmatch::kFirstJoinerDcb;
			existing.phase = inmatch::ConnectionPhase::InMatch;
			existing.slot_state = inmatch::PlayerSlotState::Joined;
			existing.burst.spawned = true;
			existing.reply.player_slot = 1;
			existing.link.transport = &existing_transport;
			existing.link.mode = ns::TransportMode::Client;
		}
		inmatch::NapiNPConnection &joiner = ctx->np_protocol.connection_list.emplace_back();
		joiner.type = inmatch::NapiNPConnection::kTypeServerSide;
		joiner.peer = opennova::PeerAddr{0x0100007Fu, 30002};
		joiner.connection_id = inmatch::kFirstJoinerDcb + 1;
		joiner.player_name = "Joiner";
		joiner.phase = inmatch::ConnectionPhase::Joined;
		joiner.admission_stage = inmatch::GameAdmissionStage::Complete;
		joiner.self_id_seen = true;
		joiner.link.transport = &joiner_transport;
		joiner.link.mode = ns::TransportMode::Client;
	}

	inmatch::NapiNPConnection &joiner() { return ctx->np_protocol.connection_list.back(); }

	// One producer pass; returns the S2C tags the joiner's transport took.
	std::vector<uint8_t> pump() {
		(void)inmatch::tick_connections(*ctx, /*elapsed_ms=*/16, tick++);
		return drain(joiner_transport);
	}

	static std::vector<uint8_t> drain(ns::UdpSessionTransport &transport) {
		std::vector<uint8_t> tags;
		ns::Datagram d;
		while (transport.pop_outbound(d)) tags.push_back(d.tag);
		return tags;
	}

	// One C2S message through the host dispatch; returns the reply tags.
	std::vector<uint8_t> send(uint8_t tag, std::vector<uint8_t> body = {}) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = ctx.get();
		inputs.mission_metadata_blob = &ctx->mission_metadata_blob;
		inputs.mission_metadata_transfer_id = ctx->mission_metadata_transfer_id;
		std::vector<opennova::ProtocolMessage> msgs;
		msgs.push_back(opennova::make_protocol_message(tag, std::move(body)));
		std::vector<uint8_t> tags;
		for (const opennova::ProtocolMessage &m : inmatch::dispatch_session_replies(
				ctx->config, joiner(), msgs, tick, ctx->np_protocol.connection_list,
				world.get(), inputs))
			tags.push_back(m.tag);
		return tags;
	}

	// Drives the added joiner through its C2S 0x09 and 0x0A legs to slot state 5, draining
	// everything the producer sent on the way.
	bool drive_to_await_join() {
		(void)send(opennova::c2s::CHECKSUM_RESPONSE);
		for (int i = 0; i < 64 && joiner().slot_state != inmatch::PlayerSlotState::AwaitWorldRequest; ++i)
			(void)pump();
		(void)send(opennova::c2s::SPAWN_MENU_REQUEST);
		for (int i = 0; i < 256 && joiner().slot_state != inmatch::PlayerSlotState::AwaitJoin; ++i)
			(void)pump();
		(void)drain(joiner_transport);
		return joiner().slot_state == inmatch::PlayerSlotState::AwaitJoin;
	}

	// The joiner's C2S 0x0B; returns the datagrams its transport took.
	std::vector<ns::Datagram> join() {
		(void)send(opennova::c2s::MISSION_FILE_STATUS, {0x00});
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (joiner_transport.pop_outbound(d)) out.push_back(d);
		return out;
	}
};

bool contains(const std::vector<uint8_t> &tags, uint8_t tag) {
	for (uint8_t t : tags)
		if (t == tag) return true;
	return false;
}

// D-NET-500: nothing of the player-sync track before the C2S 0x09; the 0x2C opens it after.
bool run_track_waits_for_player_sync_request() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	inmatch::NapiNPConnection &j = f.joiner();
	if (!expect(j.slot_state == inmatch::PlayerSlotState::Added && j.burst.game_state == 8,
	            "the add leaves slot state 1 and game state 8 [@0x4C911A, @0x4C9123]"))
		return false;
	for (int i = 0; i < 40; ++i) {
		const std::vector<uint8_t> tags = f.pump();
		if (!expect(!contains(tags, opennova::s2c::MISSION_MAP_NAMES) &&
		                    !contains(tags, opennova::s2c::SESSION_CONFIG) &&
		                    !contains(tags, opennova::s2c::CHAT_HISTORY),
		            "no player-sync record before the C2S 0x09")) return false;
	}
	if (!expect(f.send(opennova::c2s::CHECKSUM_RESPONSE).empty(),
	            "the C2S 0x09 answers nothing")) return false;
	if (!expect(j.slot_state == inmatch::PlayerSlotState::PlayerSync && j.burst.game_state == 9,
	            "the C2S 0x09 enters slot state 2 and game state 9")) return false;
	const std::vector<uint8_t> first = f.pump();
	if (!expect(!first.empty() && first.front() == opennova::s2c::MISSION_MAP_NAMES,
	            "the track's first record after the 0x09 is the S2C 0x2C")) return false;
	// A repeated 0x09 in state 2 leaves the track where it is.
	const uint16_t subphase = j.burst.player_sync_subphase;
	(void)f.send(opennova::c2s::CHECKSUM_RESPONSE);
	if (!expect(j.slot_state == inmatch::PlayerSlotState::PlayerSync &&
	                    j.burst.player_sync_subphase == subphase,
	            "a 0x09 in slot state 2 changes nothing")) return false;
	return true;
}

// D-NET-504: the 0x37 reply is the 0x64 alone, and a whole join adds no 0x16 for the joiner
// or for a client already in the match (it learns the joiner at the clock's next boundary).
bool run_no_player_list_off_the_scoreboard_clock() {
	JoinFixture f(/*with_existing=*/true);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	inmatch::NapiNPConnection &j = f.joiner();
	j.reply.spawn_metadata_pushed = true; // the spawn pump's 0x05 / 0x04 / 0x7B went out
	const std::vector<uint8_t> mission_reply = f.send(opennova::c2s::MISSION_CHUNK_REQUEST);
	if (!expect(mission_reply == std::vector<uint8_t>{opennova::s2c::MISSION_DATA_CHUNK},
	            "the C2S 0x37 reply is the S2C 0x64 alone")) return false;

	std::vector<uint8_t> joiner_tags;
	auto keep = [&](const std::vector<uint8_t> &tags) {
		joiner_tags.insert(joiner_tags.end(), tags.begin(), tags.end());
	};
	keep(f.send(opennova::c2s::CHECKSUM_RESPONSE));
	for (int i = 0; i < 64 && j.slot_state != inmatch::PlayerSlotState::AwaitWorldRequest; ++i)
		keep(f.pump());
	if (!expect(j.slot_state == inmatch::PlayerSlotState::AwaitWorldRequest,
	            "the track's tail parks for the C2S 0x0A")) return false;
	keep(f.send(opennova::c2s::SPAWN_MENU_REQUEST));
	for (int i = 0; i < 256 && j.slot_state != inmatch::PlayerSlotState::AwaitJoin; ++i)
		keep(f.pump());
	keep(f.send(opennova::c2s::MISSION_FILE_STATUS, {0x00}));
	keep(JoinFixture::drain(f.joiner_transport));
	if (!expect(j.burst.spawned && j.slot_state == inmatch::PlayerSlotState::Joined,
	            "the joiner's 0x0B joins it (slot state 6)")) return false;
	for (int i = 0; i < 8; ++i) keep(f.pump());
	if (!expect(!contains(joiner_tags, opennova::s2c::PLAYER_LIST),
	            "no S2C 0x16 reaches the joiner off the scoreboard clock")) return false;
	const std::vector<uint8_t> existing_tags = JoinFixture::drain(f.existing_transport);
	if (!expect(!contains(existing_tags, opennova::s2c::PLAYER_LIST),
	            "no S2C 0x16 reaches a joined client at the joiner's spawn")) return false;
	if (!expect(contains(existing_tags, opennova::s2c::PLAYER_SYNC),
	            "the joined client still takes the joiner's 0x46")) return false;
	return true;
}

// The producer's pacing: one step per send boundary, a step that sends nothing included, and
// the 20-message backlog gate [orig: Server_TickUpdate @0x51E3D6..0x51E3E4;
// Server_SendInitialGameStateToPlayer @0x51BBA0 - world case 0 @0x51BC22..0x51BC29, the gate
// `cmp [ecx+768h], 14h` @0x51BF14..0x51BF1B; the tail @0x51C03A..0x51C13C].
bool run_one_step_per_boundary() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	inmatch::NapiNPConnection &j = f.joiner();
	(void)f.send(opennova::c2s::CHECKSUM_RESPONSE);
	// 20 messages on the wire unacknowledged hold the track; 19 do not.
	j.seq.retained_outbound_message_count = 20;
	if (!expect(f.pump().empty(), "20 unacknowledged messages hold the track")) return false;
	j.seq.retained_outbound_message_count = 19;
	const std::vector<uint8_t> first = f.pump();
	if (!expect(first == std::vector<uint8_t>{opennova::s2c::MISSION_MAP_NAMES},
	            "below 20 the track sends its one 0x2C step")) return false;
	j.seq.retained_outbound_message_count = 0;
	// Subphases 9..15: one record per boundary (the 0x08, then the six 0x2A rows).
	for (int i = 0; i < 7; ++i) {
		if (!expect(f.pump().size() == 1, "one player-sync record per boundary")) return false;
	}
	const std::vector<uint8_t> tail = f.pump();
	if (!expect(tail.size() >= 4 && tail.front() == opennova::s2c::NOOP &&
	                    tail.back() == opennova::s2c::DISCONNECT_UNLOCK &&
	                    j.slot_state == inmatch::PlayerSlotState::AwaitWorldRequest,
	            "the tail is one boundary and parks the slot in state 3")) return false;
	(void)f.send(opennova::c2s::SPAWN_MENU_REQUEST);
	if (!expect(f.pump().empty() && j.burst.world_stream_phase == 1,
	            "the world stream's case 0 spends a boundary with no send")) return false;
	const std::vector<uint8_t> statics = f.pump();
	if (!expect(statics == std::vector<uint8_t>{0x10},
	            "the next boundary sends the first 0x10 page alone")) return false;
	return true;
}

// D-NET-503: the add's body is hidden and carries no spawn protection; the join places it
// again (whatever moved it), clears its hidden bit, and the 0x0F carries the join's placement,
// then the 620 [orig: Entity_SpawnFromAnimSlotProperty @0x43C433 / @0x43C508; Server_OnPlayerJoin
// @0x51A786, @0x51A7DA, the 0x0F @0x51A84C / @0x51A869, 620 @0x51A882].
bool run_join_places_the_hidden_body() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	inmatch::NapiNPConnection &j = f.joiner();
	w::Entity *body = f.world->registry.get(j.link.owned_entity);
	if (!expect(body != nullptr && (body->flags & 1u) != 0 && body->damage_state == 0,
	            "the add's body is hidden, with no spawn protection")) return false;
	const int32_t marker[3] = {body->position.x.q16, body->position.y.q16, body->position.z.q16};
	body->position.set_q16({marker[0] + 0x8000, marker[1], marker[2] + 0xD999}); // moved off it
	(void)f.send(opennova::c2s::CHECKSUM_RESPONSE);
	for (int i = 0; i < 64 && j.slot_state != inmatch::PlayerSlotState::AwaitWorldRequest; ++i)
		(void)f.pump();
	(void)f.send(opennova::c2s::SPAWN_MENU_REQUEST);
	for (int i = 0; i < 256 && j.slot_state != inmatch::PlayerSlotState::AwaitJoin; ++i)
		(void)f.pump();
	(void)f.send(opennova::c2s::MISSION_FILE_STATUS, {0x00});
	std::vector<uint8_t> state_load;
	ns::Datagram d;
	while (f.joiner_transport.pop_outbound(d))
		if (d.tag == opennova::s2c::WORLD_STATE_LOAD) state_load = d.body;
	body = f.world->registry.get(j.link.owned_entity);
	if (!expect(body != nullptr && body->position.x.q16 == marker[0] &&
	                    body->position.y.q16 == marker[1] && body->position.z.q16 == marker[2],
	            "the join places the body on its start marker again")) return false;
	if (!expect((body->flags & 1u) == 0 && body->damage_state == 620,
	            "the join clears the hidden bit and stores the 620 protection")) return false;
	auto word = [&state_load](std::size_t at) {
		return static_cast<int32_t>(static_cast<uint32_t>(state_load[at]) |
				(static_cast<uint32_t>(state_load[at + 1]) << 8) |
				(static_cast<uint32_t>(state_load[at + 2]) << 16) |
				(static_cast<uint32_t>(state_load[at + 3]) << 24));
	};
	return expect(state_load.size() >= 16 && word(4) == marker[0] && word(8) == marker[1] &&
	                      word(12) == marker[2],
	              "the 0x0F carries the join's placement");
}

std::vector<uint8_t> tags_of(const std::vector<ns::Datagram> &datagrams) {
	std::vector<uint8_t> tags;
	for (const ns::Datagram &d : datagrams) tags.push_back(d.tag);
	return tags;
}

// D-NET-501: the join's game-start bundle, in retail's order: the 0x42, the slot's first 0x0A
// (in session), the 0x0F, the 0x4D, the 0x61 seed and the 0x3E; no restriction word set, no
// cease fire, the round running.
bool run_join_bundle_carries_the_first_entity_state() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	if (!expect(f.drive_to_await_join(), "the world stream parks the slot in state 5"))
		return false;
	const std::vector<ns::Datagram> bundle = f.join();
	const std::vector<uint8_t> want = {
			opennova::s2c::CHARATTR_DISABLED_PROPERTIES, opennova::s2c::PER_FRAME_UPDATE,
			opennova::s2c::WORLD_STATE_LOAD, opennova::s2c::SPAWN_SLOT_NOTICE,
			opennova::s2c::TICK_SEED, opennova::s2c::MAP_OVERLAY_RESET};
	if (!expect(tags_of(bundle) == want,
	            "the bundle is 0x42, 0x0A, 0x0F, 0x4D, 0x61, 0x3E")) return false;
	if (!expect(!bundle[1].reliable && !bundle[1].body.empty(),
	            "the join's 0x0A is the unreliable per-frame update")) return false;
	if (!expect(f.joiner().slot_state == inmatch::PlayerSlotState::Joined &&
	                    f.joiner().burst.game_state == 10,
	            "the join leaves slot state 6 and game state 10")) return false;
	// The tick's own 0x0A follows the tail at the slot's next fan.
	f.joiner().s2c_send_boundary_open = true;
	inmatch::Server_TickUpdate(*f.ctx);
	const std::vector<uint8_t> next = JoinFixture::drain(f.joiner_transport);
	return expect(contains(next, opennova::s2c::PER_FRAME_UPDATE),
	              "the server tick's 0x0A follows the bundle");
}

// FOUND-6: a cease fire in effect adds one reliable chat line after the seed, channel 0,
// sender slot 0xFF [orig: Server_OnPlayerJoin @0x51A998..0x51A9F3].
bool run_join_tells_the_cease_fire() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	if (!expect(f.drive_to_await_join(), "the world stream parks the slot in state 5"))
		return false;
	f.world->rules.cease_fire = true;
	const std::vector<ns::Datagram> bundle = f.join();
	const std::vector<uint8_t> tags = tags_of(bundle);
	if (!expect(tags.size() == 7 && tags[4] == opennova::s2c::TICK_SEED &&
	                    tags[5] == opennova::s2c::CHAT_BROADCAST &&
	                    tags[6] == opennova::s2c::MAP_OVERLAY_RESET,
	            "the cease-fire 0x14 sits between the 0x61 and the 0x3E")) return false;
	const std::string text = "* * * CEASE FIRE IS IN EFFECT! * * *";
	std::vector<uint8_t> want = {0x00, 0xFF};
	want.insert(want.end(), text.begin(), text.end());
	want.push_back(0x00);
	return expect(bundle[5].body == want && bundle[5].reliable,
	              "the line is [channel 0][sender 0xFF][text], reliable");
}

// FOUND-6: a join into a finished round takes the round end's legs after its seed: the zero
// 0x61, this slot's 0x1D, game state 11 and slot state 7 [orig: Server_OnPlayerJoin
// @0x51A9F8..0x51AA5C].
bool run_join_into_a_finished_round() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	if (!expect(f.drive_to_await_join(), "the world stream parks the slot in state 5"))
		return false;
	f.world->process_round_end(1);
	if (!expect(f.world->match.outcome().ended, "the round ended")) return false;
	const std::vector<ns::Datagram> bundle = f.join();
	const std::vector<uint8_t> tags = tags_of(bundle);
	const std::vector<uint8_t> want = {
			opennova::s2c::CHARATTR_DISABLED_PROPERTIES, opennova::s2c::PER_FRAME_UPDATE,
			opennova::s2c::WORLD_STATE_LOAD, opennova::s2c::SPAWN_SLOT_NOTICE,
			opennova::s2c::TICK_SEED, opennova::s2c::TICK_SEED, opennova::s2c::END_ROUND_HEADER,
			opennova::s2c::MAP_OVERLAY_RESET};
	if (!expect(tags == want, "the round-over legs follow the seed, ahead of the 0x3E"))
		return false;
	if (!expect(bundle[4].body != std::vector<uint8_t>(4, 0) &&
	                    bundle[5].body == std::vector<uint8_t>(4, 0),
	            "the join's seed, then the zero seed")) return false;
	const inmatch::NapiNPConnection &j = f.joiner();
	return expect(j.slot_state == inmatch::PlayerSlotState::RoundOver &&
	                      j.burst.game_state == 11 && j.link.round_watermark == 0 &&
	                      j.tick_seed == 0,
	              "slot state 7, game state 11, the watermark and the seed cleared");
}

// D-NET-NEW-L1-7: once the round is over the producer sends nothing and holds the track
// where it stood [orig: Server_SendInitialGameStateToPlayer @0x51BBA3..0x51BBA9,
// @0x51BEFF..0x51BF05].
bool run_track_waits_while_the_round_is_over() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	inmatch::NapiNPConnection &j = f.joiner();
	(void)f.send(opennova::c2s::CHECKSUM_RESPONSE);
	f.world->process_round_end(1);
	for (int i = 0; i < 16; ++i) {
		if (!expect(f.pump().empty(), "no player-sync record after the round end")) return false;
	}
	return expect(j.slot_state == inmatch::PlayerSlotState::PlayerSync &&
	                      j.burst.player_sync_subphase <
	                              inmatch::InitialStateBurst::kSyncMissionMapNames + 1,
	              "the track holds where it stood");
}

// The shot words of the round events the first S2C 0x0A among `datagrams` carries.
std::vector<uint16_t> round_shots(const std::vector<ns::Datagram> &datagrams) {
	std::vector<uint16_t> shots;
	for (const ns::Datagram &d : datagrams) {
		if (d.tag != opennova::s2c::PER_FRAME_UPDATE) continue;
		opennova::FrameUpdate frame;
		if (!opennova::decode_frame_update(d.body.data(), d.body.size(), ns::class_for_type_id,
				frame))
			break;
		for (const opennova::RoundEventRecord &round : frame.round_events)
			shots.push_back(round.shot_seq);
		break;
	}
	return shots;
}

// D-NET-NEW-L1-8: the join arms the slot's round-event watermark at the frame clock less one,
// so the join's own 0x0A carries the rounds its frame fired and none older; each walk stamps
// the clock, and a round the frame's entity update fires (the next frame's clock, the tick
// having stepped it after its send pump) rides the next 0x0A. Ours armed lazily at the first
// walk, which sent nothing, so a joiner lost its join frame's rounds.
// [orig: RoundData_AddRound @0x4FDCAC..0x4FDCBA; CNetPlayer_SetGameState @0x4C4225..0x4C422D;
//  Server_OnPlayerJoin @0x51A740..0x51A74C; Server_BuildRoundEventListForPlayer @0x4FFEE8,
//  @0x5001A8..0x5001AE; Server_TickUpdate @0x51E48C]
bool run_join_arms_the_round_watermark() {
	JoinFixture f(/*with_existing=*/false);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	if (!expect(f.drive_to_await_join(), "the world stream parks the slot in state 5"))
		return false;
	w::Entity seed;
	seed.kind = w::EntityKind::Organic;
	seed.health = 100;
	seed.alive = true;
	seed.position = {12.0f, 22.0f, 1.0f};
	const w::EntityHandle shooter = f.world->registry.spawn(0, seed);
	w::RoundRing &ring = f.world->out.rounds;
	const auto fire = [&](uint16_t shot) {
		w::RoundEvent round;
		round.shooter_handle = shooter.packed;
		round.origin_x = w::to_fixed(12.0);
		round.origin_y = w::to_fixed(22.0);
		round.origin_z = w::to_fixed(2.0);
		round.shot_seq = shot;
		ring.add(round);
	};
	ring.frame_clock = 99;
	fire(1); // an earlier frame's round
	ring.frame_clock = 100;
	fire(2); // the join's own frame, ahead of its 0x0B
	const std::vector<ns::Datagram> bundle = f.join();
	if (!expect(round_shots(bundle) == std::vector<uint16_t>{2},
	            "the join's 0x0A carries its own frame's round and no older one")) return false;
	if (!expect(f.joiner().link.round_watermark == 100, "the walk stamps the frame clock"))
		return false;
	f.joiner().s2c_send_boundary_open = true;
	inmatch::Server_TickUpdate(*f.ctx); // its fan finds nothing new; the clock steps
	(void)JoinFixture::drain(f.joiner_transport);
	if (!expect(ring.frame_clock == 101, "the tick steps the frame clock once")) return false;
	fire(3); // the entity update's round
	inmatch::Server_TickUpdate(*f.ctx);
	std::vector<ns::Datagram> next;
	ns::Datagram d;
	while (f.joiner_transport.pop_outbound(d)) next.push_back(d);
	return expect(round_shots(next) == std::vector<uint16_t>{3},
	              "the next 0x0A carries the entity update's round");
}

// D-NET-401: the player-slot table is the slot limit alone. A Serve Only host's spectator
// limit adds no rows; its spawn pump's 0x04 sends that count (byte 18) and the joiner's row.
// [orig: Server_InitNewRoundState - the limit @0x51C948..0x51C96B (maxPlayers plus the
//  dedicated host's row), @0x51C976, Server_AllocatePlayerSlotTable @0x51C97B, the count
//  @0x51C1E7; NetPacket_WriteSlotAssignment @0x502B30, the count byte @0x502BB4]
bool run_slot_table_is_the_slot_limit() {
	JoinFixture f(/*with_existing=*/false);
	f.ctx->config.dedicated_server = true;
	f.ctx->config.spectator_slots = 3;
	const uint32_t limit = f.ctx->config.player_slot_limit();
	if (!expect(limit == 5 && f.ctx->config.total_player_slot_capacity() == limit,
	            "the table is max_players plus the host's row, no spectator rows")) return false;
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1,
	            "the spawn pump adds the joiner")) return false;
	const std::vector<opennova::ProtocolMessage> metadata = inmatch::build_spawn_pump_metadata(
			f.ctx->config, f.ctx->mission_text_title, f.joiner(), f.ctx->np_protocol.connection_list,
			f.world.get(), f.ctx->rotation, &f.ctx->host_slot);
	for (const opennova::ProtocolMessage &m : metadata) {
		if (m.tag != 0x04) continue;
		return expect(m.payload.size() >= 19 && m.payload[18] == limit && m.payload[17] == 1,
		              "the 0x04 sends the slot limit and the joiner's row 1");
	}
	return expect(false, "the spawn pump's metadata carries a 0x04");
}

// A Serve Only host (cap 5, six rows: the host's row 0 and 1..5) and admitted joiners
// waiting for the spawn pump. Heap-held, as JoinFixture.
struct SlotFixture {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	opennova::bms::File mission;
	std::vector<std::unique_ptr<ns::UdpSessionTransport>> transports;

	explicit SlotFixture(int pool0_rows = 16) {
		world->registry.configure_pool(0, pool0_rows);
		world->registry.configure_pool(3, 4);
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {10.0f, 20.0f, 1.0f};
		start.item_type_index = 1;
		start.has_item_def = true;
		world->registry.spawn(3, start);
		opennova::mission::make_default(mission);
		inmatch::GameConfig config;
		config.max_players = 5;
		config.dedicated_server = true;
		inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Socketless, /*host_key=*/0, nullptr, config);
		ctx->world = world.get();
		ctx->mission = &mission;
		inmatch::Server_InitNewRoundState(*ctx);
	}

	inmatch::NapiNPConnection &admit(uint16_t port) {
		transports.push_back(
				std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host));
		inmatch::NapiNPConnection &c = ctx->np_protocol.connection_list.emplace_back();
		c.type = inmatch::NapiNPConnection::kTypeServerSide;
		c.peer = opennova::PeerAddr{0x0100007Fu, port};
		c.connection_id = inmatch::kFirstJoinerDcb + port;
		c.player_name = "J" + std::to_string(port);
		c.phase = inmatch::ConnectionPhase::Joined;
		c.admission_stage = inmatch::GameAdmissionStage::Complete;
		c.self_id_seen = true;
		c.link.transport = transports.back().get();
		c.link.mode = ns::TransportMode::Client;
		return c;
	}

	inmatch::NapiNPConnection *find(uint16_t port) {
		for (inmatch::NapiNPConnection &c : ctx->np_protocol.connection_list)
			if (c.peer.port == port) return &c;
		return nullptr;
	}

	int row_of(uint16_t port) {
		const inmatch::NapiNPConnection *c = find(port);
		return c != nullptr && c->phase >= inmatch::ConnectionPhase::PlayerAdded
				? c->reply.player_slot : -1;
	}

	void ticks(int n) {
		for (int i = 0; i < n; ++i) inmatch::Server_AdvanceFreeRowTicks(*ctx);
	}
};

// D-NET-351: each inactive row's +0x180 counts the server ticks since it was freed (or the
// table allocated); a remote add takes the inactive row whose count is greatest, the lowest
// index on a tie. So joiners fill rows 1, 2, 3 in turn, and after the first leaves the next
// joiner takes row 4 (never used), not row 1 (just freed); rows 4 and 5 taken, row 1 comes
// next. The server tick steps every inactive row's count once and no active row's.
// [orig: PlayerSlot_PickFreeSlot @0x4FD7B0 - @0x4FD7D0, @0x4FD7DA, @0x4FD7E2..0x4FD7EA;
//  Server_TickUpdate @0x51E3A0..0x51E3A4, @0x51E457..0x51E467; Server_PlayerAdd's memset
//  @0x51CD06; Server_AllocatePlayerSlotTable @0x51C1CB]
bool run_the_pick_takes_the_row_free_longest() {
	SlotFixture f;
	f.ticks(10);
	(void)f.admit(31001);
	(void)f.admit(31002);
	(void)f.admit(31003);
	(void)inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world);
	if (!expect(f.row_of(31001) == 1 && f.row_of(31002) == 2 && f.row_of(31003) == 3,
	            "equal counts: the joiners take rows 1, 2, 3")) return false;
	if (!expect(f.ctx->player_row_free_ticks[1] == 0 && f.ctx->player_row_free_ticks[4] == 10,
	            "the add zeroes its row; a free row keeps counting")) return false;
	const uint32_t before = f.ctx->player_row_free_ticks[4];
	inmatch::Server_TickUpdate(*f.ctx);
	if (!expect(f.ctx->player_row_free_ticks[4] == before + 1 &&
	                    f.ctx->player_row_free_ticks[1] == 0 && f.ctx->player_row_free_ticks[0] == 0,
	            "one server tick steps each inactive row once, no active row")) return false;
	if (!expect(inmatch::destroy_connection(*f.ctx, f.find(31001)->peer, nullptr),
	            "the first joiner leaves")) return false;
	f.ticks(3);
	(void)f.admit(31004);
	(void)inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world);
	if (!expect(f.row_of(31004) == 4, "after the leave the next joiner takes row 4, not row 1"))
		return false;
	(void)f.admit(31005);
	(void)inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world);
	(void)f.admit(31006);
	(void)inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world);
	return expect(f.row_of(31005) == 5 && f.row_of(31006) == 1,
	              "rows 4 and 5 taken, the freed row 1 comes next");
}

// D-NET-715: the pump takes the joiner off its pending list and sends its 0x03 before the add,
// so an add that fails (here the player pool is full) leaves it connected with no row, no body
// and no disconnect; the pump never walks it again, so no second attempt and no further 0x03.
// [orig: CNapiServer_ProcessPendingPlayerSpawns - the pending test @0x4C8E68..0x4C8E6C,
//  NapiNode_UnlinkFromList @0x4C8FF6, the 0x03 @0x4C9034, the add @0x4C903A, its success test
//  @0x4C904E, goto LABEL_5 @0x4C9128]
bool run_a_failed_add_leaves_the_joiner_bodyless() {
	SlotFixture f(/*pool0_rows=*/1);
	w::Entity filler;
	filler.kind = w::EntityKind::Organic;
	(void)f.world->registry.spawn(0, filler); // the one pool-0 row taken
	inmatch::NapiNPConnection &j = f.admit(31101);
	ns::UdpSessionTransport *transport = f.transports.back().get();
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 0,
	            "the add fails")) return false;
	if (!expect(!j.link.owned_entity.valid() && j.phase < inmatch::ConnectionPhase::PlayerAdded &&
	                    !j.host_disconnect_sent && !j.pending_disconnect && j.spawn_queue_unlinked,
	            "no body, no row, no disconnect; off the pending list")) return false;
	auto sync_ticks = [&](uint32_t tick) {
		(void)inmatch::tick_connections(*f.ctx, /*elapsed_ms=*/16, tick);
		int count = 0;
		ns::Datagram d;
		while (transport->pop_outbound(d))
			if (d.tag == opennova::s2c::SYNC_TICK) ++count;
		return count;
	};
	if (!expect(sync_ticks(5) == 1, "its unlink's 0x03 is the one message it gets")) return false;
	(void)inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world);
	if (!expect(!j.link.owned_entity.valid() && sync_ticks(6) == 0,
	            "the pump never walks it again: no second add, no further 0x03")) return false;
	return expect(f.find(31101) != nullptr, "the joiner stays connected");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_track_waits_for_player_sync_request()) ++failures;
	if (!run_no_player_list_off_the_scoreboard_clock()) ++failures;
	if (!run_one_step_per_boundary()) ++failures;
	if (!run_join_places_the_hidden_body()) ++failures;
	if (!run_join_bundle_carries_the_first_entity_state()) ++failures;
	if (!run_join_tells_the_cease_fire()) ++failures;
	if (!run_join_into_a_finished_round()) ++failures;
	if (!run_track_waits_while_the_round_is_over()) ++failures;
	if (!run_join_arms_the_round_watermark()) ++failures;
	if (!run_slot_table_is_the_slot_limit()) ++failures;
	if (!run_the_pick_takes_the_row_free_longest()) ++failures;
	if (!run_a_failed_add_leaves_the_joiner_bodyless()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
