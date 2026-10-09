// The .trn readers' own report of the lines they read otherwise than the record holds (trn_source_issue.h): the walk
// the terrain's parser takes over a .trn's lines [orig: Terrain_ParseConfigCallback @ 0x60F330], with the
// environment's reader beside it for the environment keywords it reads in the terrain's pass [orig:
// Environment_LoadTimeOfDayConfig @ 0x57DB30, the .trn pass @ 0x57DBCC..0x57DBDE].
#include <formats/trn/trn_source_issue.h>

#include <algorithm>
#include <map>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/env/env.h>
#include <formats/foliage/foliage.h>
#include <formats/trn/trn.h>
#include <formats/trn/trn_io.h>

namespace opennova {

namespace {

// The keys the terrain's reader reads out of a block (trn_parser_key) [orig: Terrain_ParseConfigCallback
// @ 0x60F330], with the two the record keeps for whoever edits the file (terrain_name and terrain_creator: read by
// no arm, held by TrnConfig and written by save_trn; a horizon line, which no reader has, is skipped and never
// written, D-TERRAIN-20).
bool terrain_reader_key(const std::string &key) {
	return trn_parser_key(key) || key == "terrain_name" || key == "terrain_creator";
}

// The environment's keywords the terrain's record holds (trn.h): its water.
bool held_environment_key(const std::string &key) {
	return key == "water_height" || key == "water_rgb" || key == "water_murk";
}

} // namespace

std::vector<TrnSourceIssue> trn_source_issues(const std::string &text) {
	std::vector<TrnSourceIssue> issues;
	bool in_foliage = false, swallowed = false;
	int closed = 0, sector_count = 0, row_lines = 0;
	size_t row_line_17 = 0;
	std::map<std::string, size_t> first_line;
	const auto issue = [&](TrnSourceRule rule, bool blocks, size_t line, const std::string &field) -> TrnSourceIssue & {
		TrnSourceIssue out;
		out.rule = rule;
		out.blocks = blocks;
		out.line = line;
		out.field = field;
		issues.push_back(std::move(out));
		return issues.back();
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
				issue(TrnSourceRule::CutBlockEnd, false, line, "end");
				return;
			}
			issue(TrnSourceRule::CutLastLine, false, line, key).token = std::string(1, text.back());
		}
		// The environment's reader reads every line of the terrain, inside a foliage block too (its walk has
		// no block state): what it takes is the mission's environment until the .env's line [orig:
		// Environment_LoadTimeOfDayConfig @ 0x57DB30, the .trn pass @ 0x57DBCC..0x57DBDE].
		if (env::is_env_key(key) && key != "enviro_name" && key != "vertex_rgb") {
			if (!held_environment_key(key)) {
				issue(TrnSourceRule::EnvironmentKey, true, line, key).token = tokens.tokens[0];
				return;
			}
			if (key == "water_rgb") {
				if (tokens.count < 4) issue(TrnSourceRule::ShortColour, false, line, key).read = tokens.count - 1;
				for (int c = 1; c <= 3; ++c) {
					const int read = io::retail_atol(tokens.token(c));
					if (read < 0 || read > 255) {
						TrnSourceIssue &out = issue(TrnSourceRule::ColourByte, false, line, key);
						out.read = read;
						out.kept = std::clamp(read, 0, 255);
						break;
					}
				}
			}
			if (key == "water_murk" && static_cast<float>(io::retail_atof(value)) > 0.99f)
				issue(TrnSourceRule::MurkClamp, false, line, key);
			if (in_foliage) return;
		}
		if (in_foliage) {
			if (key == "end") {
				in_foliage = false;
				++closed;
			} else if (key == "match") {
				if (tokens.count - 1 > FOLIAGE_MATCH_CODES) issue(TrnSourceRule::MatchPastFour, false, line, key);
				for (int i = 1; i < tokens.count && i <= FOLIAGE_MATCH_CODES; ++i) {
					const int read = io::retail_atol(tokens.tokens[i]);
					if (read < 0 || read > 255) {
						TrnSourceIssue &out = issue(TrnSourceRule::MatchByte, false, line, key);
						out.read = read;
						out.kept = uint8_t(read);
						break;
					}
				}
			} else if (key == "attrib") {
				for (int i = 1; i < tokens.count && i < 8; ++i)
					if (!strutil::iequals(tokens.tokens[i], "forceon") && !strutil::iequals(tokens.tokens[i], "shadow")) {
						issue(TrnSourceRule::AttribWord, false, line, key).token = tokens.tokens[i];
						break;
					}
			} else if (key == "color_lower" || key == "color_upper") {
				const int read = io::retail_atol(value);
				if (read < 0 || read > 2) {
					TrnSourceIssue &out = issue(TrnSourceRule::ColourMode, false, line, key);
					out.read = read;
					out.kept = foliage_normalize_color_mode(read);
				}
			} else if (key != "graphic") {
				issue(TrnSourceRule::BlockKeySkipped, false, line, key).token = tokens.tokens[0];
			}
			return;
		}
		if (key == "foliage") {
			if (closed >= FOLIAGE_MAX_DEFS) {
				swallowed = true;
				issue(TrnSourceRule::FifthBlock, false, line, key);
				return;
			}
			in_foliage = true;
			return;
		}
		if (key == "polytrn_sectors") {
			++row_lines;
			if (row_lines == kTerrainGridSide + 1) row_line_17 = line;
			const int columns = tokens.count - 1;
			const int width = std::min(sector_count, kTerrainGridSide);
			if (row_lines > kTerrainGridSide) return;
			const auto row = [&](TrnSourceRule rule) {
				TrnSourceIssue &out = issue(rule, false, line, "polytrn_sectors");
				out.read = columns;
				out.kept = width;
			};
			if (width <= 0 && columns > 0) row(TrnSourceRule::RowBeforeWidth);
			else if (columns < width) row(TrnSourceRule::RowShort);
			else if (columns > width && width > 0) row(TrnSourceRule::RowWide);
			return;
		}
		if (key == "polytrn_sectorcount") sector_count = io::retail_atol(value);
		if (key == "polytrn_scale") {
			// Read for the multiplayer check alone [orig: Terrain_LoadEnvironmentConfig @ 0x61096D, its default;
			// @ 0x60C5FD feeds the CRC]: the record does not hold it.
			issue(TrnSourceRule::ScaleKey, true, line, key);
			return;
		}
		if (key == "polytrn_depthmap") {
			// An arm reads it [orig: Terrain_ParseConfigCallback @ 0x60F81D]; the record (TrnConfig) does not hold it.
			issue(TrnSourceRule::DepthmapKey, true, line, key);
			return;
		}
		if (!terrain_reader_key(key) && !held_environment_key(key)) {
			issue(TrnSourceRule::Skipped, false, line, key).token = tokens.tokens[0];
			return;
		}
		// A key read again: the last line wins.
		const auto first = first_line.emplace(key, line);
		if (!first.second) {
			issue(TrnSourceRule::ReadAgain, false, first.first->second, key).again = line;
			first.first->second = line;
		}
	});
	if (row_lines > kTerrainGridSide) issue(TrnSourceRule::TooManyRows, false, row_line_17, "polytrn_sectors").read = row_lines;
	std::stable_sort(issues.begin(), issues.end(),
	                 [](const TrnSourceIssue &a, const TrnSourceIssue &b) { return a.line < b.line; });
	return issues;
}

} // namespace opennova
