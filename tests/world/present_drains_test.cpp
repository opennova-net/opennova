// The present-pass row fills (world/present_drains.h), pinned where they used
// to live in the Godot binding (ADR 0040 ladder E0): the throwable key packing
// and the tag-1 move-effect liveness, the viewer-team device variant
// [orig: @0x5469db..0x546a15], the vehicle trail rows, the impact drain's
// effect/sound/light gates and age clamp, the fire drain's arm split
// [orig: @0x42f521 / @0x42f6ce] with the adm arm's FIRE-row effect and
// userpoint, the death-piece rows, the round glows, and the clear-after-drain
// contract of both drains.
#include <runtime/world/present_drains.h>

#include <runtime/world/ammo_table.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/occlusion_camera.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

bool near_f(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A world with one live local organic (team 1, bms 77), two ammo rows (1: a
// move effect, an impact row with effect + sound + light, a muzzle effect, an
// MF light and a move glow; 2: bare) and one weapon def whose FIRE row names
// its particle and userpoint.
struct Rig {
	World w;
	bool w_jo = test_titles::jo_world(w);
	EntityHandle local;

	Rig() {
		w.registry.configure_pool(0, 8);
		Entity seed;
		seed.kind = EntityKind::Organic;
		seed.item_id = 0x14B9;
		seed.net_id = 1;
		seed.bms_id = 77;
		seed.team = 1;
		seed.position = {10.0f, 20.0f, 3.0f};
		seed.alive = true;
		seed.health = 100;
		local = w.registry.spawn(0, seed);
		w.cached.local_player = local;

		w.tables.ammo.entries.resize(3);
		AmmoTableEntry &a = w.tables.ammo.entries[1];
		a.valid = true;
		a.impact_effects[1] = {"smoke_trail", "", true};
		a.impact_effects[3] = {"dirt_hit", "thud", true};
		a.light_impact_radius = 4.0f;
		a.light_impact_color = 0x00FF8040u;
		a.light_impact_ticks = 12;
		a.ai_launch_effect = "muzzle_rifle";
		a.mf_light = 1;
		a.light_move_radius = 2.5f;
		a.light_move_color = 0x00FFFF00u;
		AmmoTableEntry &b = w.tables.ammo.entries[2];
		b.valid = true;

		w.tables.weapons.entries.resize(2);
		WeaponTableEntry &def = w.tables.weapons.entries[1];
		def.valid = true;
		std::strncpy(def.action_fsm.actions[weapon_action::kFire].particle, "flash_m4", 127);
		std::strncpy(def.action_fsm.actions[weapon_action::kFire].particle_userpoint, "muzzle", 127);
	}
};

void test_throwable_rows() {
	Rig rig;
	World &w = rig.w;
	// Slot 5: a tracer round with a visible item and a live move effect.
	LiveRound &r5 = w.round_sim.rounds[5];
	r5.active = true;
	r5.tracer = true;
	r5.item_type_id = 42;
	// A TrcrID item's round carries its def; only a def'd item shows
	// [orig: RoundData_SpawnRound @0x4EC0D0 - the no-TrcrID leg's def-less item 1
	//  @0x4EC7C1].
	r5.has_item_def = true;
	// Life left, aged 2 of 100 ticks at 1.0 unit a tick: past the 1.5-unit gate.
	w.tables.ammo.entries[1].max_age_ticks = 100;
	r5.max_age_ticks = 100;
	r5.age_ticks = 2;
	r5.vel = {1.0f, 0.0f, 0.0f};
	r5.presentation_generation = 7;
	r5.ammo_index = 1;
	r5.pos = {1.0f, 2.0f, 3.0f};
	r5.yaw_bam = 0x40000000;   // a 90 degree heading -> MISSION yaw 0
	r5.pitch_bam = 0x20000000; // 45 degrees
	r5.roll_bam = -0x20000000; // -45 degrees
	r5.move_effect_live = true;
	// Slot 6: a non-tracer shot of an ammo with no move effect -> nothing drawn.
	LiveRound &r6 = w.round_sim.rounds[6];
	r6.active = true;
	r6.tracer = false;
	r6.item_type_id = 42;
	r6.ammo_index = 2;
	// Slot 7: a non-tracer shot whose ammo carries a move effect -> the row
	// survives with no item, and a zero generation keys as 1.
	LiveRound &r7 = w.round_sim.rounds[7];
	r7.active = true;
	r7.tracer = false;
	r7.item_type_id = 42;
	r7.ammo_index = 1;
	r7.presentation_generation = 0;
	r7.move_effect_live = false;
	// A placed device of the other team, and one with no item at all.
	PlacedDevice foe;
	foe.active = true;
	foe.entity = rig.local;
	foe.item_friendly = 100;
	foe.item_enemy = 200;
	foe.team = 2;
	foe.pos = {4.0f, 5.0f, 6.0f};
	w.throwables.devices.push_back(foe);
	PlacedDevice bare;
	bare.active = true;
	w.throwables.devices.push_back(bare);

	std::vector<ThrowableVisualRow> rows;
	fill_throwable_visual_rows(w, rows);
	CHECK(rows.size() == 3);
	CHECK(rows[0].key == ((int64_t(7) << 10) | 5));
	CHECK(rows[0].item_id == 42);
	CHECK(near_f(rows[0].pos.y, 2.0f));
	CHECK(near_f(rows[0].pitch_deg, 45.0f));
	CHECK(near_f(rows[0].yaw_deg, 0.0f));
	CHECK(near_f(rows[0].roll_deg, -45.0f));
	CHECK(rows[0].move_effect == "smoke_trail");
	CHECK(rows[0].move_effect_live);
	// A flying round lies in the RoundPool, which no water mirror's reflected
	// collect walks; a placed device is a pool-1 entity, which it does
	// [orig: Terrain_CollectVisibleEntitiesForReflection @0x5C90A0 - the calls
	//  @0x5C913D, @0x5C9145, @0x5C914E; Terrain_CollectVisibleEntities @0x5C9160 -
	//  the RoundPool_GetBase call @0x5C91C1].
	CHECK(!rows[0].mirror_collected);
	CHECK(!rows[1].mirror_collected);
	CHECK(rows[2].mirror_collected);
	CHECK(rows[1].key == ((int64_t(1) << 10) | 7));
	CHECK(rows[1].item_id == 0);
	CHECK(rows[1].move_effect == "smoke_trail");
	CHECK(!rows[1].move_effect_live);
	const int expected_item = throwable_item_for_viewer(100, 200, 2, 1);
	CHECK(expected_item != 0);
	CHECK(rows[2].item_id == expected_item);
	CHECK(rows[2].key == (0x4000000000000000LL | static_cast<int64_t>(rig.local.packed)));
	CHECK(rows[2].move_effect.empty());
	CHECK(!rows[2].move_effect_live);
	CHECK(near_f(rows[2].pos.x, 4.0f));

	// The scene collect's model gates: a round that has flown under 1.5 units
	// (one tick at 1.0) or has no life left draws no model; its move effect row
	// stays. A round not yet aged (the ammo's lifetime equal to its remaining word)
	// takes no distance test. [orig: Terrain_CollectVisibleEntities @0x5C9160 -
	//  @0x5C923A..0x5C9242, @0x5C9248..0x5C92AA]
	r5.age_ticks = 1;
	CHECK(!round_model_collected(r5, &w.tables.ammo.entries[1]));
	fill_throwable_visual_rows(w, rows);
	CHECK(rows.size() == 3 && rows[0].item_id == 0 && rows[0].move_effect == "smoke_trail");
	r5.age_ticks = 100;
	CHECK(!round_model_collected(r5, &w.tables.ammo.entries[1]));
	r5.age_ticks = 0;
	r5.vel = {0.0f, 0.0f, 0.0f};
	CHECK(round_model_collected(r5, &w.tables.ammo.entries[1]));
	r5.age_ticks = 2;
	r5.vel = {1.0f, 0.0f, 0.0f};
	CHECK(round_model_collected(r5, &w.tables.ammo.entries[1]));

	// A second fill starts from an empty vector.
	fill_throwable_visual_rows(w, rows);
	CHECK(rows.size() == 3);
}

void test_vehicle_trail_rows() {
	Rig rig;
	World &w = rig.w;
	VehicleTraits traits;
	traits.trails[0].effect = "dust";
	traits.trails[2].effect = "wake";
	w.vehicles.traits.set(500, traits);

	Entity seed;
	seed.kind = EntityKind::Item;
	seed.item_id = 500;
	seed.alive = true;
	const EntityHandle truck = w.registry.spawn(0, seed);
	Entity &e = *w.registry.get(truck);
	e.veh.trails.points[0].definition = 1;
	e.veh.trails.points[0].magnitude_q16 = 0x8000;
	e.veh.trails.points[0].source_tick = 40;
	e.veh.trails.points[0].position = {1.0f, 1.0f, 0.0f};
	e.veh.trails.points[0].direction = {0.0f, 1.0f, 0.0f};
	e.veh.trails.points[1].definition = 3;
	e.veh.trails.points[2].definition = 5; // out of the four definitions: skipped
	// A second vehicle with its movement effects disabled draws nothing.
	Entity quiet = seed;
	const EntityHandle parked = w.registry.spawn(0, quiet);
	w.registry.get(parked)->veh.movement_effects_disabled = true;
	w.registry.get(parked)->veh.trails.points[0].definition = 1;

	std::vector<VehicleTrailVisualRow> rows;
	fill_vehicle_trail_visual_rows(w, rows);
	CHECK(rows.size() == 2);
	CHECK(rows[0].handle_packed == truck.packed);
	CHECK(rows[0].registry_spawn_id == e.registry_spawn_id);
	CHECK(rows[0].point == 0);
	CHECK(rows[0].source_tick == 40);
	CHECK(rows[0].effect == "dust");
	CHECK(rows[0].magnitude_q16 == 0x8000u);
	CHECK(near_f(rows[0].dir.y, 1.0f));
	CHECK(rows[1].point == 1);
	CHECK(rows[1].effect == "wake");
	// Every trail lane spawns with the vehicle as its descriptor tag [orig:
	// Entity_UpdateBoneTrailEffects @ 0x458C5F].
	CHECK(rows[0].section_tagged && rows[1].section_tagged);

	// A dead vehicle draws nothing either.
	e.flags |= kEntityFlagDead;
	fill_vehicle_trail_visual_rows(w, rows);
	CHECK(rows.empty());
}

void test_impact_drain() {
	Rig rig;
	World &w = rig.w;
	w.logic_tick = 100;
	RoundImpact full;
	full.position = {1.0f, 2.0f, 3.0f};
	full.direction = {0.0f, 0.0f, 1.0f};
	full.ammo_index = 1;
	full.effect_tag = 3;
	full.tick = 90;
	full.source_order = 11;
	w.round_sim.impacts.push_back(full);
	RoundImpact future = full;
	future.tick = 120; // a rewound clock never pre-ages by billions of ticks
	future.source_order = 12;
	w.round_sim.impacts.push_back(future);
	RoundImpact bare = full;
	bare.ammo_index = 2; // no effect, no sound: dropped
	w.round_sim.impacts.push_back(bare);
	RoundImpact sound_only = full;
	sound_only.present_effect = false; // the light rides the effect leg
	w.round_sim.impacts.push_back(sound_only);
	RoundImpact bad_tag = full;
	bad_tag.effect_tag = kImpactEffectTagCount;
	w.round_sim.impacts.push_back(bad_tag);

	std::vector<RoundImpactPresentation> rows;
	drain_round_impact_rows(w, rows);
	CHECK(rows.size() == 3);
	CHECK(rows[0].effect == "dirt_hit");
	CHECK(rows[0].sound == "thud");
	CHECK(rows[0].age_ticks == 10u);
	CHECK(rows[0].source_tick == 90u);
	CHECK(rows[0].source_order == 11u);
	CHECK(rows[0].has_light);
	CHECK(near_f(rows[0].light_radius, 4.0f));
	CHECK(rows[0].light_color_rgb24 == 0x00FF8040u);
	CHECK(rows[0].light_ticks == 12);
	CHECK(near_f(rows[0].direction.z, 1.0f));
	CHECK(rows[1].age_ticks == 0u);
	CHECK(rows[2].effect.empty());
	CHECK(rows[2].sound == "thud");
	CHECK(!rows[2].has_light);
	// The drain consumed the ring.
	CHECK(w.round_sim.impacts.empty());
	drain_round_impact_rows(w, rows);
	CHECK(rows.empty());

	// A tag the ammo authors no row of plays ammo def 0's bank at the tag's place; a tag
	// past the table plays obj's [orig: AmmoDef_ProcessImpactEffect @0x40a1b8..0x40a1fd].
	w.tables.ammo.null_bank[3] = {5, "bank_dirt", "bank_thud"};
	w.tables.ammo.null_bank[4] = {6, "bank_obj", ""};
	RoundImpact unauthored = full;
	unauthored.ammo_index = 2;
	w.round_sim.impacts.push_back(unauthored);
	RoundImpact past = bad_tag;
	past.ammo_index = 2;
	w.round_sim.impacts.push_back(past);
	RoundImpact own_reader = unauthored;
	own_reader.own_row = true; // a direct reader takes its own row by position: none
	w.round_sim.impacts.push_back(own_reader);
	drain_round_impact_rows(w, rows);
	CHECK(rows.size() == 2);
	if (rows.size() == 2) {
		CHECK(rows[0].effect == "bank_dirt");
		CHECK(rows[0].sound == "bank_thud");
		CHECK(rows[1].effect == "bank_obj");
		CHECK(rows[1].sound.empty());
	}
}

void test_fire_drain() {
	Rig rig;
	World &w = rig.w;
	FireEvent own;
	own.shooter = rig.local;
	own.shooter_handle = 0x1005;
	own.ammo_index = 1;
	own.origin = {5.0f, 6.0f, 7.0f};
	own.yaw_bam = 0;
	own.pitch_bam = 0;
	own.wire_round_flags = 0;
	w.round_sim.fired.push_back(own);
	FireEvent adm;
	adm.shooter_handle = 0x2001; // an unknown shooter: no bms, not local
	adm.ammo_index = 1;
	adm.adm_index = 1;
	adm.yaw_bam = 0x40000000; // 90 degrees: forward along +y
	adm.pitch_bam = 0;
	adm.wire_round_flags = round_event_flag::kAdmIndexed;
	w.round_sim.fired.push_back(adm);
	FireEvent alt = adm;
	alt.wire_round_flags = round_event_flag::kAltFire | round_event_flag::kAdmIndexed;
	w.round_sim.fired.push_back(alt);
	FireEvent unknown_def = adm;
	unknown_def.adm_index = 7; // no such def: the adm arm carries no action rows
	w.round_sim.fired.push_back(unknown_def);

	std::vector<FirePresentationRow> rows;
	drain_fire_presentation_rows(w, rows);
	CHECK(rows.size() == 4);
	CHECK(!rows[0].adm_arm);
	CHECK(rows[0].is_local_player);
	CHECK(rows[0].shooter_handle == 0x1005);
	CHECK(rows[0].source_bms_id == 77);
	CHECK(rows[0].ammo_index == 1);
	CHECK(rows[0].effect == "muzzle_rifle");
	CHECK(rows[0].mf_light == 1);
	CHECK(near_f(rows[0].origin.z, 7.0f));
	CHECK(near_f(rows[0].forward.x, 1.0f));
	CHECK(near_f(rows[0].forward.y, 0.0f));
	CHECK(near_f(rows[0].forward.z, 0.0f));
	// The adm arm: bit 0 clear, bit 1 set. The FIRE row of the ADDRESSED def
	// names the effect and the gfx3 userpoint; the ammo effect still rides.
	CHECK(rows[1].adm_arm);
	CHECK(rows[1].adm_index == 1);
	CHECK(!rows[1].is_local_player);
	CHECK(rows[1].source_bms_id == 0);
	CHECK(rows[1].action_effect == "flash_m4");
	CHECK(rows[1].action_userpoint == "muzzle");
	CHECK(rows[1].effect == "muzzle_rifle");
	CHECK(near_f(rows[1].forward.x, 0.0f));
	CHECK(near_f(rows[1].forward.y, 1.0f));
	// Bit 0 set wins: the ammo arm, whatever bit 1 says. The row still carries
	// the addressed def's FIRE-row legs; the arm bit is the consumer's gate.
	CHECK(!rows[2].adm_arm);
	CHECK(rows[2].action_effect == "flash_m4");
	// An unaddressable def leaves the action legs empty.
	CHECK(rows[3].adm_arm);
	CHECK(rows[3].action_effect.empty());
	CHECK(rows[3].action_userpoint.empty());
	// The drain consumed the ring.
	CHECK(w.round_sim.fired.empty());
	drain_fire_presentation_rows(w, rows);
	CHECK(rows.empty());
}

void test_death_pieces() {
	Rig rig;
	World &w = rig.w;
	DeathPiece &p = w.death_pieces.pieces[3];
	p.active = true;
	p.generation = 9;
	p.item_id = 12;
	p.section = 2;
	p.type_index = 4;
	p.render_scale = 0.5f;
	p.pos = {7.0f, 8.0f, 9.0f};
	p.heading = 0x15555555; // 30 deg, the presentation view of the word
	p.pitch = 0x071C71C7;   // 10 deg
	p.settled = true;
	DeathPiece &idle = w.death_pieces.pieces[4];
	idle.item_id = 13; // never allocated: skipped

	std::vector<DeathPieceRow> rows;
	fill_death_pieces(w, rows);
	CHECK(rows.size() == 1);
	CHECK(rows[0].slot == 3);
	CHECK(rows[0].generation == 9u);
	CHECK(rows[0].item_id == 12);
	CHECK(rows[0].section == 2);
	CHECK(rows[0].type_index == 4);
	CHECK(near_f(rows[0].scale, 0.5f));
	CHECK(near_f(rows[0].pos.z, 9.0f));
	CHECK(near_f(rows[0].heading, 30.0f));
	CHECK(near_f(rows[0].pitch, 10.0f));
	CHECK(rows[0].settled);
}

void test_round_glows() {
	Rig rig;
	World &w = rig.w;
	LiveRound &lit = w.round_sim.rounds[5];
	lit.active = true;
	lit.ammo_index = 1;
	lit.presentation_generation = 7;
	lit.pos = {1.0f, 2.0f, 3.0f};
	LiveRound &dark = w.round_sim.rounds[6];
	dark.active = true;
	dark.ammo_index = 2; // no move glow authored
	LiveRound &unbound = w.round_sim.rounds[8];
	unbound.active = true;
	unbound.ammo_index = -1;

	std::vector<RoundGlowRow> rows;
	fill_round_glows(w, rows);
	CHECK(rows.size() == 1);
	CHECK(rows[0].id == 7u);
	CHECK(near_f(rows[0].pos.y, 2.0f));
	CHECK(near_f(rows[0].radius, 2.5f));
	CHECK(rows[0].color_rgb24 == 0x00FFFF00u);
}

// The view camera at the mission origin, presentation frame (x, z, -y): its
// forward, right and up, a 16:9 rect at a 60-degree vertical field, fog in units.
OcclusionFrameCamera view_camera(const float forward[3], const float right[3],
		const float up[3], float fog) {
	OcclusionViewSpec view;
	for (int i = 0; i < 3; ++i) {
		view.eye[i] = 0.0f;
		view.forward[i] = forward[i];
		view.right[i] = right[i];
		view.up[i] = up[i];
	}
	view.fov_h_deg = 91.4928f;
	view.viewport_width = 1920.0f;
	view.viewport_height = 1080.0f;
	view.fog_dist_units = fog;
	view.water_z_units = -1000.0f;
	OcclusionFrameCamera cam;
	occlusion_camera_from_view(view, cam);
	return cam;
}

// The scene collect's RoundPool tail over one view: the x / y / z box against the
// fog with no radius word, the view-independent gates, the item row's model, the
// view depth against the fog alone and the viewport clip at four times the model's
// radius; the collected rounds' presentation keys in pool order.
// [orig: Terrain_CollectVisibleEntities @0x5C9160 - the box @0x5C91F5..0x5C9234,
//  the model @0x5C92CB and its radius x 4 @0x5C92D6..0x5C92E6, the depth
//  @0x5C92E8..0x5C9301, the clip @0x5C930E..0x5C931F]
void test_round_view_collect() {
	auto w_heap = std::make_unique<World>();
	test_titles::jo_world(*w_heap);
	World &w = *w_heap;
	w.tables.ammo.entries.resize(2);
	w.tables.ammo.entries[1].valid = true;
	w.tables.ammo.entries[1].max_age_ticks = 100;
	// Row 0 is the Null row; type 42's graphic has a 0.5-unit radius word, type 43
	// names no loaded graphic, type 44 has no row.
	w.tables.item_rows.resize(3);
	w.tables.item_rows[0].type_id = 0;
	w.tables.item_rows[1].type_id = 42;
	w.tables.item_rows[1].has_graphic_model = true;
	w.tables.item_rows[1].graphic_model_radius_q16 = 0x8000;
	w.tables.item_rows[2].type_id = 43;
	w.tables.item_rows[2].has_graphic_model = false;
	const auto flying = [&](int slot, float x, float y, float z) -> LiveRound & {
		LiveRound &r = w.round_sim.rounds[static_cast<size_t>(slot)];
		r = LiveRound{};
		r.active = true;
		r.tracer = true;
		r.has_item_def = true;
		r.item_type_id = 42;
		r.ammo_index = 1;
		// Aged 10 of 100 ticks at 1 unit a tick: past the travel gate.
		r.max_age_ticks = 100;
		r.age_ticks = 10;
		r.vel = {1.0f, 0.0f, 0.0f};
		r.presentation_generation = 3;
		r.pos = {x, y, z};
		return r;
	};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const float side[3] = {0.0f, 0.0f, 1.0f};
	const float ahead[3] = {1.0f, 0.0f, 0.0f};
	OcclusionWorld occlusion;
	std::vector<int64_t> keys;

	// Looking down mission +x, fog 100. The side planes lean 45.75 degrees out: at
	// 30 units a round 32.5 units aside lies 1.2 units outside, within the clip's
	// 2 units (four times the model's 0.5), and one 35 units aside 2.95 units out.
	const OcclusionFrameCamera cam = view_camera(ahead, side, up, 100.0f);
	flying(0, 30.0f, 0.0f, 0.0f);
	flying(1, -30.0f, 0.0f, 0.0f); // behind the eye
	flying(2, 30.0f, 32.5f, 0.0f);
	flying(3, 30.0f, 35.0f, 0.0f);
	flying(4, 150.0f, 0.0f, 0.0f);                   // past the x box
	flying(5, 30.0f, 0.5f, 0.0f).item_type_id = 43;  // no model
	flying(6, 30.0f, -0.5f, 0.0f).item_type_id = 44; // no row
	flying(7, 30.0f, 1.0f, 0.0f).age_ticks = 100;    // no life left
	flying(8, 30.0f, -1.0f, 0.0f).tracer = false;    // a non-tracer shot shows no model
	flying(9, 30.0f, 2.0f, 0.0f).active = false;
	occlusion.collect_round_draws(w, cam, keys);
	CHECK(keys.size() == 2);
	if (keys.size() == 2) {
		CHECK(keys[0] == round_presentation_key(w.round_sim.rounds[0], 0));
		CHECK(keys[0] == ((int64_t(3) << 10) | 0));
		CHECK(keys[1] == round_presentation_key(w.round_sim.rounds[2], 2));
	}
	// The keys are the throwable rows' own.
	std::vector<ThrowableVisualRow> rows;
	fill_throwable_visual_rows(w, rows);
	CHECK(!rows.empty() && rows[0].key == keys.front());

	// The depth word against the fog alone: yawed 45 degrees, a round at (71.5, 71.5)
	// lies 101.1 units deep, inside each axis of the box and within the fog plus the
	// clip radius, past the fog itself; one at (70, 70) lies 99 deep.
	for (LiveRound &r : w.round_sim.rounds) r.active = false;
	const float a = 0.70710678f;
	const float yaw_fwd[3] = {a, 0.0f, -a};
	const float yaw_right[3] = {a, 0.0f, a};
	const OcclusionFrameCamera yawed = view_camera(yaw_fwd, yaw_right, up, 100.0f);
	flying(0, 70.0f, 70.0f, 0.0f);
	flying(1, 71.5f, 71.5f, 0.0f);
	occlusion.collect_round_draws(w, yawed, keys);
	CHECK(keys.size() == 1 && keys[0] == round_presentation_key(w.round_sim.rounds[0], 0));

	// The z box: pitched 45 degrees up, a round 101 units above the eye and 30 ahead
	// lies 92.6 deep and inside the frustum, outside the box on z alone; one 99 above
	// stays.
	for (LiveRound &r : w.round_sim.rounds) r.active = false;
	const float pitch_fwd[3] = {a, a, 0.0f};
	const float pitch_up[3] = {-a, a, 0.0f};
	const OcclusionFrameCamera pitched = view_camera(pitch_fwd, side, pitch_up, 100.0f);
	flying(0, 30.0f, 0.0f, 99.0f);
	flying(1, 30.0f, 0.0f, 101.0f);
	occlusion.collect_round_draws(w, pitched, keys);
	CHECK(keys.size() == 1 && keys[0] == round_presentation_key(w.round_sim.rounds[0], 0));
}

} // namespace

int main() {
	test_throwable_rows();
	test_vehicle_trail_rows();
	test_impact_drain();
	test_fire_drain();
	test_death_pieces();
	test_round_glows();
	test_round_view_collect();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("present_drains_test OK\n");
	return 0;
}
