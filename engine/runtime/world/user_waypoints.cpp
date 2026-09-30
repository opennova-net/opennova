#include <runtime/world/user_waypoints.h>

#include <base/io/strutil.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <vector>

namespace opennova::world {

EntityHandle waypoint_create_for_player(World &world, int32_t x, int32_t y, const char *name,
		EntityHandle owner, std::string &received_line) {
	received_line.clear();
	// The record: the def ordinal and row, the position with the terrain
	// height, the name (or "Unknown"), the owner at +368, +290 = 3 and team 0.
	// [orig: @0x4dfcf0..0x4dfd8b — waypoint_data[7]/[8] the def, [1]/[2] x/y,
	//  [3] Terrain_SampleHeightBilinear(x, y), strcpy [61] (+244) name or
	//  "Unknown", [92] (+368) owner, HIWORD([72]) (+290) = 3, BYTE2([88])
	//  (+354) = 0]
	const std::string text = name != nullptr ? std::string(name) : std::string("Unknown");
	Entity seed;
	seed.kind = EntityKind::Marker;
	seed.item_id = kUserWaypointTypeId;
	seed.item_type_index = world.tables.user_waypoint_type_index;
	seed.has_item_def = true;
	seed.item_type = world.tables.user_waypoint_item_type;
	seed.position.x = static_cast<float>(x) / 65536.0f;
	seed.position.y = static_cast<float>(y) / 65536.0f;
	seed.position.z = world.tables.terrain != nullptr
			? terrain::height_field_height_world_bilinear(*world.tables.terrain, seed.position.x,
					  -seed.position.y)
			: 0.0f;
	seed.display_name = text.substr(0, kUserWaypointNameMax);
	seed.primary_occupant = owner;
	seed.ammo_count = 3;
	seed.team = 0;

	// Every pool-4 row with a def, the same owner, the same x / y and the
	// same name (stricmp) is wiped in place first.
	// [orig: @0x4dfd9a..0x4dfe12 — `*(entry+0x1C) && entry+368 == owner &&
	//  entry+4 == x && entry+8 == y && !stricmp(entry+244, name)` ->
	//  memset(entry, 0, 0x2B4)]
	std::vector<EntityHandle> wiped;
	world.registry.for_each_in_pool(kUserWaypointPool, [&](const Entity &e) {
		if (e.item_type_index == 0) return;
		if (e.primary_occupant != owner) return;
		if (world::to_fixed(e.position.x) != x || world::to_fixed(e.position.y) != y) return;
		if (!strutil::iequals(e.display_name, text)) return;
		wiped.push_back(e.handle);
	});
	for (const EntityHandle h : wiped) world.registry.despawn(h);

	const EntityHandle handle = world.registry.spawn(kUserWaypointPool, seed);
	if (!handle.valid()) return handle;
	// A waypoint from another player posts its receipt on the SYSTEM ring.
	// [orig: `if (ownerPlayer != g_LocalPlayerEntity)` @0x4dfe38; sprintf
	//  "Waypoint \"%s\" received from %s." with owner->Name @0x4dfe56]
	if (owner != world.cached.local_player) {
		const Entity *owner_entity = world.registry.get(owner);
		const std::string owner_name = owner_entity != nullptr ? owner_entity->display_name
				: std::string();
		char line[128];
		std::snprintf(line, sizeof(line), "Waypoint \"%s\" received from %s.", text.c_str(),
				owner_name.c_str());
		received_line = line;
	}
	return handle;
}

EntityHandle place_user_waypoint(World &world, int32_t x, int32_t y, const std::string &name) {
	if (world.user_waypoints.count >= UserWaypointTable::kCapacity) return EntityHandle{};
	std::string line;
	const EntityHandle handle = waypoint_create_for_player(world, x, y,
			name.substr(0, 31).c_str(), world.cached.local_player, line);
	if (world.registry.get(handle) == nullptr) return EntityHandle{};
	world.user_waypoints.append(handle);
	return handle;
}

EntityHandle delete_hovered_user_waypoint(World &world) {
	if (world.user_waypoints.count <= 0) return EntityHandle{};
	const int index = world.user_waypoints.first_hovered();
	if (index < 0) return EntityHandle{};
	const EntityHandle handle = world.user_waypoints.entries[static_cast<size_t>(index)].handle;
	destroy_pool_row(world, handle);
	world.user_waypoints.remove_at(index);
	return handle;
}

void clear_user_waypoints(World &world, std::vector<EntityHandle> &removed) {
	removed.clear();
	if (world.user_waypoints.count <= 0) return;
	for (const UserWaypointTable::Entry &entry : world.user_waypoints.entries) {
		if (!entry.handle.valid()) continue;
		removed.push_back(entry.handle);
		destroy_pool_row(world, entry.handle);
	}
	world.user_waypoints.clear();
}

void destroy_pool_row(World &world, EntityHandle handle) {
	if (world.registry.get(handle) != nullptr) world.registry.despawn(handle);
}

bool UserWaypointTable::append(EntityHandle handle) {
	if (count >= kCapacity) return false;
	entries[static_cast<size_t>(count)].handle = handle;
	entries[static_cast<size_t>(count)].hover = false;
	++count;
	return true;
}

int UserWaypointTable::first_hovered() const {
	for (int i = 0; i < kCapacity; ++i) {
		const Entry &e = entries[static_cast<size_t>(i)];
		if (e.handle.valid() && e.hover) return i;
	}
	return -1;
}

void UserWaypointTable::remove_at(int index) {
	if (index < 0 || index >= kCapacity || count <= 0) return;
	// [orig: the slot zeroed @0x5479fe..0x547a07, the memcpy of (count - i -
	//  1) entries down @0x547a17, entry[count - 1] zeroed @0x547a24..0x547a2a,
	//  count - 1 @0x547a37]
	entries[static_cast<size_t>(index)] = Entry{};
	for (int i = index; i + 1 < count && i + 1 < kCapacity; ++i)
		entries[static_cast<size_t>(i)] = entries[static_cast<size_t>(i + 1)];
	if (count - 1 >= 0 && count - 1 < kCapacity)
		entries[static_cast<size_t>(count - 1)] = Entry{};
	--count;
}

} // namespace opennova::world
