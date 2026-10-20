// The .env line forms the shipped JO:CA environments take (fixtures/env/line_forms.env, authored after the census of
// JO:CA's ten FULL_*.ENV: CR LF lines, comment lines at the margin and behind a tab or spaces, keywords commented
// out, a value behind a `;` or `//` comment, a quoted name, a keyword with no value but its comment (star_3di), the
// decimal forms, ten time-of-day blocks indented with spaces, colours with a tab-led comment, and two blocks a later
// tod_begin closes with no tod_end of their own), and the minted fixtures/env/synth_full.env (every keyword): each
// parses, and saved and parsed again reads the same. The text facts that back the tracked divergence decisions in
// docs/env/env-tod-re.md, which the shipped set was swept for once:
//   - envscale (when present) precedes every *_rgb line (divergence #8): a file setting it after a colour fails
//   - tod blocks that author fog_rgb also author skyfog_rgb, or rely on the mirror (divergence #9)
//   - no file exceeds the engine's 16-keyframe cap (divergence #3)
#include <formats/env/env.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace {

bool rgb_equal(const opennova::env::Rgb &a, const opennova::env::Rgb &b) {
	const float eps = 0.5f / 255.0f;
	return std::fabs(a.r - b.r) <= eps && std::fabs(a.g - b.g) <= eps && std::fabs(a.b - b.b) <= eps;
}

bool keyframe_equal(const opennova::env::Keyframe &a, const opennova::env::Keyframe &b) {
	return a.time == b.time && rgb_equal(a.sun, b.sun) && rgb_equal(a.ground, b.ground) &&
	       rgb_equal(a.fog, b.fog) && rgb_equal(a.sky, b.sky) && rgb_equal(a.moon, b.moon) &&
	       rgb_equal(a.skyfog, b.skyfog) && rgb_equal(a.skybase, b.skybase) &&
	       rgb_equal(a.skybright, b.skybright) && rgb_equal(a.skyhighlight, b.skyhighlight) &&
	       rgb_equal(a.cloudbase, b.cloudbase) && rgb_equal(a.cloudhighlight, b.cloudhighlight) &&
	       rgb_equal(a.cloudedge, b.cloudedge);
}

// Streaming scan for divergence-assumption checks on the raw text.
struct TextFacts {
	bool envscale_after_color = false;
	int tod_blocks = 0;
	int fog_without_skyfog = 0;
};

TextFacts scan_text(const std::string &text) {
	TextFacts facts;
	std::istringstream input(text);
	std::string line;
	bool seen_color = false;
	bool in_tod = false;
	bool block_has_fog = false;
	bool block_has_skyfog = false;
	while (std::getline(input, line)) {
		const size_t comment = line.find(';');
		if (comment != std::string::npos) {
			line = line.substr(0, comment);
		}
		std::istringstream tokens(line);
		std::string key;
		tokens >> key;
		for (auto &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (key.empty()) continue;
		if (key == "envscale" && seen_color) facts.envscale_after_color = true;
		if (key.size() > 4 && key.compare(key.size() - 4, 4, "_rgb") == 0) seen_color = true;
		if (key == "tod_begin") {
			++facts.tod_blocks;
			in_tod = true;
			block_has_fog = false;
			block_has_skyfog = false;
		} else if (key == "tod_end") {
			if (in_tod && block_has_fog && !block_has_skyfog) ++facts.fog_without_skyfog;
			in_tod = false;
		} else if (in_tod && key == "fog_rgb") {
			block_has_fog = true;
		} else if (in_tod && key == "skyfog_rgb") {
			block_has_skyfog = true;
		}
	}
	return facts;
}

} // namespace

int main() {
	int failures = 0;
	int checked = 0;
	for (const char *rel : {"env/line_forms.env", "env/synth_full.env"}) {
		const std::vector<uint8_t> bytes = test_io::read_file(test_paths_fixture(rel));
		const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		++checked;
		opennova::env::Config parsed;
		std::string error;
		std::istringstream input(text);
		if (bytes.empty() || !opennova::env::load_env(input, parsed, error)) {
			std::fprintf(stderr, "FAIL: %s: parse: %s\n", rel, error.c_str());
			++failures;
			continue;
		}
		std::ostringstream saved;
		opennova::env::Config reparsed;
		std::istringstream saved_input;
		bool stable = opennova::env::save_env(saved, parsed, error);
		if (stable) {
			saved_input.str(saved.str());
			stable = opennova::env::load_env(saved_input, reparsed, error);
		}
		stable = stable && reparsed.keyframes.size() == parsed.keyframes.size() && reparsed.curtime == parsed.curtime &&
		         reparsed.fog_type == parsed.fog_type && std::fabs(reparsed.fog_level - parsed.fog_level) <= 0.5f &&
		         std::fabs(reparsed.envscale - parsed.envscale) <= 0.001f &&
		         reparsed.water_height_set == parsed.water_height_set && rgb_equal(reparsed.water_rgb, parsed.water_rgb);
		for (size_t i = 0; stable && i < parsed.keyframes.size(); ++i)
			stable = keyframe_equal(parsed.keyframes[i], reparsed.keyframes[i]);
		if (!stable) {
			std::fprintf(stderr, "FAIL: %s: save->reparse not semantically stable %s\n", rel, error.c_str());
			++failures;
			continue;
		}
		const TextFacts facts = scan_text(text);
		std::printf("%s: %zu keyframes, %d tod blocks, %d have fog without skyfog\n", rel, parsed.keyframes.size(),
		            facts.tod_blocks, facts.fog_without_skyfog);
		if (facts.envscale_after_color) {
			std::fprintf(stderr, "FAIL: %s sets envscale after a color; divergence #8 assumption violated\n", rel);
			++failures;
		}
		if (facts.tod_blocks > opennova::env::kMaxTodKeyframes || facts.fog_without_skyfog != 0) {
			std::fprintf(stderr, "FAIL: %s: past the keyframe cap or a block's fog without its skyfog\n", rel);
			++failures;
		}
		if (std::string(rel) == "env/line_forms.env") {
			// The forms read as the shipped files read: the ten blocks (the two left open closed by the next
			// tod_begin), the commented-out colours not read, the values behind their comments.
			const bool forms = parsed.keyframes.size() == 10 && facts.tod_blocks == 10 && parsed.timeofday == "Night" &&
			                   std::fabs(parsed.envscale - 1.0f) <= 0.001f && std::fabs(parsed.iris_center - 0.5f) <= 0.001f &&
			                   rgb_equal(parsed.ceiling_rgb, {56 / 255.0f, 56 / 255.0f, 56 / 255.0f}) &&
			                   rgb_equal(parsed.floor_rgb, {24 / 255.0f, 24 / 255.0f, 24 / 255.0f}) &&
			                   parsed.sky_map1 == "cloud01.pcx" && parsed.fog_type == 2 && parsed.curtime == 1200 &&
			                   parsed.advanced_clouds == 1 && parsed.keyframes[6].time == 1400;
			if (!forms) {
				std::fprintf(stderr, "FAIL: line_forms.env does not read its forms as written\n");
				++failures;
			}
		}
	}
	if (failures) {
		std::fprintf(stderr, "FAIL: %d of the %d .env forms failed\n", failures, checked);
		return 1;
	}
	std::printf("OK: %d .env files parse and round-trip semantically\n", checked);
	return 0;
}
