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
#include <vector>
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

	// --- 1. humans = the active human slot count (the SP host counts itself). ---
	np::Server_TickUpdate(ctx);
	expect(world.cached.humans == 1, "humans == 1 for the SP host");

	// A HIDDEN player is not a human. Retail counts entities, not connections:
	// pool 0, item type present, the player classifier (Flags 0x100), and NOT
	// Flags 0x1 - the bit that marks a body tucked inside something else, and the
	// bit the host ORs onto a player entity that is sitting at the deploy screen
	// rather than standing in the world.
	// [orig: Server_BuildEntitySlotLists @0x4f97a0 - the walk's gates
	//  `entity+32 != 0`, `Flags & 0x100`, `(Flags & 1) == 0`, then
	//  ++wac_var_humans @0x4f98b1; the pending OR is
	//  NetPacket_WritePlayerState @0x4ff7dd]
	//
	// This is load-bearing now that the count gates the whole mission script
	// (World::script_may_advance): an undeployed player must not make a host look
	// occupied, or the mission runs before anyone is there to see it.
	if (w::Entity *pe = world.registry.get(player)) {
		pe->flags |= 1u;
		np::Server_TickUpdate(ctx);
		expect(world.cached.humans == 0,
		       "a hidden (Flags&1) player does not count as a human");
		pe->flags &= ~1u;
		np::Server_TickUpdate(ctx);
		expect(world.cached.humans == 1, "clearing the hidden bit restores the count");

		// The player classifier is equally required: an AI body in pool 0 is not a
		// human however alive it is.
		const uint32_t saved = pe->flags;
		const uint32_t saved_engine = pe->engine_flags;
		pe->flags &= ~static_cast<uint32_t>(w::kEntityFlagPlayer);
		pe->engine_flags &= ~static_cast<uint32_t>(w::kEntityFlagPlayer);
		np::Server_TickUpdate(ctx);
		expect(world.cached.humans == 0, "a non-player entity never counts as a human");
		pe->flags = saved;
		pe->engine_flags = saved_engine;
		np::Server_TickUpdate(ctx);
		expect(world.cached.humans == 1, "restoring the player bit restores the count");
	}

	// --- 2. Kill tallies by the local player: green person -> greenkills, blue person
	// -> bluekills, red person -> enemy; a green NON-person tallies nothing. ---
	push_death(world, green_person, player);
	np::Server_TickUpdate(ctx);
	expect(world.kill_stats.greenkills_by_player == 1, "green person kill -> greenkills");
	expect(!world.round_end.ended, "kill tallies alone never end the round");

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
	expect(!world.round_end.ended, "death with SP-respawn never auto-loses");
	for (int i = 0; i < 621; ++i) np::Server_TickUpdate(ctx);
	expect(world.registry.get(player)->alive, "the player respawned after the timer");

	// --- 5. No SP-respawn: the 1 Hz check ends the round, winner 2 (lose); the
	// respawn queue holds and the latch never double-fires. ---
	world.mission_attrib_flags = 0;
	push_death(world, player, red_person);
	loop.clear(); // capture the round-end WIRE pass on its ended-edge (section 6)
	for (int i = 0; i < 63; ++i) np::Server_TickUpdate(ctx);
	int saw61 = 0, saw1d = 0;
	bool marker_precedes_header = false;
	std::vector<uint8_t> header;
	{
		ns::Datagram dg;
		while (loop.client_recv(dg)) {
			if (dg.tag == 0x61 && dg.body.size() == 4 && dg.body[0] == 0 &&
					dg.body[1] == 0 && dg.body[2] == 0 && dg.body[3] == 0) {
				++saw61;
				if (saw1d == 0) marker_precedes_header = true;
			} else if (dg.tag == 0x1D && dg.body.size() == 7) {
				++saw1d;
				header = dg.body;
			}
		}
	}
	expect(world.round_end.ended, "dead player without SP-respawn -> round over");
	expect(world.round_end.winner_team == 2, "auto-lose winner is team 2 (red)");
	expect(world.effects.count("round_end") == 1, "one round_end host effect");
	for (int i = 0; i < 700; ++i) np::Server_TickUpdate(ctx);
	expect(!world.registry.get(player)->alive,
	       "respawns hold once the round is over [orig: the gate check @0x519af6]");
	world.process_round_end(1);
	expect(world.round_end.winner_team == 2, "the latch ignores a second round end");
	expect(world.effects.count("round_end") == 1, "no second round_end effect");

	// --- 6. The round-end WIRE pass [orig: Server_ProcessRoundEnd @0x5164f0,
	// the state-6 slot loop @0x516790..0x51685e], captured on the ended-edge
	// raised in section 5 and driven through the real tick so the call-site
	// wiring is covered too. Per active slot: S2C 0x61 (4 zero bytes) then
	// S2C 0x1D (the 7-byte scoreboard header), then game state 11. ---
	expect(world.round_end.draw,
	       "both team scores 0 -> draw flag [orig: the equality @0x508f30, kong 213720]");
	expect(saw61 == 1, "one 0x61 round-end marker, 4 zero bytes [orig: @0x516790]");
	expect(saw1d == 1, "one 7-byte 0x1D scoreboard header [orig: @0x516839]");
	expect(marker_precedes_header, "0x61 precedes 0x1D, retail's per-slot order");
	// [u8 winner][s16 score0][s16 score1][u8 draw][s8 myEntryIndex]
	// [orig: EndRoundScoreboard_SerializeHeader @0x505280 — the 7-byte arm, which
	//  Co-op always takes because g_GameType 0x30020 has bit 0x10000 set
	//  (AI_GetTaskTypeFromFlags @0x40DAE0: attrib 0x1000000 -> task 2), matching
	//  the 00TRg retail baseline capture's S 0x1D len=7]
	if (expect(header.size() == 7, "header is 7 bytes")) {
		expect(header[0] == 2, "winner byte = the winning team (auto-lose = 2)");
		expect(header[1] == 0 && header[2] == 0, "teamScore0 — UNPORTED source, ledger D2");
		expect(header[3] == 0 && header[4] == 0, "teamScore1 — UNPORTED source, ledger D2");
		expect(header[5] == 1, "draw flag = (score0 == score1)");
		expect(static_cast<int8_t>(header[6]) == 0, "myEntryIndex = this slot's row");
	}
	expect(ctx.np_protocol.connection_list[0].burst.game_state == 11,
	       "slot moved to game state 11 [orig: CNetPlayer_SetGameState @0x516846]");
	// The 2790-tick linger is MP-ONLY: gated on is_in_session, which this SP
	// fixture leaves 0 [orig: @0x5166c4].
	expect(ctx.endround_linger_timer == 0,
	       "SP arms no end-round linger [orig: the is_in_session gate @0x5166c4]");
	// One-shot: later ticks never re-emit the block.
	loop.clear();
	np::Server_TickUpdate(ctx);
	{
		ns::Datagram dg;
		int again = 0;
		while (loop.client_recv(dg))
			if (dg.tag == 0x61 || (dg.tag == 0x1D && dg.body.size() == 7)) ++again;
		expect(again == 0, "the wire pass is one-shot on the ended edge");
	}

	if (failures == 0) std::printf("round end tests passed\n");
	return failures ? 1 : 0;
}
