// The local player's radar contact state and its producers: the add (edge
// timer, first free row, life 62, the NoTracers gate), the per-tick update
// (bearing -> 12/24 sectors, the ageing, the kind-255 counter bump), the
// missile list's one-past append, the round reset, the damage kind rule and
// its callers' entry point, the tracer whiz, the Stinger lock note, the whiz
// radius resolve, and the per-HUD-frame snapshot.
// [orig: Radar_AddBlip @0x59b280; Radar_UpdateContacts @0x59a7e0;
//  sub_59B200 @0x59b200; HUD_ResetAllOverlayBuffers @0x59dd40;
//  Player_OnDamageReceived @0x4dd880; Projectile_SpawnTracerScarEffect
//  @0x4e5ac0; Entity_UpdateGuidedMissile_0 @0x4465db; AmmoDef_InitEffectsTable
//  @0x409f20; HUD_RenderAllOverlays @0x5a817d / @0x5a87ef]
#include <runtime/audio/oneshot_play.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_view.h>
#include <runtime/world/radar_contacts.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

using namespace opennova::world;

static int g_failures = 0;
#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::printf("FAIL line %d: %s\n", __LINE__, msg);                  \
			++g_failures;                                                      \
		}                                                                      \
	} while (0)

namespace {

constexpr int32_t kOrigin[3] = {0, 0, 0};
constexpr int32_t kEast[3] = {10 << 16, 0, 0};
constexpr int32_t kNorth[3] = {0, 10 << 16, 0};

int lit(const uint8_t *sectors, int n) {
	int count = 0;
	for (int i = 0; i < n; ++i) count += sectors[i] != 0;
	return count;
}

void test_add() {
	RadarContactState state;
	radar_add_blip(state, 0, kOrigin, 0, 77, kEast, kRadarKindRed12);
	CHECK(state.rows[0].life == kRadarContactLife && state.rows[0].life == 62, "a fresh row lives 62");
	CHECK(state.rows[0].kind == 0 && state.rows[0].source == 77 && state.rows[0].pos[0] == (10 << 16),
			"the row keeps the source, point and kind");
	// East at yaw 0 is quadrant 0 of the 0x1FFFFFE0-biased edge split; north 1.
	CHECK(state.edge[0] == 31 && state.edge[1] == 0, "the add stamps its edge timer to 31");
	radar_add_blip(state, 0, kOrigin, 0, 78, kNorth, kRadarKindOlive12);
	CHECK(state.rows[1].life == 62 && state.edge[1] == 31, "the next add takes the next free row");
	// NoTracers drops the whole add, edge timer included.
	RadarContactState gated;
	radar_add_blip(gated, kRadarRulesNoTracers, kOrigin, 0, 1, kEast, 0);
	CHECK(gated.rows[0].life == 0 && gated.edge[0] == 0, "mpattrib bit 0 gates the add");
	// A full table drops the row but still stamps the edge.
	RadarContactState full;
	for (int i = 0; i < kRadarContactRows; ++i) radar_add_blip(full, 0, kOrigin, 0, 1, kNorth, 0);
	full.edge.fill(0);
	radar_add_blip(full, 0, kOrigin, 0, 2, kEast, 0);
	bool any_source2 = false;
	for (const RadarContact &row : full.rows) any_source2 |= row.source == 2;
	CHECK(!any_source2 && full.edge[0] == 31, "the 129th add is dropped after its edge stamp");
}

void test_update_sectors_and_ageing() {
	RadarContactState state;
	radar_add_blip(state, 0, kOrigin, 0, 1, kEast, kRadarKindRed12);
	radar_add_blip(state, 0, kOrigin, 0, 2, kNorth, kRadarKindOlive12);
	radar_add_blip(state, 0, kOrigin, 0, 3, kEast, kRadarKindRed24);
	radar_add_blip(state, 0, kOrigin, 0, 4, kNorth, kRadarKindOlive24);
	radar_update_contacts(state, 10, kOrigin, 0);
	CHECK(state.red12[2] == 1 && lit(state.red12.data(), 12) == 1, "east lights red 12-sector 2");
	CHECK(state.olive12[5] == 1 && lit(state.olive12.data(), 12) == 1, "north lights olive 12-sector 5");
	CHECK(state.red24[4] == 1 && lit(state.red24.data(), 24) == 1, "east lights red 24-sector 4");
	CHECK(state.olive24[10] == 1 && lit(state.olive24.data(), 24) == 1, "north lights olive 24-sector 10");
	CHECK(state.rows[0].life == 52 && state.last_tick == 10, "the rows age by the elapsed ticks");
	CHECK(state.edge[0] == 21, "the edge timers age by the same count");
	// Same tick: a no-op.
	radar_update_contacts(state, 10, kOrigin, 0);
	CHECK(state.rows[0].life == 52, "one update per tick");
	// The viewer yaw turns the sectors: a north-facing viewer puts east at 11.
	radar_update_contacts(state, 11, kOrigin, 0x40000000u);
	CHECK(state.red12[11] == 1 && state.red12[2] == 0, "the sectors rebuild relative to the yaw");
	CHECK(state.olive12[2] == 1, "north is dead ahead of a north-facing viewer");
	// Expiry: a row whose life is at or under the elapsed count frees.
	radar_update_contacts(state, 11 + 51, kOrigin, 0);
	CHECK(state.rows[0].life == 0 && lit(state.red12.data(), 12) == 0, "life <= elapsed frees the row");
	CHECK(state.edge[0] == 0, "and the edge timers bottom out");
}

void test_kind255_counter_bump() {
	RadarContactState state;
	// Rows 0..2 are ordinary contacts, row 3 the self damage, the tail rows
	// ordinary again.
	for (int i = 0; i < 3; ++i) radar_add_blip(state, 0, kOrigin, 0, 1, kEast, kRadarKindOlive24);
	radar_add_blip(state, 0, kOrigin, 0, 1, kOrigin, kRadarKindSelf);
	for (int i = 4; i < kRadarContactRows; ++i)
		radar_add_blip(state, 0, kOrigin, 0, 1, kNorth, kRadarKindOlive12);
	radar_update_contacts(state, 5, kOrigin, 0);
	// Row 3's fill lights red 12-sectors 3..11 and leaves the counter at 12:
	// the sweep then ends nine rows early, so rows 119..127 neither age nor
	// light.
	for (int k = 0; k < 12; ++k)
		CHECK((state.red12[static_cast<size_t>(k)] != 0) == (k >= 3), "the fill runs from the counter to 11");
	CHECK(state.rows[118].life == 57, "row 118 is the last one the sweep ages");
	CHECK(state.rows[119].life == 62 && state.rows[127].life == 62, "the tail rows are skipped");
	CHECK(state.olive12[5] == 1, "the walked north rows still light");
	// A self row at index 12 or later lights nothing at all.
	RadarContactState late;
	for (int i = 0; i < 12; ++i) radar_add_blip(late, 0, kOrigin, 0, 1, kEast, kRadarKindOlive24);
	radar_add_blip(late, 0, kOrigin, 0, 1, kOrigin, kRadarKindSelf);
	radar_update_contacts(late, 5, kOrigin, 0);
	CHECK(lit(late.red12.data(), 12) == 0, "a self row past index 11 fills nothing");
}

void test_missile_list_quirk() {
	RadarContactState state;
	const int32_t p1[3] = {1, 2, 3};
	const int32_t p2[3] = {4, 5, 6};
	radar_note_missile(state, 42, p1);
	CHECK(state.missile_count == 1 && state.missiles[1].source == 42 && state.missiles[0].source == 0,
			"the first append lands at index 1, row 0 stays null");
	radar_note_missile(state, 42, p2);
	CHECK(state.missile_count == 2 && state.missiles[2].source == 42,
			"the newest row sits outside [0, count), so a repeat appends again");
	radar_note_missile(state, 42, p1);
	CHECK(state.missile_count == 2 && state.missiles[1].pos[0] == 1,
			"the third note finds row 1 and refreshes it");
	RadarContactState full;
	for (uint32_t i = 1; i <= 64; ++i) radar_note_missile(full, i, p1);
	CHECK(full.missile_count == 64 && full.missiles[64].source == 64,
			"the 64th append writes one past the table");
	radar_note_missile(full, 999, p1);
	CHECK(full.missile_count == 64, "a full list drops the note");
	// The round reset clears the 64 rows, the contacts, the edges and the tick,
	// never the count.
	radar_add_blip(full, 0, kOrigin, 0, 1, kEast, 0);
	radar_update_contacts(full, 3, kOrigin, 0);
	radar_reset(full);
	CHECK(full.missiles[1].source == 0 && full.rows[0].life == 0 && full.edge[0] == 0 &&
			full.last_tick == 0, "the reset zeroes the tables and the tick");
	CHECK(full.missile_count == 64 && full.missiles[64].source == 64,
			"the count and the overflow row survive the reset");
}

void test_damage_kind() {
	Entity class6;
	class6.player_class = 6;
	Entity class5;
	class5.player_class = 5;
	CHECK(radar_damage_kind({1, true, nullptr}) == 255, "self damage is kind 255");
	CHECK(radar_damage_kind({}) == 0, "a null source is kind 0");
	CHECK(radar_damage_kind({1, false, nullptr}) == 0, "a source without a +0x170 entity is 0");
	CHECK(radar_damage_kind({1, false, &class6}) == 2, "a class-6 +0x170 entity is kind 2");
	CHECK(radar_damage_kind({1, false, &class5}) == 0, "any other class is kind 0");
}

struct Rig {
	std::unique_ptr<World> world = std::make_unique<World>();
	std::unique_ptr<LocalPlayer> local;
	EntityHandle self;
	EntityHandle enemy;
	Rig() {
		world->registry.configure_pool(0, 8);
		Entity seed;
		seed.kind = EntityKind::Organic;
		seed.health = 100;
		seed.team = 1;
		self = world->registry.spawn(0, seed);
		seed.team = 2;
		seed.player_class = 6;
		seed.position = {30.0f, 0.0f, 0.0f};
		enemy = world->registry.spawn(0, seed);
		world->ai.attach(self);
		if (AiEntity *body = world->ai.for_handle(self)) {
			body->pos[0] = body->pos[1] = body->pos[2] = 0;
			body->heading = 0; // facing east: yaw 0
		}
		world->cached.local_player = self;
		local = std::make_unique<LocalPlayer>(*world);
		world->local_player_state = local.get();
	}
	~Rig() { world->local_player_state = nullptr; }
};

void test_damage_entry_point() {
	Rig rig;
	World &world = *rig.world;
	RadarContactState &state = rig.local->radar;
	// A blast from the class-6 enemy: kind 0 (the enemy's own +0x170 is empty).
	const int32_t blast[3] = {10 << 16, 0, 0};
	player_on_damage_received(world, radar_entity_source(world, rig.enemy), blast);
	CHECK(state.rows[0].life == 62 && state.rows[0].kind == 0, "the blast blip is kind 0");
	// A round from that enemy: the projectile's +0x170 is the class-6 shooter.
	LiveRound round;
	round.owner = rig.enemy;
	player_on_damage_received(world, radar_round_source(world, round, 5), blast);
	CHECK(state.rows[1].kind == 2, "a class-6 shooter's round is kind 2");
	// The fall: the body itself.
	player_on_damage_received(world, radar_entity_source(world, rig.self), kOrigin);
	CHECK(state.rows[2].kind == 255, "self damage is kind 255");
	CHECK(rig.local->view.flash.red == 255, "the vignette arms alongside");
	// NoTracers keeps the vignette but drops the blip.
	world.rules.mpattrib = kRadarRulesNoTracers;
	player_on_damage_received(world, radar_entity_source(world, rig.enemy), blast);
	CHECK(state.rows[3].life == 0, "the rules gate drops the damage blip");
	world.rules.mpattrib = 0;
}

void test_tracer_whiz() {
	Rig rig;
	World &world = *rig.world;
	RadarContactState &state = rig.local->radar;
	world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
	AmmoTableEntry ammo;
	ammo.valid = true;
	ammo.whiz_radius_q16 = 5 << 16;
	LiveRound round;
	round.owner = rig.enemy;
	// A round from the enemy at x = 30 flying west along y = 1 passes the
	// listener at the origin one unit off.
	const FixedVec3 start{3 << 16, 1 << 16, 0};
	const FixedVec3 kPastListener{-(1 << 16), 1 << 16, 0};
	const FixedVec3 velocity{-(4 << 16), 0, 0};
	round_tracer_whiz(world, round, &ammo, start, kPastListener, velocity);
	CHECK(round.whiz_latched, "a pass inside the radius latches the round");
	CHECK(state.rows[0].life == 62 && state.rows[0].kind == kRadarKindOlive12,
			"an enemy shooter lights the olive 12-ring");
	CHECK(state.rows[0].pos[0] == (30 << 16), "the blip sits at the shooter, not the round");
	// Latched: never again.
	round_tracer_whiz(world, round, &ammo, start, kPastListener, velocity);
	CHECK(state.rows[1].life == 0, "the latch whizzes a round once");
	// Out of the box: no judgement, no latch.
	LiveRound far = LiveRound{};
	far.owner = rig.enemy;
	round_tracer_whiz(world, far, &ammo, {3 << 16, 9 << 16, 0}, {-(1 << 16), 9 << 16, 0}, velocity);
	CHECK(!far.whiz_latched && state.rows[1].life == 0, "a pass outside the radius is ignored");
	// Receding: t < 0 fails the gate.
	LiveRound away;
	away.owner = rig.enemy;
	round_tracer_whiz(world, away, &ammo, {-(1 << 16), 1 << 16, 0}, {-(5 << 16), 1 << 16, 0}, velocity);
	CHECK(!away.whiz_latched, "a round already past the listener does not whiz");
	// fgrenade ammo never whizzes.
	AmmoTableEntry grenade = ammo;
	grenade.flags = 0x10000000u;
	LiveRound nade;
	nade.owner = rig.enemy;
	round_tracer_whiz(world, nade, &grenade, start, kPastListener, velocity);
	CHECK(!nade.whiz_latched, "fgrenade skips the leg");
	// Silenced ammo latches but adds no blip.
	AmmoTableEntry silenced = ammo;
	silenced.flags = 0x8u;
	LiveRound quiet;
	quiet.owner = rig.enemy;
	round_tracer_whiz(world, quiet, &silenced, start, kPastListener, velocity);
	CHECK(quiet.whiz_latched && state.rows[1].life == 0, "silenced rounds latch without a blip");
	// A teammate in a team game adds no blip.
	world.registry.get(rig.enemy)->team = 1;
	MatchRules team;
	team.game_type = 0x10000u;
	world.match.configure(team);
	LiveRound friendly;
	friendly.owner = rig.enemy;
	round_tracer_whiz(world, friendly, &ammo, start, kPastListener, velocity);
	CHECK(friendly.whiz_latched && state.rows[1].life == 0, "a teammate's round adds no blip");
	// A class-6 shooter holding a category-3 weapon lights the olive 24-ring.
	world.registry.get(rig.enemy)->team = 2;
	WeaponTableEntry sniper;
	sniper.valid = true;
	sniper.category = 3;
	world.tables.weapons.entries.assign(4, WeaponTableEntry{});
	world.tables.weapons.entries[3] = sniper;
	world.registry.get(rig.enemy)->equipped_adm_index = 3;
	LiveRound scoped;
	scoped.owner = rig.enemy;
	round_tracer_whiz(world, scoped, &ammo, start, kPastListener, velocity);
	CHECK(state.rows[1].kind == kRadarKindOlive24, "a class-6 category-3 shooter takes the 24-ring");
}

void test_lock_note_and_hud_frame() {
	Rig rig;
	World &world = *rig.world;
	RadarContactState &state = rig.local->radar;
	world.round_sim.rounds[9].pos = {5.0f, 6.0f, 0.0f};
	world.round_sim.rounds[9].guided.target = rig.self.packed;
	world.round_sim.rounds[9].guided.pos[0] = 1 << 16;
	radar_note_guided_missile(world, world.round_sim.rounds[9], 9);
	CHECK(state.missile_count == 1 && state.missiles[1].source != 0,
			"a missile locked on the local player rides the list");
	world.round_sim.rounds[10].guided.target = rig.enemy.packed;
	radar_note_guided_missile(world, world.round_sim.rounds[10], 10);
	CHECK(state.missile_count == 1, "a missile on someone else is not noted");
	// A second note makes row 1 visible to the ring ([0, count) = rows 0, 1).
	radar_note_guided_missile(world, world.round_sim.rounds[9], 9);
	CHECK(state.missile_count == 2, "the repeat appends past the null row");
	radar_add_blip(world, 7, kEast, kRadarKindRed12);
	opennova::hud::HudMinimapRadar radar;
	CHECK(radar_hud_frame(world, 20, true, false, false, radar) == 20,
			"the frame's update reports the ticks it aged");
	CHECK(radar.red12[2] == 1, "the frame runs the contact update");
	CHECK(radar.threats.size() == 2 && radar.threats[0].present == 0 &&
			radar.threats[1].present == 1, "the snapshot keeps row 0, null");
	CHECK(radar.threats[1].x == (5 << 16) && radar.threats[1].y == (6 << 16),
			"the ring reads the missile's live position");
	CHECK(state.missile_count == 0, "the pass clears the count after the map draw");
	// The pass early-outs neither update nor clear.
	radar_note_guided_missile(world, world.round_sim.rounds[9], 9);
	CHECK(radar_hud_frame(world, 30, false, false, false, radar) == 0, "a skipped pass ages nothing");
	CHECK(state.last_tick == 20 && state.missile_count == 1, "a skipped pass leaves the state alone");
	// The death screen skips the pass's update unless the map site runs.
	rig.local->view.death_screen_active = true;
	radar_hud_frame(world, 40, true, false, false, radar);
	CHECK(state.last_tick == 20, "the death screen skips the pass's own update");
	CHECK(radar_hud_frame(world, 50, true, true, false, radar) == 30,
			"the map site's update ages from the last one");
	CHECK(state.last_tick == 50, "the corner map's site still updates");
	rig.local->view.death_screen_active = false;
	// The round reset.
	rig.local->reset_for_new_round();
	CHECK(state.last_tick == 0 && state.rows[0].life == 0, "the round init resets the radar");
}

void test_zip_impact() {
	Rig rig;
	World &world = *rig.world;
	world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
	AmmoTableEntry ammo;
	ammo.valid = true;
	ammo.whiz_radius_q16 = 5 << 16;
	const FixedVec3 start{3 << 16, 1 << 16, 0};
	const FixedVec3 end{-(1 << 16), 1 << 16, 0};
	const FixedVec3 velocity{-(4 << 16), 0, 0};
	LiveRound round;
	round.owner = rig.enemy;
	round.ammo_index = 2;
	round_tracer_whiz(world, round, &ammo, start, end, velocity);
	CHECK(world.round_sim.impacts.size() == 1, "the whiz presents one impact row");
	if (!world.round_sim.impacts.empty()) {
		const RoundImpact &zip = world.round_sim.impacts[0];
		CHECK(zip.effect_tag == 3 && zip.ammo_index == 2, "the row is the ammo's zip (tag 3)");
		CHECK(zip.present_sound && !zip.present_effect,
				"the 0x80000000 record plays the sound leg alone");
		CHECK(zip.position.x == 0.0f && zip.position.y == 1.0f && zip.position.z == 0.0f,
				"the zip sits at the closest point o + t*dir");
	}
	CHECK(round.prev_z_q16 == start.z, "the tail copy follows the whiz");
	// A latched round presents nothing more.
	round_tracer_whiz(world, round, &ammo, start, end, velocity);
	CHECK(world.round_sim.impacts.size() == 1, "the latch stops the zip too");
	// ClipWaterFx under water on both Z words drops the zip, not the blip.
	world.round_sim.impacts.clear();
	world.env.water_z = 5 << 16;
	AmmoTableEntry clip = ammo;
	clip.flags = kAmmoFlagClipWaterFx;
	LiveRound wet;
	wet.owner = rig.enemy;
	wet.prev_z_q16 = 0;
	round_tracer_whiz(world, wet, &clip, start, end, velocity);
	CHECK(world.round_sim.impacts.empty() && wet.whiz_latched,
			"a submerged ClipWaterFx round whizzes without its zip");
	LiveRound surfacing;
	surfacing.owner = rig.enemy;
	surfacing.prev_z_q16 = 6 << 16; // last tick's start above the plane
	round_tracer_whiz(world, surfacing, &clip, start, end, velocity);
	CHECK(world.round_sim.impacts.size() == 1, "a +0x88 above the plane keeps the zip");
	world.env.water_z = 0;
}

void test_wire_shooter_and_target() {
	Rig rig;
	World &world = *rig.world;
	RadarContactState &state = rig.local->radar;
	world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
	WeaponTableEntry sniper;
	sniper.valid = true;
	sniper.category = 3;
	world.tables.weapons.entries.assign(4, WeaponTableEntry{});
	world.tables.weapons.entries[3] = sniper;
	RoundSim::WireActor remote;
	remote.team = 2;
	remote.player_class = 6;
	remote.equipped_adm_index = 3;
	remote.pos[0] = 40 << 16;
	world.round_sim.wire_actor_provider = [&remote](uint16_t handle, RoundSim::WireActor &out) {
		if (handle == 0x0007) { // the local player's own wire handle
			out = RoundSim::WireActor{};
			out.is_local = true;
			return true;
		}
		if (handle != 0x0005) return false;
		out = remote;
		return true;
	};
	AmmoTableEntry ammo;
	ammo.valid = true;
	ammo.whiz_radius_q16 = 5 << 16;
	const FixedVec3 start{3 << 16, 1 << 16, 0};
	const FixedVec3 end{-(1 << 16), 1 << 16, 0};
	const FixedVec3 velocity{-(4 << 16), 0, 0};
	// A joiner's remote round: no registry owner, a wire shooter handle.
	LiveRound round;
	round.shooter_handle = 0x0005;
	round_tracer_whiz(world, round, &ammo, start, end, velocity);
	CHECK(state.rows[0].life == 62 && state.rows[0].kind == kRadarKindOlive24,
			"the wire proxy's class 6 / category 3 picks the olive 24-ring");
	CHECK(state.rows[0].pos[0] == (40 << 16), "the blip sits at the proxy's Position");
	// A teammate proxy in a team game adds nothing.
	remote.team = 1;
	MatchRules team;
	team.game_type = 0x10000u;
	world.match.configure(team);
	LiveRound mate;
	mate.shooter_handle = 0x0005;
	round_tracer_whiz(world, mate, &ammo, start, end, velocity);
	CHECK(mate.whiz_latched && state.rows[1].life == 0, "a teammate proxy adds no blip");
	// An unresolved handle is a shooterless round: latched, no blip.
	LiveRound unknown;
	unknown.shooter_handle = 0x0009;
	round_tracer_whiz(world, unknown, &ammo, start, end, velocity);
	CHECK(unknown.whiz_latched && state.rows[1].life == 0, "an unresolved proxy adds no blip");
	// The guided lock note: the local player's own wire handle is the
	// target-is-local arm, and a pool-0 wire target never reads the registry.
	world.round_sim.rounds[3].guided.target = 0x0007;
	radar_note_guided_missile(world, world.round_sim.rounds[3], 3);
	CHECK(state.missile_count == 1 && state.lock_tone == kRadarLockToneTicks,
			"a missile on the local wire handle is noted and sets the tone");
	state.lock_tone = 0;
	world.round_sim.rounds[4].guided.target = rig.self.packed; // a remote slot on a joiner
	radar_note_guided_missile(world, world.round_sim.rounds[4], 4);
	CHECK(state.missile_count == 1 && state.lock_tone == 0,
			"a pool-0 wire target resolves through the rows, not the registry");
	world.round_sim.wire_actor_provider = {};
}

void test_lock_tone() {
	Rig rig;
	World &world = *rig.world;
	RadarContactState &state = rig.local->radar;
	world.round_sim.rounds[9].guided.target = rig.self.packed;
	radar_note_guided_missile(world, world.round_sim.rounds[9], 9);
	CHECK(state.lock_tone == 31, "the lock note stores 31");
	radar_tick_lock_tone(state);
	CHECK(state.lock_tone == 30, "the pending-slot pass drains one per tick");
	world.tick_pending_sound_slots();
	CHECK(state.lock_tone == 29, "the world's pending-sound pass drains it");
	state.lock_tone = 0;
	radar_tick_lock_tone(state);
	CHECK(state.lock_tone == 0, "the drain stops at zero");
	// The HUD pass registers the LPLOCKONME loop while the word is live.
	state.lock_tone = 5;
	world.out.sound_emitters.clear();
	opennova::hud::HudMinimapRadar radar;
	radar_hud_frame(world, 1, true, false, false, radar);
	CHECK(world.out.sound_emitters.size() == 1, "a live tone registers one loop");
	if (world.out.sound_emitters.size() == 1) {
		const SoundEmitterEvent &tone = world.out.sound_emitters[0];
		CHECK(tone.set_name == "LPLOCKONME" && tone.lane == 255 && tone.lifetime_ticks == 20 &&
						tone.pitch_q16 == 0x10000 && tone.volume_q8_8 == 0xFFFF,
				"SoundEmitter_Register(local, LPLOCKONME, pos, 255, 20, 0x10000, 0xFFFF)");
		CHECK(tone.source_handle == rig.self.packed, "the loop rides the local player");
	}
	world.out.sound_emitters.clear();
	radar_hud_frame(world, 2, true, false, true, radar);
	CHECK(world.out.sound_emitters.empty(), "the in-game menu pause skips the loop");
	rig.local->view.death_screen_active = true;
	radar_hud_frame(world, 3, true, true, false, radar);
	CHECK(world.out.sound_emitters.empty(), "the death screen skips the loop");
	rig.local->view.death_screen_active = false;
	radar_hud_frame(world, 4, false, false, false, radar);
	CHECK(world.out.sound_emitters.empty(), "a skipped pass registers nothing");
	state.lock_tone = 0;
	radar_hud_frame(world, 5, true, false, false, radar);
	CHECK(world.out.sound_emitters.empty(), "a dead word registers nothing");
	// The round's overlay reset leaves the word; the local class init zeroes it.
	state.lock_tone = 7;
	radar_reset(state);
	CHECK(state.lock_tone == 7, "HUD_ResetAllOverlayBuffers does not touch the word");
	PlayerSpawn spawn;
	const EntityHandle respawned = spawn_player(world, spawn);
	CHECK(respawned.valid() && state.lock_tone == 0, "the local player's class init zeroes it");
}

void test_whiz_radius_resolve() {
	opennova::lwf::File bank;
	opennova::lwf::Multi zip;
	zip.name = "SndZip";
	zip.target_id = 12;
	opennova::lwf::Multi move;
	move.name = "SndMove";
	move.target_id = 7;
	opennova::lwf::Multi huge;
	huge.name = "SndHuge";
	huge.target_id = 80;
	bank.multis = {zip, move, huge};
	opennova::audio::SoundSetIndex sets;
	sets.add_bank(0, bank);
	AmmoTable table;
	table.entries.resize(4);
	for (AmmoTableEntry &e : table.entries) e.valid = true;
	table.entries[0].impact_effects[1] = {"", "SndMove", true};
	table.entries[0].impact_effects[3] = {"", "SndZip", true};
	table.entries[1].impact_effects[1] = {"", "SndMove", true};
	table.entries[2].impact_effects[3] = {"", "SndHuge", true};
	table.entries[3].impact_effects[3] = {"", "SndNone", true};
	resolve_ammo_whiz_radii(table, &sets);
	CHECK(table.entries[0].whiz_radius_q16 == (12 << 16), "the zip row wins over the move row");
	CHECK(table.entries[1].whiz_radius_q16 == (7 << 16), "the move row alone sets it");
	CHECK(table.entries[2].whiz_radius_q16 == 3276800, "the radius caps at 50 units");
	CHECK(table.entries[3].whiz_radius_q16 == 0, "an unresolved set leaves 0");
	resolve_ammo_whiz_radii(table, nullptr);
	CHECK(table.entries[0].whiz_radius_q16 == 0, "no banks, no whiz");
}

} // namespace

int main() {
	test_add();
	test_update_sectors_and_ageing();
	test_kind255_counter_bump();
	test_missile_list_quirk();
	test_damage_kind();
	test_damage_entry_point();
	test_tracer_whiz();
	test_lock_note_and_hud_frame();
	test_zip_impact();
	test_wire_shooter_and_target();
	test_lock_tone();
	test_whiz_radius_resolve();
	if (g_failures != 0) {
		std::printf("radar_contacts: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("radar_contacts: OK\n");
	return 0;
}
