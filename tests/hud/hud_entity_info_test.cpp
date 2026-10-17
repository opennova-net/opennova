// The local player's per-frame HUD info record (runtime/hud/hud_entity_info.h):
// the health fraction, the stance and fov defaults, the weapon cluster's ammo
// pair (the clipsize -1 clip, the capacity-1 fold), the live-FSM gates, the
// SIGHTS card gate under the binoculars and the NVG scene arm, the
// vehicle-attack clear of the two selector bytes ahead of the overlay fork,
// and the circle mask's three inputs.
// [orig: HUD_BuildEntityInfo @0x4b8440; Render_ProcessMainSceneFrame
//  @0x5ca2ff..0x5ca304, @0x5caae3..0x5cab15]

#include <runtime/hud/hud_entity_info.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_weapon.h>

#include <cstdio>

namespace {

namespace hud = opennova::hud;
namespace world = opennova::world;

int failures = 0;

void expect(bool cond, const char *msg) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
}

world::LocalPlayerWeaponView live_weapon(int32_t clip, int32_t reserve) {
	world::LocalPlayerWeaponView w;
	w.active = true;
	w.clip = clip;
	w.reserve = reserve;
	w.heat = 0x1234;
	w.hud_spread_fp16 = 0x8000;
	w.aimed_shot_available = true;
	w.windup_active = true;
	w.windup_held_ticks = 7;
	w.ammo_bucket = 3;
	w.ammo_class_id = 11;
	return w;
}

world::LocalPlayerViewFrame scoped_view() {
	world::LocalPlayerViewFrame v;
	v.scope_card_active = true;
	v.fov_h_deg = 20.0f;
	v.hud_stance = 2;
	return v;
}

void test_no_view_no_weapon() {
	const hud::HudEntityInfo info = hud::local_player_entity_info(50, 100, nullptr, nullptr, nullptr);
	expect(info.health_fraction == 0.5f, "half health is 0.5");
	expect(info.stance == 0, "no view: stance 0");
	expect(info.fov_h_deg == 80.0f, "no view: the default 80 degree fov");
	expect(!info.weapon_drawn && info.clip == -1 && info.reserve == -1, "no FSM: no cluster, -1 pair");
	expect(info.heat == 0 && info.spread_fp16 == 0 && !info.aimed_shot_available,
			"no FSM: the cluster words read 0");
	expect(!info.windup_active && info.windup_held_ticks == 0, "no FSM: no charge");
	expect(info.binocular_range == 1 && info.flash_revive_channel == 255,
			"no view: the rangefinder 1, the revive channel 255");
	expect(!info.sights_card_up && info.overlay == hud::ScopedViewOverlay::kEntityMarkers,
			"no view: no card, the markers arm");
	expect(!info.scope_mask_up && info.scope_mask_draw_crosshair, "no view: no mask, no-row argument");
}

void test_health_fraction() {
	expect(hud::local_player_entity_info(150, 100, nullptr, nullptr, nullptr).health_fraction == 1.0f,
			"an overhealed body clamps to 1");
	expect(hud::local_player_entity_info(-5, 100, nullptr, nullptr, nullptr).health_fraction == 0.0f,
			"a negative health clamps to 0 (D-HUD-X1)");
	expect(hud::local_player_entity_info(40, 0, nullptr, nullptr, nullptr).health_fraction == 0.0f,
			"no ceiling reads empty");
	expect(hud::local_player_entity_info(1, 3, nullptr, nullptr, nullptr).health_fraction ==
					static_cast<float>(1.0 / 3.0),
			"the double quotient narrowed once");
}

void test_ammo_pair() {
	const world::LocalPlayerWeaponView w = live_weapon(12, 90);
	hud::HudEntityWeaponDef def;
	def.clipsize = 30;
	hud::HudEntityInfo info = hud::local_player_entity_info(100, 100, nullptr, &w, &def);
	expect(info.weapon_drawn && info.clip == 12 && info.reserve == 90, "a 30-round def keeps its pair");
	expect(info.heat == 0x1234 && info.spread_fp16 == 0x8000 && info.aimed_shot_available,
			"a live FSM's cluster words");
	expect(info.windup_active && info.windup_held_ticks == 7, "a live FSM's charge");
	expect(info.ammo_bucket == 3 && info.ammo_class_id == 11, "the clip-flash key's halves");
	def.clipsize = -1;
	info = hud::local_player_entity_info(100, 100, nullptr, &w, &def);
	expect(info.clip == -1 && info.reserve == 90, "the clipsize -1 weapon reads clip -1");
	const world::LocalPlayerWeaponView one = live_weapon(1, 4);
	def.clipsize = 1;
	info = hud::local_player_entity_info(100, 100, nullptr, &one, &def);
	expect(info.clip == 1 && info.reserve == 5, "capacity 1 folds the chambered round");
	info = hud::local_player_entity_info(100, 100, nullptr, &w, nullptr);
	expect(!info.weapon_drawn && info.clip == 12 && info.reserve == 90,
			"no def: the raw pair, no cluster");
	world::LocalPlayerWeaponView idle = w;
	idle.active = false;
	info = hud::local_player_entity_info(100, 100, nullptr, &idle, &def);
	expect(!info.weapon_drawn && info.clip == -1 && info.heat == 0 && info.ammo_bucket == 0,
			"an inactive FSM reads as none");
}

void test_card_and_fork() {
	hud::HudEntityWeaponDef def;
	def.clipsize = 5;
	def.scoped_selector = true;
	def.has_sights = true;
	world::LocalPlayerViewFrame v = scoped_view();
	hud::HudEntityInfo info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(info.stance == 2 && info.fov_h_deg == 20.0f, "the view frame's stance and fov");
	expect(info.sights_card_up, "a settled Scoped weapon raises its card");
	expect(info.overlay == hud::ScopedViewOverlay::kScopedCardWithCircleMask && info.scope_mask_up,
			"the Scoped arm chains the circle mask");
	expect(!info.scope_mask_draw_crosshair, "an authored SIGHTS row suppresses the reticle");
	def.has_sights = false;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(info.scope_mask_draw_crosshair, "no SIGHTS row: the mask draws the reticle");
	// The vehicle-attack context clears both selector bytes before the fork;
	// the card switch itself stands.
	v.vehicle_attack_context = true;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(info.overlay == hud::ScopedViewOverlay::kEntityMarkers && !info.scope_mask_up,
			"the vehicle-attack context clears the selectors");
	expect(info.sights_card_up, "the card switch is not the selector bytes");
	v.vehicle_attack_context = false;
	// The binocular arm is tested first.
	v.binoculars_view_active = true;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(!info.sights_card_up && info.overlay == hud::ScopedViewOverlay::kBinocularMask,
			"no card under the binoculars");
	v.binoculars_view_active = false;
	// The NVG Sighted arm draws the card into the scene, not over the frame.
	def.scoped_selector = false;
	def.sighted_selector = true;
	v.nvg_sights_in_scene = true;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(!info.sights_card_up && info.nvg_sights_in_scene, "the NVG Sighted arm takes the card");
	expect(info.overlay == hud::ScopedViewOverlay::kSightedCard && !info.scope_mask_up,
			"the Sighted arm never chains the mask");
	// The NVG lens raises the unit-scale reticle without the Scoped arm.
	v.nvg_sights_in_scene = false;
	v.nvg_lens_active = true;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(info.scope_mask_up && info.scope_mask_nvg_lens, "the NVG lens raises the mask");
	// No card switch: neither selector reaches the fork.
	v.scope_card_active = false;
	v.nvg_lens_active = false;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, &def);
	expect(!info.sights_card_up && info.overlay == hud::ScopedViewOverlay::kEntityMarkers,
			"no card switch: the markers arm");
	// No def: no selectors, the no-row argument.
	v.scope_card_active = true;
	info = hud::local_player_entity_info(100, 100, &v, nullptr, nullptr);
	expect(info.sights_card_up && info.overlay == hud::ScopedViewOverlay::kEntityMarkers,
			"no def: the card switch stands, no selector");
	expect(info.scope_mask_draw_crosshair, "no def: the no-row argument");
}

void test_view_effects() {
	world::LocalPlayerViewFrame v;
	v.binoculars_view_active = true;
	v.aim_range_q16 = 250 * 65536 + 0x8000;
	v.nvg_mask_visible = true;
	v.nvg_gain = 9;
	v.screen_flash_white_alpha = 64;
	v.screen_flash_red_alpha = 32;
	v.screen_flash_revive = 5;
	v.screen_flash_revive_channel = 1;
	v.hud_overlays_suppressed = true;
	v.hud_keep_crosshair_while_aimed = true;
	const hud::HudEntityInfo info = hud::local_player_entity_info(100, 100, &v, nullptr, nullptr);
	expect(info.binoculars_view_active && info.binocular_range == 250,
			"the rangefinder's whole units");
	expect(info.nvg_mask && info.nvg_gain == 9, "the NVG mask and gain");
	expect(info.flash_white == 64 && info.flash_red == 32 && info.flash_revive == 5 &&
					info.flash_revive_channel == 1,
			"the damage-feedback quads");
	expect(info.overlays_suppressed, "the white flash's early return");
	expect(info.keep_crosshair_while_aimed, "the Inset keep-up predicate");
}

} // namespace

int main() {
	test_no_view_no_weapon();
	test_health_fraction();
	test_ammo_pair();
	test_card_and_fork();
	test_view_effects();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_entity_info: ok\n");
	return 0;
}
