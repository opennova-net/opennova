// The AI profile's writer (formats/aip/aip.h write_profile), from the profile alone (ADR 0003): a profile minted
// with no source text is the writer's own form, byte for byte, and reads back as itself; an authored file (its
// comments, its spacing, a number's own spelling, the keys the reader reads nothing of, a key before `type`)
// through the reader, written and read again, is the same profile, and an edit reads back; a mask the reader
// ORs over two lines is written on one; every unit's shortest word reads back to its value. With a JO install
// configured (--retail): every shipped profile through the game's reader, written from its profile and read
// again, has the same fields, and an edit reads back.
#include <formats/aip/aip.h>
#include <base/vfs/vfs.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova;

namespace {

aip::Profile parsed(const std::string &text) {
	return aip::parse_profile(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

// A profile written and read again is itself.
bool reads_back(const aip::Profile &profile) {
	std::string text, error;
	if (!aip::write_profile(profile, text, error)) {
		std::fprintf(stderr, "refused: %s\n", error.c_str());
		return false;
	}
	return aip::same_profile(parsed(text), profile);
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
	TEST_EXPECT(aip::write_profile(profile, text, error));
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
	TEST_EXPECT(aip::write_profile(helo, text, error));
	TEST_EXPECT(text == "type\tHELO\r\npatrol_altitude\t25\r\npatrol_climb\t5\r\nflight_skill\t3\r\n");
	// A GROUND speed set through its key's store: the parsed value, the raw whole number, the read.
	aip::Profile ground;
	ground.type = aip::kTypeGround;
	const aip::KeyRow *patrol = aip::key_row("patrol_speed");
	TEST_EXPECT(patrol && patrol->set);
	patrol->set(ground, aip::read_value(*patrol, ground.type, {"40.5"}, 0));
	TEST_EXPECT(ground.patrol_speed == 40 && ground.has_ground_patrol_speed && aip::key_value(*patrol, ground) != 0);
	TEST_EXPECT(aip::write_profile(ground, text, error) && text == "type\tGROUND\r\npatrol_speed\t40.5\r\n");
	// A value no word reads back to is refused, saying which.
	aip::Profile odd;
	odd.type = aip::kTypeGround;
	odd.view_dist = 12345;
	TEST_EXPECT(!aip::write_profile(odd, text, error) && error.find("view_dist") != std::string::npos);
	std::printf("minted: a profile with no source text in the writer's own form, read back as itself\n");
	return 0;
}

// An authored file through the reader: written and read again, the same profile; an edit reads back.
int authored() {
	const std::string file =
			"// an authored profile\r\n"
			"rank 9\r\n" // before `type`: read for nothing
			"description     \"Grnd - test\"\r\n"
			"type\t\t\tGROUND\r\n"
			"default_state\tGROUND_FOLLOWWP\t\t\r\n"
			"\r\n"
			"rank\t\t\t\t0\t// a zero\r\n"
			"view_fov\t\t360.0\t\r\n"
			"primary_rate\t\t0.25\t\r\n"
			"primary_flags\t\tWEAPON_FAST WEAPON_TURRET\r\n"
			"min_speed\t\t0\t\r\n" // a HELO key: read for nothing by a GROUND profile
			"accel_time\t\t\t10\r\n"
			"\0";
	const std::string text(file.c_str(), file.size() + 1);
	aip::Profile profile = parsed(text);
	TEST_EXPECT(profile.type == aip::kTypeGround && profile.rank == 0 && profile.primary.rate_ticks == 15);
	// The lines the reader reads nothing of, and why.
	{
		std::vector<aip::UnreadLine> unread;
		aip::parse_profile(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &unread);
		TEST_EXPECT(unread.size() == 3);
		TEST_EXPECT(unread[0].why == aip::UnreadLine::Why::NoType && unread[0].key == "rank");
		TEST_EXPECT(unread[1].why == aip::UnreadLine::Why::NoType && unread[1].key == "description");
		TEST_EXPECT(unread[2].why == aip::UnreadLine::Why::OtherType && unread[2].key == "min_speed" &&
		            text.compare(unread[2].offset, 9, "min_speed") == 0);
	}
	std::string out, error;
	TEST_EXPECT(aip::write_profile(profile, out, error));
	TEST_EXPECT(out == "type\tGROUND\r\ndefault_state\tGROUND_FOLLOWWP\r\nview_fov\t360\r\nprimary_rate\t0.24\r\n"
	                   "primary_flags\tWEAPON_TURRET\tWEAPON_FAST\r\naccel_time\t10\r\n");
	TEST_EXPECT(aip::same_profile(parsed(out), profile));
	aip::Profile changed = profile;
	changed.accel_ticks = 62 * 12;
	TEST_EXPECT(reads_back(changed));
	changed = profile;
	changed.rank = 3;
	TEST_EXPECT(reads_back(changed));
	changed = profile;
	changed.primary.flags |= aip::kWeaponSlow;
	TEST_EXPECT(reads_back(changed));
	changed = profile;
	changed.radar_dist = 800 << 16;
	TEST_EXPECT(reads_back(changed));
	changed = profile;
	changed.accel_ticks = 0;
	TEST_EXPECT(reads_back(changed));
	std::printf("authored: an authored profile written from its profile, read back as it; a value, a flag, a new "
	            "key and a cleared key each read back\n");
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

// The review's cases (#987): a flag mask the reader ORs over two lines written on one, the mask the same; the
// values whose products wrap in 32 bits, every one the reader makes having its word (written and read back, an
// unrelated edit too); hunt_limit through _ftol2_sse as react_time [orig: @ 0x45F657..0x45F677].
int review_cases() {
	{
		const aip::Profile mask = parsed("type GROUND\r\nprimary_flags WEAPON_TURRET\r\nprimary_flags WEAPON_FAST\r\n");
		std::string out, error;
		TEST_EXPECT(aip::write_profile(mask, out, error) &&
		            out == "type\tGROUND\r\nprimary_flags\tWEAPON_TURRET\tWEAPON_FAST\r\n" &&
		            aip::same_profile(parsed(out), mask));
		TEST_EXPECT(reads_back(parsed("type HELO\r\ncombat_flags FOLLOW_WP\r\n// note\r\ncombat_flags NO_CAP\r\n")));
	}
	for (const char *text : {"type GROUND\r\naccel_time 100000000\r\n", "type GROUND\r\nturn_rate 10000\r\n",
	                         "type HELO\r\nturn_rate -9000\r\n", "type GROUND\r\nturn_rate 360\r\naccel_time -3\r\n"}) {
		aip::Profile profile = parsed(text);
		TEST_EXPECT(reads_back(profile));
		profile.rank = 3;
		TEST_EXPECT(reads_back(profile));
	}
	// Every turn rate and accel time the reader makes has a word reading back to it.
	const aip::KeyRow *turn = aip::key_row("turn_rate"), *accel = aip::key_row("accel_time");
	TEST_EXPECT(turn && accel);
	if (!turn || !accel) return 1;
	for (int64_t n = -2000000; n <= 2000000; n += 7919) {
		const std::string word = std::to_string(n);
		const int32_t t = aip::read_value(*turn, aip::kTypeGround, {word}, 0), a = aip::read_value(*accel, aip::kTypeGround, {word}, 0);
		const std::vector<std::string> tw = aip::value_words(*turn, aip::kTypeGround, t), aw = aip::value_words(*accel, aip::kTypeGround, a);
		TEST_EXPECT(!tw.empty() && aip::read_value(*turn, aip::kTypeGround, tw, 0) == t);
		TEST_EXPECT(!aw.empty() && aip::read_value(*accel, aip::kTypeGround, aw, 0) == a);
	}
	const aip::Profile helo = parsed("type HELO\r\nhunt_limit 40000000\r\n");
	TEST_EXPECT(helo.hunt_limit == INT32_MIN);
	std::printf("review cases: a mask over two lines written on one, wrapping values written, hunt_limit through ftol\n");
	return 0;
}

int retail_profiles() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every shipped AI profile written and read back)");
		return 0;
	}
	Vfs vfs;
	TEST_EXPECT(vfs.mount_game(install, "", VfsMountMode::Packed));
	size_t files = 0, edited = 0;
	for (const auto &location : vfs.list_files()) {
		std::string name = location.logical_name;
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".aip") continue;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(vfs.read_file(name, bytes));
		// Through the game's reader, written from its profile and read again: the same fields.
		const aip::Profile profile = aip::parse_profile(bytes.data(), bytes.size());
		if (!reads_back(profile)) {
			std::fprintf(stderr, "FAIL %s: its writer's form reads back otherwise\n", name.c_str());
			return 1;
		}
		++files;
		// One whole number changed reads back.
		if (profile.type == aip::kTypeGround || profile.type == aip::kTypeHelo) {
			aip::Profile changed = profile;
			changed.priority_air = profile.priority_air + 1;
			if (!reads_back(changed)) {
				std::fprintf(stderr, "FAIL %s: one value changed reads back otherwise\n", name.c_str());
				return 1;
			}
			++edited;
		}
	}
	std::printf("retail: %zu shipped profiles written from their profiles and read back, %zu edited\n", files, edited);
	TEST_EXPECT(files >= 98); // JO:CA ships 98, all in its base archives
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (minted() || authored() || units() || review_cases() || retail_profiles()) return 1;
	return 0;
}
