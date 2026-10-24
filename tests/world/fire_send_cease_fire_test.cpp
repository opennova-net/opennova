// The fire send's cease-fire head (D-NET-NEW-LI-14, LI-s17): every shot that reaches the send
// under a cease fire leaves no round, no ring record (the S2C 0x0A) and no C2S 0x06, while the
// slot's fire action runs all the same (its round spent, the FIRE action begun): the local
// player's fire, the host's and a joiner's alike, and an AI gunner's.
// [orig: Entity_FireWeaponAndSendPacket @0x42BD80 - g_InCeaseFire @0x42BDB3, the exit
//  @0x42BDC5 -> @0x42C07A; WeaponAction_Fire @0x542B10 - the send's call @0x542C5E, whose
//  return nothing reads, then Weapon_ConsumeAmmo @0x542C75]
#include <cstdio>
#include <memory>

#include <runtime/world/ai.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::def;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

struct Rig {
	std::unique_ptr<World> storage = std::make_unique<World>();
	World &world = *storage;
	LocalPlayer local{world};
	EntityHandle shooter;

	Rig() {
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(1, 8);
		Entity e;
		e.kind = EntityKind::Organic;
		e.item_type = 3;
		e.item_id = 1;
		e.alive = true;
		e.health = 100;
		e.flags = kEntityFlagPlayer;
		e.team = 1;
		e.position = {10, 20, 3};
		e.equipped_adm_index = 1;
		shooter = world.registry.spawn(0, e);
		world.cached.local_player = shooter;
		world.local_player_state = &local;
		world.ai.attach(shooter);
		AiEntity &body = *world.ai.for_handle(shooter);
		body.inf.active = true;
		body.inf.is_local_player = true;
		body.self->position.x.q16 = 10 * 65536;
		body.self->position.y.q16 = 20 * 65536;
		body.self->position.z.q16 = 3 * 65536;
		world.tables.weapons.entries.resize(2);
		auto &weapon = world.tables.weapons.entries[1];
		weapon.valid = true;
		weapon.name = "WPN_SENDTEST";
		weapon.ammo_index = 1;
		world.tables.ammo.entries.resize(2);
		auto &ammo = world.tables.ammo.entries[1];
		ammo.valid = true;
		ammo.velocity = 248;
		ammo.max_age_ticks = 200;
		DefWeaponDef authored{};
		WeaponInstallData data = weapon_install_data_from_def(authored);
		data.name = "WPN_SENDTEST";
		data.clipsize = 4;
		data.rows.resize(3);
		std::snprintf(data.rows[0].name, sizeof(data.rows[0].name), "idle");
		std::snprintf(data.rows[1].name, sizeof(data.rows[1].name), "fire");
		data.rows[1].delayend = 6;
		std::snprintf(data.rows[2].name, sizeof(data.rows[2].name), "recoil");
		local_weapon_install(world, local.weapon, data, false, false, nullptr, local.view);
	}

	LocalWeaponPumpIO fire(bool authority) {
		local_weapon_set_input(local.weapon, local.view, true, true, false);
		LocalWeaponPumpIO io;
		io.view = &local.view;
		io.is_authority = authority;
		io.self_wire_handle = shooter.packed;
		local_weapon_pump_tick(world, local.weapon, io);
		return io;
	}
};

void test_the_local_send_refuses_under_a_cease_fire() {
	for (const bool authority : {true, false}) {
		Rig r;
		r.world.rules.cease_fire = true;
		const int32_t clip = r.local.weapon.slot.clip;
		const LocalWeaponPumpIO io = r.fire(authority);
		// The fire action ran: its event, its round spent.
		CHECK(r.local.weapon.fired_serial == 1);
		CHECK(r.local.weapon.slot.clip == clip - 1);
		// The send refused it: no ring record, no C2S 0x06, no round.
		CHECK(r.world.out.rounds.count == 0);
		CHECK(!io.fired.valid);
		CHECK(r.world.round_sim.active_count == 0);
		CHECK(r.local.weapon.pending_throw_charge == 0);
	}
	for (const bool authority : {true, false}) {
		Rig r; // the same shot with no cease fire reaches the send
		const LocalWeaponPumpIO io = r.fire(authority);
		CHECK(authority ? r.world.out.rounds.count == 1 : io.fired.valid);
		CHECK(r.world.round_sim.active_count == 1);
	}
}

void test_an_ai_gunners_send_refuses_under_a_cease_fire() {
	Rig r;
	AiEntity &gunner = *r.world.ai.for_handle(r.shooter);
	const int32_t origin[3] = {10 * 65536, 20 * 65536, 3 * 65536};
	r.world.rules.cease_fire = true;
	// The fire action's legs stand (true); the send leaves no round and no ring record.
	CHECK(r.world.ai.fire_ai_round(r.world, gunner, origin, 0, 0, 1, 2, 1));
	CHECK(r.world.out.rounds.count == 0);
	CHECK(r.world.round_sim.active_count == 0);
	r.world.rules.cease_fire = false;
	CHECK(r.world.ai.fire_ai_round(r.world, gunner, origin, 0, 0, 1, 2, 1));
	CHECK(r.world.out.rounds.count == 1);
	CHECK(r.world.round_sim.active_count == 1);
}

} // namespace

int main() {
	test_the_local_send_refuses_under_a_cease_fire();
	test_an_ai_gunners_send_refuses_under_a_cease_fire();
	if (failures != 0) {
		std::printf("fire_send_cease_fire: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("fire_send_cease_fire: ok\n");
	return 0;
}
