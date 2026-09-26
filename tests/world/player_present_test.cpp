// The local player's presentation law (runtime/world/player_present): the
// camera-mode split, the FP submit AND-composition, the lighting split, the
// local fire-effect gate and the gfx1/gfx3 pick, the fixed-tick batch order,
// the per-submit CTRL writers, the raw-key latch, the def precedence/memo and
// the spawn-loadout projection.

#include <runtime/world/player_present.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace w = opennova::world;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

constexpr int kFire = 2; // weapon_action::kFire

void test_camera_mode_split() {
	CHECK(!w::presents_third_person(false, 0));
	CHECK(w::presents_third_person(true, 1));
	// The death lerp camera presents the body like the chase.
	CHECK(w::presents_third_person(false, 4));
	CHECK(!w::presents_third_person(false, 3));
}

void test_fp_submit_is_the_and_of_its_gates() {
	const w::FpViewmodelSubmitGates open{};
	CHECK(w::fp_viewmodel_retail_submit(open));
	w::FpViewmodelSubmitGates g = open;
	g.third_person = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	g = open;
	g.scope_card_active = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	g = open;
	g.binoculars_view_active = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	g = open;
	g.fp_weapon_view_flag = false;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	g = open;
	g.seat_hides_weapon = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	// The alive gate [orig: Player_RenderViewModelIfAlive @0x4E0145 /
	// @0x4E014B]: a dead local entity or a decided winner draws no gun.
	g = open;
	g.local_dead = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	g = open;
	g.round_winner_set = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	// An Emplaced def skips the showhud bit [orig: @0x4DEDD9..0x4DEDF1] but
	// nothing else.
	g = open;
	g.fp_weapon_view_flag = false;
	g.emplaced = true;
	CHECK(w::fp_viewmodel_retail_submit(g));
	g.seat_hides_weapon = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
	// A scoped Inset def shows the aperture, not the model
	// [orig: @0x4DEDF7..0x4DEE19].
	g = open;
	g.inset_scoped = true;
	CHECK(!w::fp_viewmodel_retail_submit(g));
}

void test_lighting_context_splits_body_and_fp() {
	const w::LocalPlayerLightingContext outdoors = w::local_player_lighting_context(0, 0.4f, 0.5f);
	CHECK(!outdoors.interior);
	CHECK(outdoors.light_transfer == 0.0f);
	CHECK(outdoors.body_effect_scale == 0.5f);
	CHECK(outdoors.fp_effect_scale == 1.0f);
	const w::LocalPlayerLightingContext indoors = w::local_player_lighting_context(105310, 0.4f, 0.25f);
	CHECK(indoors.interior);
	CHECK(indoors.light_transfer == 0.4f);
	CHECK(indoors.body_effect_scale == 0.25f);
	CHECK(indoors.fp_effect_scale == 1.0f);
}

void test_fire_effect_gate() {
	// FIRE only, with a particle.
	CHECK(w::local_fire_effect_admitted(kFire, kFire, true, false, false, false));
	CHECK(!w::local_fire_effect_admitted(kFire, kFire, false, false, false, false));
	CHECK(!w::local_fire_effect_admitted(3, kFire, true, false, false, false));
	// Settled-scoped first-person fire shows no flash; third person or a
	// vehicle attack context lifts the suppression.
	CHECK(!w::local_fire_effect_admitted(kFire, kFire, true, true, false, false));
	CHECK(w::local_fire_effect_admitted(kFire, kFire, true, true, true, false));
	CHECK(w::local_fire_effect_admitted(kFire, kFire, true, true, false, true));
	// The gfx pick follows the FP bit.
	CHECK(w::action_particle_uses_third_person_gun(true));
	CHECK(!w::action_particle_uses_third_person_gun(false));
	// A world-only emplacement uses its carrier in first person. An FP action
	// model (gfx1 loaded with its animadm) wins in first person, while a
	// third-person mount uses the carrier.
	CHECK(w::action_particle_uses_mounted_gun(true, false, false));
	CHECK(!w::action_particle_uses_mounted_gun(true, false, true));
	CHECK(w::action_particle_uses_mounted_gun(true, true, false));
	CHECK(w::action_particle_uses_mounted_gun(true, true, true));
	for (const bool third_person : {false, true}) {
		for (const bool fp_model : {false, true}) {
			CHECK(!w::action_particle_uses_mounted_gun(false, third_person, fp_model));
		}
	}
	CHECK(w::kActionEffectSpawnPolicy.suppress_while_owned);
	CHECK(w::kActionEffectSpawnPolicy.follow_owner);
	CHECK(w::kActionEffectSpawnPolicy.world_render_domain);
}

bool step_is(const w::WeaponPresentStep &step, w::WeaponPresentOp op, int event) {
	return step.op == op && step.event == event;
}

void test_batch_plan_without_a_view_only_reinstalls() {
	w::WeaponBatchEvent events[3] = {};
	events[0].starts_clip = true;
	events[0].action_started = true;
	events[1].switch_weapon = true;
	events[2].clear_weapon = true;
	events[2].switch_weapon = true; // clear wins over switch
	w::WeaponBatchPlan plan;
	w::weapon_batch_plan(false, 7, 3, events, 3, plan);
	CHECK(plan.steps.size() == 2);
	CHECK(plan.steps.size() == 2 && step_is(plan.steps[0], w::WeaponPresentOp::kSwitchWeapon, 1));
	CHECK(plan.steps.size() == 2 && step_is(plan.steps[1], w::WeaponPresentOp::kClearWeapon, 2));
	CHECK(plan.play_serial == -1);
}

void test_batch_plan_poses_first_when_no_clip_starts() {
	w::WeaponBatchEvent events[2] = {};
	events[0].action_effect = true;
	events[1].action_finished = true;
	events[1].switch_denied = true;
	w::WeaponBatchPlan plan;
	w::weapon_batch_plan(true, 5, 5, events, 2, plan);
	CHECK(plan.steps.size() == 4);
	if (plan.steps.size() == 4) {
		CHECK(step_is(plan.steps[0], w::WeaponPresentOp::kPoseChannel, -1));
		CHECK(step_is(plan.steps[1], w::WeaponPresentOp::kDirectEffect, 0));
		CHECK(step_is(plan.steps[2], w::WeaponPresentOp::kActionEnd, 1));
		CHECK(step_is(plan.steps[3], w::WeaponPresentOp::kSwitchDenied, 1));
	}
	CHECK(plan.play_serial == 5);
}

void test_batch_plan_orders_one_events_legs_and_adopts_the_serial() {
	w::WeaponBatchEvent events[1] = {};
	events[0].starts_clip = true;
	events[0].action_started = true;
	events[0].action_effect = true;
	events[0].action_finished = true;
	events[0].switch_weapon = true;
	events[0].switch_denied = true;
	w::WeaponBatchPlan plan;
	w::weapon_batch_plan(true, 9, 3, events, 1, plan);
	CHECK(plan.steps.size() == 6);
	if (plan.steps.size() == 6) {
		CHECK(step_is(plan.steps[0], w::WeaponPresentOp::kPlayClip, 0));
		CHECK(step_is(plan.steps[1], w::WeaponPresentOp::kActionBegin, 0));
		CHECK(step_is(plan.steps[2], w::WeaponPresentOp::kDirectEffect, 0));
		CHECK(step_is(plan.steps[3], w::WeaponPresentOp::kActionEnd, 0));
		CHECK(step_is(plan.steps[4], w::WeaponPresentOp::kSwitchWeapon, 0));
		CHECK(step_is(plan.steps[5], w::WeaponPresentOp::kSwitchDenied, 0));
	}
	// A batch that started a clip adopts the view serial: no trailing replay.
	CHECK(plan.play_serial == 9);
}

void test_batch_plan_replays_only_an_unseen_serial() {
	// A fresh viewmodel (serial -1) poses the snapshot's clip once, after the
	// batch; a seen serial adds nothing.
	w::WeaponBatchPlan fresh;
	w::weapon_batch_plan(true, 4, -1, nullptr, 0, fresh);
	CHECK(fresh.steps.size() == 2);
	if (fresh.steps.size() == 2) {
		CHECK(step_is(fresh.steps[0], w::WeaponPresentOp::kPoseChannel, -1));
		CHECK(step_is(fresh.steps[1], w::WeaponPresentOp::kPoseChannel, -1));
	}
	CHECK(fresh.play_serial == 4);
	w::WeaponBatchPlan seen;
	w::weapon_batch_plan(true, 4, 4, nullptr, 0, seen);
	CHECK(seen.steps.size() == 1);
	CHECK(seen.play_serial == 4);
}

void test_fp_ctrl_writers() {
	const w::FpCtrlRegisterWrites hidden = w::fp_ctrl_register_writes(false, true, true, true);
	CHECK(!hidden.team && !hidden.heat && !hidden.emplaced && !hidden.arms_camo);
	const w::FpCtrlRegisterWrites gun = w::fp_ctrl_register_writes(true, true, false, false);
	CHECK(gun.team && gun.heat && !gun.emplaced && !gun.arms_camo);
	const w::FpCtrlRegisterWrites arms = w::fp_ctrl_register_writes(true, true, true, true);
	CHECK(arms.team && arms.heat && arms.emplaced && arms.arms_camo);
	const w::FpCtrlRegisterWrites no_view = w::fp_ctrl_register_writes(true, false, true, true);
	CHECK(no_view.team && !no_view.heat && !no_view.emplaced && no_view.arms_camo);
}

void test_raw_key_latch_updates_regardless_of_the_gate() {
	bool was_down = false;
	CHECK(w::latched_key_edge(true, true, was_down));
	CHECK(was_down);
	CHECK(!w::latched_key_edge(true, true, was_down));
	// Held across an inactive gate: the latch follows the key, so the
	// reopened gate sees no edge.
	CHECK(!w::latched_key_edge(true, false, was_down));
	CHECK(!w::latched_key_edge(true, true, was_down));
	CHECK(!w::latched_key_edge(false, true, was_down));
	CHECK(!was_down);
	CHECK(w::latched_key_edge(true, true, was_down));
	CHECK(std::strcmp(w::kWeaponSwitchDenySoundset, "DRY_CLAYSATCH") == 0);
}

void test_def_precedence_and_memo() {
	const w::ViewmodelDefPick none = w::viewmodel_def_pick(true, "WPN_M4AUTO", "WPN_AK47AUTO");
	CHECK(!none.resolves);
	const w::ViewmodelDefPick fallback = w::viewmodel_def_pick(false, "", "WPN_AK47AUTO");
	CHECK(fallback.resolves && fallback.name == "WPN_AK47AUTO");
	const w::ViewmodelDefPick equipped = w::viewmodel_def_pick(false, "WPN_M4AUTO", "WPN_AK47AUTO");
	CHECK(equipped.resolves && equipped.name == "WPN_M4AUTO");
	CHECK(w::viewmodel_def_memo_hit("WPN_M4AUTO", "WPN_M4AUTO", true));
	CHECK(!w::viewmodel_def_memo_hit("WPN_M4AUTO", "WPN_M4AUTO", false));
	CHECK(!w::viewmodel_def_memo_hit("WPN_M4AUTO", "WPN_AK47AUTO", true));
}

void test_spawn_loadout_projection() {
	CHECK(std::strcmp(w::kSpawnLoadoutSlotKeys[0], "primary") == 0);
	CHECK(std::strcmp(w::kSpawnLoadoutSlotKeys[1], "secondary") == 0);
	CHECK(std::strcmp(w::kSpawnLoadoutSlotKeys[2], "accessory") == 0);
	// Nothing staged: no class, nothing applied.
	w::SpawnLoadoutInput empty;
	const w::SpawnLoadoutPlan none = w::spawn_loadout_plan(empty, false);
	CHECK(!none.set_player_class && none.action == w::SpawnLoadoutAction::kNone);
	// A class alone commits the class and applies nothing.
	w::SpawnLoadoutInput class_only;
	class_only.has_player_class = true;
	class_only.player_class = 8;
	const w::SpawnLoadoutPlan classed = w::spawn_loadout_plan(class_only, false);
	CHECK(classed.set_player_class && classed.player_class == 8);
	CHECK(classed.action == w::SpawnLoadoutAction::kNone);
	// A mission kit outranks the profile: sync the inventory, apply nothing.
	w::SpawnLoadoutInput staged;
	staged.slots[0] = {true, "WPN_M4AUTO", -1};
	staged.slots[2] = {true, "WPN_SATCHEL_CHARGE", 2};
	staged.has_player_class = true;
	staged.player_class = 8;
	const w::SpawnLoadoutPlan mission = w::spawn_loadout_plan(staged, true);
	CHECK(mission.set_player_class && mission.action == w::SpawnLoadoutAction::kSyncInventory);
	CHECK(mission.kit.empty());
	// The profile kit applies its non-empty slots in kit order with clips.
	const w::SpawnLoadoutPlan profile = w::spawn_loadout_plan(staged, false);
	CHECK(profile.action == w::SpawnLoadoutAction::kApplyKit);
	CHECK(profile.kit.size() == 2);
	if (profile.kit.size() == 2) {
		CHECK(profile.kit[0].name == "WPN_M4AUTO" && profile.kit[0].clips == -1);
		CHECK(profile.kit[1].name == "WPN_SATCHEL_CHARGE" && profile.kit[1].clips == 2);
	}
	CHECK(profile.after_apply == w::SpawnLoadoutAfterApply::kSyncInventory);
	// Present-but-empty slots are the all-NONE kit: apply, then clear.
	w::SpawnLoadoutInput all_none;
	all_none.slots[0] = {true, "", -1};
	all_none.slots[1] = {true, "", -1};
	all_none.slots[2] = {true, "", -1};
	const w::SpawnLoadoutPlan cleared = w::spawn_loadout_plan(all_none, false);
	CHECK(cleared.action == w::SpawnLoadoutAction::kApplyKit);
	CHECK(cleared.kit.empty());
	CHECK(cleared.after_apply == w::SpawnLoadoutAfterApply::kClearWeapon);
	CHECK(!cleared.set_player_class && cleared.player_class == 0);
}

// The remote fire's effect admission [orig: arms @0x42f521 / @0x42f6ce;
// WeaponSlot_FireAndSpawnEffects @0x53f597 / ActionSlot_SpawnEffect
// @0x402080 for the glow].
void test_fire_effect_plan() {
	w::FirePresentationRow row;
	row.effect = "muzzle_rifle";
	row.action_effect = "flash_m4";
	row.action_userpoint = "muzzle";
	row.mf_light = 1;
	// The ammo arm: the ammo effect at the origin, the glow at the origin.
	w::FireEffectPlan plan = w::fire_effect_plan(row);
	CHECK(plan.glow && !plan.glow_at_muzzle);
	CHECK(plan.spawn && !plan.spawn_at_muzzle);
	CHECK(plan.effect == "muzzle_rifle");
	CHECK(plan.userpoint == "muzzle");
	// The adm arm: the addressed def's FIRE-row effect, anchored on the gun.
	row.adm_arm = true;
	plan = w::fire_effect_plan(row);
	CHECK(plan.glow && plan.glow_at_muzzle);
	CHECK(plan.spawn && plan.spawn_at_muzzle);
	CHECK(plan.effect == "flash_m4");
	// The adm arm never falls back to the ammo effect.
	row.action_effect.clear();
	plan = w::fire_effect_plan(row);
	CHECK(!plan.spawn && plan.effect.empty());
	CHECK(plan.glow_at_muzzle);
	// No MF_Light, no glow; the local player's own fire spawns no effect here
	// but its glow still re-arms.
	row.action_effect = "flash_m4";
	row.mf_light = 0;
	CHECK(!w::fire_effect_plan(row).glow);
	row.mf_light = 1;
	row.is_local_player = true;
	plan = w::fire_effect_plan(row);
	CHECK(plan.glow && !plan.spawn && plan.effect.empty());
}

} // namespace

int main() {
	test_fire_effect_plan();
	test_camera_mode_split();
	test_fp_submit_is_the_and_of_its_gates();
	test_lighting_context_splits_body_and_fp();
	test_fire_effect_gate();
	test_batch_plan_without_a_view_only_reinstalls();
	test_batch_plan_poses_first_when_no_clip_starts();
	test_batch_plan_orders_one_events_legs_and_adopts_the_serial();
	test_batch_plan_replays_only_an_unseen_serial();
	test_fp_ctrl_writers();
	test_raw_key_latch_updates_regardless_of_the_gate();
	test_def_precedence_and_memo();
	test_spawn_loadout_projection();
	if (failures != 0) {
		std::printf("player_present_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("player_present_test passed\n");
	return 0;
}
