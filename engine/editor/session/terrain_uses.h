#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

struct GraphEdge;
struct SessionView;

// A mission that runs on a terrain (the deep-integration plan's DI-30), as the game loads the two together
// [orig: Game_StartMission @ 0x524B26 -> Game_LoadTerrainDuringConnect @ 0x520710 -> Terrain_LoadEnvironmentConfig
// @ 0x610940]: the header that names it, and what the header loads beside it: the environment, the tile set
// that replaces the terrain's own [orig: Terrain_LoadEnvironmentConfig @ 0x6109C8], the overrides over the
// environment [orig: Game_LoadTerrainDuringConnect @ 0x520710; Game_StartMission @ 0x525371..0x525399], the
// clock it starts on and its own tile placement (<mission>.til, read before the terrain's [orig: Terrain_Init
// @ 0x60FCFD]). What the terrain viewport draws the terrain with where it draws it under a mission.
struct TerrainMissionUse {
	const GraphEdge *edge = nullptr; // the header's terrain field: where Go to opens the mission
	std::string mission;             // project-relative
	std::string name;                // its logical name without the extension (what <mission>.til is named by)
	std::string title;               // the mission's name as its header holds it
	bool read = false;               // its header was read
	std::string environment;         // as the header names it
	std::string environment_file;    // project-relative; "" where the project lacks it
	std::string tile_set;            // as the header names it ("" the terrain's own)
	bool tiles = false;              // the project has <mission>.til
	uint32_t attrib_flags = 0;
	int water_override = 0, fog_override = 0, water_murk = 0;
	int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
	int start_time = 0;      // 8.8 hours
	int minutes_per_day = 0; // 0: the clock stands
	// The lines of overcast.def and of its environment the terrain's parser takes after this terrain's lines
	// (D-TERRAIN-18, formats/trn load_mission_trn): each key there is the mission's terrain's, over this file's.
	std::vector<TrnLaterLine> later;
};

// One image a terrain set names (S20, terrain_import.h's keys), and the project's file at it.
struct TerrainSetImage {
	std::string key;  // heightmap, colormap, detail, tiles, surface, foliagemap
	std::string name; // as the set writes it, from its folder
	std::string file; // project-relative ("" where the project lacks it)
};

// One option of the terrain importer as the import's record holds it (its row's fallback where it holds none).
struct TerrainImportOption {
	std::string key, label, value, words;
	bool set = false; // the record holds it
};

// The import that makes a terrain (S20): the terrain set it is an output of, the images the set names, its
// foliage definitions, the options, and the files the import made. What the terrain document shows of a
// terrain it does not edit (DocumentSet: document.imported) and what its Reimport imports again.
struct TerrainImport {
	bool imported = false;   // the terrain is an import's output
	std::string source;      // the set, project-relative
	std::string record;      // its .import, project-relative
	std::string error;       // why the set or its record does not read ("" they do)
	std::vector<TerrainSetImage> images;
	size_t foliage = 0;      // the set's foliage blocks
	std::vector<TerrainImportOption> options;
	std::vector<std::string> outputs; // project-relative
};

// The missions of the project that run on the terrain at `path` (by the graph's edges into it, each a mission
// header's terrain field, its header read from its open document or its file), and the import it comes from.
struct TerrainUses {
	std::string path;
	bool found = false;   // the project has the file
	bool reading = false; // the project's references are not read yet
	std::string overcast_file; // the project's overcast.def ("" none), which every mission's terrain reads after it
	std::vector<TerrainMissionUse> missions;
	TerrainImport import;
};
TerrainUses terrain_uses(const SessionView &view, const std::string &path);

// The terrain_uses query's answer: {path, found, reading?, overcast?, missions [{mission, name, title, read, locator,
// field, environment {name, file?}, tile_set, tiles, start_time, minutes_per_day, later [{file, line, key, value}]}],
// import: null or {source, record, error?, images [{key, name, file?}], foliage, options [{key, label, value, set,
// words}], outputs, reimport (the request that imports it again)}}.
io::JsonValue terrain_uses_json(const TerrainUses &uses);

} // namespace opennova::editor
