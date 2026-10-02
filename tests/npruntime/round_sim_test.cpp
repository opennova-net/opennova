// §5.60 — the authoritative round sim + damage + death routing. An accepted C2S 0x06
// spawns a live round SYNCHRONOUSLY with the ring append [orig: RoundData_AddRound
// @0x4fdb40 inline-calls RoundData_SpawnRound @0x4ec0d0]; Server_TickUpdate's world tick
// flies it [orig: Weapon_UpdateAllProjectiles @0x4ec020 -> Projectile_UpdatePhysics
// @0x4e9d70], the hit applies the KINETIC damage number [orig: Weapon_CalcImpactDamage
// @0x4ec920 — min(62*|vel|,1219) * weight_in_grains / 875, floored/capped by
// min/max_damage], clamped to remaining health [orig: @0x4e8064], and a health<=0 victim
// raises the death routing [orig: Entity_CheckAndProcessDeath @0x51b550]: S2C 0x13
// [u16 victim][i16 deathAnimStateId] to every non-host in-match connection, victim-only
// S2C 0x61 tick seed + 0x52 killer position, mask-0x80 S2C 0x1E kill-feed, and
// the conditional S2C 0x54
// downed/revive-window split; every victim receives the retail +360/+364
// post-death hold, early C2S 0x0E picks are dropped, and the listen host's local
// presentation fallback releases through the same deployment transaction at expiry
// [orig: GameEvent_PlayerDeath @0x516dd0; Server_ProcessClientRequestRespawn @0x519af0].
//
// Coverage: build_ammo_table + round_type resolve; fire -> one live round with the
// velocity/62 step; three body hits kill a 150-hp player (60/60/30 clamped); exact
// 0x13 -> victim-only 0x61 -> victim-only 0x52 -> 0x1E -> conditional 0x54
// route, including the 0x1E-only loopback leg; configured and recent-spawn
// hold branches; exact silent-drop/accept boundary; a dead loopback victim releases at
// spawn health without a second respawn implementation.

#include <runtime/world/ammo_table_build.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_idle_timers.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>

#include <runtime/replication/connection.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <formats/def/def.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/replication_model.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <runtime/terrain_query/height_field.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/weapon_scope_zero.h>
#include <runtime/world/world.h>

#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "conn_fixture.h"

using namespace opennova::def;

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;
using conn_fixture::make_seeded_conn;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

struct DeathAnimSource final : w::IRootMotionSource {
	bool has_clip(int, int state_id) const override {
		return state_id == w::anim_state::kIdle ||
		       state_id == w::anim_state::kIdle2 ||
		       (state_id >= w::anim_state::kDeathFire &&
		        state_id <= w::anim_state::kDeathBulletBase + 59);
	}
	int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
	bool advance(int, int state_id, int32_t &phase, w::RootMotionFrame &out) override {
		if (!has_clip(0, state_id)) return false;
		++phase;
		out = w::RootMotionFrame{};
		return true;
	}
};

w::PlayerSpawn player_spawn(uint16_t net_id, float x, float y, float z) {
	w::PlayerSpawn s;
	s.position = {x, y, z};
	s.net_id = net_id;
	return s;
}

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t(v >> 8));
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t((v >> 8) & 0xFF));
	b.push_back(uint8_t((v >> 16) & 0xFF));
	b.push_back(uint8_t((v >> 24) & 0xFF));
}

// The fixed 45-B C2S 0x06 body (§5.16 field order).
std::vector<uint8_t> fire_body(uint32_t tick, uint16_t shooter, uint8_t adm, int32_t px, int32_t py,
                               int32_t pz, int32_t dx, int32_t dy) {
	std::vector<uint8_t> b;
	put_u32(b, tick);
	put_u16(b, shooter);
	b.push_back(0x02); // primary fire
	b.push_back(adm);
	put_u32(b, uint32_t(px));
	put_u32(b, uint32_t(py));
	put_u32(b, uint32_t(pz));
	put_u32(b, uint32_t(dx));
	put_u32(b, uint32_t(dy));
	put_u16(b, 0xFFFF); // no claimed target
	put_u16(b, 513);
	b.push_back(0x07);
	b.push_back(0x0c);
	b.push_back(0x00);
	put_u16(b, 0);
	put_u16(b, 0);
	put_u16(b, 0);
	put_u16(b, 0);
	put_u16(b, 0);
	return b;
}

void dispatch_fire(inmatch::NapiNPConnection &conn, std::vector<inmatch::NapiNPConnection> &roster,
                   w::World &world, const std::vector<uint8_t> &body) {
	std::vector<ProtocolMessage> msgs;
	msgs.push_back(make_protocol_message(0x06, body));
	inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn, msgs, 100, roster, &world);
}

// Drain ALL staged S2C datagrams (0x0A noise included) once; pick tags from the result.
struct Drained {
	std::vector<std::vector<uint8_t>> raw;
	std::vector<std::vector<uint8_t>> tag(uint8_t want) const {
		std::vector<std::vector<uint8_t>> out;
		for (const auto &d : raw)
			if (!d.empty() && d[0] == want) out.emplace_back(d.begin() + 1, d.end());
		return out;
	}
};
Drained drain_all(ns::UdpSessionTransport &t) {
	Drained out;
	std::vector<uint8_t> raw;
	while (t.pop_outbound(raw)) out.raw.push_back(raw);
	return out;
}

Drained drain_all(ns::LoopbackChannel &t) {
	Drained out;
	ns::Datagram datagram;
	while (t.client_recv(datagram)) {
		std::vector<uint8_t> raw;
		raw.reserve(1 + datagram.body.size());
		raw.push_back(datagram.tag);
		raw.insert(raw.end(), datagram.body.begin(), datagram.body.end());
		out.raw.push_back(std::move(raw));
	}
	return out;
}

// A vehicle death takes only the scoring leg of the death route: the organic
// 0x13 transaction never runs for it, and its Flags/alive belong to its own
// death states (which also send its S2C 0x26), so a respawned vehicle remains
// damageable. [orig: Entity_CheckAndProcessDeath @0x51B550, called only from
// Entity_UpdateInfantryPlayerBody @0x4B4CEA, Entity_UpdateInfantryAI @0x4B9D4D
// and the console kill @0x4D29EC; the only 0x13 sends are GameEvent_PlayerDeath
// @0x516E8E and Entity_CheckAndProcessDeath @0x51B58F;
// AI_TransitionToDeath_GroundVehicle @0x467B58; Entity_RespawnVehicle @0x45FF40]
bool test_respawned_vehicle_takes_projectile_damage() {
	auto world_owner = std::make_unique<w::World>();
	w::World &world = *world_owner;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	world.rules.mp_session = true;
	world.rules.projectile_authority = true;

	w::Entity shooter_seed;
	shooter_seed.kind = w::EntityKind::Organic;
	const auto shooter = world.registry.spawn(0, shooter_seed);
	w::Entity vehicle_seed;
	vehicle_seed.kind = w::EntityKind::Item;
	vehicle_seed.has_item_def = true;
	vehicle_seed.item_type = 1;
	vehicle_seed.item_id = 1213;
	vehicle_seed.item_attrib = w::kItemAttribPlayerControl;
	vehicle_seed.position = {5.0f, 0.0f, 0.0f};
	vehicle_seed.health = vehicle_seed.health_max = 20;
	vehicle_seed.bound_radius = 1.0f;
	const auto vehicle = world.registry.spawn(1, vehicle_seed);
	// A vertical authored collision face at the vehicle origin, crossed by +X.
	w::CollisionWorld collision;
	w::CollisionModel model;
	model.face_vertices = {{0, -256, -256}, {0, 256, -256}, {0, 0, 256}};
	w::CollisionFace face;
	face.v[0] = 0; face.v[1] = 1; face.v[2] = 2;
	face.normal[0] = -16384;
	face.axis = 4;
	face.min[0] = face.max[0] = 0;
	face.min[1] = face.min[2] = -65536;
	face.max[1] = face.max[2] = 65536;
	model.faces.push_back(face);
	model.sections.resize(1);
	model.sections[0].face_count = 1;
	model.sections[0].face_vertex_count = 3;
	collision.assign_entity(vehicle, collision.add_model(std::move(model)));
	const int32_t position[3] = {5 * 65536, 0, 0};
	collision.publish_entity_section_matrices(
			vehicle, {w::collision_matrix_from_heading(0, position)});
	collision.build_tick_tables(world);
	world.collision = &collision;

	w::AmmoTableEntry ammo;
	ammo.name = "VEHICLE_RESPAWN_TEST";
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.weight_in_grains = 875;
	ammo.min_damage = ammo.max_damage = 20;
	ammo.max_age_ticks = 8;
	ammo.flags = w::kAmmoFlagNoGravity;
	world.tables.ammo.entries.push_back(ammo);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ns::UdpSessionTransport remote_wire(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			2, 1, &remote_wire, ns::TransportMode::Client, shooter, true));

	for (int life = 0; life < 2; ++life) {
		w::RoundSpawnParams params;
		params.owner = shooter;
		params.shooter_handle = shooter.packed;
		params.ammo_index = 0;
		if (!expect(world.round_sim.spawn(world, params) >= 0,
		            "a live projectile spawns for each vehicle life")) return false;
		world.round_sim.tick(world, nullptr);
		if (!expect(world.registry.get(vehicle)->health == 0 &&
		            world.round_sim.deaths.size() == 1,
		            life == 0 ? "a projectile destroys the initial vehicle"
		                      : "a projectile destroys the respawned vehicle")) return false;
		drain_all(remote_wire);
		inmatch::Server_TickUpdate(ctx);
		w::Entity &body = *world.registry.get(vehicle);
		if (!expect((body.flags & 2u) == 0,
		            "the organic death route never publishes the vehicle's Flags bit 1"))
			return false;
		const Drained sent = drain_all(remote_wire);
		bool vehicle_0x13 = false;
		for (const auto &death : sent.tag(s2c::ENTITY_DEATH))
			if (death.size() >= 2 && (death[0] | (death[1] << 8)) == vehicle.packed)
				vehicle_0x13 = true;
		bool vehicle_0x26 = false;
		for (const auto &kill : sent.tag(s2c::KILL_SYNC))
			if (kill.size() >= 2 && (kill[0] | (kill[1] << 8)) == vehicle.packed)
				vehicle_0x26 = true;
		if (!expect(!vehicle_0x13 && vehicle_0x26,
		            "a vehicle death fans its class S2C 0x26, never the organic 0x13"))
			return false;
		world.vehicles.respawn(body);
		if (!expect(body.alive && body.health == body.health_max,
		            "vehicle respawn restores its health")) return false;
	}
	return true;
}

bool run_death_feed_classifier_matrix() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 4);
	world.rules.mp_session = true;
	// The breath samples ride the World's every-32 idle legs, registered the
	// way the mission kernel does (between WAC and BMS).
	world.add_system(&world.server_idle_legs);

	w::Entity attacker_seed;
	attacker_seed.kind = w::EntityKind::Organic;
	attacker_seed.has_item_def = true;
	attacker_seed.item_type = 3;
	attacker_seed.flags = w::kEntityFlagPlayer;
	attacker_seed.alive = true;
	attacker_seed.health = 100;
	attacker_seed.team = 1;
	const w::EntityHandle attacker = world.registry.spawn(0, attacker_seed);

	w::Entity victim_seed = attacker_seed;
	victim_seed.team = 2;
	const w::EntityHandle victim = world.registry.spawn(0, victim_seed);
	if (!expect(attacker.valid() && victim.valid(),
	            "death classifier fixture players spawned"))
		return false;

	w::Entity flag_seed;
	flag_seed.kind = w::EntityKind::Item;
	flag_seed.has_item_def = true;
	flag_seed.item_id = 4091;
	const w::EntityHandle flag = world.registry.spawn(1, flag_seed);
	if (!expect(flag.valid(), "death classifier fixture flag spawned")) return false;

	world.tables.ammo.entries.resize(2);
	world.tables.ammo.entries[0].valid = true;
	world.tables.ammo.entries[0].name = "AMMO_TEST_RIFLE";
	world.tables.ammo.entries[1].valid = true;
	world.tables.ammo.entries[1].name = "AMMO_60MM_MORTAR";

	ns::LoopbackChannel host;
	ns::UdpSessionTransport victim_wire(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.respawn_timeout = 3;
	ctx.config.death_messages = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(
			1, 2, &host, ns::TransportMode::Loopback, attacker, true));
	roster.push_back(make_seeded_conn(
			2, 1, &victim_wire, ns::TransportMode::Client, victim, true));

	auto reset = [&]() {
		w::Entity *a = world.registry.get(attacker);
		w::Entity *v = world.registry.get(victim);
		a->flags = w::kEntityFlagPlayer;
		a->alive = true;
		a->health = 100;
		a->team = 1;
		v->flags = w::kEntityFlagPlayer;
		v->alive = true;
		v->health = 100;
		v->damage_state = 0;
		v->team = 2;
		v->cause_flags = 0;
		v->mounted_child = w::EntityHandle{};
		roster[1].link.respawn_pending = false;
		roster[1].link.respawn_hold_armed = false;
		roster[1].link.respawn_delay_seconds = 0;
		roster[1].link.spawn_target_hold_seconds = 0;
		roster[1].link.downed_revive_seconds = 0;
		roster[1].link.medic_request_active = false;
		roster[1].link.underwater_breath_samples = 0;
		ctx.config.death_messages = 1;
		world.env.water_z = 0;
		drain_all(host);
		drain_all(victim_wire);
	};
	auto death = [&](w::EntityHandle killer) {
		w::RoundDeath d;
		d.victim = victim;
		d.killer = killer;
		d.victim_handle = victim.packed;
		d.killer_handle = killer.packed;
		d.ammo_index = 0;
		return d;
	};
	// The classifier reads the victim's LIVE entity+44 cause word and clears the
	// bit it reports; the RoundDeath carries only the snapshot every producer
	// stamps. A hand-built death therefore latches the cause on the entity the
	// way Weapon_CalcImpactDamage / the projectile death edge do.
	// [orig: GameEvent_PlayerDeath @0x517180 / @0x517311 reads entity+44;
	//  the clears @0x5171ca / @0x5171e8 / @0x517206 / @0x517325]
	auto cause = [&](w::RoundDeath d, uint32_t bits) {
		d.event_flags = bits;
		world.registry.get(victim)->cause_flags = bits;
		return d;
	};
	auto route = [&](const w::RoundDeath &d) {
		world.round_sim.deaths.push_back(d);
		inmatch::Server_TickUpdate(ctx);
		const Drained host_out = drain_all(host);
		drain_all(victim_wire);
		return host_out.tag(0x1E);
	};
	auto expect_family = [&](uint8_t lo, uint8_t hi, w::RoundDeath d,
	                         const char *message) {
		const auto feed = route(d);
		return expect(feed.size() == 1 && feed[0].size() == 8 &&
		                      feed[0][0] >= lo && feed[0][0] <= hi,
		              message);
	};

	reset();
	if (!expect_family(4, 6, death(attacker), "ordinary kills use event types 4..6"))
		return false;
	reset();
	{
		const w::RoundDeath d = cause(death(attacker), 0x800u);
		if (!expect_family(10, 12, d, "critical/headshot kills use event types 10..12"))
			return false;
		if (!expect(world.registry.get(victim)->cause_flags == 0,
		            "the reported 0x800 bit is cleared on the entity [orig: @0x5171e8]"))
			return false;
	}
	reset();
	{
		const w::RoundDeath d = cause(death(attacker), 0x400u);
		if (!expect_family(13, 15, d, "knife kills use event types 13..15"))
			return false;
	}
	reset();
	{
		const w::RoundDeath d = cause(death(attacker), 0x100u);
		const uint32_t state_before = world.crt_rand.state;
		if (!expect_family(32, 32, d,
		                  "retail's narrowed CRT roll makes the same-bullet branch event 32"))
			return false;
		// The roll spends exactly one draw of the owned CRT stream
		// [orig: rand @0x51718A].
		if (!expect(world.crt_rand.state == state_before * 214013u + 2531011u,
		            "the 0x100 death branch consumes exactly one CRT draw"))
			return false;
	}
	reset();
	{
		world.registry.get(victim)->mounted_child = flag;
		const w::RoundDeath d = cause(death(attacker), 0x800u);
		if (!expect_family(24, 24, d,
		                  "killing the carrier of a retail flag type uses event 24"))
			return false;
	}
	reset();
	{
		w::RoundDeath d = death(attacker);
		d.ammo_index = 1;
		if (!expect_family(49, 49, d, "60 mm mortar kills use event 49")) return false;
	}
	reset();
	world.registry.get(attacker)->team = 2;
	if (!expect_family(7, 9, death(attacker), "same-team kills use event types 7..9"))
		return false;
	reset();
	{
		const auto feed = route(death(victim));
		if (!expect(feed.size() == 1 && feed[0].size() == 8 &&
		                    feed[0][0] >= 1 && feed[0][0] <= 3 &&
		                    feed[0][1] == uint8_t(victim.slot()) &&
		                    feed[0][2] == 0 && feed[0][3] == 0,
		            "suicide uses 1..3 and the retail self-death actor shape"))
			return false;
	}
	reset();
	world.registry.get(attacker)->flags = 0;
	{
		const auto feed = route(death(attacker));
		if (!expect(feed.size() == 1 && feed[0].size() == 8 &&
		                    feed[0][0] == 22 &&
		                    feed[0][1] == uint8_t(victim.slot()) &&
		                    feed[0][2] == 0 && feed[0][3] == 0,
		            "a non-player killer uses event 22 and the self-death actor shape"))
			return false;
	}
	reset();
	{
		const auto feed = route(death(w::EntityHandle{}));
		if (!expect(feed.size() == 1 && feed[0].size() == 8 &&
		                    feed[0][0] == 22 &&
		                    feed[0][1] == uint8_t(victim.slot()) &&
		                    feed[0][2] == 0 && feed[0][3] == 0,
		            "a killer-less ordinary death uses event 22 and the self-death actor shape"))
			return false;
	}
	reset();
	{
		const w::RoundDeath d = cause(death(w::EntityHandle{}), 0x200u);
		if (!expect_family(23, 23, d,
		                  "a killer-less crash cause at the breath boundary uses event 23"))
			return false;
	}
	reset();
	{
		roster[1].link.underwater_breath_samples = 80;
		if (!expect_family(22, 22, death(w::EntityHandle{}),
		                  "retail keeps sample 4*20 inside the ordinary no-killer family"))
			return false;
	}
	reset();
	{
		roster[1].link.underwater_breath_samples = 81;
		if (!expect_family(26, 26, death(w::EntityHandle{}),
		                  "sample 4*20+1 selects the drowned event 26"))
			return false;
	}
	reset();
	ctx.config.death_messages = 0;
	{
		const auto feed = route(death(attacker));
		if (!expect(feed.empty(), "deathmes=0 suppresses the S2C 0x1E feed"))
			return false;
	}

	// The same +460 counter is produced by the authority every 32 host ticks.
	// At the first sample beyond 4*20, the eye-under-water predicate kills the
	// player with the drown animation and routes the same no-killer transaction.
	// [orig: Server_UpdatePlayerBreathTimers @0x50D770, call gate @0x51D8C4;
	// GameEvent_PlayerDeath @0x5172EC..0x51734D]
	reset();
	world.logic_tick = 32;
	world.env.water_z = 1 << 16;
	w::Entity *submerged = world.registry.get(victim);
	submerged->position.z = 0.0f;
	submerged->eye_offset_z = 0;
	roster[1].link.underwater_breath_samples = 80;
	// An earlier hit's kill credit: the drowning clears it, so the death
	// reports no killer. [orig: Server_UpdatePlayerBreathTimers `mov
	// [ecx+178h],ebx` @0x50D800]
	submerged->last_attacker = attacker;
	inmatch::Server_TickUpdate(ctx);
	{
		const Drained host_out = drain_all(host);
		drain_all(victim_wire);
		const auto feed = host_out.tag(0x1E);
		if (!expect(feed.size() == 1 && feed[0].size() == 8 && feed[0][0] == 26 &&
		                    submerged->health <= 0 && submerged->death_anim_state == 175,
		            "the 32-tick authority breath sample produces a drowned player death"))
			return false;
		if (!expect(!submerged->last_attacker.valid(),
		            "the drowning clears the attacker slot"))
			return false;
	}

	// The timer pass holds itself while a pre-round countdown runs.
	// [orig: Server_UpdatePlayerBreathTimers `cmp g_PreRoundDelayTimer,ebx`
	//  @0x50D773]
	reset();
	world.env.water_z = 1 << 16;
	submerged = world.registry.get(victim);
	submerged->position.z = 0.0f;
	submerged->eye_offset_z = 0;
	roster[1].link.underwater_breath_samples = 5;
	world.preround_delay_seconds = 3;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
	if (!expect(roster[1].link.underwater_breath_samples == 5,
	            "a running pre-round countdown holds the breath sample"))
		return false;
	world.preround_delay_seconds = 0;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
	if (!expect(roster[1].link.underwater_breath_samples == 6,
	            "the sample counts once the countdown ends"))
		return false;

	// The limit is 4 * breathtime, the live named value: a script's
	// set(breathtime,4) drowns at the 17th sample, and the classifier's
	// event-26 test reads the same value.
	// [orig: Server_UpdatePlayerBreathTimers @0x50D7E6..0x50D7FB;
	//  GameEvent_PlayerDeath @0x5172F6..0x51730A]
	reset();
	world.logic_tick = 64;
	world.env.water_z = 1 << 16;
	world.script.wac_values.breathtime = 4;
	submerged = world.registry.get(victim);
	submerged->position.z = 0.0f;
	submerged->eye_offset_z = 0;
	roster[1].link.underwater_breath_samples = 16;
	inmatch::Server_TickUpdate(ctx);
	{
		const Drained host_out = drain_all(host);
		drain_all(victim_wire);
		const auto feed = host_out.tag(0x1E);
		if (!expect(feed.size() == 1 && feed[0].size() == 8 && feed[0][0] == 26 &&
		                    submerged->health <= 0,
		            "breathtime 4 drowns at sample 17"))
			return false;
	}
	world.script.wac_values.breathtime = 20;

	// The sample shares the WAC tick's admission, so the SP lose epilog holds
	// it like the script. [orig: Server_TickUpdate — the epilog test
	// @0x51D8B7..0x51D8BD precedes the Server_UpdatePlayerBreathTimers call
	// @0x51D8D7]
	reset();
	world.rules.mp_session = false;
	if (!expect(world.match.finish(2, world), "the SP round ends lost")) return false;
	world.round_end_tick = 0;
	world.logic_tick = 96;
	world.env.water_z = 1 << 16;
	submerged = world.registry.get(victim);
	submerged->position.z = 0.0f;
	submerged->eye_offset_z = 0;
	roster[1].link.underwater_breath_samples = 80;
	if (!expect(world.epilog_screen_active(), "the lose epilog screen is up")) return false;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(submerged->health > 0 && roster[1].link.underwater_breath_samples == 80,
	            "the epilog screen holds the breath sample"))
		return false;
	return true;
}

void deliver_all(const Drained &drained, ns::UdpSessionTransport &client) {
	for (const std::vector<uint8_t> &raw : drained.raw) client.push_inbound(raw);
}

bool test_retail_random_spread_vectors() {
	struct Vector {
		int32_t spread;
		uint32_t seed;
		int32_t vertical;
		bool alternate;
		int32_t yaw;
		int32_t pitch;
	};
	// Independent IDA/x87 oracle vectors. In particular, strict-f32
	// intermediates miss several of these by 1-4 BAM units.
	// [orig: Weapon_CalcRandomSpreadOffset @0x4E4120]
	constexpr std::array<Vector, 11> vectors{{
		{0x00000000, 0xFFFFFFFFu, 0x00010000, false, 0, 0},
		{0x00010000, 0x00000000u, 0, false, -7340450, 6771822},
		{0x00010000, 0x12345678u, 0, false, -4357031, 7278683},
		{0x00010000, 0x12345678u, 0x00008000, false, -4357031, 3639341},
		{0x00010000, 0x12345678u, 0, true, 9357827, -5601610},
		{0x00010000, 0x12345678u, 0x00008000, true, 9357827, 66155},
		{0x00028000, 0x80000000u, 0x00008000, false, -20294730, 3391413},
		{0x00028000, 0x80000000u, 0x00008000, true, 16461567, 1713528},
		{0x00004000, 0xFFFFFFFFu, 0, false, 1199147, 1599490},
		{0x00004000, 0xFFFFFFFFu, 0x00018000, true, 244338, -1924904},
		{static_cast<int32_t>(0xFFFF0000u), 0xDEADBEEFu, 0, false, 225733, -8847762},
	}};
	for (const Vector &v : vectors) {
		const w::RandomSpreadOffset got = w::weapon_calc_random_spread_offset(
				v.spread, v.seed, v.vertical, v.alternate);
		if (!expect(got.yaw_bam == v.yaw && got.pitch_bam == v.pitch,
		            "retail random-spread vector matches IDA/x87 oracle")) {
			std::fprintf(stderr,
			             "  spread=%08X seed=%08X vertical=%08X alt=%d got=(%d,%d) expected=(%d,%d)\n",
			             static_cast<uint32_t>(v.spread), v.seed,
			             static_cast<uint32_t>(v.vertical), v.alternate ? 1 : 0,
			             got.yaw_bam, got.pitch_bam, v.yaw, v.pitch);
			return false;
		}
	}

	struct ShotgunVector {
		int32_t pie_slice;
		uint16_t radial;
		uint16_t phase;
		int32_t yaw;
		int32_t pitch;
	};
	static constexpr ShotgunVector shotgun_vectors[] = {
		{0x01000000, 12695, 50206, 1614661, -15924875},
		{0x00100000, 1, 32785, -1048573, -1706},
		{0x00280000, 65518, 32478, -1133, 31},
		{0x00010000, 28826, 12516, 18303, 47072},
	};
	for (const ShotgunVector &v : shotgun_vectors) {
		const w::RandomSpreadOffset got =
				w::weapon_calc_shotgun_spread_offset(
						v.pie_slice, v.radial, v.phase);
		if (!expect(got.yaw_bam == v.yaw && got.pitch_bam == v.pitch,
		            "retail shotgun radial-spread vector matches IDA/x87 oracle"))
			return false;
	}
	return true;
}

bool test_spawn_spread_then_recoil() {
	w::World world;
	world.registry.configure_pool(0, 4);
	w::AiSystem &ai = world.ai;
	w::PlayerSpawn seed;
	seed.net_id = 41;
	seed.equipped_adm_index = 1;
	const w::EntityHandle shooter = w::spawn_player(world, seed);
	w::AiEntity *body = ai.for_handle(shooter);
	if (!expect(shooter.valid() && body != nullptr,
	            "spread/recoil integration player spawned"))
		return false;

	world.tables.ammo.entries.resize(1);
	w::AmmoTableEntry &ammo = world.tables.ammo.entries[0];
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 100;
	ammo.recoil[0] = 1;
	ammo.recoil[1] = 2;
	ammo.recoil[2] = 3;
	world.tables.weapons.entries.resize(2);
	w::WeaponTableEntry &weapon = world.tables.weapons.entries[1];
	weapon.valid = true;
	weapon.error_fp16[2] = 0x10000;

	w::RoundSpawnParams params;
	params.owner = shooter;
	params.shooter_handle = shooter.packed;
	params.ammo_index = 0;
	params.adm_index = 1;
	params.shot_seq = 0;
	const w::RandomSpreadOffset first_error =
			w::weapon_calc_random_spread_offset(0x10000, 0, 0, false);
	const int first = world.round_sim.spawn(world, params);
	if (!expect(first >= 0, "ordinary weapon round spawned")) return false;
	if (!expect(world.round_sim.rounds[static_cast<size_t>(first)].yaw_bam ==
	                    first_error.yaw_bam &&
	                    world.round_sim.rounds[static_cast<size_t>(first)].pitch_bam ==
	                    first_error.pitch_bam,
	            "current shot uses static ERROR before adding its recoil"))
		return false;
	if (!expect(body->inf.recoil_pitch == (3 << 18),
	            "standing ammo recoil is added after successful spawn"))
		return false;
	if (!expect(world.round_sim.fired.size() == 1 &&
	                    world.round_sim.fired[0].yaw_bam == 0 &&
	                    world.round_sim.fired[0].pitch_bam == 0,
	            "fire descriptor remains pre-random-spread"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	body->inf.scope_raised = true;
	world.registry.get(shooter)->flags |= w::kEntityFlagScopeRaised;
	const int scoped = world.round_sim.spawn(world, params);
	if (!expect(scoped >= 0 && body->inf.recoil_pitch == ((3 << 18) * 3 / 4),
	            "scope-raised recoil applies the exact binary32 0.75 scale"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	body->inf.scope_raised = false;
	world.registry.get(shooter)->flags &= ~w::kEntityFlagScopeRaised;
	world.round_sim.weapon_spread_enabled = false;
	const int gated = world.round_sim.spawn(world, params);
	if (!expect(gated >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(gated)].yaw_bam == 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(gated)].pitch_bam == 0 &&
	                    body->inf.recoil_pitch == (3 << 18),
	            "weapon rules gate suppresses ERROR but never recoil"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	world.round_sim.weapon_spread_enabled = true;
	ammo.flags = w::kAmmoFlagShotgun;
	ammo.spread_count = 1;
	ammo.kz_pieslice_bam = 0x01000000;
	world.throwables.fan_prng_state = 0x2B0749C1u;
	const int shotgun = world.round_sim.spawn(world, params);
	if (!expect(shotgun >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(shotgun)].yaw_bam ==
	                            1614661 &&
	                    world.round_sim.rounds[static_cast<size_t>(shotgun)].pitch_bam ==
	                            -15924875 &&
	                    body->inf.recoil_pitch == (3 << 18),
	            "shotgun uses its radial fan, skips weapon ERROR, and applies recoil"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	ammo.flags = w::kAmmoFlagDesignateTarget;
	const int designator = world.round_sim.spawn(world, params);
	if (!expect(designator < 0 && world.round_sim.active_count == 0 &&
	                    world.round_sim.fired.empty() && body->inf.recoil_pitch == 0,
	            "designator returns before projectile allocation and recoil"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	ammo.flags = 0;
	body->pos[2] = 1 << 16;
	world.env.water_z = 2 << 16;
	const int submerged = world.round_sim.spawn(world, params);
	if (!expect(submerged >= 0 && body->inf.recoil_pitch == (3 << 20),
	            "submerged source uses standing-row recoil at the underwater shift"))
		return false;

	// The classifier projects the per-tick EYE height, not the body origin:
	// retail compares Position.Z + CameraOffset.Z (entity+0x74) with the water
	// plane [orig: RoundData_SpawnRound @0x4ec342..0x4ec35a]. A body chest-deep
	// in water with its eyes above the plane takes ordinary recoil.
	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	world.registry.get(shooter)->eye_offset_z = 2 << 16; // eye at z=3 > water 2
	const int eyes_dry = world.round_sim.spawn(world, params);
	if (!expect(eyes_dry >= 0 && body->inf.recoil_pitch == (3 << 18),
	            "an above-water eye keeps the ordinary recoil shift"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	world.registry.get(shooter)->eye_offset_z = 0x4000; // eye at 1.25 < water 2
	const int eyes_wet = world.round_sim.spawn(world, params);
	if (!expect(eyes_wet >= 0 && body->inf.recoil_pitch == (3 << 20),
	            "a below-water eye keeps the underwater shift"))
		return false;

	return true;
}

struct DismembermentRig {
	w::World world;
	w::AiSystem &ai = world.ai;
	w::CollisionWorld collision;
	w::EntityHandle shooter;
	w::EntityHandle victim;

	// `on_ray_bone` is the ONE authored section the +X round crosses (the
	// hit-record bone hitRecord[14]); every lower section is unposed.
	explicit DismembermentRig(size_t pool_capacity, uint32_t victim_attrib = 0,
	                          int on_ray_bone = 3) {
		world.registry.configure_pool(0, pool_capacity);
		world.rules.projectile_authority = true;

		w::Entity shooter_seed;
		shooter_seed.kind = w::EntityKind::Organic;
		shooter_seed.position = {0.0f, 0.0f, 0.0f};
		shooter = world.registry.spawn(0, shooter_seed);

		w::Entity victim_seed;
		victim_seed.kind = w::EntityKind::Organic;
		victim_seed.item_id = 1419;
		victim_seed.has_item_def = true;
		victim_seed.item_type = 3;
		victim_seed.item_attrib = victim_attrib;
		// The items.def *_function class stamp the host's item-traits sweep
		// applies (ItemDef+356); the wire bridge admits rows by it, and the
		// clone inherits it through the seed copy.
		victim_seed.net_class_code =
				static_cast<uint8_t>(EntityClass::Infantry);
		victim_seed.position = {5.0f, 0.0f, 0.0f};
		victim_seed.spawn_position = victim_seed.position;
		victim_seed.yaw = 90; // engine heading 0; a +X round is rear quadrant 2
		victim_seed.health = 10;
		victim_seed.health_max = 20;
		// A pre-hidden bit OUTSIDE every bone mask, so the "already hidden
		// sections stay hidden on both halves" property stays observable.
		victim_seed.section_mask = 0x2000000u;
		victim = world.registry.spawn(0, victim_seed);

		const int body_index = ai.attach(victim);
		w::AiEntity *body = ai.at(body_index);
		body->pos[0] = 5 * 65536;
		body->heading = 0;
		body->health = 10;
		body->inf.active = true;
		body->inf.anim_state = w::anim_state::kIdle;
		body->inf.vel[0] = 100;
		body->inf.vel[1] = 200;
		body->inf.vel[2] = 300;

		w::CollisionModel model;
		model.sections.resize(static_cast<size_t>(on_ray_bone + 1));
		for (w::CollisionSection &section : model.sections)
			section.radius = -1;
		w::CollisionSection &on_ray = model.sections[static_cast<size_t>(on_ray_bone)];
		on_ray.authored_bounds = true;
		on_ray.min_x = on_ray.min_y = on_ray.min_z = -0x4000;
		on_ray.max_x = on_ray.max_y = on_ray.max_z = 0x4000;
		on_ray.radius = 0x4000;
		const int model_id = collision.add_model(std::move(model));
		collision.assign_entity(victim, model_id);
		const int32_t bone_at[3] = {5 * 65536, 0, 58982};
		std::vector<w::CollisionMatrix> pose(
				static_cast<size_t>(on_ray_bone + 1),
				w::collision_matrix_from_heading(0, bone_at));
		collision.publish_entity_section_matrices(victim, pose);
		collision.build_tick_tables(world);

		w::AmmoTableEntry ammo;
		ammo.name = "DISMEMBER_TEST";
		ammo.valid = true;
		ammo.velocity = 620; // 10 world units/tick
		ammo.weight_in_grains = 875;
		ammo.min_damage = 10;
		ammo.max_damage = 10;
		ammo.max_age_ticks = 8;
		ammo.flags = w::kAmmoFlagNoGravity;
		world.tables.ammo.entries.push_back(ammo);
	}

	void fire() {
		w::RoundSpawnParams params;
		params.owner = shooter;
		params.shooter_handle = shooter.packed;
		params.origin = {0.0f, 0.0f, 58982.0f / 65536.0f};
		params.ammo_index = 0;
		world.round_sim.spawn(world, params);
		world.round_sim.tick(world, nullptr, &collision);
	}

	const w::Entity *piece() const {
		const w::Entity *found = nullptr;
		world.registry.for_each([&](const w::Entity &entity) {
			if (entity.dismemberment_piece) found = &entity;
		});
		return found;
	}
};

// The local fire composite and the two spawn legs it drives: bit 7 selects the
// aimed ERROR row 3 at any stance, and the low six bits add the shooter AdmDef's
// zero elevation to the flying round only.
// [orig: Entity_FireWeaponAndSendPacket @0x42bdcb..0x42bdfb; Weapon_GetScopeZoomLevel
//  @0x422fc0; RoundData_SpawnRound @0x4ec155..0x4ec181 / @0x4ec3bf..0x4ec3d6;
//  Score_GetMultiplierValue @0x4fc440]
bool test_spawn_aimed_row_and_zero_elevation() {
	w::WeaponScopeZero zero;
	zero.step_metres = 100;
	const int32_t optic = static_cast<int32_t>(opennova::def::DEF_WEAPON_FLAG_SIGHTED);
	if (!expect(w::weapon_scope_zoom_step(zero, optic, -1, 250 << 16, false, 12) == 12 &&
	                    w::weapon_scope_zoom_step(zero, 0, -1, 250 << 16, true, 12) == 12,
	            "no optic fire or a plain weapon keeps the default zero step"))
		return false;
	if (!expect(w::weapon_scope_zoom_step(zero, optic, 3, 250 << 16, true, 12) == 0,
	            "a manual zero reports step 0"))
		return false;
	if (!expect(w::weapon_scope_zoom_step(zero, optic, -1, 249 << 16, true, 12) == 2 &&
	                    w::weapon_scope_zoom_step(zero, optic, -1, 250 << 16, true, 12) == 3 &&
	                    w::weapon_scope_zoom_step(zero, optic, -1, 9000 << 16, true, 12) == 39,
	            "the automatic zero rounds the rangefinder to the step and caps at 39"))
		return false;
	zero.step_metres = 0;
	if (!expect(w::weapon_scope_zoom_step(zero, optic, -1, 60 << 16, true, 12) == 2,
	            "an unset step spans 25 m"))
		return false;

	w::World world;
	world.registry.configure_pool(0, 4);
	w::PlayerSpawn seed;
	seed.net_id = 42;
	seed.equipped_adm_index = 1;
	const w::EntityHandle shooter = w::spawn_player(world, seed);
	w::AiEntity *body = world.ai.for_handle(shooter);
	if (!expect(shooter.valid() && body != nullptr, "aimed/zero player spawned")) return false;
	world.tables.ammo.entries.resize(1);
	w::AmmoTableEntry &ammo = world.tables.ammo.entries[0];
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 100;
	world.tables.weapons.entries.resize(2);
	w::WeaponTableEntry &weapon = world.tables.weapons.entries[1];
	weapon.valid = true;
	weapon.error_fp16[2] = 0x10000; // standing hip row
	weapon.error_fp16[3] = 0x1000;  // the aimed row
	weapon.action_fsm.scope_zero.elevation[5] = 0x123456;
	weapon.action_fsm.scope_zero.elevation[39] = 0x654321;

	w::RoundSpawnParams params;
	params.owner = shooter;
	params.shooter_handle = shooter.packed;
	params.ammo_index = 0;
	params.adm_index = 1;
	params.shot_seq = 7;
	params.subtype = 0x80;
	const w::RandomSpreadOffset aimed = w::weapon_calc_random_spread_offset(0x1000, 7, 0, false);
	const int aimed_round = world.round_sim.spawn(world, params);
	if (!expect(aimed_round >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(aimed_round)].yaw_bam ==
	                            aimed.yaw_bam &&
	                    world.round_sim.rounds[static_cast<size_t>(aimed_round)].pitch_bam ==
	                            aimed.pitch_bam,
	            "bit 7 takes ERROR row 3 even standing, and step 0 adds no elevation"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	world.round_sim.weapon_spread_enabled = false;
	params.subtype = 0x85;
	const int zeroed = world.round_sim.spawn(world, params);
	if (!expect(zeroed >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(zeroed)].pitch_bam == 0x123456 &&
	                    world.round_sim.fired.size() == 1 &&
	                    world.round_sim.fired[0].pitch_bam == 0,
	            "step 5 elevates the flying round; the fire record keeps the descriptor"))
		return false;

	world.round_sim.reset();
	params.subtype = 0x3F;
	const int clamped = world.round_sim.spawn(world, params);
	if (!expect(clamped >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(clamped)].pitch_bam == 0x654321,
	            "a step past 39 reads row 39"))
		return false;

	world.round_sim.reset();
	world.registry.get(shooter)->equipped_adm_index = 0;
	params.subtype = 5;
	const int no_adm = world.round_sim.spawn(world, params);
	if (!expect(no_adm >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(no_adm)].pitch_bam == 0,
	            "AdmDef 0 adds no elevation"))
		return false;

	// A decoded remote shooter without a local entity keys on its replica's adm.
	world.round_sim.reset();
	w::RoundSourceState replica;
	replica.equipped_adm_index = 1;
	params.owner = w::EntityHandle{};
	params.source_state = &replica;
	const int remote = world.round_sim.spawn(world, params, w::RoundConsequenceMode::VisualOnly);
	return expect(remote >= 0 &&
	                      world.round_sim.rounds[static_cast<size_t>(remote)].pitch_bam == 0x123456,
	              "a replica source elevates by its equipped AdmDef");
}

bool test_dismemberment_damage_path() {
	// Each mask is the hit bone's own bit (1 << bone) OR the witnessed case
	// addend [orig: @0x407601 + the switch @0x407608].
	constexpr uint32_t kBone3Mask = (1u << 3) | 0x1E670u;
	const std::array<uint32_t, 13> expected_masks{{
		(1u << 1) | 0x1E67Cu, (1u << 2) | 0x1E678u, (1u << 3) | 0x1E670u,
		(1u << 4) | 0x1E668u, (1u << 5) | 0x10200u, (1u << 6) | 0x08400u,
		(1u << 7) | 0x20800u, (1u << 8) | 0x41000u, (1u << 9) | 0x10000u,
		(1u << 10) | 0x08000u, (1u << 11) | 0x20000u, (1u << 12) | 0x40000u,
		(1u << 13) | 0x04000u,
	}};
	for (int bone = 1; bone <= 13; ++bone) {
		if (!expect(w::dismemberment_mask_for_bone(bone) ==
					expected_masks[static_cast<size_t>(bone - 1)],
				"bone-to-dismemberment mask table is exact"))
			return false;
	}
	if (!expect(w::dismemberment_mask_for_bone(0) == 0 &&
					w::dismemberment_mask_for_bone(-1) == 0,
			"bone zero and negatives never dismember"))
		return false;
	if (!expect(w::dismemberment_mask_for_bone(14) == (1u << 14),
			"bones past the table keep just their own section bit"))
		return false;

	{
		DismembermentRig rig(3);
		rig.fire();
		const w::Entity *victim = rig.world.registry.get(rig.victim);
		const w::Entity *piece = rig.piece();
		if (!expect(victim != nullptr && victim->health == 0,
				"bone hit kills the NPC victim"))
			return false;
		if (!expect(piece != nullptr && rig.world.registry.live_count() == 3,
				"lethal authored bone hit allocates one pool-0 corpse clone"))
			return false;
		if (!expect(victim->section_mask == (0x2000000u | kBone3Mask),
				"victim hides the selected cut sections"))
			return false;
		if (!expect(piece->section_mask == (0x2000000u | ~kBone3Mask),
				"clone keeps exactly the complementary cut sections"))
			return false;
		if (!expect(piece->health == 0 && !piece->alive &&
					piece->damage_state == -1 && piece->net_id == 0 &&
					piece->spawn_origin == w::kSpawnOriginNone,
				"corpse clone has no live gameplay or authored identity"))
			return false;

		const w::AiEntity *victim_body = rig.ai.for_handle(rig.victim);
		const w::AiEntity *piece_body = rig.ai.for_handle(piece->handle);
		// bodyRoll (entity+0x94 -> AiEntity::roll), rear quadrant 2 negative;
		// the clone memcpy-inherits the freshly written roll. The torso
		// overlay channel (+0x2DC) is not the death roll's store.
		// [orig: @0x407575; clone copy @0x4398dc]
		if (!expect(victim_body != nullptr && piece_body != nullptr &&
					victim_body->roll == -0x05B05B00 &&
					piece_body->roll == -0x05B05B00 &&
					victim_body->inf.torso_roll == 0 &&
					piece_body->inf.torso_roll == 0,
				"torso death takes the exact rear-quadrant body roll on both halves"))
			return false;
		// X/Y only [orig: @0x4076b7/@0x4076c9] — the vertical component stays.
		if (!expect(piece_body->inf.vel[0] == 100 + (10 * 65536 >> 8) &&
					piece_body->inf.vel[1] == 200 &&
					piece_body->inf.vel[2] == 300,
				"clone velocity adds the witnessed horizontal round impulse only"))
			return false;

		// The clone is an ordinary pool-0 slot on the wire: the 0x0A priority
		// walk has no dead/connection filter [orig: @0x50e6cb-0x50e6da] and
		// the join download serializes every pool-0 slot. Its net_id/name are
		// our cleared stand-ins (D-AI-9); no record carries the section mask.
		const auto snapshots = ns::snapshot_world(rig.world);
		const GameEntitySnapshot *piece_snapshot = nullptr;
		for (const auto &snapshot : snapshots) {
			if (snapshot.wire_handle == piece->handle.packed)
				piece_snapshot = &snapshot;
		}
		if (!expect(piece_snapshot != nullptr &&
					piece_snapshot->entity_class == EntityClass::Infantry,
				"corpse clone streams live compact snapshots like any NPC"))
			return false;
		const OrganicSpawnBatch batch =
				ns::build_pool0_organic_batch(rig.world);
		const OrganicSpawnRecord *piece_record = nullptr;
		for (const OrganicSpawnRecord &record : batch.records) {
			if (record.slot_id == piece->handle.packed) piece_record = &record;
		}
		if (!expect(piece_record != nullptr && piece_record->has_body &&
					piece_record->net_id == 0,
				"corpse clone joins the pool-0 spawn batch with a cleared net id"))
			return false;
	}

	{
		DismembermentRig rig(3, w::kItemAttribNoDismember);
		rig.fire();
		if (!expect(rig.piece() == nullptr &&
					rig.world.registry.get(rig.victim)->section_mask ==
							0x2000000u,
				"NoDismember kills without cloning or changing section masks"))
			return false;
	}

	{
		DismembermentRig rig(2); // shooter + victim fill the actor pool
		rig.fire();
		if (!expect(rig.piece() == nullptr &&
					rig.world.registry.get(rig.victim)->section_mask ==
							0x2000000u,
				"pool exhaustion leaves the original section mask intact"))
			return false;
	}
	return true;
}

// Every projectile hit on a person runs the class callback's presentation
// legs — the +0x2C0 selection, the torso-stack body roll, the +0x178 attacker
// stamp — lethal or not, damage 0 included; only the killing hit's lethal
// tail (dismemberment, the death record) stays behind the health test.
// [orig: OrganicClass_HandleEvent @0x407483 select, @0x40755e..0x407575
//  roll; Projectile_ProcessDamageOnTarget @0x4e81e7..0x4e81f9 lastAttacker,
//  callback(entity, 1, 0) @0x4e820e after the authority-gated subtraction]
bool test_person_hit_presentation_legs_run_on_every_hit() {
	struct Case {
		int16_t yaw;          // victim Entity yaw (mission deg); the round flies +X
		int32_t expected_roll;
		const char *label;
	};
	const Case cases[] = {
		{90, -0x05B05B00, "rear quadrant 2 tips a living body backward"},
		{-90, 0x05B05B00, "front quadrant 0 tips a living body forward"},
		{180, 0, "side quadrant 1 leaves the roll"},
		{0, 0, "side quadrant 3 leaves the roll"},
	};
	for (const Case &c : cases) {
		DismembermentRig rig(3);
		w::Entity *victim = rig.world.registry.get(rig.victim);
		w::AiEntity *body = rig.ai.for_handle(rig.victim);
		victim->health = 100;
		body->health = 100;
		victim->yaw = c.yaw;
		rig.fire();
		if (!expect(victim->health == 90 && rig.world.round_sim.deaths.empty() &&
		                    rig.piece() == nullptr,
		            "a non-lethal torso hit takes damage without a death or a clone"))
			return false;
		if (!expect(body->roll == c.expected_roll, c.label)) return false;
		const int quadrant = w::death_quadrant_from_round(
				w::bam_heading_from_mission_yaw_deg(c.yaw), 1.0f, 0.0f);
		if (!expect(victim->death_anim_state ==
		                    w::compute_death_anim_state(3, quadrant, w::death_cause::kBullet),
		            "a non-lethal hit stages the bone/quadrant bullet death in +0x2C0"))
			return false;
		if (!expect(victim->last_attacker == rig.shooter,
		            "a non-lethal hit stamps the shooter as the attacker"))
			return false;
	}
	{
		// A limb bone (>= 5) stages and stamps but never rolls.
		DismembermentRig rig(3, 0, 6);
		w::Entity *victim = rig.world.registry.get(rig.victim);
		w::AiEntity *body = rig.ai.for_handle(rig.victim);
		victim->health = 100;
		body->health = 100;
		rig.fire();
		if (!expect(victim->health == 90 && body->roll == 0 &&
		                    victim->death_anim_state ==
		                            w::compute_death_anim_state(6, 2, w::death_cause::kBullet) &&
		                    victim->last_attacker == rig.shooter,
		            "a limb hit (bone >= 5) stages without tipping the body"))
			return false;
	}
	{
		// A zero-damage authoritative hit (the +0x124 damage-state gate) still
		// runs the callback: the roll, the selection and the hit's alert leg
		// land; the health write and the kill do not.
		DismembermentRig rig(3);
		w::Entity *victim = rig.world.registry.get(rig.victim);
		w::AiEntity *body = rig.ai.for_handle(rig.victim);
		victim->health = 100;
		body->health = 100;
		victim->damage_state = 620;
		rig.fire();
		if (!expect(victim->health == 100 && rig.world.round_sim.hits.size() == 1 &&
		                    rig.world.round_sim.hits[0].damage == 0 &&
		                    rig.world.round_sim.deaths.empty(),
		            "a damage-state-gated hit applies no damage"))
			return false;
		if (!expect(body->roll == -0x05B05B00 &&
		                    victim->death_anim_state ==
		                            w::compute_death_anim_state(3, 2, w::death_cause::kBullet) &&
		                    victim->last_attacker == rig.shooter,
		            "a zero-damage hit still rolls, stages, and stamps the attacker"))
			return false;
	}
	{
		// A body already flagged dead takes nothing (the callback's first return).
		DismembermentRig rig(3);
		w::Entity *victim = rig.world.registry.get(rig.victim);
		w::AiEntity *body = rig.ai.for_handle(rig.victim);
		victim->health = 100;
		body->health = 100;
		victim->flags |= w::kEntityFlagDead;
		victim->engine_flags |= w::kEntityFlagDead;
		rig.fire();
		if (!expect(body->roll == 0 && victim->death_anim_state == 0,
		            "a dead body neither rolls nor re-stages [orig: @0x40772f]"))
			return false;
	}
	return true;
}

} // namespace

int main() {
	if (!test_respawned_vehicle_takes_projectile_damage()) return 1;
	if (!test_retail_random_spread_vectors()) return 1;
	if (!test_spawn_spread_then_recoil()) return 1;
	if (!test_spawn_aimed_row_and_zero_elevation()) return 1;
	if (!test_dismemberment_damage_path()) return 1;
	if (!test_person_hit_presentation_legs_run_on_every_hit()) return 1;
	if (!run_death_feed_classifier_matrix()) return 1;
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem &ai = world.ai;
	DeathAnimSource death_clips;
	ai.is_authority = true;
	ai.root_motion = &death_clips;
	world.tables.player.item_hp = 150; // items.def Player hp (D-NET-144)

	// Host player down-range past the victim on the same +X line; shooter at the origin.
	const w::EntityHandle ha = w::spawn_player(world, player_spawn(0xFFF0, 60.0f, 0.0f, 10.0f));
	const w::EntityHandle hb =
			w::spawn_remote_player(world, player_spawn(0xFFF1, 0.0f, 0.0f, 10.0f)); // shooter
	const w::EntityHandle hc =
			w::spawn_remote_player(world, player_spawn(0xFFF2, 30.0f, 0.0f, 10.0f)); // victim
	if (!expect(ha.valid() && hb.valid() && hc.valid(), "three players spawned")) return 1;
	// entity+0: a passing round parks this far past the victim.
	world.registry.get(hc)->bound_radius = 1.0f;
	if (!expect(world.registry.get(hc)->health == 150, "victim spawns at template hp 150"))
		return 1;
	w::AiEntity *shooter_body = ai.for_handle(hb);
	w::AiEntity *victim_body = ai.for_handle(hc);
	if (!expect(shooter_body != nullptr && victim_body != nullptr,
	            "remote player motor bodies spawned"))
		return 1;
	shooter_body->net_is_remote_peer = true;
	victim_body->net_is_remote_peer = true;

	// Armory: adm 5 = a rifle firing TEST_556. Ammo table via the real builder — entry 0
	// is the file-order null, entry 1 the live round (854 u/s, 62 grains, 3 s, C4 kz —
	// the real 5.56 shape).
	world.tables.weapons.entries.resize(8);
	{
		w::WeaponTableEntry &rifle = world.tables.weapons.entries[5];
		rifle.name = "WPN_TESTRIFLE";
		rifle.category = 3;
		rifle.rank = 2;
		rifle.clipsize = 30;
		rifle.round_type = "TEST_556";
		rifle.valid = true;
	}
	{
		DefAmmoFile file;
		std::memset(&file, 0, sizeof(file));
		static DefAmmoDef defs[2];
		std::memset(defs, 0, sizeof(defs));
		std::snprintf(defs[0].name, sizeof(defs[0].name), "AT_NULL");
		std::snprintf(defs[1].name, sizeof(defs[1].name), "TEST_556");
		defs[1].velocity = 854;
		defs[1].weight_in_grains = 62;
		defs[1].max_age_ticks = 186;
		defs[1].kztype = DEF_AMMO_KZ_C4;
		defs[1].drag_fp16 = 65536;
		defs[1].min_stable_velocity = 101;
		defs[1].tumble_error_fp16 = 655;
		defs[1].bullet_radius_fp16 = 182;
		// The per-surface impact rows (the real AMMO_AK47_556MM shape): the bake maps
		// tag names to the canonical table slots, keeps the FIRST duplicate, empties
		// 'none' columns, and discards the count column [orig: effects_table stage
		// @ 0x40a46a; count discard @ 0x40a587; AmmoDef_InitEffectsTable @ 0x409f20].
		static DefEffectTableEntry fx_rows[4];
		std::memset(fx_rows, 0, sizeof(fx_rows));
		std::snprintf(fx_rows[0].surface_type, sizeof(fx_rows[0].surface_type), "dirt");
		std::snprintf(fx_rows[0].hit_effect, sizeof(fx_rows[0].hit_effect), "Effect_AmHitDirt");
		std::snprintf(fx_rows[0].impact_sound, sizeof(fx_rows[0].impact_sound), "IMP_BULLET_DIRT");
		fx_rows[0].value = 15;
		std::snprintf(fx_rows[1].surface_type, sizeof(fx_rows[1].surface_type), "Player");
		std::snprintf(fx_rows[1].hit_effect, sizeof(fx_rows[1].hit_effect), "Effect_AmHitBody");
		std::snprintf(fx_rows[1].impact_sound, sizeof(fx_rows[1].impact_sound), "IMP_BULLET_PLAYER");
		fx_rows[1].value = 10;
		std::snprintf(fx_rows[2].surface_type, sizeof(fx_rows[2].surface_type), "zip");
		std::snprintf(fx_rows[2].hit_effect, sizeof(fx_rows[2].hit_effect), "none");
		std::snprintf(fx_rows[2].impact_sound, sizeof(fx_rows[2].impact_sound), "WSH_BULLET_BY");
		fx_rows[2].value = 10;
		std::snprintf(fx_rows[3].surface_type, sizeof(fx_rows[3].surface_type), "dirt"); // dup
		std::snprintf(fx_rows[3].hit_effect, sizeof(fx_rows[3].hit_effect), "Effect_WRONG");
		std::snprintf(fx_rows[3].impact_sound, sizeof(fx_rows[3].impact_sound), "none");
		defs[1].effects_table = fx_rows;
		defs[1].effects_table_count = 4;
		file.entries = defs;
		file.count = 2;
		world.tables.ammo = world::build_ammo_table(file);
		defs[1].effects_table = nullptr; // static rows; keep def_free-style cleanup moot
		defs[1].effects_table_count = 0;
		world::resolve_weapon_round_types(world.tables.weapons, world.tables.ammo);
	}
	if (!expect(world.tables.weapons.entries[5].ammo_index == 1, "round_type resolved to ammo 1"))
		return 1;
	{
		// The baked rows land in the canonical tag slots [orig: g_AmmoEffectTagTable
		// @ 0x813420 — player=2, dirt=5, zip=3].
		const w::AmmoTableEntry *a = world.tables.ammo.by_index(1);
		if (!expect(a != nullptr, "ammo 1 valid")) return 1;
		if (!expect(a->drag_fp16 == 65536 && a->min_stable_velocity == 101 &&
		                    a->tumble_error_fp16 == 655 && a->bullet_radius_fp16 == 182,
		            "exact fixed-point flight fields survive the ammo-table bake"))
			return 1;
		if (!expect(a->impact_effects[5].effect == "Effect_AmHitDirt" &&
		                    a->impact_effects[5].sound == "IMP_BULLET_DIRT",
		            "dirt row baked at tag 5 (first duplicate wins)"))
			return 1;
		if (!expect(a->impact_effects[2].effect == "Effect_AmHitBody",
		            "case-insensitive 'Player' tag baked at 2"))
			return 1;
		if (!expect(a->impact_effects[3].effect.empty() &&
		                    a->impact_effects[3].sound == "WSH_BULLET_BY",
		            "'none' effect column stays empty; the sound still bakes"))
			return 1;
		if (!expect(a->impact_effects[11].effect.empty() && a->impact_effects[11].sound.empty(),
		            "unauthored tags stay empty"))
			return 1;
		// This integration scenario isolates the existing fire/hit/death chain;
		// aerodynamic drag itself is pinned by projectile_combat_test's exact vectors.
		world.tables.ammo.entries[1].drag = 0.0f;
		world.tables.ammo.entries[1].drag_fp16 = 0;
	}

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_c(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport client_b_in(ns::UdpSessionTransport::Role::Client);
	ns::UdpSessionTransport client_c_in(ns::UdpSessionTransport::Role::Client);
	ns::ClientReplicaPipeline client_b_view;
	ns::ClientReplicaPipeline client_c_view;

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.respawn_timeout = 5;
	ctx.config.death_messages = 1;
	// This scenario is an MP session (three players over transports): stamp the
	// world-side flag too, or the SP-only round-outcome legs (kill tallies + the
	// death auto-lose in check_win_conditions) run and hold the respawn queue.
	world.rules.mp_session = true;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(1, 2, &loop, ns::TransportMode::Loopback, ha, true));
	roster.push_back(make_seeded_conn(3, 1, &udp_b, ns::TransportMode::Client, hb, true));
	roster.push_back(make_seeded_conn(4, 1, &udp_c, ns::TransportMode::Client, hc, true));
	// Distinct roster slot indexes (playerSlot+20): the chat sender byte and the
	// C2S 0x22 slot pull below address players by this index.
	for (size_t i = 0; i < roster.size(); ++i)
		roster[i].reply.player_slot = static_cast<uint8_t>(i);
	// These fixture players predate the measured 620-tick recent-spawn window.
	// Individual sub-cases below stamp a fresh deploy when they exercise that arm.
	for (inmatch::NapiNPConnection &conn : roster) {
		conn.link.last_deploy_tick = world.logic_tick - 620u;
		conn.link.last_deploy_tick_valid = true;
	}
	// Only the shooter is a Medic. Retail's send mask 0x580 is the conjunction of
	// slot state 6/7 (in-match) + same team byte + the charattr Medic bit — no
	// entity health/dead test [orig:
	// NapiNPServer_SendFiltered @0x4C87E0]. The victim disables OPTIONS_AUTOMEDIC
	// through C2S 0x03, so their death exercises the split branch: medics get state
	// zero while the victim alone gets the live 120-second revive window [orig:
	// NapiNPServerMsg_AutoMedicPreference @0x501BE0; GameEvent_PlayerDeath @0x516DD0].
	world.registry.get(ha)->player_class = 7;
	world.registry.get(hb)->player_class = 8;
	world.registry.get(hc)->player_class = 7;
	world.tables.class_attribute_flags[(8u - 1u) & 0xFu] |= w::MissionTables::kCharAttrMedic;
	// Both decoded views know the victim's roster binding before the live 0x54
	// edge arrives, matching the retail join-time 0x46 roster walk.
	PlayerReplicationState victim_rep;
	victim_rep.player_slot = 2;
	victim_rep.entity_handle = hc.packed;
	victim_rep.team = world.registry.get(hc)->team;
	const std::vector<uint8_t> victim_sync = encode_player_sync(
			victim_rep, kPlayerSyncHasTeamByte | kPlayerSyncHasDownedState);
	client_b_view.apply(s2c::PLAYER_SYNC, victim_sync);
	client_c_view.apply(s2c::PLAYER_SYNC, victim_sync);
	// ...and the join-time spawn stream's rows for the three players: a compact
	// 0x0A never creates one [orig: NapiNPClientMsg_0x00A @0x42FEC0 — the
	// pre-apply check @0x4307B1..0x4307FA queues C2S 0x0F on a miss].
	for (ns::ClientReplicaPipeline *view : {&client_b_view, &client_c_view}) {
		for (const w::EntityHandle h : {ha, hb, hc}) {
			view->state().upsert(h.packed).type_id =
					static_cast<uint16_t>(world.registry.get(h)->item_id);
		}
	}
	{
		// A short 0x03 body stores 0 = automatic rather than leaving the slot
		// untouched [orig: NapiNPServerMsg_AutoMedicPreference @0x501C16].
		roster[2].link.auto_medic_enabled = false;
		std::vector<ProtocolMessage> short_pref{
				make_protocol_message(0x03, {1, 0})};
		inmatch::dispatch_session_replies(
				ctx.config, roster[2], short_pref, world.logic_tick,
				roster, &world);
		if (!expect(roster[2].link.auto_medic_enabled,
		            "a short C2S 0x03 body selects automatic medic requests"))
			return 1;
		std::vector<ProtocolMessage> manual_medic{
				make_protocol_message(0x03, {1, 0, 0, 0})};
		inmatch::dispatch_session_replies(
				ctx.config, roster[2], manual_medic, world.logic_tick,
				roster, &world);
		if (!expect(!roster[2].link.auto_medic_enabled,
		            "a nonzero C2S 0x03 dword selects manual medic requests"))
			return 1;
	}
	auto advance_second_boundaries = [&](int count) {
		int crossed = 0;
		while (crossed < count) {
			inmatch::Server_TickUpdate(ctx);
			if (world.match.periodic_second()) ++crossed;
		}
	};


	// --- 1. Fire spawns one live round with the witnessed velocity step. ---
	// Wire yaw BAM 0 -> mission bearing 0 = +X: the 0x06 yaw IS the mission bearing
	// (v29 wire-validated; D-NET-153). Muzzle at torso height (hit spheres at z + 0.9).
	const int32_t muzzle_z = int32_t((10.0 + 0.9) * 65536.0);
	dispatch_fire(roster[1], roster, world, fire_body(roster[1].fire_tick_floor + 1u, hb.packed, 5, 0, 0, muzzle_z, 0, 0));
	if (!expect(world.round_sim.active_count == 1, "one live round after the fire")) return 1;
	{
		const w::LiveRound &r = world.round_sim.rounds[0];
		if (!expect(r.active && r.ammo_index == 1, "round bound to the resolved ammo"))
			return 1;
		const float speed = std::sqrt(r.vel.x * r.vel.x + r.vel.y * r.vel.y + r.vel.z * r.vel.z);
		if (!expect(std::fabs(speed - 854.0f / 62.0f) < 0.01f,
		            "round speed = ammo velocity / 62 per tick [orig: @0x4ec508]"))
			return 1;
		if (!expect(r.vel.x > 13.0f && std::fabs(r.vel.y) < 0.1f && std::fabs(r.vel.z) < 0.1f,
		            "wire yaw BAM 0 flies +X (bearing = wire yaw; D-NET-153)"))
			return 1;
	}

	// --- 2. Three ticks reach the victim at x=30; the hit applies the kinetic number:
	// min(62*13.77, 1219)=854 -> 854*62/875 = 60. ---
	for (int i = 0; i < 3; ++i) inmatch::Server_TickUpdate(ctx);
	if (!expect(world.round_sim.active_count == 1,
	            "the round passes the person (material 19) [orig: @0x4E99EE]"))
		return 1;
	if (!expect(world.registry.get(hc)->health == 90, "150 - 60 kinetic damage = 90")) return 1;
	// The hit queued ONE impact for the presenting host: tag 23 'flesh'. The person
	// leg splits on identity — the LOCAL player takes tag 2 'player', everyone else
	// takes tag 23 — and this authority has no local avatar, so the victim is a
	// non-local person. Direction = the normalized flight ray, position on the hit
	// sphere short of the victim at x=30.
	// [orig: Projectile_HandleTerrainImpact_0 @ 0x4e98f0 — local-player compare
	//  @0x4e9a55, push 2 @0x4e9aa1, push 17h @0x4e9ad7]
	if (!expect(world.round_sim.impacts.size() == 1, "one impact queued for the hit")) return 1;
	{
		const w::RoundImpact &imp = world.round_sim.impacts[0];
		if (!expect(imp.effect_tag == 23, "a non-local person hit selects tag 23 'flesh'"))
			return 1;
		if (!expect(imp.ammo_index == 1, "impact carries the round's ammo index")) return 1;
		if (!expect(std::fabs(imp.direction.x - 1.0f) < 0.01f, "impact direction = +X flight"))
			return 1;
		if (!expect(imp.position.x > 27.0f && imp.position.x < 30.5f,
		            "impact position lands at the victim's hit sphere"))
			return 1;
	}
	world.round_sim.impacts.clear(); // the presenter drain, stubbed
	// The round passed the victim and flies on toward the host down-range; the
	// later legs each fire their own rounds.
	world.round_sim.reset();

	// --- 2b. Terrain impact: a missed shot stops ON the surface with the dirt tag.
	// Flat synthetic heightfield (ground = 0 everywhere); the round flies down at
	// -45 deg from z=+5 on an empty lane (y=50, no entities). The impact-effect tag
	// is the no-surface-map default (type 1 + 4 = dirt) and the interpolated stop
	// sits on the plane, not a sub-step under it
	// [orig: Projectile_HandleTerrainImpact @ 0x4e9210 -> AmmoDef_ProcessImpactEffect
	//  @ 0x40a170; D-WPN-15 carries the remaining tag-selection gaps]. ---
	{
		std::vector<uint16_t> flat_hm(512 * 512, 0);
		std::vector<int> flat_grid(256, 1);
		opennova::terrain::TerrainHeightField field;
		field.heightmap = flat_hm.data();
		field.dim = 512;
		field.layout.sector_grid = flat_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;

		w::RoundSpawnParams params;
		params.origin = {0.0f, 50.0f, 5.0f};
		params.dir_yaw_bam = 0;                    // +X
		params.dir_pitch_bam = int32_t(0xE0000000); // -45 deg
		params.ammo_index = 1;
		if (!expect(world.round_sim.spawn(world, params) >= 0, "terrain-leg round spawned"))
			return 1;
		for (int i = 0; i < 4 && world.round_sim.active_count > 0; ++i)
			world.round_sim.tick(world, &field, nullptr);
		if (!expect(world.round_sim.active_count == 0, "terrain stopped the round")) return 1;
		if (!expect(world.round_sim.impacts.size() == 1, "one terrain impact queued")) return 1;
		const w::RoundImpact &imp = world.round_sim.impacts[0];
		if (!expect(imp.effect_tag == 1 + 4, "terrain hit takes the no-map dirt tag [orig: @ 0x4e8862]"))
			return 1;
		if (!expect(std::fabs(imp.position.z) < 0.02f,
		            "the interpolated stop sits ON the surface, not a sub-step under it"))
			return 1;
		if (!expect(imp.direction.x == 0.0f && imp.direction.y == 0.0f &&
		                imp.direction.z == 0.0f,
		            "a terrain stop carries NO orientation: the handler's descriptor has a "
		            "NULL record [orig: Projectile_HandleTerrainImpact @ 0x4e92c8 -> "
		            "CEffectWorld_SpawnEmitterAtPosition @ 0x5f6e52]"))
			return 1;
		world.round_sim.impacts.clear();
	}

	// --- The impact's sound plays where the impact is produced, through the
	// distance-attenuated play: past 30 units it waits (62 * dist / 330) >> 2
	// ticks in a pending slot, inside 30 units it is ready at once, and with
	// no listener (a dedicated host) nothing plays.
	// [orig: AmmoDef_ProcessImpactEffect @ 0x40a216 ->
	//  Sound_PlayWithDistanceAttenuation @ 0x528e40, the delay @ 0x528ef2] ---
	{
		std::vector<uint16_t> flat_hm(512 * 512, 0);
		std::vector<int> flat_grid(256, 1);
		opennova::terrain::TerrainHeightField field;
		field.heightmap = flat_hm.data();
		field.dim = 512;
		field.layout.sector_grid = flat_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;
		const std::string saved_sound = world.tables.ammo.entries[1].impact_effects[5].sound;
		world.tables.ammo.entries[1].impact_effects[5].sound = "IMP_TEST_DIRT";
		const auto land_round = [&]() -> w::Vec3 {
			w::RoundSpawnParams params;
			params.origin = {0.0f, 50.0f, 5.0f};
			params.dir_yaw_bam = 0;
			params.dir_pitch_bam = int32_t(0xE0000000);
			params.ammo_index = 1;
			world.round_sim.spawn(world, params);
			world.out.fire_sounds.clear(); // the spawn's own fire leg is not the subject
			(void)world.out.fire_sounds.drain();
			for (int i = 0; i < 4 && world.round_sim.active_count > 0; ++i)
				world.round_sim.tick(world, &field, nullptr);
			const w::Vec3 at = world.round_sim.impacts.empty() ? w::Vec3{}
					: world.round_sim.impacts[0].position;
			world.round_sim.impacts.clear();
			return at;
		};
		// Far: the listener 400 units off.
		const w::Vec3 far_listener{0.0f, 50.0f, 400.0f};
		world.out.fire_sounds.set_listener(far_listener);
		const w::Vec3 at = land_round();
		const double dx = double(at.x - far_listener.x), dy = double(at.y - far_listener.y),
				dz = double(at.z - far_listener.z);
		int32_t countdown = (62 * int32_t(std::sqrt(dx * dx + dy * dy + dz * dz)) / 330) >> 2;
		if (countdown == 0) countdown = 1;
		if (!expect(world.out.fire_sounds.drain().empty() &&
		                world.out.fire_sounds.pending_count() == 1,
		            "a far impact's sound waits in one pending slot"))
			return 1;
		for (int i = 0; i + 1 < countdown; ++i) world.out.fire_sounds.tick();
		if (!expect(world.out.fire_sounds.drain().empty(),
		            "the far impact stays silent until its travel delay runs out"))
			return 1;
		world.out.fire_sounds.tick();
		const auto far_ready = world.out.fire_sounds.drain();
		if (!expect(far_ready.size() == 1 && far_ready[0].set_name == "IMP_TEST_DIRT" &&
		                far_ready[0].pos.x == at.x && far_ready[0].pos.z == at.z,
		            "the far impact plays its surface row at the impact after the delay"))
			return 1;
		// Near: the listener 10 units over the impact.
		world.out.fire_sounds.set_listener(w::Vec3{at.x, at.y, at.z + 10.0f});
		(void)land_round();
		const auto near_ready = world.out.fire_sounds.drain();
		if (!expect(near_ready.size() == 1 && near_ready[0].set_name == "IMP_TEST_DIRT" &&
		                world.out.fire_sounds.pending_count() == 0,
		            "a near impact's sound is ready the tick it lands"))
			return 1;
		world.tables.ammo.entries[1].impact_effects[5].sound = saved_sound;
	}

	// The processed hit also writes the sticky SHOT relations (players carry
	// group 0, so only the single rows land; rows outside the retail < 0x80
	// guard are no-ops) [orig: Projectile_ProcessDamageOnTarget
	// @ 0x4e80ae..0x4e80ef; guard e.g. EntityMatrix_SetProximityBit @ 0x452b60].
	{
		const w::Entity *sh = world.registry.get(hb);
		const w::Entity *vic = world.registry.get(hc);
		const bool in_range = sh->net_id < 128 && vic->net_id < 128;
		if (!expect(world.script.relations.single_single(w::TriggerRelations::kShot,
		            sh->net_id, vic->net_id) == in_range,
		            "shot S->S written on processed damage iff rows pass the <0x80 guard"))
			return 1;
	}

	// --- 3. Two more hits kill: 90 -> 30 -> 0 (the last clamped to remaining health
	// [orig: @0x4e8064]); death routes the complete retail packet transaction. ---
	const Drained before_kill_b = drain_all(udp_b);
	deliver_all(before_kill_b, client_b_in);
	client_b_view.pump(client_b_in);
	drain_all(loop);
	const ns::ClientEntityState *remote_before = client_b_view.state().find(hc.packed);
	if (!expect(remote_before != nullptr,
	            "observer decoded the live remote victim before lethal damage"))
		return 1;
	const int32_t remote_before_x = remote_before->x;
	const int32_t remote_before_y = remote_before->y;
	const int32_t remote_before_z = remote_before->z;
	const uint8_t remote_before_anim = remote_before->anim_state_id;
	drain_all(udp_c);
	for (int shot = 0; shot < 2; ++shot) {
		dispatch_fire(roster[1], roster, world,
		              fire_body(roster[1].fire_tick_floor + 1u, hb.packed, 5, 0, 0, muzzle_z, 0, 0));
		for (int i = 0; i < 4; ++i) inmatch::Server_TickUpdate(ctx);
		world.round_sim.reset(); // it passed the victim toward the host down-range
	}
	if (!expect(world.registry.get(hc)->health == 0, "victim dead at 0 hp (clamped)")) return 1;
	// The 0x0A is a PRE-motor snapshot, so the death-family animation the motor
	// selected on the last tick above rides the NEXT tick's frame.
	// [orig: Game_ProcessMainFrame @0x5263F0 — the 0x0A of Server_TickUpdate
	//  @0x51D7E0 (@0x51E3D6..0x51E450) precedes the Entity_UpdateAllEntities call
	//  @0x52674B]
	inmatch::Server_TickUpdate(ctx);
	const Drained after_kill_b = drain_all(udp_b);
	const Drained after_kill_c = drain_all(udp_c);
	const Drained after_kill_host = drain_all(loop);
	{
		auto death_tags = [](const Drained &drained) {
			std::vector<uint8_t> tags;
			for (const std::vector<uint8_t> &raw : drained.raw) {
				if (raw.empty()) continue;
				if (raw[0] == 0x13 || raw[0] == 0x61 || raw[0] == 0x52 ||
						raw[0] == 0x1E || raw[0] == 0x54)
					tags.push_back(raw[0]);
			}
			return tags;
		};
		if (!expect(death_tags(after_kill_b) ==
		                    std::vector<uint8_t>({0x13, 0x1E, 0x54}),
		            "medic observer receives the retail death-tail order"))
			return 1;
		if (!expect(death_tags(after_kill_c) ==
		                    std::vector<uint8_t>({0x13, 0x61, 0x52, 0x1E, 0x54}),
		            "victim receives exact 0x13 -> 0x61 -> 0x52 -> 0x1E -> 0x54 order"))
			return 1;
		if (!expect(death_tags(after_kill_host) ==
		                    std::vector<uint8_t>({0x1E}),
		            "mask 0x80 includes the listen host for the kill feed only"))
			return 1;
	}
	deliver_all(after_kill_b, client_b_in);
	client_b_view.pump(client_b_in);
	deliver_all(after_kill_c, client_c_in);
	client_c_view.pump(client_c_in);
	const ns::ClientEntityState *remote_dead = client_b_view.state().find(hc.packed);
	if (!expect(remote_dead != nullptr,
	            "observer retained the remote victim through the death handoff"))
		return 1;
	if (!expect((remote_dead->state_flags & 0x02u) != 0u,
	            "observer decoded the remote victim's lethal/dead sample"))
		return 1;
	if (!expect(remote_before_anim < w::anim_state::kDeathFire &&
	                    remote_dead->anim_state_id >= w::anim_state::kDeathFire &&
	                    remote_dead->anim_state_id <=
	                            w::anim_state::kDeathBulletBase + 59,
	            "observer decoded the first alive-to-death-family animation edge"))
		return 1;
	if (!expect(remote_dead->x == remote_before_x &&
	                    remote_dead->y == remote_before_y &&
	                    remote_dead->z == remote_before_z,
	            "remote networked victim does not jump at lethal/death-animation handoff"))
		return 1;
	{
		auto notif_b = after_kill_b.tag(0x13);
		auto notif_c = after_kill_c.tag(0x13);
		if (!expect(notif_b.size() == 1 && notif_c.size() == 1,
		            "one S2C 0x13 death notify per client"))
			return 1;
		const std::vector<uint8_t> &n = notif_b[0];
		if (!expect(n.size() == 4, "0x13 body is 4 B [u16 victim][i16 deathAnimStateId]"))
			return 1;
		const uint16_t victim = uint16_t(n[0] | (n[1] << 8));
		const uint16_t death_anim_slot = uint16_t(n[2] | (n[3] << 8));
		// word1 is the victim's +0x2C0 death-anim slot as the sender sees it —
		// never the killer handle. The infantry death edge consumes and zeroes
		// that slot before retail's send, so an infantry death ships 0 (every
		// 0x13 in the retail capture carries 0) even though the bullet kill
		// staged a nonzero selection for the edge.
		// [orig: NetPacket_BuildDeathNotifyPayload @0x5036E0 (@0x503733); edge zero
		//  @0x4B9D38 -> Entity_CheckAndProcessDeath @0x4B9D4D]
		if (!expect(victim == hc.packed && death_anim_slot == 0,
		            "0x13 carries the victim handle + the post-edge zero death-anim slot"))
			return 1;
		if (!expect(death_anim_slot != hb.packed,
		            "0x13 word1 is not the killer's packed handle"))
			return 1;
	}
	{
		const auto seed_b = after_kill_b.tag(0x61);
		const auto seed_c = after_kill_c.tag(0x61);
		if (!expect(seed_b.empty() && seed_c.size() == 1,
		            "death-time S2C 0x61 is targeted only to the victim"))
			return 1;
		const std::vector<uint8_t> &seed = seed_c[0];
		if (!expect(seed.size() == 4, "0x61 body is one u32 tick seed")) return 1;
		const uint32_t value = uint32_t(seed[0]) | (uint32_t(seed[1]) << 8) |
		                       (uint32_t(seed[2]) << 16) |
		                       (uint32_t(seed[3]) << 24);
		// Death passes enable=0: the four-zero disarm that also clears the slot's
		// retained stamp. The deploy release re-arms with the fresh roll.
		// [orig: GameEvent_PlayerDeath @0x516EF4 ->
		//  Server_SendRandomSeedToPlayer @0x5101A0 enable==0 arm @0x510237]
		if (!expect(value == 0u && roster[2].tick_seed == 0u,
		            "death 0x61 is the four-zero disarm and clears the stamp"))
			return 1;
	}
	{
		auto feed_b = after_kill_b.tag(0x1E);
		if (!expect(feed_b.size() == 1, "one S2C 0x1E kill-feed event")) return 1;
		const std::vector<uint8_t> &f = feed_b[0];
		if (!expect(f.size() == 8, "0x1E body is 8 B (§5.26)")) return 1;
		if (!expect(f[0] >= 4 && f[0] <= 6,
		            "standard-kill event_type is the retail 4..6 presentation family"))
			return 1;
		if (!expect(f[1] == uint8_t(hb.packed & 0xFF) && f[2] == uint8_t(hc.packed & 0xFF),
		            "0x1E attacker/victim pool-0 index bytes"))
			return 1;
		const int16_t px = int16_t(f[4] | (f[5] << 8));
		const int16_t py = int16_t(f[6] | (f[7] << 8));
		if (!expect(px == 0 && py == 0,
		            "player-death 0x1E carries the retail zero position"))
			return 1;
	}
	{
		const auto death_pos_b = after_kill_b.tag(0x52);
		const auto death_pos_c = after_kill_c.tag(0x52);
		if (!expect(death_pos_b.empty() && death_pos_c.size() == 1,
		            "S2C 0x52 is targeted only to the victim"))
			return 1;
		const std::vector<uint8_t> &p = death_pos_c[0];
		if (!expect(p.size() == 12,
		            "0x52 body is [i32 killerX][i32 killerY][i32 killerZ]"))
			return 1;
		const int32_t x = static_cast<int32_t>(
				uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
				(uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
		const int32_t y = static_cast<int32_t>(
				uint32_t(p[4]) | (uint32_t(p[5]) << 8) |
				(uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24));
		const int32_t z = static_cast<int32_t>(
				uint32_t(p[8]) | (uint32_t(p[9]) << 8) |
				(uint32_t(p[10]) << 16) | (uint32_t(p[11]) << 24));
		if (!expect(x == 0 && y == 0 && z == w::to_fixed(10.0),
		            "0x52 carries the killer's authoritative fixed-point position"))
			return 1;
		const ns::ClientDeathCameraTarget &folded =
				client_c_view.state().death_camera;
		if (!expect(folded.known && folded.updates == 1 &&
		                    folded.x == x && folded.y == y && folded.z == z,
		            "the victim client folds 0x52 into its canonical camera target"))
			return 1;
	}
	{
		const auto marker_b = after_kill_b.tag(0x54);
		const auto marker_c = after_kill_c.tag(0x54);
		if (!expect(marker_b.size() == 1 && marker_c.size() == 1,
		            "manual-medic death sends one 0x54 to medics and one to the victim"))
			return 1;
		if (!expect(marker_b[0] == std::vector<uint8_t>({
		                    uint8_t(hc.packed & 0xFF), uint8_t(hc.packed >> 8), 0}),
		            "manual-medic branch hides the downed marker from eligible medics"))
			return 1;
		if (!expect(marker_c[0] == std::vector<uint8_t>({
		                    uint8_t(hc.packed & 0xFF), uint8_t(hc.packed >> 8), 120}),
		            "manual-medic victim receives the exact 120-second revive window"))
			return 1;
		if (!expect(client_b_view.state().roster[2].downed_revive_seconds == 0 &&
		                    client_c_view.state().roster[2].downed_revive_seconds == 120,
		            "decoded roster state preserves the manual-medic recipient split"))
			return 1;
	}

	// --- 3a. The manual-mode victim calls a medic (C2S 0x2E). The medic gets
	// the live window (0x54, no bit 7) + the chat line; the requester gets the
	// chat line back; a same-team non-medic (the host) gets neither; the
	// help call fans to alive players (the host's copy rides the local slot
	// route); the once-only latch then hides the 0x54 on a repeat while the
	// chat + sound repeat; later 0x46 folds bit 7; an alive requester and an
	// absent STRSRV_MEDREQ both no-op.
	// [orig: Server_BroadcastMedicRequest @0x515390; SoundProfile_FindByEntityAndType
	// @0x528180 type 1 -> "<prefix>_MEDIC_REQUEST"]
	{
		ctx.server_text.medic_request_format = "%s needs a medic!";
		auto send_medic_request = [&](inmatch::NapiNPConnection &requester) {
			inmatch::ServerDispatchInputs inputs;
			inputs.medic_request_format = &ctx.server_text.medic_request_format;
			MedicRequest request;
			request.entity_index = requester.link.owned_entity.packed & 0xFFFu;
			return inmatch::dispatch_session_replies(
					ctx.config, requester,
					{make_protocol_message(c2s::MEDIC_REQUEST,
							encode_medic_request(request))},
					world.logic_tick, roster, &world, inputs);
		};
		const std::vector<uint8_t> victim_window{
				uint8_t(hc.packed & 0xFF), uint8_t(hc.packed >> 8), 120};
		const std::vector<uint8_t> chat_line = encode_chat_broadcast(
				ChatBroadcast{2, roster[2].reply.player_slot,
						world.registry.get(hc)->name + " needs a medic!"});
		drain_all(udp_b);
		drain_all(udp_c);
		drain_all(loop);
		world.out.slot_sounds.clear();
		const std::vector<ProtocolMessage> own = send_medic_request(roster[2]);
		const Drained medic = drain_all(udp_b);
		const Drained host_side = drain_all(loop);
		if (!expect(medic.tag(0x54).size() == 1 && medic.tag(0x54)[0] == victim_window,
		            "the medic receives the live revive window without bit 7"))
			return 1;
		if (!expect(medic.tag(0x14).size() == 1 && medic.tag(0x14)[0] == chat_line,
		            "the medic receives STRSRV_MEDREQ as chat channel 2 from the victim's slot"))
			return 1;
		if (!expect(own.size() == 1 && own[0].tag == s2c::CHAT_BROADCAST &&
		                    own[0].payload == chat_line,
		            "the requester receives the chat line alone (mask 0x20), never a 0x54"))
			return 1;
		if (!expect(host_side.tag(0x54).empty() && host_side.tag(0x14).empty(),
		            "a same-team non-medic receives neither the window nor the chat"))
			return 1;
		if (!expect(medic.tag(0x34).size() == 1 &&
		                    medic.tag(0x34)[0][0] == 1 &&
		                    std::string(reinterpret_cast<const char *>(
		                            medic.tag(0x34)[0].data()) + 1) == "BM1_MEDIC_REQUEST",
		            "alive players receive the positioned MEDIC_REQUEST composite (0x34)"))
			return 1;
		if (!expect(host_side.tag(0x34).empty() && world.out.slot_sounds.size() == 1 &&
		                    std::string(world.out.slot_sounds[0].set_name) == "BM1_MEDIC_REQUEST" &&
		                    world.out.slot_sounds[0].source_handle == hc.packed,
		            "the listen host's own copy rides the local slot-sound route"))
			return 1;
		if (!expect(roster[2].link.medic_request_active, "the once-only latch closes"))
			return 1;
		if (!expect(drain_all(udp_c).tag(0x54).empty(),
		            "the requester's transport carries no 0x54 for its own call"))
			return 1;

		// A repeat call: chat + sound again, no 0x54 (the latch).
		world.out.slot_sounds.clear();
		const std::vector<ProtocolMessage> again = send_medic_request(roster[2]);
		const Drained medic_again = drain_all(udp_b);
		if (!expect(medic_again.tag(0x54).empty() &&
		                    medic_again.tag(0x14).size() == 1 &&
		                    medic_again.tag(0x34).size() == 1 &&
		                    again.size() == 1 && again[0].tag == s2c::CHAT_BROADCAST,
		            "a repeat call re-sends the chat and sound but the latch hides the 0x54"))
			return 1;
		// The latched state now folds bit 7 into every later 0x54 / 0x46.
		{
			PlayerDownedState latched;
			latched.entity_handle = hc.packed;
			latched.revive_seconds = uint8_t(roster[2].link.downed_revive_seconds);
			latched.medic_request_active = roster[2].link.medic_request_active;
			if (!expect(encode_player_downed_state(latched)[2] == uint8_t(120 | 0x80),
			            "a latched request folds bit 7 into the 0x54 state byte"))
				return 1;
			// A player-sync pull of the victim's slot with field 0x0008 folds the
			// same latch into 0x46 [orig: NetPacket_SerializePlayerSync0x46 @0x505E80].
			const std::vector<ProtocolMessage> sync = inmatch::dispatch_session_replies(
					ctx.config, roster[1],
					{make_protocol_message(c2s::PLAYER_SYNC_REQUEST,
							{roster[2].reply.player_slot, 0x08, 0x00})},
					world.logic_tick, roster, &world);
			PlayerSync folded;
			if (!expect(sync.size() == 1 && sync[0].tag == s2c::PLAYER_SYNC &&
			                    decode_player_sync(sync[0].payload.data(),
			                            sync[0].payload.size(), folded) &&
			                    folded.downed_state == uint8_t(120 | 0x80),
			            "a latched request folds bit 7 into the 0x46 downed-state field"))
				return 1;
		}

		// An alive requester: nothing at all.
		drain_all(udp_b);
		world.out.slot_sounds.clear();
		const std::vector<ProtocolMessage> alive_call = send_medic_request(roster[1]);
		if (!expect(alive_call.empty() && drain_all(udp_b).raw.empty() &&
		                    drain_all(udp_c).tag(0x14).empty() && world.out.slot_sounds.empty() &&
		                    !roster[1].link.medic_request_active,
		            "an alive requester's medic call is ignored (slot+368 == 0)"))
			return 1;

		// An absent STRSRV_MEDREQ: the whole handler no-ops.
		ctx.server_text.medic_request_format.clear();
		const std::vector<ProtocolMessage> silent = send_medic_request(roster[2]);
		if (!expect(silent.empty() && drain_all(udp_b).raw.empty() &&
		                    world.out.slot_sounds.empty(),
		            "a null STRSRV_MEDREQ lookup no-ops the medic call"))
			return 1;
		ctx.server_text.medic_request_format = "%s needs a medic!";
	}
	if (!expect(roster[2].link.respawn_delay_seconds == 5,
	            "death arms the configured +360 hold outside the recent-spawn window"))
		return 1;
	if (!expect(roster[2].link.downed_revive_seconds == 120,
	            "ordinary other-player death arms playerSlot+368 to 120 seconds"))
		return 1;
	if (!expect(roster[2].link.spawn_target_hold_seconds == 0,
	            "a mission without spawn zones clears the +364 target hold"))
		return 1;
	std::vector<ProtocolMessage> default_pick{
			make_protocol_message(0x0E, {0xFF, 0xFF})};
	auto request_client_respawn = [&]() {
		return inmatch::dispatch_session_replies(
				ctx.config, roster[2], default_pick, world.logic_tick,
				roster, &world);
	};
	if (!expect(request_client_respawn().empty() &&
	                    world.registry.get(hc)->health == 0,
	            "an early default 0x0E pick is silently dropped by +360"))
		return 1;
	advance_second_boundaries(4);
	if (!expect(roster[2].link.respawn_delay_seconds == 1 &&
	                    roster[2].link.downed_revive_seconds == 116 &&
	                    request_client_respawn().empty(),
	            "death and revive counters decrement at the same four 1 Hz boundaries"))
		return 1;
	advance_second_boundaries(1);
	if (!expect(roster[2].link.respawn_delay_seconds == 0 &&
	                    roster[2].link.downed_revive_seconds == 115 &&
	                    world.registry.get(hc)->health == 0,
	            "the fifth boundary expires the hold without auto-respawning a client"))
		return 1;
	const std::vector<ProtocolMessage> client_release = request_client_respawn();
	if (!expect(client_release.size() >= 2 &&
	                    world.registry.get(hc)->health == 150 &&
	                    roster[2].link.last_deploy_tick == world.logic_tick &&
	                    roster[2].link.downed_revive_seconds == 0,
	            "the first post-expiry pick runs the shared deployment release"))
		return 1;

	// --- 4. Kill the HOST player (loopback-owned): its fresh deployment selects
	// the exact three-second recent-spawn arm and the local presentation fallback
	// releases through the same deployment transaction. Retail corpses remain ballistic
	// colliders (the proximity walk does not skip Flags bit 1 / dead), so move
	// the already-verified client corpse off this unrelated line-of-fire fixture.
	// Retail checks the dead-state gate after geometric impact, so a corpse is
	// not transparent to later rounds. Move this completed victim off the firing
	// lane before the separate host-player kill scenario below; the focused
	// projectile_combat test pins corpse interception itself.
	world.registry.get(hc)->position.y = 20.0f;
	roster[0].link.last_deploy_tick = world.logic_tick;
	roster[0].link.last_deploy_tick_valid = true;
	drain_all(udp_b);
	for (int shot = 0; shot < 3; ++shot) {
		dispatch_fire(roster[1], roster, world,
		              fire_body(roster[1].fire_tick_floor + 1u, hb.packed, 5, 0, 0, muzzle_z, 0, 0));
		for (int i = 0; i < 6; ++i) inmatch::Server_TickUpdate(ctx);
	}
	if (!expect(world.registry.get(ha)->health == 0, "host player dead")) return 1;
	if (!expect(roster[0].link.respawn_delay_seconds == 3,
	            "a death within 620 ticks forces the retail three-second hold"))
		return 1;
	{
		const auto automatic_marker = drain_all(udp_b).tag(0x54);
		if (!expect(automatic_marker.size() == 1 &&
		                    automatic_marker[0] == std::vector<uint8_t>({
		                            uint8_t(ha.packed & 0xFF), uint8_t(ha.packed >> 8), 120}),
		            "auto-medic death exposes the 120-second revive window to medics"))
			return 1;
	}
	// --- 4a. The loopback host's own downed player runs the same 0x2E handler:
	// automatic preference, so no 0x54, but the chat line reaches the host's
	// same-team medic (hb) and comes back to the host, and the help call fans
	// to alive players (the host is dead: the mask-128 alive filter drops its
	// own local copy).
	// [orig: Server_BroadcastMedicRequest @0x515390 — no loopback early-out]
	{
		inmatch::ServerDispatchInputs inputs;
		inputs.medic_request_format = &ctx.server_text.medic_request_format;
		drain_all(udp_b);
		drain_all(loop);
		world.out.slot_sounds.clear();
		const std::vector<ProtocolMessage> host_call = inmatch::dispatch_session_replies(
				ctx.config, roster[0],
				{make_protocol_message(c2s::MEDIC_REQUEST,
						encode_medic_request(MedicRequest{}))},
				world.logic_tick, roster, &world, inputs);
		const std::vector<uint8_t> host_line = encode_chat_broadcast(
				ChatBroadcast{2, roster[0].reply.player_slot,
						world.registry.get(ha)->name + " needs a medic!"});
		if (!expect(host_call.size() == 1 && host_call[0].tag == s2c::CHAT_BROADCAST &&
		                    host_call[0].payload == host_line,
		            "the loopback host's own call returns its chat line"))
			return 1;
		const Drained medic_side = drain_all(udp_b);
		if (!expect(medic_side.tag(0x54).empty() && medic_side.tag(0x14).size() == 1 &&
		                    medic_side.tag(0x14)[0] == host_line &&
		                    medic_side.tag(0x34).size() == 1,
		            "the host's call reaches its medic as chat + help call, with no 0x54 in automatic mode"))
			return 1;
		if (!expect(world.out.slot_sounds.empty() && roster[0].link.medic_request_active,
		            "a dead host gets no local copy (mask 128) and latches its request"))
			return 1;
	}
	{
		// Displace the corpse to prove the release snaps back [orig: the D-NET-66
		// death/respawn teleport]. The listen host's own player is MOTOR-simulated, and
		// the motor is the WRITER of the Entity/AiEntity pose pair — finish_infantry_tick
		// mirrors AiEntity.pos into Entity.position every tick. Displacing BOTH stores is
		// what makes this falsifiable: a respawn that writes only the registry Entity is
		// reverted on the next tick and the player is left standing in its own corpse's
		// spot at full health, which is indistinguishable from "I cannot respawn".
		w::Entity *host = world.registry.get(ha);
		host->position.x = 12.0f;
		w::AiEntity *host_ae = ai.for_handle(ha);
		if (!expect(host_ae != nullptr && host_ae->inf.active,
		            "the host player is motor-simulated")) return 1;
		host_ae->pos[0] = w::to_fixed(12.0);
		host_ae->inf.stance = w::InfantryState::Stance::kProne;
		advance_second_boundaries(2);
		if (!expect(roster[0].link.respawn_delay_seconds == 1 && host->health == 0,
		            "local fallback remains held through two second boundaries"))
			return 1;
		advance_second_boundaries(1);
		if (!expect(roster[0].link.respawn_delay_seconds == 0 && host->health == 0,
		            "expiry and deployment remain distinct retail phases"))
			return 1;
		inmatch::Server_TickUpdate(ctx);
		host = world.registry.get(ha);
		if (!expect(host->health == 150, "respawn restores template health")) return 1;
		if (!expect(std::fabs(host->position.x - 60.0f) < 0.01f,
		            "respawn snaps to the spawn point"))
			return 1;
		host_ae = ai.for_handle(ha);
		if (!expect(host_ae != nullptr, "the respawned host player kept its motor entity"))
			return 1;
		if (!expect(host_ae->pos[0] == w::to_fixed(60.0),
		            "respawn snaps the MOTOR store too, so the mirror cannot revert it"))
			return 1;
		if (!expect(host_ae->health == 150, "the motor health store respawns with it"))
			return 1;
		if (!expect(host_ae->inf.stance == w::InfantryState::Stance::kStand,
		            "the respawned body stands up out of the death pose"))
			return 1;
	}

	// --- 5. The 0x0E deploy of a RESPAWN-PENDING joiner emits the DEPLOY-RELEASE bundle
	// (0x5A + 0x61 + optional 0x1E) and clears the pending/hidden pair — the client's 0x5A
	// apply resets its dword_81474C wait-gate (set by the pick) and resumes the C2S 0x0C
	// uplink [orig: Server_ProcessPlayerDeath deploy tail: Server_SendWeaponSlotListToPlayer
	// @0x502550 + Server_SendRandomSeedToPlayer @0x5101a0; client un-latch §5.30
	// @0x4290E0; golden deploy frame 240018 = 0x5A + 0x61 + 0x1E one datagram; the v32
	// rubber-band]. (D-NET-156 tail) ---
	{
		const w::EntityHandle jb = w::spawn_remote_player(world, player_spawn(0xFFF3, 5, 5, 0));
		if (!expect(jb.valid(), "deploy-test joiner spawned")) return 1;
		inmatch::NapiNPConnection conn =
				make_seeded_conn(7, 1, &udp_b, ns::TransportMode::Client, jb, /*spawned=*/true);
		conn.link.respawn_pending = true;
		w::Entity *je = world.registry.get(jb);
		je->flags |= 1u; // the join-time hidden bit rides with pending
		conn.reply.last_loadout_reply = {8, 2, 255, 0, 0, 0xFF}; // a granted 0x5A body

		// +364 gates only a pick that resolves to a real target. A Default
		// Spawn pick bypasses it, then clears both counters in the release.
		// [orig: Server_ProcessClientRequestRespawn @0x519c67/@0x519cf2]
		world.registry.configure_pool(2, 8);
		w::Entity spawn_zone;
		spawn_zone.kind = w::EntityKind::Item;
		spawn_zone.has_item_def = true;
		spawn_zone.is_spawn_point = true;
		spawn_zone.alive = true;
		spawn_zone.team = je->team;
		spawn_zone.position = {25.0f, 25.0f, 0.0f};
		const w::EntityHandle zone = world.registry.spawn(2, spawn_zone);
		if (!expect(zone.valid(), "target-hold spawn zone created")) return 1;
		conn.link.spawn_target_hold_seconds = 2;
		std::vector<ProtocolMessage> target_pick;
		target_pick.push_back(make_protocol_message(
				0x0E, {static_cast<uint8_t>(zone.packed),
				       static_cast<uint8_t>(zone.packed >> 8)}));
		if (!expect(inmatch::dispatch_session_replies(
		                    ctx.config, conn, target_pick, 99, roster, &world).empty() &&
		                    conn.link.respawn_pending,
		            "+364 silently rejects a real spawn-target pick"))
			return 1;

		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(0x0E, {0xFF, 0xFF})); // param-0 pick (base deploy)
		std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				inmatch::GameConfig{}, conn, msgs, 100, roster, &world);

		bool saw_5a = false, saw_61 = false;
		for (const ProtocolMessage &m : replies) {
			if (m.tag == 0x5A) {
				saw_5a = true;
				if (!expect(m.payload == conn.reply.last_loadout_reply,
				            "deploy 0x5A re-sends the GRANTED loadout body"))
					return 1;
			}
			if (m.tag == 0x61) {
				saw_61 = true;
				// The deploy release re-rolls this player's TICK SEED — per connection,
				// never the session constant (a shared value would re-seed every client's
				// clock to the same tick on every deploy). The witnessed shape is
				// ((rand() & 0xFE) + 1) << 16: the low word is always zero, the seed is
				// never zero, it never exceeds 0xFF0000, and bit 16 is always set.
				// [orig: Server_SendRandomSeedToPlayer @0x5101a0 value @0x5101d4]
				const uint32_t seed = static_cast<uint32_t>(m.payload[0]) |
						(static_cast<uint32_t>(m.payload[1]) << 8) |
						(static_cast<uint32_t>(m.payload[2]) << 16) |
						(static_cast<uint32_t>(m.payload[3]) << 24);
				if (!expect(m.payload.size() == 4 && (seed & 0xFFFFu) == 0 && seed != 0 &&
				                    seed <= 0xFF0000u && (seed & 0x10000u) != 0 &&
				                    seed == conn.tick_seed,
				            "deploy 0x61 carries this connection's re-rolled tick seed"))
					return 1;
			}
		}
		if (!expect(saw_5a, "deploy release emits the 0x5A un-latcher")) return 1;
		if (!expect(saw_61, "deploy release emits the 0x61 seed")) return 1;
		if (!expect(!conn.link.respawn_pending, "deploy clears respawn_pending")) return 1;
		je = world.registry.get(jb);
		if (!expect((je->flags & 1u) == 0, "deploy clears the hidden bit")) return 1;
		if (!expect(je->health > 0, "deploy restores health")) return 1;

		// An alive DEPLOYED player's 0x0E is a no-op (the dead-or-pending gate @0x519cc7):
		// no bundle, no reposition.
		std::vector<ProtocolMessage> again = inmatch::dispatch_session_replies(
				inmatch::GameConfig{}, conn, msgs, 101, roster, &world);
		for (const ProtocolMessage &m : again)
			if (!expect(m.tag != 0x5A && m.tag != 0x61,
			            "alive deployed 0x0E draws no release bundle"))
				return 1;
	}

	// --- 6. The tracer decision [orig: RoundData_SpawnRound @0x4ec184-0x4ec1e5]:
	// every tracer_rate-th round per shooter is a tracer (the counter wraps at the
	// rate; tracer on wrap), rate 0 = never, FORCETRACER (flags 0x8000) = every
	// round; team is stamped from the shooter [orig: round+0x162 @0x4ec705]. Every
	// spawn also records a FireEvent for the host present drain (§17.4).
	{
		world.tables.ammo.entries[1].tracer_rate = 3;
		world.tables.ammo.entries[1].tracer_item_friendly = 1883;
		w::Entity *shooter = world.registry.get(hb);
		shooter->tracer_shot_counter = 0;
		world.round_sim.fired.clear();
		w::RoundSpawnParams rp;
		rp.owner = hb;
		rp.shooter_handle = hb.packed;
		rp.origin = {0.0f, 0.0f, 30.0f};
		rp.ammo_index = 1;
		for (int shot = 1; shot <= 6; ++shot) {
			const int slot = world.round_sim.spawn(world, rp);
			if (!expect(slot >= 0, "tracer-cadence round spawned")) return 1;
			const bool want = (shot % 3) == 0; // counter wrap = every 3rd shot
			if (!expect(world.round_sim.rounds[size_t(slot)].tracer == want,
			            "tracer cadence: tracer exactly on the counter wrap"))
				return 1;
			const w::LiveRound &round =
					world.round_sim.rounds[size_t(slot)];
			if (!expect(round.item_type_id == 1883 &&
			                    w::round_visible_item_id(round) ==
			                            (want ? 1883 : 0),
			            "TrcrID stays class-bound while only cadence tracer shots show its model"))
				return 1;
			if (!expect(world.round_sim.rounds[size_t(slot)].team == shooter->team,
			            "tracer round carries the shooter team"))
				return 1;
		}
		world.tables.ammo.entries[1].tracer_rate = 0;
		int s0 = world.round_sim.spawn(world, rp);
		if (!expect(s0 >= 0 && !world.round_sim.rounds[size_t(s0)].tracer,
		            "tracer_rate 0 -> never a tracer"))
			return 1;
		if (!expect(
		            world.round_sim.rounds[size_t(s0)].item_type_id == 1883 &&
		                    w::round_visible_item_id(
		                            world.round_sim.rounds[size_t(s0)]) == 0,
		            "non-tracer cadence keeps the TrcrID bind but clears its visible model"))
			return 1;
		world.tables.ammo.entries[1].flags |= 0x8000u; // forcetracer
		int s1 = world.round_sim.spawn(world, rp);
		if (!expect(s1 >= 0 && world.round_sim.rounds[size_t(s1)].tracer,
		            "FORCETRACER overrides rate 0"))
			return 1;
		if (!expect(w::round_visible_item_id(
		                    world.round_sim.rounds[size_t(s1)]) == 1883,
		            "FORCETRACER keeps the selected TrcrID model visible"))
			return 1;
		world.tables.ammo.entries[1].flags &= ~0x8000u;

		w::RoundSim lifetime_sim;
		const int first_lifetime_slot = lifetime_sim.spawn(world, rp);
		if (!expect(first_lifetime_slot >= 0,
		            "presentation-lifetime probe spawned its first round"))
			return 1;
		const uint64_t first_generation =
				lifetime_sim.rounds[size_t(first_lifetime_slot)]
						.presentation_generation;
		lifetime_sim.rounds[size_t(first_lifetime_slot)].active = false;
		--lifetime_sim.active_count;
		const int second_lifetime_slot = lifetime_sim.spawn(world, rp);
		if (!expect(second_lifetime_slot == first_lifetime_slot &&
		                    lifetime_sim.rounds[size_t(second_lifetime_slot)]
		                                    .presentation_generation !=
		                            first_generation,
		            "same-slot reuse receives a new presentation lifetime identity"))
			return 1;

		world.tables.ammo.entries[1].tracer_item_friendly = 0;
		if (!expect(world.round_sim.fired.size() == 8 &&
		                    world.round_sim.fired.back().ammo_index == 1 &&
		                    world.round_sim.fired.back().shooter_handle == hb.packed,
		            "every spawn records a FireEvent for the present drain"))
			return 1;
		world.round_sim.fired.clear();

		// The cadence byte belongs to the FIRING SLOT (MountSlot+0x80): a
		// producer that supplies its slot byte advances THAT byte, two slots
		// keep independent phases across an interleaved switch, and the
		// owner-entity stand-in byte stays untouched [orig: RoundData_SpawnRound
		// @0x4ec199..0x4ec1bb on the passed weaponSlot].
		world.tables.ammo.entries[1].tracer_rate = 3;
		shooter->tracer_shot_counter = 0;
		uint8_t slot_a = 0;
		uint8_t slot_b = 0;
		w::RoundSpawnParams rp_slot = rp;
		rp_slot.tracer_counter = &slot_a;
		const int a1 = world.round_sim.spawn(world, rp_slot);
		const int a2 = world.round_sim.spawn(world, rp_slot);
		rp_slot.tracer_counter = &slot_b;
		const int b1 = world.round_sim.spawn(world, rp_slot);
		rp_slot.tracer_counter = &slot_a;
		const int a3 = world.round_sim.spawn(world, rp_slot);
		if (!expect(a1 >= 0 && a2 >= 0 && b1 >= 0 && a3 >= 0,
		            "per-slot cadence rounds spawned"))
			return 1;
		if (!expect(!world.round_sim.rounds[size_t(a1)].tracer &&
		                    !world.round_sim.rounds[size_t(a2)].tracer &&
		                    world.round_sim.rounds[size_t(a3)].tracer,
		            "slot A's third round is the tracer despite the interleaved "
		            "slot-B shot"))
			return 1;
		if (!expect(!world.round_sim.rounds[size_t(b1)].tracer && slot_b == 1,
		            "slot B keeps its own phase"))
			return 1;
		if (!expect(shooter->tracer_shot_counter == 0,
		            "the owner-entity stand-in byte is untouched while a slot "
		            "byte is supplied"))
			return 1;
		world.round_sim.fired.clear();
	}

	// --- 7. The tracer trail channels [orig: g_TracerEmitterPool @ 0x2BF5270 — alloc
	// at spawn with the friendly/enemy style vs the local team @ 0x4ec740, one
	// pre-move point per tick (Projectile_UpdatePhysics @ 0x4ea97a), ring-capped at
	// the style count, death append + drain (Projectile_ReleaseEffects @ 0x4e8280 ->
	// CEffectEmitterPool_Tick @ 0x5db830: cap-length grace, then one pop per tick)].
	{
		auto &sim = world.round_sim;
		auto &ammo1 = world.tables.ammo.entries[1];
		ammo1.tracer_rate = 1; // every round a tracer
		ammo1.tracer_type_friendly = 1;
		ammo1.tracer_type_enemy = 2;
		w::Entity *shooter = world.registry.get(hb);
		shooter->tracer_shot_counter = 0;
		sim.local_player = hb; // shooter == the presenting player -> friendly
		sim.local_team = static_cast<uint8_t>(shooter->team);
		w::RoundSpawnParams rp;
		rp.owner = hb;
		rp.shooter_handle = hb.packed;
		rp.ammo_index = 1;
		rp.origin = {0.0f, 0.0f, 500.0f}; // high above every organic + no terrain
		const int slot = sim.spawn(world, rp);
		if (!expect(slot >= 0 && sim.rounds[size_t(slot)].trail_slot >= 0,
		            "a tracer round allocates a trail channel at spawn"))
			return 1;
		const int ch_i = sim.rounds[size_t(slot)].trail_slot;
		auto &ch = sim.trails.channels[size_t(ch_i)];
		if (!expect(ch.style_id == 1, "shooter == local player selects the friendly style"))
			return 1;
		if (!expect(ch.cap == 12, "stdred ring cap = the witnessed 12-entry table"))
			return 1;
		// A fresh channel starts at count -1: the first tick's pre-move append
		// (the spawn origin) only lifts it to 0 [orig: CEffectChannel_Init
		// @ 0x5db233; CEffectChannel_AppendPoint @ 0x5db2c3 -> @ 0x5db333].
		if (!expect(ch.count == -1, "a fresh channel starts at count -1")) return 1;
		sim.tick(world, nullptr, nullptr);
		if (!expect(ch.count == 0, "the first append stores nothing")) return 1;
		const w::Vec3 after_first_move = sim.rounds[size_t(slot)].pos;
		for (int t = 0; t < 4; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(ch.count == 4, "one trail point per tick after the first")) return 1;
		if (!expect(ch.pts[0].pos.x == after_first_move.x &&
		                    ch.pts[0].pos.y == after_first_move.y &&
		                    ch.pts[0].pos.z == after_first_move.z &&
		                    !(after_first_move.x == 0.0f && after_first_move.z == 500.0f),
		            "the first STORED point is the second tick's pre-move point, not the origin"))
			return 1;
		if (!expect(ch.pts[0].w == 1.0f, "std styles carry no width jitter")) return 1;
		if (!expect(ch.age == 1, "a live channel's age re-arms every append")) return 1;
		// Death by age-out: final point + kill request, then the drain timeline.
		sim.rounds[size_t(slot)].max_age_ticks = sim.rounds[size_t(slot)].age_ticks;
		sim.tick(world, nullptr, nullptr);
		if (!expect(!sim.rounds[size_t(slot)].active && ch.kill && ch.count == 5,
		            "round death appends the final point and requests the drain"))
			return 1;
		for (int t = 0; t < 11; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(ch.active && ch.count == 5,
		            "the dead trail holds shape through the cap-length grace"))
			return 1;
		for (int t = 0; t < 30; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(!ch.active, "the drained channel frees its slot")) return 1;

		// Enemy select: a presenting client on another team gets the enemy style.
		sim.local_player = w::EntityHandle{};
		sim.local_team = 99;
		const int slot_e = sim.spawn(world, rp);
		if (!expect(slot_e >= 0 && sim.rounds[size_t(slot_e)].trail_slot >= 0 &&
		                    sim.trails.channels[size_t(sim.rounds[size_t(slot_e)].trail_slot)]
		                                    .style_id == 2,
		            "a team mismatch selects the enemy style"))
			return 1;
		sim.rounds[size_t(slot_e)].active = false;
		--sim.active_count;

		// Ring cap: a long flight tops out at the style's point count.
		sim.local_team = static_cast<uint8_t>(shooter->team);
		const int slot_r = sim.spawn(world, rp);
		if (!expect(slot_r >= 0 && sim.rounds[size_t(slot_r)].trail_slot >= 0,
		            "ring-cap round spawned"))
			return 1;
		auto &ch_r = sim.trails.channels[size_t(sim.rounds[size_t(slot_r)].trail_slot)];
		for (int t = 0; t < 20; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(ch_r.count == 12, "the ring caps at the style count (oldest drops)"))
			return 1;
		sim.rounds[size_t(slot_r)].active = false;
		--sim.active_count;

		// The MP NoTracers rules bit kills the visual unless FORCETRACER
		// [orig: dword_24D1E34 & 1 gate @ 0x4ec740 / forcetracer bypass].
		sim.no_tracers_rule = true;
		const int slot_n = sim.spawn(world, rp);
		if (!expect(slot_n >= 0 && sim.rounds[size_t(slot_n)].trail_slot < 0 &&
		                    sim.rounds[size_t(slot_n)].tracer,
		            "NoTracers keeps the sim tracer flag but spawns no channel"))
			return 1;
		sim.rounds[size_t(slot_n)].active = false;
		--sim.active_count;
		ammo1.flags |= 0x8000u; // forcetracer
		const int slot_f = sim.spawn(world, rp);
		if (!expect(slot_f >= 0 && sim.rounds[size_t(slot_f)].trail_slot >= 0,
		            "FORCETRACER bypasses NoTracers for the visual"))
			return 1;
		ammo1.flags &= ~0x8000u;
		sim.no_tracers_rule = false;
		sim.rounds[size_t(slot_f)].active = false;
		--sim.active_count;
		sim.trails.reset();
		sim.fired.clear();
	}

	std::printf("round_sim_test: all green\n");
	return 0;
}
