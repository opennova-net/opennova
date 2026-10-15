#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <editor/model/diagnostic.h>
#include <runtime/mission/player_start.h>

namespace opennova::editor {

// Play from here (DI-26): where the game's player starts, a point of the mission and the heading it
// faces (mission::PlayerStart, runtime/mission/player_start.h), which the staged build's copy of the
// mission (in the run directory, never the project's file nor the build's) holds as its start markers
// at the point (mission::place_player_start); each engine places its player there by its own spawn
// selection, the stock game and OpenNova alike.

// How a staged mission's start was made: the engine's placement (the start marker type, how many stand
// at the point, whether one was added), and the run directory's archive written with it ('/'-separated
// under the run directory).
struct PlayStartPlaced : mission::PlayerStartPlaced {
	std::string archive;
};

// Play from here in a staged run directory: the archive that serves `mission` there (the boot table
// in its slot order, an expansion's two archives first [orig: PFF_OpenAllArchives @ 0x4a4310; slot
// order is lookup precedence, and a mission is read from the archives whatever /d says [orig:
// Mission_LoadBMSFromPFF @ 0x40d43c]) written again in the run directory with the mission's start
// placed (place_player_start), every other entry as the archive held it. The archive's name in the
// run directory, a link to the build's file, is replaced, never written through: the build and the
// project stay as they were. False with `error` (play.start): no archive there serves the mission,
// the mission does not read, the start cannot be placed, or the archive cannot be written.
bool stage_play_start(const std::string &run_dir, const std::string &expansion, const std::string &mission,
		const mission::PlayerStart &start, PlayStartPlaced &out, Diagnostic &error);

// "(412.0, -88.5, 36.2) facing 270", for Output.
std::string play_start_words(const mission::PlayerStart &start);

} // namespace opennova::editor
