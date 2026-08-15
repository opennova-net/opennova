// D-THROW-7: a placed throwable's authoritative pool-1 lifetime must cross the
// retail S2C lifecycle seam. The conversion tick broadcasts 0x59 to every
// in-match remote (never the host loopback); the removal tick broadcasts 0x12.

#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_tick.h>

#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>

#include <terrain_query/height_field.h>

#include <world/ai.h>
#include <world/ammo_table.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/round_sim.h>
#include <world/throwables.h>
#include <world/vehicle_motor.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

constexpr int32_t kBaseItem = 1891;
constexpr int32_t kEnemyItem = 1892;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

struct FlatField {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	terrain::TerrainHeightField field;

	FlatField() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;
	}
};

w::PlayerSpawn player_spawn(uint16_t net_id, uint8_t team) {
	w::PlayerSpawn spawn;
	spawn.position = {20.0f + static_cast<float>(net_id & 3u), 20.0f, 0.0f};
	spawn.net_id = net_id;
	spawn.team = team;
	return spawn;
}

np::NapiNPConnection make_conn(uint32_t id, int type,
		ns::ISessionTransport *transport, ns::TransportMode mode,
		w::EntityHandle owned, bool spawned) {
	np::NapiNPConnection conn;
	conn.connection_id = id;
	conn.type = type;
	conn.link.transport = transport;
	conn.link.mode = mode;
	conn.link.owned_entity = owned;
	conn.burst.spawned = spawned;
	conn.spawned_announced = spawned;
	conn.phase = spawned
			? np::ConnectionPhase::InMatch
			: np::ConnectionPhase::New;
	return conn;
}

std::vector<ns::Datagram> drain(ns::UdpSessionTransport &transport) {
	std::vector<ns::Datagram> out;
	ns::Datagram datagram;
	while (transport.pop_outbound(datagram)) out.push_back(std::move(datagram));
	return out;
}

std::vector<ns::Datagram> drain(ns::LoopbackChannel &transport) {
	std::vector<ns::Datagram> out;
	ns::Datagram datagram;
	while (transport.client_recv(datagram)) out.push_back(std::move(datagram));
	return out;
}

std::vector<const ns::Datagram *> tagged(
		const std::vector<ns::Datagram> &datagrams, uint8_t tag) {
	std::vector<const ns::Datagram *> out;
	for (const ns::Datagram &datagram : datagrams)
		if (datagram.tag == tag) out.push_back(&datagram);
	return out;
}

bool run_placed_device_spawn_and_remove_fanout() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::AiSystem ai;
	world.ai = &ai;
	FlatField flat;
	world.terrain = &flat.field;
	world.mp_session = true;

	const w::EntityHandle host =
			w::spawn_player(world, player_spawn(0xFFF0, 1));
	const w::EntityHandle owner =
			w::spawn_remote_player(world, player_spawn(0xFFF1, 1));
	const w::EntityHandle observer =
			w::spawn_remote_player(world, player_spawn(0xFFF2, 2));
	if (!expect(host.valid() && owner.valid() && observer.valid(),
			"host and two remote players spawn"))
		return false;
	world.cached.local_player = host;
	world.round_sim.local_player = host;
	world.round_sim.local_team = 1;

	w::Entity carrier_seed;
	carrier_seed.kind = w::EntityKind::Item;
	carrier_seed.item_id = 5008;
	carrier_seed.position = {10.0f, 10.0f, 0.0f};
	const w::EntityHandle carrier = world.registry.spawn(1, carrier_seed);
	if (!expect(carrier.valid(), "device carrier occupies a pool-1 row"))
		return false;
	w::stamp_saved_live_pose(*world.registry.get(carrier));

	world.ammo.entries.resize(1);
	w::AmmoTableEntry &satchel = world.ammo.entries[0];
	satchel.name = "satchel";
	satchel.valid = true;
	satchel.flags = w::kAmmoFlagUseOwnMove | w::kAmmoFlagNoAge |
			w::kAmmoFlagForceTracer;
	satchel.velocity = 6;
	satchel.max_age_ticks = 62;
	satchel.drag_fp16 = 0x10000;
	satchel.tracer_item_friendly = kBaseItem;
	satchel.tracer_item_enemy = kEnemyItem;
	world.throwables.classes.set({kBaseItem, w::ThrowClass::kSatchel,
			w::ThrowClass::kSatchel, 5, 0, 0});
	world.throwables.classes.set({kEnemyItem, w::ThrowClass::kSatchel,
			w::ThrowClass::kSatchel, 5, 0, 0});

	ns::LoopbackChannel loopback;
	ns::UdpSessionTransport remote_owner(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport remote_observer(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport pending_peer(ns::UdpSessionTransport::Role::Host);
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	auto &connections = ctx.np_protocol.connection_list;
	connections.push_back(make_conn(
			1, 2, &loopback, ns::TransportMode::Loopback, host, true));
	connections.push_back(make_conn(
			3, 1, &remote_owner, ns::TransportMode::Client, owner, true));
	connections.push_back(make_conn(
			4, 1, &remote_observer, ns::TransportMode::Client, observer, true));
	connections.push_back(make_conn(
			5, 1, &pending_peer, ns::TransportMode::Client, {}, false));

	w::RoundSpawnParams params;
	params.owner = owner;
	params.shooter_handle = owner.packed;
	// Stay a few fixed-point bits above terrain after the first gravity step:
	// the rest gate converts without the terrain-penetration leg clearing the
	// already-authored parent.
	params.origin = {11.0f, 10.0f, 1.0f / 1024.0f};
	params.dir_yaw_bam = 0x20000000;
	params.ammo_index = 0;
	params.shot_seq = 0x1234;
	const int slot = world.round_sim.spawn(world, params);
	if (!expect(slot >= 0, "satchel round enters the authoritative projectile pool"))
		return false;
	w::LiveRound &round = world.round_sim.rounds[static_cast<std::size_t>(slot)];
	round.parent = carrier;
	round.parent_spawn_id = world.registry.get(carrier)->registry_spawn_id;

	np::Server_TickUpdate(ctx);
	if (!expect(world.throwables.devices.size() == 1,
			"the conversion tick creates one placed-device lifetime"))
		return false;
	const w::PlacedDevice &device = world.throwables.devices.front();
	const std::vector<ns::Datagram> owner_spawn = drain(remote_owner);
	const std::vector<ns::Datagram> observer_spawn = drain(remote_observer);
	const std::vector<ns::Datagram> loop_spawn = drain(loopback);
	const std::vector<ns::Datagram> pending_spawn = drain(pending_peer);
	const auto owner_59 = tagged(owner_spawn, s2c::DEPLOYED_ITEM);
	const auto observer_59 = tagged(observer_spawn, s2c::DEPLOYED_ITEM);
	if (!expect(owner_59.size() == 1 && observer_59.size() == 1 &&
			tagged(loop_spawn, s2c::DEPLOYED_ITEM).empty() &&
			tagged(pending_spawn, s2c::DEPLOYED_ITEM).empty(),
			"0x59 reaches both in-match remotes, never loopback or pre-spawn peers"))
		return false;

	DeployedItemSpawn decoded_spawn;
	std::size_t consumed = 0;
	if (!expect(owner_59.front()->reliable &&
			decode_deployed_item_spawn(owner_59.front()->body.data(),
					owner_59.front()->body.size(), decoded_spawn, consumed) &&
			consumed == owner_59.front()->body.size(),
			"the relayed 0x59 is a reliable exact typed body"))
		return false;
	if (!expect(decoded_spawn.item_id == kBaseItem &&
			decoded_spawn.owner_handle == owner.packed &&
			decoded_spawn.friendly_item_id == kBaseItem &&
			decoded_spawn.enemy_item_id == kEnemyItem &&
			decoded_spawn.slot_handle == device.entity.packed &&
			decoded_spawn.parent_handle == carrier.packed &&
			decoded_spawn.pos_x == w::to_fixed(device.pos.x) &&
			decoded_spawn.pos_y == w::to_fixed(device.pos.y) &&
			decoded_spawn.pos_z == w::to_fixed(device.pos.z) &&
			// words 12/13/14 = entity+16/+20/+24 high halves = yaw/pitch/roll
			decoded_spawn.angle_x == static_cast<uint16_t>(
					static_cast<uint32_t>(device.yaw_bam) >> 16) &&
			decoded_spawn.angle_y == static_cast<uint16_t>(
					static_cast<uint32_t>(device.pitch_bam) >> 16) &&
			decoded_spawn.angle_z == static_cast<uint16_t>(
					static_cast<uint32_t>(device.roll_bam) >> 16),
			"0x59 carries base/friend/foe ids, owner, parent, pose, and exact slot"))
		return false;

	w::Entity *placed = world.registry.get(device.entity);
	if (!expect(placed != nullptr, "placed device remains live before removal"))
		return false;
	placed->health = -1;
	world.throwables.devices.front().think_delay_ticks = 0;
	np::Server_TickUpdate(ctx);
	const std::vector<ns::Datagram> owner_remove = drain(remote_owner);
	const std::vector<ns::Datagram> observer_remove = drain(remote_observer);
	const std::vector<ns::Datagram> loop_remove = drain(loopback);
	const auto owner_12 = tagged(owner_remove, s2c::ENTITY_REMOVE);
	const auto observer_12 = tagged(observer_remove, s2c::ENTITY_REMOVE);
	if (!expect(owner_12.size() == 1 && observer_12.size() == 1 &&
			tagged(loop_remove, s2c::ENTITY_REMOVE).empty(),
			"0x12 reaches both remotes and skips the authoritative loopback"))
		return false;
	EntityRemove decoded_remove;
	consumed = 0;
	return expect(owner_12.front()->reliable &&
			decode_entity_remove(owner_12.front()->body.data(),
					owner_12.front()->body.size(), decoded_remove, consumed) &&
			consumed == owner_12.front()->body.size() &&
			decoded_remove.entity_handle == device.entity.packed &&
			world.registry.get(device.entity) == nullptr,
			"the reliable 0x12 names the exact lifetime removed by authority");
}

} // namespace

int main() {
	if (!run_placed_device_spawn_and_remove_fanout()) return 1;
	std::puts("placed_device_relay_test: PASS");
	return 0;
}
