// The .env reader's own report of the lines it reads otherwise than the record holds (env_source_issue.h): the
// walk TimeOfDay_ParseProperty takes over a .env's lines [orig: TimeOfDay_ParseProperty @ 0x57c590], with the
// terrain's parser beside it for the terrain keys (formats/trn, D-TERRAIN-18).
#include <formats/env/env_source_issue.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/env/env.h>
#include <formats/trn/trn_io.h>

namespace opennova::env {

namespace {

// An HHMM time as its clock reads ("23:00").
std::string clock_words(int hhmm) {
	char text[8];
	std::snprintf(text, sizeof(text), "%02d:%02d", hhmm / 100, hhmm % 100);
	return text;
}

// The keywords the terrain's reader compares, a foliage block's included (formats/trn's reader, trn_parser_key): its
// hook stays on for the .env pass, so it reads an environment's lines too, and the mission's terrain takes them after
// its .trn's (D-TERRAIN-18) [orig: Terrain_LoadEnvironmentConfig @ 0x6109AD pushes Terrain_ParseConfigCallback;
// Environment_LoadTimeOfDayConfig @ 0x57DB44 keeps it as g_EnvParseHook for every pass]. terrain_name and horizon
// are read by no arm of either reader: a line the game skips.
bool terrain_key(const std::string &key) { return trn_parser_key(key, true); }

} // namespace

std::vector<EnvSourceIssue> env_source_issues(const std::string &text) {
	std::vector<EnvSourceIssue> issues;
	// The walk the reader takes (io::for_each_config_line_span: its lines and tokens, the callback's gate),
	// numbering every line it cuts.
	int keyframes = 0;
	bool on_slot = false;
	float envscale = 1.0f;
	// The colour lines a later envscale would scale otherwise than the record does (env.h divergence
	// #8): a keyframe's and the scaled global colours', each with the envscale it was read under.
	struct Scaled {
		size_t line;
		std::string key;
		float envscale;
	};
	std::vector<Scaled> scaled;
	std::map<std::string, size_t> first_line; // a keyword outside the blocks: where it was first read
	const auto issue = [&](bool blocks, size_t line, const std::string &field, const std::string &message) {
		EnvSourceIssue out;
		out.blocks = blocks;
		out.line = line;
		out.field = field;
		out.message = message;
		issues.push_back(std::move(out));
	};
	size_t line = 0;
	// A last line no CR LF ends loses its final byte to the walk [orig: File_ParseASCIIFile
	// @ 0x53D8C7..0x53D8F5] (shipped FULL_03.ENV and FULL_05.ENV end on such a "tod_end").
	const bool ends_cut = text.size() < 2 || text.compare(text.size() - 2, 2, "\r\n") != 0;
	io::for_each_config_line_span(text.data(), text.size(), [&](io::ConfigTokens &tokens, const io::ConfigLineSpan &span) {
		++line;
		if (tokens.count == 0 || tokens.tokens[0][0] == '/') return;
		const std::string key = strutil::to_lower(tokens.tokens[0]);
		const char *value = tokens.token(1);
		const bool last_cut = ends_cut && !text.empty() && span.end + 1 >= text.size();
		if (last_cut)
			issue(false, line, key,
			      "The file's last line has no line end, so the game's reader loses its last character ('" +
			              std::string(1, text.back()) + "') and reads the line short: a save writes the line as the "
			              "game read it, ended.");
		if (!is_env_key(key)) {
			if (terrain_key(key)) {
				// A terrain key: a reader keeps it, and a save writes it again, from scratch, after the environment's
				// keywords. One whose line reads back otherwise (a quote, a control character, past the 30 tokens)
				// no line can keep.
				TrnKeyLine kept;
				kept.key = key;
				for (int i = 1; i < tokens.count; ++i) kept.values.emplace_back(tokens.tokens[i]);
				std::string written, why;
				if (!write_trn_key_line(written, kept, why))
					issue(true, line, key,
					      "This terrain key's line cannot be written again as the game's reader reads it: " + why +
					              " Correct the line in the file.");
			} else {
				issue(false, line, key,
				      "The game's environment reader skips '" + std::string(tokens.tokens[0]) +
				              "': a save leaves the line out.");
			}
			return;
		}
		if (key == "tod_begin") {
			if (keyframes >= kMaxTodKeyframes) {
				issue(false, line, "time",
				      std::string("The game reads 16 keyframes: this tod_begin takes no slot and its time is not read, so "
				                  "its colours land on ") +
				              (on_slot ? "the 16th keyframe." : "the colours with no keyframe.") +
				              " A save writes them there.");
				return;
			}
			++keyframes;
			on_slot = true;
		} else if (key == "tod_end") {
			on_slot = false;
			return;
		}
		// A time that reads as another clock time (a short token, an hour past 23, a minute past 59).
		if (key == "tod_begin" || key == "curtime") {
			const int read = parse_tod_time(value);
			if (read != io::retail_atol(value))
				issue(false, line, key == "curtime" ? "curtime" : "time",
				      "The game reads '" + std::string(value) + "' as " + clock_words(read) + ": a save writes " +
				              clock_words(read).substr(0, 2) + clock_words(read).substr(3) + ".");
			return;
		}
		const bool colour_line = key.size() > 4 && key.compare(key.size() - 4, 4, "_rgb") == 0;
		if (colour_line && tokens.count < 4)
			issue(false, line, key,
			      "This colour line has " + std::to_string(tokens.count - 1) +
			              " of its three values: the game reads the missing ones from where earlier, longer lines left "
			              "them, and a save writes the three it read.");
		if (key == "envscale") envscale = static_cast<float>(io::retail_atof(value));
		// The scaled colours the record scales by the last envscale: a keyframe's, and the global ones.
		if (is_envscaled_key(key) && (on_slot || !is_tod_color_key(key))) scaled.push_back({line, key, envscale});
		// A keyword read again outside the blocks: the last line wins.
		if (!on_slot) {
			const auto first = first_line.emplace(key, line);
			if (!first.second) {
				issue(false, first.first->second, key,
				      "'" + key + "' is written again on line " + std::to_string(line) +
				              ": the game reads the last, and a save writes that one alone.");
				first.first->second = line;
			}
		}
	});
	for (const Scaled &each : scaled)
		if (each.envscale != envscale) {
			issue(true, each.line, each.key,
			      "An envscale after this colour line scales it otherwise: the game scales each colour line by the "
			      "envscale read before it, and the engine's environment record holds one for every colour. Move the "
			      "envscale line above the colours.");
			break;
		}
	std::stable_sort(issues.begin(), issues.end(),
	                 [](const EnvSourceIssue &a, const EnvSourceIssue &b) { return a.line < b.line; });
	return issues;
}

} // namespace opennova::env
