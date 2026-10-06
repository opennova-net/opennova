#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <editor/model/diagnostic.h>

namespace opennova::bms {
struct File;
}

namespace opennova::editor {

// Play from here (DI-26): where the game's player starts, a point of the mission and the heading it
// faces. No flag of the stock game's command line names a mission or a place [orig:
// Game_ParseCommandLineAndInit @ 0x4a7310, its argument walk @ 0x4a73e9..0x4a7bef]: the game deploys
// its player at a start marker of the mission [orig: Server_PositionPlayerForSpawn @ 0x50CF60;
// runtime/world/spawn_select.h], so Play from here gives it one. The staged build's copy of the
// mission (in the run directory, never the project's file nor the build's) has its start markers at
// the point, and each engine places its player there by its own spawn selection, the stock game and
// OpenNova alike.
struct PlayStart {
	bool set = false;
	double at[3] = { 0.0, 0.0, 0.0 }; // mission metres: x east, y north, z up (the ground's height there)
	double yaw = 0.0;                  // compass degrees, as an entity's yaw (0 north, 90 east)
};

inline bool operator==(const PlayStart &a, const PlayStart &b) {
	return a.set == b.set && (!a.set || (a.at[0] == b.at[0] && a.at[1] == b.at[1] && a.at[2] == b.at[2] && a.yaw == b.yaw));
}
inline bool operator!=(const PlayStart &a, const PlayStart &b) {
	return !(a == b);
}

// How a staged mission's start was made: the start marker type the game's single player deploys at
// under the mission's mode (world::start_marker_types for team 1, the single player's), how many of
// that type stand at the point now (every one: the game picks among them by the player's slot), whether
// one was added (the mission had none of the primary type nor the fallback), and the run directory's
// archive written with it ('/'-separated under the run directory).
struct PlayStartPlaced {
	int32_t type = 0;
	size_t moved = 0;
	bool added = false;
	std::string archive;
};

// The mission's player start set to `start`: every start marker of the type the single player
// deploys at (the mode's primary when the mission holds one, else its fallback) moved to the point,
// facing the heading, its pitch and roll level, and a team-2 marker's team made 1 (the Co-op arm
// queues a mount onto a team-2 marker's carrier [orig: Server_PositionPlayerForSpawn @ 0x50D42E]: a
// start of its own stands on foot); none of either type, one of the primary type added at the point
// (bms_edit's new record, its SSN the next). False with `error` for a mode with no start (a team mode
// whose team 1 has none) or a point past what the file's positions hold.
bool place_player_start(bms::File &file, const PlayStart &start, PlayStartPlaced &out, std::string &error);

// Play from here in a staged run directory: the archive that serves `mission` there (the boot table
// in its slot order, an expansion's two archives first [orig: PFF_OpenAllArchives @ 0x4a4310; slot
// order is lookup precedence, and a mission is read from the archives whatever /d says [orig:
// Mission_LoadBMSFromPFF @ 0x40d43c]) written again in the run directory with the mission's start
// placed (place_player_start), every other entry as the archive held it. The archive's name in the
// run directory, a link to the build's file, is replaced, never written through: the build and the
// project stay as they were. False with `error` (play.start): no archive there serves the mission,
// the mission does not read, the start cannot be placed, or the archive cannot be written.
bool stage_play_start(const std::string &run_dir, const std::string &expansion, const std::string &mission,
		const PlayStart &start, PlayStartPlaced &out, Diagnostic &error);

// "(412.0, -88.5, 36.2) facing 270", for Output.
std::string play_start_words(const PlayStart &start);

} // namespace opennova::editor
