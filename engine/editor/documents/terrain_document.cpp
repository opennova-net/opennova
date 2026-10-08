// The terrain document (terrain_document.h; the deep-integration plan's DI-30): a `.trn` over the engine's
// own reader and writer (formats/trn, docs/terrain/terrain-re.md), its fields the keys [orig:
// Terrain_ParseConfigCallback @ 0x60F330] reads, in the units the file writes them.
#include <editor/documents/terrain_document.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

#include <base/io/ascii_config.h>
#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/documents/texture_roles.h>
#include <editor/model/staged_rows.h>
#include <formats/env/env.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

namespace {

constexpr NodeKind kTerrain = node_kind(TerrainKind::Terrain);
constexpr NodeKind kSectorRow = node_kind(TerrainKind::SectorRow);
constexpr NodeKind kFoliage = node_kind(TerrainKind::Foliage);
constexpr int kSide = kTerrainGridSide;

TerrainRow &row_of(const RecordHandle &record) { return record.as<TerrainRow>(); }
TrnConfig &config_of(const RecordHandle &record) { return row_of(record).config; }
FoliageDef &foliage_of(const RecordHandle &record) { return record.as<FoliageDef>(); }
// A sector row is its first cell's place in the grid (TrnConfig::sector_grid[r]), sixteen cells on.
int *cells_of(const RecordHandle &record) { return &record.as<int>(); }

// --- values -------------------------------------------------------------------------------------

bool whole_of(const Value &value, int64_t &out, std::string &error) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = *whole;
		return true;
	}
	if (const auto *real = std::get_if<double>(&value); real && std::isfinite(*real) && std::floor(*real) == *real &&
	                                                    std::fabs(*real) < 9.0e15) {
		out = static_cast<int64_t>(*real);
		return true;
	}
	error = "A whole number.";
	return false;
}

bool in_range(int64_t value, int64_t min, int64_t max, const std::string &words, std::string &error) {
	if (value >= min && value <= max) return true;
	error = words;
	return false;
}

bool real_of(const Value &value, double &out, std::string &error) {
	if (const auto *real = std::get_if<double>(&value)) out = *real;
	else if (const auto *whole = std::get_if<int64_t>(&value)) out = static_cast<double>(*whole);
	else {
		error = "A number.";
		return false;
	}
	if (!std::isfinite(out)) {
		error = "A finite number.";
		return false;
	}
	return true;
}

bool power_of_two_or_zero(int64_t n) { return n >= 0 && ((n - 1) & n) == 0; }

// The next power of two at or past `n`, at most 16.
int next_power_of_two(int n) {
	int out = 1;
	while (out < n && out < kSide) out <<= 1;
	return out;
}

// A name as the writer writes it: unquoted (a map's, a model's), so no character the tokenizer cuts a token
// or a line at (a space, a comma, a tab, a ';', "//", a '"') nor a control character [orig:
// File_ParseASCIIFile @ 0x53D810, its delimiters @ 0x53CC33..0x53CC4C]; or quoted (the terrain's name and
// its maker's), so no '"' and no control character.
bool token_writable(const std::string &name, bool quoted) {
	for (const unsigned char c : name) {
		if (c < 0x20 || c == 0x7F || c == '"') return false;
		if (!quoted && (c == ' ' || c == ',' || c == '\t' || c == ';')) return false;
	}
	return quoted || name.find("//") == std::string::npos;
}

bool text_of(const Value &value, bool quoted, std::string &stored, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) {
		error = "A name.";
		return false;
	}
	if (!utf8_to_cp1252(*text, stored)) {
		error = "The game's text encoding (Windows-1252) has no character for part of this name.";
		return false;
	}
	if (!token_writable(stored, quoted)) {
		error = quoted ? "A name holds no '\"' and no control character: the game's reader cannot read one."
		               : "A file name holds no space, comma, tab, ';', \"//\", '\"' or control character: the game's "
		                 "reader ends the name there.";
		return false;
	}
	return true;
}

// --- the fields -----------------------------------------------------------------------------------

FieldSchema schema(const std::string &id, FieldType type, const char *label, const char *section,
                   const std::string &description) {
	FieldSchema out;
	out.id = id;
	out.type = type;
	out.label = label;
	out.section = section;
	out.description = description;
	return out;
}

void ranged(FieldSchema &field, double min, double max) {
	field.ranged = true;
	field.min = min;
	field.max = max;
}

// A name a key holds (a map, the height data, the tile set): "" leaves the key out, as the writer does.
LabelledField name_field(const char *id, const char *label, const char *section, const std::string &description,
                         std::string TrnConfig::*member, ReferenceKind reference, bool quoted = false) {
	LabelledField field;
	field.schema = schema(id, FieldType::Text, label, section, description);
	field.schema.reference = reference;
	field.schema.code_page = true;
	field.value.get = [member](const RecordHandle &record, Value &out) {
		out = cp1252_to_utf8(config_of(record).*member);
		return true;
	};
	field.value.set = [member, quoted](const RecordHandle &record, const Value &value, std::string &error) {
		std::string stored;
		if (!text_of(value, quoted, stored, error)) return false;
		config_of(record).*member = std::move(stored);
		return true;
	};
	return field;
}

// A key the reader reads with atol into an int [orig: Terrain_ParseConfigCallback @ 0x60F330, j__atol of
// tokens[2] in its arm], within `min`..`max` (the 32-bit long where nothing narrower holds it).
LabelledField int_field(const char *id, const char *label, const char *section, const std::string &description,
                        int TrnConfig::*member, int64_t min = INT32_MIN, int64_t max = INT32_MAX,
                        const std::string &range_words = "A whole number the game's 32-bit reader holds.") {
	LabelledField field;
	field.schema = schema(id, FieldType::Integer, label, section, description);
	if (min != INT32_MIN || max != INT32_MAX) ranged(field.schema, double(min), double(max));
	field.value.get = [member](const RecordHandle &record, Value &out) {
		out = static_cast<int64_t>(config_of(record).*member);
		return true;
	};
	field.value.set = [member, min, max, range_words](const RecordHandle &record, const Value &value, std::string &error) {
		int64_t whole = 0;
		if (!whole_of(value, whole, error) || !in_range(whole, min, max, range_words, error)) return false;
		config_of(record).*member = static_cast<int>(whole);
		return true;
	};
	return field;
}

// One of a pair a line holds (an origin's x and y, a lock's), its two values on one row (its `group`).
LabelledField pair_field(const std::string &id, const char *label, const char *section, const char *group,
                         const char *token, const std::string &description,
                         const std::function<int &(TrnConfig &)> &at) {
	LabelledField field;
	field.schema = schema(id, FieldType::Integer, label, section, description);
	field.schema.group = group;
	field.schema.token = token;
	field.value.get = [at](const RecordHandle &record, Value &out) {
		out = static_cast<int64_t>(at(config_of(record)));
		return true;
	};
	field.value.set = [at](const RecordHandle &record, const Value &value, std::string &error) {
		int64_t whole = 0;
		if (!whole_of(value, whole, error) ||
		    !in_range(whole, INT32_MIN, INT32_MAX, "A whole number the game's 32-bit reader holds.", error))
			return false;
		at(config_of(record)) = static_cast<int>(whole);
		return true;
	};
	return field;
}

// The texture roles the terrain's maps are opened by (ADR 0046 S18, documents/texture_roles.h): what
// PolyTrn_InitTextures does with each key's file [orig: PolyTrn_InitTextures @ 0x60AAA0]. Without its colour
// map the game logs "colormap" @ 0x60B389; without its blend map, once the key names one at all (the key
// alone sets the blend on [orig: Terrain_ParseConfigCallback @ 0x60F7D0], and every card with pixel shaders
// takes it, PolyTrn_InitTextures @ 0x60B15D..0x60B176), "blendermap" @ 0x60B19A. Either error aborts the
// mission [orig: sub_520AA0 @ 0x520B4E]: kTextureArgGates.
struct MapRole {
	const char *id;
	TextureRoleId role;
	int32_t flags;
};
constexpr MapRole kMapRoles[] = {
	{"polytrn_colormap", TextureRoleId::TerrainColourMap, kTextureArgGates},
	{"polytrn_detailmap", TextureRoleId::TerrainDetailCoefficient, 0},
	{"polytrn_detailmap_c1", TextureRoleId::TerrainSplatDetail, 0},
	{"polytrn_detailmap_c2", TextureRoleId::TerrainSplatDetail, 0},
	{"polytrn_detailmap_c3", TextureRoleId::TerrainSplatDetail, 0},
	{"polytrn_detailmap2", TextureRoleId::TerrainSecondDetail, 0},
	{"polytrn_detailmapdist", TextureRoleId::TerrainFarDetail, 0},
	{"polytrn_detailmapdist2", TextureRoleId::TerrainFarDetail, 0},
	{"polytrn_detailblendmap", TextureRoleId::TerrainBlendMap, kTextureArgGates},
	{"polytrn_tilestrip", TextureRoleId::TerrainTileAtlas, 0},
	{"polytrn_charmap", TextureRoleId::TerrainCharMap, 0},
	{"polytrn_foliagemap", TextureRoleId::TerrainFoliageMap, 0},
};

const char *const kRequiredWords = " An empty name refuses the mission [orig: Terrain_LoadEnvironmentConfig @ 0x610A2A..0x610A3E].";

void terrain_fields(TableKind &kind) {
	using T = TrnConfig;
	// --- what the readers never read [orig: neither word is in the binary but inside "TexHorizon"]
	{
		LabelledField field = name_field("terrain_name", "Name", "Terrain",
		                                 "The terrain's name, kept in its file: no reader of the game reads the line.",
		                                 &T::name, ReferenceKind::None, true);
		field.schema.applies = Applicability::Ignored;
		kind.field(std::move(field));
	}
	{
		LabelledField field = name_field("terrain_creator", "Made by", "Terrain",
		                                 "Who made the terrain, kept in its file: no reader of the game reads the line.",
		                                 &T::creator, ReferenceKind::None, true);
		field.schema.applies = Applicability::Ignored;
		kind.field(std::move(field));
	}

	// --- the heights [orig: PolyTrn_LoadTerrainConfig @ 0x60E62F -> sub_603CC0 @ 0x603D0A; Terrain_LoadLodStorage
	// @ 0x603550, the missing file @ 0x60356D]
	kind.field(name_field("polytrn_polydata", "Height data", "Heights",
	                      std::string("The .cpt the game loads the heights and the ground mesh from, by the name as "
	                                  "written. A missing file loads no terrain and logs nothing: the mission goes on with "
	                                  "no ground.") + kRequiredWords,
	                      &T::polydata, ReferenceKind::TerrainData));

	// --- the colour [orig: PolyTrn_InitTextures @ 0x60B395 (missing), @ 0x60C616 (unreadable)]
	kind.field(name_field("polytrn_colormap", "Colour map", "Colour",
	                      std::string("The ground's colour, a 1024 x 1024 TGA laid over the heightmap. Missing, or a TGA "
	                                  "the game's reader refuses, aborts the mission's load.") + kRequiredWords,
	                      &T::colormap, ReferenceKind::Texture));

	// --- the detail [orig: PolyTrn_InitTextures @ 0x60AAF4, @ 0x60ABDF, @ 0x60AF80, @ 0x60AC27..0x60AD72,
	// @ 0x60B18E..0x60B1A6; the densities' defaults @ 0x610972, @ 0x610977; the UVs, source x density / 512
	// @ 0x6029A0..0x6029AA]
	kind.field(name_field("polytrn_detailmap", "Detail map", "Detail",
	                      std::string("The detail's coefficient map. A missing file is no error: the coefficient is none "
	                                  "and the detail's average colour 128.") + kRequiredWords,
	                      &T::detailmap, ReferenceKind::Texture));
	kind.field(int_field("polytrn_detaildensity", "Detail density", "Detail",
	                     "How often the detail repeats: each layer's texture coordinate is the texel times this over 512 "
	                     "(128 by default).",
	                     &T::detail_density));
	kind.field(name_field("polytrn_detailmap_c1", "Splat layer 1", "Detail",
	                      "The detail drawn where the blend map is red. With this one named (on a card with pixel shaders) "
	                      "the game draws the three splat layers; each is loaded as DXT1, a missing one an empty slot.",
	                      &T::detailmap_c1, ReferenceKind::Texture));
	kind.field(name_field("polytrn_detailmap_c2", "Splat layer 2", "Detail",
	                      "The detail drawn where the blend map is green.", &T::detailmap_c2, ReferenceKind::Texture));
	kind.field(name_field("polytrn_detailmap_c3", "Splat layer 3", "Detail",
	                      "The detail drawn where the blend map is blue.", &T::detailmap_c3, ReferenceKind::Texture));
	kind.field(name_field("polytrn_detailblendmap", "Blend map", "Detail",
	                      "How much of each splat layer each texel takes (red, green, blue). Naming it turns the blend on, "
	                      "and the file must then load: missing, it aborts the mission's load.",
	                      &T::detailblendmap, ReferenceKind::Texture));
	kind.field(name_field("polytrn_detailmap2", "Second detail", "Detail",
	                      "A second detail over the first at the second density: optional, and a missing one is no error.",
	                      &T::detailmap2, ReferenceKind::Texture));
	kind.field(int_field("polytrn_detaildensity2", "Second density", "Detail",
	                     "How often the second detail repeats, as the detail density (8 by default).", &T::detail_density2));
	kind.field(name_field("polytrn_detailmapdist", "Far detail", "Detail",
	                      "The detail far away: optional, and a missing one is no error.", &T::detailmapdist,
	                      ReferenceKind::Texture));
	kind.field(name_field("polytrn_detailmapdist2", "Far second detail", "Detail",
	                      "The second detail far away: optional, and a missing one is no error.", &T::detailmapdist2,
	                      ReferenceKind::Texture));

	// --- what grows and what the ground is made of [orig: sub_605A10 @ 0x605A31 -> Texture_LoadPCXFromPFF8Bit
	// @ 0x56E0A0; Terrain_GetSurfaceTypeAtPosition @ 0x606519..0x60651B; Foliage_LoadFoliageMapPCX @ 0x605AD0;
	// Foliage_SampleFoliageMapMask @ 0x60662B]
	kind.field(name_field("polytrn_charmap", "Surface map", "Surfaces and foliage",
	                      "An 8-bit PCX whose index at each texel is the surface class the game reads there: footsteps, "
	                      "impact effects, how rounds and throwables behave. Square, a power of two at most 1024 a side, "
	                      "laid over the heightmap as the colour map is. With none the game reads dirt (1) everywhere.",
	                      &T::charmap, ReferenceKind::Texture));
	kind.field(name_field("polytrn_foliagemap", "Foliage map", "Surfaces and foliage",
	                      "An 8-bit PCX whose index at each texel is a foliage code: each definition below grows where the "
	                      "map holds one of its codes. Square, a power of two at most 1024 a side. With none nothing grows.",
	                      &T::foliagemap, ReferenceKind::Texture));

	// --- the placed tiles [orig: Terrain_LoadTileSetAtlas @ 0x604AB8, @ 0x604B7C..0x604BC2; the mission's tile
	// set over it, Terrain_LoadEnvironmentConfig @ 0x6109C8..0x610A1C; Terrain_Init @ 0x60FCFD;
	// PolyTrn_LoadTerrainConfig @ 0x60E6C9, @ 0x60E6DC..0x60E6E5]
	kind.field(name_field("polytrn_tilestrip", "Tile set", "Tiles",
	                      "The atlas the placed tiles draw from, cut in 64-texel cells. A mission that names a tile set "
	                      "draws from that one instead.",
	                      &T::tilestrip, ReferenceKind::Texture));
	kind.field(name_field("polytrn_tileinfo", "Tile placement", "Tiles",
	                      "The terrain's own tile placement (.til), read only where a mission has none of its own name "
	                      "(<mission>.til); with neither no tile is placed.",
	                      &T::tileinfo, ReferenceKind::TilePlacement));

	// --- the sector grid [orig: Terrain_ParseConfigCallback @ 0x60F330, the polytrn_sectorcount, wrap and origin
	// arms; the gate, Terrain_LoadEnvironmentConfig @ 0x610940's tail; Terrain_ShiftHeightmapRows @
	// 0x60F2A5..0x60F317; the wraps' cell masks, PolyTrn_LoadTerrainConfig @ 0x60E4B1..0x60E4CB]
	{
		LabelledField field = int_field(
				"polytrn_sectorcount", "Grid width", "Sector grid",
				"How many sectors each grid row places, each a 512-unit square of the heightmap: a power of two, at most "
				"16 (the game refuses the mission otherwise).",
				&T::sector_count, 0, kSide, "The grid's width: 0 to 16.");
		field.schema.unit = "sectors";
		field.schema.choices = {{"1", 1, ""}, {"2", 2, ""}, {"4", 4, ""}, {"8", 8, ""}, {"16", 16, ""}};
		field.schema.open_choices = true;
		const auto set = field.value.set;
		field.value.set = [set](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t whole = 0;
			if (whole_of(value, whole, error) && !power_of_two_or_zero(whole)) {
				error = "A power of two (1, 2, 4, 8 or 16): the game refuses a terrain whose width is not one.";
				return false;
			}
			return set(record, value, error);
		};
		kind.field(std::move(field));
	}
	for (const auto &[id, label, member, axis] :
	     {std::make_tuple("polytrn_wrapx", "Wraps east-west", &T::wrap_x, "east and west"),
	      std::make_tuple("polytrn_wrapy", "Wraps north-south", &T::wrap_y, "north and south")}) {
		LabelledField field = int_field(id, label, "Sector grid",
		                                std::string("Whether the grid repeats past its ") + axis +
		                                        " edges (any number but 0), or its edge sectors run on past them (0).",
		                                member);
		field.schema.choices = {{"0", 0, "No"}, {"1", 1, "Wraps"}};
		field.schema.open_choices = true;
		kind.field(std::move(field));
	}
	const std::string origin_words = "Where the grid's first sector lies, counted in sectors from the world's origin "
	                                 "(an 8 x 8 grid from -4, -4 has its middle there).";
	kind.field(pair_field("origin_x", "East", "Sector grid", "Origin", "polytrn_origin", origin_words,
	                      [](TrnConfig &c) -> int & { return c.origin_x; }));
	kind.field(pair_field("origin_y", "North", "Sector grid", "Origin", "polytrn_origin", origin_words,
	                      [](TrnConfig &c) -> int & { return c.origin_y; }));
	// The quadrant locks [orig: Terrain_ParseConfigCallback @ 0x60FAF7..0x60FBC8; the taps @ 0x60327A..0x6032EC].
	const std::string lock_words = "How the heights' neighbour taps cross this quadrant's edges along the axis: 0 crosses "
	                               "into the next quadrant, any other number wraps inside the quadrant's own 512 texels.";
	struct Lock {
		const char *key;
		const char *group;
		TerrainLockCoord TrnConfig::*member;
	};
	for (const Lock &lock : {Lock{"lock_topleft", "Top left lock", &T::lock_topleft},
	                         Lock{"lock_topright", "Top right lock", &T::lock_topright},
	                         Lock{"lock_bottomleft", "Bottom left lock", &T::lock_bottomleft},
	                         Lock{"lock_bottomright", "Bottom right lock", &T::lock_bottomright}}) {
		auto member = lock.member;
		kind.field(pair_field(std::string(lock.key) + "_x", "X", "Quadrant locks", lock.group, lock.key, lock_words,
		                      [member](TrnConfig &c) -> int & { return (c.*member).x; }));
		kind.field(pair_field(std::string(lock.key) + "_y", "Y", "Quadrant locks", lock.group, lock.key, lock_words,
		                      [member](TrnConfig &c) -> int & { return (c.*member).y; }));
	}

	// --- the water: water_height is read by the environment's reader in the terrain's pass, atol << 15
	// [orig: TimeOfDay_ParseProperty @ 0x57CB48..0x57CB6A], taken by the terrain [orig: Terrain_Init @ 0x60FCB3],
	// a .env's over it and a mission's where its header says; 0 draws no water [orig: render_water_surface
	// @ 0x5C32E0..0x5C32E7]; water_rgb and water_murk are the environment's, read in the same pass (trn.h)
	{
		LabelledField field = int_field("water_height", "Water height", "Water",
		                                "The sea's height in half metres (24 is 12 m); 0 draws no water. A mission's header "
		                                "can set its own; the environment's comes after the terrain's.",
		                                &T::water_height, int64_t(INT32_MIN) >> 15, int64_t(INT32_MAX) >> 15,
		                                "-65536 to 65535 half metres: the game keeps half metres in 16.16.");
		field.schema.unit = "half m";
		kind.field(std::move(field));
	}
	static const char *const kChannels[] = {"r", "g", "b"};
	static const char *const kChannelLabels[] = {"Red", "Green", "Blue"};
	for (int c = 0; c < 3; ++c) {
		LabelledField field;
		field.schema = schema(std::string("water_rgb_") + kChannels[c], FieldType::Byte, kChannelLabels[c], "Water",
		                      "The water's colour, read by the environment's reader as it reads the terrain, before the "
		                      "mission's .env, whose own water colour then wins.");
		field.schema.token = "water_rgb";
		field.schema.group = "Water colour";
		field.schema.color = FieldColor::Channel;
		field.schema.optional = true;
		ranged(field.schema, 0, 255);
		field.value.get = [c](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(config_of(record).water_rgb[size_t(c)]);
			return true;
		};
		field.value.set = [c](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t byte = 0;
			if (!whole_of(value, byte, error) || !in_range(byte, 0, 255, "A colour's byte: 0 to 255.", error)) return false;
			// A Set of another value writes the line; one of the value it reads changes nothing.
			TrnConfig &config = config_of(record);
			if (config.water_rgb[size_t(c)] == byte) return true;
			config.water_rgb[size_t(c)] = static_cast<int>(byte);
			config.water_rgb_set = true;
			return true;
		};
		field.value.present = [](const RecordHandle &record) { return config_of(record).water_rgb_set; };
		field.value.set_present = [](const RecordHandle &record, bool present, std::string &) {
			config_of(record).water_rgb_set = present;
			return true;
		};
		kind.field(std::move(field));
	}
	{
		LabelledField field;
		field.schema = schema("water_murk", FieldType::Real, "Water murk", "Water",
		                      "How murky the water is, at most 0.99, read by the environment's reader as it reads the "
		                      "terrain: no shipped .env sets one, so a mission's murk is its terrain's unless its header "
		                      "sets its own.");
		field.schema.optional = true;
		ranged(field.schema, 0, 0.99);
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = static_cast<double>(config_of(record).water_murk);
			return true;
		};
		field.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
			double real = 0.0;
			if (!real_of(value, real, error)) return false;
			if (static_cast<float>(real) > 0.99f) {
				error = "At most 0.99: the game reads a larger murk as 0.99.";
				return false;
			}
			TrnConfig &config = config_of(record);
			if (config.water_murk == static_cast<float>(real)) return true;
			config.water_murk = static_cast<float>(real);
			config.water_murk_set = true;
			return true;
		};
		field.value.present = [](const RecordHandle &record) { return config_of(record).water_murk_set; };
		field.value.set_present = [](const RecordHandle &record, bool present, std::string &) {
			config_of(record).water_murk_set = present;
			return true;
		};
		kind.field(std::move(field));
	}
	{
		LabelledField field;
		field.schema = schema("horizon", FieldType::Real, "Horizon", "Terrain",
		                      "Kept in its file: no reader of the game reads the line.");
		field.schema.applies = Applicability::Ignored;
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = config_of(record).horizon;
			return true;
		};
		field.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
			double real = 0.0;
			if (!real_of(value, real, error)) return false;
			config_of(record).horizon = real;
			return true;
		};
		kind.field(std::move(field));
	}
}

// A grid row's cells: the sector each places [orig: the polytrn_sectors arm of Terrain_ParseConfigCallback @
// 0x60F330, atol of each column's token]: 0 none (the flat fallback, terrain-re.md D-TERRAIN-12), 1 to 4 the
// heightmap's quadrants (1 its top left, 2 bottom left, 3 top right, 4 bottom right: runtime/terrain_query/
// coords.h). The game reads the width's columns of a row; the rest the grid's extension fills.
void sector_row_fields(TableKind &kind) {
	for (int c = 0; c < kSide; ++c) {
		LabelledField field;
		field.schema = schema("sector_" + std::to_string(c + 1), FieldType::Integer, "", "",
		                      "The sector this cell of the grid places: none (0), or one of the heightmap's four quadrants "
		                      "(1 top left, 2 bottom left, 3 top right, 4 bottom right). The game reads as many cells "
		                      "of a row as the grid's width.");
		static const std::string kLabels[kSide] = {"1", "2", "3", "4", "5", "6", "7", "8",
		                                           "9", "10", "11", "12", "13", "14", "15", "16"};
		field.schema.label = kLabels[c];
		field.schema.group = c < 8 ? "Sectors 1 to 8" : "Sectors 9 to 16";
		field.schema.token = "polytrn_sectors";
		field.schema.choices = {{"0", 0, "None"}, {"1", 1, "Top left"}, {"2", 2, "Bottom left"},
		                        {"3", 3, "Top right"}, {"4", 4, "Bottom right"}};
		field.schema.open_choices = true;
		field.value.get = [c](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(cells_of(record)[c]);
			return true;
		};
		field.value.set = [c](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t whole = 0;
			if (!whole_of(value, whole, error) ||
			    !in_range(whole, INT32_MIN, INT32_MAX, "A whole number the game's 32-bit reader holds.", error))
				return false;
			cells_of(record)[c] = static_cast<int>(whole);
			return true;
		};
		// A cell past the width is read by no row's arm: the file does not write it.
		field.applies = [c](const RecordHandle &, const RecordOwners &owners) {
			if (owners.empty()) return Applicability::Reads;
			return c < config_of(owners.nearest().owner).sector_count ? Applicability::Reads : Applicability::Ignored;
		};
		kind.field(std::move(field));
	}
}

// A foliage definition's fields [orig: Terrain_ParseConfigCallback @ 0x60F330, its block arms; the codes
// Foliage_RemapPixelToDefMask @ 0x5FF4E0 compares; the colour modes the generator writes over,
// Foliage_GenerateInstances_0 @ 0x6002DB..0x60030A (foliage-re.md)].
void foliage_fields(TableKind &kind) {
	{
		LabelledField field;
		field.schema = schema("graphic", FieldType::Text, "Model", "",
		                      "The model it grows (a .3di of one part, cards up from the ground; drawn at its X and Z as "
		                      "authored and half its height). A definition with none grows nothing. The game sizes its "
		                      "cells by the model's vertices: at most 65534 / (36 x vertices) of them.");
		field.schema.reference = ReferenceKind::Model;
		field.schema.code_page = true;
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = cp1252_to_utf8(foliage_of(record).graphic);
			return true;
		};
		field.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
			std::string stored;
			if (!text_of(value, false, stored, error)) return false;
			foliage_of(record).graphic = std::move(stored);
			return true;
		};
		kind.field(std::move(field));
	}
	for (int i = 0; i < FOLIAGE_MATCH_CODES; ++i) {
		LabelledField field;
		static const char *const kLabels[] = {"1", "2", "3", "4"};
		field.schema = schema("match_" + std::to_string(i + 1), FieldType::Integer, kLabels[i], "",
		                      "A foliage code: it grows where the foliage map holds one of its four codes, 1 to 255. 0 is no "
		                      "code (the game never matches it). The codes are written in order on one line.");
		field.schema.group = "Codes";
		field.schema.token = "match";
		ranged(field.schema, 0, 255);
		field.value.get = [i](const RecordHandle &record, Value &out) {
			const int code = foliage_of(record).match[size_t(i)];
			out = static_cast<int64_t>(code < 0 ? 0 : code);
			return true;
		};
		// The codes stay in order, as the one `match` line writes them and the reader reads them back: a code is
		// given after the last, and one set to 0 (none: pixel 0 never matches [orig: Foliage_RemapPixelToDefMask
		// @ 0x5FF4E0]) leaves the line, the codes after it moving up.
		field.value.set = [i](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t code = 0;
			if (!whole_of(value, code, error) ||
			    !in_range(code, 0, 255, "A code of 0 to 255: the game keeps each as a byte.", error))
				return false;
			std::array<int, FOLIAGE_MATCH_CODES> &match = foliage_of(record).match;
			const int count = foliage_def_match_count(foliage_of(record));
			if (code == 0) {
				if (i >= count) return true;
				for (int k = i; k + 1 < FOLIAGE_MATCH_CODES; ++k) match[size_t(k)] = match[size_t(k + 1)];
				match[FOLIAGE_MATCH_CODES - 1] = FOLIAGE_MATCH_UNSET;
				return true;
			}
			if (i > count) {
				error = "The codes are written in order on the one match line: give code " + std::to_string(count + 1) +
				        " first.";
				return false;
			}
			match[size_t(i)] = static_cast<int>(code);
			return true;
		};
		kind.field(std::move(field));
	}
	for (const auto &[id, label, member] :
	     {std::make_tuple("color_lower", "Lower colour", &FoliageDef::color_lower),
	      std::make_tuple("color_upper", "Upper colour", &FoliageDef::color_upper)}) {
		LabelledField field;
		field.schema = schema(id, FieldType::Integer, label, "",
		                      "The shipped files' word for how the model's colour meets the ground's (0 the ground's, 1 "
		                      "half and half, 2 its own). The game's generator writes over the colour before it draws, "
		                      "so neither changes the picture.");
		ranged(field.schema, 0, 2);
		field.schema.choices = {{"0", 0, "The ground's"}, {"1", 1, "Half and half"}, {"2", 2, "Its own"}};
		auto m = member;
		field.value.get = [m](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(foliage_of(record).*m);
			return true;
		};
		field.value.set = [m](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t mode = 0;
			if (!whole_of(value, mode, error) || !in_range(mode, 0, 2, "0, 1 or 2.", error)) return false;
			foliage_of(record).*m = static_cast<int>(mode);
			return true;
		};
		kind.field(std::move(field));
	}
	{
		LabelledField field;
		field.schema = schema("attrib", FieldType::Integer, "Attributes", "",
		                      "forceon: it grows on the placed tiles too (else a tile keeps it off). shadow: read, and "
		                      "never drawn.");
		field.schema.flags = true;
		field.schema.choices = {{"forceon", FOLIAGE_ATTRIB_FORCE_ON, "Grows on placed tiles"},
		                        {"shadow", FOLIAGE_ATTRIB_SHADOW, "Shadow (never drawn)"}};
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(foliage_of(record).attrib_flags);
			return true;
		};
		field.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t flags = 0;
			if (!whole_of(value, flags, error)) return false;
			if (flags < 0 || (flags & ~int64_t(FOLIAGE_ATTRIB_KNOWN_MASK)) != 0) {
				error = "forceon (1) and shadow (2): the game's reader knows no other.";
				return false;
			}
			foliage_of(record).attrib_flags = static_cast<uint8_t>(flags);
			return true;
		};
		kind.field(std::move(field));
	}
}

// The sector rows over the record's fixed grid: its first sector_rows rows, at most 16.
ListOps sector_rows_list() {
	ListOps ops;
	ops.size = [](const RecordHandle &owner) { return size_t(std::max(0, config_of(owner).sector_rows)); };
	ops.at = [](const RecordHandle &owner, size_t index) {
		TrnConfig &config = config_of(owner);
		return index < size_t(std::max(0, config.sector_rows)) ? RecordHandle{kSectorRow, &config.sector_grid[index][0]}
		                                                       : RecordHandle{};
	};
	ops.insert = [](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		TrnConfig &config = config_of(owner);
		const int rows = std::max(0, config.sector_rows);
		if (rows >= kSide) {
			error = "The game reads 16 grid rows at most.";
			return false;
		}
		if (record && (!record->data || record->kind != kSectorRow)) {
			error = "This list takes grid rows only.";
			return false;
		}
		index = std::min(index, size_t(rows));
		// A new row is what the game's extension reads there: the row before it again (past the last, the
		// last; with a north-south wrap, the grid's rows again from the first) [orig: Terrain_ShiftHeightmapRows
		// @ 0x60F2A5..0x60F317].
		std::array<int, kSide> made{};
		if (record) made = *static_cast<const std::array<int, kSide> *>(record->data.get());
		else if (rows > 0) {
			const size_t from = index == size_t(rows) && config.wrap_y ? index % size_t(rows) : index > 0 ? index - 1 : 0;
			std::copy(config.sector_grid[from], config.sector_grid[from] + kSide, made.begin());
		}
		for (int r = rows; r > int(index); --r)
			std::copy(config.sector_grid[r - 1], config.sector_grid[r - 1] + kSide, config.sector_grid[r]);
		std::copy(made.begin(), made.end(), config.sector_grid[index]);
		config.sector_rows = rows + 1;
		return true;
	};
	ops.erase = [](const RecordHandle &owner, size_t index) {
		TrnConfig &config = config_of(owner);
		const int rows = std::max(0, config.sector_rows);
		if (index >= size_t(rows)) return false;
		for (int r = int(index); r + 1 < rows; ++r)
			std::copy(config.sector_grid[r + 1], config.sector_grid[r + 1] + kSide, config.sector_grid[r]);
		std::fill(config.sector_grid[rows - 1], config.sector_grid[rows - 1] + kSide, 0);
		config.sector_rows = rows - 1;
		return true;
	};
	ops.copy = [](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		TrnConfig &config = config_of(owner);
		if (index >= size_t(std::max(0, config.sector_rows))) return out;
		auto cells = std::make_shared<std::array<int, kSide>>();
		std::copy(config.sector_grid[index], config.sector_grid[index] + kSide, cells->begin());
		out.kind = kSectorRow;
		out.data = cells;
		return out;
	};
	return ops;
}

// A new definition as the shipped ones are written: the ground's colour below, its own above, shadow.
FoliageDef fresh_foliage(const TerrainRow &, size_t) {
	FoliageDef made;
	made.color_lower = 0;
	made.color_upper = 2;
	made.attrib_flags = FOLIAGE_ATTRIB_SHADOW;
	return made;
}

RecordTable make_table() {
	TableKind terrain(RecordKindRow{kTerrain, "terrain", "Terrain", "", true});
	terrain_fields(terrain);
	TableList rows;
	rows.spec.kind = kSectorRow;
	rows.spec.label = "Grid rows";
	rows.spec.max = size_t(kSide);
	rows.ops = sector_rows_list();
	terrain.list(std::move(rows));
	TableList foliage;
	foliage.spec.kind = kFoliage;
	foliage.spec.label = "Foliage";
	foliage.spec.max = size_t(FOLIAGE_MAX_DEFS);
	foliage.ops = vector_list<TerrainRow, FoliageDef>(
			kFoliage, [](TerrainRow &row) -> std::vector<FoliageDef> & { return row.config.foliage_defs; }, fresh_foliage);
	// The parser keeps four blocks [orig: Terrain_ParseConfigCallback @ 0x60F330, `dword_31BC900 < 4`].
	const auto insert = foliage.ops.insert;
	foliage.ops.insert = [insert](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		if (row_of(owner).config.foliage_defs.size() >= size_t(FOLIAGE_MAX_DEFS)) {
			error = "The game reads four foliage definitions at most.";
			return false;
		}
		return insert(owner, index, record, error);
	};
	terrain.list(std::move(foliage));
	TableKind sector_row(RecordKindRow{kSectorRow, "grid_row", "Grid row", "", false});
	sector_row_fields(sector_row);
	TableKind definition(RecordKindRow{kFoliage, "foliage", "Foliage", "", false});
	foliage_fields(definition);
	return RecordTable({std::move(terrain), std::move(sector_row), std::move(definition)});
}

// --- the source issues ------------------------------------------------------------------------------

// The keys the terrain's reader reads [orig: Terrain_ParseConfigCallback @ 0x60F330], with the two it keeps for
// whoever edits the file (terrain_name, terrain_creator: read by no arm).
bool terrain_reader_key(const std::string &key) {
	static const char *const kKeys[] = {
		"terrain_name", "terrain_creator", "horizon", "polytrn_colormap", "polytrn_detailmap_c1", "polytrn_detailmap_c2",
		"polytrn_detailmap_c3", "polytrn_detailblendmap", "polytrn_polydata", "polytrn_detailmap", "polytrn_detailmap2",
		"polytrn_detailmapdist", "polytrn_detailmapdist2", "polytrn_detaildensity", "polytrn_detaildensity2",
		"polytrn_sectorcount", "polytrn_wrapx", "polytrn_wrapy", "lock_topleft", "lock_topright", "lock_bottomleft",
		"lock_bottomright", "polytrn_origin", "polytrn_sectors", "polytrn_charmap", "polytrn_foliagemap",
		"polytrn_tilestrip", "polytrn_tileinfo",
	};
	for (const char *known : kKeys)
		if (key == known) return true;
	return false;
}

// The environment's keywords the terrain's record holds (trn.h): its water.
bool held_environment_key(const std::string &key) {
	return key == "water_height" || key == "water_rgb" || key == "water_murk";
}

} // namespace

void terrain_source_issues(const std::string &text, std::vector<SourceIssue> &issues) {
	bool in_foliage = false, swallowed = false;
	int closed = 0, sector_count = 0, row_lines = 0;
	size_t row_line_17 = 0;
	std::map<std::string, size_t> first_line;
	const auto issue = [&](bool blocks, size_t line, const std::string &field, const std::string &message) {
		SourceIssue out;
		out.blocks = blocks;
		out.line = line;
		out.field = field;
		out.message = message;
		issues.push_back(std::move(out));
	};
	size_t line = 0;
	// A last line no CR LF ends loses its final byte to the walk [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D8F5]
	// (shipped Dvxi4.trn and Dvxi4_c.trn end on such an "end").
	const bool ends_cut = text.size() < 2 || text.compare(text.size() - 2, 2, "\r\n") != 0;
	io::for_each_config_line_span(text.data(), text.size(), [&](io::ConfigTokens &tokens, const io::ConfigLineSpan &span) {
		++line;
		if (swallowed || tokens.count == 0 || tokens.tokens[0][0] == '/') return;
		const std::string key = strutil::to_lower(tokens.tokens[0]);
		const char *value = tokens.token(1);
		if (ends_cut && !text.empty() && span.end + 1 >= text.size()) {
			// A block's "end" read "en" leaves the block open; a block no end closes is still a definition, as
			// the runtime takes every slot whose graphic is named [orig: Terrain_Init @ 0x60FD11..0x60FD16;
			// Foliage_RemapPixelToDefMask @ 0x5FF4E0] (shipped Dvxi4.trn and Dvxi4_c.trn).
			if (in_foliage && key == "en") {
				issue(false, line, "end",
				      "The file's last line has no line end, so the game's reader reads 'end' as 'en' and leaves the "
				      "block open: it is still a definition, and a save writes its end.");
				return;
			}
			issue(false, line, key,
			      "The file's last line has no line end, so the game's reader loses its last character ('" +
			              std::string(1, text.back()) + "') and reads the line short: a save writes the line as the "
			              "game read it, ended.");
		}
		// The environment's reader reads every line of the terrain, inside a foliage block too (its walk has
		// no block state): what it takes is the mission's environment until the .env's line [orig:
		// Environment_LoadTimeOfDayConfig @ 0x57DB30, the .trn pass @ 0x57DBCC..0x57DBDE].
		if (env::is_env_key(key) && key != "enviro_name" && key != "vertex_rgb") {
			if (!held_environment_key(key)) {
				issue(true, line, key,
				      "'" + std::string(tokens.tokens[0]) +
				              "' is an environment keyword: the game's environment reader reads the terrain's lines too and "
				              "takes it for the mission's environment before the .env, which the editor's terrain cannot "
				              "keep (it holds the water's keywords alone). Move it to the mission's .env.");
				return;
			}
			if (key == "water_rgb") {
				if (tokens.count < 4)
					issue(false, line, key,
					      "This colour line has " + std::to_string(tokens.count - 1) +
					              " of its three values: the game reads the missing ones from where earlier, longer lines "
					              "left them, and a save writes the three it read.");
				for (int c = 1; c <= 3; ++c) {
					const int read = io::retail_atol(tokens.token(c));
					if (read < 0 || read > 255) {
						issue(false, line, key,
						      "The game holds a colour's byte: it reads " + std::to_string(read) + " as " +
						              std::to_string(std::clamp(read, 0, 255)) + ", and a save writes that.");
						break;
					}
				}
			}
			if (key == "water_murk" && static_cast<float>(io::retail_atof(value)) > 0.99f)
				issue(false, line, key, "The game reads a murk past 0.99 as 0.99: a save writes 0.99.");
			if (in_foliage) return;
		}
		if (in_foliage) {
			if (key == "end") {
				in_foliage = false;
				++closed;
			} else if (key == "match") {
				if (tokens.count - 1 > FOLIAGE_MATCH_CODES)
					issue(false, line, key,
					      "The game keeps up to seven codes and compares the first four: the rest are never used, and a "
					      "save leaves them out.");
				for (int i = 1; i < tokens.count && i <= FOLIAGE_MATCH_CODES; ++i) {
					const int read = io::retail_atol(tokens.tokens[i]);
					if (read < 0 || read > 255) {
						issue(false, line, key,
						      "The game keeps each code as a byte: it reads " + std::to_string(read) + " as " +
						              std::to_string(uint8_t(read)) + ", and a save writes that.");
						break;
					}
				}
			} else if (key == "attrib") {
				for (int i = 1; i < tokens.count && i < 8; ++i)
					if (!strutil::iequals(tokens.tokens[i], "forceon") && !strutil::iequals(tokens.tokens[i], "shadow")) {
						issue(false, line, key,
						      "The game reads nothing of '" + std::string(tokens.tokens[i]) +
						              "' (forceon and shadow alone): a save leaves it out.");
						break;
					}
			} else if (key == "color_lower" || key == "color_upper") {
				const int read = io::retail_atol(value);
				if (read < 0 || read > 2)
					issue(false, line, key,
					      "The editor holds a colour mode of 0 to 2, the shipped files' words; the game's generator writes "
					      "over the colour before it draws, so a save writing " +
					              std::to_string(std::clamp(read, 0, 2)) + " changes nothing drawn.");
			} else if (key != "graphic") {
				issue(false, line, key,
				      "Inside a foliage block the game's terrain reader reads graphic, match, color_lower, color_upper, "
				      "attrib and end alone: it skips '" +
				              std::string(tokens.tokens[0]) + "', and a save leaves the line out.");
			}
			return;
		}
		if (key == "foliage") {
			if (closed >= FOLIAGE_MAX_DEFS) {
				swallowed = true;
				issue(false, line, key,
				      "The game reads four foliage blocks: from this fifth one on, its terrain reader reads nothing more "
				      "of the file, and a save leaves it all out.");
				return;
			}
			in_foliage = true;
			return;
		}
		if (key == "polytrn_sectors") {
			++row_lines;
			if (row_lines == kSide + 1) row_line_17 = line;
			const int columns = tokens.count - 1;
			const int width = std::min(sector_count, kSide);
			if (row_lines > kSide) return;
			if (width <= 0 && columns > 0)
				issue(false, line, "polytrn_sectors",
				      "This grid row comes before the grid's width (polytrn_sectorcount), so the game reads none of its "
				      "sectors: a save writes the row as the game read it.");
			else if (columns < width)
				issue(false, line, "polytrn_sectors",
				      "This grid row has " + std::to_string(columns) + " of the width's " + std::to_string(width) +
				              " sectors: the game reads the rest from where earlier, longer lines left them, and a save "
				              "writes the " +
				              std::to_string(width) + " it read.");
			else if (columns > width && width > 0)
				issue(false, line, "polytrn_sectors",
				      "This grid row has " + std::to_string(columns) + " sectors past the width's " + std::to_string(width) +
				              ": the game reads none of the rest, and a save leaves them out.");
			return;
		}
		if (key == "polytrn_sectorcount") sector_count = io::retail_atol(value);
		if (key == "polytrn_scale") {
			// Read for the multiplayer check alone [orig: Terrain_LoadEnvironmentConfig @ 0x61096D, its default;
			// @ 0x60C5FD feeds the CRC]: the record does not hold it.
			issue(true, line, key,
			      "polytrn_scale feeds only the multiplayer file check, which the editor's terrain cannot keep: remove the "
			      "line, or keep editing the file as a text.");
			return;
		}
		if (!terrain_reader_key(key) && !held_environment_key(key)) {
			issue(false, line, key,
			      "The game's readers skip '" + std::string(tokens.tokens[0]) + "': a save leaves the line out.");
			return;
		}
		// A key read again: the last line wins.
		const auto first = first_line.emplace(key, line);
		if (!first.second) {
			issue(false, first.first->second, key,
			      "'" + key + "' is written again on line " + std::to_string(line) +
			              ": the game reads the last, and a save writes that one alone.");
			first.first->second = line;
		}
	});
	if (row_lines > kSide)
		issue(false, row_line_17, "polytrn_sectors",
		      "The grid has " + std::to_string(row_lines) +
		              " rows: the game refuses a terrain of more than 16, and a save writes the first 16.");
	std::stable_sort(issues.begin(), issues.end(), [](const SourceIssue &a, const SourceIssue &b) { return a.line < b.line; });
}

// --- the row --------------------------------------------------------------------------------------

TerrainRow::TerrainRow() { kind = kTerrain; }

RecordHandle TerrainRow::record() const { return {kTerrain, const_cast<TerrainRow *>(this)}; }

size_t TerrainRow::footprint() const {
	size_t bytes = sizeof(*this) + footprint_of(config.name) + footprint_of(config.creator) +
	               footprint_of(config.colormap) + footprint_of(config.detailmap_c1) + footprint_of(config.detailmap_c2) +
	               footprint_of(config.detailmap_c3) + footprint_of(config.detailblendmap) + footprint_of(config.polydata) +
	               footprint_of(config.detailmap) + footprint_of(config.detailmap2) + footprint_of(config.detailmapdist) +
	               footprint_of(config.detailmapdist2) + footprint_of(config.foliagemap) + footprint_of(config.charmap) +
	               footprint_of(config.tilestrip) + footprint_of(config.tileinfo) + ids_footprint();
	for (const FoliageDef &def : config.foliage_defs) bytes += sizeof(def) + def.graphic.size();
	return bytes;
}

const RecordTable &terrain_table() {
	static const RecordTable table = make_table();
	return table;
}

const FieldSchema *terrain_key_field(const std::string &key, int32_t *loader_arg) {
	if (loader_arg) *loader_arg = -1;
	for (const NodeKind kind : {kTerrain, kFoliage})
		for (const FieldSchema &field : terrain_table().fields(kind)) {
			if (field.id != key && field.token != key) continue;
			if (loader_arg && field.reference == ReferenceKind::Texture)
				for (const MapRole &map : kMapRoles)
					if (field.id == map.id) *loader_arg = texture_role_arg(map.role, map.flags);
			return &field;
		}
	return nullptr;
}

bool is_terrain_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::Terrain; }

std::string terrain_refusal(const TrnConfig &config) {
	// [orig: Terrain_LoadEnvironmentConfig @ 0x610940, its tail: the colormap (+256), detailmap (+512) and polydata
	// (+3072) names, then the row count (+5960) and the width (+5956), each at most 16 and a power of two]. The
	// writer writes one row where the record holds none.
	if (config.colormap.empty()) return "The colour map (polytrn_colormap) is not named: the game refuses the terrain.";
	if (config.detailmap.empty()) return "The detail map (polytrn_detailmap) is not named: the game refuses the terrain.";
	if (config.polydata.empty()) return "The height data (polytrn_polydata) is not named: the game refuses the terrain.";
	const int rows = std::max(1, config.sector_rows);
	if (rows > kSide || !power_of_two_or_zero(rows))
		return "The grid has " + std::to_string(rows) + " rows: the game refuses a terrain whose rows are not a power of "
		       "two of at most 16.";
	if (config.sector_count > kSide || !power_of_two_or_zero(config.sector_count))
		return "The grid's width is " + std::to_string(config.sector_count) +
		       ": the game refuses a terrain whose width is not a power of two of at most 16.";
	return std::string();
}

// --- the document ---------------------------------------------------------------------------------

const TerrainRow *TerrainDocument::terrain_row() const {
	for (const auto &row : rows())
		if (row && row->kind == kTerrain) return static_cast<const TerrainRow *>(row.get());
	return nullptr;
}

const TrnConfig *TerrainDocument::config() const {
	const TerrainRow *row = terrain_row();
	return row ? &row->config : nullptr;
}

std::string TerrainDocument::refused() const {
	const TrnConfig *held = config();
	return held ? terrain_refusal(*held) : std::string();
}

std::string TerrainDocument::record_title(const NodeAddress &address) const {
	if (address.child && address.kind == kFoliage) {
		Value graphic;
		const std::string name = record_name(address);
		if (get(address, "graphic", graphic))
			if (const auto *text = std::get_if<std::string>(&graphic); text && !text->empty()) return name + ": " + *text;
		return name;
	}
	if (address.child && address.kind == kSectorRow) {
		Placement at;
		if (placement(address, at)) return "Row " + std::to_string(at.index + 1);
	}
	return Document::record_title(address);
}

void TerrainDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	if (use.reference != ReferenceKind::Texture || !use.schema) return;
	for (const MapRole &map : kMapRoles)
		if (use.schema->id == map.id) {
			use.loader_arg = texture_role_arg(map.role, map.flags);
			return;
		}
}

bool TerrainDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                            std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_terrain_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a terrain.", path());
		return false;
	}
	const std::string text(bytes.begin(), bytes.end());
	auto row = std::make_shared<TerrainRow>();
	std::istringstream input(text);
	std::string message;
	// A terrain the game's gate refuses still opens, as read up to the gate (load_trn fills the record
	// before it): its refusal is a finding of the record (terrain.refused), which an edit sets right.
	load_trn(input, row->config, message);
	terrain_source_issues(text, issues);
	shape(*row);
	rows.push_back(row);
	return true;
}

SerializeResult TerrainDocument::serialize() const {
	SerializeResult result;
	const TrnConfig *held = config();
	if (!held) {
		result.issues.push_back({true, 0, "", "", "The terrain holds no row."});
		return result;
	}
	std::ostringstream output;
	std::string error;
	if (!save_trn(output, *held, error)) {
		result.issues.push_back({true, 0, "", "", error});
		return result;
	}
	result.text = output.str();
	return result;
}

std::string TerrainDocument::save_words() const {
	std::string words = "Saving writes the terrain in the editor's layout: each key the game reads on a line of its own "
	                    "in a fixed order, the grid's rows, then the foliage blocks";
	const size_t ignored = ignored_lines();
	if (ignored)
		words += ", without the " + std::to_string(ignored) + (ignored == 1 ? " thing" : " things") +
		         " the game reads otherwise or skips (Problems lists each)";
	return words + ". The file's comments and spacing are not kept; the game reads the same terrain.";
}

std::shared_ptr<Node> TerrainDocument::make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                                 std::string &error) {
	error = "A terrain keeps its one row; add grid rows and foliage inside it.";
	return nullptr;
}

bool TerrainDocument::accept_step(const EditStep &, const StagedRows &staged, StepRefusal &refusal) const {
	// One row after any step: an edit inside it, or the file read again in its place (Restore CR LF line ends).
	if (staged.rows().size() == 1) return true;
	refusal.message = "A terrain keeps its one row.";
	return false;
}

// --- the findings ---------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<TerrainFinding> kFindingEntries[] = {
	{ TerrainFinding::InvalidInput, { "terrain.invalid_input", FindingFix::None, nullptr, true } },
	{ TerrainFinding::IgnoredInput, { "terrain.ignored_input", FindingFix::Rewrite, "with each line as the game reads it" } },
	// The gate's refusal aborts the mission's load [orig: Game_StartMission @ 0x524780..0x52479F, @ 0x524B30]: it
	// gates the build.
	{ TerrainFinding::Refused, { "terrain.refused", FindingFix::EditRecord } },
	// What the game reads then is the config's own bytes, not a refusal: listed.
	{ TerrainFinding::NoWidth, listed_code("terrain.no_width", FindingFix::EditRecord) },
	{ TerrainFinding::FoliageInert, listed_code("terrain.foliage_inert", FindingFix::EditRecord) },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(TerrainFinding::kCount),
              "every TerrainFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
              "the terrain's rows follow TerrainFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Terrains);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

Edit set_edit(const NodeAddress &at, const char *field, Value value) {
	Edit edit;
	edit.address = at;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

} // namespace

const FindingCodeRow &finding_code(TerrainFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable terrain_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_terrain_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *terrain = dynamic_cast<const TerrainDocument *>(&document);
	if (!terrain) return findings;
	source_issue_findings(*terrain, finding_code(TerrainFinding::InvalidInput), finding_code(TerrainFinding::IgnoredInput),
	                      findings);
	if (document.blocked()) return findings;
	const TerrainRow *row = terrain->terrain_row();
	if (!row) return findings;
	const TrnConfig &config = row->config;
	const NodeAddress at{row->id, kTerrain, 0};
	const auto add = [&](TerrainFinding code, DiagnosticSeverity severity, const std::string &message, const char *field,
	                     const NodeAddress &record) -> Diagnostic & {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = record.row;
		d.child_id = record.child;
		d.record_kind = record.child ? record.kind : kTerrain;
		d.record = record.child ? terrain->record_path(record) : row->name();
		findings.push_back(std::move(d));
		return findings.back();
	};
	// The gate, in its order; its fixes where the game's own rule says what the value becomes.
	const std::string refusal = terrain_refusal(config);
	if (!refusal.empty()) {
		const char *field = config.colormap.empty()    ? "polytrn_colormap"
		                    : config.detailmap.empty() ? "polytrn_detailmap"
		                    : config.polydata.empty()  ? "polytrn_polydata"
		                    : (std::max(1, config.sector_rows) > kSide || !power_of_two_or_zero(std::max(1, config.sector_rows)))
		                            ? ""
		                            : "polytrn_sectorcount";
		Diagnostic &d = add(TerrainFinding::Refused, DiagnosticSeverity::Error,
		                    refusal + " A mission on it stops loading and returns to the menus.", field, at);
		const int rows = std::max(1, config.sector_rows);
		if (std::string(field).empty() && rows < kSide) {
			// Filled to the next power of two with the rows the game's extension reads past the last (the last again,
			// or the grid's rows again from the first where it wraps north-south) [orig: Terrain_ShiftHeightmapRows @
			// 0x60F2A5..0x60F317]: the ground the game would draw past the rows does not move.
			const int target = next_power_of_two(rows);
			PlannedFix fix;
			fix.label = "Fill the grid to " + std::to_string(target) + " rows";
			fix.detail = "Adds " + std::to_string(target - rows) + (target - rows == 1 ? " row" : " rows") +
			             " at the grid's end, each the row the game's extension reads there (" +
			             (config.wrap_y ? "the grid's rows again from the first" : "the last row again") +
			             "), so the grid's rows are a power of two and the game takes the terrain; nothing it draws moves.";
			for (int r = rows; r < target; ++r) {
				Edit edit;
				edit.operation = EditOperation::Add;
				edit.address = {row->id, kSectorRow, 0};
				fix.edits.push_back(std::move(edit));
			}
			d.planned.push_back(std::move(fix));
		} else if (std::string(field) == "polytrn_sectorcount" && config.sector_count > 0) {
			const int target = next_power_of_two(config.sector_count);
			d.planned.push_back({"Set the width to " + std::to_string(target),
			                     "Sets polytrn_sectorcount to " + std::to_string(target) +
			                             ", the next power of two: each row's cells past " +
			                             std::to_string(std::min(config.sector_count, kSide)) +
			                             " are written as the record holds them.",
			                     {set_edit(at, "polytrn_sectorcount", int64_t(target))}});
		}
	} else if (config.sector_count == 0 && !config.wrap_x) {
		// With no width and no wrap the extension copies the config's bytes before the grid into it (terrain-re.md
		// "A terrain from images" [orig: Terrain_ShiftHeightmapRows @ 0x60F2A5..0x60F317]).
		Diagnostic &d = add(TerrainFinding::NoWidth, DiagnosticSeverity::Warning,
		                    "The grid's width is 0: the game reads no sector of any row, and its extension fills the grid "
		                    "with the bytes of the settings before it, so where the ground lies is not the file's.",
		                    "polytrn_sectorcount", at);
		d.planned.push_back({"Set the width to 1",
		                     "Sets polytrn_sectorcount to 1: each row's first cell is its one sector, as the record holds it.",
		                     {set_edit(at, "polytrn_sectorcount", int64_t(1))}});
	}
	// A definition that grows nothing [orig: Foliage_RemapPixelToDefMask @ 0x5FF4E0: a slot whose graphic is
	// unnamed is skipped, and pixel 0 never matches].
	const std::vector<Document::Collection> lists = terrain->collections_of(at);
	for (size_t i = 0; i < config.foliage_defs.size(); ++i) {
		const FoliageDef &def = config.foliage_defs[i];
		bool codes = false;
		for (int code : def.match) codes = codes || code > 0;
		if (!def.graphic.empty() && codes) continue;
		NodeAddress record = at;
		for (const Document::Collection &list : lists)
			if (list.spec.kind == kFoliage && i < list.ids.size()) record = {row->id, kFoliage, list.ids[i]};
		const std::string why = def.graphic.empty() ? "names no model, and the game skips a definition with none"
		                                            : "holds no code but 0, which the foliage map's texels never match";
		Diagnostic &d = add(TerrainFinding::FoliageInert, DiagnosticSeverity::Warning,
		                    "Foliage " + std::to_string(i + 1) + " grows nothing: it " + why + ".",
		                    def.graphic.empty() ? "graphic" : "match_1", record);
		if (record.child) {
			Edit remove;
			remove.operation = EditOperation::Remove;
			remove.address = record;
			d.planned.push_back({"Remove it",
			                     "Removes foliage " + std::to_string(i + 1) +
			                             ": nothing the game grows changes, since it grows nothing.",
			                     {remove}});
		}
	}
	return findings;
}

} // namespace opennova::editor
