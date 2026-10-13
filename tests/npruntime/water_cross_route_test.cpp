// D-SND-36 / D-SND-37: the authority's water-crossing fan. Every crossing the
// motor records goes as S2C 0x34 to every in-match player (slot state 6/7,
// dead or alive: mask 128 reads no entity, D-NET-637) under the authority,
// single player included; the authority's own player hears it through its
// loopback as the slot sound its 0x34 handler would play. The set is the
// crossing's airborne bit: BODYWATER1 in the air, SURFACE_WTR not. The
// breath timers' WATER_GAG and SURFACE_BREATH ride the same fan. A set no
// loaded bank holds is a null set, which sends nothing, and the breath
// composites go only for a body with an ItemDef (D-NET-653).
// [orig: Server_SendOverlayActionToAlive @0x50a1b0 (the is_authority gate
//  @0x50a1bf, send_mask 128 @0x50a1d3); NapiNPServer_SendFiltered's 0x80 leg
//  @0x4C893E..0x4C8953; Server_UpdatePlayerBreathTimers @0x50D770 (the gag
//  call @0x50D878, the breath call @0x50D8CB); Entity_UpdateInfantryPlayerBody
//  @0x4b82e3..0x4b82f6 (g_SndBodyWater1 / g_SndSurfaceWtr, the registry rows
//  33 @0x82FA34 / 32 @0x82FA10); NapiNPClientMsg_PlaySoundByName @0x4283A0;
//  the null test @0x50A1B4..0x50A1B6; the ItemDef tests @0x50D863 / @0x50D88D]
#include <formats/lwf/lwf.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_entity_routes.h>
#include <runtime/inmatch/server_idle_timers.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode_session.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

std::vector<PlaySoundCommand> take_sounds(ns::LoopbackChannel &channel) {
	std::vector<PlaySoundCommand> out;
	ns::Datagram dg;
	while (channel.client_recv(dg)) {
		if (dg.tag != s2c::PLAY_SOUND) continue;
		PlaySoundCommand cmd;
		CHECK(decode_play_sound(dg.body.data(), dg.body.size(), cmd));
		out.push_back(cmd);
	}
	return out;
}

// A player body carries the Player ItemDef (+0x20).
w::EntityHandle spawn_player(w::World &world, int health) {
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	person.alive = health > 0;
	person.health = health;
	person.has_item_def = true;
	return world.registry.spawn(0, person);
}

// The loaded banks the sets resolve against: one bank holding `names`.
void load_banks(audio::SoundSetIndex &sets, w::World &world,
		std::initializer_list<const char *> names) {
	lwf::File bank;
	for (const char *name : names) {
		lwf::Multi set;
		set.name = name;
		bank.multis.push_back(set);
	}
	sets.clear();
	sets.add_bank(0, bank);
	world.tables.sound_sets = &sets;
}

const std::initializer_list<const char *> kShippedSets = {
		"BODYWATER1", "SURFACE_WTR", "BM1_WATER_GAG", "BM1_SURFACE_BREATH",
		"BM1_SURFACE_GASP", "BM2_WATER_GAG"};

constexpr int32_t kWater = 12 * 65536;

// The set names, by the registry's binding of the two slots (D-SND-37).
void test_set_binding() {
	CHECK(std::string(kWaterCrossAirborneEffect) == "BODYWATER1");
	CHECK(std::string(kWaterCrossWadeEffect) == "SURFACE_WTR");
}

// Single player: no session, the authority's own (loopback) player alone.
// Both crossings play locally, positioned at the wire's whole units on the
// plane with no entity, and nothing goes on the wire (D-SND-36).
void test_single_player_hears_its_own_crossings() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = false;
	audio::SoundSetIndex sets;
	load_banks(sets, world, kShippedSets);
	const w::EntityHandle local = spawn_player(world, 100);
	ns::LoopbackChannel local_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 0;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			1, 2, &local_wire, ns::TransportMode::Loopback, local, true));

	world.out.water_crossings.add(-834 * 65536 + 0x8000, 122 * 65536 + 0x4000, kWater, true);
	world.out.water_crossings.add(10 * 65536, -5 * 65536 - 1, kWater, false);
	inmatch::Server_RouteWaterCrossings(ctx, world);
	CHECK(world.out.water_crossings.events.empty());
	CHECK(take_sounds(local_wire).empty());
	CHECK(world.out.slot_sounds.size() == 2);
	if (world.out.slot_sounds.size() == 2) {
		const w::SoundSlotEvent &jump = world.out.slot_sounds[0];
		CHECK(std::strcmp(jump.set_name, "BODYWATER1") == 0);
		CHECK(jump.pos[0] == -834 * 65536 && jump.pos[1] == 122 * 65536 && jump.pos[2] == kWater);
		CHECK(jump.source_handle == 0xFFFF);
		const w::SoundSlotEvent &wade = world.out.slot_sounds[1];
		CHECK(std::strcmp(wade.set_name, "SURFACE_WTR") == 0);
		CHECK(wade.pos[0] == 10 * 65536 && wade.pos[1] == -6 * 65536 && wade.pos[2] == kWater);
	}

	// A dead local player hears it too: mask 128 reads the slot state alone
	// (D-NET-637); the queue drains.
	world.out.slot_sounds.clear();
	world.registry.get(local)->health = 0;
	world.registry.get(local)->alive = false;
	world.registry.get(local)->flags |= w::kEntityFlagDead;
	world.out.water_crossings.add(0, 0, kWater, true);
	inmatch::Server_RouteWaterCrossings(ctx, world);
	CHECK(world.out.slot_sounds.size() == 1);
	CHECK(world.out.water_crossings.events.empty());
}

// A listen host in session: every in-match remote gets the 0x34, a dead one
// too (D-NET-637), and the host's own player hears it through the loopback.
void test_listen_host_fans_to_every_in_match_player() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	audio::SoundSetIndex sets;
	load_banks(sets, world, kShippedSets);
	const w::EntityHandle alive = spawn_player(world, 100);
	const w::EntityHandle dead = spawn_player(world, 0);
	const w::EntityHandle local = spawn_player(world, 100);
	ns::LoopbackChannel alive_wire, dead_wire, local_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &connections = ctx.np_protocol.connection_list;
	connections.push_back(conn_fixture::make_conn(1, 1, &alive_wire,
			ns::TransportMode::Client, alive, true));
	connections.push_back(conn_fixture::make_conn(2, 1, &dead_wire,
			ns::TransportMode::Client, dead, true));
	connections.push_back(conn_fixture::make_conn(3, 2, &local_wire,
			ns::TransportMode::Loopback, local, true));

	world.out.water_crossings.add(3 * 65536, 4 * 65536, kWater, true);
	world.out.water_crossings.add(5 * 65536, 6 * 65536, kWater, false);
	inmatch::Server_RouteWaterCrossings(ctx, world);
	const std::vector<PlaySoundCommand> remote = take_sounds(alive_wire);
	CHECK(remote.size() == 2);
	if (remote.size() == 2) {
		CHECK(remote[0].flag == 1 && remote[0].sound_name == "BODYWATER1");
		CHECK(remote[0].pos_x == 3 && remote[0].pos_y == 4 && remote[0].pos_z == 12);
		CHECK(remote[1].sound_name == "SURFACE_WTR");
	}
	CHECK(take_sounds(dead_wire).size() == 2);
	CHECK(take_sounds(local_wire).empty());
	CHECK(world.out.slot_sounds.size() == 2);
	if (world.out.slot_sounds.size() == 2) {
		CHECK(std::strcmp(world.out.slot_sounds[0].set_name, "BODYWATER1") == 0);
		CHECK(std::strcmp(world.out.slot_sounds[1].set_name, "SURFACE_WTR") == 0);
	}
}

// The breath timers' composites ride the same mask-128 fan: a dead in-match
// listener hears a diver's WATER_GAG warning and its SURFACE_BREATH
// (D-NET-637). [orig: Server_UpdatePlayerBreathTimers @0x50D770 - the
//  warnings @0x50D842..0x50D878, the surfacing @0x50D882..0x50D8CB]
void test_breath_composites_reach_a_dead_listener() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	world.script.wac_values.breathtime = 20; // the limit: 80 samples
	world.env.water_z = kWater;
	audio::SoundSetIndex sets;
	load_banks(sets, world, kShippedSets);
	const w::EntityHandle diver = spawn_player(world, 100);
	world.registry.get(diver)->anim_slot = 1;
	const w::EntityHandle dead = spawn_player(world, 0);
	world.registry.get(dead)->flags |= w::kEntityFlagDead;
	ns::LoopbackChannel diver_wire, dead_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &connections = ctx.np_protocol.connection_list;
	connections.push_back(conn_fixture::make_conn(1, 1, &diver_wire,
			ns::TransportMode::Client, diver, true));
	connections.push_back(conn_fixture::make_conn(2, 1, &dead_wire,
			ns::TransportMode::Client, dead, true));

	const auto ends_with = [](const std::string &name, const char *suffix) {
		const std::string tail(suffix);
		return name.size() > tail.size() &&
				name.compare(name.size() - tail.size(), tail.size(), tail) == 0;
	};
	// Below the plane, sample 68 (the limit less 12) warns.
	connections[0].link.underwater_breath_samples = 67;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
	CHECK(connections[0].link.underwater_breath_samples == 68);
	const std::vector<PlaySoundCommand> gag = take_sounds(dead_wire);
	CHECK(gag.size() == 1);
	if (gag.size() == 1) CHECK(ends_with(gag[0].sound_name, "_WATER_GAG"));
	CHECK(take_sounds(diver_wire).size() == 1);
	// Surfacing after more than four samples, within 160% of breathtime.
	world.registry.get(diver)->position.z = 20.0f;
	connections[0].link.underwater_breath_samples = 10;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
	const std::vector<PlaySoundCommand> breath = take_sounds(dead_wire);
	CHECK(breath.size() == 1);
	if (breath.size() == 1) CHECK(ends_with(breath[0].sound_name, "_SURFACE_BREATH"));
}

// A crossing whose set no loaded bank holds sends nothing, to a remote or to
// the host's own player: every crossing site hands the fan the mission
// start's resolve, a null set on a miss (D-NET-653).
// [orig: DialogSystem_Init @0x5275E0 (the resolve @0x527692..0x5276C0);
//  Server_SendOverlayActionToAlive's null test @0x50A1B4..0x50A1B6]
void test_unresolved_crossing_sends_nothing() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	audio::SoundSetIndex sets;
	load_banks(sets, world, {"SURFACE_WTR"});
	const w::EntityHandle remote = spawn_player(world, 100);
	const w::EntityHandle local = spawn_player(world, 100);
	ns::LoopbackChannel remote_wire, local_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &connections = ctx.np_protocol.connection_list;
	connections.push_back(conn_fixture::make_conn(1, 1, &remote_wire,
			ns::TransportMode::Client, remote, true));
	connections.push_back(conn_fixture::make_conn(2, 2, &local_wire,
			ns::TransportMode::Loopback, local, true));

	world.out.water_crossings.add(3 * 65536, 4 * 65536, kWater, true);
	world.out.water_crossings.add(5 * 65536, 6 * 65536, kWater, false);
	inmatch::Server_RouteWaterCrossings(ctx, world);
	const std::vector<PlaySoundCommand> heard = take_sounds(remote_wire);
	CHECK(heard.size() == 1);
	if (heard.size() == 1) CHECK(heard[0].sound_name == "SURFACE_WTR");
	CHECK(world.out.slot_sounds.size() == 1);
	if (world.out.slot_sounds.size() == 1)
		CHECK(std::strcmp(world.out.slot_sounds[0].set_name, "SURFACE_WTR") == 0);
	CHECK(world.out.water_crossings.events.empty());

	// No loaded bank at all: the wade crossing sends nothing either, and the
	// queue drains.
	world.out.slot_sounds.clear();
	world.tables.sound_sets = nullptr;
	world.out.water_crossings.add(3 * 65536, 4 * 65536, kWater, false);
	inmatch::Server_RouteWaterCrossings(ctx, world);
	CHECK(take_sounds(remote_wire).empty());
	CHECK(world.out.slot_sounds.empty());
	CHECK(world.out.water_crossings.events.empty());
}

// The breath composites' gates: a body with no ItemDef (+0x20) sends neither
// the WATER_GAG nor the surfacing composite; a body with one and a zero
// body-model byte (+0x374) sends BM1's (the prefix lookup maps 0 to type 1);
// a composite no loaded bank holds sends nothing, while the counter runs on
// (D-NET-653). [orig: Server_UpdatePlayerBreathTimers - the ItemDef tests
//  `cmp [ecx+20h], ebx` @0x50D863..0x50D866 / @0x50D88D..0x50D890;
//  Entity_GetBodyModelPrefix @0x5280F0 (@0x5280F8..0x528103);
//  SoundProfile_FindByEntityAndType @0x5281D6; the null test @0x50A1B4]
void test_breath_composite_gates() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	world.script.wac_values.breathtime = 20; // the limit: 80 samples
	world.env.water_z = kWater;
	audio::SoundSetIndex sets;
	load_banks(sets, world, kShippedSets);
	const w::EntityHandle diver = spawn_player(world, 100);
	const w::EntityHandle listener = spawn_player(world, 100);
	ns::LoopbackChannel diver_wire, listener_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &connections = ctx.np_protocol.connection_list;
	connections.push_back(conn_fixture::make_conn(1, 1, &diver_wire,
			ns::TransportMode::Client, diver, true));
	connections.push_back(conn_fixture::make_conn(2, 1, &listener_wire,
			ns::TransportMode::Client, listener, true));
	// The listener's eye stays above the plane, so it never samples.
	world.registry.get(listener)->position.z = 20.0f;
	auto warn = [&]() {
		world.registry.get(diver)->position.z = 0.0f;
		connections[0].link.underwater_breath_samples = 67;
		inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
		take_sounds(diver_wire);
		return take_sounds(listener_wire);
	};
	auto surface = [&]() {
		world.registry.get(diver)->position.z = 20.0f;
		connections[0].link.underwater_breath_samples = 10;
		inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
		take_sounds(diver_wire);
		return take_sounds(listener_wire);
	};

	// A zero body-model byte with an ItemDef: BM1's composites.
	world.registry.get(diver)->anim_slot = 0;
	std::vector<PlaySoundCommand> heard = warn();
	CHECK(heard.size() == 1);
	if (heard.size() == 1) CHECK(heard[0].sound_name == "BM1_WATER_GAG");
	CHECK(connections[0].link.underwater_breath_samples == 68);
	heard = surface();
	CHECK(heard.size() == 1);
	if (heard.size() == 1) CHECK(heard[0].sound_name == "BM1_SURFACE_BREATH");
	CHECK(connections[0].link.underwater_breath_samples == 0);

	// No ItemDef: neither composite, whatever the body-model byte; the counter
	// runs and resets as before.
	world.registry.get(diver)->has_item_def = false;
	world.registry.get(diver)->anim_slot = 2;
	CHECK(warn().empty());
	CHECK(connections[0].link.underwater_breath_samples == 68);
	CHECK(surface().empty());
	CHECK(connections[0].link.underwater_breath_samples == 0);

	// A composite no loaded bank holds: BM2_WATER_GAG resolves and sends,
	// BM2_SURFACE_BREATH does not and sends nothing.
	world.registry.get(diver)->has_item_def = true;
	heard = warn();
	CHECK(heard.size() == 1);
	if (heard.size() == 1) CHECK(heard[0].sound_name == "BM2_WATER_GAG");
	CHECK(surface().empty());
	CHECK(connections[0].link.underwater_breath_samples == 0);
}

} // namespace

int main() {
	test_set_binding();
	test_single_player_hears_its_own_crossings();
	test_listen_host_fans_to_every_in_match_player();
	test_breath_composites_reach_a_dead_listener();
	test_unresolved_crossing_sends_nothing();
	test_breath_composite_gates();
	if (failures == 0) std::printf("npruntime_water_cross_route: OK\n");
	return failures == 0 ? 0 : 1;
}
