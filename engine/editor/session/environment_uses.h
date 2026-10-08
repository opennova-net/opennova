#pragma once

#include <string>
#include <vector>

#include <base/io/json.h>
#include <formats/env/env.h>
#include <formats/trn/trn_io.h>

namespace opennova::mission {
struct MissionInfo;
}

namespace opennova::editor {

struct GraphEdge;
struct SessionView;

// A mission's header facts as the game reads them now: from its open document's record, else from the 616
// bytes its file starts with (the header the game reads first [orig: Mission_LoadBMSFile, bms.h Header]).
// False where neither reads.
bool read_mission_header(const SessionView &view, const std::string &path, mission::MissionInfo &out);

// Which of the game's rungs gives a mission its water plane (runtime/environment/water_frame.h's
// ladder): the mission header's override, the terrain's water height, the environment's, or none.
enum class WaterFrom { Mission, Terrain, Environment, None };
const char *water_from_token(WaterFrom from);

// A line of an environment its mission's terrain takes after its .trn's and overcast.def's (D-TERRAIN-18,
// formats/trn load_mission_trn): the line (`line` in the environment's text as the game reads it now, the open
// document's save standing in), its place among the environment's terrain keys (the document's list:
// EnvironmentDocument::terrain_key_address), and what it sets over: the value its keyword held before it in the
// load, as a line writes it ("" for a keyword that adds, a foliage block's or a grid row, or holds no such value),
// and the file whose line set that value (the terrain's .trn, overcast.def, this environment; "" the load's own
// default, no line before it naming one).
struct EnvironmentTerrainKey : TrnLaterLine {
	size_t index = 0;
	std::string over;
	std::string over_file;
};

// A mission that runs on an environment (the deep-integration plan's DI-19a), as the game loads
// the two together [orig: Game_StartMission @ 0x524B26 -> Game_LoadTerrainDuringConnect @ 0x520710 ->
// Terrain_LoadEnvironmentConfig @ 0x610940]: the header that names it, the terrain it pairs it with,
// what the header sets over it (the fog, the water [orig: Game_LoadTerrainDuringConnect @ 0x520710;
// Game_StartMission @ 0x525371..0x525399]) and the clock it starts on [orig: Game_StartMission
// @ 0x5253d5, @ 0x5253e2], and where its water plane comes from.
struct EnvironmentMissionUse {
	const GraphEdge *edge = nullptr; // the header's environment field: where Go to opens the mission
	std::string mission;             // project-relative
	std::string title;               // the mission's name as its header holds it
	bool read = false;               // its header was read
	const GraphEdge *terrain_edge = nullptr; // the header's terrain field
	std::string terrain;                     // as the header names it
	std::string terrain_file;                // project-relative; "" where the project lacks it
	bool terrain_read = false;
	int terrain_water = 0;            // the terrain's water_height, half metres (0: none)
	env::BmsEnvOverrides overrides;   // what the header sets over the environment
	int start_time = 0;               // the header's start time, 8.8 hours
	int minutes_per_day = 0;          // the header's day length (0: the clock stands)
	// The header's fields the game reads beside the environment, as written (DI-19b's environment
	// viewport loads the mission's terrain, its tiles and its overrides as the mission device does):
	// the tile set, the attribute flags that gate the overrides, the water and fog overrides, the fog
	// and water colours, the murk.
	std::string tile_set;
	uint32_t attrib_flags = 0;
	int water_override = 0, fog_override = 0, water_murk = 0;
	int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
	WaterFrom water_from = WaterFrom::None;
	float water_height = 0.0f;        // metres
	// The lines of this environment the mission's terrain's parser takes after its .trn's and overcast.def's
	// (D-TERRAIN-18, formats/trn load_mission_trn): each key the terrain's, over theirs.
	std::vector<EnvironmentTerrainKey> terrain_keys;
};

// The missions of the project that run on the environment at `path`, by the graph's edges into it
// (each a mission header's environment field), each with its header read from its open document or
// its file (the project's files, the open documents standing in) and its terrain's water height.
struct EnvironmentUses {
	std::string path;
	bool found = false;   // the project has the file
	bool reading = false; // the project's references are not read yet
	std::vector<EnvironmentMissionUse> missions;
};
EnvironmentUses environment_uses(const SessionView &view, const std::string &path);

// The fields of a mission's header each override names (mission_table's keys), for a Go to on it.
struct EnvironmentOverride {
	const char *field = ""; // the header's field
	std::string words;      // "fog 800 m"
};
std::vector<EnvironmentOverride> environment_overrides(const EnvironmentMissionUse &use);
// "06:00, a day of 30 min": the clock a mission starts on, in words.
std::string mission_clock_words(const EnvironmentMissionUse &use);
// "at 12.5 m, from the terrain": the water plane's height and where it comes from, in words.
std::string water_words(const EnvironmentMissionUse &use);

// The environment_uses query's answer: {path, found, reading?, missions [{mission, title, locator,
// field, terrain {name, file?, water_height?}, overrides [{field, words}], start_time, minutes_per_day,
// clock, water {from, height}, terrain_keys [{line, index, key, value, over?, over_file?}]}]}; a mission's file and
// locator are where Go to opens it, and an override's field the header's field there.
io::JsonValue environment_uses_json(const EnvironmentUses &uses);

} // namespace opennova::editor
