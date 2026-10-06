// The ground under a point of a mission, in the game's words (mission_ground_facts.h, DI-07).

#include <editor/preview/mission_ground_facts.h>

#include <cmath>
#include <cstdio>

#include <editor/preview/mission_scene.h>
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/til/til_tsd.h>
#include <formats/trn/charmap_legend.h>
#include <formats/trn/trn.h>
#include <runtime/audio/footstep_slot.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/environment/water_frame.h>
#include <runtime/terrain_query/surface_tiles.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/world/ammo_table.h>

namespace opennova::editor {

using io::JsonValue;
using io::json_number;
using io::json_string;

namespace {

// The classes in a modder's words, by the tile set table's order (formats/til/til_tsd.h): tooling words,
// the game's own being its TSD_ names.
constexpr const char *kSurfaceWords[] = {
	"Null", "Dirt", "Grass", "Snow", "Cement", "Sand", "Packed dirt", "Underwater", "Railroad", "Mud", "Ice",
	"Quicksand", "Stone", "Wood", "Metal", "Glass", "Cloth", "Foliage", "Heavy metal", "Flesh",
};
static_assert(sizeof(kSurfaceWords) / sizeof(kSurfaceWords[0]) == TIL_TSD_SURFACE_NAME_COUNT, "a word per class");
static_assert(kCharmapLegendCount == TIL_TSD_SURFACE_NAME_COUNT, "a legend colour per class");

bool known(int surface) { return surface >= 0 && surface < TIL_TSD_SURFACE_NAME_COUNT; }

std::string metres(double value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1f m", value);
	return text;
}

// "Snow (surface 3, TSD_SNOW"; the caller closes it.
std::string class_words(int surface) {
	std::string out = mission_surface_words(surface) + " (surface " + std::to_string(surface);
	if (const char *name = mission_surface_name(surface)) out += std::string(", ") + name;
	return out;
}

// What the slots a body plays there are in the game's words: "SSLFootSnow and SSRFootSnow", the water's
// one slot for both feet once.
std::string slot_words(const MissionGroundFacts &facts) {
	const char *left = audio::sound_profile_slot_keyword(facts.footstep[0]);
	const char *right = audio::sound_profile_slot_keyword(facts.footstep[1]);
	if (!left || !right) return std::string();
	if (facts.footstep[0] == facts.footstep[1]) return left;
	return std::string(left) + " and " + right;
}

// What a slot pair is, in a few words: "ground", "snow", "object", "water".
const char *slot_kind(int slot) {
	switch (slot) {
	case audio::kSlotFootLGround:
	case audio::kSlotFootRGround: return "ground";
	case audio::kSlotFootLSnow:
	case audio::kSlotFootRSnow: return "snow";
	case audio::kSlotFootLObject:
	case audio::kSlotFootRObject: return "object";
	case audio::kSlotFootWater: return "water";
	default: return "";
	}
}

std::string row_words(int row) {
	return std::string("its ammo's ") + world::kImpactEffectTagNames[row] + " row (" + std::to_string(row) + ")";
}

// The game's sampler at one point over `map`, with the placed tiles and their table as given.
int32_t sample(terrain::SurfaceTypeMap map, int32_t x, int32_t y, const terrain::SurfaceTileEntry *tiles,
		int32_t count, const uint8_t *table) {
	map.tiles = tiles;
	map.tile_count = count;
	map.tile_surface = table;
	return terrain::surface_type_at_fixed(map, x, y);
}

// Tables a probe of the sampler reads: each tile index to itself, and to the next; and a table whose
// first entry is no class at all.
struct Probes {
	uint8_t self[256];
	uint8_t next[256];
	uint8_t none[256];
	Probes() {
		for (int i = 0; i < 256; ++i) {
			self[i] = uint8_t(i);
			next[i] = uint8_t((i + 1) & 0xFF);
			none[i] = 0xFF;
		}
	}
};
const Probes &probes() {
	static const Probes kProbes;
	return kProbes;
}

} // namespace

const char *mission_surface_from_token(MissionSurfaceFrom from) {
	switch (from) {
	case MissionSurfaceFrom::NoTerrain: return "no_terrain";
	case MissionSurfaceFrom::NoMap: return "no_map";
	case MissionSurfaceFrom::Ocean: return "ocean";
	case MissionSurfaceFrom::Map: return "map";
	case MissionSurfaceFrom::Tile: return "tile";
	}
	return "no_terrain";
}

const char *mission_ground_on_token(MissionGroundOn on) {
	switch (on) {
	case MissionGroundOn::Nothing: return "nothing";
	case MissionGroundOn::Terrain: return "terrain";
	case MissionGroundOn::Record: return "record";
	}
	return "nothing";
}

std::string mission_surface_words(int surface) {
	return known(surface) ? std::string(kSurfaceWords[surface]) : "Surface " + std::to_string(surface);
}

const char *mission_surface_name(int surface) { return known(surface) ? til_tsd_surface_names[surface] : nullptr; }

std::string mission_surface_colour(int surface) {
	if (!known(surface)) return std::string();
	const CharmapLegendColour &c = kCharmapLegend[surface];
	char text[8];
	std::snprintf(text, sizeof(text), "#%02X%02X%02X", c.r, c.g, c.b);
	return text;
}

bool MissionGround::Key::operator==(const Key &other) const {
	return terrain == other.terrain && tile_set == other.tile_set && environment == other.environment &&
			mission == other.mission && water_attrib == other.water_attrib && water_override == other.water_override;
}

void MissionGround::follow(const std::shared_ptr<const FileSource> &files, uint64_t generation,
		const MissionSceneHeader &header, const std::string &mission) {
	Key key;
	key.terrain = header.terrain;
	key.tile_set = header.tile_set;
	key.environment = header.environment;
	key.mission = mission;
	key.water_attrib = header.attrib_flags;
	key.water_override = header.water_override;
	// A file it read is asked its stamp only where the files' generation moved.
	const bool moved = generation != generation_ && files && stamps_.moved(*files);
	generation_ = generation;
	if (read_once_ && key == key_ && !moved) return;
	read_once_ = true;
	key_ = std::move(key);
	read_(files, header);
}

void MissionGround::read_(const std::shared_ptr<const FileSource> &files, const MissionSceneHeader &header) {
	++reads_;
	store_.clear();
	error_.clear();
	surface_map_.clear();
	tiles_.clear();
	// Retail's memset table: every placed tile TSD_NULL with no .tsd [orig: PolyTrn_InitTextures @ 0x60c5c9].
	tile_surface_.fill(0);
	water_z_ = 0;
	stamps_.clear();
	if (!files) {
		error_ = "The project's files are not read yet.";
		return;
	}
	const auto stamped = std::make_shared<StampedFiles>(files);
	TrnConfig trn;
	bool loaded = false;
	if (header.terrain.empty()) error_ = "The mission names no terrain.";
	else loaded = terrain::terrain_field_store_load(store_, *stamped, header.terrain, error_, &trn);
	if (loaded) {
		if (store_.surface_map().data != nullptr) surface_map_ = trn.charmap;
		// The mission's placed tiles, read as the game reads <mission>.til [orig: Terrain_LoadTileInfoFile @
		// 0x60a740], and the table their tile set's .tsd fills.
		std::vector<uint8_t> til;
		if (!key_.mission.empty() && stamped->read(key_.mission + ".til", til))
			tiles_ = terrain::surface_tiles_from_til_bytes(til);
		terrain::SurfaceTileFileSource tile_files;
		tile_files.has_file = [stamped](const std::string &name) { return stamped->stamp(name) != 0; };
		tile_files.read_file = [stamped](const std::string &name, std::vector<uint8_t> &out) {
			return stamped->read(name, out);
		};
		terrain::resolve_tileset_surface_table(tile_files, trn_mission_tilestrip(trn, header.tile_set), tile_surface_.data());
	}
	// The water plane by the game's ladder: the mission's override, the terrain's, the environment's
	// (water_frame.h; the device's Water resolves the same).
	env::Config config;
	bool env_loaded = false;
	std::vector<uint8_t> bytes;
	if (!header.environment.empty() && stamped->read(header.environment + ".env", bytes)) {
		const std::string text(bytes.begin(), bytes.end());
		env_loaded = env::load_mission_env(&text, config);
	}
	env::EnvironmentState state;
	if (env_loaded) state.set_config(&config, true);
	const env::BmsEnvOverrides overrides = env::bms_env_overrides_from_header(header.attrib_flags, header.water_override,
			header.fog_override, header.fog_color, header.water_color, header.water_murk);
	env::WaterHeightRungs rungs;
	rungs.has_mission_override = overrides.has_water_height;
	rungs.mission_override = overrides.has_water_height ? overrides.water_height * 0.5f : 0.0f;
	rungs.terrain_height = loaded ? float(trn.water_height) * 0.5f : 0.0f;
	rungs.has_loaded_terrain = loaded;
	const float height = env::resolve_water_height(rungs, env_loaded ? &state : nullptr, 0.0f);
	// As the game's world takes it (Simulation::set_water_z): the plane in 16.16, 0 none.
	water_z_ = static_cast<int32_t>(double(height) * 65536.0);
	stamps_ = stamped->stamps();
}

double MissionGround::water_height() const { return double(water_z_) / 65536.0; }

void MissionGround::foot_(MissionGroundFacts &facts, bool on_record) const {
	facts.water = water();
	facts.water_height = water_height();
	// The feet at the point: a body standing there (its position dipped to foot level by its capsule).
	const int32_t feet = bms::to_fixed_16_16(facts.at[2]);
	for (int foot = 0; foot < 2; ++foot)
		facts.footstep[foot] = audio::footstep_slot(feet, water_z_, on_record, facts.surface, foot);
	facts.under_water = facts.footstep[0] == audio::kSlotFootWater;
	// A round from above meets the water plane first where the point lies under it (the water's row); on
	// the terrain the class's row; on an entity the material of the face it strikes decides.
	if (facts.under_water) facts.impact_row = world::kWaterImpactEffectTag;
	else if (!on_record && facts.surface >= 0) facts.impact_row = world::terrain_impact_effect_tag(facts.surface);
	else facts.impact_row = -1;
}

MissionGroundFacts MissionGround::terrain_at(double x, double y, double z) const {
	MissionGroundFacts facts;
	facts.on = MissionGroundOn::Terrain;
	facts.at[0] = x;
	facts.at[1] = y;
	facts.at[2] = z;
	if (terrain()) {
		const terrain::SurfaceTypeMap &map = store_.surface_map();
		const int32_t fx = bms::to_fixed_16_16(x), fy = bms::to_fixed_16_16(y);
		const terrain::SurfaceTileEntry *tiles = tiles_.empty() ? nullptr : tiles_.data();
		const int32_t count = int32_t(tiles_.size());
		// The class the game reads there, its placed tiles and their table as the world holds them.
		facts.surface = sample(map, fx, fy, tiles, count, tile_surface_.data());
		// Which leg decided, asked of the same sampler rather than worked out beside it: with no char map it
		// reads 1 before anything; a tile placed at the point itself, whose table entry is no class, comes
		// back only where the sampler reaches the tile walk (else an empty sector's 7 came first); and two
		// tables, each tile to itself and to the next, disagree only where a placed tile's square decides,
		// the first naming its index.
		if (map.data == nullptr) {
			facts.from = MissionSurfaceFrom::NoMap;
		} else {
			// The probe's 16-unit square centred on the point (its z stored negated, as the .til keeps it);
			// a point within 8 units of what a 16.16 word holds is past every terrain's sectors anyway.
			constexpr int32_t kHalf = 0x80000, kFar = 0x7F000000;
			const bool inside = fx > -kFar && fx < kFar && fy > -kFar && fy < kFar;
			const terrain::SurfaceTileEntry here{ fx - kHalf, kHalf - fy, 0 };
			if (inside && sample(map, fx, fy, &here, 1, probes().none) != 0xFF) {
				facts.from = MissionSurfaceFrom::Ocean;
			} else {
				const int32_t self = sample(map, fx, fy, tiles, count, probes().self);
				const int32_t next = sample(map, fx, fy, tiles, count, probes().next);
				if (self != next) {
					facts.from = MissionSurfaceFrom::Tile;
					facts.tile = self;
					facts.map_surface = sample(map, fx, fy, nullptr, 0, nullptr);
				} else {
					facts.from = MissionSurfaceFrom::Map;
				}
			}
		}
	}
	foot_(facts, false);
	return facts;
}

MissionGroundFacts MissionGround::record_at(NodeId row, const std::string &title, double x, double y, double z) const {
	MissionGroundFacts facts;
	facts.on = MissionGroundOn::Record;
	facts.row = row;
	facts.record = title;
	facts.at[0] = x;
	facts.at[1] = y;
	facts.at[2] = z;
	foot_(facts, true);
	return facts;
}

std::string mission_ground_line(const MissionGroundFacts &facts) {
	if (facts.on == MissionGroundOn::Nothing) return std::string();
	const std::string slots = slot_words(facts);
	if (facts.under_water) {
		std::string line = metres(facts.water_height - facts.at[2]) + " under the water: footsteps play " + slots +
				"; a round from above meets the water first, " + row_words(facts.impact_row) + ".";
		if (facts.on == MissionGroundOn::Record) line += " On " + facts.record + ".";
		else if (facts.surface >= 0) line += " Below it: " + class_words(facts.surface) + ").";
		return line;
	}
	if (facts.on == MissionGroundOn::Record)
		return "On " + facts.record + ": footsteps play " + slots +
				" (a body standing on an entity); a round plays the row of the material it strikes.";
	if (facts.surface < 0) return "The terrain was not read: footsteps play " + slots + ".";
	std::string line = class_words(facts.surface);
	switch (facts.from) {
	case MissionSurfaceFrom::NoMap: line += ": the terrain has no char map, so 1 everywhere)"; break;
	case MissionSurfaceFrom::Ocean: line += ": a sector the terrain leaves empty, the ocean)"; break;
	case MissionSurfaceFrom::Tile:
		line += ") from placed tile " + std::to_string(facts.tile) + ", over the char map's " +
				mission_surface_words(facts.map_surface) + " (" + std::to_string(facts.map_surface) + ")";
		break;
	default: line += ")"; break;
	}
	return line + ": footsteps play " + slots + "; a round plays " + row_words(facts.impact_row) + ".";
}

io::JsonValue mission_ground_to_json(const MissionGroundFacts &facts) {
	JsonValue out = JsonValue::make_object();
	out.set("on", json_string(mission_ground_on_token(facts.on)));
	if (facts.on == MissionGroundOn::Nothing) return out;
	JsonValue at = JsonValue::make_array();
	for (const double value : facts.at) at.push(json_number(value));
	out.set("at", std::move(at));
	if (facts.on == MissionGroundOn::Record) {
		out.set("row", json_number(double(facts.row)));
		out.set("record", json_string(facts.record));
	}
	if (facts.on == MissionGroundOn::Terrain && facts.surface >= 0) {
		out.set("surface", json_number(facts.surface));
		const char *name = mission_surface_name(facts.surface);
		out.set("name", name ? json_string(name) : JsonValue::make_null());
		out.set("words", json_string(mission_surface_words(facts.surface)));
		out.set("colour", json_string(mission_surface_colour(facts.surface)));
	} else {
		out.set("surface", JsonValue::make_null());
	}
	if (facts.on == MissionGroundOn::Terrain) {
		out.set("from", json_string(mission_surface_from_token(facts.from)));
		out.set("map_surface", facts.from == MissionSurfaceFrom::Tile ? json_number(facts.map_surface) : JsonValue::make_null());
		out.set("tile", facts.from == MissionSurfaceFrom::Tile ? json_number(facts.tile) : JsonValue::make_null());
	}
	out.set("water", JsonValue::make_bool(facts.water));
	out.set("water_height", facts.water ? json_number(facts.water_height) : JsonValue::make_null());
	out.set("under_water", JsonValue::make_bool(facts.under_water));
	JsonValue footstep = JsonValue::make_object();
	JsonValue slots = JsonValue::make_array(), names = JsonValue::make_array();
	for (const int slot : facts.footstep) {
		slots.push(json_number(slot));
		const char *keyword = audio::sound_profile_slot_keyword(slot);
		names.push(keyword ? json_string(keyword) : JsonValue::make_null());
	}
	footstep.set("slots", std::move(slots));
	footstep.set("names", std::move(names));
	footstep.set("words", json_string(slot_kind(facts.footstep[0])));
	out.set("footstep", std::move(footstep));
	if (facts.impact_row >= 0) {
		JsonValue row = JsonValue::make_object();
		row.set("row", json_number(facts.impact_row));
		row.set("tag", json_string(world::kImpactEffectTagNames[facts.impact_row]));
		row.set("words", json_string(world::kImpactEffectTagWords[facts.impact_row]));
		out.set("impact_row", std::move(row));
	} else {
		out.set("impact_row", JsonValue::make_null());
	}
	out.set("line", json_string(mission_ground_line(facts)));
	return out;
}

} // namespace opennova::editor
