#include <runtime/inmatch/server_vehicle_spawn.h>

#include <algorithm>

#include <net/npwire/ingame_encode.h>   // encode_vehicle_spawn_availability / encode_full_entity_spawn
#include <net/npwire/ingame_message_id.h>
#include <runtime/replication/entity_wire_bridge.h> // build_full_entity_spawn
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

constexpr uint32_t kDeployableFlag = 0x1000u;  // Entity_SpawnDeployable @0x51C492
constexpr int32_t kSpawnRaiseNoUserpoint = 0x20000; // +2.0 world units @0x51C682

} // namespace

int Server_CountEntitiesByTypeAndTeam(const world::World &world, uint16_t type_id, uint8_t team) {
	int count = 0;
	world.registry.for_each_in_pool(1, [&](const world::Entity &e) {
		if (!e.has_item_def) return;                            // entity+32 @0x51048C
		if ((e.flags & 1u) != 0u || (e.flags & kDeployableFlag) == 0u) return; // @0x5104B2
		if (e.team == team && e.item_id == type_id) ++count;
	});
	return count;
}

std::vector<uint8_t> Server_BuildVehicleSpawnAvailability(
		const NapiNPServerCtx &ctx, const world::World &world, uint8_t team) {
	VehicleSpawnAvailabilityList list;
	for (const auto &row : ctx.vehicle_spawn_limits) {
		VehicleSpawnAvailabilityRow out;
		out.type_id = row.type_id;
		if (ctx.vehicle_spawns_unlimited) {                        // @0x5105FF
			out.available = kVehicleSpawnUnlimited;
			out.max_count = kVehicleSpawnUnlimited;
		} else if (row.type_cap == -1 && row.per_team_flag == -1) { // @0x510610..0x510615
			out.available = kVehicleSpawnUnlimited;
			out.max_count = kVehicleSpawnUnlimited;
		} else if (row.per_team_flag == -1) {                       // @0x510622
			out.max_count = kVehicleSpawnUnlimited;
			out.available = static_cast<uint8_t>(
					row.type_cap - Server_CountEntitiesByTypeAndTeam(world, row.type_id, team));
		} else {                                                    // @0x510639..0x510651
			const int32_t max_count = team < row.team_slots.size() ? row.team_slots[team] : 0;
			uint8_t available = static_cast<uint8_t>(max_count);
			if (row.type_cap != -1) {
				const uint8_t computed = static_cast<uint8_t>(
						row.type_cap - Server_CountEntitiesByTypeAndTeam(world, row.type_id, team));
				if (computed <= static_cast<uint8_t>(max_count)) available = computed;
			}
			out.max_count = static_cast<uint8_t>(max_count);
			out.available = available;
		}
		list.rows.push_back(out);
	}
	list.terminated = true;
	return encode_vehicle_spawn_availability(list);
}

bool Server_VehicleSpawnAllowed(NapiNPServerCtx &ctx, const world::World &world,
		uint16_t type_id, uint8_t team, bool consume) {
	// The retail walk keeps the LAST matching row [orig: @0x5104D8..0x5104E4].
	NapiNPServerCtx::VehicleSpawnLimitRow *row = nullptr;
	for (auto &candidate : ctx.vehicle_spawn_limits)
		if (candidate.type_id == type_id) row = &candidate;
	if (row == nullptr) return false;                             // @0x5104E8
	if (team >= row->team_slots.size()) return false;
	int32_t &slots = row->team_slots[team];                       // row[team + 3] @0x5104F0
	if (slots == 0) return false;
	if (row->type_cap != -1 &&
			Server_CountEntitiesByTypeAndTeam(world, type_id, team) >= row->type_cap)
		return false;                                              // @0x510513
	if (consume && slots != -1) --slots;                          // @0x510521..0x51052B
	return true;
}

bool Server_HandleVehicleSpawnRequest(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		const VehicleSpawnRequest &request, world::World &world) {
	if (!ctx.is_authority || !conn.link.owned_entity.valid()) return false;
	const world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr || conn.link.spectator) return false;   // slot+100567 @0x51C4F2
	if (request.source_handle == 0xFFFFu) return false;           // @0x51C532
	const world::EntityHandle source_handle{request.source_handle};
	if (source_handle.pool() >= 5) return false;                  // (handle & 0xF000) < 0x5000 @0x51C570
	const world::Entity *source = world.registry.get(source_handle);
	if (source == nullptr || !source->has_item_def) return false; // @0x51C5A2..0x51C5AD
	// (1 << typeIndex) & def+2772, then g_ItemGroups[typeIndex].itemId
	// [orig: @0x51C5C0]: the traits sweep records the resolved id beside its
	// group bit, so the bit test is a group lookup.
	uint16_t item_id = 0;
	for (size_t i = 0; i < source->vehicle_spawn_groups.size() &&
			i < source->vehicle_spawn_ids.size(); ++i) {
		if (source->vehicle_spawn_groups[i] == request.type_index) {
			const int32_t full = source->vehicle_spawn_ids[i];
			item_id = static_cast<uint16_t>(full >= 100000 ? full - 100000 : full);
		}
	}
	if (item_id == 0) return false;                               // @0x51C55E
	const uint8_t team = player->team;
	if (source->team != 0 && source->team != team) return false; // @0x51C5C6..0x51C5F6
	if (!ctx.vehicle_spawns_unlimited &&
			!Server_VehicleSpawnAllowed(ctx, world, item_id, team, /*consume=*/true))
		return false;
	// The spawn position: the source's "boat" / "helo" userpoint transformed
	// by its attitude, else its position raised 2.0 [orig: @0x51C60C..0x51C682].
	// Model userpoints are not reachable from this layer: the raised position.
	const int32_t position[3] = {
			world::to_fixed(source->position.x),
			world::to_fixed(source->position.y),
			world::to_fixed(source->position.z) + kSpawnRaiseNoUserpoint};
	if (!ctx.deployable_spawner) return false;
	const world::EntityHandle spawned = ctx.deployable_spawner(world, item_id, team, position);
	world::Entity *vehicle = world.registry.get(spawned);
	if (vehicle == nullptr) return false;                         // @0x51C6AA
	vehicle->team = team;                                         // @0x51C6BD
	vehicle->flags |= kDeployableFlag;                            // Entity_SpawnDeployable @0x51C492
	// S2C 0x18, mask 0x90: every active player except the listen host.
	{
		const std::vector<uint8_t> body =
				encode_full_entity_spawn(replication::build_full_entity_spawn(*vehicle));
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!is_in_match(c) || c.link.transport == nullptr) continue;
			if (c.link.mode == replication::TransportMode::Loopback) continue;
			c.link.transport->host_send(s2c::FULL_ENTITY_SPAWN, body);
		}
	}
	// The entity+533 sibling fan (same +0x215 group) is unmodeled. Then the
	// requester mounts: Entity_ProcessVehicleAttach({player, vehicle, def+613
	// bone}) — the def's default seat bone is unmodeled, so bone 0.
	world.vehicles.process_attach(conn.link.owned_entity, spawned, 0);
	return true;
}

} // namespace opennova::inmatch
