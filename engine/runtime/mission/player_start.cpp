#include <runtime/mission/player_start.h>

#include <cmath>

#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <runtime/world/spawn_select.h>

namespace opennova::mission {

namespace {

bool holds(double metres) {
	return metres >= bms::kFixed16Min && metres <= bms::kFixed16Max;
}

} // namespace

int16_t wrapped_yaw(double degrees) {
	const long whole = std::lround(degrees);
	const long turn = whole % 360;
	return int16_t(turn < 0 ? turn + 360 : turn);
}

bool place_player_start(bms::File &file, const PlayerStart &start, PlayerStartPlaced &out, std::string &error) {
	out = PlayerStartPlaced();
	for (const double metres : start.at)
		if (!holds(metres)) {
			error = "The point is past what a mission's positions hold (32,768 m from its origin).";
			return false;
		}
	// The single player's chain under the mission's mode: the spawn joins it as team 1 [orig:
	// Server_PositionPlayerForSpawn @ 0x50CF60; runtime/mission's spawn_local_player_at_start].
	const uint32_t mode = game_type::for_mission_attribs(file.header.attrib_flags);
	const world::StartMarkerTypes types = world::start_marker_types(mode, 1);
	if (types.primary == 0) {
		error = "The mission's mode places its single player at no start marker.";
		return false;
	}
	const auto count_of = [&file](int32_t type) {
		size_t n = 0;
		for (const bms::Entity &marker : file.markers)
			if (marker.type_id == type) ++n;
		return n;
	};
	out.type = count_of(types.primary) != 0 || count_of(types.fallback) == 0 ? types.primary : types.fallback;
	const int16_t yaw = wrapped_yaw(start.yaw);
	const auto place = [&](bms::Entity &marker) {
		marker.x = bms::to_fixed_16_16(start.at[0]);
		marker.y = bms::to_fixed_16_16(start.at[1]);
		marker.z = bms::to_fixed_16_16(start.at[2]);
		marker.yaw = yaw;
		marker.pitch = 0;
		marker.roll = 0;
		// A team-2 start queues a mount onto its carrier as the player deploys [orig:
		// Server_PositionPlayerForSpawn @ 0x50D42E..0x50D45A]: the start of its own stands on foot.
		if (marker.team == 2) marker.team = 1;
	};
	for (bms::Entity &marker : file.markers)
		if (marker.type_id == out.type) {
			place(marker);
			++out.moved;
		}
	if (out.moved != 0) return true;
	// None of either: one of the primary type, at the point (bms_edit's new record, its SSN the next).
	EntityTransform at;
	const size_t index = add_entity(file, EntityKind::Marker, out.type + kItemIdOffset, at);
	place(file.markers[index]);
	out.moved = 1;
	out.added = true;
	return true;
}

} // namespace opennova::mission
