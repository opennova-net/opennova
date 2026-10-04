// The local player's equipped-weapon snapshot (world/player_weapon.h
// local_player_weapon_view), pinned where it used to live in the Godot binding
// (ADR 0040 ladder E0): the inactive default, the slot and serial copies, the
// action legs, the counter-gated FP clip position, the PowerThrow windup gates
// [orig: HUD_DrawPowerThrowChargeBar @0x599830], the crosshair spread rows and
// shifts [orig: HUD_DrawCrosshair @0x592b07..0x592b87], the two heat clamps
// [orig: HUD_BuildEntityInfo @0x4B852E; Player_RenderFirstPersonViewModel
// @0x4DEEC2], the round-ring read-back and the body channel's off state.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <base/io/bam.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A world with one live local organic that has an AI body, and one weapon
// def whose six error rows are distinct.
struct Rig {
	World w;
	EntityHandle local;
	LocalPlayerWeapon weapon;
	WeaponInventory inventory;

	Rig() {
		w.registry.configure_pool(0, 8);
		Entity seed;
		seed.kind = EntityKind::Organic;
		seed.item_id = 0x14B9;
		seed.net_id = 1;
		seed.position = {10.0f, 20.0f, 3.0f};
		seed.alive = true;
		seed.health = 100;
		seed.equipped_adm_index = 1;
		local = w.registry.spawn(0, seed);
		w.cached.local_player = local;
		w.ai.attach(local);

		w.tables.weapons.entries.resize(2);
		WeaponTableEntry &def = w.tables.weapons.entries[1];
		def.valid = true;
		for (int i = 0; i < 6; ++i) def.error_fp16[i] = 100 * (i + 1);

		weapon.active = true;
		weapon.slot.current = weapon_action::kFire;
		weapon.slot.next = weapon_action::kIdle;
		weapon.slot.phase = 2;
		weapon.slot.clip = 17;
		weapon.slot.reserve = 90;
		weapon.slot.kick = 3;
		weapon.slot.tracer_shot_counter = 4;
		weapon.switch_deferred_action = 5;
		weapon.switch_in_flight = true;
		weapon.anim_key = "anim_wpn_fire";
		weapon.anim_variant = 2;
		weapon.anim_advance_ticks = 9;
		weapon.play_serial = 11;
		weapon.action_serial = 12;
		weapon.action_started = weapon_action::kFire;
		weapon.action_end_serial = 13;
		weapon.action_finished = weapon_action::kFire;
		std::strncpy(weapon.def.actions[weapon_action::kFire].soundset, "GS_M4", 127);
		std::strncpy(weapon.def.actions[weapon_action::kFire].particle, "flash_m4", 127);
		std::strncpy(weapon.def.actions[weapon_action::kFire].particle_userpoint, "muzzle", 127);
		std::strncpy(weapon.def.actions[weapon_action::kFire].soundsetend, "GS_M4_END", 127);
		weapon.fired_serial = 21;
		weapon.dry_serial = 22;
		weapon.reload_serial = 23;
		weapon.reload_applied_serial = 24;
		weapon.reload_received_serial = 25;
		weapon.reload_received_entity = 26;
		weapon.reload_received_param = 27;
		weapon.unscope_serial = 28;
		weapon.rescope_serial = 29;
		weapon.usegun_slot_active = true;
		inventory.pending_combo = 3;
	}
	Entity &entity() { return *w.registry.get(local); }
	AiEntity &body() { return *w.ai.for_handle(local); }
	LocalPlayerWeaponView view() const { return local_player_weapon_view(w, weapon, inventory); }
};

void test_inactive_reads_as_defaults() {
	Rig rig;
	rig.weapon.active = false;
	const LocalPlayerWeaponView v = rig.view();
	CHECK(!v.active);
	CHECK(v.current_action == 0);
	CHECK(v.clip == 0);
	CHECK(v.anim_key.empty());
	CHECK(v.hud_spread_row == 0);
	CHECK(v.usegun_mount_handle == EntityHandle::kInvalid);
}

void test_slot_serials_and_action_legs() {
	Rig rig;
	const LocalPlayerWeaponView v = rig.view();
	CHECK(v.active);
	CHECK(v.current_action == weapon_action::kFire);
	CHECK(v.next_action == weapon_action::kIdle);
	CHECK(v.phase == 2);
	CHECK(v.switch_deferred_action == 5);
	CHECK(v.switch_in_flight);
	CHECK(v.pending_combo == 3);
	CHECK(v.anim_key == "anim_wpn_fire");
	CHECK(v.anim_variant == 2);
	CHECK(v.anim_advance_ticks == 9);
	CHECK(v.play_serial == 11);
	CHECK(v.action_serial == 12);
	CHECK(v.action_started == weapon_action::kFire);
	CHECK(v.action_soundset == "GS_M4");
	CHECK(v.action_particle == "flash_m4");
	CHECK(v.action_particle_userpoint == "muzzle");
	CHECK(v.action_end_serial == 13);
	CHECK(v.action_end_soundset == "GS_M4_END");
	CHECK(v.fired_serial == 21 && v.dry_serial == 22 && v.reload_serial == 23);
	CHECK(v.reload_applied_serial == 24 && v.reload_received_serial == 25);
	CHECK(v.reload_received_entity == 26 && v.reload_received_param == 27);
	CHECK(v.unscope_serial == 28 && v.rescope_serial == 29);
	CHECK(v.clip == 17 && v.reserve == 90 && v.kick == 3);
	CHECK(v.tracer_counter == 4);
	CHECK(v.borrowed_usegun_slot);
	CHECK(!v.emplaced_controls_valid);
	CHECK(v.round_ring_count == 0);
	CHECK(v.body_anim_key.empty());
	CHECK(v.body_anim_blend_weight == 1.0f);

	// No action started / finished: the legs read empty.
	rig.weapon.action_started = -1;
	rig.weapon.action_finished = weapon_action::kCount;
	const LocalPlayerWeaponView off = rig.view();
	CHECK(off.action_started == -1);
	CHECK(off.action_soundset.empty() && off.action_particle.empty());
	CHECK(off.action_end_soundset.empty());

	// The FP clip position is 0 without a clip key, whatever the counter says.
	rig.weapon.anim_key.clear();
	CHECK(rig.view().anim_advance_ticks == 0);
}

void test_effect_anchor_tracks_the_committed_usegun_slot() {
	Rig rig;
	rig.w.registry.configure_pool(1, 2);
	Entity gun;
	gun.has_item_def = true;
	gun.item_attrib = kItemAttribEweap;
	gun.primary_weapon_slot.clip = 33;
	const EntityHandle first = rig.w.registry.spawn(1, gun);
	const EntityHandle pending = rig.w.registry.spawn(1, gun);
	rig.weapon.usegun_mount = first;
	rig.weapon.usegun_pending_mount = pending;
	CHECK(rig.view().clip == 33);
	CHECK(rig.view().usegun_mount_handle == first.packed);
	rig.weapon.usegun_mount = pending;
	CHECK(rig.view().usegun_mount_handle == pending.packed);
	rig.weapon.usegun_slot_active = false;
	CHECK(rig.view().clip == 17);
	CHECK(rig.view().usegun_mount_handle == EntityHandle::kInvalid);
}

// The def half of the pump's FP bit: a loaded gfx1 model AND its animadm; an
// arms-only viewmodel (no gun model) or a gun without an anim map is not an
// FP action model. [orig: WeaponAction_ProcessFrame @0x540EA5 (Def+0x16C),
// @0x540EC3 (Def+0x174)]
void test_first_person_action_model_needs_gfx1_and_animadm() {
	Rig rig;
	rig.weapon.first_person_model_adm = 0xFF;
	rig.weapon.anim_map = "m4.adm";
	CHECK(!rig.view().first_person_action_model);
	rig.weapon.first_person_model_adm = 1;
	rig.weapon.anim_map.clear();
	CHECK(!rig.view().first_person_action_model);
	rig.weapon.anim_map = "m4.adm";
	CHECK(rig.view().first_person_action_model);
}

void test_power_throw_windup() {
	Rig rig;
	rig.w.logic_tick = 25;
	rig.weapon.def.flags = weapon_flag::kPowerThrow;
	rig.weapon.power_throw_start_tick = 10;
	LocalPlayerWeaponView v = rig.view();
	CHECK(v.windup_active);
	CHECK(v.windup_held_ticks == 15);
	// No ammo and a finite clip: no windup; an infinite clip winds up anyway.
	rig.weapon.slot.clip = 0;
	v = rig.view();
	CHECK(!v.windup_active && v.windup_held_ticks == 0);
	rig.weapon.def.clip_capacity = -1;
	CHECK(rig.view().windup_active);
	// The def's clip size rides beside the clip (the magazine registers'
	// denominator, D-3DI-7).
	CHECK(rig.view().clip_capacity == -1);
	rig.weapon.def.clip_capacity = 30;
	CHECK(rig.view().clip_capacity == 30);
	rig.weapon.def.clip_capacity = -1;
	// The start tick and the def bit are both gates.
	rig.weapon.power_throw_start_tick = 0;
	CHECK(!rig.view().windup_active);
	rig.weapon.power_throw_start_tick = 10;
	rig.weapon.def.flags = 0;
	CHECK(!rig.view().windup_active);
}

void test_crosshair_spread_rows() {
	Rig rig;
	AiEntity &body = rig.body();
	body.inf.recoil_pitch = 0x1000;
	body.inf.weapon_weight_spread = 0x800;
	body.inf.aimed_shot_available = true;
	body.inf.stance = InfantryState::Stance::kCrouch;
	LocalPlayerWeaponView v = rig.view();
	CHECK(v.recoil_pitch_bam == 0x1000);
	CHECK(v.weapon_weight_spread_bam == 0x800);
	CHECK(v.aimed_shot_available);
	CHECK(v.hud_spread_row == 4); // crouch + the aimed triplet
	const int32_t expected = opennova::io::bam_add(500,
			opennova::io::bam_add(opennova::io::bam_sar(0x1000, 7), opennova::io::bam_sar(0x800, 7)));
	CHECK(v.hud_spread_fp16 == expected);
	// Prone, un-aimed: row 0.
	body.inf.stance = InfantryState::Stance::kProne;
	body.inf.aimed_shot_available = false;
	v = rig.view();
	CHECK(v.hud_spread_row == 0);
	// An eye under the water plane forces stand, and the compare is raw: with
	// no authored water (the plane at 0) an eye below Z 0 is under it too.
	// [orig: HUD_DrawCrosshair @0x592B59..0x592B65, no unauthored-plane test]
	CHECK(rig.w.env.water_z == 0);
	const int32_t body_z = body.pos[2];
	body.pos[2] = -(1 << 16);
	CHECK(rig.view().hud_spread_row == 2);
	body.pos[2] = body_z;
	CHECK(rig.view().hud_spread_row == 0);
	// Airborne forces stand.
	body.inf.airborne = true;
	CHECK(rig.view().hud_spread_row == 2);
	body.inf.airborne = false;
	// The in-air entity flag forces stand too, and a mount finally forces crouch.
	rig.entity().flags |= kEntityFlagInAir;
	CHECK(rig.view().hud_spread_row == 2);
	rig.entity().mounted = true;
	CHECK(rig.view().hud_spread_row == 1);
	// No authored def: the live error is the shifted terms alone.
	rig.entity().equipped_adm_index = 0;
	CHECK(rig.view().hud_spread_fp16 ==
			opennova::io::bam_add(opennova::io::bam_sar(0x1000, 7), opennova::io::bam_sar(0x800, 7)));
}

void test_heat_clamps() {
	Rig rig;
	CHECK(rig.view().heat == 0 && rig.view().heat_glow == 0);
	rig.w.logic_tick = 100;
	rig.weapon.def.heat_per_shot = 1;
	rig.weapon.def.heat_decay_per_tick = 0x100;
	rig.weapon.slot.heat_window_end_tick = 100 + 0x200; // 0x20000 of heat left
	const LocalPlayerWeaponView v = rig.view();
	CHECK(v.heat == weapon_heat::kFull);
	CHECK(v.heat_glow == 0x10000);
	rig.weapon.slot.heat_window_end_tick = 100 + 0x40; // 0x4000: under both clamps
	CHECK(rig.view().heat == 0x4000 && rig.view().heat_glow == 0x4000);
}

void test_round_ring_read_back() {
	Rig rig;
	RoundEvent ev;
	ev.mode_flags = 0x22;
	ev.subtype = 5;
	ev.slot_byte = 3;
	ev.shot_seq = 77;
	rig.w.out.rounds.add(ev);
	ev.mode_flags = 0x12;
	ev.shot_seq = 78;
	rig.w.out.rounds.add(ev);
	LocalPlayerWeaponView v = rig.view();
	CHECK(v.round_ring_count == 2);
	CHECK(v.last_round_flags == 0x12);
	CHECK(v.last_round_subtype == 5);
	CHECK(v.last_round_slot_byte == 3);
	CHECK(v.last_round_seq == 78);
	// A cursor that wrapped to 0 reads the last slot.
	for (int i = 2; i < RoundRing::kCapacity; ++i) {
		ev.shot_seq = static_cast<uint16_t>(100 + i);
		rig.w.out.rounds.add(ev);
	}
	CHECK(rig.w.out.rounds.cursor == 0);
	v = rig.view();
	CHECK(v.round_ring_count == RoundRing::kCapacity);
	CHECK(v.last_round_seq == 100 + RoundRing::kCapacity - 1);
}

// A local reload request's C2S 0x25 body (player_weapon.h
// local_reload_request_wire): a UseGun gunner addresses the entity it sits on,
// its parentEntity, and an EWeap entity's parameter word is 0xFFFF; any other
// seat, or none, addresses the actor with the slot's parameter.
// [orig: WeaponAction_Reload `cmp [edi+168h], 3` / `mov ecx, [edi+16Ch]`
//  @0x5430EA..0x543103; NetPacket_SendEntityDeathNotification `test byte ptr
//  [eax+54h], 20h` @0x43296C -> 0xFFFF @0x432974]
void test_reload_request_addresses_the_seat_parent() {
	Rig rig;
	rig.w.registry.configure_pool(1, 4);
	Entity gun;
	gun.kind = EntityKind::Item;
	gun.has_item_def = true;
	gun.item_attrib = kItemAttribEweap;
	const EntityHandle gun_h = rig.w.registry.spawn(1, gun);
	Entity seat;
	seat.kind = EntityKind::Item;
	seat.has_item_def = true;
	const EntityHandle seat_h = rig.w.registry.spawn(1, seat);
	constexpr uint16_t kActor = 0x0002, kParam = 11 * 65 + 2;
	Entity &actor = *rig.w.registry.get(rig.local);
	LocalWeaponReloadWire r = local_reload_request_wire(rig.w, kActor, kParam);
	CHECK(r.valid && r.entity_handle == kActor && r.reload_param == kParam);
	actor.mounted = true;
	actor.mount_target = gun_h;
	actor.mount_type = SeatType::Gunner;
	r = local_reload_request_wire(rig.w, kActor, kParam);
	CHECK(r.entity_handle == gun_h.packed && r.reload_param == 0xFFFF);
	actor.mount_target = seat_h; // a UseGun parent without the EWeap attribute
	r = local_reload_request_wire(rig.w, kActor, kParam);
	CHECK(r.entity_handle == seat_h.packed && r.reload_param == kParam);
	actor.mount_target = gun_h;
	actor.mount_type = SeatType::Controller; // the armed ctrlx: the actor
	r = local_reload_request_wire(rig.w, kActor, kParam);
	CHECK(r.entity_handle == kActor && r.reload_param == kParam);
}

} // namespace

int main() {
	test_inactive_reads_as_defaults();
	test_slot_serials_and_action_legs();
	test_effect_anchor_tracks_the_committed_usegun_slot();
	test_first_person_action_model_needs_gfx1_and_animadm();
	test_power_throw_windup();
	test_crosshair_spread_rows();
	test_heat_clamps();
	test_round_ring_read_back();
	test_reload_request_addresses_the_seat_parent();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("player_weapon_view_test OK\n");
	return 0;
}
