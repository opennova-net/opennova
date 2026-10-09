// A player start placed in a mission (runtime/mission/player_start.h). The single player deploys at the
// first start marker type its mode's chain holds (world::start_marker_types, team 1): every marker of that
// type moves to the point, facing the heading, a team-2 marker made team 1 (no queued mount), every other
// marker as it was; none of either type, one of the primary is added; a point past the 16.16 positions is
// refused.
#include <string>

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <runtime/mission/player_start.h>

#include "common/test_expect.h"

using namespace opennova;
using mission::PlayerStart;
using mission::PlayerStartPlaced;

static int test_place_player_start() {
	const auto markers_at = [](bms::File &file, int item_id, int count) {
		mission::EntityTransform at;
		at.x = 10.0f;
		at.y = 20.0f;
		at.z = 3.0f;
		at.yaw = 45;
		for (int i = 0; i < count; ++i) mission::add_entity(file, mission::EntityKind::Marker, item_id, at);
	};
	PlayerStart start;
	start.set = true;
	start.at[0] = 412.5;
	start.at[1] = -88.25;
	start.at[2] = 36.0;
	start.yaw = -90.0;
	const auto at_start = [&start](const bms::Entity &marker) {
		return marker.x == bms::to_fixed_16_16(start.at[0]) && marker.y == bms::to_fixed_16_16(start.at[1]) &&
		       marker.z == bms::to_fixed_16_16(start.at[2]) && marker.yaw == 270 && marker.pitch == 0 && marker.roll == 0;
	};
	PlayerStartPlaced placed;
	std::string error;

	// Co-op (no mode bit, as single player): the insertion points 6094, both; the fallback and a waypoint stay.
	bms::File coop;
	mission::make_default(coop);
	markers_at(coop, 106094, 2);
	markers_at(coop, 106001, 1);
	markers_at(coop, 106000, 1);
	coop.markers[1].team = 2;
	TEST_EXPECT(mission::place_player_start(coop, start, placed, error));
	TEST_EXPECT(placed.type == 6094 && placed.moved == 2 && !placed.added && coop.markers.size() == 4);
	TEST_EXPECT(at_start(coop.markers[0]) && at_start(coop.markers[1]) && coop.markers[1].team == 1);
	TEST_EXPECT(!at_start(coop.markers[2]) && coop.markers[2].x == bms::to_fixed_16_16(10.0) && !at_start(coop.markers[3]));

	// Only the fallback: the 6001s.
	bms::File fallback;
	mission::make_default(fallback);
	markers_at(fallback, 106001, 2);
	TEST_EXPECT(mission::place_player_start(fallback, start, placed, error));
	TEST_EXPECT(placed.type == 6001 && placed.moved == 2 && !placed.added && at_start(fallback.markers[1]));

	// None: an insertion point added at the point, its SSN the next.
	bms::File none;
	mission::make_default(none);
	const int ssn = mission::next_entity_ssn(none);
	TEST_EXPECT(mission::place_player_start(none, start, placed, error));
	TEST_EXPECT(placed.type == 6094 && placed.moved == 1 && placed.added && none.markers.size() == 1 &&
	            none.markers[0].type_id == 6094 && none.markers[0].id == ssn && at_start(none.markers[0]));

	// Deathmatch: its chain's 6095, the Co-op starts left.
	bms::File deathmatch;
	mission::make_default(deathmatch);
	deathmatch.header.attrib_flags = bms::AttribFlags::Deathmatch;
	markers_at(deathmatch, 106094, 1);
	markers_at(deathmatch, 106095, 1);
	TEST_EXPECT(mission::place_player_start(deathmatch, start, placed, error));
	TEST_EXPECT(placed.type == 6095 && placed.moved == 1 && !at_start(deathmatch.markers[0]) && at_start(deathmatch.markers[1]));

	// Past what a position holds.
	PlayerStart far = start;
	far.at[0] = 40000.0;
	TEST_EXPECT(!mission::place_player_start(coop, far, placed, error) && error.find("32,768") != std::string::npos);
	return 0;
}

// A yaw in the 0..359 degrees the file stores; a start equal to another by its point and heading, any
// unset start equal to any other.
static int test_start_values() {
	TEST_EXPECT(mission::wrapped_yaw(-90.0) == 270 && mission::wrapped_yaw(360.0) == 0 && mission::wrapped_yaw(719.6) == 0 &&
	            mission::wrapped_yaw(45.4) == 45 && mission::wrapped_yaw(-0.4) == 0);
	PlayerStart a, b;
	a.at[0] = 5.0;
	TEST_EXPECT(a == b);
	a.set = b.set = true;
	TEST_EXPECT(a != b);
	b.at[0] = 5.0;
	TEST_EXPECT(a == b);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_place_player_start();
	failures += test_start_values();
	if (failures == 0) std::printf("player_start_test OK\n");
	return failures == 0 ? 0 : 1;
}
