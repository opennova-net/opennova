// P3 — the World-driven host-side player spawn pipeline (server_spawn.{h,cpp}). Proves the §5.2a
// step 1-2 flow spawns the pool-0 player in World (ADR 0012), stamping entity+0x78 ownerConnectionId
// from the connection dcb, for BOTH the host's own type-2 loopback (-> spawn_player, publishes
// cached.local_player at the host dcb 2) and a remote type-1 joiner (-> spawn_remote_player at its
// own dcb, host's local player untouched). End-to-end: build_pool0_organic_batch then carries the
// real dcb at OrganicSpawnRecord::entity_flags (the §1 wiring), the F3 self-match field.

#include <npruntime/server_session.h>
#include <npruntime/server_spawn.h>

#include "host_test_setup.h"

#include <netsim/entity_wire_bridge.h>
#include <netsim/loopback_channel.h>

#include <novaworld/ingame_decode.h> // OrganicSpawnRecord / OrganicSpawnBatch

#include <world/ai.h>
#include <world/entity.h>
#include <world/player_spawn.h> // kPlayerInfantryTypeId
#include <world/spawn_select.h>
#include <world/world.h>

#include <cstdio>

namespace {
namespace np = opennova::np;
namespace w = opennova::world;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A World with an AiSystem (spawn_player requires it), pools 0 (players) and 3 (markers) configured,
// and one authored 6002 SP start marker so select_player_spawn finds a real spawn pose.
void make_world(w::World &world, w::AiSystem &ai) {
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002; // SP/DM player-start (kSpawnMarkerStartTypes[0])
	start.position = {123.0f, 456.0f, 7.0f};
	start.yaw = 30;
	world.registry.spawn(3, start);
}

const w::Entity *pool0_player(const w::World &world, uint16_t want_dcb) {
	const w::Entity *found = nullptr;
	world.registry.for_each([&](const w::Entity &e) {
		if (e.handle.pool() == 0 && e.owner_connection_id == want_dcb) found = &e;
	});
	return found;
}
} // namespace

int main() {
	w::World world;
	w::AiSystem ai;
	make_world(world, ai);

	// Stand up the listen host with its own loopback (P0->P1->P2), then wire the authoritative World.
	ns::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        /*host_key=*/0, &loopback);
	ctx.world = &world;

	// --- §5.2a step 1-2: spawn the host's OWN player (the type-2 loopback). ---
	np::Server_InitNewRoundState(ctx);
	const int n1 = np::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(n1 == 1, "host's own player spawned exactly once")) return 1;

	if (!expect(world.cached.local_player.valid(), "cached.local_player published for the host")) return 1;
	const w::Entity *host = world.registry.get(world.cached.local_player);
	if (!expect(host != nullptr, "host player entity resolvable")) return 1;
	if (!expect(host->item_id == w::kPlayerInfantryTypeId, "host player is 0x14B9 infantry")) return 1;
	if (!expect(host->owner_connection_id == np::kHostPlayerDcb,
	            "host player entity+0x78 == kHostPlayerDcb (2), NOT 0")) return 1;
	if (!expect(host->handle.slot() >= np::kRetailPlayerMinEntitySlot,
	            "host player lands at/after the retail player slot")) return 1;
	if (!expect(host->position.x == 123.0f && host->position.y == 456.0f,
	            "host player took the authored start-marker pose, not the origin/an NPC")) return 1;
	// Idempotent: a second pass spawns nothing (phase advanced to PlayerAdded).
	if (!expect(np::Server_ProcessPendingPlayerSpawns(ctx, world) == 0,
	            "re-running ProcessPendingPlayerSpawns is idempotent")) return 1;

	// --- A remote joiner reaches the accepted phase: it spawns as a REMOTE peer, host untouched. ---
	{
		np::NapiNPConnection joiner;
		joiner.type = 1;                       // server-side view of a remote client
		joiner.connection_id = np::kFirstJoinerDcb; // 3, the host-assigned dcb
		joiner.self_id_seen = true;            // accepted (post-0x42 / 0x48)
		joiner.phase = np::ConnectionPhase::Joined;
		ctx.np_protocol.connection_list.push_back(joiner);
	}
	const w::EntityHandle host_handle_before = world.cached.local_player;
	const int n2 = np::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(n2 == 1, "the remote joiner spawned exactly once")) return 1;
	if (!expect(world.cached.local_player == host_handle_before,
	            "the joiner did NOT republish cached.local_player (host keeps its own)")) return 1;

	const w::Entity *joiner_ent = pool0_player(world, np::kFirstJoinerDcb);
	if (!expect(joiner_ent != nullptr && joiner_ent->owner_connection_id == np::kFirstJoinerDcb,
	            "joiner player entity+0x78 == its dcb (3)")) return 1;
	if (!expect(joiner_ent->handle != world.cached.local_player,
	            "joiner is a distinct pool-0 entity from the host")) return 1;

	// --- End-to-end §1 wiring: the 0x0C organic batch carries each player's real dcb at entity+0x78. ---
	const opennova::OrganicSpawnBatch batch = ns::build_pool0_organic_batch(world);
	bool saw_host = false, saw_joiner = false;
	for (const opennova::OrganicSpawnRecord &r : batch.records) {
		if (r.entity_flags == np::kHostPlayerDcb) saw_host = true;
		if (r.entity_flags == np::kFirstJoinerDcb) saw_joiner = true;
	}
	if (!expect(saw_host && saw_joiner,
	            "build_pool0_organic_batch stamps entity_flags from owner_connection_id (host 2 + joiner 3)")) return 1;

	std::printf("OK\n");
	return 0;
}
