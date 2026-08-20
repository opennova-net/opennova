// The HOST half of the death channel: a round-sim death must reach every REMOTE
// in-match joiner as S2C 0x13 (and, for a player victim, the 0x1E game event),
// while the host's own loopback view is skipped.
//
// Motivation: a live self-test round showed no 0x13 on the wire, and the wire
// alone could not say whether the emitter was broken or the scenario simply
// never killed anyone (an unattended bot session produces no kills — this
// mission's combat is player-driven). This test settles that deterministically,
// so the emitter is never again judged by an unexercised capture.
//
// [orig: sender Entity_CheckAndProcessDeath @0x51b550 (msg 19, send mask 0x90
//  NOT_HOST); the 0x1E kill event @0x517237; client fold
//  NapiNPClientMsg_EntityDeath @0x42EB50]
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_tick.h>

#include <netsim/loopback_channel.h>

#include <npwire/ingame_message_id.h>

#include <world/ai.h>
#include <world/player_spawn.h>
#include <world/world.h>

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
                               ns::TransportMode mode, w::EntityHandle owned) {
	np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = true;
	c.spawned_announced = true;
	c.phase = np::ConnectionPhase::InMatch;
	return c;
}

// Count datagrams the host queued to this channel carrying `tag`.
int count_tag(ns::LoopbackChannel &channel, uint8_t tag) {
	int n = 0;
	ns::Datagram dg;
	while (channel.client_recv(dg)) {
		if (dg.tag == tag) ++n;
	}
	return n;
}

} // namespace

int main() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;

	w::PlayerSpawn ps;
	ps.position = {0.0f, 0.0f, 10.0f};
	ps.team = 1;
	ps.net_id = 0xFFF0;
	const w::EntityHandle host_player = w::spawn_player(world, ps);
	if (!expect(host_player.valid(), "host player spawned")) return 1;
	world.cached.local_player = host_player;

	w::PlayerSpawn js;
	js.position = {5.0f, 0.0f, 10.0f};
	js.team = 1;
	js.net_id = 0xFFF1;
	const w::EntityHandle joiner_player = w::spawn_player(world, js);
	if (!expect(joiner_player.valid(), "joiner player spawned")) return 1;

	w::Entity npc;
	npc.net_id = 200;
	npc.team = 2;
	npc.kind = w::EntityKind::Organic;
	npc.alive = true;
	npc.health = 100;
	const w::EntityHandle victim = world.registry.spawn(0, npc);

	ns::LoopbackChannel host_link;   // the host's own type-2 view: must be SKIPPED
	ns::LoopbackChannel joiner_link; // a remote joiner: must RECEIVE
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1; // MP: the wire fan is session-gated
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 2, &host_link, ns::TransportMode::Loopback, host_player));
	ctx.np_protocol.connection_list.push_back(
			make_conn(2, 1, &joiner_link, ns::TransportMode::Client, joiner_player));

	np::Server_TickUpdate(ctx);
	(void)count_tag(host_link, s2c::ENTITY_DEATH);   // drain the join-burst traffic
	(void)count_tag(joiner_link, s2c::ENTITY_DEATH);

	// --- 1. An NPC killed by the host player: the remote joiner gets 0x13. ---
	{
		w::RoundDeath d;
		d.victim = victim;
		d.killer = host_player;
		d.victim_handle = victim.packed;
		d.killer_handle = host_player.packed;
		world.round_sim.deaths.push_back(d);
		np::Server_TickUpdate(ctx);

		expect(count_tag(joiner_link, s2c::ENTITY_DEATH) == 1,
		       "0x13: the remote joiner receives exactly one entity-death notify");
		expect(count_tag(host_link, s2c::ENTITY_DEATH) == 0,
		       "0x13: the host's own loopback view is skipped (mask 0x90 NOT_HOST)");
		const w::Entity *dead = world.registry.get(victim);
		expect(dead != nullptr && !dead->alive && (dead->flags & 2u) != 0,
		       "0x13: the victim carries the wire-dead bit");
	}

	// --- 2. A PLAYER victim also raises the 0x1E kill event; an NPC victim does not. ---
	{
		w::RoundDeath d;
		d.victim = joiner_player;
		d.killer = victim;
		d.victim_handle = joiner_player.packed;
		d.killer_handle = victim.packed;
		world.round_sim.deaths.push_back(d);
		np::Server_TickUpdate(ctx);
		expect(count_tag(joiner_link, s2c::GAME_EVENT) == 1,
		       "0x1E: a player victim raises exactly one kill event");
	}

	if (failures == 0) std::printf("death broadcast tests passed\n");
	return failures ? 1 : 0;
}
