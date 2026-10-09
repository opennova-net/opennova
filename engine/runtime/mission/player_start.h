#pragma once

// Where the game's player starts, a point of the mission and the heading it faces, and the mission
// edit that puts it there. No flag of the stock game's command line names a mission or a place [orig:
// Game_ParseCommandLineAndInit @ 0x4a7310, its argument walk @ 0x4a73e9..0x4a7bef]: the game deploys
// its player at a start marker of the mission [orig: Server_PositionPlayerForSpawn @ 0x50CF60;
// runtime/world/spawn_select.h], so a start is given by moving the mission's start markers to the
// point, and each engine places its player there by its own spawn selection, the stock game and
// OpenNova alike (the editor's Play from here stages such a copy of the mission).

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::bms {
struct File;
}

namespace opennova::mission {

// A player start: whether one is given at all, the point and the heading.
struct PlayerStart {
	bool set = false;
	double at[3] = { 0.0, 0.0, 0.0 }; // mission metres: x east, y north, z up (the ground's height there)
	double yaw = 0.0;                  // compass degrees, as an entity's yaw (0 north, 90 east)
};

inline bool operator==(const PlayerStart &a, const PlayerStart &b) {
	return a.set == b.set && (!a.set || (a.at[0] == b.at[0] && a.at[1] == b.at[1] && a.at[2] == b.at[2] && a.yaw == b.yaw));
}
inline bool operator!=(const PlayerStart &a, const PlayerStart &b) {
	return !(a == b);
}

// How a mission's start was placed: the start marker type the game's single player deploys at under
// the mission's mode (world::start_marker_types for team 1, the single player's), how many of that type
// stand at the point now (every one: the game picks among them by the player's slot), and whether one
// was added (the mission had none of the primary type nor the fallback).
struct PlayerStartPlaced {
	int32_t type = 0;
	size_t moved = 0;
	bool added = false;
};

// Degrees in 0..359, as the file stores a yaw.
int16_t wrapped_yaw(double degrees);

// The mission's player start set to `start`: every start marker of the type the single player
// deploys at (the mode's primary when the mission holds one, else its fallback) moved to the point,
// facing the heading, its pitch and roll level, and a team-2 marker's team made 1 (the Co-op arm
// queues a mount onto a team-2 marker's carrier [orig: Server_PositionPlayerForSpawn @ 0x50D42E]: a
// start of its own stands on foot); none of either type, one of the primary type added at the point
// (bms_edit's new record, its SSN the next). False with `error` for a mode with no start (a team mode
// whose team 1 has none) or a point past what the file's positions hold.
bool place_player_start(bms::File &file, const PlayerStart &start, PlayerStartPlaced &out, std::string &error);

} // namespace opennova::mission
