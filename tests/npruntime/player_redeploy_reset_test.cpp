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
//     resolves nothing and requests no state: L keeps the corpse's channels and its first
//     ticks run on the death state (D-NET-NEW-L5-66; the arm's own pin is
//     inmatch_joiner_role's run_the_respawn_arm_resolves_nothing_in_the_receive), its death
//     edge gated on its flags alone.
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

#include <base/gameprofile/game_type.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h> // placed_heading_bam
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/infantry.h>
#include <runtime/world/infantry_internal.h> // player_body_death_edge_open
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include "../common/test_expect.h"

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

using test_expect::expect;

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

// The reset clears the being-revived word (+0x1E0) off the local player alone; the local
// player's is the round init's, so a reset without one (a teleport) leaves it set.
// [orig: Entity_ResetToSpawnState @0x4B9610 - `cmp esi, g_LocalPlayerEntity; jz`
//  @0x4B97FB..0x4B9801, `mov [esi+1E0h], ebx` @0x4B9803; Game_InitNewRound @0x422740 - @0x4227D4]
bool run_the_reset_leaves_the_local_players_revive_word() {
	Rig remote(/*dead=*/false);
	remote.entity().medic_reviving = true;
	w::entity_reset_to_spawn_state(*remote.world, remote.world->ai, remote.entity());
	if (!expect(!remote.entity().medic_reviving, "revive word: the reset clears a remote body's"))
		return false;
	Rig local(/*dead=*/false);
	local.world->cached.local_player = local.body;
	local.entity().medic_reviving = true;
	w::entity_reset_to_spawn_state(*local.world, local.world->ai, local.entity());
	return expect(local.entity().medic_reviving, "revive word: the reset leaves the local player's");
}

// The reset stores no kill-credit attacker (+0x178): the respawned body keeps its killer.
// [orig: Entity_ResetToSpawnState @0x4B9610..0x4B9900 (no +0x178 store)]
bool run_the_reset_keeps_the_kill_credit_attacker() {
	Rig r(/*dead=*/true);
	r.entity().last_attacker = r.wall; // any live row stands for the killer
	w::entity_reset_to_spawn_state(*r.world, r.world->ai, r.entity());
	return expect(r.entity().last_attacker == r.wall, "attacker: the reset keeps the body's killer");
}

// A pool-2 ladder: one CL (type-4) box, x -1..1, y -1..0.25, z -2..1.5 off the entity's
// origin, its first plane the +Y face (the ladder's facing).
w::CollisionModel ladder_model() {
	w::CollisionModel model;
	const auto plane = [&](int16_t x, int16_t y, int16_t z, int32_t dist) {
		w::CollisionPlane p;
		p.nx = x;
		p.ny = y;
		p.nz = z;
		p.dist = dist;
		model.planes.push_back(p);
	};
	plane(0, 16384, 0, -kOne / 4);
	plane(0, -16384, 0, -kOne);
	plane(16384, 0, 0, -kOne);
	plane(-16384, 0, 0, -kOne);
	plane(0, 0, 16384, -(kOne + kOne / 2));
	plane(0, 0, -16384, -2 * kOne);
	w::CollisionVolume volume;
	volume.type = w::bvol_type::kLadderCL;
	volume.min_x = -kOne;
	volume.max_x = kOne;
	volume.min_y = -kOne;
	volume.max_y = kOne / 4;
	volume.min_z = -2 * kOne;
	volume.max_z = kOne + kOne / 2;
	volume.plane_count = 6;
	model.volumes.push_back(volume);
	w::CollisionSection section;
	section.volume_count = 1;
	model.sections.push_back(section);
	return model;
}

// A live body reset on a ladder's CL box: the body off the anchor column at (8.5, 7.75, 0), its
// savedLivePose 1.5 units off the +Y face and 2 units up (above the anchor), its look level.
// Heap-held (the 1 MB test-main stack under MSVC).
struct LadderRig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<w::CollisionWorld> collision = std::make_unique<w::CollisionWorld>();
	Source source;
	w::EntityHandle body;

	explicit LadderRig(bool player) {
		world->registry.configure_pool(0, 8);
		world->registry.configure_pool(2, 4);
		world->collision = collision.get();
		world->ai.collision = collision.get();
		world->ai.root_motion = &source;
		world->ai.is_authority = true;
		w::PlayerSpawn spawn;
		spawn.position.x.q16 = 8 * kOne + kOne / 2;
		spawn.position.y.q16 = 7 * kOne + 3 * kOne / 4;
		spawn.position.z.q16 = 0;
		spawn.team = 1;
		body = w::spawn_remote_player(*world, spawn);
		w::Entity &e = entity();
		e.item_type_index = 3;
		e.bound_radius = 1;
		uint32_t cleared = w::kEntityFlagInAir | w::kEntityFlagLadderContact;
		if (!player) cleared |= w::kEntityFlagPlayer;
		e.flags &= ~cleared;
		e.engine_flags &= ~cleared;
		w::Entity ladder;
		ladder.kind = w::EntityKind::Building;
		ladder.position = {8.0f, 8.0f, 0.0f};
		ladder.heading = w::placed_heading_bam(90); // the identity rotation, Yaw word 0
		ladder.bound_radius = 30;
		ladder.alive = true;
		const w::EntityHandle ladder_h = world->registry.spawn(2, ladder);
		collision->assign_entity(ladder_h, collision->add_model(ladder_model()));
		for (int i = 0; i < 17; ++i) collision->build_tick_tables(*world);
		e.saved_live_pos[0] = 8 * kOne + kOne / 2;
		e.saved_live_pos[1] = 9 * kOne + kOne / 2;
		e.saved_live_pos[2] = 2 * kOne;
		e.saved_live_valid = true;
		world->ai.for_handle(body)->pitch = 0;
		world->entity_update_counter = 1;
	}

	w::Entity &entity() { return *world->registry.get(body); }
};

// The reset resolves through the tick's one resolver, its ladder entry included. A player body
// takes the fresh entry from above (no facing test; the level look agrees with the anchor below
// savedLivePose.Z): it is snapped onto the anchor column at the anchor less 0x1000 (the box's top
// less one unit, 0.5, so 0x7000), latched, and the same contact's alignment then moves it.
//   - The ladder's Yaw is its first plane's, -90 degrees: 0 - 0x40000000. Its trig runs through
//     the image's radians-per-BAM double (dbl_7C3608, about 30.5 ppm above 2*pi/2^32), so the
//     2^22-scaled sine is -4194303 (-4194304 at the 24-bit precision) and the cosine -201.
//   - The anchor is the box's mid (0, -0.375) on the ladder's (8, 8), pulled back 0x6000 along
//     the Yaw, each axis floored by the shrd: (8 + 2/65536, 8).
//   - The snap puts the body there; the facing press then moves it 0x1000 along the Yaw
//     ((cos22 * 0x1000) >> 22 = -1, (sin22 * 0x1000) >> 22 = -4096) and the sixty-fourth
//     chase back toward the anchor ((anchor - pos + 32) >> 6 = 0, 64): (8*65536 + 1,
//     8*65536 - 4032), whichever precision the trig ran at.
// The resolve takes no lift (no ground under it). A resolve without the climb channels latches
// on any CL touch but never snaps.
// [orig: Entity_ResetToSpawnState @0x4B9610 - the pushes @0x4B9752..0x4B9754, the call
//  @0x4B9755, the lift test @0x4B975D..0x4B9761; Entity_MovementCollisionResolver @0x4B2BD0 -
//  `sub edi, [esi+88h]` @0x4B327D, the player gate @0x4B329F..0x4B32D7 (its Pitch read
//  @0x4B32C8), the latch `or ecx, 100000h` @0x4B32DD stored @0x4B32E5, the column @0x4B32F2..
//  0x4B3308, the snap from above @0x4B330D..0x4B3319, the latched test @0x4B339C..0x4B33A4,
//  the press @0x4B340F..0x4B3469, the chase @0x4B346C..0x4B3495; Entity_ComputeBoneCollisionForce
//  @0x4AE150 - the CL anchor @0x4AE894..0x4AEA30 (the mid @0x4AE8A8..0x4AE8C6, the Yaw
//  @0x4AE97D..0x4AE993, the pull-back @0x4AE9C8..0x4AEA30); dbl_7C3608 @0x7C3608]
bool run_the_reset_resolve_enters_a_ladder() {
	LadderRig r(/*player=*/true);
	if (!expect(r.collision->candidate_count(r.body) >= 1, "ladder: the body's slice holds the ladder"))
		return false;
	w::entity_reset_to_spawn_state(*r.world, r.world->ai, r.entity());
	const w::Entity &e = r.entity();
	const w::LadderContact &frame = r.collision->last_ladder_frame;
	const bool column = e.position.x.q16 == 8 * kOne + 1 && e.position.y.q16 == 8 * kOne - 4032;
	const bool from_above = e.position.z.q16 == kOne / 2 - 0x1000;
	if (!column || !from_above)
		std::fprintf(stderr, "ladder: pose (%d, %d, %d), anchor (%d, %d, %d) yaw %d valid %d, flags 0x%x\n",
		             e.position.x.q16, e.position.y.q16, e.position.z.q16, frame.anchor[0],
		             frame.anchor[1], frame.anchor[2], frame.yaw, int(frame.valid),
		             static_cast<unsigned>(e.flags));
	if (!expect(column, "ladder: the entry snaps the body onto the anchor column, then the press and the chase"))
		return false;
	if (!expect(from_above, "ladder: from above, the body sits at the anchor less 0x1000")) return false;
	return expect((e.flags & w::kEntityFlagLadderContact) != 0,
	              "ladder: the entry latches the ladder");
}

// The fresh entry's gate: a body with no Player bit (0x100), no AI move order and no climb
// behavior (aiRuntime +4 & 0x400) never latches a CL volume it touches, nor snaps onto it.
// [orig: Entity_MovementCollisionResolver @0x4B2BD0 - the move order `jnz` @0x4B325D, the Player
//  bit @0x4B325F..0x4B3265, the climb behavior `jz` @0x4B3271]
bool run_the_reset_resolve_gates_a_non_climber_off_a_ladder() {
	LadderRig r(/*player=*/false);
	w::entity_reset_to_spawn_state(*r.world, r.world->ai, r.entity());
	const w::Entity &e = r.entity();
	if (!expect(e.position.x.q16 == 8 * kOne + kOne / 2 && e.position.z.q16 == 0,
	            "ladder gate: the non-climber is not snapped")) return false;
	return expect((e.flags & w::kEntityFlagLadderContact) == 0,
	              "ladder gate: the non-climber does not latch");
}

// Retail's reset neither reads nor stores MoveOrder: the stance bits are the player deploy's own
// clear, ahead of its call (V-ROLL-BF N1 on D-NET-NEW-L5-65), so a body the reset alone resets (a
// teleport, an NPC respawn, the joiner's arm) keeps them, and its motor mirror with them.
// [orig: Entity_ResetToSpawnState @0x4B9610..0x4B9900 (no +0x12C store); Server_DeployPlayer
//  @0x517740 - `and dword ptr [eax+12Ch], 0FFFFFDFFh` @0x517880, `and dword ptr [eax+12Ch],
//  0FFFFFEFFh` @0x51788C, ahead of the reset called @0x517899]
bool run_the_reset_leaves_the_stance_bits() {
	Rig r(/*dead=*/false);
	static constexpr uint8_t kCrouchBit = w::Entity::kMoveOrderCrouch >> 8;
	r.entity().net_stance_bits = kCrouchBit;
	r.motor().inf.stance = w::InfantryState::Stance::kCrouch;
	w::entity_reset_to_spawn_state(*r.world, r.world->ai, r.entity());
	return expect(r.entity().net_stance_bits == kCrouchBit &&
	                      r.motor().inf.stance == w::InfantryState::Stance::kCrouch,
	              "stance: the reset keeps the stance bits and their mirror");
}

// The host's deploy places a dead body on a Co-op start (6094) at (123, 456, 7), a quarter unit
// into a wall whose +Y face its corpse's slice holds, and clears the dead bit ahead of the reset:
// the deploy leaves the body pushed out along +Y, its X the start's. The Co-op marker arm
// places with no warm-up (the scored arm's warm-up resolves the body itself, ahead of the
// reset), so the reset's resolve is the one that pushes. The host's world is the authority and
// runs the root-motion clips; the body carries the bound radius its model's init stamps, which
// the contact force's clamp reads (a zero radius clamps every push to nothing).
// [orig: Server_PositionPlayerForSpawn @0x50CF60 - the Co-op marker arm's copy and its jump
//  past the warm-up @0x50D460, the scored arm's Entity_WarmUpOrganicAnimation @0x50D572]
// [orig: Entity_ComputeBoneCollisionForce @0x4AE150 - the clamp's `shl ecx, 7` @0x4AEEC6;
//  Entity_InitFromModel @0x40DC30 - the +0 store @0x40E076]
bool run_the_deploy_pushes_the_body_out_of_a_wall() {
	Source source;
	auto world = std::make_unique<w::World>();
	world->ai.root_motion = &source;
	world->ai.is_authority = true;
	world->tables.player.has_item_def = true;
	world->tables.player.item_type_index = 1;
	world->tables.player.item_type = 3;
	world->registry.configure_pool(0, 16);
	world->registry.configure_pool(2, 4);
	world->registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6094; // the Co-op primary start
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
	settings.game_type = opennova::game_type::kCoop;
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
	body->bound_radius = 1;
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
	// The corpse crouched: MoveOrder 0x200 and the motor's mirror of it.
	body->net_stance_bits = static_cast<uint8_t>(w::Entity::kMoveOrderCrouch >> 8);
	w::AiEntity *motor = world->ai.for_handle(body_h);
	if (!expect(motor != nullptr && motor->inf.active, "deploy: the player has a motor"))
		return false;
	motor->inf.stance = w::InfantryState::Stance::kCrouch;
	world->entity_update_counter = 1;
	inmatch::Server_ReleasePlayerDeployment(ctx.config, own, *world, w::EntityHandle{}, 0xFFFF);
	body = world->registry.get(body_h);
	if (!expect(body != nullptr && body->alive && (body->flags & w::kEntityFlagDead) == 0u,
	            "deploy: the body is deployed")) return false;
	if (!expect(body->net_stance_bits == 0 &&
	                    motor->inf.stance == w::InfantryState::Stance::kStand,
	            "deploy: the deploy's own clear stands the body")) return false;
	if (body->spawn_position.y.q16 != 456 * kOne || body->position.y.q16 <= 456 * kOne)
		std::fprintf(stderr, "deploy: spawn backup (%d, %d, %d), position (%d, %d, %d)\n",
		             body->spawn_position.x.q16, body->spawn_position.y.q16,
		             body->spawn_position.z.q16, body->position.x.q16, body->position.y.q16,
		             body->position.z.q16);
	if (!expect(body->position.x.q16 == 123 * kOne, "deploy: the body stands on the start's X"))
		return false;
	if (!expect(body->spawn_position.y.q16 == 456 * kOne,
	            "deploy: the reset's spawn backup is the placed pose")) return false;
	return expect(body->position.y.q16 > 456 * kOne,
	              "deploy: the reset's resolve pushes the body out of the wall along +Y");
}

// D-NET-NEW-L5-66: a player body's death edge opens on its flags on every peer, never on its
// animation state: a joiner's redeployed L runs its first ticks on the corpse's death state
// (0x82) with its dead bit clear, and a death then still takes the edge; the dead bit or the
// hidden bit closes it; a rowless test body keys the posed death clip.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the hidden-bit return @0x4B411E..0x4B4127,
//  Health <= 0 @0x4B4BF1..0x4B4BF8, the dead bit @0x4B4BFE..0x4B4C02]
bool run_a_joiner_bodys_death_edge_reads_its_flags() {
	Rig r(/*dead=*/false);
	r.world->ai.is_authority = false;
	w::AiEntity &ai = r.motor();
	ai.inf.reset_body_animation(174);
	w::Entity &e = r.entity();
	e.flags &= ~(w::kEntityFlagDead | w::kEntityFlagCarried);
	e.engine_flags &= ~(w::kEntityFlagDead | w::kEntityFlagCarried);
	if (!expect(w::player_body_death_edge_open(r.world->ai, ai, &e),
	            "edge gate: open on a death state with the dead and hidden bits clear")) return false;
	e.flags |= w::kEntityFlagDead;
	if (!expect(!w::player_body_death_edge_open(r.world->ai, ai, &e),
	            "edge gate: the dead bit closes it")) return false;
	e.flags &= ~w::kEntityFlagDead;
	e.flags |= w::kEntityFlagCarried;
	if (!expect(!w::player_body_death_edge_open(r.world->ai, ai, &e),
	            "edge gate: the hidden bit closes it")) return false;
	return expect(!w::player_body_death_edge_open(r.world->ai, ai, nullptr),
	              "edge gate: a rowless body keys the posed death clip");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_the_reset_resolves_a_live_body_out_of_a_wall();
	ok &= run_the_reset_on_a_dead_body_resolves_nothing();
	ok &= run_the_reset_leaves_the_stance_bits();
	ok &= run_the_reset_resolve_enters_a_ladder();
	ok &= run_the_reset_resolve_gates_a_non_climber_off_a_ladder();
	ok &= run_the_reset_leaves_the_local_players_revive_word();
	ok &= run_the_reset_keeps_the_kill_credit_attacker();
	ok &= run_the_deploy_pushes_the_body_out_of_a_wall();
	ok &= run_a_joiner_bodys_death_edge_reads_its_flags();
	if (!ok) return 1;
	std::printf("player_redeploy_reset_test: OK\n");
	return 0;
}
