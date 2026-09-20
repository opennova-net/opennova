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
	s.combat.dead = true;
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
	state.combat.impact_point.clip = 16;
	CHECK(compiler.compile(state, 1024, 768).tris.empty());
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
	world.rules.mpattrib = 0x100;
	read();
	CHECK(frame.hud_combat.state.target_brackets);
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
int main() {
	geometry_and_draw();
	mortar_map_callbacks();
	mortar_map_and_world_cues();
	world_feeds();
	parser_and_received_feedback();
	sent_feedback();
	if (!failures)
		std::puts("hud_combat_test: all checks passed");
	return failures ? 1 : 0;
}
