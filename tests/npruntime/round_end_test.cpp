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
#include <npruntime/server_tick.h>

#include <netsim/loopback_channel.h>

#include <npwire/replication_model.h>

#include <world/ai.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>

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

	const PlayerReplicationState anchor{};

	// --- 1. humans = the active human slot count (the SP host counts itself). ---
	np::Server_TickUpdate(ctx, anchor);
	expect(world.cached.humans == 1, "humans == 1 for the SP host");

	// --- 2. Kill tallies by the local player: green person -> greenkills, blue person
	// -> bluekills, red person -> enemy; a green NON-person tallies nothing. ---
	push_death(world, green_person, player);
	np::Server_TickUpdate(ctx, anchor);
	expect(world.kill_stats.greenkills_by_player == 1, "green person kill -> greenkills");
	expect(!world.round_end.ended, "kill tallies alone never end the round");

	push_death(world, blue_person, player);
	push_death(world, red_person, player);
	push_death(world, green_item, player);
	np::Server_TickUpdate(ctx, anchor);
	expect(world.kill_stats.bluekills_by_player == 1, "blue person kill -> bluekills");
	expect(world.kill_stats.enemy_kills_by_player == 1, "team>=2 kill -> enemy bucket");
	expect(world.kill_stats.greenkills_by_player == 1,
	       "a green NON-person victim tallies nothing [orig: the def+92==3 gate]");

	// --- 3. A kill by someone else lands in the by-others family. ---
	push_death(world, green_person2, red_person);
	np::Server_TickUpdate(ctx, anchor);
	expect(world.kill_stats.friendly_kills_by_others == 1,
	       "green person killed by an NPC -> friendly_kills_by_others");
	expect(world.kill_stats.greenkills_by_player == 1, "the by-player bucket is untouched");

	// --- 4. SinglePlayerRespawn (attrib 0x40): the dead player respawns, no auto-lose. ---
	world.mission_attrib_flags = 0x40;
	push_death(world, player, red_person);
	for (int i = 0; i < 63; ++i) np::Server_TickUpdate(ctx, anchor); // past a 1 Hz check
	expect(!world.round_end.ended, "death with SP-respawn never auto-loses");
	for (int i = 0; i < 621; ++i) np::Server_TickUpdate(ctx, anchor);
	expect(world.registry.get(player)->alive, "the player respawned after the timer");

	// --- 5. No SP-respawn: the 1 Hz check ends the round, winner 2 (lose); the
	// respawn queue holds and the latch never double-fires. ---
	world.mission_attrib_flags = 0;
	push_death(world, player, red_person);
	for (int i = 0; i < 63; ++i) np::Server_TickUpdate(ctx, anchor);
	expect(world.round_end.ended, "dead player without SP-respawn -> round over");
	expect(world.round_end.winner_team == 2, "auto-lose winner is team 2 (red)");
	expect(world.effects.count("round_end") == 1, "one round_end host effect");
	for (int i = 0; i < 700; ++i) np::Server_TickUpdate(ctx, anchor);
	expect(!world.registry.get(player)->alive,
	       "respawns hold once the round is over [orig: the gate check @0x519af6]");
	world.process_round_end(1);
	expect(world.round_end.winner_team == 2, "the latch ignores a second round end");
	expect(world.effects.count("round_end") == 1, "no second round_end effect");

	if (failures == 0) std::printf("round end tests passed\n");
	return failures ? 1 : 0;
}
