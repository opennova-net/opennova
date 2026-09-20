// The per-entity death state retail keeps beside the infantry death edges
// (world/infantry.h): the plyr class callback's 64-tick think cadence and its
// kill-cause bit clear, the edge's attacker fallback, and the org0 skin
// callback's DEATH register (the corpse fade).
#include <cstdint>
#include <cstdio>
#include <memory>

#include <runtime/world/ai.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/system.h>
#include <runtime/world/world.h>

#include "death_clip_source.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

using test_world::DeathClipSource;

void run_ticks(World &world, uint32_t from, uint32_t to_excl) {
	TickContext ctx;
	ctx.world = &world;
	ctx.is_authority = true;
	for (uint32_t t = from; t < to_excl; ++t) {
		ctx.logic_tick = t;
		world.ai.tick(world, ctx);
	}
}

// The org0 skin callback's DEATH register: 0xFFFF for a live body or a corpse
// still above move timer 248, then (max(0, timer - 62) << 16) / 186 down to
// 0 for the corpse's last 62 ticks. [orig: BoneCallback_org0_Skin
// @0x4e3669..0x4e368e]
void test_death_ctrl_register_formula() {
	CHECK(death_ctrl_register_value(false, 100) == 0xFFFF);
	CHECK(death_ctrl_register_value(false, 0) == 0xFFFF);
	CHECK(death_ctrl_register_value(true, 500) == 0xFFFF);
	CHECK(death_ctrl_register_value(true, 248) == 0xFFFF);
	CHECK(death_ctrl_register_value(true, 247) == (185 << 16) / 186); // 65183
	CHECK(death_ctrl_register_value(true, 247) == 65183);
	CHECK(death_ctrl_register_value(true, 100) == (38 << 16) / 186);  // 13389
	CHECK(death_ctrl_register_value(true, 63) == (1 << 16) / 186);    // 352
	CHECK(death_ctrl_register_value(true, 62) == 0);
	CHECK(death_ctrl_register_value(true, 0) == 0);
	CHECK(death_ctrl_register_value(true, -5) == 0);
}

// The plyr class callback's non-hit event on a live player body clears cause
// bits 8..11 and re-arms the 64-tick think; a dead body returns first.
// [orig: Entity_HandleDamageAndTriggerZones @0x40772f; @0x407b4d..0x407b4f;
//  @0x407b5e / @0x407c71]
void test_player_body_class_think_clears_and_rearms() {
	Entity live;
	live.flags = kEntityFlagPlayer;
	live.engine_flags = kEntityFlagPlayer;
	live.cause_flags = 0xF0Fu;
	live.spawn_phase = -3;
	player_body_class_think(live);
	CHECK(live.cause_flags == 0x00Fu);
	CHECK(live.spawn_phase == 64);

	Entity dead = live;
	dead.cause_flags = 0x800u;
	dead.spawn_phase = 0;
	dead.flags |= kEntityFlagDead;
	dead.engine_flags |= kEntityFlagDead;
	player_body_class_think(dead);
	CHECK(dead.cause_flags == 0x800u);
	CHECK(dead.spawn_phase == 0);
}

// The org2 body update fires the think whenever entity+0x2AC is <= 0 and
// decrements it every tick, ahead of the death edge; a corpse keeps counting
// down but its callback no longer clears. [orig: Entity_UpdateInfantryPlayerBody
// @0x4b4bc9..0x4b4be9]
void test_player_body_think_cadence() {
	auto world_owner = std::make_unique<World>();
	World &world = *world_owner;
	world.registry.configure_pool(0, 4);
	world.tables.player.has_item_def = true;
	world.tables.player.item_type = 3;
	world.tables.player.item_hp = 100;
	DeathClipSource clips;
	world.ai.is_authority = true;
	world.ai.root_motion = &clips;

	PlayerSpawn spawn;
	spawn.position = {5.0f, 0.0f, 0.0f};
	spawn.net_id = 0xFFF1;
	const EntityHandle handle = spawn_remote_player(world, spawn);
	CHECK(handle.valid());
	Entity *ent = world.registry.get(handle);
	AiEntity *body = world.ai.for_handle(handle);
	CHECK(ent != nullptr && body != nullptr);
	if (ent == nullptr || body == nullptr) return;
	body->net_is_remote_peer = true;

	// A fresh body starts at 0: the first tick fires the think (clearing a
	// latched bit), re-arms to 64, then decrements.
	ent->cause_flags = 0x800u;
	CHECK(ent->spawn_phase == 0);
	run_ticks(world, 1, 2);
	CHECK(ent->cause_flags == 0);
	CHECK(ent->spawn_phase == 63);

	// A bit latched right after that think survives 63 more ticks and the
	// 64th tick (counter back at 0) still does not fire; the next one does.
	ent->cause_flags = 0x900u;
	run_ticks(world, 2, 65);
	CHECK(ent->spawn_phase == 0);
	CHECK(ent->cause_flags == 0x900u);
	run_ticks(world, 65, 66);
	CHECK(ent->spawn_phase == 63);
	CHECK(ent->cause_flags == 0);

	// A hit-style re-arm mid-window (the event-1 leg) pushes the clear out.
	ent->cause_flags = 0x800u;
	run_ticks(world, 66, 96);
	CHECK(ent->spawn_phase == 33);
	ent->spawn_phase = 64;
	run_ticks(world, 96, 160);
	CHECK(ent->spawn_phase == 0);
	CHECK(ent->cause_flags == 0x800u);
	run_ticks(world, 160, 161);
	CHECK(ent->cause_flags == 0);

	// A corpse: the counter keeps decrementing below zero, the callback
	// returns on the dead bit, so the latched bit stays for the death edge.
	ent->cause_flags = 0x800u;
	ent->health = 0;
	body->health = 0;
	ent->flags |= kEntityFlagDead;
	ent->engine_flags |= kEntityFlagDead;
	run_ticks(world, 161, 231);
	CHECK(ent->spawn_phase < 0);
	CHECK(ent->cause_flags == 0x800u);
}

// An NPC organic never runs the plyr cadence: its helper phase stays put.
void test_npc_body_keeps_its_spawn_phase() {
	auto world_owner = std::make_unique<World>();
	World &world = *world_owner;
	world.registry.configure_pool(0, 4);
	DeathClipSource clips;
	world.ai.is_authority = true;
	world.ai.root_motion = &clips;
	Entity seed;
	seed.kind = EntityKind::Organic;
	seed.health = 100;
	seed.spawn_phase = 63;
	seed.cause_flags = 0x800u;
	const EntityHandle handle = world.registry.spawn(0, seed);
	CHECK(handle == EntityHandle::make(0, 0));
	const int idx = world.ai.attach(handle);
	AiEntity *body = world.ai.at(idx);
	body->inf.active = true;
	body->health = 100;
	run_ticks(world, 1, 70);
	Entity *ent = world.registry.get(handle);
	CHECK(ent != nullptr && ent->spawn_phase == 63);
	CHECK(ent != nullptr && ent->cause_flags == 0x800u);
}

// The infantry death edge's fallback: nothing staged in +0x2C0 plays the
// generic 174 AND clears the attacker slot; a staged selection keeps both.
// [orig: Entity_UpdateInfantryAI @0x4b9cc9..0x4b9cf1 (+0x178 = 0 @0x4b9ceb);
//  Entity_UpdateInfantryPlayerBody @0x4b4c72..0x4b4c8d]
void test_death_edge_fallback_clears_the_attacker() {
	{
		auto world_owner = std::make_unique<World>();
		World &world = *world_owner;
		world.registry.configure_pool(0, 4);
		DeathClipSource clips;
		world.ai.is_authority = true;
		world.ai.root_motion = &clips;
		Entity seed;
		seed.kind = EntityKind::Organic;
		seed.health = 0;
		seed.deathtime_ticks = 100;
		seed.death_anim_state = 0;
		seed.last_attacker = EntityHandle::make(0, 1);
		const EntityHandle handle = world.registry.spawn(0, seed);
		CHECK(handle == EntityHandle::make(0, 0));
		const int idx = world.ai.attach(handle);
		AiEntity *body = world.ai.at(idx);
		body->inf.active = true;
		run_ticks(world, 1, 2);
		Entity *ent = world.registry.get(handle);
		CHECK(body->inf.anim_state == anim_state::kDeathPungi);
		CHECK(ent != nullptr && !ent->last_attacker.valid());
		CHECK(ent != nullptr && ent->death_anim_state == 0);
	}
	{
		auto world_owner = std::make_unique<World>();
		World &world = *world_owner;
		world.registry.configure_pool(0, 4);
		DeathClipSource clips;
		world.ai.is_authority = true;
		world.ai.root_motion = &clips;
		Entity seed;
		seed.kind = EntityKind::Organic;
		seed.health = 0;
		seed.deathtime_ticks = 100;
		seed.death_anim_state = 189; // death_bullet_head_right, staged by a hit
		seed.last_attacker = EntityHandle::make(0, 1);
		const EntityHandle handle = world.registry.spawn(0, seed);
		CHECK(handle == EntityHandle::make(0, 0));
		const int idx = world.ai.attach(handle);
		AiEntity *body = world.ai.at(idx);
		body->inf.active = true;
		run_ticks(world, 1, 2);
		Entity *ent = world.registry.get(handle);
		CHECK(body->inf.anim_state == 189);
		CHECK(ent != nullptr && ent->last_attacker == EntityHandle::make(0, 1));
		CHECK(ent != nullptr && ent->death_anim_state == 0); // consumed
	}
}

} // namespace

int main() {
	test_death_ctrl_register_formula();
	test_player_body_class_think_clears_and_rearms();
	test_player_body_think_cadence();
	test_npc_body_keeps_its_spawn_phase();
	test_death_edge_fallback_clears_the_attacker();
	if (failures == 0) std::printf("death_state_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
