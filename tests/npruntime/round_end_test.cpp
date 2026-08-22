// world-wac-ai-re §20 — the SP round-outcome loop at the server tick: the kill
// tallies feeding the WAC bluekills/greenkills builtins [orig: Score_ProcessKillEvent
// @0x4fd400 -> Score_TallyKillByLocalPlayer @0x4fd160 / Score_TallyKillByOthers
// @0x4fd300], the humans count [orig: Server_BuildEntitySlotLists @0x4f97a0], the SP
// auto-lose win condition [orig: Server_CheckWinConditions @0x51ad40, SP leg
// @0x51ad6f — dead local player without the SinglePlayerRespawn attrib (0x40)], the
// round-end latch + host effect [orig: Server_ProcessRoundEnd @0x5164f0], and the
// post-round respawn hold [orig: the g_spawn_success_gate check @0x519af6].
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_message_dispatch.h>
#include <npruntime/server_tick.h>

#include <netsim/loopback_channel.h>

#include <npwire/replication_model.h>
#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>

#include <world/ai.h>
#include <world/game_type.h>
#include <world/player_spawn.h>
#include <world/world.h>
#include <world/zone_chain.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

np::NapiNPConnection make_conn(uint32_t id, int type, ns::ISessionTransport *t,
                               ns::TransportMode mode, w::EntityHandle owned, bool spawned) {
	np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? np::ConnectionPhase::InMatch : np::ConnectionPhase::New;
	return c;
}

void push_death(w::World &world, w::EntityHandle victim, w::EntityHandle killer) {
	w::RoundDeath d;
	d.victim = victim;
	d.killer = killer;
	d.victim_handle = victim.packed;
	d.killer_handle = killer.packed;
	world.round_sim.deaths.push_back(d);
}

w::EntityHandle match_player(w::World &world, uint8_t slot, uint8_t team,
		const char *name) {
	w::Entity e;
	e.kind = w::EntityKind::Organic;
	e.team = team;
	e.health = 100;
	e.alive = true;
	const w::EntityHandle handle = world.registry.spawn(0, e);
	world.match.upsert_player({handle, slot, name});
	return handle;
}

void ready_mp_connection(np::NapiNPConnection &conn, uint8_t slot) {
	conn.reply.player_slot = slot;
	conn.admission_stage = np::GameAdmissionStage::Complete;
}

bool drain_round_header(ns::LoopbackChannel &channel, EndRoundHeader &header) {
	bool saw_seed = false;
	bool saw_header = false;
	ns::Datagram datagram;
	while (channel.client_recv(datagram)) {
		if (datagram.tag == s2c::TICK_SEED)
			saw_seed = datagram.body == std::vector<uint8_t>(4, 0);
		if (datagram.tag == s2c::END_ROUND_HEADER) {
			saw_header = decode_end_round_header(
					datagram.body.data(), datagram.body.size(), header);
		}
	}
	return saw_seed && saw_header;
}

bool pull_round_board(np::NapiNPServerCtx &ctx, np::NapiNPConnection &connection,
		w::World &world, EndRoundStats &board, size_t &chunk_count) {
	std::vector<uint8_t> bytes;
	uint16_t offset = 0;
	uint16_t total_size = 0;
	chunk_count = 0;
	for (;;) {
		const std::vector<ProtocolMessage> replies = np::dispatch_session_replies(
				ctx.config, connection,
				{make_protocol_message(c2s::END_ROUND_STATS_REQUEST,
						encode_end_round_stats_request(offset))},
				world.logic_tick, ctx.np_protocol.connection_list, &world);
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
	world.mp_session = true;
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
	np::NapiNPServerCtx ctx;
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

	push_death(world, red, blue);
	for (int i = 0; i < 61; ++i) np::Server_TickUpdate(ctx);
	blue_wire.clear();
	red_wire.clear();
	np::Server_TickUpdate(ctx); // the 62-tick win-condition boundary
	expect(world.match.outcome().ended &&
			world.match.outcome().winner_team == 1,
			"TDM kill limit ends for the killer's team");
	expect(ctx.round_end_announced && ctx.round_end_linger_ticks == 2790,
			"TDM announces once and seeds the exact MP linger");

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
	const std::vector<ProtocolMessage> invalid_offset_reply =
			np::dispatch_session_replies(
					ctx.config, ctx.np_protocol.connection_list[0],
					{make_protocol_message(
							c2s::END_ROUND_STATS_REQUEST,
							encode_end_round_stats_request(UINT16_MAX))},
					world.logic_tick, ctx.np_protocol.connection_list, &world);
	expect(invalid_offset_reply.empty(),
			"C2S 0x2B offset beyond the frozen board receives no 0x56 reply");

	for (int i = 0; i < 2789; ++i) np::Server_TickUpdate(ctx);
	expect(ctx.is_in_session == 1 && ctx.round_end_linger_ticks == 1,
			"MP session remains live through linger tick 2789");
	np::Server_TickUpdate(ctx);
	expect(ctx.is_in_session == 0 && ctx.round_end_linger_ticks == 0,
			"MP session closes at exactly 2790 post-announcement ticks");
}

void test_aas_and_coop_share_round_wire() {
	for (const uint32_t game_type : {0x10010u, 0x10020u}) {
		w::World world;
		world.registry.configure_pool(0, 4);
		world.registry.configure_pool(1, 4);
		world.mp_session = true;
		w::MatchRules rules;
		rules.game_type = game_type;
		world.match.configure(rules);
		const w::EntityHandle red = match_player(world, 4, 2, "Red");
		if (game_type == 0x10010u) {
			w::Entity z1;
			z1.kind = w::EntityKind::Item;
			z1.team = 2;
			z1.zone_number = 1;
			w::Entity z2 = z1;
			z2.zone_number = 2;
			world.zone_chain.zones.push_back(world.registry.spawn(1, z1));
			world.zone_chain.zones.push_back(world.registry.spawn(1, z2));
		} else {
			// Co-op has no automatic winner; WAC/BMS owns this edge.
			world.process_round_end(2);
		}

		ns::LoopbackChannel wire;
		np::NapiNPServerCtx ctx;
		ctx.world = &world;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		ctx.config.game_type = game_type;
		ctx.np_protocol.connection_list.push_back(
				make_conn(3, 1, &wire, ns::TransportMode::Client, red, true));
		ready_mp_connection(ctx.np_protocol.connection_list[0], 4);
		if (game_type == 0x10010u) {
			for (int i = 0; i < 61; ++i) np::Server_TickUpdate(ctx);
			wire.clear();
		}
		np::Server_TickUpdate(ctx);

		EndRoundHeader header;
		expect(drain_round_header(wire, header),
				game_type == 0x10010u
						? "A&S uses the shared 0x61/0x1D transition"
						: "network Co-op uses the shared 0x61/0x1D transition");
		expect(header.winner_team == 2,
				"A&S/Co-op header preserves the authoritative winner");
		if (game_type == 0x10010u) {
			expect(header.team_score_0 == 0 && header.team_score_1 == 2,
					"A&S header scores are owned-zone counts");
			expect(ctx.round_end_linger_ticks == 2790,
					"automatic A&S outcome does not consume its announcement tick");
		} else {
			expect(ctx.round_end_linger_ticks == 2789,
					"script-driven Co-op outcome consumes the originating server tick");
		}
	}
}

void test_aas_events_use_spawn_registry_index() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	world.registry.configure_pool(2, 4);
	world.mp_session = true;
	w::MatchRules rules;
	rules.game_type = 0x10010u;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	world.registry.get(blue)->player_class = 8;
	world.registry.get(blue)->position = {100.0f, 0.0f, 0.0f};

	// A sorted pool-2 spawn object precedes the three pool-1 capture zones. The
	// target zone is therefore spawn-registry index 2 but zone-chain index 1.
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
		zone.is_capture_trigger = true;
		zone.is_spawn_point = true;
		zone.alive = true;
		return world.registry.spawn(1, zone);
	};
	spawn_zone(1, 1, -100.0f);
	const w::EntityHandle target = spawn_zone(2, 0, 100.0f);
	spawn_zone(3, 2, 300.0f);
	w::zone_chain_build_from_mission(world, world.zone_chain);
	w::zone_chain_latch_control(world, world.zone_chain);

	ns::LoopbackChannel wire;
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &wire, ns::TransportMode::Client, blue, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);
	for (int i = 0; i < 61; ++i) np::Server_TickUpdate(ctx);
	wire.clear();
	np::Server_TickUpdate(ctx);

	bool saw_capture_event = false;
	ns::Datagram datagram;
	while (wire.client_recv(datagram)) {
		if (datagram.tag != 0x1E || datagram.body.size() != 8) continue;
		const uint8_t event = datagram.body[0];
		if (event < 50 || event > 57) continue;
		saw_capture_event = true;
		expect(datagram.body[1] == 2,
				"A&S 0x1E capture actor is the sorted SpawnZoneList index");
	}
	expect(world.registry.get(target)->team == 1 && saw_capture_event,
			"A&S authority flips the target and emits its capture event family");
}

void test_ctf_pickup_and_capture_wire_transaction() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	world.mp_session = true;
	w::MatchRules rules;
	rules.game_type = game_type::kCaptureTheFlag;
	world.match.configure(rules);
	const w::EntityHandle blue = match_player(world, 3, 1, "Blue");
	w::Entity *blue_entity = world.registry.get(blue);
	blue_entity->position = {10.0f, 20.0f, 3.0f};
	blue_entity->net_move_input |= w::Entity::kMoveOrderMoving;

	w::Entity red_flag;
	red_flag.kind = w::EntityKind::Item;
	red_flag.item_id = 4093; // Flag (Red) [orig: item-id branch @0x43C1B7]
	red_flag.position = blue_entity->position;
	red_flag.spawn_position = red_flag.position;
	const w::EntityHandle flag = world.registry.spawn(1, red_flag);
	w::Entity blue_bay;
	blue_bay.kind = w::EntityKind::Item;
	blue_bay.item_id = 4098; // Blue bay [orig: Entity_ProcessWaypointInteraction @0x4AD8D4]
	blue_bay.position = blue_entity->position;
	const w::EntityHandle bay = world.registry.spawn(1, blue_bay);
	expect(flag.valid() && bay.valid(), "CTF objective fixtures spawn");

	ns::LoopbackChannel wire;
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 1, &wire, ns::TransportMode::Client, blue, true));
	ready_mp_connection(ctx.np_protocol.connection_list[0], 3);

	np::Server_TickUpdate(ctx); // contact -> pickup
	bool saw_pickup = false;
	ns::Datagram datagram;
	while (wire.client_recv(datagram)) {
		if (datagram.tag != s2c::OBJECTIVE_ENTITY_STATE) continue;
		ObjectiveEntityState state;
		size_t consumed = 0;
		if (decode_objective_entity_state(
				datagram.body.data(), datagram.body.size(), state, consumed) &&
				consumed == datagram.body.size() && state.entity_handle == flag.packed &&
				state.attach_handle == blue.packed && (state.flags_byte & 1u) != 0)
			saw_pickup = true;
	}
	expect(saw_pickup,
			"CTF pickup fans the retail 19-byte 0x2F carried-state record");

	np::Server_TickUpdate(ctx); // carried flag overlaps bay -> capture
	bool saw_capture_event = false;
	bool saw_remove = false;
	bool saw_reset = false;
	int capture_order = -1;
	int remove_order = -1;
	int order = 0;
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
}

} // namespace

int main() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;

	// The host player + NPC victims across the witnessed classification axes.
	w::PlayerSpawn ps;
	ps.position = {0.0f, 0.0f, 10.0f};
	ps.team = 1;
	ps.net_id = 0xFFF0;
	const w::EntityHandle player = w::spawn_player(world, ps);
	if (!expect(player.valid(), "host player spawned")) return 1;
	world.cached.local_player = player;

	auto spawn_npc = [&](uint16_t net_id, uint8_t team, w::EntityKind kind) {
		w::Entity e;
		e.net_id = net_id;
		e.team = team;
		e.kind = kind;
		e.alive = true;
		e.health = 100;
		return world.registry.spawn(0, e);
	};
	const w::EntityHandle green_person = spawn_npc(100, 0, w::EntityKind::Organic);
	const w::EntityHandle blue_person = spawn_npc(101, 1, w::EntityKind::Organic);
	const w::EntityHandle red_person = spawn_npc(102, 2, w::EntityKind::Organic);
	const w::EntityHandle green_item = spawn_npc(103, 0, w::EntityKind::Item);
	const w::EntityHandle green_person2 = spawn_npc(104, 0, w::EntityKind::Organic);

	ns::LoopbackChannel loop;
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 0; // SP: the tallies + the auto-lose leg are SP-only
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 2, &loop, ns::TransportMode::Loopback, player, true));

	// --- 1. humans = the active human slot count (the SP host counts itself). ---
	np::Server_TickUpdate(ctx);
	expect(world.cached.humans == 1, "humans == 1 for the SP host");

	// --- 2. Kill tallies by the local player: green person -> greenkills, blue person
	// -> bluekills, red person -> enemy; a green NON-person tallies nothing. ---
	push_death(world, green_person, player);
	np::Server_TickUpdate(ctx);
	expect(world.kill_stats.greenkills_by_player == 1, "green person kill -> greenkills");
	expect(!world.match.outcome().ended, "kill tallies alone never end the round");

	push_death(world, blue_person, player);
	push_death(world, red_person, player);
	push_death(world, green_item, player);
	np::Server_TickUpdate(ctx);
	expect(world.kill_stats.bluekills_by_player == 1, "blue person kill -> bluekills");
	expect(world.kill_stats.enemy_kills_by_player == 1, "team>=2 kill -> enemy bucket");
	expect(world.kill_stats.greenkills_by_player == 1,
	       "a green NON-person victim tallies nothing [orig: the def+92==3 gate]");

	// --- 3. A kill by someone else lands in the by-others family. ---
	push_death(world, green_person2, red_person);
	np::Server_TickUpdate(ctx);
	expect(world.kill_stats.friendly_kills_by_others == 1,
	       "green person killed by an NPC -> friendly_kills_by_others");
	expect(world.kill_stats.greenkills_by_player == 1, "the by-player bucket is untouched");

	// --- 4. SinglePlayerRespawn (attrib 0x40): the dead player respawns, no auto-lose. ---
	world.mission_attrib_flags = 0x40;
	push_death(world, player, red_person);
	for (int i = 0; i < 63; ++i) np::Server_TickUpdate(ctx); // past a 1 Hz check
	expect(!world.match.outcome().ended, "death with SP-respawn never auto-loses");
	for (int i = 0; i < 621; ++i) np::Server_TickUpdate(ctx);
	expect(world.registry.get(player)->alive, "the player respawned after the timer");

	// --- 5. No SP-respawn: the 1 Hz check ends the round, winner 2 (lose); the
	// respawn queue holds and the latch never double-fires. ---
	world.mission_attrib_flags = 0;
	push_death(world, player, red_person);
	for (int i = 0; i < 63; ++i) np::Server_TickUpdate(ctx);
	expect(world.match.outcome().ended, "dead player without SP-respawn -> round over");
	expect(world.match.outcome().winner_team == 2, "auto-lose winner is team 2 (red)");
	expect(world.effects.count("round_end") == 1, "one round_end host effect");
	for (int i = 0; i < 700; ++i) np::Server_TickUpdate(ctx);
	expect(!world.registry.get(player)->alive,
	       "respawns hold once the round is over [orig: the gate check @0x519af6]");
	world.process_round_end(1);
	expect(world.match.outcome().winner_team == 2, "the latch ignores a second round end");
	expect(world.effects.count("round_end") == 1, "no second round_end effect");

	test_tdm_round_wire_and_linger();
	test_aas_and_coop_share_round_wire();
	test_aas_events_use_spawn_registry_index();
	test_ctf_pickup_and_capture_wire_transaction();

	if (failures == 0) std::printf("round end tests passed\n");
	return failures ? 1 : 0;
}
