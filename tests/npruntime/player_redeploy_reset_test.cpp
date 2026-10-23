// npruntime_player_redeploy_reset: the player redeploys reset through the shared World reset,
// Entity_ResetToSpawnState's whole port (D-AI-9's player leg, D-NET-NEW-L5-65). Its spawn-pose
// leg runs on a body whose dead bit is clear: the state request, the twelve dual updates, then
// the resolve over the slice the body's last build left, a clearance at or below zero lifting
// it. The resolver's idle-skip count (+0x373) is zeroed at the reset's head, so that resolve
// runs whole whatever count the body kept. The tail builds the slices over the published tables
// and runs the body's own list.
//   - The host's deploy clears the dead bit ahead of the reset, so a body placed into a wall
//     leaves the deploy pushed out of it.
//   - The joiner's record respawn arm calls the reset on its still-dead L, so the reset
//     resolves nothing (the arm's own pin is inmatch_joiner_role's
//     run_the_respawn_arm_resolves_nothing_in_the_receive).
// [orig: Entity_ResetToSpawnState @0x4B9610 - +0x373 = 0 @0x4B9656, the dead test @0x4B96ED
//  (its jnz @0x4B96F7), AnimMap_UpdateDualChannels called @0x4B973D,
//  Entity_MovementCollisionResolver called @0x4B9755, the lift @0x4B9761, the clear @0x4B97B0,
//  Entity_BuildProximityListsFromPools called @0x4B98EB, Entity_BuildProximityList called
//  @0x4B98F1; Server_DeployPlayer @0x517740 - the dead-bit clear @0x51787A, the reset called
//  @0x517899; NetPacket_SerializePlayerState @0x4C09C0 - the dead test @0x4C1109, the reset
//  called @0x4C113C; Entity_MovementCollisionResolver @0x4B2BD0 - the skip count's read
//  @0x4B2CBC, the skip return @0x4B2CF5]

#include <runtime/inmatch/server_message_dispatch.h> // Server_ReleasePlayerDeployment
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>

#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h> // placed_heading_bam
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr int32_t kOne = 65536;

// A pool-2 building's one solid volume: its +Y face a quarter unit past the building's origin,
// every other face 20 units off, so a body standing on the origin stands a quarter unit into
// the wall and only the +Y face is near it.
w::CollisionModel wall_model() {
	w::CollisionModel model;
	const auto plane = [&](int16_t x, int16_t y, int16_t z, int32_t dist) {
		w::CollisionPlane p;
		p.nx = x;
		p.ny = y;
		p.nz = z;
		p.dist = dist;
		model.planes.push_back(p);
	};
	plane(16384, 0, 0, -20 * kOne);
	plane(-16384, 0, 0, -20 * kOne);
	plane(0, 16384, 0, -kOne / 4);
	plane(0, -16384, 0, -20 * kOne);
	plane(0, 0, 16384, -20 * kOne);
	plane(0, 0, -16384, -20 * kOne);
	w::CollisionVolume volume;
	volume.type = w::bvol_type::kSolidCB;
	volume.min_x = volume.min_y = volume.min_z = -20 * kOne;
	volume.max_x = volume.max_z = 20 * kOne;
	volume.max_y = kOne / 4;
	volume.plane_count = 6;
	model.volumes.push_back(volume);
	w::CollisionSection section;
	section.volume_count = 1;
	model.sections.push_back(section);
	return model;
}

// A root-motion double: a one-unit capsule and no motion, on every state the reset reaches
// (the respawn state 44 and the death clip it blends out of).
struct Source final : w::IRootMotionSource {
	std::set<int> clips{0, 43, 44, 174};
	bool has_clip(int, int state) const override { return clips.count(state) != 0; }
	int32_t clip_length_ticks(int, int, int) const override { return 10000; }
	bool advance(int, int state, int32_t &phase, w::RootMotionFrame &out) override {
		if (!has_clip(0, state)) return false;
		++phase;
		out = {};
		out.capsule_bottom = kOne;
		out.capsule_top = 2 * kOne;
		return true;
	}
};

// A player body whose corpse lay at (8, 9.5, 0), 1.25 units off the wall's +Y face, where its
// slice was last built and its savedLivePose last stamped; it is then put on the wall's origin
// (8, 8, 0), a quarter unit into the wall, with a skip count of 15 left from the corpse's
// idle resolves (the band's skip). Nothing but the skip count holds the resolve back: the move
// is along Y, which the skip test does not read, the body is still, and the update counter is
// off the every-64th edge. Heap-held (the 1 MB test-main stack under MSVC).
struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<w::CollisionWorld> collision = std::make_unique<w::CollisionWorld>();
	Source source;
	w::EntityHandle body;
	w::EntityHandle wall;

	explicit Rig(bool dead) {
		world->registry.configure_pool(0, 8);
		world->registry.configure_pool(2, 4);
		world->collision = collision.get();
		world->ai.collision = collision.get();
		world->ai.root_motion = &source;
		world->ai.is_authority = true;
		w::PlayerSpawn spawn;
		spawn.position.x.q16 = 8 * kOne;
		spawn.position.y.q16 = 9 * kOne + kOne / 2;
		spawn.position.z.q16 = 0;
		spawn.team = 1;
		body = w::spawn_remote_player(*world, spawn);
		w::Entity &e = entity();
		// The +0x1C word the pool-0 slice build gates on, and a bound radius.
		e.item_type_index = 3;
		e.bound_radius = 1;
		e.flags &= ~w::kEntityFlagInAir;
		e.engine_flags &= ~w::kEntityFlagInAir;
		w::Entity building;
		building.kind = w::EntityKind::Building;
		building.position = {8.0f, 8.0f, 0.0f};
		building.heading = w::placed_heading_bam(90); // the identity rotation
		building.bound_radius = 30;
		building.alive = true;
		wall = world->registry.spawn(2, building);
		collision->assign_entity(wall, collision->add_model(wall_model()));
		// The corpse's slice, built where it lay (the 17-tick edge).
		for (int i = 0; i < 17; ++i) collision->build_tick_tables(*world);
		e.saved_live_pos[0] = 8 * kOne;
		e.saved_live_pos[1] = 9 * kOne + kOne / 2;
		e.saved_live_pos[2] = 0;
		e.saved_live_valid = true;
		w::AiEntity &ai = motor();
		ai.inf.reset_body_animation(174); // the corpse's death clip
		ai.collide_state.skip_counter = 15;
		world->entity_update_counter = 1;
		if (dead) {
			e.flags |= w::kEntityFlagDead;
			e.engine_flags |= w::kEntityFlagDead;
			e.alive = false;
			e.health = 0;
			// The Indoors bit its last resolve took, which only the tail's list clears on a
			// body the reset does not resolve.
			e.flags |= w::kEntityFlagIndoors;
		}
		e.position.set_q16({8 * kOne, 8 * kOne, 0});
	}

	w::Entity &entity() { return *world->registry.get(body); }
	w::AiEntity &motor() { return *world->ai.for_handle(body); }
};

// A body whose dead bit is clear resolves inside the reset: the twelve dual updates leave it on
// the respawn state, and the resolve, which the zeroed skip count lets run, pushes it out of the
// wall along +Y; the spawn backup keeps the pose ahead of the resolve.
bool run_the_reset_resolves_a_live_body_out_of_a_wall() {
	Rig r(/*dead=*/false);
	if (!expect(r.collision->candidate_count(r.body) >= 1,
	            "live reset: the corpse's slice holds the wall")) return false;
	w::entity_reset_to_spawn_state(*r.world, r.world->ai, r.entity());
	const w::Entity &e = r.entity();
	if (!expect(e.position.y.q16 > 8 * kOne, "live reset: the resolve pushes the body out along +Y"))
		return false;
	if (!expect(e.position.x.q16 == 8 * kOne, "live reset: the push is the +Y face's alone"))
		return false;
	if (!expect(e.spawn_position.y.q16 == 8 * kOne,
	            "live reset: the spawn backup is the pose ahead of the resolve")) return false;
	if (!expect(r.motor().inf.anim_state == 44,
	            "live reset: the dual updates leave the body on the respawn state")) return false;
	return expect((e.flags & w::kEntityFlagDead) == 0u, "live reset: the dead bit stays clear");
}

// A body whose dead bit is set takes no spawn-pose leg: no state request, no dual update and no
// resolve, so it stays a quarter unit in the wall; the skip count is still zeroed, the dead bit
// cleared, and the tail's list runs on it (the Indoors bit its last resolve left goes).
bool run_the_reset_on_a_dead_body_resolves_nothing() {
	Rig r(/*dead=*/true);
	w::entity_reset_to_spawn_state(*r.world, r.world->ai, r.entity());
	const w::Entity &e = r.entity();
	if (!expect(e.position.x.q16 == 8 * kOne && e.position.y.q16 == 8 * kOne &&
	                    e.position.z.q16 == 0,
	            "dead reset: no resolve moves the body")) return false;
	if (!expect(r.motor().collide_state.skip_counter == 0, "dead reset: the skip count is zeroed"))
		return false;
	if (!expect(r.motor().inf.anim_state == 174, "dead reset: no state request")) return false;
	if (!expect((e.flags & w::kEntityFlagDead) == 0u && (e.engine_flags & w::kEntityFlagDead) == 0u,
	            "dead reset: the dead bit clears")) return false;
	return expect((e.flags & w::kEntityFlagIndoors) == 0u,
	              "dead reset: the tail's list runs on the body");
}

// The host's deploy places a dead body on the 6002 start (123, 456, 7), a quarter unit into a
// wall whose +Y face its corpse's slice holds, and clears the dead bit ahead of the reset: the
// deploy leaves the body pushed out along +Y, its X the start's.
bool run_the_deploy_pushes_the_body_out_of_a_wall() {
	auto world = std::make_unique<w::World>();
	world->tables.player.has_item_def = true;
	world->tables.player.item_type_index = 1;
	world->tables.player.item_type = 3;
	world->registry.configure_pool(0, 16);
	world->registry.configure_pool(2, 4);
	world->registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002; // the non-team fallback player start
	start.position = {123.0f, 456.0f, 7.0f};
	start.heading = w::placed_heading_bam(30);
	world->registry.spawn(3, start);
	auto collision = std::make_unique<w::CollisionWorld>();
	world->collision = collision.get();
	world->ai.collision = collision.get();
	w::Entity building;
	building.kind = w::EntityKind::Building;
	building.position = {123.0f, 456.0f, 7.0f};
	building.heading = w::placed_heading_bam(90);
	building.bound_radius = 30;
	building.alive = true;
	const w::EntityHandle wall = world->registry.spawn(2, building);
	collision->assign_entity(wall, collision->add_model(wall_model()));
	collision->build_initial_tables(*world);
	ns::LoopbackChannel loop;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.max_players = 8;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                        inmatch::SocketMode::Socketless, /*host_key=*/0, &loop, settings);
	ctx.world = world.get();
	inmatch::Server_InitNewRoundState(ctx);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, *world) == 1,
	            "deploy: the host player is added")) return false;
	inmatch::test::round_init_own_body(ctx);
	inmatch::NapiNPConnection &own = ctx.np_protocol.connection_list.front();
	const w::EntityHandle body_h = world->cached.local_player;
	w::Entity *body = world->registry.get(body_h);
	if (!expect(body != nullptr && body->item_type_index == 1,
	            "deploy: the player is a pool-0 slice source")) return false;
	// The corpse 1.25 units off the wall's +Y face, its slice built there and its savedLivePose
	// stamped there.
	body->position.set_q16({123 * kOne, 457 * kOne + kOne / 2, 7 * kOne});
	for (int i = 0; i < 17; ++i) collision->build_tick_tables(*world);
	if (!expect(collision->candidate_count(body_h) >= 1, "deploy: the corpse's slice holds the wall"))
		return false;
	body->saved_live_pos[0] = 123 * kOne;
	body->saved_live_pos[1] = 457 * kOne + kOne / 2;
	body->saved_live_pos[2] = 7 * kOne;
	body->saved_live_valid = true;
	body->flags &= ~w::kEntityFlagInAir;
	body->engine_flags &= ~w::kEntityFlagInAir;
	body->alive = false;
	body->health = 0;
	body->flags |= w::kEntityFlagDead;
	body->engine_flags |= w::kEntityFlagDead;
	world->entity_update_counter = 1;
	inmatch::Server_ReleasePlayerDeployment(ctx.config, own, *world, w::EntityHandle{}, 0xFFFF);
	body = world->registry.get(body_h);
	if (!expect(body != nullptr && body->alive && (body->flags & w::kEntityFlagDead) == 0u,
	            "deploy: the body is deployed")) return false;
	if (!expect(body->position.x.q16 == 123 * kOne, "deploy: the body stands on the start's X"))
		return false;
	if (!expect(body->spawn_position.y.q16 == 456 * kOne,
	            "deploy: the reset's spawn backup is the placed pose")) return false;
	return expect(body->position.y.q16 > 456 * kOne,
	              "deploy: the reset's resolve pushes the body out of the wall along +Y");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_the_reset_resolves_a_live_body_out_of_a_wall();
	ok &= run_the_reset_on_a_dead_body_resolves_nothing();
	ok &= run_the_deploy_pushes_the_body_out_of_a_wall();
	if (!ok) return 1;
	std::printf("player_redeploy_reset_test: OK\n");
	return 0;
}
