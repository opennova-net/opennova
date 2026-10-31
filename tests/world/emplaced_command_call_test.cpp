// The ewep update's tank-command call, the window leg's tail while an occupant rides the gun:
// with the local player in a UseGun seat of a gun on this gun's ground entity, an occupant
// holding the packed jump bit (MoveOrder 0x20) more than 124 ticks past the gun's last call
// (+0x31C) re-stamps it, and an occupant other than the local player plays its TANK_COMAND
// composite on the interface channel and posts HUD_COM2GUN to the CHAT ring in yellow.
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - @0x4412B6..0x44135C]

#include <runtime/world/entity.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include "../common/title_fixture.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
	do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// A carrier with two guns on it, the local player in gun A's UseGun seat and a remote
// occupant riding gun B.
struct Rig {
	std::unique_ptr<World> wp = std::make_unique<World>();
	bool wp_jo = test_titles::jo_world(*wp);
	World &w = *wp;
	EntityHandle carrier, gun_a, gun_b, local, rider;

	Rig() {
		w.registry.configure_pool(0, 4);
		w.registry.configure_pool(1, 8);
		Entity hull;
		hull.kind = EntityKind::Item;
		carrier = w.registry.spawn(1, hull);
		Entity gun;
		gun.kind = EntityKind::Item;
		gun.emplaced_update = true;
		gun.ground_target = carrier;
		gun_a = w.registry.spawn(1, gun);
		gun_b = w.registry.spawn(1, gun);
		Entity body;
		body.kind = EntityKind::Organic;
		body.mounted = true;
		body.mount_target = gun_a;
		body.mount_type = SeatType::Gunner;
		local = w.registry.spawn(0, body);
		w.cached.local_player = local;
		Entity other;
		other.kind = EntityKind::Organic;
		other.anim_slot = 7; // the BF1 body
		rider = w.registry.spawn(0, other);
	}
	Entity &b() { return *w.registry.get(gun_b); }
	Entity &l() { return *w.registry.get(local); }
	void call(uint8_t move_order, bool rider_is_local = false) {
		emplaced_command_call(w, b().ground_target, b().emplaced_command_stamp, rider_is_local,
				move_order, 7);
	}
	size_t lines() const { return w.out.hud_chat_lines.size(); }
	size_t sounds() const { return w.out.script_sounds.size(); }
};

// The first call past tick 124 posts the line and the occupant's composite, then the gun's
// stamp holds the next for 125 ticks (`sub edx, [ebp+31Ch]; cmp edx, 7Ch; jle`).
void test_the_call_posts_and_restamps() {
	Rig r;
	r.w.logic_tick = 124;
	r.call(Entity::kMoveOrderJump);
	CHECK(r.lines() == 0 && r.sounds() == 0 && r.b().emplaced_command_stamp == 0);
	r.w.logic_tick = 125;
	r.call(Entity::kMoveOrderJump);
	CHECK(r.b().emplaced_command_stamp == 125);
	CHECK(r.lines() == 1);
	if (r.lines() == 1) {
		const HudChatLine &line = r.w.out.hud_chat_lines[0];
		CHECK(std::strcmp(line.section, "HUD") == 0);
		CHECK(std::strcmp(line.key, "HUD_COM2GUN") == 0);
		CHECK(line.argb == 0xFFF0F000u); // g_HUDColors+0x10
	}
	CHECK(r.sounds() == 1);
	if (r.sounds() == 1) {
		CHECK(r.w.out.script_sounds[0].name == "BF1_TANK_COMAND");
		CHECK(r.w.out.script_sounds[0].kind == ScriptSoundEvent::Kind::Interface);
	}
	r.w.logic_tick = 249;
	r.call(Entity::kMoveOrderJump);
	CHECK(r.lines() == 1 && r.b().emplaced_command_stamp == 125);
	r.w.logic_tick = 250;
	r.call(Entity::kMoveOrderJump | Entity::kMoveOrderMoving);
	CHECK(r.lines() == 2 && r.sounds() == 2 && r.b().emplaced_command_stamp == 250);
}

// No jump bit, no call and no stamp.
void test_no_jump_bit_no_call() {
	Rig r;
	r.w.logic_tick = 1000;
	r.call(Entity::kMoveOrderMoving | Entity::kMoveOrderLeanLeft);
	CHECK(r.lines() == 0 && r.sounds() == 0 && r.b().emplaced_command_stamp == 0);
}

// The local player's seat: only a UseGun seat (attach type 3) of a gun on this gun's ground.
void test_the_local_seat_gates() {
	{
		Rig r;
		r.w.logic_tick = 1000;
		r.l().mount_type = SeatType::Driver;
		r.call(Entity::kMoveOrderJump);
		CHECK(r.lines() == 0 && r.b().emplaced_command_stamp == 0);
	}
	{
		Rig r;
		r.w.logic_tick = 1000;
		r.b().ground_target = {};
		r.call(Entity::kMoveOrderJump);
		CHECK(r.lines() == 0 && r.b().emplaced_command_stamp == 0);
	}
	{
		// Two guns with no ground entity compare equal (`cmp eax, [ebp+28h]` over two nulls).
		Rig r;
		r.w.logic_tick = 1000;
		r.w.registry.get(r.gun_a)->ground_target = {};
		r.b().ground_target = {};
		r.call(Entity::kMoveOrderJump);
		CHECK(r.lines() == 1 && r.b().emplaced_command_stamp == 1000);
	}
	{
		Rig r;
		r.w.logic_tick = 1000;
		r.w.cached.local_player = {};
		r.call(Entity::kMoveOrderJump);
		CHECK(r.lines() == 0 && r.b().emplaced_command_stamp == 0);
	}
}

// The local player's own call re-stamps the gun and posts nothing.
void test_the_local_occupant_only_restamps() {
	Rig r;
	r.w.logic_tick = 600;
	r.call(Entity::kMoveOrderJump, /*rider_is_local=*/true);
	CHECK(r.b().emplaced_command_stamp == 600);
	CHECK(r.lines() == 0 && r.sounds() == 0);
}

// The occupied channel runs the call only for a gun whose update is the ewep row's.
void test_the_channel_gates_on_the_ewep_row() {
	Rig r;
	r.w.logic_tick = 1000;
	r.b().emplaced_update = false;
	Entity &rider = *r.w.registry.get(r.rider);
	rider.net_move_input = Entity::kMoveOrderJump;
	AiEntity gunner;
	tick_emplaced_weapon_channel(r.w, r.b(), rider, gunner);
	CHECK(r.lines() == 0 && r.b().emplaced_command_stamp == 0);
}

// An ewep gun with no slot Def (+0x2D4) ends its update ahead of the window leg: its authored
// arc pins no occupant look. A mount whose update is not the ewep row keeps the channel's clamp
// (that path is not this leg's). [orig: Entity_UpdateTransformAndTurret @0x440CA0 - @0x440EA0,
// @0x4411C9..0x4411CB]
void test_an_ewep_gun_without_a_slot_def_runs_no_window() {
	constexpr int32_t kBamPerDegree = 11930464;
	for (const bool ewep : {true, false}) {
		Rig r;
		Entity &gun = r.b();
		gun.emplaced_update = ewep;
		gun.primary_weapon_slot_adm = kAdmSlotNone; // no slot Def
		gun.heading = 0x40000000;
		gun.emplacement_right_limit_bam = 30 * kBamPerDegree;
		gun.emplacement_left_limit_bam = -30 * kBamPerDegree;
		gun.emplacement_down_limit_bam = 20 * kBamPerDegree;
		gun.emplacement_up_limit_bam = -10 * kBamPerDegree;
		const Entity &rider = *r.w.registry.get(r.rider);
		AiEntity gunner;
		gunner.heading = 0; // 90 degrees off the gun, far past its 30-degree arc
		gunner.inf.target_heading = 0;
		tick_emplaced_weapon_channel(r.w, gun, rider, gunner);
		CHECK((gunner.heading == 0) == ewep);
	}
}

} // namespace

int main() {
	test_an_ewep_gun_without_a_slot_def_runs_no_window();
	test_the_call_posts_and_restamps();
	test_no_jump_bit_no_call();
	test_the_local_seat_gates();
	test_the_local_occupant_only_restamps();
	test_the_channel_gates_on_the_ewep_row();
	if (failures == 0) std::puts("emplaced_command_call: OK");
	return failures == 0 ? 0 : 1;
}
