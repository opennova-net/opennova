// D-SND-36 / D-SND-37: the authority's water-crossing fan. Every crossing the
// motor records goes as S2C 0x34 to every alive in-match player under the
// authority, single player included; the authority's own player hears it
// through its loopback as the slot sound its 0x34 handler would play. The set
// is the crossing's airborne bit: BODYWATER1 in the air, SURFACE_WTR not.
// [orig: Server_SendOverlayActionToAlive @0x50a1b0 (the is_authority gate
//  @0x50a1bf, send_mask 128 @0x50a1d3); Entity_UpdateInfantryPlayerBody
//  @0x4b82e3..0x4b82f6 (g_SndBodyWater1 / g_SndSurfaceWtr, the registry rows
//  33 @0x82FA34 / 32 @0x82FA10); NapiNPClientMsg_PlaySoundByName @0x4283A0]
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_entity_routes.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode_session.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <cstring>
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

w::EntityHandle spawn_player(w::World &world, int health) {
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	person.alive = health > 0;
	person.health = health;
	return world.registry.spawn(0, person);
}

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

	// A dead local player hears nothing (the mask-128 alive filter); the
	// queue still drains.
	world.out.slot_sounds.clear();
	world.registry.get(local)->health = 0;
	world.out.water_crossings.add(0, 0, kWater, true);
	inmatch::Server_RouteWaterCrossings(ctx, world);
	CHECK(world.out.slot_sounds.empty());
	CHECK(world.out.water_crossings.events.empty());
}

// A listen host in session: every alive remote gets the 0x34, a dead one does
// not, and the host's own player hears it through the loopback.
void test_listen_host_fans_to_every_alive_player() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
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
	CHECK(take_sounds(dead_wire).empty());
	CHECK(take_sounds(local_wire).empty());
	CHECK(world.out.slot_sounds.size() == 2);
	if (world.out.slot_sounds.size() == 2) {
		CHECK(std::strcmp(world.out.slot_sounds[0].set_name, "BODYWATER1") == 0);
		CHECK(std::strcmp(world.out.slot_sounds[1].set_name, "SURFACE_WTR") == 0);
	}
}

} // namespace

int main() {
	test_set_binding();
	test_single_player_hears_its_own_crossings();
	test_listen_host_fans_to_every_alive_player();
	if (failures == 0) std::printf("npruntime_water_cross_route: OK\n");
	return failures == 0 ? 0 : 1;
}
