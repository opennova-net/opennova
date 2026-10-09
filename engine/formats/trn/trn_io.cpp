#include <formats/trn/trn_io.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/env/env.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <sstream>

// [orig: PolyTrn_LoadTerrainConfig @0x60e3d0 -> Terrain_ParseConfigCallback @0x60f330 — the .trn
//  key parser (foliage attribs: "forceon" @0x60f58b; `match` stores up to 7 byte args per
//  slot at +0x108..); Terrain_LoadEnvironmentConfig @0x610940 — the admission gate at its tail]

namespace opennova {

namespace {

// The terrain parser's state, which lives across every file one load walks: inside a `foliage` block, the
// blocks an `end` has closed [orig: dword_31BC904, dword_31BC900, zeroed once @0x6109b4..0x6109ba before the
// three passes], the block being read, the `polytrn_sectors` lines read [orig: dword_31BCB30], and the
// tokenizer's slots, which retail's static tokenizer carries from file to file (ascii_config.h).
struct TrnWalk {
	bool in_foliage = false;
	int foliage_closed = 0;
	FoliageDef def;
	int seen_sector_rows = 0;
	io::ConfigTokens tokens;
};

// One file's lines through the terrain's parser. `own`: the .trn's, whose environment and editor keywords
// TrnConfig holds as written (trn.h); a later file's are the environment load's alone. `took` hears each
// line an arm took (its keyword lower case, its 1-based line, its tokens).
template <typename Took>
void walk_trn(const std::string &text, TrnConfig &out, TrnWalk &walk, bool own, Took &&took) {
	bool &in_foliage = walk.in_foliage;
	int &foliage_closed = walk.foliage_closed;
	FoliageDef &def = walk.def;
	int line = 0;

	// The lines and tokens are the shared retail walk's (a CR LF pair ends a line and nothing else does, an
	// unterminated last line loses its final byte, `;` or `//` outside quotes cuts a line, and space, comma or
	// tab separates tokens), the callback reached by a line with a token whose first does not start with '/'
	// [orig: File_ParseASCIIFile @0x53D810, @0x53D915, @0x53D91E]. A key compares without case and reads its
	// values by token, numbers through the CRT's atol and atof (io::retail_atol, io::retail_atof), whatever the
	// line's count [orig: Terrain_ParseConfigCallback @0x60f330, the stricmp of tokens[1] in every arm].
	io::for_each_config_line_span(text.data(), text.size(), walk.tokens,
			[&](const io::ConfigTokens &tokens, const io::ConfigLineSpan &) {
		++line;
		if (tokens.count == 0 || tokens.tokens[0][0] == '/') return;
		const std::string key = strutil::to_lower(tokens.tokens[0]);
		const char *value = tokens.token(1);

		// Inside a block every line is the block's (a `foliage` line too): the
		// first four blocks read their keys into their slots, and from the fifth
		// on nothing closes the block, so the rest of the load is read by no arm
		// [orig: the dword_31BC904 test, then `dword_31BC900 < 4` around every
		// block key, `end` bumping the count].
		if (in_foliage) {
			if (foliage_closed >= 4) return;
			if (key == "end") {
				out.foliage_defs.push_back(foliage_normalize_def(def));
				++foliage_closed;
				in_foliage = false;
			} else if (key == "graphic") {
				def.graphic = value;
			} else if (key == "color_lower") {
				def.color_lower = io::retail_atol(value);
			} else if (key == "color_upper") {
				def.color_upper = io::retail_atol(value);
			} else if (key == "match") {
				// Up to 7 args as bytes, to the line's count; only the first
				// FOLIAGE_MATCH_CODES are ever consumed (foliage.h), so the rest
				// are read and dropped here [orig: the match arm, `idx < 8` and
				// `idx >= count` @0x60f330].
				def.match.fill(FOLIAGE_MATCH_UNSET);
				for (int i = 1; i < tokens.count && i < 8; ++i)
					if (i - 1 < FOLIAGE_MATCH_CODES)
						def.match[static_cast<size_t>(i - 1)] =
								static_cast<uint8_t>(io::retail_atol(tokens.tokens[i]));
			} else if (key == "attrib") {
				// Each arg ORs its flag; nothing clears them [orig: the attrib
				// arm, "forceon" @0x60f58b ORs 1 and "shadow" 2].
				for (int i = 1; i < tokens.count && i < 8; ++i) {
					if (strutil::iequals(tokens.tokens[i], "forceon"))
						def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_FORCE_ON);
					if (strutil::iequals(tokens.tokens[i], "shadow"))
						def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_SHADOW);
				}
			} else {
				return;
			}
			took(key, line, tokens);
			return;
		}

		if (own) {
			// The keys no arm of the terrain's parser reads, which the .trn holds for the editor and the
			// environment's load (trn.h).
			if (key == "terrain_name") {
				out.name = value;
				return;
			}
			if (key == "terrain_creator") {
				out.creator = value;
				return;
			}
			if (key == "water_height") {
				out.water_height = io::retail_atol(value);
				return;
			}
			if (key == "water_rgb") {
				// The environment reader's arms over the terrain's lines (trn.h): a colour's three bytes as
				// atol of tokens 2..4 packed at the load's envscale of 1, held to a byte [orig:
				// TimeOfDay_ParseProperty @ 0x57caf6; Color_ScaleRGBAndPack @ 0x57f890], the murk's atof held
				// at 0.99 [orig: TimeOfDay_ParseProperty @ 0x57cb7c..0x57cba9].
				out.water_rgb_set = true;
				for (int c = 0; c < 3; ++c)
					out.water_rgb[static_cast<size_t>(c)] = std::clamp(io::retail_atol(tokens.token(1 + c)), 0, 255);
				return;
			}
			if (key == "water_murk") {
				out.water_murk_set = true;
				out.water_murk = std::min(static_cast<float>(io::retail_atof(value)), 0.99f);
				return;
			}
		}

		if (key == "foliage") {
			in_foliage = true;
			def = FoliageDef();
		} else if (key == "polytrn_colormap") {
			out.colormap = value;
		} else if (key == "polytrn_detailmap_c1") {
			out.detailmap_c1 = value;
		} else if (key == "polytrn_detailmap_c2") {
			out.detailmap_c2 = value;
		} else if (key == "polytrn_detailmap_c3") {
			out.detailmap_c3 = value;
		} else if (key == "polytrn_detailblendmap") {
			out.detailblendmap = value;
		} else if (key == "polytrn_polydata") {
			out.polydata = value;
		} else if (key == "polytrn_detailmap") {
			out.detailmap = value;
		} else if (key == "polytrn_detailmap2") {
			out.detailmap2 = value;
		} else if (key == "polytrn_detailmapdist") {
			out.detailmapdist = value;
		} else if (key == "polytrn_detailmapdist2") {
			out.detailmapdist2 = value;
		} else if (key == "polytrn_detaildensity") {
			out.detail_density = io::retail_atol(value);
		} else if (key == "polytrn_detaildensity2") {
			out.detail_density2 = io::retail_atol(value);
		} else if (key == "polytrn_sectorcount") {
			out.sector_count = io::retail_atol(value);
		} else if (key == "polytrn_wrapx") {
			out.wrap_x = io::retail_atol(value);
		} else if (key == "polytrn_wrapy") {
			out.wrap_y = io::retail_atol(value);
		} else if (key == "lock_topleft") {
			out.lock_topleft.x = io::retail_atol(value);
			out.lock_topleft.y = io::retail_atol(tokens.token(2));
		} else if (key == "lock_topright") {
			out.lock_topright.x = io::retail_atol(value);
			out.lock_topright.y = io::retail_atol(tokens.token(2));
		} else if (key == "lock_bottomleft") {
			out.lock_bottomleft.x = io::retail_atol(value);
			out.lock_bottomleft.y = io::retail_atol(tokens.token(2));
		} else if (key == "lock_bottomright") {
			out.lock_bottomright.x = io::retail_atol(value);
			out.lock_bottomright.y = io::retail_atol(tokens.token(2));
		} else if (key == "polytrn_origin") {
			out.origin_x = io::retail_atol(value);
			out.origin_y = io::retail_atol(tokens.token(2));
		} else if (key == "polytrn_sectors") {
			// A row reads polytrn_sectorcount columns (16 at most) whatever the
			// line's count, a short line's missing ones from the slots an earlier
			// line left; every row line counts (+5960), and the gate below
			// rejects more than 16, which the grid has no room for, so they are
			// counted, not stored [orig: the polytrn_sectors arm @0x60f330,
			// dword_31BCB30].
			++walk.seen_sector_rows;
			if (out.sector_rows < kTerrainGridSide) {
				for (int col = 0; col < kTerrainGridSide && col < out.sector_count; ++col)
					out.sector_grid[out.sector_rows][col] = io::retail_atol(tokens.token(1 + col));
				out.sector_rows++;
			}
		} else if (key == "polytrn_charmap") {
			out.charmap = value;
		} else if (key == "polytrn_foliagemap") {
			out.foliagemap = value;
		} else if (key == "polytrn_tilestrip") {
			out.tilestrip = value;
		} else if (key == "polytrn_tileinfo") {
			out.tileinfo = value;
		} else {
			return;
		}
		took(key, line, tokens);
	});
}

// The tokens after a line's keyword, a space apart.
std::string values_of(const io::ConfigTokens &tokens) {
	std::string value;
	for (int i = 1; i < tokens.count; ++i) {
		if (i > 1) value += ' ';
		value += tokens.tokens[i];
	}
	return value;
}

void ignore_line(const std::string &, int, const io::ConfigTokens &) {}

bool finish_trn(TrnConfig &out, TrnWalk &walk, std::string &error) {
	// A block no `end` closed (the load ran out, or its `end` is the
	// unterminated last line, read `en`) still wrote its keys into its slot, and
	// the runtime takes all four slots whose graphic is named, not the closed
	// count, so the block is a definition [orig: the block keys write slot
	// dword_31BC900 directly; Terrain_Init copies all four 0x218-byte slots
	// @0x60FD11..0x60FD16 (sub_5FF4C0); Foliage_RemapPixelToDefMask @0x5FF4E0
	// gates each slot on its graphic's first byte]. JO:CA's Dvxi4.trn and
	// Dvxi4_c.trn end on such a block.
	if (walk.in_foliage && walk.foliage_closed < 4) out.foliage_defs.push_back(foliage_normalize_def(walk.def));

	// The admission gate over the row lines the walk read (trn_refusal). Its last leg,
	// `Terrain_ShiftHeightmapRows @0x60f190`, never rejects: it extends the
	// parsed grid to 16 columns and rows (periodically under wrap, else the
	// last column and row copied outward) and returns 0 on every path
	// [orig: @0x60F2A5..0x60F317, @0x60F31A]; the extension below is it.
	switch (trn_refusal(out, walk.seen_sector_rows)) {
	case TrnRefusal::None: break;
	case TrnRefusal::NoColormap: error = "TRN rejected: polytrn_colormap is empty"; return false;
	case TrnRefusal::NoDetailmap: error = "TRN rejected: polytrn_detailmap is empty"; return false;
	case TrnRefusal::NoPolydata: error = "TRN rejected: polytrn_polydata is empty"; return false;
	case TrnRefusal::SectorRows:
		error = "TRN rejected: polytrn_sectors row count " + std::to_string(walk.seen_sector_rows) +
			" is not a power of two <= 16";
		return false;
	case TrnRefusal::SectorCount:
		error = "TRN rejected: polytrn_sectorcount " + std::to_string(out.sector_count) +
			" is not a power of two <= 16";
		return false;
	}

	const int rows = std::max(out.sector_rows, 1);
	const int cols = std::max(out.sector_count, 1);

	for (int r = 0; r < rows; ++r) {
		for (int c = cols; c < kTerrainGridSide; ++c) {
			out.sector_grid[r][c] = out.wrap_x ? out.sector_grid[r][c % cols] : out.sector_grid[r][cols - 1];
		}
	}
	for (int r = rows; r < kTerrainGridSide; ++r) {
		for (int c = 0; c < kTerrainGridSide; ++c) {
			out.sector_grid[r][c] = out.wrap_y ? out.sector_grid[r % rows][c] : out.sector_grid[rows - 1][c];
		}
	}
	return true;
}

} // namespace

TrnRefusal trn_refusal(const TrnConfig &config, int sector_rows) {
	// The admission gate [orig: Terrain_LoadEnvironmentConfig @0x610940 tail]:
	// the config is rejected (returns 0) when the colormap (+256), detailmap
	// (+512) or polydata (+3072) name is empty, when the `polytrn_sectors` row
	// count (+5960) or `polytrn_sectorcount` (+5956) exceeds 16, or when either
	// is not a power of two (`((n - 1) & n) != 0`).
	if (config.colormap.empty()) return TrnRefusal::NoColormap;
	if (config.detailmap.empty()) return TrnRefusal::NoDetailmap;
	if (config.polydata.empty()) return TrnRefusal::NoPolydata;
	const auto power_of_two_or_zero = [](int n) {
		const uint32_t u = static_cast<uint32_t>(n); // the 32-bit wrap of the original's `n - 1`
		return ((u - 1u) & u) == 0u;
	};
	if (sector_rows > kTerrainGridSide || !power_of_two_or_zero(sector_rows)) return TrnRefusal::SectorRows;
	if (config.sector_count > kTerrainGridSide || !power_of_two_or_zero(config.sector_count))
		return TrnRefusal::SectorCount;
	return TrnRefusal::None;
}

bool load_trn(std::istream &f, TrnConfig &out, std::string &error) {
	const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	TrnWalk walk;
	walk_trn(text, out, walk, true, ignore_line);
	return finish_trn(out, walk, error);
}

bool load_mission_trn(const std::string &terrain, const TrnLaterTexts &later, TrnConfig &out, std::string &error,
		std::vector<TrnLaterLine> *taken) {
	out = TrnConfig();
	if (taken != nullptr) taken->clear();
	TrnWalk walk;
	walk_trn(terrain, out, walk, true, ignore_line);
	const auto pass = [&](const std::string *text, TrnLaterLine::File file) {
		if (text == nullptr) return;
		walk_trn(*text, out, walk, false, [&](const std::string &key, int line, const io::ConfigTokens &tokens) {
			if (taken != nullptr) taken->push_back(TrnLaterLine{ file, line, key, values_of(tokens) });
		});
	};
	// overcast.def after the .trn, then the .env [orig: Environment_LoadTimeOfDayConfig @ 0x57dc3b, @ 0x57dcbf].
	pass(later.overcast, TrnLaterLine::File::Overcast);
	pass(later.environment, TrnLaterLine::File::Environment);
	return finish_trn(out, walk, error);
}

bool read_mission_trn(const TrnTextReader &read, const std::string &terrain_file, const std::string &environment_file,
		TrnConfig &out, std::string &error, std::vector<TrnLaterLine> *taken) {
	std::string terrain, overcast, environment;
	if (terrain_file.empty() || !read(terrain_file, terrain)) {
		out = TrnConfig();
		if (taken != nullptr) taken->clear();
		error = (terrain_file.empty() ? std::string("the mission names no terrain") : terrain_file + " is not there");
		return false;
	}
	TrnLaterTexts later;
	if (read(env::kOvercastFile, overcast)) later.overcast = &overcast;
	if (!environment_file.empty() && read(environment_file, environment)) later.environment = &environment;
	return load_mission_trn(terrain, later, out, error, taken);
}

namespace {

// Every keyword an arm compares out of a block, then in one [orig: Terrain_ParseConfigCallback @0x60f330, the
// block's arms @0x60f36c..0x60f5f0, the others from @0x60f5fa].
const char *const kParserKeys[] = { "foliage", "polytrn_colormap", "polytrn_detailmap", "polytrn_detailmap_c1",
	"polytrn_detailmap_c2", "polytrn_detailmap_c3", "polytrn_detailmap2", "polytrn_detailmapdist",
	"polytrn_detailmapdist2", "polytrn_detailblendmap", "polytrn_charmap", "polytrn_depthmap", "polytrn_polydata",
	"polytrn_tilestrip", "polytrn_foliagemap", "polytrn_tileinfo", "polytrn_wrapx", "polytrn_wrapy",
	"polytrn_detaildensity", "polytrn_detaildensity2", "polytrn_sectorcount", "polytrn_sectors", "polytrn_origin",
	"polytrn_scale", "lock_topleft", "lock_topright", "lock_bottomleft", "lock_bottomright" };
const char *const kParserBlockKeys[] = { "end", "graphic", "stampdown_file", "stampdown_color", "stampdown_radius",
	"color_lower", "color_upper", "match", "attrib" };

} // namespace

bool trn_parser_key(const std::string &key, bool block) {
	const auto in = [&key](const auto &keys) {
		return std::any_of(std::begin(keys), std::end(keys), [&key](const char *k) { return key == k; });
	};
	return in(kParserKeys) || (block && in(kParserBlockKeys));
}

std::vector<std::string> trn_parser_keys(bool block) {
	std::vector<std::string> keys(std::begin(kParserKeys), std::end(kParserKeys));
	if (block) keys.insert(keys.end(), std::begin(kParserBlockKeys), std::end(kParserBlockKeys));
	return keys;
}

std::string trn_parser_lines(const std::string &text) {
	std::string lines;
	io::for_each_config_line(text.data(), text.size(), [&](const io::ConfigTokens &tokens) {
		if (!trn_parser_key(strutil::to_lower(tokens.tokens[0]), true)) return;
		// Every slot a short line reads stays part of it (a sector row's columns run to 16).
		for (int i = 0; i < io::kConfigMaxTokens; ++i) {
			lines += tokens.token(i);
			lines += '\x1f';
		}
		lines += '\n';
	});
	return lines;
}

std::vector<TrnKeyLine> read_trn_key_lines(const std::string &text, std::vector<int> *lines) {
	std::vector<TrnKeyLine> out;
	if (lines != nullptr) lines->clear();
	io::ConfigTokens walk;
	int line = 0;
	io::for_each_config_line_span(text.data(), text.size(), walk, [&](const io::ConfigTokens &tokens, const io::ConfigLineSpan &) {
		++line;
		// The callback's gate [orig: File_ParseASCIIFile @0x53D915, @0x53D91E].
		if (tokens.count == 0 || tokens.tokens[0][0] == '/') return;
		TrnKeyLine read;
		read.key = strutil::to_lower(tokens.tokens[0]);
		if (!trn_parser_key(read.key, true)) return;
		for (int i = 1; i < tokens.count; ++i) read.values.emplace_back(tokens.tokens[i]);
		out.push_back(std::move(read));
		if (lines != nullptr) lines->push_back(line);
	});
	return out;
}

std::vector<std::string> trn_key_readings(const std::vector<TrnKeyLine> &keys) {
	// The terrain's parser's block state over the file's own lines: inside a block every line is the block's, the
	// first four blocks read their keys, `end` closes one, and from the fifth on nothing closes it [orig:
	// Terrain_ParseConfigCallback @ 0x60F330, its block test of dword_31BC904, `dword_31BC900 < 4` around every block
	// key, the block's arms @ 0x60F36C..0x60F5F0]. A terrain may close blocks before the file, or leave one open into
	// it; what the file alone shows is said.
	std::vector<std::string> out(keys.size());
	bool in_block = false;
	int closed = 0;
	for (size_t i = 0; i < keys.size(); ++i) {
		const std::string &key = keys[i].key;
		const bool block_key = trn_parser_key(key, true) && !trn_parser_key(key);
		if (in_block) {
			if (closed >= 4) {
				out[i] = "This line is inside a fifth foliage block, which the terrain's reader opens but reads nothing of: "
				         "no end closes it, so no arm reads a line after it [orig: Terrain_ParseConfigCallback @ 0x60F330, "
				         "`dword_31BC900 < 4` around every block key].";
			} else if (key == "end") {
				in_block = false;
				++closed;
			} else if (!block_key) {
				out[i] = "This line is inside the foliage block a foliage line above opens: the terrain's reader reads a "
				         "block's lines as the block's, so it skips this one until an end closes the block [orig: "
				         "Terrain_ParseConfigCallback @ 0x60F330, its block test of dword_31BC904].";
			}
			continue;
		}
		if (key == "foliage") {
			in_block = true;
			if (closed >= 4)
				out[i] = "A fifth foliage block: the terrain's reader opens it but reads none of its lines, and nothing "
				         "closes it, so no arm reads a line after it [orig: Terrain_ParseConfigCallback @ 0x60F330, "
				         "`dword_31BC900 < 4` around every block key].";
		} else if (block_key) {
			out[i] = "'" + key +
			         "' is a foliage block's keyword: the terrain's reader reads it only inside a block, and no foliage "
			         "line above opens one in this file, so the game takes it only on a terrain whose .trn ends inside "
			         "a block [orig: Terrain_ParseConfigCallback @ 0x60F330, its block test of dword_31BC904].";
		} else if (key == "polytrn_sectors") {
			out[i] = "A grid row here is a row after the terrain's own, not in place of one: the terrain's reader counts "
			         "every polytrn_sectors line of the load, the .trn's first, and refuses the terrain where the count "
			         "is past 16 or not a power of two [orig: Terrain_ParseConfigCallback @ 0x60F330, its row count "
			         "dword_31BCB30, which no pass resets; Terrain_LoadEnvironmentConfig @ 0x610A24..0x610A77].";
		}
	}
	return out;
}

bool trn_value_needs_quotes(const std::string &value) {
	// What the tokenizer ends a token or a line at outside quotes [orig: Terrain_TokenizeConfigLine @0x53CC16..0x53CC4C].
	return value.find_first_of(" ,\t;") != std::string::npos || value.find("//") != std::string::npos;
}

bool trn_value_writable(const std::string &value, std::string &error) {
	if (value.empty()) {
		error = "A value holds at least one character: the game's reader makes no empty value.";
		return false;
	}
	for (const unsigned char c : value)
		if (c < 0x20 || c == 0x7F || c == '"') {
			error = "A value holds no '\"' and no control character: the game's reader cannot read one back.";
			return false;
		}
	return true;
}

std::string trn_values_text(const std::vector<std::string> &values, size_t from) {
	std::string out;
	for (size_t i = from; i < values.size(); ++i) {
		if (i > from) out += ' ';
		out += trn_value_needs_quotes(values[i]) ? "\"" + values[i] + "\"" : values[i];
	}
	return out;
}

bool trn_values_of_text(const std::string &text, std::vector<std::string> &values, std::string &error) {
	values.clear();
	bool quoted = false;
	std::string token;
	bool in_token = false;
	const auto end_token = [&] {
		if (in_token) values.push_back(token);
		token.clear();
		in_token = false;
	};
	for (size_t i = 0; i < text.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c < 0x20 || c == 0x7F) {
			if (c == '\t' && !quoted) {
				end_token();
				continue;
			}
			error = "A value holds no control character: the game's reader cannot read one back.";
			return false;
		}
		if (!quoted && (c == ';' || (c == '/' && i + 1 < text.size() && text[i + 1] == '/'))) {
			error = "A ';' or \"//\" outside quotes starts a comment, where the game's reader cuts the line: quote the "
			        "value that holds it.";
			return false;
		}
		if (c == '"') {
			// A quote toggles quoting and ends a token either way [orig: Terrain_TokenizeConfigLine @0x53CC4E..0x53CC70].
			quoted = !quoted;
			end_token();
		} else if (!quoted && (c == ' ' || c == ',')) {
			end_token();
		} else {
			token += static_cast<char>(c);
			in_token = true;
		}
	}
	if (quoted) {
		error = "A quote is left open.";
		return false;
	}
	end_token();
	if (values.size() > size_t(io::kConfigMaxTokens - 1)) {
		error = "A line holds " + std::to_string(io::kConfigMaxTokens - 1) +
		        " values after its keyword at most: the game's reader cuts 30 tokens.";
		return false;
	}
	return true;
}

bool write_trn_key_line(std::string &out, const TrnKeyLine &line, std::string &error) {
	if (!trn_parser_key(line.key, true)) {
		error = "'" + line.key + "' is no keyword of the terrain's reader.";
		return false;
	}
	for (const std::string &value : line.values)
		if (!trn_value_writable(value, error)) return false;
	std::string text = line.key;
	if (!line.values.empty()) text += ' ' + trn_values_text(line.values);
	// Read back as the game's tokenizer reads the line: the same keyword and values, or the line is refused.
	io::ConfigTokens back;
	io::tokenize_config_line(text.c_str(), back);
	bool same = text.size() <= io::kConfigMaxLineChars && back.count == int(line.values.size()) + 1 &&
	            strutil::to_lower(back.tokens[0]) == line.key;
	for (size_t i = 0; same && i < line.values.size(); ++i) same = line.values[i] == back.tokens[i + 1];
	if (!same) {
		error = "The game's reader would read this " + line.key +
		        " line otherwise: it cuts a line at 30 tokens and 1000 characters, the 30th token running to the line's "
		        "end.";
		return false;
	}
	out += text;
	out += "\r\n";
	return true;
}

std::string trn_key_value(const TrnConfig &config, const std::string &key) {
	const auto name = [](const std::string &value) {
		return value.empty() ? std::string() : trn_values_text({ value });
	};
	const auto pair = [](int x, int y) { return std::to_string(x) + " " + std::to_string(y); };
	if (key == "polytrn_colormap") return name(config.colormap);
	if (key == "polytrn_detailmap") return name(config.detailmap);
	if (key == "polytrn_detailmap_c1") return name(config.detailmap_c1);
	if (key == "polytrn_detailmap_c2") return name(config.detailmap_c2);
	if (key == "polytrn_detailmap_c3") return name(config.detailmap_c3);
	if (key == "polytrn_detailmap2") return name(config.detailmap2);
	if (key == "polytrn_detailmapdist") return name(config.detailmapdist);
	if (key == "polytrn_detailmapdist2") return name(config.detailmapdist2);
	if (key == "polytrn_detailblendmap") return name(config.detailblendmap);
	if (key == "polytrn_polydata") return name(config.polydata);
	if (key == "polytrn_charmap") return name(config.charmap);
	if (key == "polytrn_foliagemap") return name(config.foliagemap);
	if (key == "polytrn_tilestrip") return name(config.tilestrip);
	if (key == "polytrn_tileinfo") return name(config.tileinfo);
	if (key == "polytrn_detaildensity") return std::to_string(config.detail_density);
	if (key == "polytrn_detaildensity2") return std::to_string(config.detail_density2);
	if (key == "polytrn_sectorcount") return std::to_string(config.sector_count);
	if (key == "polytrn_wrapx") return std::to_string(config.wrap_x);
	if (key == "polytrn_wrapy") return std::to_string(config.wrap_y);
	if (key == "polytrn_origin") return pair(config.origin_x, config.origin_y);
	if (key == "lock_topleft") return pair(config.lock_topleft.x, config.lock_topleft.y);
	if (key == "lock_topright") return pair(config.lock_topright.x, config.lock_topright.y);
	if (key == "lock_bottomleft") return pair(config.lock_bottomleft.x, config.lock_bottomleft.y);
	if (key == "lock_bottomright") return pair(config.lock_bottomright.x, config.lock_bottomright.y);
	return std::string();
}

bool trn_parser_reads_one_value(const std::string &key) {
	// The arms that read tokens[2] alone (walk_trn above): the names, the numbers, a block's model and colours.
	static const char *const kOne[] = { "polytrn_colormap", "polytrn_detailmap", "polytrn_detailmap_c1",
		"polytrn_detailmap_c2", "polytrn_detailmap_c3", "polytrn_detailmap2", "polytrn_detailmapdist",
		"polytrn_detailmapdist2", "polytrn_detailblendmap", "polytrn_charmap", "polytrn_polydata", "polytrn_tilestrip",
		"polytrn_foliagemap", "polytrn_tileinfo", "polytrn_wrapx", "polytrn_wrapy", "polytrn_detaildensity",
		"polytrn_detaildensity2", "polytrn_sectorcount", "graphic", "color_lower", "color_upper" };
	return std::any_of(std::begin(kOne), std::end(kOne), [&key](const char *k) { return key == k; });
}

bool save_trn(std::ostream &f, const TrnConfig &cfg, std::string &error) {
	const char *nl = "\r\n";

	f << "terrain_name     \"" << cfg.name << "\"" << nl;
	if (!cfg.creator.empty()) f << "terrain_creator  \"" << cfg.creator << "\"" << nl;
	f << nl;
	f << "water_height     " << cfg.water_height << nl;
	if (cfg.water_rgb_set)
		f << "water_rgb        " << cfg.water_rgb[0] << "," << cfg.water_rgb[1] << "," << cfg.water_rgb[2] << nl;
	if (cfg.water_murk_set) {
		// The fewest digits from six that the parser's atof reads back as the same float.
		std::string murk;
		for (int digits = 6; digits <= 9; ++digits) {
			std::ostringstream out;
			out << std::setprecision(digits) << std::defaultfloat << cfg.water_murk;
			murk = out.str();
			if (static_cast<float>(io::retail_atof(murk.c_str())) == cfg.water_murk) break;
		}
		f << "water_murk       " << murk << nl;
	}
	f << nl;

	if (!cfg.colormap.empty()) {
		f << "polytrn_colormap         " << cfg.colormap << nl;
	}
	if (!cfg.detailmap.empty()) {
		f << "polytrn_detailmap        " << cfg.detailmap << nl;
	}
	if (!cfg.detailmap_c1.empty()) {
		f << "polytrn_detailmap_c1     " << cfg.detailmap_c1 << nl;
	}
	if (!cfg.detailmap_c2.empty()) {
		f << "polytrn_detailmap_c2     " << cfg.detailmap_c2 << nl;
	}
	if (!cfg.detailmap_c3.empty()) {
		f << "polytrn_detailmap_c3     " << cfg.detailmap_c3 << nl;
	}
	if (!cfg.detailmap2.empty()) {
		f << "polytrn_detailmap2       " << cfg.detailmap2 << nl;
	}
	if (!cfg.detailmapdist.empty()) {
		f << "polytrn_detailmapdist    " << cfg.detailmapdist << nl;
	}
	if (!cfg.detailmapdist2.empty()) {
		f << "polytrn_detailmapdist2   " << cfg.detailmapdist2 << nl;
	}
	if (!cfg.polydata.empty()) {
		f << "polytrn_polydata         " << cfg.polydata << nl;
	}
	if (!cfg.tilestrip.empty()) {
		f << "polytrn_tilestrip        " << cfg.tilestrip << nl;
	}
	if (!cfg.charmap.empty()) {
		f << "polytrn_charmap          " << cfg.charmap << nl;
	}
	if (!cfg.foliagemap.empty()) {
		f << "polytrn_foliagemap       " << cfg.foliagemap << nl;
	}
	if (!cfg.detailblendmap.empty()) {
		f << "polytrn_detailblendmap   " << cfg.detailblendmap << nl;
	}
	if (!cfg.tileinfo.empty()) {
		f << "polytrn_tileinfo         " << cfg.tileinfo << nl;
	}
	f << nl;

	f << "polytrn_detaildensity\t\t" << cfg.detail_density << nl;
	f << "polytrn_detaildensity2\t\t" << cfg.detail_density2 << nl;
	f << "polytrn_sectorcount\t\t" << cfg.sector_count << nl;
	f << "polytrn_wrapx\t\t\t" << cfg.wrap_x << nl;
	f << "polytrn_wrapy\t\t\t" << cfg.wrap_y << nl;
	const bool has_quadrant_locks =
		cfg.lock_topleft.x != 0 || cfg.lock_topleft.y != 0 ||
		cfg.lock_topright.x != 0 || cfg.lock_topright.y != 0 ||
		cfg.lock_bottomleft.x != 0 || cfg.lock_bottomleft.y != 0 ||
		cfg.lock_bottomright.x != 0 || cfg.lock_bottomright.y != 0;
	if (has_quadrant_locks) {
		f << "lock_topleft\t\t\t" << cfg.lock_topleft.x << "\t" << cfg.lock_topleft.y << nl;
		f << "lock_topright\t\t\t" << cfg.lock_topright.x << "\t" << cfg.lock_topright.y << nl;
		f << "lock_bottomleft\t\t" << cfg.lock_bottomleft.x << "\t" << cfg.lock_bottomleft.y << nl;
		f << "lock_bottomright\t" << cfg.lock_bottomright.x << "\t" << cfg.lock_bottomright.y << nl;
	}
	f << nl;
	f << "polytrn_origin\t\t\t" << cfg.origin_x << "\t" << cfg.origin_y << nl;
	f << nl;

	const int rows = cfg.sector_rows > 0 ? cfg.sector_rows : 1;
	const int cols = cfg.sector_count > 0 ? cfg.sector_count : 1;
	for (int r = 0; r < rows; ++r) {
		f << "polytrn_sectors\t\t\t";
		for (int c = 0; c < cols; ++c) {
			f << cfg.sector_grid[r][c];
			if (c < cols - 1) {
				f << "\t";
			}
		}
		f << nl;
	}

	for (const auto &def : cfg.foliage_defs) {
		const FoliageDef normalized = foliage_normalize_def(def);
		f << nl << "foliage" << nl;
		if (!normalized.graphic.empty()) {
			f << "  graphic         " << normalized.graphic << nl;
		}
		f << "  color_lower     " << normalized.color_lower << nl;
		f << "  color_upper     " << normalized.color_upper << nl;
		if (foliage_def_match_count(normalized) > 0) {
			// Every authored code on the one `match` line, in retail's arg order.
			f << "  match           ";
			bool wrote = false;
			for (int code : normalized.match) {
				if (code < 0) {
					continue;
				}
				if (wrote) {
					f << " ";
				}
				f << code;
				wrote = true;
			}
			f << nl;
		}
		if (normalized.attrib_flags != 0) {
			f << "  attrib          ";
			bool wrote = false;
			if ((normalized.attrib_flags & FOLIAGE_ATTRIB_SHADOW) != 0) {
				f << "shadow";
				wrote = true;
			}
			if ((normalized.attrib_flags & FOLIAGE_ATTRIB_FORCE_ON) != 0) {
				if (wrote) {
					f << " ";
				}
				f << "forceon";
			}
			f << nl;
		}
		f << "end" << nl;
	}

	if (!f.good()) {
		error = "Write error for TRN";
		return false;
	}
	return true;
}

std::string trn_mission_tilestrip(const TrnConfig &trn,
		const std::string &mission_tile_set) {
	// An empty mission name leaves the .trn value [orig: the NUL test
	// @ 0x6109C8 skipping to @ 0x610A24].
	if (mission_tile_set.empty() || mission_tile_set.front() == '\0') {
		return trn.tilestrip;
	}
	// Everything from the FIRST '.' becomes ".TGA"; without a dot ".TGA" is
	// appended [orig: Path_ReplaceOrAppendExtension @ 0x53C7C0..0x53C7CD].
	std::string atlas = mission_tile_set.substr(0, mission_tile_set.find('\0'));
	const std::size_t dot = atlas.find('.');
	if (dot != std::string::npos) atlas.resize(dot);
	return atlas + ".TGA";
}

} // namespace opennova
