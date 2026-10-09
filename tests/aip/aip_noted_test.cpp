// The AI profile's writer (formats/aip/aip.h write_profile) over the file's modeled layout (textlayout; the
// maintainer's ruling of 2026-10-04, "model it, generate it", ADR 0003 holding): a profile minted with no
// source text is the writer's own form, byte for byte, and reads back as itself; a file read with its layout
// and written again is the file (its comments, its spacing, a number's own spelling, the keys the reader reads
// nothing of, a key before `type`, a value the writer leaves out, its last line with no ending); one value
// changed changes its one line, keeping its blanks and comment; a key set anew goes after the key before it in
// the writer's order; a key cleared to the zeroed record's value goes; every unit's shortest word reads back to
// its value. With a JO install configured (--retail): every shipped profile is read with its layout and written
// again byte for byte, its writer's own form reads back as the same profile, and one whole number changed
// changes one line.
#include <formats/aip/aip.h>
#include <base/vfs/vfs.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

aip::Profile read(const std::string &text, textlayout::Notes &notes) {
	const std::vector<uint8_t> bytes = bytes_of(text);
	return aip::parse_profile(bytes.data(), bytes.size(), notes);
}

std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t end = text.find("\r\n", at);
		out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
		at = end == std::string::npos ? text.size() : end + 2;
	}
	return out;
}

// How many lines differ between two texts of the same line count (-1 for a different count).
int changed_lines(const std::string &a, const std::string &b) {
	const std::vector<std::string> x = lines_of(a), y = lines_of(b);
	if (x.size() != y.size()) return -1;
	int changed = 0;
	for (size_t i = 0; i < x.size(); ++i) changed += x[i] != y[i];
	return changed;
}

int minted() {
	aip::Profile profile;
	profile.type = aip::kTypeGround;
	profile.default_state = 17;
	profile.rank = 2;
	profile.view_fov_bam = int32_t(int64_t(90.0 * 11930464.0));
	profile.view_dist = 500 << 16;
	profile.evade_flags = 0x1;
	profile.combat_flags = 0x1 | 0x8;
	profile.react_ticks = 125;
	profile.primary.weapon = "AI_LAW";
	profile.primary.ammo = -1;
	profile.primary.rate_ticks = 15; // 0.25 s chopped: the writer's shortest decimal is 0.24
	profile.primary.flags = aip::kWeaponTurret | aip::kWeaponFast;
	profile.ground_patrol_speed = int32_t(40.0 * 1000.0 * 4.444444444444444e-06 * 65536.0);
	profile.patrol_speed = 40;
	profile.has_ground_patrol_speed = true;
	profile.drive_skill = 2;
	profile.turn_rate_bam_tick = int32_t(uint32_t(11930464u) * 60u) / 62;
	profile.accel_ticks = 620;
	std::string text, error;
	TEST_EXPECT(aip::write_profile(profile, nullptr, text, error));
	const std::string expected =
			"type\tGROUND\r\n"
			"default_state\tGROUND_FOLLOWWP\r\n"
			"rank\t2\r\n"
			"view_fov\t90\r\n"
			"view_dist\t500\r\n"
			"evade_flags\tFOLLOW_WP\r\n"
			"combat_flags\tFOLLOW_WP\tNO_CAP\r\n"
			"react_time\t2\r\n"
			"primary_weap\tAI_LAW\r\n"
			"primary_ammo\t-1\r\n"
			"primary_rate\t0.24\r\n"
			"primary_flags\tWEAPON_TURRET\tWEAPON_FAST\r\n"
			"patrol_speed\t40\r\n"
			"drive_skill\t2\r\n"
			"turn_rate\t60\r\n"
			"accel_time\t10\r\n";
	if (text != expected) std::fprintf(stderr, "minted:\n%s\n", text.c_str());
	TEST_EXPECT(text == expected);
	const aip::Profile again = aip::parse_profile(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	TEST_EXPECT(aip::same_profile(again, profile));
	// A HELO profile writes its skill as the shipped helicopters do.
	aip::Profile helo;
	helo.type = aip::kTypeHelo;
	helo.drive_skill = 3;
	helo.helo_patrol_altitude = 25 << 16;
	helo.helo_patrol_climb = int32_t(5 * 0.016 * 65536.0);
	TEST_EXPECT(aip::write_profile(helo, nullptr, text, error));
	TEST_EXPECT(text == "type\tHELO\r\npatrol_altitude\t25\r\npatrol_climb\t5\r\nflight_skill\t3\r\n");
	// A value no word reads back to is refused, saying which.
	aip::Profile odd;
	odd.type = aip::kTypeGround;
	odd.view_dist = 12345;
	TEST_EXPECT(!aip::write_profile(odd, nullptr, text, error) && error.find("view_dist") != std::string::npos);
	std::printf("minted: a profile with no source text in the writer's own form, read back as itself\n");
	return 0;
}

int noted() {
	const std::string file =
			"// an authored profile\r\n"
			"rank 9\r\n" // before `type`: read for nothing
			"description     \"Grnd - test\"\r\n"
			"type\t\t\tGROUND\r\n"
			"default_state\tGROUND_FOLLOWWP\t\t\r\n"
			"\r\n"
			"rank\t\t\t\t0\t// a zero the writer leaves out\r\n"
			"view_fov\t\t360.0\t\r\n"
			"primary_rate\t\t0.25\t\r\n"
			"primary_flags\t\tWEAPON_FAST WEAPON_TURRET\r\n"
			"min_speed\t\t0\t\r\n" // a HELO key: read for nothing by a GROUND profile
			"accel_time\t\t\t10\r\n"
			"\0";
	const std::string text(file.c_str(), file.size() + 1);
	textlayout::Notes notes;
	aip::Profile profile = read(text, notes);
	TEST_EXPECT(profile.type == aip::kTypeGround && profile.rank == 0 && profile.primary.rate_ticks == 15);
	std::string out, error;
	bool rewritten = true;
	TEST_EXPECT(aip::write_profile(profile, &notes, out, error, &rewritten) && !rewritten);
	if (out != text) std::fprintf(stderr, "noted:\n%s\n", out.c_str());
	TEST_EXPECT(out == text);

	// One value changed: its one line, its blanks kept.
	aip::Profile changed = profile;
	changed.accel_ticks = 62 * 12;
	TEST_EXPECT(aip::write_profile(changed, &notes, out, error));
	TEST_EXPECT(changed_lines(text, out) == 1 && out.find("accel_time\t\t\t12\r\n") != std::string::npos);
	// A value the writer left out set: its kept line takes the writer's words in its spacing.
	changed = profile;
	changed.rank = 3;
	TEST_EXPECT(aip::write_profile(changed, &notes, out, error));
	TEST_EXPECT(changed_lines(text, out) == 1 && out.find("rank\t\t\t\t3\t// a zero the writer leaves out\r\n") != std::string::npos);
	// A flag added: the file's words where they stood, the new one after them.
	changed = profile;
	changed.primary.flags |= aip::kWeaponSlow;
	TEST_EXPECT(aip::write_profile(changed, &notes, out, error));
	TEST_EXPECT(out.find("primary_flags\t\tWEAPON_FAST WEAPON_TURRET WEAPON_SLOW\r\n") != std::string::npos);
	// A key set anew goes after the key before it in the writer's order.
	changed = profile;
	changed.radar_dist = 800 << 16;
	TEST_EXPECT(aip::write_profile(changed, &notes, out, error));
	TEST_EXPECT(out.find("view_fov\t\t360.0\t\r\nradar_dist\t800\r\n") != std::string::npos);
	// A key cleared to the zeroed record's value goes, its line with it.
	changed = profile;
	changed.accel_ticks = 0;
	TEST_EXPECT(aip::write_profile(changed, &notes, out, error));
	TEST_EXPECT(out.find("accel_time") == std::string::npos && out.find("min_speed\t\t0\t\r\n\0") != std::string::npos);
	// Each written text reads back as the profile it was written from.
	const aip::Profile again = aip::parse_profile(reinterpret_cast<const uint8_t *>(out.data()), out.size());
	TEST_EXPECT(aip::same_profile(again, changed));
	std::printf("noted: an authored profile written again as it was; a value, a flag, a new key and a cleared key "
	            "each its one line\n");
	return 0;
}

// Every unit's shortest word reads back to its value, for values a person would write.
int units() {
	int checked = 0;
	for (const aip::KeyRow &row : aip::key_rows()) {
		if (row.unit == aip::Unit::Weapon || row.unit == aip::Unit::Type || row.unit == aip::Unit::Flags ||
		    row.unit == aip::Unit::HuntFlags || row.unit == aip::Unit::State || row.unit == aip::Unit::Alert ||
		    row.unit == aip::Unit::Subtype)
			continue;
		for (const char *word : {"1", "2.5", "0.1", "45", "360", "1000", "0.25", "-30", "62.4"}) {
			const int32_t type = (row.types & aip::kGroundKeys) ? aip::kTypeGround : aip::kTypeHelo;
			const int32_t value = aip::read_value(row, type, {word}, 0);
			if (value == 0) continue;
			const std::vector<std::string> words = aip::value_words(row, type, value);
			if (words.empty()) std::fprintf(stderr, "%s: no word reads back to %d (from %s)\n", row.key, value, word);
			TEST_EXPECT(!words.empty());
			TEST_EXPECT(aip::read_value(row, type, words, 0) == value);
			++checked;
		}
	}
	std::printf("units: %d values' shortest words read back to them\n", checked);
	return 0;
}

int retail_profiles() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every shipped AI profile written again byte for byte)");
		return 0;
	}
	Vfs vfs;
	TEST_EXPECT(vfs.mount_game(install, "", VfsMountMode::Packed));
	size_t files = 0, written_form = 0, edited = 0;
	for (const auto &location : vfs.list_files()) {
		std::string name = location.logical_name;
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".aip") continue;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(vfs.read_file(name, bytes));
		const std::string text(bytes.begin(), bytes.end());
		textlayout::Notes notes;
		const aip::Profile profile = aip::parse_profile(bytes.data(), bytes.size(), notes);
		std::string out, error;
		bool rewritten = true;
		if (!aip::write_profile(profile, &notes, out, error, &rewritten)) {
			std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.c_str());
			return 1;
		}
		if (out != text || rewritten) {
			std::fprintf(stderr, "FAIL %s is not written again as it was (rewritten %d)\n", name.c_str(), int(rewritten));
			return 1;
		}
		++files;
		// The writer's own form reads back as the profile.
		aip::Profile unnoted = profile;
		unnoted.note = 0;
		if (!aip::write_profile(unnoted, nullptr, out, error)) {
			std::fprintf(stderr, "FAIL %s: its writer's own form is refused: %s\n", name.c_str(), error.c_str());
			return 1;
		}
		const aip::Profile again = aip::parse_profile(reinterpret_cast<const uint8_t *>(out.data()), out.size());
		if (!aip::same_profile(again, profile)) {
			std::fprintf(stderr, "FAIL %s: its writer's own form reads back otherwise:\n%s\n", name.c_str(), out.c_str());
			return 1;
		}
		++written_form;
		// One whole number changed: one line.
		if (profile.type == aip::kTypeGround || profile.type == aip::kTypeHelo) {
			aip::Profile changed = profile;
			changed.priority_air = profile.priority_air + 1;
			TEST_EXPECT(aip::write_profile(changed, &notes, out, error));
			const int lines = changed_lines(text, out);
			if (lines != 1 && profile.priority_air != 0) {
				std::fprintf(stderr, "FAIL %s: one value changed %d lines\n", name.c_str(), lines);
				return 1;
			}
			++edited;
		}
	}
	std::printf("retail: %zu shipped profiles written again byte for byte, %zu read back from the writer's own form, "
	            "%zu edited one line\n",
	            files, written_form, edited);
	TEST_EXPECT(files >= 98); // JO:CA ships 98, all in its base archives
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (minted() || noted() || units() || retail_profiles()) return 1;
	return 0;
}
