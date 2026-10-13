#pragma once

// The world's scene stack and the load that fills it: the environment, sky,
// weather, water, terrain and their presenters game_world.tscn declares, as
// ONE table of rows (the name, the class, the paths each takes, the capture
// layer), and what a mission's load does over them (the .trn, overcast.def
// and the .env with the load's post-steps, the mission header's overrides and
// clock, the weather's start, the terrain's binding and its foliage). GameWorld
// binds the rows its packaged scene holds; a device that draws a mission's
// world without one (the editor's previews) creates the rows it draws from the
// same table, so neither can leave a row or a path out. Device work (ADR 0043
// d9): every witnessed rule stays in engine/ (env::load_mission_env, the water
// ladder, the placed-tiles read), cited there.

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class Celestial;
class EnvironmentCubeCapture;
class FoliageDispatcher;
class FrameFx;
class MissionEnvironment;
class MissionEnvironmentOverrides;
class Precipitation;
class ResourceRoot;
class SkyDome;
class SlotShadow;
class SunShadow;
class Terrain;
class TerrainData;
class TerrainTileInfo;
class Water;
class Weather;
class WorldEnvironment;

// One row per node, in game_world.tscn's order.
enum class WorldSceneRow : uint8_t {
	ClearColor,
	FrameFx,
	Terrain,
	Foliage,
	Environment,
	Sky,
	Weather,
	Precipitation,
	Water,
	EnvironmentCube,
	Celestial,
	SunShadow,
	SlotShadow,
	Count,
};
inline constexpr size_t kWorldSceneRowCount = static_cast<size_t>(WorldSceneRow::Count);
using WorldSceneRows = std::bitset<kWorldSceneRowCount>;

// The stack's nodes; null where a row is absent (a code-built world, a device
// that draws without it).
struct WorldSceneNodes {
	WorldEnvironment *clear_color = nullptr;
	FrameFx *framefx = nullptr;
	Terrain *terrain = nullptr;
	FoliageDispatcher *foliage = nullptr;
	MissionEnvironment *environment = nullptr;
	SkyDome *sky = nullptr;
	Weather *weather = nullptr;
	Precipitation *precipitation = nullptr;
	Water *water = nullptr;
	EnvironmentCubeCapture *environment_cube = nullptr;
	Celestial *celestial = nullptr;
	SunShadow *sun_shadow = nullptr;
	SlotShadow *slot_shadow = nullptr;
};

// What the environment's load found, for its caller to word in its own
// channel (the game's missing-resource rows, a device's notes).
struct WorldEnvironmentLoad {
	bool loaded = false; // the .env read and parsed over the earlier passes
	bool environment_missing = false; // named and not there (one that does not parse is no miss)
	bool overcast_missing = false; // overcast.def not there
};

class WorldScene {
public:
	// Every row.
	static WorldSceneRows all_rows();
	// The table as data (each row's name, class, its NodePath properties and
	// its capture layer, in order): what a test pins game_world.tscn against.
	static Array describe_rows();

	// The rows a packaged scene holds, found by their names under `world`, then
	// wired (GameWorld::_ready over game_world.tscn).
	static WorldScene bind(Node &p_world);
	// The rows `rows` names made under `parent`, named, ordered and pathed as
	// the table says (a path only to a row made with it), then wired as bind
	// wires them. `held_globals`: the environment, the water, the weather and
	// the cube capture start with their process-wide globals held (ADR 0046
	// S14 E13, a device that shares the process with other pictures).
	static WorldScene create(Node &p_parent, WorldSceneRows p_rows, bool p_held_globals);

	const WorldSceneNodes &nodes() const { return nodes_; }

	// GameWorld::load_environment: the terrain's .trn, overcast.def, then the
	// .env over them (a .env that is not there is skipped, the earlier passes
	// over the engine's defaults standing), the overcast table beside it; then
	// the load's post-steps: the weather snaps its colours to the new targets,
	// the bodies and the drops read through `root`, FrameFX draws its mission
	// texture, the cube captures again. The environment row is required.
	WorldEnvironmentLoad load_environment(const Ref<ResourceRoot> &p_root, const String &p_trn_path,
			const String &p_env_path) const;
	// The mission header's attrib-gated overrides over the loaded environment
	// (null clears them) and the water's own rung for the header's height.
	void apply_mission_overrides(const Ref<MissionEnvironmentOverrides> &p_overrides) const;
	void set_mission_water_height_override(float p_world_height) const;
	// The mission's clock: the header's Q8.8 start hour and its day length.
	void configure_mission_clock(int p_start_q8_8, int p_minutes_per_day) const;
	// The weather's three starts: a mission whose world ticks it, a world with
	// no mission (its own home), and the start boundary with no simulation
	// (the seed, the initializer and the 255-tick settle a mission's start
	// runs, Weather::run_mission_start_boundary).
	void prepare_world_driven_weather() const;
	void prepare_autonomous_weather() const;
	void run_mission_start_boundary() const;

	// A loaded terrain bound to every row that reads it (GameWorld::load_terrain):
	// the terrain (its placed tiles first), the slot shadows' anchor march, the
	// water's terrain rung, the cube's clamp and the glare's occlusion rays.
	// The terrain's build is the caller's (whole or stepped).
	void bind_terrain(const Ref<TerrainData> &p_data, const Ref<TerrainTileInfo> &p_tiles) const;
	// No terrain: every row lets go of it.
	void clear_terrain() const;
	// The foliage as the load configures it after the terrain's build: the
	// terrain's detail cells and foliage map, its placed tiles as the candidate
	// blocker, each definition's model and :fd texture through `root`. Returns
	// the slot diagnostics (FoliageDispatcher::get_slot_diagnostics).
	Array configure_foliage(const Ref<ResourceRoot> &p_root, const Ref<TerrainData> &p_data) const;

	// The hold (ADR 0046 S14, E13) over every process-wide write the rows make:
	// the environment's shader globals, the water's plane, the weather's
	// globals and model rings, the cube's texture.
	void hold_globals(bool p_held) const;
	// Every such global written again from this scene's state, whatever the
	// hold (another picture published its own since).
	void publish_globals() const;

private:
	void wire_() const;

	WorldSceneNodes nodes_;
};

// A TerrainData about to load a mission's terrain: the mission's tile set and
// its .env (`environment_file`, "<name>.env", "" none), whose terrain lines
// the terrain's parser takes after the .trn's (D-TERRAIN-18). Every
// TerrainData load of a mission's terrain goes through it.
void prepare_terrain_load(
		TerrainData &p_data, const String &p_tile_set, const String &p_environment_file);

// The placed tiles a mission's load takes (mission::read_placed_tiles: the
// mission's <mission>.til, else the terrain's own polytrn_tileinfo), parsed;
// null when none loads or the one taken does not parse. `terrain` and
// `environment` are the header's names without extensions. `r_read`
// (optional) says which file was taken (empty: none), whether it is the
// terrain's own, and its bytes.
struct MissionTilesRead {
	std::string file;
	bool terrain_own = false;
	PackedByteArray bytes;
};
Ref<TerrainTileInfo> read_mission_tiles(const Ref<ResourceRoot> &p_root,
		const std::string &p_mission_file, const std::string &p_terrain,
		const std::string &p_environment, MissionTilesRead *r_read = nullptr);

} // namespace godot
