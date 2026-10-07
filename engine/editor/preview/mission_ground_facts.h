#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/model/node.h>
#include <editor/preview/viewport_follow.h>
#include <formats/foliage/foliage.h>
#include <formats/pcx/pcx.h>
#include <formats/til/til.h>
#include <runtime/terrain_query/foliage_mask_map.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/terrain_query/terrain_field_store.h>

namespace opennova::editor {

struct MissionSceneHeader;

// What decides the surface class at a point, by the legs of the game's own sampler
// (terrain::surface_type_at_fixed [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510]).
enum class MissionSurfaceFrom : uint8_t {
	NoTerrain, // no terrain read: nothing to say
	NoMap, // the terrain names no char map, or it does not read: class 1 everywhere
	Ocean, // a sector the terrain's grid leaves empty: 7, the off-island ocean
	Map, // the char map's texel there
	Tile, // a placed tile's square (the mission's .til): its tile set's table entry, 0 (TSD_NULL) with no .tsd
};
// "no_terrain", "no_map", "ocean", "map", "tile".
const char *mission_surface_from_token(MissionSurfaceFrom from);

// What the ground under a point is: nothing (the sky, or no device to say), the terrain, or a placed
// entity's drawn surface.
enum class MissionGroundOn : uint8_t { Nothing, Terrain, Record };
// "nothing", "terrain", "record".
const char *mission_ground_on_token(MissionGroundOn on);

// A foliage definition the terrain's foliage map selects at a point (DI-29): its slot (0 to 3, the .trn's
// foliage block order) and the model it places.
struct MissionFoliageAt {
	int slot = 0;
	std::string graphic;
};

// The ground at a point in the game's words (DI-07): the surface class the game reads there, the
// footstep slots a body standing there plays (audio::footstep_slot: under the water plane the water's,
// on an entity the object's, on surface 3 the snow's, else the ground's), and the ammo effects row a
// round striking it plays (world::terrain_impact_effect_tag: the class + 4; the water's row where the
// plane is crossed first). DI-29: the terrain's foliage there, the code its foliage map holds and the
// definitions that grow on it, those the placed tiles keep off apart.
struct MissionGroundFacts {
	MissionGroundOn on = MissionGroundOn::Nothing;
	double at[3] = { 0.0, 0.0, 0.0 }; // where, mission metres (x east, y north, z up)
	NodeId row = 0; // Record: the entity's row
	std::string record; // Record: its title
	int surface = -1; // the class the game reads at (x, y) (-1: no terrain)
	MissionSurfaceFrom from = MissionSurfaceFrom::NoTerrain;
	int map_surface = -1; // Tile: the char map's class under the tile
	int tile = -1; // Tile: the tile's index in its set
	bool water = false; // the mission has a water plane
	double water_height = 0.0; // its height, metres
	bool under_water = false; // the point lies under it
	int footstep[2] = { -1, -1 }; // the sound-profile slots, left and right foot (-1: nothing)
	int impact_row = -1; // the effects row a round plays there (-1: a record's, the material it strikes)
	// Terrain: the foliage map's code there (-1: the terrain names no foliage map, or no terrain; an empty
	// sector reads none), what grows on it, and what a placed tile's square keeps off it (no forceon).
	int foliage_code = -1;
	std::vector<MissionFoliageAt> grows;
	std::vector<MissionFoliageAt> kept_off;
};

// A surface class in a modder's words ("Snow"; "Surface 31" past the twenty) and in the game's own
// (the tile set table's name, "TSD_SNOW"; "" past them).
std::string mission_surface_words(int surface);
const char *mission_surface_name(int surface);
// The char map legend's colour of a class, "#CCEFF4" (formats/trn/charmap_legend.h; "" past it).
std::string mission_surface_colour(int surface);

// What the line under the mission's picture says of the ground under the pointer ("" for nothing).
std::string mission_ground_line(const MissionGroundFacts &facts);
// Its wire form (the viewport's hit): {on, at, row, record, surface, name, words, colour, from, map_surface,
// tile, water, water_height, under_water, footstep {slots, names, words}, impact_row {row, tag, words},
// foliage {code, grows: [{slot, graphic}], kept_off} (null where the terrain names no foliage map), line};
// {on: "nothing"} alone for nothing.
io::JsonValue mission_ground_to_json(const MissionGroundFacts &facts);

// The ground of one mission as the game reads it (DI-07): its terrain through the game's own load
// (terrain_field_store_load: the .trn, the .cpt and the char map the .trn names), the mission's placed
// tiles (<mission>.til, the game's surface_tiles_from_til_bytes) and its tile set's table (the .tsd beside
// the tile strip the mission's tile set names, resolve_tileset_surface_table), and the water plane the
// game resolves (env::resolve_water_height: the mission's override, the terrain's, the environment's).
// Read when first asked, and again where the header's terrain, tile set, environment or water override or
// the mission's name moved, or (the files' generation having moved) a file it read moved its stamp.
class MissionGround {
public:
	void follow(const std::shared_ptr<const FileSource> &files, uint64_t generation, const MissionSceneHeader &header,
			const std::string &mission);
	// A terrain read, and why not ("" before the first follow).
	bool terrain() const { return store_.valid(); }
	// The terrain's heights as the game's collision reads them (null: none read).
	const terrain::TerrainHeightField *height_field() const { return store_.valid() ? &store_.height_field() : nullptr; }
	const std::string &error() const { return error_; }
	// The char map read ("" none: class 1 everywhere), the placed tiles and the file they were read from (the
	// mission's <mission>.til, else the terrain's own polytrn_tileinfo; "" none), the water plane.
	const std::string &surface_map() const { return surface_map_; }
	size_t tiles() const { return tiles_.size(); }
	const std::string &tiles_file() const { return tiles_file_; }
	bool water() const { return water_z_ != 0; }
	double water_height() const;
	// How often it read its files (a test's count).
	int reads() const { return reads_; }

	// The game's own samplers over what it read (DI-29): the char map's, with the placed tiles and their
	// table (terrain::surface_type_at_fixed), and the foliage map's, its codes (`codes` true) or its texels
	// remapped to the definitions they select (terrain::foliage_mask_at_fixed); zeroed where nothing was read.
	terrain::SurfaceTypeMap surface_sampler() const;
	terrain::FoliageMaskMap foliage_sampler(bool codes) const;
	// The char map's side (0 none) and the foliage map read ("" none), its codes as read (its side, its
	// palette), and the .trn's foliage definitions (its slots).
	int surface_side() const { return store_.valid() ? store_.surface_map().width : 0; }
	const std::string &foliage_map() const { return foliage_map_; }
	const IndexedImage8 &foliage_codes() const { return foliage_codes_; }
	const std::vector<FoliageDef> &foliage_defs() const { return foliage_defs_; }
	// The placed tiles' squares, as the game keeps them (x and z 16.16, z the negated mission y).
	const std::vector<terrain::SurfaceTileEntry> &placed_tiles() const { return tiles_; }
	// The definition slots (a mask) a placed tile's square keeps a candidate at mission (x, y) off: every
	// slot without forceon where the candidate's square (2 units each way) meets a tile's [orig:
	// Foliage_PathBlockedByPlacedTile @ 0x606490; til_blocks_foliage], 0 for none.
	int foliage_kept_off(double x, double y, int mask) const;

	// The terrain at mission (x, y), a body's feet at height z.
	MissionGroundFacts terrain_at(double x, double y, double z) const;
	// A body standing on an entity's surface at (x, y, z), the entity's row and title.
	MissionGroundFacts record_at(NodeId row, const std::string &title, double x, double y, double z) const;

private:
	struct Key {
		std::string terrain, tile_set, environment, mission;
		uint32_t water_attrib = 0;
		int water_override = 0;
		bool operator==(const Key &other) const;
	};
	void read_(const std::shared_ptr<const FileSource> &files, const MissionSceneHeader &header);
	// The footstep slots and the impact row at height z over a class (on: a record's).
	void foot_(MissionGroundFacts &facts, bool on_record) const;

	bool read_once_ = false;
	Key key_;
	uint64_t generation_ = 0;
	FileStamps stamps_;
	terrain::TerrainFieldStore store_;
	std::string error_;
	std::string surface_map_;
	std::vector<terrain::SurfaceTileEntry> tiles_;
	std::string tiles_file_;
	TilFile placed_; // the same placements as the foliage blocker reads them
	std::array<uint8_t, 256> tile_surface_{};
	int32_t water_z_ = 0; // 16.16, 0 none (the game's g_EnvWaterHeightFixed)
	// The foliage map as read (its indices the codes) and as the game keeps it (each texel the definition
	// slots it selects), and the definitions.
	std::string foliage_map_;
	IndexedImage8 foliage_codes_;
	std::vector<uint8_t> foliage_masks_;
	std::vector<FoliageDef> foliage_defs_;
	int reads_ = 0;
};

} // namespace opennova::editor
