// Weapon/vehicle HUD state and draw regressions against the original overlay paths.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <formats/def/def.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/inset_scope.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/hud_impact.h>
#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/world.h>
using namespace opennova;
using namespace opennova::hud;
using namespace opennova::world;
static int failures = 0;
#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x)) {                                                                                \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)
static bool near(float a, float b) {
	return std::abs(a - b) < 0.001f;
}
static int sprites(const HudDrawList &d, int texture) {
	return int(std::count_if(
			d.quads.begin(), d.quads.end(), [&](const auto &q) { return q.texture == texture; }));
}

static void geometry_and_draw() {
	const auto g = inset_scope_geometry(1024, 768, 0, 80);
	CHECK(g.valid && g.left == 639 && g.right == 961 && g.top == 223 && g.bottom == 545);
	CHECK(near(g.fov_h_deg, 22.5f) && near(g.center_x, 800) && near(g.center_y, 384));
	CHECK(g.cross_dx == 40 && g.cross_dy == 40 && near(g.stroke, 2));
	CHECK(near(g.inner[0].x, 960) && near(g.inner[8].y, 544));
	CHECK(!inset_scope_geometry(0, 768, 0, 80).valid);
	const auto wide = inset_scope_geometry(1920, 1080, 0, 80);
	CHECK(wide.valid && wide.radius_x != wide.radius_y);
	CHECK(hud_silhouette_alpha(0, 62, 51, 255) == 255);
	CHECK(hud_silhouette_alpha(62, 62, 51, 255) == 153);
	CHECK(hud_silhouette_alpha(5, 0, 51, 255) == 0);
	HudLayout layout;
	layout.combat.target = { 12, 16, true };
	layout.combat.target_friendly = { 20, 24, true };
	layout.combat.custom_aim = { 8, 8, true };
	layout.combat.commander = { 10, 10, true };
	layout.combat.vehicle_fixed = { 8, 8, true };
	layout.combat.vehicle_lag = { 12, 12, true };
	HudFrameCompiler compiler;
	compiler.configure(layout, nullptr);
	HudFrameState s;
	s.weapon.active = true;
	s.combat.target_cursor = true;
	s.combat.target_point = { 100, 200, 0, true };
	CHECK(sprites(compiler.compile(s, 1024, 768), kHudTexTarget) == 1);
	s.combat.target_friendly = true;
	CHECK(sprites(compiler.compile(s, 1024, 768), kHudTexTargetFriendly) == 1);
	s.combat.target_brackets = true;
	const auto &brackets = compiler.compile(s, 1024, 768);
	CHECK(std::count_if(brackets.lines.begin(), brackets.lines.end(),
				  [](auto &l) { return l.color == uint32_t(-44976); }) == 4);
	s.declutter_visible[kDeclutterXhairs] = false;
	CHECK(sprites(compiler.compile(s, 1024, 768), kHudTexTargetFriendly) == 0);
	s.combat.vehicle_lag = true;
	s.combat.vehicle_lag_center = true;
	CHECK(sprites(compiler.compile(s, 1024, 768), kHudTexVehicleLag) == 1); // independent of XHAIRS
	s.scope.active = true;
	s.combat.commander = true;
	s.combat.commander_point = { 1024, 384, 0, true };
	const auto &command = compiler.compile(s, 1024, 768);
	CHECK(sprites(command, kHudTexCommander) == 1);
	CHECK(std::any_of(command.lines.begin(), command.lines.end(), [](auto &l) {
		return l.color == uint32_t(-32736) && near(l.x0, 846) && near(l.x1, 512);
	}));
	s.combat.inset = true;
	s.combat.inset_fov_over_zoom = 80;
	const auto &inset = compiler.compile(s, 1024, 768);
	CHECK(inset.tris.size() == 570);
	CHECK(std::any_of(inset.tris.begin(), inset.tris.end(),
			[](auto &t) { return (t.a.color >> 24) == 0 && (t.c.color >> 24) == 255; }));
	s.combat.hit_feedback = true;
	const auto &hit = compiler.compile(s, 1024, 768);
	CHECK(std::count_if(hit.lines.begin(), hit.lines.end(),
				  [](auto &l) { return l.color == 0xFFFF5050u; }) == 4);
	s.binoculars_view_active = true;
	CHECK(compiler.compile(s, 1024, 768).tris.empty());
	CHECK(sprites(compiler.last_draw_list(), kHudTexVehicleLag) == 0);
	s.binoculars_view_active = false;
	s.combat.death_screen = true;
	CHECK(compiler.compile(s, 1024, 768).tris.empty());
	CHECK(sprites(compiler.last_draw_list(), kHudTexCommander) == 0);
	layout.combat.agl_left = 20;
	layout.combat.agl_right = 24;
	layout.combat.agl_height = 200;
	layout.combat.agl_y = 100;
	compiler.configure(layout, nullptr);
	s = {};
	s.combat.altitude_agl_q16 = 100 << 16;
	const auto &instruments = compiler.compile(s, 1024, 768);
	CHECK(std::any_of(instruments.quads.begin(), instruments.quads.end(), [](const auto &q) {
		return q.filled && near(q.x0, 20) && near(q.x1, 24) && near(q.y1 - q.y0, 200);
	}));
	// HUD item flash timer 0 (BMS action 28 sub 37) blinks the instrument: an
	// armed timer in its dark phase skips every draw, its lit phase (bit 0x10)
	// draws. [orig: HUD_DrawAltitudeBar @0x59F168..0x59F176]
	const auto agl_bar = [](const HudDrawList &d) {
		return std::any_of(d.quads.begin(), d.quads.end(), [](const auto &q) {
			return q.filled && near(q.x0, 20) && near(q.x1, 24) && near(q.y1 - q.y0, 200);
		});
	};
	s.item_flash[0] = 0x20;
	CHECK(!agl_bar(compiler.compile(s, 1024, 768)));
	s.item_flash[0] = 0x30;
	CHECK(agl_bar(compiler.compile(s, 1024, 768)));
	s.item_flash[0] = 0;
}

static void mortar_map_and_world_cues() {
	HudFrameCompiler compiler;
	compiler.configure({}, nullptr);
	HudFrameState state;
	state.minimap.map_mode = 2;
	state.combat.impact_map = true;
	state.combat.impact_x_q16 = 80 << 16;
	state.combat.impact_radius_q16 = 3 << 16;
	const auto &map = compiler.compile(state, 1024, 768).big_map;
	CHECK(map.visible);
	CHECK(std::count_if(map.lines.begin(), map.lines.end(),
				  [](const auto &l) { return l.color == 0xFFFFFF00u; }) == 64);
	state.combat.impact_map = false;
	const auto &cleared = compiler.compile(state, 1024, 768).big_map;
	CHECK(std::none_of(cleared.lines.begin(), cleared.lines.end(),
			[](const auto &l) { return l.color == 0xFFFFFF00u; }));
	state.combat.designator = true;
	state.combat.impact_point = { 512, 384, 0, true };
	const auto &designator = compiler.compile(state, 1024, 768);
	CHECK(!designator.tris.empty());
	CHECK(std::any_of(designator.lines.begin(), designator.lines.end(),
			[](const auto &l) { return l.color == 0x60FF0000u; }));
	// The LollyPop head is a 2:1 ellipse: radius 20 with the 2.0 stroke spans
	// +-21 vertically and twice that horizontally around (512, 384 - 40).
	// [orig: HUD_DrawEntityMarker ring record +0x1C = 2.0 @0x59327B]
	float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
	for (const auto &t : designator.tris)
		for (const auto *v : { &t.a, &t.b, &t.c }) {
			min_x = std::min(min_x, v->x);
			max_x = std::max(max_x, v->x);
			min_y = std::min(min_y, v->y);
			max_y = std::max(max_y, v->y);
		}
	CHECK(std::abs((max_y - min_y) - 42) < 0.5f && std::abs((max_x - min_x) - 84) < 0.5f);
	CHECK(std::abs((min_x + max_x) / 2 - 512) < 0.5f && std::abs((min_y + max_y) / 2 - 344) < 0.5f);
	// Neither cue tests the death screen: the overlay walk's tail is ungated.
	// [orig: HUD_RenderAllOverlays @0x5A87EF..0x5A89DA]
	state.combat.death_screen = true;
	CHECK(!compiler.compile(state, 1024, 768).tris.empty());
	state.combat.death_screen = false;
	state.combat.impact_point.clip = 16;
	CHECK(compiler.compile(state, 1024, 768).tris.empty());
}

// A synthetic 1-page font: every glyph 8x16 px, spacing 2, design width 800.
static fnt::fnt_font_t make_font() {
	fnt::fnt_font_t font{};
	fnt::fnt_init_blank(&font, 1, 2);
	font.design_width = 800;
	for (uint32_t i = 0; i < fnt::FNT_GLYPH_COUNT; ++i) {
		font.glyphs[i].page = 0;
		font.glyphs[i].uv.u0 = 0.0f;
		font.glyphs[i].uv.v0 = 0.0f;
		font.glyphs[i].uv.u1 = 8.0f / 256.0f;
		font.glyphs[i].uv.v1 = 16.0f / 256.0f;
	}
	return font;
}

// Which overlay font slot each combat text rides.
// [orig: gear label slot 0xB4C3A0 @0x59A6F9; service prompts slot 0xB4C3A0
//  @0x5BDFD7 / @0x5BE0AA / @0x5BE0F7; Inset friendly name slot 0xB4C394
//  @0x5CA0C0; impact distance measured in slot 0xB4C394 @0x5A897E and drawn in
//  the hudpos slot 0x2723C74 @0x5A89D0]
static void combat_text_font_slots() {
	const fnt::fnt_font_t font = make_font();
	HudLayout layout;
	layout.alpha_fade_seconds = 1;
	layout.combat.gear_x = 100;
	layout.combat.gear_y = 150;
	layout.combat.impact_x = 512;
	layout.combat.impact_y = 600;
	HudFrameCompiler compiler;
	compiler.configure(layout, &font);
	compiler.configure_label_fonts(&font, &font, &font, 2.0f, 3.0f);
	const auto pages = [&](const HudDrawList &d, int slot) {
		return std::count_if(d.glyphs.begin(), d.glyphs.end(), [&](const auto &g) {
			return g.page == uint32_t(slot) * fnt::FNT_MAX_PAGES;
		});
	};
	HudFrameState s;
	s.combat.vehicle_controls = true;
	s.combat.gear = 1;
	s.combat.gear_text = { "Med", "Low", "High" };
	const auto &gear = compiler.compile(s, 1024, 768);
	CHECK(pages(gear, kHudFontSlotLabelLarge) == 3 && pages(gear, kHudFontSlotHud) == 0);
	// Left-aligned at the scaled design anchor, in the slot's own scale (3x).
	CHECK(!gear.glyphs.empty() && near(gear.glyphs[0].x_top_left, 99.5f) &&
			near(gear.glyphs[0].y_top, 99.5f));
	CHECK(!gear.glyphs.empty() &&
			near(gear.glyphs[0].x_bottom_right - gear.glyphs[0].x_top_left, 24));

	s = {};
	s.combat.service_prompt = 3;
	s.combat.service_text = "Wait";
	const auto &prompt = compiler.compile(s, 1024, 768);
	CHECK(pages(prompt, kHudFontSlotLabelLarge) == 4 && pages(prompt, kHudFontSlotHud) == 0);
	float left = 1e9f, right = -1e9f;
	for (const auto &g : prompt.glyphs) {
		left = std::min(left, g.x_top_left);
		right = std::max(right, g.x_bottom_right);
	}
	CHECK(std::abs((left + right) / 2 - 512) < 4);

	s = {};
	s.weapon.active = true;
	s.combat.inset = true;
	s.combat.inset_fov_over_zoom = 80;
	s.combat.inset_friendly = true;
	s.combat.target_name = "Ally";
	const auto &inset = compiler.compile(s, 1024, 768);
	CHECK(pages(inset, kHudFontSlotLabelBold) == 4 && pages(inset, kHudFontSlotHud) == 0);
	CHECK(std::all_of(inset.glyphs.begin(), inset.glyphs.end(),
			[](const auto &g) { return g.color == 0xFF7F0000u; }));

	// "89 m" measures 4 * (8 + 1) - 1 = 35 in the font, 70 at the bold slot's
	// 2x: the hudpos-font line starts at design 512 - 35 and runs left to right.
	s = {};
	s.combat.impact_distance = true;
	s.combat.impact_distance_m = 89;
	s.combat.impact_format = "%d m"; // the authored Overlays/STROVER_DIST shape
	const auto &distance = compiler.compile(s, 1024, 768);
	CHECK(pages(distance, kHudFontSlotHud) == 4 && pages(distance, kHudFontSlotLabelBold) == 0);
	CHECK(!distance.glyphs.empty() && near(distance.glyphs[0].x_top_left, 476.5f));

	// A slot whose file is absent falls back to the hudpos font at scale 1.
	compiler.configure_label_fonts(nullptr, nullptr, nullptr, 1.0f, 1.0f);
	s = {};
	s.combat.service_prompt = 3;
	s.combat.service_text = "Wait";
	CHECK(pages(compiler.compile(s, 1024, 768), kHudFontSlotHud) == 4);
}

static void mortar_map_callbacks() {
	WeaponFsmActionRow rows[3];
	const char *names[] = { "scopeup", "scopedown", "switchfrom" };
	const char *functions[] = { "wpn_std_scopeup_map", "wpn_std_scopedown_map",
		"wpn_std_switchfrom_map" };
	for (int i = 0; i < 3; ++i) {
		std::strcpy(rows[i].name, names[i]);
		std::strcpy(rows[i].function, functions[i]);
		rows[i].delaystart = rows[i].delayend = 0;
	}
	WeaponFsmDef def;
	weapon_fsm_bake(rows, 3, nullptr, nullptr, nullptr, def);
	HudMapControl map;
	for (int action :
			{ weapon_action::kScopeUp, weapon_action::kScopeDown, weapon_action::kSwitchFrom }) {
		WeaponSlotState slot;
		slot.current = uint8_t(action);
		slot.next = slot.current;
		slot.phase = weapon_phase::kActive;
		WeaponFsmInputs in;
		WeaponFsmEvents ev;
		weapon_fsm_tick(def, slot, in, ev);
		CHECK(ev.map_command == (action == weapon_action::kScopeUp ? 1 : -1));
		map.mode = action == weapon_action::kScopeUp ? 0 : 2;
		map.weapon_command(ev.map_command);
		CHECK(map.mode == (action == weapon_action::kScopeUp ? 2 : 0));
		map.mode = 3;
		map.weapon_command(ev.map_command);
		CHECK(map.mode == 3);
	}
}

static void world_feeds() {
	World world;
	world.registry.configure_pool(0, 8);
	Entity seed;
	seed.alive = true;
	seed.health = 100;
	seed.team = 1;
	seed.has_item_def = true;
	seed.item_type = 3;
	seed.position = { 0, 0, 10 };
	seed.yaw = 90;
	const auto player = world.registry.spawn(0, seed);
	world.cached.local_player = player;
	world.ai.attach(player);
	auto *body = world.ai.for_handle(player);
	body->pos[2] = 10 << 16;
	body->heading = 0;
	auto *p = world.registry.get(player);
	seed.position = { 100, 0, 10 };
	seed.display_name = "Squadmate";
	const auto target = world.registry.spawn(0, seed);
	body->inf.combat_target = target;
	body->slot.f[2] = 1;
	LocalPlayerWeapon weapon;
	weapon.active = true;
	weapon.def_name = "MORTAR";
	LocalPlayerViewFrame frame;
	LocalPlayerViewTracker tracker;
	auto read = [&](bool scoped = false) {
		frame = {};
		frame.scope_settled = scoped;
		fill_hud_combat_view(world, weapon, frame, tracker, scoped);
	};
	read();
	CHECK(frame.hud_combat.state.target_cursor && frame.hud_combat.state.target_locked);
	CHECK(frame.hud_combat.state.target_brackets && frame.hud_combat.state.inset_friendly);
	// Every HUD-owning instance is a client (modes 2 and 3): bit 3 admits or
	// refuses the brackets on a listen host and a joiner alike, and bit 8 --
	// the dedicated host's -- never reaches a HUD. [orig: @0x5926D4..0x5926E1]
	world.rules.mpattrib = 0x100;
	read();
	CHECK(frame.hud_combat.state.target_brackets);
	world.rules.mpattrib = 8;
	read();
	CHECK(!frame.hud_combat.state.target_brackets);
	world.rules.mp_session = true;
	world.rules.projectile_authority = false;
	read();
	CHECK(!frame.hud_combat.state.target_brackets);
	// Inside a session only a team game type admits a teammate target: the
	// same admitted target draws nothing in a non-team type, and its brackets
	// and friendly inset come back in a team one. Outside a session (single
	// player) every type admits it.
	// [orig: HUD_DrawCrosshair -- `cmp g_NapiNPCtx.is_in_session` @0x5926C0,
	//  `test g_GameType,10000h` @0x5926C4, the clear @0x5926D0]
	world.rules.mpattrib = 0x100;
	read();
	CHECK(!frame.hud_combat.state.target_brackets && !frame.hud_combat.state.inset_friendly);
	MatchRules team;
	team.game_type = 0x10000u;
	world.match.configure(team);
	read();
	CHECK(frame.hud_combat.state.target_brackets && frame.hud_combat.state.inset_friendly);
	world.match.configure(MatchRules{});
	world.rules.mp_session = false;
	read();
	CHECK(frame.hud_combat.state.target_brackets && frame.hud_combat.state.inset_friendly);
	world.rules.mp_session = true;
	world.rules.mpattrib = 0;
	Entity mount;
	mount.has_item_def = true;
	mount.item_unit_type = 3;
	mount.position.z = 100;
	mount.veh.ground_cache = 10 << 16;
	mount.hud_image = "helo.tga";
	const auto mounted = world.registry.spawn(0, mount);
	p = world.registry.get(player);
	p->mount_target = mounted;
	p->mounted = true;
	p->mount_type = SeatType::Controller;
	p->net_stance_bits = 2;
	p->carry_flags = 24;
	read();
	CHECK(frame.hud_combat.state.vehicle_controls && frame.hud_combat.state.gear == 0);
	CHECK(frame.hud_combat.state.altitude_agl_q16 == 90 << 16);
	CHECK(frame.hud_combat.state.parachute && frame.hud_combat.state.armor);
	CHECK(!frame.hud_combat.state.target_brackets &&
			frame.hud_combat.vehicle_texture == "helo.tga");
	Entity station;
	station.has_item_def = true;
	station.item_attrib2 = 0x2000;
	station.zone_number = 2;
	const auto station_handle = world.registry.spawn(0, station);
	world.registry.get(mounted)->ground_target = station_handle;
	world.registry.get(mounted)->carry_flags = 64;
	auto &service = tracker.hud_service;
	service.reload_seconds = 7;
	service.owned_zone_mask = 4;
	frame = {};
	fill_hud_combat_view(world, weapon, frame, tracker, false);
	CHECK(frame.hud_combat.state.service_prompt == 3 &&
			frame.hud_combat.state.service_wait_seconds == 7);
	CHECK(frame.hud_combat.state.service_above_declutter);
	service.owned_zone_mask = 0;
	frame = {};
	fill_hud_combat_view(world, weapon, frame, tracker, false);
	CHECK(frame.hud_combat.state.service_prompt == 0);
	service.preround_seconds = 3;
	frame = {};
	fill_hud_combat_view(world, weapon, frame, tracker, false);
	CHECK(frame.hud_combat.state.service_prompt == 1 &&
			!frame.hud_combat.state.service_above_declutter);
	service.preround_seconds = 0;
	service.owned_zone_mask = 4;
	service.reload_seconds = 0;
	frame = {};
	fill_hud_combat_view(world, weapon, frame, tracker, false);
	CHECK(frame.hud_combat.state.service_prompt == 4);
	service.reload_seconds = 255;
	frame = {};
	fill_hud_combat_view(world, weapon, frame, tracker, false);
	CHECK(frame.hud_combat.state.service_prompt == 0);
	p->mount_type = SeatType::Passenger;
	read();
	CHECK(!frame.hud_combat.state.vehicle_controls);
	CHECK(frame.hud_combat.state.altitude_agl_q16 == 0);
	p->mount_target = {};
	p->mount_type = SeatType::None;
	p->mounted = false;
	p->carry_flags = 0;
	read();
	CHECK(!frame.hud_combat.state.parachute && frame.hud_combat.vehicle_texture.empty());
	world.tables.weapons.entries.resize(1);
	auto &def = world.tables.weapons.entries[0];
	def.valid = true;
	def.name = "MORTAR";
	def.ammo_index = 0;
	world.tables.ammo.entries.resize(1);
	auto &ammo = world.tables.ammo.entries[0];
	ammo.valid = true;
	ammo.velocity = 62;
	ammo.max_age_ticks = 200;
	ammo.kz_maxradius = 19;
	def.hud_splash_radius = 3;
	uint16_t heights[16] = {};
	terrain::TerrainHeightField field;
	field.heightmap = heights;
	field.dim = 4;
	world.ai.terrain = &field;
	const int rounds = world.round_sim.active_count;
	const auto impact = predict_hud_impact(world, weapon, true, 0);
	CHECK(impact.hit && impact.position[2] == 0);
	// z starts at 10u; each falling-object tick subtracts 167 more than the last.
	// 89 ticks reach the water/terrain plane; horizontal motion is 1u per tick.
	CHECK(impact.position[0] == 89 << 16 && impact.position[1] == 0);
	CHECK(impact.distance_q16 == 89 << 16 && impact.radius_q16 == 3 << 16);
	CHECK(world.round_sim.active_count == rounds && p->health == 100);
	weapon.def.flags = 0x80000 | 0x8000 | 0x100000 | 0x800000;
	read();
	CHECK(!frame.hud_combat.state.impact_map && !frame.hud_combat.state.impact_distance);
	read(true);
	CHECK(frame.hud_combat.state.impact_map && frame.hud_combat.state.impact_distance);
	CHECK(!frame.inset_scope_active && frame.hud_combat.state.impact_distance_m == 89);
	weapon.def.flags |= def::DEF_WEAPON_FLAG_USEDESIGNATOR;
	tracker.hud_designations = { { 89 << 16, 0, 10 << 16, 2 }, { 95 << 16, 0, 12 << 16, 1 } };
	read(true);
	CHECK(frame.hud_combat.state.impact_radius_q16 == 12 << 16); // friendly inside radius
	tracker.hud_designations = { { 91 << 16, 0, 1 << 16, 1 } };
	read(true);
	CHECK(frame.hud_combat.state.impact_radius_q16 == 2 << 16); // outside caps spread
	tracker.hud_designations.clear();
	read(true);
	CHECK(frame.hud_combat.state.impact_radius_q16 == 3 << 16);
	// The dead bit and the death lerp camera gate neither the preview nor its
	// cues, and neither is the HUD's death-screen flag.
	// [orig: Player_UpdatePerFrame @0x4DE760..0x4DE79D]
	world.registry.get(player)->flags |= kEntityFlagDead;
	frame = {};
	frame.scope_settled = true;
	frame.camera_mode = 4;
	fill_hud_combat_view(world, weapon, frame, tracker, true);
	CHECK(frame.hud_combat.state.impact_distance && frame.hud_combat.state.designator);
	CHECK(!frame.hud_combat.state.death_screen);
	world.registry.get(player)->flags &= ~uint32_t(kEntityFlagDead);
	ammo.max_age_ticks = 1; // Miss after a successful preview.
	read(true);
	CHECK(!frame.hud_combat.state.impact_map);
	CHECK(frame.hud_combat.state.impact_distance_m == 89);
	CHECK(frame.hud_combat.impact[2] == 10 << 16);
	weapon.active = false;
	read(true);
	CHECK(!frame.hud_combat.state.impact_map);
}

static void parser_and_received_feedback() {
	const char text[] =
			"weapon HUD_TEST\n crosshair primary.tga secondary.tga\n commandersX commander.tga\n "
			"hud_loadout_select bar.tga\n hudicon gun.tga\n splash 17\nend\n";
	def::DefWeaponsFile defs{};
	CHECK(def::def_parse_weapons_memory(
				  reinterpret_cast<const uint8_t *>(text), sizeof(text) - 1, &defs) == 0);
	CHECK(defs.count == 1);
	if (defs.count) {
		CHECK(defs.entries[0].splash == 17);
		CHECK(std::strcmp(defs.entries[0].crosshair, "primary.tga") == 0);
		CHECK(std::strcmp(defs.entries[0].crosshair_secondary, "secondary.tga") == 0);
		CHECK(std::strcmp(defs.entries[0].commanders_x, "commander.tga") == 0);
		CHECK(std::strcmp(defs.entries[0].hud_loadout_select, "bar.tga") == 0);
	}
	def::def_free_weapons(&defs);
	replication::ClientReplicaPipeline peer;
	auto receive = [&](uint8_t flags) {
		FrameUpdate frame;
		frame.flags1 = flags;
		peer.apply(0x0A, encode_frame_update(frame));
	};
	receive(4);
	CHECK(peer.state().hud_hit_feedback_frames == 10);
	// Folded datagrams each count; reading/rendering the retained state never does.
	receive(0);
	receive(0);
	CHECK(peer.state().hud_hit_feedback_frames == 8);
	for (int i = 0; i < 100; ++i)
		CHECK(peer.state().hud_hit_feedback_frames == 8);
	receive(4);
	CHECK(peer.state().hud_hit_feedback_frames == 10);
	for (int i = 0; i < 11; ++i)
		receive(0);
	CHECK(peer.state().hud_hit_feedback_frames == 0);
	FrameUpdate service;
	service.flags2 = 0;
	service.weapon.reload_seconds = 37;
	service.weapon.uniform_team_mask = 0x104;
	service.weapon.preround_timer = 8;
	peer.apply(0x0A, encode_frame_update(service));
	CHECK(peer.state().vehicle_reload_seconds == 37 && peer.state().owned_zone_mask == 0x104);
	service.flags2 = 1;
	peer.apply(0x0A, encode_frame_update(service));
	CHECK(peer.state().vehicle_reload_seconds == 37 && peer.state().owned_zone_mask == 0x104);
}
static void sent_feedback() {
	World world;
	world.registry.configure_pool(0, 2);
	Entity seed;
	seed.alive = true;
	seed.health = 100;
	const auto owner = world.registry.spawn(0, seed);
	replication::LoopbackChannel transport;
	replication::Connection conn;
	conn.transport = &transport;
	conn.owned_entity = owner;
	replication::ClientReplicaPipeline peer;
	auto send = [&]() {
		CHECK(replication::emit_connection_s2c(world, conn, {}));
		replication::Datagram packet;
		CHECK(transport.client_recv(packet));
		peer.apply(packet.tag, packet.body);
	};
	send();
	CHECK(peer.state().hud_hit_feedback_frames == 0);
	++world.registry.get(owner)->hud_hit_feedback_serial;
	send();
	CHECK(peer.state().hud_hit_feedback_frames == 10);
	send();
	CHECK(peer.state().hud_hit_feedback_frames == 9);
	world.rules.hit_feedback = false;
	++world.registry.get(owner)->hud_hit_feedback_serial;
	send();
	CHECK(peer.state().hud_hit_feedback_frames == 8);
	world.rules.hit_feedback = true;
	send();
	CHECK(peer.state().hud_hit_feedback_frames == 10);
}
// The projected cues reach design space through the integer screen mapping
// (x * 1024 + w / 2) / w, not a truncated float scale, and the commander's
// clamp runs in integers with its line ending at the integer screen centre.
// [orig: Viewport_ScreenToVirtual @0x5D2C70; HUD_DrawScopeOverlayDetails
//  @0x59E77E..0x59E87F]
static void integer_screen_mapping() {
	CHECK(screen_to_design_x(499, 1000) == 511); // 510.976 rounds, not truncates
	CHECK(screen_to_design_x(1000, 1000) == 1024);
	CHECK(screen_to_design_y(300, 768) == 300);
	CHECK(screen_to_design_y(1, 1080) == 1); // (768 + 540) / 1080
	CHECK(screen_to_design_x(-3, 1000) == -2); // idiv truncates toward zero
	HudLayout layout;
	layout.combat.target = { 12, 16, true };
	layout.combat.commander = { 10, 10, true };
	HudFrameCompiler compiler;
	compiler.configure(layout, nullptr);
	HudFrameState s;
	s.weapon.active = true; // the targeting cues ride the weapon cluster
	s.combat.target_cursor = true;
	s.combat.target_point = { 499, 300, 0, true };
	const auto &target = compiler.compile(s, 1000, 768);
	bool found = false;
	for (const auto &q : target.quads) {
		if (q.texture != kHudTexTarget) continue;
		found = true;
		// Design centre 511 -> left 505 -> floor((505 * 1000 + 512) / 1024).
		CHECK(near(q.x0, 493));
	}
	CHECK(found);
	s = {};
	s.weapon.active = true;
	s.scope.active = true;
	s.combat.commander = true;
	s.combat.commander_point = { 998, 383, 0, true };
	const auto &command = compiler.compile(s, 999, 768);
	// Design (1023, 383): dx 511, dy -1, distance 511, so the clamp lands on
	// (334 * 511 / 511 + 512, 334 * -1 / 511 + 384) = (846, 384).
	bool reticle = false;
	for (const auto &q : command.quads) {
		if (q.texture != kHudTexCommander) continue;
		reticle = true;
		CHECK(near(q.x0, float(scale_axis(841, 999, 1024))));
		CHECK(near(q.y0, float(scale_axis(379, 768, 768))));
	}
	CHECK(reticle);
	CHECK(std::any_of(command.lines.begin(), command.lines.end(), [](const auto &l) {
		return l.color == uint32_t(-32736) && near(l.x0, float(scale_axis(846, 999, 1024))) &&
				near(l.x1, 499) && near(l.y1, 384);
	}));
}

int main() {
	integer_screen_mapping();
	geometry_and_draw();
	mortar_map_callbacks();
	mortar_map_and_world_cues();
	combat_text_font_slots();
	world_feeds();
	parser_and_received_feedback();
	sent_feedback();
	if (!failures)
		std::puts("hud_combat_test: all checks passed");
	return failures ? 1 : 0;
}
