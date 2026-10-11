// world-wac-ai-re §20 — the SP round-outcome loop at the server tick: the kill
// tallies feeding the WAC bluekills/greenkills builtins [orig: Score_ProcessKillEvent
// @0x4fd400 -> Score_TallyKillByLocalPlayer @0x4fd160 / Score_TallyKillByOthers
// @0x4fd300], the humans count [orig: Server_BuildEntitySlotLists @0x4f97a0], the SP
// auto-lose win condition [orig: Server_CheckWinConditions @0x51ad40, SP leg
// @0x51ad6f — dead local player without the SinglePlayerRespawn attrib (0x40)], the
// round-end latch + host effect [orig: Server_ProcessRoundEnd @0x5164f0], and the
// post-round respawn hold [orig: the g_SpawnSuccessGate check @0x519af6].
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/end_round_protocol.h>

#include <runtime/mission/event_runtime.h>

#include <runtime/inmatch/loopback_channel.h>

#include <net/npwire/replication_model.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/objectives_feed.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>
#include <runtime/world/zone_chain.h>

#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

using conn_fixture::make_conn;

void push_death(w::World &world, w::EntityHandle victim, w::EntityHandle killer) {
	w::RoundDeath d;
	d.victim = victim;
	d.killer = killer;
	d.victim_handle = victim.packed;
	d.killer_handle = killer.packed;
	world.round_sim.deaths.push_back(d);
}

// A death raised at a damage-pass lethal edge, the only producer of the kill
// accounting [orig: Score_ProcessKillEvent @0x4FD400 from
// Projectile_ProcessDamageOnTarget @0x4E8133].
void push_kill(w::World &world, w::EntityHandle victim, w::EntityHandle killer) {
	push_death(world, victim, killer);
	world.round_sim.deaths.back().kill_event = true;
}

w::EntityHandle match_player(w::World &world, uint8_t slot, uint8_t team,
		const char *name) {
	w::Entity e;
	e.kind = w::EntityKind::Organic;
	e.player_class = 8;
	e.team = team;
	e.health = 100;
	e.alive = true;
	e.flags = w::kEntityFlagPlayer;
	e.engine_flags = w::kEntityFlagPlayer;
	const w::EntityHandle handle = world.registry.spawn(0, e);
	world.match.upsert_player({handle, slot, name});
	return handle;
}

int32_t fixed(float value) {
	return static_cast<int32_t>(value * 65536.0f);
}

w::CollisionModel contact_box(int32_t type) {
	w::CollisionModel model;
	auto plane = [&](int nx, int ny, int nz, float distance) {
		w::CollisionPlane value;
		value.nx = static_cast<int16_t>(nx);
		value.ny = static_cast<int16_t>(ny);
		value.nz = static_cast<int16_t>(nz);
		value.dist = fixed(distance);
		model.planes.push_back(value);
	};
	plane(16384, 0, 0, -2.0f);
	plane(-16384, 0, 0, -2.0f);
	plane(0, 16384, 0, -2.0f);
	plane(0, -16384, 0, -2.0f);
	plane(0, 0, 16384, -3.0f);
	plane(0, 0, -16384, 0.0f);

	w::CollisionVolume volume;
	volume.type = type;
	volume.min_x = volume.min_y = fixed(-2.0f);
	volume.max_x = volume.max_y = fixed(2.0f);
	volume.min_z = 0;
	volume.max_z = fixed(3.0f);
	volume.plane_count = 6;
	model.volumes.push_back(volume);

	w::CollisionSection section;
	section.volume_count = 1;
	model.sections.push_back(section);
	return model;
}

void install_collision_system(w::World &world, w::CollisionWorld &collision) {
	world.collision = &collision;
	world.ai.collision = &collision;
}

w::AiEntity *attach_remote_body(w::World &world, w::AiSystem &ai,
		w::EntityHandle handle, float previous_x) {
	w::Entity *entity = world.registry.get(handle);
	if (entity == nullptr) return nullptr;
	w::AiEntity *body = ai.at(ai.attach(handle));
	if (body == nullptr) return nullptr;
	body->inf.active = true;
	body->net_is_remote_peer = true;
	body->health = entity->health;
	body->team = entity->team;
	body->pos[0] = fixed(entity->position.x);
	body->pos[1] = fixed(entity->position.y);
	body->pos[2] = fixed(entity->position.z);
	body->collide_state.prev_valid = true;
	body->collide_state.prev_pos[0] = fixed(previous_x);
	body->collide_state.prev_pos[1] = body->pos[1];
	body->collide_state.prev_pos[2] = body->pos[2];
	return body;
}

void move_remote_body(w::World &world, w::AiSystem &ai,
		w::EntityHandle handle, const w::Vec3 &position) {
	w::Entity *entity = world.registry.get(handle);
	w::AiEntity *body = ai.for_handle(handle);
	if (entity == nullptr || body == nullptr) return;
	entity->position = position;
	body->pos[0] = fixed(position.x);
	body->pos[1] = fixed(position.y);
	body->pos[2] = fixed(position.z);
}

void prime_collision_tables(w::World &world, w::CollisionWorld &collision) {
	for (int i = 0; i < 17; ++i)
		collision.build_tick_tables(world);
}

void ready_mp_connection(inmatch::NapiNPConnection &conn, uint8_t slot) {
	conn.reply.player_slot = slot;
	conn.admission_stage = inmatch::GameAdmissionStage::Complete;
}

bool drain_round_header(ns::LoopbackChannel &channel, EndRoundHeader &header,
		bool non_team_form = false) {
	bool saw_seed = false;
	bool saw_header = false;
	ns::Datagram datagram;
	while (channel.client_recv(datagram)) {
		if (datagram.tag == s2c::TICK_SEED)
			saw_seed = datagram.body == std::vector<uint8_t>(4, 0);
		// The 0x61 seed precedes 0x1D on the wire; a header before it is a
		// failure, not a pass. The header form mirrors the receiving
		// client's `is_in_session && !(g_GameType & 0x10000)` pick.
		if (datagram.tag == s2c::END_ROUND_HEADER && saw_seed) {
			saw_header = decode_end_round_header(
					datagram.body.data(), datagram.body.size(),
					non_team_form, header);
		}
	}
	return saw_seed && saw_header;
}

// The 0x2B service reads the host's frozen board stream through the dispatch
// inputs, exactly as the owner pump threads it.
inmatch::ServerDispatchInputs board_inputs(const inmatch::NapiNPServerCtx &ctx) {
	inmatch::ServerDispatchInputs inputs;
	inputs.round_end_board_stream = &ctx.round_end_board_stream;
	return inputs;
}

std::vector<ProtocolMessage> request_board_chunk(
		inmatch::NapiNPServerCtx &ctx, inmatch::NapiNPConnection &connection,
		w::World &world, uint16_t offset) {
	return inmatch::dispatch_session_replies(
			ctx.config, connection,
			{make_protocol_message(c2s::END_ROUND_STATS_REQUEST,
					encode_end_round_stats_request(offset))},
			world.logic_tick, ctx.np_protocol.connection_list, &world,
			board_inputs(ctx));
}

bool pull_round_board(inmatch::NapiNPServerCtx &ctx, inmatch::NapiNPConnection &connection,
		w::World &world, EndRoundStats &board, size_t &chunk_count) {
	std::vector<uint8_t> bytes;
	uint16_t offset = 0;
	uint16_t total_size = 0;
	chunk_count = 0;
	for (;;) {
		const std::vector<ProtocolMessage> replies =
				request_board_chunk(ctx, connection, world, offset);
		if (replies.size() != 1 || replies[0].tag != s2c::END_ROUND_STATS)
			return false;

		EndRoundStatsChunk chunk;
		if (!decode_end_round_stats_chunk(
					replies[0].payload.data(), replies[0].payload.size(), chunk) ||
				chunk.chunk_offset != offset)
			return false;
		if (chunk_count == 0) total_size = chunk.total_size;
		else if (chunk.total_size != total_size) return false;
		bytes.insert(bytes.end(), chunk.chunk.begin(), chunk.chunk.end());
		++chunk_count;
		if (chunk.complete()) break;
		if (chunk.chunk.empty() || bytes.size() > UINT16_MAX) return false;
		offset = static_cast<uint16_t>(bytes.size());
	}
	return bytes.size() == total_size &&
			decode_end_round_stats(bytes.data(), bytes.size(), board);
}

void test_tdm_round_wire_and_linger() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = 0x10000u;
	rules.score_limit = 1;
	rules.score_values.emplace();
	(*rules.score_values)[3] = 10;
	(*rules.score_values)[5] = -2;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	const w::EntityHandle red = match_player(world, 7, 2, "Red");
	// Retail omits a configured scoreboard column when every player value is
	// zero. Seed the otherwise-unimplemented stat producers so this fixture
	// exercises all 14 TDM columns and crosses the 200-byte 0x56 boundary.
	w::MatchStats &blue_stats = world.match.player(blue)->stats;
	for (const size_t index : {2u, 4u, 6u, 9u, 10u, 15u, 16u, 18u, 19u, 20u, 28u})
		blue_stats[index] = 1;

	ns::LoopbackChannel blue_wire;
	ns::LoopbackChannel red_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &blue_wire, ns::TransportMode::Client, blue, true));
	ctx.np_protocol.connection_list.push_back(
			make_conn(2, 1, &red_wire, ns::TransportMode::Client, red, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);
	ready_mp_connection(ctx.np_protocol.connection_list[1], 7);

	// The zero-armed one-second service fires on the first frame, then every
	// 62 ticks; a death routed between boundaries waits for the next pass.
	inmatch::Server_TickUpdate(ctx);
	push_death(world, red, blue);
	for (int i = 0; i < 61; ++i) inmatch::Server_TickUpdate(ctx);
	expect(!world.match.outcome().ended,
			"TDM kill-limit win waits for the 1 Hz win-condition pass");
	blue_wire.clear();
	red_wire.clear();
	// Before the round-end producer runs there is no board stream to cut from:
	// a 0x2B request receives nothing.
	expect(ctx.round_end_board_stream.empty() &&
			request_board_chunk(ctx, ctx.np_protocol.connection_list[0],
					world, 0).empty(),
			"C2S 0x2B before the round-end announce receives no 0x56");
	inmatch::Server_TickUpdate(ctx); // the next one-second win-condition boundary
	expect(world.match.outcome().ended &&
			world.match.outcome().winner_team == 1,
			"TDM kill limit ends for the killer's team");
	expect(ctx.round_end_announced && ctx.round_end_linger_ticks == 2790,
			"TDM announces once and seeds the exact MP linger");
	// The round tallies the status page shows: one TDM round, one team 1 win
	// [orig: Server_ProcessRoundEnd @0x516883..0x5168d8].
	expect(ctx.rounds_played == 1 && ctx.round_wins[0] == 1 && ctx.round_wins[1] == 0,
			"a TDM round end counts the round and the winner's win");
	// The producer froze the stream once (stru_C947D8) before the 0x61/0x1D
	// push; every 0x2B pull cuts from that same byte sequence.
	// [orig: Server_BuildEndOfRoundScoreboard(1, winTeam) @0x516590]
	const std::vector<uint8_t> frozen_board = encode_end_round_stats(
			inmatch::build_end_round_stats(world.match.result()));
	expect(!ctx.round_end_board_stream.empty() &&
			ctx.round_end_board_stream == frozen_board,
			"the announce freezes the encoded board stream once");
	{
		const std::vector<ProtocolMessage> first = request_board_chunk(
				ctx, ctx.np_protocol.connection_list[0], world, 0);
		const std::vector<ProtocolMessage> second = request_board_chunk(
				ctx, ctx.np_protocol.connection_list[0], world, 0);
		expect(first.size() == 1 && second.size() == 1 &&
				first[0].tag == s2c::END_ROUND_STATS &&
				first[0].payload == second[0].payload,
				"two offset-0 pulls return byte-identical 0x56 chunks");
		EndRoundStatsChunk chunk;
		expect(first.size() == 1 &&
				decode_end_round_stats_chunk(first[0].payload.data(),
						first[0].payload.size(), chunk) &&
				chunk.total_size == frozen_board.size() &&
				chunk.chunk.size() == 200 &&
				std::equal(chunk.chunk.begin(), chunk.chunk.end(),
						frozen_board.begin()),
				"the offset-0 chunk is the frozen stream's first 200 bytes");
	}

	EndRoundHeader blue_header;
	EndRoundHeader red_header;
	expect(drain_round_header(blue_wire, blue_header),
			"TDM blue peer receives 0x61 then decodable 0x1D");
	expect(drain_round_header(red_wire, red_header),
			"TDM red peer receives 0x61 then decodable 0x1D");
	expect(blue_header.winner_team == 1 && blue_header.team_score_0 == 1 &&
			blue_header.team_score_1 == 0 && blue_header.player_index == 0,
			"TDM 0x1D carries team scores and recipient board index");
	expect(red_header.player_index == 1,
			"TDM 0x1D is recipient-specific");

	EndRoundStats board;
	size_t chunk_count = 0;
	const bool decoded = pull_round_board(
			ctx, ctx.np_protocol.connection_list[0], world, board, chunk_count);
	expect(decoded && chunk_count == 2,
			"C2S 0x2B pulls the 223-byte TDM board as 200-byte 0x56 chunks");
	if (decoded) {
		expect(board.players.size() == 2 && board.players[0].slot == 3,
				"TDM board freezes point-sorted players");
		expect(board.players[0].kills == 1 &&
				board.players[0].deaths == 10 &&
				board.players[0].score == 1,
				"TDM board preserves primary/raw29/raw5 wire positions");
		expect(board.team_fields.size() == 14 &&
				board.team_fields[0] == std::pair<uint8_t, uint8_t>{19, 1} &&
				board.team_fields[1] == std::pair<uint8_t, uint8_t>{3, 1} &&
				board.team_fields[2] == std::pair<uint8_t, uint8_t>{2, 0} &&
				board.team_fields.back() == std::pair<uint8_t, uint8_t>{21, 1},
				"TDM board declares the retail FIELD schema in retail order");
		expect(board.players[0].per_team.size() == 14 &&
				board.players[0].per_team[0] == 10 &&
				board.players[0].per_team[1] == 1,
				"TDM player columns resolve raw points and enemy kills");
		expect(board.team_rows.size() == 3 &&
				board.team_rows[1].size() == 14 &&
				board.team_rows[1][0] == 10 &&
				board.team_rows[1][1] == 1,
				"TDM trailing matrix carries neutral/team-1/team-2 score rows");
	}
	const std::vector<ProtocolMessage> invalid_offset_reply = request_board_chunk(
			ctx, ctx.np_protocol.connection_list[0], world, UINT16_MAX);
	expect(invalid_offset_reply.empty(),
			"C2S 0x2B offset beyond the frozen board receives no 0x56 reply");

	for (int i = 0; i < 2789; ++i) inmatch::Server_TickUpdate(ctx);
	expect(ctx.is_in_session == 1 && ctx.round_end_linger_ticks == 1,
			"MP session remains live through linger tick 2789");
	inmatch::Server_TickUpdate(ctx);
	// The linger's end stores the map change's exit and leaves the session up:
	// 3 with REPLAY off (and 4 under REPLAY with LASTGAME off).
	// [orig: Server_TickUpdate @0x51DB47..0x51DB63]
	expect(ctx.is_in_session == 1 && ctx.round_end_linger_ticks == 0 &&
					ctx.mission_exit_reason == inmatch::kMissionExitMapCycle,
			"the round end stores mission exit 3 at exactly 2790 post-announcement ticks");
}

// The DM/KOTH-family 0x1D: an in-session non-team round end serializes the
// top three frozen-board rows (name + primary score) instead of the
// winner/team-score words, and the recipient index still names the frozen
// board row. [orig: EndRoundScoreboard_SerializeHeader @0x5052a6..0x505381]
void test_dm_round_wire_named_header() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = game_type::kDeathmatch;
	rules.score_limit = 1;
	rules.score_values.emplace();
	(*rules.score_values)[3] = 10;
	world.match.configure(rules);
	// Every non-team Player sits on team 1 [orig: Server_AssignPlayerTeam
	// @0x4FE3EC]; the kills stay enemy kills because the scorer's team rows
	// exist only in team modes [orig: GameEvent_ProcessScoring @0x52F657].
	const w::EntityHandle ace = match_player(world, 3, 1, "Ace");
	const w::EntityHandle bee = match_player(world, 7, 1, "Bee");
	const w::EntityHandle cid = match_player(world, 9, 1, "Cid");

	ns::LoopbackChannel ace_wire;
	ns::LoopbackChannel cid_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &ace_wire, ns::TransportMode::Client, ace, true));
	ctx.np_protocol.connection_list.push_back(
			make_conn(2, 1, &cid_wire, ns::TransportMode::Client, cid, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);
	ready_mp_connection(ctx.np_protocol.connection_list[1], 9);

	inmatch::Server_TickUpdate(ctx);
	push_death(world, cid, ace);
	push_death(world, bee, ace);
	push_death(world, cid, bee);
	for (int i = 0; i < 61; ++i) inmatch::Server_TickUpdate(ctx);
	expect(!world.match.outcome().ended,
			"DM kill-limit win waits for the 1 Hz win-condition pass");
	ace_wire.clear();
	cid_wire.clear();
	inmatch::Server_TickUpdate(ctx); // the next one-second win-condition boundary
	expect(world.match.outcome().ended, "DM score limit ends the round");
	const w::MatchResult &result = world.match.result();
	expect(result.players.size() == 3, "three frozen DM board rows");

	EndRoundHeader ace_header;
	expect(drain_round_header(ace_wire, ace_header, /*non_team_form=*/true),
			"DM round end reaches the named 0x1D form on the wire");
	for (size_t i = 0; i < 3 && i < result.players.size(); ++i) {
		expect(ace_header.player_names[i] == result.players[i].identity.name,
				"named header row matches the frozen board name");
		expect(ace_header.player_scores[i] ==
						static_cast<int16_t>(result.players[i].primary_score),
				"named header score is the row's primary score");
	}
	expect(ace_header.player_names[0] == "Ace",
			"the top scorer heads the named header");
	expect(ace_header.draw == 0, "a decided DM round is not a draw");
	expect(ace_header.player_index >= 0 &&
					result.players[ace_header.player_index].identity.slot == 3,
			"the recipient index names the recipient's frozen board row");

	EndRoundHeader cid_header;
	expect(drain_round_header(cid_wire, cid_header, /*non_team_form=*/true),
			"every recipient gets the named form");
	expect(cid_header.player_index >= 0 &&
					result.players[cid_header.player_index].identity.slot == 9,
			"the second recipient's index is its own frozen row");
	expect(cid_header.player_names[0] == ace_header.player_names[0],
			"the named rows are recipient-independent");
}

// The DM/KOTH-family board orders by the game-type primary (kills), not raw
// points, and the 0x1D draw byte is the all-tied test.
// [orig: Player_ComputeScore @0x500ad0..0x500adf; Server_BuildEndOfRoundScoreboard
// @0x50920e..0x50926c; Server_ProcessRoundEnd @0x5165a3..0x5167fd]
void test_dm_round_wire_kill_order_and_tie() {
	// (a) Ace: 2 kills + 3 suicides = 5 points; Bee: 1 kill - 1 death = 8
	// points. The named rows and their scores follow kills.
	{
		w::World world;
		world.registry.configure_pool(0, 8);
		w::MatchRules rules;
		rules.game_type = game_type::kDeathmatch;
		rules.score_values.emplace();
		(*rules.score_values)[3] = 10;
		(*rules.score_values)[4] = -3;
		(*rules.score_values)[5] = -2;
		world.match.configure(rules);
		// All three on team 1, the retail non-team assignment
		// [orig: Server_AssignPlayerTeam @0x4FE3EC].
		const w::EntityHandle ace = match_player(world, 3, 1, "Ace");
		const w::EntityHandle bee = match_player(world, 7, 1, "Bee");
		const w::EntityHandle cid = match_player(world, 9, 1, "Cid");
		world.match.record_death(world, bee, ace);
		world.match.record_death(world, cid, ace);
		for (int i = 0; i < 3; ++i) world.match.record_death(world, ace, ace);
		world.match.record_death(world, cid, bee);
		expect(world.match.player(ace)->stats[w::MatchStats::kPoints] == 5 &&
				world.match.player(bee)->stats[w::MatchStats::kPoints] == 8,
				"the DM fixture makes points and kills disagree");
		world.process_round_end(0);
		const w::MatchResult &result = world.match.result();
		expect(result.players.size() == 3 &&
				result.players[0].identity.name == "Ace" &&
				result.players[1].identity.name == "Bee" &&
				result.players[2].identity.name == "Cid",
				"the frozen DM board is ordered by kills, not points");
		expect(!result.draw, "a clear DM leader is not a draw");
		const EndRoundHeader header =
				inmatch::build_end_round_header(result, 7, /*non_team_form=*/true);
		expect(header.player_names[0] == "Ace" && header.player_scores[0] == 2 &&
				header.player_names[1] == "Bee" && header.player_scores[1] == 1 &&
				header.player_names[2] == "Cid" && header.player_scores[2] == 0,
				"the named 0x1D rows carry the kill-ordered primaries");
		expect(header.draw == 0 && header.player_index == 1,
				"the recipient index follows the kill order");
		const std::vector<uint8_t> wire = encode_end_round_header(header, true);
		EndRoundHeader decoded;
		expect(decode_end_round_header(wire.data(), wire.size(), true, decoded) &&
				decoded.draw == 0 && decoded.player_scores[0] == 2 &&
				decoded.player_names[0] == "Ace",
				"the named 0x1D form round-trips the kill order and draw byte");
		expect(world.match.player(ace)->stats[w::MatchStats::kRoundMarker] == 2 &&
				world.match.player(bee)->stats[w::MatchStats::kRoundMarker] == 0,
				"the non-team winner marker lands on the top scorer only");
	}

	// (b) Both rows tied at one kill: the all-tied test makes it a draw, the
	// rows keep slot order, and nobody is awarded.
	{
		w::World world;
		world.registry.configure_pool(0, 8);
		w::MatchRules rules;
		rules.game_type = game_type::kDeathmatch;
		rules.score_values.emplace();
		(*rules.score_values)[3] = 10;
		world.match.configure(rules);
		const w::EntityHandle ace = match_player(world, 7, 1, "Ace");
		const w::EntityHandle bee = match_player(world, 3, 1, "Bee");
		world.match.record_death(world, bee, ace);
		world.match.record_death(world, ace, bee);
		world.process_round_end(0);
		const w::MatchResult &result = world.match.result();
		expect(result.draw, "every row at the top score is a draw");
		expect(result.players.size() == 2 &&
				result.players[0].identity.name == "Bee" &&
				result.players[1].identity.name == "Ace",
				"tied rows keep the slot-scan order");
		const EndRoundHeader header =
				inmatch::build_end_round_header(result, 7, /*non_team_form=*/true);
		const std::vector<uint8_t> wire = encode_end_round_header(header, true);
		EndRoundHeader decoded;
		expect(decode_end_round_header(wire.data(), wire.size(), true, decoded) &&
				decoded.draw == 1 && decoded.player_index == 1 &&
				decoded.player_scores[0] == 1 && decoded.player_scores[1] == 1,
				"the tied 0x1D form carries draw = 1");
		expect(world.match.player(ace)->stats[w::MatchStats::kRoundMarker] == 0 &&
				world.match.player(bee)->stats[w::MatchStats::kRoundMarker] == 0,
				"a tied board awards nobody");
	}
}

size_t expected_board_stream_size(const EndRoundStats &board, size_t configured) {
	size_t size = 1 + 2 + 2 + 1 + 2 * board.team_fields.size() + 1;
	for (const EndRoundPlayerRow &row : board.players) {
		size += 1 + (row.name.size() + 1) + (row.clan.size() + 1) +
				(row.tag.size() + 1) + 1 + 1 + 7 * 2 +
				2 * board.team_fields.size();
	}
	return size + 1 + board.team_rows.size() * configured * 2;
}

// A configured column with every player value at zero is left out of the
// declared columns and the player rows, but the trailing matrix still writes
// every CONFIGURED column per team row; the retail client reads the declared
// count per row, so only row 0 lands aligned.
// [orig: Server_BuildEndOfRoundScoreboard — the active flags @0x509398..0x5093db,
// the player-row gate @0x509534, the matrix @0x5095a0..0x5095cc;
// NapiNPClientMsg_0x056 @0x432174..0x4321a4]
void test_tdm_inactive_columns_keep_configured_matrix_width() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	rules.score_values.emplace();
	(*rules.score_values)[3] = 10;
	(*rules.score_values)[5] = -2;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	const w::EntityHandle red = match_player(world, 7, 2, "Red");
	world.match.record_death(world, red, blue);
	world.process_round_end(1);
	const w::MatchResult &result = world.match.result();
	const size_t configured = result.score_fields.size();
	expect(configured == 14, "TDM configures fourteen columns");

	const EndRoundStats board = inmatch::build_end_round_stats(result);
	const size_t active = board.team_fields.size();
	// One kill leaves points (19), kills (3), deaths (4) and the shots-per-kill
	// column (21, -1 for the killless victim) active; the other ten are zero.
	expect(active == 4 &&
			board.team_fields[0] == std::pair<uint8_t, uint8_t>{19, 1} &&
			board.team_fields[1] == std::pair<uint8_t, uint8_t>{3, 1} &&
			board.team_fields[2] == std::pair<uint8_t, uint8_t>{4, 1} &&
			board.team_fields[3] == std::pair<uint8_t, uint8_t>{21, 1},
			"only columns with a nonzero player value are declared");
	bool player_rows_active = !board.players.empty();
	for (const EndRoundPlayerRow &row : board.players)
		player_rows_active = player_rows_active && row.per_team.size() == active;
	expect(player_rows_active, "player rows carry the active columns only");
	bool matrix_configured = board.team_rows.size() == 3;
	for (const std::vector<int16_t> &row : board.team_rows)
		matrix_configured = matrix_configured && row.size() == configured;
	expect(matrix_configured,
			"every team row carries every configured column");
	expect(board.team_rows[1][0] == 10 && board.team_rows[1][1] == 1 &&
			board.team_rows[2][0] == -2 && board.team_rows[2][3] == 1 &&
			board.team_rows[1][5] == 0,
			"team rows resolve the configured columns in configured order");

	const std::vector<uint8_t> stream = encode_end_round_stats(board);
	expect(stream.size() == expected_board_stream_size(board, configured),
			"0x56 stream = header + active player columns + configured-width matrix");

	EndRoundStats decoded;
	expect(decode_end_round_stats(stream.data(), stream.size(), decoded) &&
			decoded.team_rows.size() == 3 &&
			decoded.team_rows[0].size() == active,
			"the retail client reads the declared count per team row");
	expect(std::equal(decoded.team_rows[0].begin(), decoded.team_rows[0].end(),
					board.team_rows[0].begin()),
			"team row 0 lands aligned");
	// The client's row 1 starts at word `active` of the producer's row 0 (a
	// zero of the neutral row), not at the producer's team-1 row (10 points).
	expect(decoded.team_rows[1][0] == board.team_rows[0][active] &&
			decoded.team_rows[1][0] == 0 && board.team_rows[1][0] == 10,
			"team row 1 begins inside the producer's row 0, as on a retail host");
}

// Team KOTH freezes five team rows at any team count and fills matrix column
// id 5 from the TeamRecord hold timer, not from the stats accessor.
// [orig: Server_BuildEndOfRoundScoreboard @0x50928c..0x50929c (count 5),
// @0x5092e5..0x5092e7 (the id-5 read), @0x508fa7 (the header team score)]
void test_tkoth_board_five_rows_and_hold_column() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(3, 4);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamKingOfTheHill;
	rules.game_time_minutes = 10;
	rules.hill_limit_minutes = 99;
	world.match.configure(rules);
	const w::EntityHandle blue_a = match_player(world, 3, 1, "BlueA");
	const w::EntityHandle blue_b = match_player(world, 4, 1, "BlueB");
	const w::EntityHandle red = match_player(world, 7, 2, "Red");
	world.registry.get(blue_a)->position = {0.0f, 0.0f, 0.0f};
	world.registry.get(blue_b)->position = {0.0f, 0.0f, 0.0f};
	world.registry.get(red)->position = {100.0f, 0.0f, 0.0f};
	w::Entity hill;
	hill.kind = w::EntityKind::Item;
	hill.item_id = 6006;
	hill.has_item_def = true;
	hill.position = {0.0f, 0.0f, 0.0f};
	hill.bound_radius = 10.0f;
	hill.alive = true;
	world.registry.spawn(3, hill);
	// Three one-second service passes: the first fires on the first tick.
	world.match.advance_tick(world);
	for (int i = 0; i < 2 * 62; ++i) world.match.advance_tick(world);
	expect(world.match.player(blue_a)->objective_ticks == 3 &&
			world.match.player(blue_b)->objective_ticks == 3,
			"both holders accumulate their own hill ticks");

	world.process_round_end(1);
	const w::MatchResult &result = world.match.result();
	expect(result.team_row_count == 5 && result.team_hold_ticks[1] == 3,
			"a two-team TKOTH freezes five rows and the team-1 hold timer");
	const EndRoundStats board = inmatch::build_end_round_stats(result);
	expect(board.team_score_0 == 3 && board.team_score_1 == 0,
			"the header team score is the hold timer, not the six-tick fold");
	const size_t configured = result.score_fields.size();
	size_t column_five = configured;
	for (size_t i = 0; i < configured; ++i)
		if (result.score_fields[i].field == 5) column_five = i;
	expect(configured == 18 && column_five == 1,
			"TKOTH configures column id 5 second");
	bool five_rows = board.team_rows.size() == 5;
	for (const std::vector<int16_t> &row : board.team_rows)
		five_rows = five_rows && row.size() == configured;
	expect(five_rows, "five configured-width team rows");
	expect(board.team_rows[1][column_five] == 3 &&
			board.team_rows[2][column_five] == 0 &&
			board.team_rows[0][column_five] == 0,
			"matrix column 5 is the team hold timer");
	expect(w::match_score_field_value(result.team_stats[1], 5, result.game_type) == 0,
			"the stats accessor would have read zero for the same column");
	// Rows 3 and 4 are the zero TeamRecords read through the same fill: every
	// configured column resolves through CPlayerStats_GetFieldByIndex over a
	// zero record — 0 everywhere except the shots-per-kill column (id 21),
	// whose zero-kills leg returns -1 — and the id-5 hold word reads 0.
	// [orig: Server_BuildEndOfRoundScoreboard fill @0x5092D0..0x509327;
	//  CPlayerStats_GetFieldByIndex case 21 @0x52d6fa..0x52d70b]
	bool tail_rows_zero_records = true;
	for (size_t row = 3; row < 5; ++row) {
		for (size_t i = 0; i < configured; ++i) {
			const uint8_t field = result.score_fields[i].field;
			const int16_t expected_word = field == 5
					? int16_t{0}
					: static_cast<int16_t>(w::match_score_field_value(
							  w::MatchStats{}, field, result.game_type));
			tail_rows_zero_records = tail_rows_zero_records &&
					board.team_rows[row][i] == expected_word;
		}
	}
	expect(tail_rows_zero_records && board.team_rows[3][configured - 1] == -1 &&
					board.team_rows[4][configured - 1] == -1,
			"rows 3 and 4 of a two-team TKOTH are the zero team records read through "
			"the accessor (-1 shots per kill)");
	const std::vector<uint8_t> stream = encode_end_round_stats(board);
	expect(stream.size() == expected_board_stream_size(board, configured),
			"the TKOTH 0x56 stream carries five configured-width rows");
}

void test_demolition_death_routes_score_and_round_wire() {
	for (const uint32_t game_type : {
			game_type::kSearchAndDestroy, game_type::kAttackDefend}) {
		w::World world;
		world.registry.configure_pool(0, 4);
		world.registry.configure_pool(2, 4);
		world.rules.mp_session = true;
		w::MatchRules rules;
		rules.game_type = game_type;
		rules.game_time_minutes = 1;
		world.match.configure(rules);
		const w::EntityHandle attacker = match_player(world, 3, 1, "Blue");

		w::Entity objective;
		objective.kind = w::EntityKind::Building;
		objective.team = 2;
		objective.health = 0;
		objective.alive = true;
		objective.has_item_def = true;
		objective.item_attrib = w::kItemAttribObjectiveTarget;
		const w::EntityHandle target = world.registry.spawn(2, objective);
		expect(target.valid(), "demolition objective fixture spawns");

		ns::LoopbackChannel wire;
		inmatch::NapiNPServerCtx ctx;
		ctx.world = &world;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		ctx.config.game_type = game_type;
		ctx.np_protocol.connection_list.push_back(
				make_conn(1, 1, &wire, ns::TransportMode::Client,
						attacker, true));
		ready_mp_connection(ctx.np_protocol.connection_list[0], 3);

		// Projectile and blast damage both stage this same transport-free death
		// record. The host consumes it only after Match has frozen the authored
		// objective census for the frame. [orig: Entity_ApplyWeaponDamage
		// @0x4E6FB4; GameEvent_ProcessScoring case 11 @0x52F550;
		// Server_CheckWinConditions demolition arm @0x51B18B]
		inmatch::Server_TickUpdate(ctx); // first-frame one-second service
		push_death(world, target, attacker);
		inmatch::Server_TickUpdate(ctx);
		const w::MatchPlayer *scorer = world.match.player(attacker);
		expect(scorer != nullptr &&
				scorer->stats[w::MatchStats::kTargetsDestroyed] == 1 &&
				scorer->stats[w::MatchStats::kPoints] == 50 &&
				world.match.team_stats(1)[w::MatchStats::kTargetsDestroyed] == 1,
			"S&D/A&D death routing awards the exact target event to player and team");
		// The organic death transaction never runs for the target: its class
		// callback owns its Flags and its S2C 0x26 kill record.
		// [orig: Entity_CheckAndProcessDeath @0x51B550, called only from
		//  Entity_UpdateInfantryPlayerBody @0x4B4CEA, Entity_UpdateInfantryAI
		//  @0x4B9D4D and the console kill @0x4D29EC; the only 0x13 sends are
		//  GameEvent_PlayerDeath @0x516E8E and Entity_CheckAndProcessDeath
		//  @0x51B58F]
		const w::Entity *dead_target = world.registry.get(target);
		expect(dead_target != nullptr && dead_target->alive &&
				(dead_target->flags & w::kEntityFlagDead) == 0,
			"the organic death route leaves the demolition target to its class callback");

		bool saw_death = false;
		ns::Datagram datagram;
		while (wire.client_recv(datagram)) {
			if (datagram.tag != s2c::ENTITY_DEATH) continue;
			EntityDeathRecord death;
			size_t consumed = 0;
			if (decode_entity_death(datagram.body.data(), datagram.body.size(),
					death, consumed) && consumed == datagram.body.size() &&
					death.entity_handle == target.packed)
				saw_death = true;
		}
		expect(!saw_death, "a demolition target death never fans the organic S2C 0x13");

		for (int i = 0; i < 60; ++i) inmatch::Server_TickUpdate(ctx);
		expect(!world.match.outcome().ended,
				"demolition win waits for the 1 Hz win-condition pass");
		wire.clear();
		inmatch::Server_TickUpdate(ctx); // the next one-second win-condition boundary
		expect(world.match.outcome().ended &&
				world.match.outcome().winner_team == 1,
			"S&D/A&D complete authored target census ends for the attacker team");

		EndRoundHeader header;
		expect(drain_round_header(wire, header) &&
				header.winner_team == 1 && header.team_score_0 == 1 &&
				header.team_score_1 == 0,
			"S&D/A&D target win reaches exact 0x61/0x1D round wire and scores");
	}
}

void test_aas_round_wire() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = game_type::kAdvanceAndSecure;
	world.match.configure(rules);
	const w::EntityHandle red = match_player(world, 4, 2, "Red");
	w::Entity z1;
	z1.kind = w::EntityKind::Item;
	z1.team = 2;
	z1.zone_number = 1;
	w::Entity z2 = z1;
	z2.zone_number = 2;
	world.zones.chain.zones.push_back(world.registry.spawn(1, z1));
	world.zones.chain.zones.push_back(world.registry.spawn(1, z2));

	ns::LoopbackChannel wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kAdvanceAndSecure;
	ctx.np_protocol.connection_list.push_back(
			make_conn(3, 1, &wire, ns::TransportMode::Client, red, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 4);
	// Every zone is already owned, so the first-frame service ends the round.
	inmatch::Server_TickUpdate(ctx);

	EndRoundHeader header;
	expect(drain_round_header(wire, header),
			"A&S uses the shared 0x61/0x1D transition");
	expect(header.winner_team == 2 && header.team_score_0 == 0 &&
			header.team_score_1 == 2,
			"A&S header preserves the winner and owned-zone scores");
	expect(ctx.round_end_linger_ticks == 2790,
			"automatic A&S outcome does not consume its announcement tick");
}

void test_coop_script_producers_share_round_wire() {
	auto run_case = [](uint32_t game_type_code, bool use_wac) {
		w::World world;
		world.registry.configure_pool(0, 4);
		world.rules.mp_session = true;
		w::MatchRules rules;
		rules.game_type = game_type_code;
		world.match.configure(rules);
		const w::EntityHandle red = match_player(world, 4, 2, "Red");

		opennova::wac::WacSystem wac_system;
		opennova::mission::BmsEventSystem bms_system;
		int producer_tick = 0;
		if (use_wac) {
			opennova::wac::CompileEnv env;
			auto program = opennova::wac::compile_source(
					"if never() then win(2) endif\n", env);
			expect(program.ok(), "stock Co-op WAC win program compiles");
			wac_system.set_program(std::move(program));
			world.add_system(&wac_system);
			producer_tick = opennova::wac::WacSystem::kTicksPerExecution;
		} else {
			opennova::bms::Event event{};
			event.action_count = 1;
			opennova::bms::Action action{};
			action.action_type = opennova::bms::ActionType::RedWin;
			bms_system.load({event}, {}, {action});
			world.add_system(&bms_system);
			producer_tick = 16;
		}
		world.load_systems();

		ns::LoopbackChannel wire;
		inmatch::NapiNPServerCtx ctx;
		ctx.world = &world;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		ctx.config.game_type = game_type_code;
		ctx.np_protocol.connection_list.push_back(
				make_conn(3, 1, &wire, ns::TransportMode::Client, red, true));
		ready_mp_connection(ctx.np_protocol.connection_list[0], 4);

		for (int tick = 1; tick < producer_tick; ++tick)
			inmatch::Server_TickUpdate(ctx);
		expect(!world.match.outcome().ended,
				"Co-op does not end before its authored script action");
		wire.clear();
		inmatch::Server_TickUpdate(ctx);
		expect(world.match.outcome().ended &&
				world.match.outcome().winner_team == 2,
				"WAC/BMS action owns the Co-op result edge");

		EndRoundHeader header;
		expect(drain_round_header(wire, header) && header.winner_team == 2,
				"scripted Co-op result reaches exact 0x61/0x1D round wire");
		expect(ctx.round_end_linger_ticks == 2789,
				"scripted Co-op outcome consumes its originating server tick");
	};

	// Stock Co-op is WAC-owned; Objective Co-op exercises the sibling BMS
	// result action. Both front ends call the same retail round transaction.
	// [orig: WacAction_Win @0x4ED4A0; EventAction_Dispatch "RedWin" ->
	// Server_ProcessRoundEnd call @0x454495; Server_ProcessRoundEnd @0x5164F0]
	run_case(game_type::kCoop, true);
	run_case(game_type::kObjectiveCoop, false);
}

void test_aas_capture_events_carry_zone_numbers() {
	w::World world;
	w::CollisionWorld collision;
	w::AiSystem &ai = world.ai;
	install_collision_system(world, collision);
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	world.registry.configure_pool(2, 4);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = 0x10010u;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	world.registry.get(blue)->player_class = 8;
	world.registry.get(blue)->position = {100.0f, 0.0f, 0.0f};
	world.registry.get(blue)->net_move_input |= w::Entity::kMoveOrderMoving;

	// A sorted pool-2 spawn object precedes the three pool-1 capture zones. The
	// target zone is therefore spawn-registry index 2, zone-chain index 1 and
	// zone NUMBER 4: the flip pair carries the number, the banner the capturer.
	w::Entity base_spawn;
	base_spawn.kind = w::EntityKind::Building;
	base_spawn.team = 1;
	base_spawn.is_spawn_point = true;
	base_spawn.alive = true;
	world.registry.spawn(2, base_spawn);
	auto spawn_zone = [&](uint8_t number, uint8_t team, float x) {
		w::Entity zone;
		zone.kind = w::EntityKind::Item;
		zone.team = team;
		zone.zone_number = number;
		zone.zone_radius = 70;
		zone.position = {x, 0.0f, 0.0f};
		zone.yaw = 90;
		zone.is_capture_trigger = true;
		zone.is_spawn_point = true;
		zone.alive = true;
		return world.registry.spawn(1, zone);
	};
	spawn_zone(3, 1, -100.0f);
	const w::EntityHandle target = spawn_zone(4, 0, 100.0f);
	spawn_zone(5, 2, 300.0f);
	world.zones.build_chain_from_mission();
	world.zones.latch_control();
	const int32_t capture_model = collision.add_model(
		contact_box(w::bvol_type::kChangeTeamCT));
	collision.assign_entity(target, capture_model);
	expect(attach_remote_body(world, ai, blue, 100.0f) != nullptr,
			"A&S authority body fixture attaches");
	prime_collision_tables(world, collision);

	ns::LoopbackChannel wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &wire, ns::TransportMode::Client, blue, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);
	// The body already stands in the box: the first frame's entity update
	// records the contact after that frame's one-second service, the next
	// server tick turns it into a request, and the next one-second service
	// flips the numbered zone. [orig: Server_TickUpdate — the periodic block
	//  @0x51DB6D..0x51E1B2 (the Server_UpdateCaptureZones call @0x51DF87)
	//  precedes Game_ProcessMainFrame's Entity_UpdateAllEntities call @0x52674B]
	for (int tick = 0; tick < 63; ++tick) inmatch::Server_TickUpdate(ctx);

	// Blue's own team gets 53 [zone number 4][team 1's new frontier 5] (the
	// enemy mask never held number 4), then the 56 banner [Blue's pool-0 index].
	// [orig: GameEvent_FlagCapture @0x50F6F0 — GetZoneInfo @0x50F781, the 53
	//  build @0x50F82B, the banner @0x50F98C]
	bool saw_pair = false;
	bool saw_banner = false;
	ns::Datagram datagram;
	while (wire.client_recv(datagram)) {
		if (datagram.tag != 0x1E || datagram.body.size() != 8) continue;
		const uint8_t event = datagram.body[0];
		if (event >= 50 && event <= 53) {
			saw_pair = true;
			expect(event == 53 && datagram.body[1] == 4 && datagram.body[2] == 5 &&
					datagram.body[3] == 0xFF,
					"A&S 0x1E pair carries the zone number and the frontier");
		} else if (event == 56 || event == 57) {
			saw_banner = true;
			expect(event == 56 && datagram.body[1] == uint8_t(blue.slot()) &&
					datagram.body[2] == 0xFF && datagram.body[3] == 0xFF,
					"A&S 0x1E banner carries the capturer's pool-0 index");
		}
	}
	expect(world.registry.get(target)->team == 1 && saw_pair && saw_banner,
			"A&S authority flips the target and emits its capture event family");
}

void test_ctf_pickup_and_capture_wire_transaction() {
	w::World world;
	w::CollisionWorld collision;
	w::AiSystem &ai = world.ai;
	install_collision_system(world, collision);
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = game_type::kCaptureTheFlag;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	w::Entity *blue_entity = world.registry.get(blue);
	const w::Vec3 flag_position{10.75f, -3.25f, 3.0f};
	const w::Vec3 bay_position{19.75f, -3.25f, 3.0f};
	blue_entity->position = flag_position;
	blue_entity->net_move_input |= w::Entity::kMoveOrderMoving;
	const w::EntityHandle host = match_player(world, 1, 1, "Host");
	world.registry.get(host)->position = {1000.0f, 1000.0f, 3.0f};

	w::Entity red_flag;
	red_flag.kind = w::EntityKind::Item;
	red_flag.item_id = 4093; // Flag (Red) [orig: item-id branch @0x43C1B7]
	red_flag.has_item_def = true;
	red_flag.item_attrib = w::kItemAttribMoveCallback;
	red_flag.position = flag_position;
	red_flag.spawn_position = red_flag.position;
	red_flag.yaw = 90;
	const w::EntityHandle flag = world.registry.spawn(1, red_flag);
	w::Entity blue_bay;
	blue_bay.kind = w::EntityKind::Item;
	blue_bay.item_id = 4098; // Blue bay [orig: Entity_ProcessWaypointInteraction @0x4AD8D4]
	blue_bay.has_item_def = true;
	blue_bay.item_attrib = w::kItemAttribMoveCallback;
	blue_bay.position = bay_position;
	blue_bay.yaw = 90;
	const w::EntityHandle bay = world.registry.spawn(1, blue_bay);
	expect(flag.valid() && bay.valid(), "CTF objective fixtures spawn");
	const int32_t waypoint_model = collision.add_model(contact_box(1));
	collision.assign_entity(flag, waypoint_model);
	collision.assign_entity(bay, waypoint_model);
	expect(attach_remote_body(world, ai, blue, flag_position.x + 3.5f) != nullptr,
			"CTF authority body fixture attaches");
	prime_collision_tables(world, collision);

	ns::LoopbackChannel wire;
	ns::LoopbackChannel host_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &wire, ns::TransportMode::Client, blue, true));
	ctx.np_protocol.connection_list.push_back(
			make_conn(2, 2, &host_wire, ns::TransportMode::Loopback, host, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);
	ready_mp_connection(ctx.np_protocol.connection_list[1], 1);

	inmatch::Server_TickUpdate(ctx); // contact -> pickup
	inmatch::Server_TickUpdate(ctx); // the pickup's records lead this queue
	bool saw_pickup_event = false;
	bool saw_pickup = false;
	int pickup_event_order = -1;
	int pickup_state_order = -1;
	int order = 0;
	ns::Datagram datagram;
	while (wire.client_recv(datagram)) {
		if (datagram.tag == s2c::GAME_EVENT &&
				datagram.body == std::vector<uint8_t>{
						0x14, static_cast<uint8_t>(blue.slot()),
						0xFF, 0xFF, 10, 0, 0xFC, 0xFF}) {
			saw_pickup_event = true;
			pickup_event_order = order;
		}
		if (datagram.tag == s2c::OBJECTIVE_ENTITY_STATE) {
			ObjectiveEntityState state;
			size_t consumed = 0;
			if (decode_objective_entity_state(
					datagram.body.data(), datagram.body.size(), state, consumed) &&
					consumed == datagram.body.size() && state.entity_handle == flag.packed &&
					state.attach_handle == blue.packed && (state.flags_byte & 1u) != 0) {
				saw_pickup = true;
				pickup_state_order = order;
			}
		}
		++order;
	}
	expect(saw_pickup_event && saw_pickup && pickup_event_order >= 0 &&
			pickup_state_order > pickup_event_order,
			"CTF pickup fans exact event 20 before the 19-byte 0x2F carried state");
	bool host_saw_pickup_event = false;
	bool host_saw_pickup = false;
	int host_pickup_event_order = -1;
	int host_pickup_state_order = -1;
	order = 0;
	while (host_wire.client_recv(datagram)) {
		if (datagram.tag == s2c::GAME_EVENT &&
				datagram.body == std::vector<uint8_t>{
						0x14, static_cast<uint8_t>(blue.slot()),
						0xFF, 0xFF, 10, 0, 0xFC, 0xFF}) {
			host_saw_pickup_event = true;
			host_pickup_event_order = order;
		}
		if (datagram.tag == s2c::OBJECTIVE_ENTITY_STATE) {
			ObjectiveEntityState state;
			size_t consumed = 0;
			if (decode_objective_entity_state(
					datagram.body.data(), datagram.body.size(), state, consumed) &&
					consumed == datagram.body.size() && state.entity_handle == flag.packed &&
					state.attach_handle == blue.packed && (state.flags_byte & 1u) != 0) {
				host_saw_pickup = true;
				host_pickup_state_order = order;
			}
		}
		++order;
	}
	expect(host_saw_pickup_event && host_saw_pickup &&
			host_pickup_event_order >= 0 &&
			host_pickup_state_order > host_pickup_event_order,
			"CTF pickup mask 0x80 includes the host with event-before-state ordering");

	move_remote_body(world, ai, blue, bay_position);
	inmatch::Server_TickUpdate(ctx); // carried flag contacts bay -> capture
	inmatch::Server_TickUpdate(ctx); // the capture's records lead this queue
	bool saw_capture_event = false;
	bool saw_remove = false;
	bool saw_reset = false;
	int capture_order = -1;
	int remove_order = -1;
	order = 0;
	while (wire.client_recv(datagram)) {
		if (datagram.tag == s2c::GAME_EVENT && datagram.body.size() == 8 &&
				datagram.body[0] == 0x13 && datagram.body[1] == blue.slot()) {
			saw_capture_event = true;
			capture_order = order;
		}
		if (datagram.tag == s2c::ENTITY_REMOVE) {
			EntityRemove removal;
			size_t consumed = 0;
			if (decode_entity_remove(datagram.body.data(), datagram.body.size(),
					removal, consumed) && removal.entity_handle == flag.packed) {
				saw_remove = true;
				remove_order = order;
			}
		}
		if (datagram.tag == s2c::OBJECTIVE_ENTITY_STATE) saw_reset = true;
		++order;
	}
	expect(saw_capture_event && saw_remove && !saw_reset &&
			capture_order >= 0 && remove_order > capture_order,
			"CTF capture fans event 19 then 0x12 removal, never a reset 0x2F");
	bool host_saw_capture_event = false;
	bool host_saw_remove = false;
	int host_capture_order = -1;
	int host_remove_order = -1;
	order = 0;
	while (host_wire.client_recv(datagram)) {
		if (datagram.tag == s2c::GAME_EVENT && datagram.body.size() == 8 &&
				datagram.body[0] == 0x13 && datagram.body[1] == blue.slot()) {
			host_saw_capture_event = true;
			host_capture_order = order;
		}
		if (datagram.tag == s2c::ENTITY_REMOVE) {
			EntityRemove removal;
			size_t consumed = 0;
			if (decode_entity_remove(datagram.body.data(), datagram.body.size(),
					removal, consumed) && removal.entity_handle == flag.packed) {
				host_saw_remove = true;
				host_remove_order = order;
			}
		}
		++order;
	}
	expect(host_saw_capture_event && !host_saw_remove &&
			host_capture_order >= 0 && host_remove_order < 0,
			"CTF capture sends the 0x80 event to the host but its 0x90 removal only remotely");
}

void test_flag_timeout_wire_transaction() {
	w::World world;
	w::CollisionWorld collision;
	w::AiSystem &ai = world.ai;
	install_collision_system(world, collision);
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = game_type::kFlagBall;
	rules.max_score = 99;
	rules.flag_return_ticks = 5;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	w::Entity *blue_entity = world.registry.get(blue);
	const w::Vec3 flag_position{30.0f, 40.0f, 2.0f};
	blue_entity->position = {flag_position.x + 1.6f,
			flag_position.y, flag_position.z};
	blue_entity->net_move_input |= w::Entity::kMoveOrderMoving;
	const w::EntityHandle host = match_player(world, 1, 1, "Host");
	world.registry.get(host)->position = {1000.0f, 1000.0f, 2.0f};

	w::Entity red_flag;
	red_flag.kind = w::EntityKind::Item;
	red_flag.item_id = 4093;
	w::ItemDeathTraits flag_traits;
	flag_traits.death_class = w::ItemDeathClass::kFlag;
	world.tables.item_death_traits.set(red_flag.item_id, flag_traits);
	red_flag.has_item_def = true;
	red_flag.item_attrib = w::kItemAttribMoveCallback;
	red_flag.position = flag_position;
	red_flag.spawn_position = {5.0f, 6.0f, 2.0f};
	red_flag.yaw = 90;
	const w::EntityHandle flag = world.registry.spawn(1, red_flag);
	expect(flag.valid(), "FlagBall timeout fixture spawns its red flag");
	const int32_t waypoint_model = collision.add_model(contact_box(1));
	collision.assign_entity(flag, waypoint_model);
	expect(attach_remote_body(world, ai, blue, flag_position.x + 3.5f) != nullptr,
			"FlagBall authority body fixture attaches");
	prime_collision_tables(world, collision);

	ns::LoopbackChannel remote_wire;
	ns::LoopbackChannel host_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	// Keep the dead remote slot active past the ordinary 360-tick redeploy punt.
	ctx.config.permanent_death = true;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &remote_wire, ns::TransportMode::Client, blue, true));
	ctx.np_protocol.connection_list.push_back(
			make_conn(2, 2, &host_wire, ns::TransportMode::Loopback, host, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);
	ready_mp_connection(ctx.np_protocol.connection_list[1], 1);

	inmatch::Server_TickUpdate(ctx); // pickup arms the return state
	remote_wire.clear();
	host_wire.clear();
	world.match.record_death(world, blue); // drop without waiting for combat routing
	blue_entity = world.registry.get(blue);
	blue_entity->alive = false;
	blue_entity->flags |= w::kEntityFlagDead;
	blue_entity->net_move_input = 0;
	inmatch::Server_TickUpdate(ctx); // route the drop
	remote_wire.clear();
	host_wire.clear();

	// The first post-drop visit observes the changed XY and re-arms five
	// seconds. Thereafter the flag callback expires on its 62-tick cadence.
	for (int tick = 0; tick < 370; ++tick)
		inmatch::Server_TickUpdate(ctx);
	expect(world.registry.get(flag)->position.x != 5.0f,
			"the flag stays dropped until the sixth class visit after pickup");
	inmatch::Server_TickUpdate(ctx);
	inmatch::Server_TickUpdate(ctx); // the return's records lead this queue

	auto saw_exact_return = [&](ns::LoopbackChannel &channel) {
		bool saw_event = false;
		bool saw_state = false;
		int event_order = -1;
		int state_order = -1;
		int order = 0;
		ns::Datagram datagram;
		while (channel.client_recv(datagram)) {
			if (datagram.tag == s2c::GAME_EVENT &&
					datagram.body == std::vector<uint8_t>{
							0x24, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0}) {
				saw_event = true;
				event_order = order;
			}
			if (datagram.tag == s2c::OBJECTIVE_ENTITY_STATE) {
				ObjectiveEntityState state;
				size_t consumed = 0;
				if (decode_objective_entity_state(
						datagram.body.data(), datagram.body.size(), state, consumed) &&
						consumed == datagram.body.size() &&
						state.entity_handle == flag.packed &&
						state.attach_handle == 0xFFFF &&
						state.pos_x == 5 * 65536 && state.pos_y == 6 * 65536) {
					saw_state = true;
					state_order = order;
				}
			}
			++order;
		}
		return saw_event && saw_state && event_order >= 0 && state_order > event_order;
	};
	expect(saw_exact_return(remote_wire),
			"red-flag timeout fans event 36 before the exact home-state 0x2F");
	expect(saw_exact_return(host_wire),
			"flag-timeout mask 0x80 includes the host with event-before-state ordering");
}

} // namespace

// D-NET-406: the SP auto-lose reads the local player's dead bit alone. A body
// whose `alive` latch dropped with its health while the bit stays clear keeps
// the round; the bit ends it, winner 2.
// [orig: Server_CheckWinConditions @0x51AD5E, Server_ProcessRoundEnd(2) @0x51AD77]
void test_sp_auto_lose_reads_the_dead_bit() {
	w::World world;
	world.registry.configure_pool(0, 4);
	w::Entity body;
	body.kind = w::EntityKind::Organic;
	body.team = 1;
	body.flags = w::kEntityFlagPlayer;
	body.alive = false;
	body.health = -1;
	const w::EntityHandle player = world.registry.spawn(0, body);
	world.cached.local_player = player;
	ns::LoopbackChannel loop;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 0;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 2, &loop, ns::TransportMode::Loopback, player, true));
	for (int i = 0; i < 63; ++i) inmatch::Server_TickUpdate(ctx); // past a 1 Hz check
	expect(!world.match.outcome().ended,
			"a clear dead bit keeps the SP round whatever the health and the alive latch");
	world.registry.get(player)->flags |= w::kEntityFlagDead;
	for (int i = 0; i < 63; ++i) inmatch::Server_TickUpdate(ctx);
	expect(world.match.outcome().ended && world.match.outcome().winner_team == 2,
			"the dead bit alone ends it, winner 2");
}

// The 0x56 row's sixth i16 is stats field 11, the FLAGSAVE counter
// [orig: CRenderState_GetFieldByIndex(stats, 0xA) @0x50918a -> the seventh
// sub_455540 word @0x509509; field 11 = GameEvent_ProcessScoring case 8
// @0x52f8d7 = score.ini VAR slot 9 "FLAGSAVE"]. The decoder's "flags" name
// is that field; nothing in the record is an assist count.
void test_end_round_row_flags_word_is_field_11() {
	w::MatchResult result;
	result.ready = true;
	result.game_type = opennova::game_type::kCaptureTheFlag;
	result.score_fields = w::default_match_score_fields(result.game_type);
	w::MatchResultPlayer p;
	p.identity.slot = 3;
	p.identity.name = "Blue";
	p.team = 1;
	p.stats[w::MatchStats::kFlagSaves] = 7;
	p.stats[w::MatchStats::kDeaths] = 2;
	result.players.push_back(p);
	const EndRoundStats board = inmatch::build_end_round_stats(result);
	expect(board.players.size() == 1 && board.players[0].slot == 3 &&
			board.players[0].flags == 7 && board.players[0].captures == 2,
			"0x56 row: the sixth word carries field 11 (FLAGSAVE), the fifth field 7 (deaths)");
}

// The S2C 0x13 records for `victim` a drained channel carried.
int entity_death_records(ns::LoopbackChannel &wire, w::EntityHandle victim) {
	int n = 0;
	ns::Datagram datagram;
	while (wire.client_recv(datagram)) {
		if (datagram.tag != s2c::ENTITY_DEATH) continue;
		EntityDeathRecord death;
		size_t consumed = 0;
		if (decode_entity_death(datagram.body.data(), datagram.body.size(), death,
				consumed) && consumed == datagram.body.size() &&
				death.entity_handle == victim.packed)
			++n;
	}
	return n;
}

// An org1 (NPC) body's death transaction is its own motor edge's, once per
// life. A damage-time record only feeds the SP kill tally; the edge raises the
// one 0x13, a script health write with no record reaches it too, and a
// scripted kill adds no second one. An edge record still fans after its corpse
// leg removed the row in the same pass, and never re-latches a row it no
// longer owns. [orig: Entity_UpdateInfantryAI @0x4B9D44..0x4B9D4D ->
// Entity_CheckAndProcessDeath @0x51B550 (0x13 @0x51B58F); Score_ProcessKillEvent
// @0x4FD400 is called from the damage paths only (@0x4E8133)]
void test_org1_death_transaction_is_the_motor_edge() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::PlayerSpawn host_spawn;
	host_spawn.position = {0.0f, 0.0f, 10.0f};
	host_spawn.team = 1;
	host_spawn.net_id = 0xFFF0;
	const w::EntityHandle host = w::spawn_player(world, host_spawn);
	world.cached.local_player = host;
	w::PlayerSpawn peer_spawn = host_spawn;
	peer_spawn.position = {4.0f, 0.0f, 10.0f};
	peer_spawn.net_id = 0xFFF1;
	const w::EntityHandle peer = w::spawn_remote_player(world, peer_spawn);

	// The SP listen host is a session: its loopback plus one remote peer that
	// sees the 0x13 fan (mask 0x90 leaves the loopback out).
	ns::LoopbackChannel host_loop;
	ns::LoopbackChannel peer_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 2, &host_loop, ns::TransportMode::Loopback, host, true));
	ctx.np_protocol.connection_list.push_back(
			make_conn(2, 1, &peer_wire, ns::TransportMode::Client, peer, true));
	ready_mp_connection(ctx.np_protocol.connection_list[1], 1);

	auto spawn_org1 = [&](uint16_t net_id, int32_t deathtime_ticks) {
		w::Entity seed;
		seed.kind = w::EntityKind::Organic;
		seed.net_id = net_id;
		seed.team = 2;
		seed.group_id = 7;
		seed.alive = true;
		seed.health = 100;
		seed.health_max = 100;
		seed.deathtime_ticks = deathtime_ticks;
		// A scored NPC person definition (items.def `score`, def+0x194), which
		// the kill tally requires of its victim [orig: @0x4FD422].
		seed.has_item_def = true;
		seed.item_score = 10;
		seed.position = {static_cast<float>(net_id - 290), 20.0f, 0.0f};
		const w::EntityHandle h = world.registry.spawn(0, seed);
		w::AiEntity &body = *world.ai.at(world.ai.attach(h));
		body.inf.active = true;
		body.health = 100;
		body.net_id = net_id;
		return h;
	};
	auto run_ticks = [&](int n) {
		for (int i = 0; i < n; ++i) inmatch::Server_TickUpdate(ctx);
	};
	auto dead_bit = [&](w::EntityHandle h) {
		const w::Entity *e = world.registry.get(h);
		return e != nullptr && ((e->flags | e->engine_flags) & w::kEntityFlagDead) != 0;
	};
	run_ticks(2);
	peer_wire.clear();

	// 1. A killing hit's record tallies once; the edge's own record raises the
	// one 0x13 and latches the dead bit.
	const w::EntityHandle shot = spawn_org1(300, 500);
	world.registry.get(shot)->health = 0;
	world.registry.get(shot)->last_attacker = host;
	push_kill(world, shot, host);
	run_ticks(3);
	expect(entity_death_records(peer_wire, shot) == 1,
			"org1 kill: one 0x13, raised by the motor edge");
	expect(world.kill_stats.enemy_kills_by_player() == 1,
			"org1 kill: the damage-time record tallies once, the edge record never");
	expect(dead_bit(shot), "org1 kill: the edge latches the dead bit");

	// 2. A script health write raises no record; the edge still sends the 0x13.
	const w::EntityHandle written = spawn_org1(301, 500);
	world.commands.set_entity_health(written, 0);
	run_ticks(3);
	expect(entity_death_records(peer_wire, written) == 1,
			"org1 health write: the motor edge sends the 0x13");
	expect(world.kill_stats.enemy_kills_by_player() == 1 &&
			world.kill_stats.enemy_kills_by_others == 0,
			"org1 health write: no kill tally");

	// 3. A scripted group kill adds no second transaction.
	const w::EntityHandle scripted = spawn_org1(302, 500);
	world.commands.kill_group(7);
	run_ticks(3);
	expect(entity_death_records(peer_wire, scripted) == 1,
			"org1 scripted kill: exactly one 0x13");
	expect(world.kill_stats.enemy_kills_by_others == 0, "org1 scripted kill: no kill tally");

	// 4. A corpse its leg removes on the edge pass still fans its 0x13. (No local
	// player watches it here: an SP corpse in view is held.)
	const w::EntityHandle gone = spawn_org1(303, 0);
	world.cached.local_player = {};
	world.commands.set_entity_health(gone, 0);
	run_ticks(2); // the edge pass, then the tick whose queue its record leads
	world.cached.local_player = host;
	expect(world.registry.get(gone) == nullptr, "a zero deathtime corpse leaves on its edge pass");
	expect(entity_death_records(peer_wire, gone) == 1,
			"the removed corpse's edge record still sends its 0x13");

	// 5. An edge record whose row lives again (a respawn in the same corpse pass)
	// leaves the new life's flags alone.
	const w::EntityHandle reborn = spawn_org1(304, 500);
	w::RoundDeath edge;
	edge.victim = reborn;
	edge.victim_handle = reborn.packed;
	edge.killer_handle = 0xFFFFu;
	edge.motor_edge = true;
	world.round_sim.deaths.push_back(edge);
	run_ticks(1);
	expect(!dead_bit(reborn) && world.registry.get(reborn)->alive,
			"an edge record never re-latches the row it no longer owns");
}

// Scorer event 12 rides the kill accounting in every session: a lethal-edge
// kill of a scored victim adds its `score` word to the killer's field 30, the
// 0x56 row's third word; a death with no kill event adds nothing.
// [orig: Score_ProcessKillEvent @0x4FD400 (the event-12 call @0x4FD438, ahead
// of the session test @0x4FD440); Server_BuildEndOfRoundScoreboard @0x508F30]
void test_kill_event_unit_score_rides_every_session() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.rules.mp_session = true;
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	rules.score_values.emplace();
	(*rules.score_values)[3] = 10;
	world.match.configure(rules);
	const w::EntityHandle ace = match_player(world, 3, 1, "Ace");
	w::Entity npc;
	npc.kind = w::EntityKind::Organic;
	npc.has_item_def = true;
	npc.item_type = 3;
	npc.item_score = 25;
	npc.team = 2;
	npc.alive = true;
	npc.health = 100;
	const w::EntityHandle scored = world.registry.spawn(0, npc);
	const w::EntityHandle scripted = world.registry.spawn(0, npc);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	inmatch::Server_TickUpdate(ctx);
	push_kill(world, scored, ace);
	push_death(world, scripted, ace);
	inmatch::Server_TickUpdate(ctx);
	const w::MatchPlayer *row = world.match.player(ace);
	expect(row != nullptr && row->stats[w::MatchStats::kUnitScore] == 25 &&
			row->stats[w::MatchStats::kEnemyKills] == 2 &&
			world.match.team_stats(1)[w::MatchStats::kUnitScore] == 25,
			"only the kill event adds the victim's score to field 30");
	expect(world.kill_stats.enemy_kills_by_player() == 0 &&
			world.kill_stats.enemy_kills_by_others == 0,
			"the session skips the SP tallies");
	world.process_round_end(1);
	const EndRoundStats board = inmatch::build_end_round_stats(world.match.result());
	expect(board.players.size() == 1 && board.players[0].assists == 25,
			"the 0x56 row's third word carries field 30");
}

int main() {
	test_sp_auto_lose_reads_the_dead_bit();
	test_end_round_row_flags_word_is_field_11();
	test_org1_death_transaction_is_the_motor_edge();
	test_kill_event_unit_score_rides_every_session();
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem &ai = world.ai;

	// The host player + NPC victims across the witnessed classification axes.
	w::PlayerSpawn ps;
	ps.position = {0.0f, 0.0f, 10.0f};
	ps.team = 1;
	ps.net_id = 0xFFF0;
	const w::EntityHandle player = w::spawn_player(world, ps);
	if (!expect(player.valid(), "host player spawned")) return 1;
	world.cached.local_player = player;

	// Every NPC carries a scored definition (the shipped soldiers author
	// `score 10`); persons are ItemDef type 3.
	auto spawn_npc = [&](uint16_t net_id, uint8_t team, w::EntityKind kind) {
		w::Entity e;
		e.net_id = net_id;
		e.team = team;
		e.kind = kind;
		e.has_item_def = true;
		e.item_type = kind == w::EntityKind::Organic ? 3 : 1;
		e.item_score = 10;
		e.alive = true;
		e.health = 100;
		return world.registry.spawn(0, e);
	};
	const w::EntityHandle green_person = spawn_npc(100, 0, w::EntityKind::Organic);
	const w::EntityHandle blue_person = spawn_npc(101, 1, w::EntityKind::Organic);
	const w::EntityHandle red_person = spawn_npc(102, 2, w::EntityKind::Organic);
	const w::EntityHandle green_item = spawn_npc(103, 0, w::EntityKind::Item);
	const w::EntityHandle green_person2 = spawn_npc(104, 0, w::EntityKind::Organic);
	const w::EntityHandle blue_person2 = spawn_npc(105, 1, w::EntityKind::Organic);
	const w::EntityHandle blue_unscored = spawn_npc(106, 1, w::EntityKind::Organic);
	world.registry.get(blue_unscored)->item_score = 0;
	const w::EntityHandle blue_item = spawn_npc(107, 1, w::EntityKind::Item);

	ns::LoopbackChannel loop;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 0; // SP: the tallies + the auto-lose leg are SP-only
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 2, &loop, ns::TransportMode::Loopback, player, true));

	// --- 1. humans = the active human slot count (the SP host counts itself). ---
	inmatch::Server_TickUpdate(ctx);
	expect(world.cached.humans == 1, "humans == 1 for the SP host");

	// --- 2. Kill tallies by the local player: green person -> greenkills, blue person
	// -> bluekills, red person -> enemy; a green NON-person tallies nothing. ---
	push_kill(world, green_person, player);
	inmatch::Server_TickUpdate(ctx);
	expect(world.kill_stats.greenkills_by_player == 1, "green person kill -> greenkills");
	expect(!world.match.outcome().ended, "kill tallies alone never end the round");

	push_kill(world, blue_person, player);
	push_kill(world, red_person, player);
	push_kill(world, green_item, player);
	inmatch::Server_TickUpdate(ctx);
	expect(world.kill_stats.bluekills_by_player == 1, "blue person kill -> bluekills");
	expect(world.kill_stats.enemy_kills_by_player() == 1, "team>=2 kill -> enemy bucket");
	expect(world.kill_stats.greenkills_by_player == 1,
	       "a green NON-person victim tallies nothing [orig: the def+92==3 gate]");

	// --- 2a. Only Score_ProcessKillEvent tallies: a scripted death (no
	// damage-pass kill event) and a victim whose ItemDef `score` word is zero
	// never reach the buckets. [orig: the five callers of Score_ProcessKillEvent
	// @0x4FD400; the `cmp word ptr [eax+194h], 0` gate @0x4FD422] ---
	push_death(world, blue_person2, player);
	push_kill(world, blue_unscored, player);
	inmatch::Server_TickUpdate(ctx);
	expect(world.kill_stats.bluekills_by_player == 1,
	       "a scripted death and a score-0 victim leave bluekills alone");

	// --- 2b. The Show Score census: enemy-unit total at mission start counts
	// non-player, team >= 2 entities with a non-zero items.def unit-class byte;
	// the defined-subgoal count is the leading authored win-condition run.
	// [orig: Score_CountMissionSubgoalsAndUnits @0x509dc0 -> Score_ClassifyEntityForCounts @0x4fd070] ---
	{
		w::Entity unit;
		unit.net_id = 200;
		unit.team = 3;
		unit.kind = w::EntityKind::Item;
		unit.item_unit_type = 9; // aircraft class folds into the one total
		world.registry.spawn(0, unit);
		if (w::Entity *rp = world.registry.get(red_person))
			rp->item_unit_type = 1; // dead or alive: the census is start-time state
		// A pool-2 row never counts: the census walks pools 0 and 1 only
		// [orig: Score_CountMissionSubgoalsAndUnits @0x509e13..0x509e4a].
		world.registry.configure_pool(2, 4);
		w::Entity pool2_unit = unit;
		pool2_unit.net_id = 201;
		pool2_unit.item_unit_type = 3;
		world.registry.spawn(2, pool2_unit);
		w::count_mission_units(world);
		expect(world.kill_stats.enemy_unit_total == 2,
		       "census counts team>=2 units with a unit-class byte only");
		expect(world.kill_stats.enemy_aircraft_total == 1 &&
		               world.kill_stats.enemy_infantry_total == 1 &&
		               world.kill_stats.enemy_vehicle_total == 0,
		       "the census splits per def+406 class; the pool-2 vehicle is not counted");
		world.script.subgoals.win_text_ids[1] = 3;
		world.script.subgoals.win_text_ids[2] = 7;
		world.script.subgoals.win_text_ids[3] = 0; // terminator: slots past it ignored
		world.script.subgoals.win_text_ids[4] = 5;
		expect(w::count_defined_subgoals(world) == 2,
		       "defined subgoals = the leading non-zero, non-0xFF run");
	}

	// --- 2c. A by-player kill that takes a unit class past its census grows
	// the class and the enemy total by the overflow; a kill by others never
	// does. The pre-census red_person kill already sits in the infantry count.
	// [orig: Score_TallyKillByLocalPlayer @0x4FD242 (the class switch),
	//  @0x4FD264 / @0x4FD2C2 (the vehicle / infantry overflow);
	//  Score_TallyKillByOthers @0x4FD300 (no census write)] ---
	{
		const w::EntityHandle red2 = spawn_npc(110, 2, w::EntityKind::Organic);
		world.registry.get(red2)->item_unit_type = 1;
		const w::EntityHandle red_truck = spawn_npc(111, 4, w::EntityKind::Item);
		world.registry.get(red_truck)->item_unit_type = 3;
		push_kill(world, red2, player);
		push_kill(world, red_truck, player);
		inmatch::Server_TickUpdate(ctx);
		const w::MissionKillStats &ks = world.kill_stats;
		expect(ks.enemy_infantry_kills_by_player == 2 &&
		               ks.enemy_vehicle_kills_by_player == 1 &&
		               ks.enemy_kills_by_player() == 3,
		       "by-player enemy kills land in their unit class");
		expect(ks.enemy_infantry_total == 2 && ks.enemy_vehicle_total == 1 &&
		               ks.enemy_unit_total == 4,
		       "each class overflow grows its census and the enemy total");
		const w::EntityHandle red3 = spawn_npc(112, 2, w::EntityKind::Organic);
		world.registry.get(red3)->item_unit_type = 1;
		push_kill(world, red3, red_person);
		inmatch::Server_TickUpdate(ctx);
		expect(ks.enemy_kills_by_others == 1 && ks.enemy_unit_total == 4,
		       "a kill by others never grows the census");
		// Both SP screens read the grown max: 4 enemy kills over 4.
		const hud::EndRoundStatisticsInput in = w::end_round_statistics_input(world);
		expect(in.enemy_kills == 4 && in.enemy_unit_total == 4 &&
		               hud::end_round_statistics_rows(in)[1].value == "4/4" &&
		               hud::epilog_score_lines(in)[1].value == "4/4",
		       "the ENEMYUNITS line carries the overflowed total");
	}

	// --- 3. A kill by someone else lands in the by-others family, whose team-1
	// and team-0 buckets take any victim type. [orig: Score_TallyKillByOthers
	// @0x4FD325..0x4FD366] ---
	push_kill(world, green_person2, red_person);
	inmatch::Server_TickUpdate(ctx);
	expect(world.kill_stats.friendly_kills_by_others == 1,
	       "green person killed by an NPC -> friendly_kills_by_others");
	expect(world.kill_stats.greenkills_by_player == 1, "the by-player bucket is untouched");
	push_kill(world, blue_item, red_person);
	inmatch::Server_TickUpdate(ctx);
	expect(world.kill_stats.team_kills_by_others == 1,
	       "a blue NON-person killed by an NPC -> team_kills_by_others");

	// --- 4. SinglePlayerRespawn (attrib 0x40): the dead player respawns by its
	// own deploy pick, no auto-lose. Nothing deploys it when the hold expires:
	// the X key's C2S 0x0E rides its loopback to the server's respawn handler
	// [orig: Input_HandleSpecialKeys @0x49c9c9..0x49c9fd -> case 12 @0x49b17b;
	//  Server_ProcessClientRequestRespawn @0x519AF0]. ---
	// Each player death below is a lethal one: the body update re-reads the
	// health word every tick, so a record over a healthy body would revive it.
	auto pick_default_spawn = [&] {
		(void)inmatch::dispatch_session_replies(ctx.config, ctx.np_protocol.connection_list[0],
				{make_protocol_message(c2s::RESPAWN_REQUEST, {0xFF, 0xFF})}, world.logic_tick,
				ctx.np_protocol.connection_list, &world);
	};
	world.tables.mission_attrib_flags = 0x40;
	world.registry.get(player)->health = 0;
	push_death(world, player, red_person);
	for (int i = 0; i < 63; ++i) inmatch::Server_TickUpdate(ctx); // past a 1 Hz check
	expect(!world.match.outcome().ended, "death with SP-respawn never auto-loses");
	for (int i = 0; i < 621; ++i) inmatch::Server_TickUpdate(ctx);
	expect(!world.registry.get(player)->alive, "the expired hold deploys nothing by itself");
	// The deploy raises the local player to its ceiling with the difficulty
	// term: the objective Co-op word -1 doubles the def hp out of a session
	// [orig: Server_ProcessPlayerDeath @0x51782F -> Entity_RaiseHealthToMax ->
	//  Entity_GetMaxHealthWithDifficulty @0x43B8A0]. (D-PWR-2)
	world.rules.difficulty = -1;
	pick_default_spawn();
	expect(world.registry.get(player)->alive, "the player respawned on its pick after the timer");
	expect(world.registry.get(player)->health == 200, "the SP Co-op deploy doubles the ceiling");
	world.rules.difficulty = 0;

	// --- 4b. The Player's own lethal blast: no Player definition authors a
	// `score`, so the self-kill tallies nothing (the missions whose WAC reads
	// bluekills never fail on it). [orig: Score_ProcessKillEvent @0x4FD422] ---
	world.registry.get(player)->health = 0;
	push_kill(world, player, player);
	for (int i = 0; i < 63; ++i) inmatch::Server_TickUpdate(ctx);
	expect(world.kill_stats.bluekills_by_player == 1 &&
	               world.kill_stats.team_kills_by_others == 1,
	       "the player's self-kill tallies nothing");
	expect(!world.match.outcome().ended, "the self-kill with SP-respawn never auto-loses");
	for (int i = 0; i < 621; ++i) inmatch::Server_TickUpdate(ctx);
	pick_default_spawn();
	expect(world.registry.get(player)->alive, "the player respawned after the self-kill");

	// --- 5. No SP-respawn: the 1 Hz check ends the round, winner 2 (lose); the
	// respawn queue holds and the latch never double-fires. ---
	world.tables.mission_attrib_flags = 0;
	world.registry.get(player)->health = 0;
	push_death(world, player, red_person);
	for (int i = 0; i < 63; ++i) inmatch::Server_TickUpdate(ctx);
	expect(world.match.outcome().ended, "dead player without SP-respawn -> round over");
	expect(world.match.outcome().winner_team == 2, "auto-lose winner is team 2 (red)");
	expect(world.out.effects.count("round_end") == 1, "one round_end host effect");
	for (int i = 0; i < 700; ++i) inmatch::Server_TickUpdate(ctx);
	expect(!world.registry.get(player)->alive,
	       "respawns hold once the round is over [orig: the gate check @0x519af6]");
	world.process_round_end(1);
	expect(world.match.outcome().winner_team == 2, "the latch ignores a second round end");
	expect(world.out.effects.count("round_end") == 1, "no second round_end effect");

	test_tdm_round_wire_and_linger();
	test_dm_round_wire_named_header();
	test_dm_round_wire_kill_order_and_tie();
	test_tdm_inactive_columns_keep_configured_matrix_width();
	test_tkoth_board_five_rows_and_hold_column();
	test_demolition_death_routes_score_and_round_wire();
	test_aas_round_wire();
	test_coop_script_producers_share_round_wire();
	test_aas_capture_events_carry_zone_numbers();
	test_ctf_pickup_and_capture_wire_transaction();
	test_flag_timeout_wire_transaction();

	if (failures == 0) std::printf("round end tests passed\n");
	return failures ? 1 : 0;
}
