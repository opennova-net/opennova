// The synthetic in-memory missions, with no retail data: the one the tick
// digest chains and the tick-profile row test ticks (two opposing squads of
// six organics, two items and one BMS event), and the two-entity one the
// mission kernel, kernel lifecycle and host-role tests boot. Boot either
// through MissionKernel::open_document with a source_over(...) that supplies
// "synth.wac" when the caller wants a WAC layer.
#pragma once

#include <formats/mission/bms.h>

#include <cstdint>

namespace test_mission {

inline opennova::bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team, int32_t yaw) {
	opennova::bms::Entity e{};
	e.type = opennova::bms::ItemType::Organic;
	e.x = x;
	e.y = y;
	e.z = z;
	e.yaw = yaw;
	e.team = team;
	return e;
}

inline opennova::bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
	opennova::bms::Entity e{};
	e.type = opennova::bms::ItemType::Item;
	e.type_id = type_id;
	e.x = x;
	e.y = y;
	e.z = z;
	return e;
}

// Two opposing squads plus a few items. The kernel's own player is the one
// human the bare tick counts, so neither the WAC tick nor the entity update
// holds.
inline opennova::bms::File synthetic_mission() {
	opennova::bms::File m{};
	int32_t next_id = 20;
	for (int i = 0; i < 6; ++i) {
		m.organics.push_back(organic((10 + i * 4) << 16, 10 << 16, 0, /*team=*/1, 90));
		m.organics.back().id = next_id++;
	}
	for (int i = 0; i < 6; ++i) {
		m.organics.push_back(organic((10 + i * 4) << 16, 60 << 16, 0, /*team=*/2, 270));
		m.organics.back().id = next_id++;
	}
	m.items.push_back(item(/*type_id=*/164, 30 << 16, 35 << 16, 3 << 16));
	m.items.back().id = next_id++;
	m.items.push_back(item(/*type_id=*/164, 40 << 16, 35 << 16, 3 << 16));
	m.items.back().id = next_id++;
	m.events.push_back(opennova::bms::Event{});
	return m;
}

// Two placed entities (item 164 net id 21, a team-1 organic net id 31) and
// one (empty) BMS event.
inline opennova::bms::File two_entity_mission() {
	opennova::bms::File m{};
	m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
	m.items[0].id = 21;
	m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1, /*yaw=*/90));
	m.organics[0].id = 31;
	m.events.push_back(opennova::bms::Event{});
	return m;
}

}  // namespace test_mission
