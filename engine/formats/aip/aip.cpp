#include <formats/aip/aip.h>
#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/tick_rate.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace opennova::aip {

namespace {

// [orig: j__atol @ 0x76AB1B, the CRT's atol on a 32-bit long: io::retail_atol].
int32_t whole(const std::string &s) { return io::retail_atol(s.c_str()); }

// The arms' float conversions. A degree is chopped to 64 bits through fistp under control word 0xC00 and its
// low 32 bits stored, as is a rate's and a facing's [orig: AIProfile_ParseProperty's fldcw/fistp sites, e.g.
// view_fov @ 0x45DF85..0x45DFB6]; the others go through _ftol2_sse (io::retail_ftol_sse2: out of range, INT32_MIN).
int32_t deg_to_bam(const std::string &s) {
	return int32_t(uint32_t(int64_t(io::retail_atof(s.c_str()) * 11930464.0))); // [orig: dbl_7C6E18]
}
int32_t secs_to_ticks(const std::string &s) {
	return int32_t(uint32_t(int64_t(io::retail_atof(s.c_str()) * io::kTickHz))); // [orig: dbl_7C3B48]
}
int32_t react_ticks(const std::string &s) {
	return io::retail_ftol_sse2(io::retail_atof(s.c_str()) * io::kTickHz); // [orig: react_time's ftol2_sse store @ 0x45EE24]
}
int32_t rate_of(const std::string &s) {
	return io::retail_ftol_sse2(io::retail_atof(s.c_str()) * 655.36); // [orig: dbl_7C6AC0]
}
int32_t units_fixed(const std::string &s) {
	return io::retail_ftol_sse2(io::retail_atof(s.c_str()) * 65536.0); // [orig: dbl_7C3CC0]
}
// The two unit conversions of the speed and climb keys [orig: the type-1 arms of AIProfile_ParseProperty
// @0x45f684..0x45f9eb]: a speed is km/h -> 16.16 units per tick, atof * 1000.0 (dbl_7C6BD0) *
// 4.444444444444444e-06 (dbl_7C6BC8) * 65536.0 (dbl_7C3CC0); a climb is atof * 0.016 (dbl_7C6A80) * 65536.0.
// Both chop through _ftol2_sse. The GROUND branch converts its speeds the same way @0x45e6df/@0x45e72d
// (+0xC0/+0xC4; the patrol leg's fmul chain and ftol @0x45E6E8..0x45E6FD) into ground_patrol_speed /
// ground_combat_speed; the raw patrol_speed / combat_speed integers stay alongside as the resolver's
// parsed-anything probe.
int32_t speed_fixed(const std::string &s) {
	return io::retail_ftol_sse2(io::retail_atof(s.c_str()) * 1000.0 * 4.444444444444444e-06 * 65536.0);
}
int32_t climb_fixed(const std::string &s) {
	return io::retail_ftol_sse2(io::retail_atof(s.c_str()) * 0.016 * 65536.0);
}
// turn_rate: atol * 0xB60B60 (11930464 BAM per degree) in 32 bits, then the signed /62 (the 0x84210843 magic
// + sar 5 + sign fix) [orig: GROUND @ 0x45E788..0x45E7C4, HELO @0x45f8b0..0x45f8dc]; accel_time: atol * 62
// [orig: GROUND @ 0x45E7DA..0x45E803, HELO @0x45f902..0x45f91b].
int32_t turn_rate_of(const std::string &s) {
	return int32_t(uint32_t(11930464u) * uint32_t(whole(s))) / io::kTicksPerSecondInt;
}
int32_t accel_of(const std::string &s) { return int32_t(62u * uint32_t(whole(s))); }

int32_t clamp_skill(int32_t v) { return v > 4 ? 4 : v < 0 ? 0 : v; } // [orig: aim_skill's 0..4 clamp @ 0x45EE43..0x45EE81]

const std::vector<FlagWord> kWeaponFlags = {
		{"WEAPON_TURRET", kWeaponTurret},
		{"WEAPON_SLOW", kWeaponSlow},
		{"WEAPON_FAST", kWeaponFast},
		{"WEAPON_PITCHLOCKED", kWeaponPitchLocked},
		{"WEAPON_PITCHLOCKED_MINUS45", kWeaponPitchLockedMinus45},
};
const std::vector<FlagWord> kCombatFlags = {
		{"FOLLOW_WP", 0x1}, {"NO_ACTION", 0x2}, {"FLEE", 0x4},    {"NO_CAP", 0x8},
		{"COUNTER", 0x10},  {"ATEAM", 0x20},    {"ATEAM_LOCK", 0x40}, {"RC_FIRE", 0x80},
};
const std::vector<FlagWord> kEvadeFlags(kCombatFlags.begin(), kCombatFlags.begin() + 5);
const std::vector<FlagWord> kHuntFlags = {{"MAINTAIN_SPEED", 0x1}};

// The name table really stores GROUND_FOLLOWWP=17 (dispatch row 16).
// Keep its literal data values; do not reinterpret it as the runtime enum.
// [orig: AIState_LookupByName @0x457530, table @0x815198]
const std::vector<StateName> kStates = {
		{"NULL", 0},           {"HELO_LAND", 6},         {"HELO_FOLLOWWP", 7},   {"HELO_COMBAT", 8},
		{"HELO_HUNT", 9},      {"HELO_EVADE", 10},       {"HELO_FORMATION", 11}, {"HELO_RETURNTOBASE", 12},
		{"HELO_DYING", 13},    {"HELO_PRETTY", 14},      {"HELO_DEAD", 15},      {"GROUND_FOLLOWWP", 17},
		{"GROUND_COMBAT", 18}, {"GROUND_EVADE", 19},     {"GROUND_FORMATION", 20}, {"GROUND_RETURNTOBASE", 21},
		{"GROUND_DYING", 22},  {"GROUND_PRETTY", 23},    {"GROUND_DEAD", 24},
};

int32_t state_by_name(const std::string &name) {
	for (const StateName &state : kStates)
		if (strutil::iequals(name, state.word)) return state.id;
	return 0;
}

uint32_t flags_of(const std::vector<FlagWord> &table, const std::vector<std::string> &words) {
	uint32_t out = 0;
	for (const std::string &word : words)
		for (const FlagWord &flag : table)
			if (strutil::iequals(word, flag.word)) {
				out |= flag.bit;
				break;
			}
	return out;
}

// The rows' accessors.
#define AIP_GET(member) +[](const Profile &p) { return int32_t(p.member); }
#define AIP_SET(member, type) +[](Profile &p, int32_t v) { p.member = type(v); }

// The writer's word for a GROUND speed: the raw integer the resolver probes is its whole part.
std::string first_word(const std::vector<std::string> &words) { return words.empty() ? std::string() : words[0]; }

// The rows, `type` first, then the order the shipped profiles write their keys in [orig: AIProfile_ParseProperty
// @ 0x45DE70; each arm's store cited at the parse below].
const std::vector<KeyRow> &rows() {
	static const std::vector<KeyRow> table = [] {
		const uint8_t both = kHeloKeys | kGroundKeys;
		std::vector<KeyRow> t;
		t.push_back({"type", nullptr, 0xFF, Unit::Type, AIP_GET(type), AIP_SET(type, int32_t)});
		t.push_back({"subtype", nullptr, both, Unit::Subtype, AIP_GET(subtype), AIP_SET(subtype, int32_t)});
		t.push_back({"default_state", nullptr, both, Unit::State, AIP_GET(default_state), AIP_SET(default_state, int32_t)});
		t.push_back({"rank", nullptr, both, Unit::Whole, AIP_GET(rank), AIP_SET(rank, int32_t)});
		t.push_back({"view_fov", nullptr, both, Unit::Degrees, AIP_GET(view_fov_bam), AIP_SET(view_fov_bam, int32_t)});
		t.push_back({"view_dist", nullptr, both, Unit::Metres, AIP_GET(view_dist), AIP_SET(view_dist, int32_t)});
		t.push_back({"radar_fov", nullptr, both, Unit::Degrees, AIP_GET(radar_fov_bam), AIP_SET(radar_fov_bam, int32_t)});
		t.push_back({"radar_dist", nullptr, both, Unit::Metres, AIP_GET(radar_dist), AIP_SET(radar_dist, int32_t)});
		t.push_back({"priority_air", nullptr, both, Unit::Whole, AIP_GET(priority_air), AIP_SET(priority_air, int32_t)});
		t.push_back({"priority_ground", nullptr, both, Unit::Whole, AIP_GET(priority_ground), AIP_SET(priority_ground, int32_t)});
		t.push_back({"priority_organics", nullptr, both, Unit::Whole, AIP_GET(priority_organics), AIP_SET(priority_organics, int32_t)});
		t.push_back({"priority_decorations", nullptr, both, Unit::Whole, AIP_GET(priority_decorations),
		             AIP_SET(priority_decorations, int32_t)});
		KeyRow evade{"evade_flags", nullptr, both, Unit::Flags, AIP_GET(evade_flags), AIP_SET(evade_flags, uint32_t)};
		evade.flags = &evade_flag_words;
		t.push_back(evade);
		KeyRow combat{"combat_flags", nullptr, both, Unit::Flags, AIP_GET(combat_flags), AIP_SET(combat_flags, uint32_t)};
		combat.flags = &combat_flag_words;
		t.push_back(combat);
		t.push_back({"react_time", nullptr, both, Unit::Seconds, AIP_GET(react_ticks), AIP_SET(react_ticks, int32_t)});
		t.push_back({"aim_skill", nullptr, both, Unit::Skill, AIP_GET(aim_skill), AIP_SET(aim_skill, int32_t)});
		t.push_back({"check_six_rate", nullptr, both, Unit::Rate, AIP_GET(check_six_rate), AIP_SET(check_six_rate, int32_t)});
		t.push_back({"target_eval_rate", nullptr, both, Unit::Rate, AIP_GET(target_eval_rate), AIP_SET(target_eval_rate, int32_t)});
		t.push_back({"tether_dist", nullptr, both, Unit::Metres, AIP_GET(tether_dist), AIP_SET(tether_dist, int32_t)});
		const auto weapon = [&](const char *side, WeaponBlock Profile::*block) {
			const std::string s(side);
			static std::deque<std::string> names; // the keys' storage, kept for the process (stable)
			const auto name = [&](const char *suffix) {
				names.push_back(s + suffix);
				return names.back().c_str();
			};
			KeyRow weap{name("_weap"), nullptr, both, Unit::Weapon, nullptr, nullptr};
			weap.name = &WeaponBlock::weapon;
			weap.block = block;
			t.push_back(weap);
			const bool primary = s == "primary";
			t.push_back({name("_ammo"), nullptr, both, Unit::Whole,
			             primary ? AIP_GET(primary.ammo) : AIP_GET(secondary.ammo),
			             primary ? AIP_SET(primary.ammo, int32_t) : AIP_SET(secondary.ammo, int32_t)});
			t.push_back({name("_rate"), nullptr, both, Unit::Seconds,
			             primary ? AIP_GET(primary.rate_ticks) : AIP_GET(secondary.rate_ticks),
			             primary ? AIP_SET(primary.rate_ticks, int32_t) : AIP_SET(secondary.rate_ticks, int32_t)});
			t.push_back({name("_fov"), nullptr, both, Unit::Degrees,
			             primary ? AIP_GET(primary.cone_bam) : AIP_GET(secondary.cone_bam),
			             primary ? AIP_SET(primary.cone_bam, int32_t) : AIP_SET(secondary.cone_bam, int32_t)});
			t.push_back({name("_range"), nullptr, both, Unit::Metres,
			             primary ? AIP_GET(primary.range) : AIP_GET(secondary.range),
			             primary ? AIP_SET(primary.range, int32_t) : AIP_SET(secondary.range, int32_t)});
			t.push_back({name("_facing"), nullptr, both, Unit::Degrees,
			             primary ? AIP_GET(primary.facing_bam) : AIP_GET(secondary.facing_bam),
			             primary ? AIP_SET(primary.facing_bam, int32_t) : AIP_SET(secondary.facing_bam, int32_t)});
			t.push_back({name("_pitch"), nullptr, both, Unit::Degrees,
			             primary ? AIP_GET(primary.pitch_bam) : AIP_GET(secondary.pitch_bam),
			             primary ? AIP_SET(primary.pitch_bam, int32_t) : AIP_SET(secondary.pitch_bam, int32_t)});
			KeyRow flags{name("_flags"), nullptr, both, Unit::Flags,
			             primary ? AIP_GET(primary.flags) : AIP_GET(secondary.flags),
			             primary ? AIP_SET(primary.flags, uint32_t) : AIP_SET(secondary.flags, uint32_t)};
			flags.flags = &weapon_flag_words;
			t.push_back(flags);
		};
		weapon("primary", &Profile::primary);
		weapon("secondary", &Profile::secondary);
		KeyRow hunt{"hunt_flags", nullptr, kHeloKeys, Unit::HuntFlags, AIP_GET(hunt_flags), AIP_SET(hunt_flags, uint32_t)};
		hunt.flags = &hunt_flag_words;
		t.push_back(hunt);
		t.push_back({"hunt_limit", nullptr, kHeloKeys, Unit::Seconds, AIP_GET(hunt_limit), AIP_SET(hunt_limit, int32_t)});
		// The HELO flight set's arms, each its store [orig: AIProfile_ParseProperty]: patrol_climb -> +208
		// [orig: AIProfile_ParseProperty @0x45f687..0x45f6bf], patrol_speed -> +200 [orig: AIProfile_ParseProperty
		// @0x45f6cf..0x45f70d], patrol_altitude -> +204 [orig: AIProfile_ParseProperty @0x45f733..0x45f747],
		// combat_climb -> +220 [orig: AIProfile_ParseProperty @0x45f757..0x45f78f], combat_speed -> +212 [orig:
		// AIProfile_ParseProperty @0x45f79f..0x45f7dd], combat_altitude -> +216 [orig: AIProfile_ParseProperty
		// @0x45f803..0x45f817], use_waypoint_z -> +56 [orig: AIProfile_ParseProperty @0x45f941..0x45f952], min_agl ->
		// +232 [orig: AIProfile_ParseProperty @0x45f975..0x45f991], min_speed -> +236 [orig: AIProfile_ParseProperty
		// @0x45f9b7..0x45f9df]. The two speeds are a HELO profile's and a GROUND profile's members of the same keys
		// (key_value below picks by the type).
		t.push_back({"patrol_speed", nullptr, both, Unit::Speed, nullptr, nullptr});
		t.push_back({"patrol_altitude", nullptr, kHeloKeys, Unit::Metres, AIP_GET(helo_patrol_altitude),
		             AIP_SET(helo_patrol_altitude, int32_t)});
		t.push_back({"patrol_climb", nullptr, kHeloKeys, Unit::Climb, AIP_GET(helo_patrol_climb), AIP_SET(helo_patrol_climb, int32_t)});
		t.push_back({"combat_speed", nullptr, both, Unit::Speed, nullptr, nullptr});
		t.push_back({"combat_altitude", nullptr, kHeloKeys, Unit::Metres, AIP_GET(helo_combat_altitude),
		             AIP_SET(helo_combat_altitude, int32_t)});
		t.push_back({"combat_climb", nullptr, kHeloKeys, Unit::Climb, AIP_GET(helo_combat_climb), AIP_SET(helo_combat_climb, int32_t)});
		t.push_back({"min_agl", nullptr, kHeloKeys, Unit::Fixed, AIP_GET(min_agl), AIP_SET(min_agl, int32_t)});
		t.push_back({"min_speed", nullptr, kHeloKeys, Unit::Speed, AIP_GET(min_speed), AIP_SET(min_speed, int32_t)});
		t.push_back({"min_chase_dist", nullptr, both, Unit::Fixed, AIP_GET(min_chase), AIP_SET(min_chase, int32_t)});
		t.push_back({"max_chase_dist", nullptr, both, Unit::Fixed, AIP_GET(max_chase), AIP_SET(max_chase, int32_t)});
		t.push_back({"drive_skill", "flight_skill", both, Unit::Skill, AIP_GET(drive_skill), AIP_SET(drive_skill, int32_t)});
		t.push_back({"turn_rate", nullptr, both, Unit::TurnRate, AIP_GET(turn_rate_bam_tick), AIP_SET(turn_rate_bam_tick, int32_t)});
		t.push_back({"accel_time", nullptr, both, Unit::AccelTime, AIP_GET(accel_ticks), AIP_SET(accel_ticks, int32_t)});
		t.push_back({"radio_distance", nullptr, both, Unit::Whole, AIP_GET(radio_distance), AIP_SET(radio_distance, int32_t)});
		t.push_back({"radio_delay", nullptr, both, Unit::Whole, AIP_GET(radio_delay), AIP_SET(radio_delay, int32_t)});
		t.push_back({"alert", nullptr, both, Unit::Alert, AIP_GET(alert), AIP_SET(alert, int32_t)});
		t.push_back({"use_waypoint_z", nullptr, kHeloKeys, Unit::Whole, AIP_GET(use_waypoint_z), AIP_SET(use_waypoint_z, int32_t)});
		return t;
	}();
	return table;
}

#undef AIP_GET
#undef AIP_SET

// A key's value on a profile (the two speeds by its type: a HELO's parsed member, a GROUND's).
int32_t key_value(const KeyRow &row, const Profile &p) {
	if (row.get) return row.get(p);
	const bool patrol = std::strcmp(row.key, "patrol_speed") == 0;
	if (p.type == kTypeHelo) return patrol ? p.helo_patrol_speed : p.helo_combat_speed;
	return patrol ? p.ground_patrol_speed : p.ground_combat_speed;
}

// What one line does to the profile: the arm its key takes for the profile's type, as AIProfile_ParseProperty
// @ 0x45DE70 dispatches (`type` for every type; HELO and GROUND their sets; ORGANIC and no type nothing more).
// The row it read (null: the line is read for nothing).
const KeyRow *apply_line(Profile &prof, const std::vector<std::string> &toks) {
	if (toks.empty()) return nullptr;
	const KeyRow *row = key_row(toks[0]);
	const std::vector<std::string> values(toks.begin() + 1, toks.end());
	const std::string value = values.empty() ? std::string() : values[0];
	if (row && row->unit == Unit::Type) {
		// [orig: @ 0x45DE8C..0x45DF07: HELO 1, GROUND 2, ORGANIC 3; another word changes nothing]
		prof.type = read_value(*row, prof.type, values, prof.type);
		return row;
	}
	if (!row || !reads(*row, prof.type)) return nullptr;
	if (row->unit == Unit::Weapon) {
		// [orig: AmmoDef_LookupByName @ 0x45EF6B (primary, +148) / @ 0x45F269 (secondary, +180)]: the port keeps the name.
		(prof.*(row->block)).*(row->name) = value;
		return row;
	}
	if (!row->get) {
		const bool patrol = std::strcmp(row->key, "patrol_speed") == 0;
		const int32_t speed = speed_fixed(value);
		if (prof.type == kTypeHelo) {
			(patrol ? prof.helo_patrol_speed : prof.helo_combat_speed) = speed; // +200 / +212
		} else {
			// [orig: +0xC0 @ 0x45E70B / +0xC4 @ 0x45F677]; the raw integer alongside
			(patrol ? prof.ground_patrol_speed : prof.ground_combat_speed) = speed;
			(patrol ? prof.patrol_speed : prof.combat_speed) = whole(value);
			(patrol ? prof.has_ground_patrol_speed : prof.has_ground_combat_speed) = true;
		}
		return row;
	}
	row->set(prof, read_value(*row, prof.type, values, row->get(prof)));
	return row;
}

// The lines the writer puts down for a profile (textlayout): `type`, then each key of its type whose value is
// other than the zeroed record's, `key<TAB>value`, a mask's words a set. False with the reason for a value no
// word reads back to (`lenient`: such a key is left out instead, as the parse's model of a file does).
bool records_of(const Profile &profile, textlayout::OutRecord &now, std::string &error, bool lenient) {
	now = textlayout::OutRecord();
	now.note = profile.note;
	for (const KeyRow &row : key_rows()) {
		if (row.unit != Unit::Type && !reads(row, profile.type)) continue;
		std::vector<std::string> words;
		if (row.unit == Unit::Weapon) {
			const std::string &name = (profile.*(row.block)).*(row.name);
			if (name.empty()) continue;
			// A name is one token of the walk: no blank, comma, quote, `;` or `//` [orig: Terrain_TokenizeConfigLine
			// @ 0x53CB60].
			if (name.find_first_of(" \t,\";\r\n") != std::string::npos || name.find("//") != std::string::npos) {
				if (lenient) continue;
				error = std::string("The ") + row.key + " \"" + name + "\" is no one word the game's reader reads.";
				return false;
			}
			words.push_back(name);
		} else {
			const int32_t value = key_value(row, profile);
			// A GROUND speed is put down when the profile read one, 0 or not: the boot resolver probes the read.
			const bool ground_speed = !row.get && profile.type == kTypeGround &&
			                          (std::strcmp(row.key, "patrol_speed") == 0 ? profile.has_ground_patrol_speed
			                                                                     : profile.has_ground_combat_speed);
			if (value == 0 && !ground_speed) continue;
			words = value_words(row, profile.type, value);
			if (words.empty()) {
				if (lenient) continue;
				error = std::string("The ") + row.key + " value " + std::to_string(value) +
				        " is one no word of the file reads back to.";
				return false;
			}
		}
		std::string form = row.key;
		if (row.alias && profile.type == kTypeHelo) form = row.alias; // flight_skill, as the shipped HELO profiles write it
		for (const std::string &word : words) form += "\t" + word;
		textlayout::OutLine line{row.key, form};
		if (row.unit == Unit::Flags || row.unit == Unit::HuntFlags) line.set_from = 1;
		now.lines.push_back(std::move(line));
	}
	return true;
}

Profile parse(const uint8_t *text, size_t size, textlayout::Notes *notes, std::vector<UnreadLine> *unread) {
	// AIProfile_LoadOrFind reads the .aip through File_ParseASCIIFile
	// (@ 0x45FE45), so the lines and tokens are the shared walk's
	// (io::for_each_config_line_span: CR LF only, the tokenizer's quotes, commas and
	// comments); keys compare without case, and a value past the line's count
	// reads "" as the reset token does (no value gate). Dispatch is gated on the
	// active `type` exactly like retail: HELO (1) and GROUND (2) accept their key
	// sets, ORGANIC (3) nothing.
	// [orig: AIProfile_ParseProperty @ 0x45de70]
	Profile prof;
	const char *chars = reinterpret_cast<const char *>(text);
	textlayout::Noter noter(chars, size, notes, textlayout::cut_ascii_walk);
	io::ConfigTokens tokens;
	io::for_each_config_line_span(chars, size, tokens, [&](io::ConfigTokens &line, const io::ConfigLineSpan &span) {
		noter.line(span.begin, span.end + 2);
		// The walk's gate: no token, or a first token starting '/' [orig: File_ParseASCIIFile @0x53D915 / @0x53D91E].
		if (line.count == 0 || line.tokens[0][0] == '/') return;
		std::vector<std::string> toks;
		for (int index = 0; index < line.count; ++index) toks.emplace_back(line.tokens[index]);
		// A value past the line's count reads the reset token's "".
		if (toks.size() < 2) toks.emplace_back(line.token(1));
		const int32_t type = prof.type;
		if (const KeyRow *row = apply_line(prof, toks)) {
			noter.entry(noter.root(), row->key);
			return;
		}
		if (!unread) return;
		UnreadLine line_read;
		line_read.offset = span.begin;
		line_read.key = toks[0];
		const KeyRow *row = key_row(toks[0]);
		line_read.why = type == 0 ? UnreadLine::Why::NoType
		                : !row    ? UnreadLine::Why::Unknown
		                : type == kTypeOrganic ? UnreadLine::Why::Organic
		                                       : UnreadLine::Why::OtherType;
		unread->push_back(std::move(line_read));
	});
	noter.finish();
	if (notes) {
		prof.note = notes->root();
		// The writer's lines for the profile as read, which each line is modeled against (a value no word reads
		// back to is left out of them: its line is kept as its tokens).
		textlayout::OutRecord as_read;
		std::string error;
		records_of(prof, as_read, error, true);
		textlayout::model(*notes, as_read, textlayout::cut_ascii_walk);
	}
	return prof;
}

// The shortest decimal of `value` in a unit (`scale`: what a file's number is multiplied by) that the arm's
// conversion reads back to it: by places from none, each the nearest decimal of so many places to the value
// over the scale and its neighbours a step away. A conversion that keeps the low 32 bits of a 64-bit chop
// (`wraps`: a degree, a rate's seconds) reads a number past the 32 bits' range to it too: 360 degrees is the
// BAM -256.
std::string shortest(int32_t value, double scale, int32_t (*convert)(const std::string &), bool wraps = false) {
	std::vector<double> targets = {double(value) / scale};
	if (wraps) {
		targets.push_back((double(value) + 4294967296.0) / scale);
		targets.push_back((double(value) - 4294967296.0) / scale);
	}
	// Of each reading's shortest decimal, the one of the fewest characters (360 degrees, not the -0.0000214577
	// that chops to the same BAM).
	std::string best;
	for (const double target : targets) {
		std::string found;
		for (int places = 0; places <= 12 && found.empty(); ++places) {
			const double step = std::pow(10.0, -places);
			for (int delta = 0; delta <= 2 && found.empty(); ++delta) {
				for (int sign : {1, -1}) {
					if (delta == 0 && sign < 0) continue;
					char buffer[64];
					const double candidate = std::round(target / step) * step + sign * delta * step;
					std::snprintf(buffer, sizeof(buffer), "%.*f", places, candidate);
					std::string word = buffer;
					if (word == "-0") word = "0";
					if (convert(word) == value) {
						found = word;
						break;
					}
				}
			}
		}
		if (!found.empty() && (best.empty() || found.size() < best.size())) best = found;
	}
	return best;
}


} // namespace

const std::vector<FlagWord> &weapon_flag_words() { return kWeaponFlags; }
const std::vector<FlagWord> &evade_flag_words() { return kEvadeFlags; }
const std::vector<FlagWord> &combat_flag_words() { return kCombatFlags; }
const std::vector<FlagWord> &hunt_flag_words() { return kHuntFlags; }
const std::vector<StateName> &state_names() { return kStates; }
const std::vector<KeyRow> &key_rows() { return rows(); }

const KeyRow *key_row(const std::string &key) {
	for (const KeyRow &row : rows())
		if (strutil::iequals(key, row.key) || (row.alias && strutil::iequals(key, row.alias))) return &row;
	return nullptr;
}

int32_t read_value(const KeyRow &row, int32_t type, const std::vector<std::string> &words, int32_t held) {
	const std::string value = words.empty() ? std::string() : words[0];
	switch (row.unit) {
	case Unit::Type:
		if (strutil::iequals(value, "HELO")) return kTypeHelo;
		if (strutil::iequals(value, "GROUND")) return kTypeGround;
		if (strutil::iequals(value, "ORGANIC")) return kTypeOrganic;
		return held;
	case Unit::Whole: return whole(value);
	case Unit::Skill: return clamp_skill(whole(value));
	case Unit::Metres: return int32_t(uint32_t(whole(value)) << 16);
	case Unit::Degrees: return deg_to_bam(value);
	case Unit::Seconds: return std::strcmp(row.key, "react_time") == 0 ? react_ticks(value) : secs_to_ticks(value);
	case Unit::Rate: return rate_of(value);
	case Unit::Fixed: return units_fixed(value);
	case Unit::Speed: return speed_fixed(value);
	case Unit::Climb: return climb_fixed(value);
	case Unit::TurnRate: return turn_rate_of(value);
	case Unit::AccelTime: return accel_of(value);
	case Unit::State: return state_by_name(value);
	case Unit::Alert:
		// [orig: GROUND @ 0x45E91F..0x45E961: GREEN 0, YELLOW 1, else RED 2 and any other word 0]
		if (strutil::iequals(value, "GREEN")) return 0;
		if (strutil::iequals(value, "YELLOW")) return 1;
		return strutil::iequals(value, "RED") ? 2 : 0;
	case Unit::Subtype:
		// [orig: GROUND @ 0x45E887..0x45E8F5: STD 0, BOAT 1, TRAIN 3; HELO @ 0x45FA63..0x45FAA0: STD 0, PLANE 2; another
		// word keeps it]
		if (strutil::iequals(value, "STD")) return 0;
		if (type == kTypeGround && strutil::iequals(value, "BOAT")) return 1;
		if (type == kTypeGround && strutil::iequals(value, "TRAIN")) return 3;
		if (type == kTypeHelo && strutil::iequals(value, "PLANE")) return 2;
		return held;
	case Unit::Flags: return int32_t(uint32_t(held) | flags_of(row.flags(), words));
	case Unit::HuntFlags: return int32_t(flags_of(row.flags(), words)); // [orig: cleared first @ 0x45F5F6]
	case Unit::Weapon: return 0;
	}
	return 0;
}

std::vector<std::string> value_words(const KeyRow &row, int32_t type, int32_t value) {
	std::string word;
	switch (row.unit) {
	case Unit::Type:
		if (value == kTypeHelo) word = "HELO";
		else if (value == kTypeGround) word = "GROUND";
		else if (value == kTypeOrganic) word = "ORGANIC";
		break;
	case Unit::Whole: word = std::to_string(value); break;
	case Unit::Skill:
		if (value >= 0 && value <= 4) word = std::to_string(value);
		break;
	case Unit::Metres:
		if ((value & 0xFFFF) == 0) word = std::to_string(value >> 16);
		break;
	case Unit::Degrees: word = shortest(value, 11930464.0, &deg_to_bam, true); break;
	case Unit::Seconds:
		if (std::strcmp(row.key, "react_time") == 0) word = shortest(value, io::kTickHz, &react_ticks);
		else word = shortest(value, io::kTickHz, &secs_to_ticks, true);
		break;
	case Unit::Rate: word = shortest(value, 655.36, &rate_of); break;
	case Unit::Fixed: word = shortest(value, 65536.0, &units_fixed); break;
	case Unit::Speed: word = shortest(value, 1000.0 * 4.444444444444444e-06 * 65536.0, &speed_fixed); break;
	case Unit::Climb: word = shortest(value, 0.016 * 65536.0, &climb_fixed); break;
	case Unit::TurnRate:
		// The product wraps in 32 bits (360 degrees reads -4): each wrap's whole number (to some 5800 degrees a
		// second either way), the fewest characters.
		for (int wraps = -16; wraps <= 16; ++wraps) {
			const double wrap = double(wraps) * 4294967296.0;
			const int64_t near = int64_t(std::floor((double(value) * 62.0 + wrap) / 11930464.0));
			for (int64_t n = near - 1; n <= near + 2; ++n) {
				const std::string candidate = std::to_string(n);
				if (turn_rate_of(candidate) == value && (word.empty() || candidate.size() < word.size())) word = candidate;
			}
		}
		break;
	case Unit::AccelTime:
		if (value % 62 == 0) word = std::to_string(value / 62);
		break;
	case Unit::State:
		for (const StateName &state : kStates)
			if (state.id == value) {
				word = state.word;
				break;
			}
		break;
	case Unit::Alert: word = value == 1 ? "YELLOW" : value == 2 ? "RED" : value == 0 ? "GREEN" : ""; break;
	case Unit::Subtype:
		if (value == 0) word = "STD";
		else if (value == 1 && type == kTypeGround) word = "BOAT";
		else if (value == 2 && type == kTypeHelo) word = "PLANE";
		else if (value == 3 && type == kTypeGround) word = "TRAIN";
		break;
	case Unit::Flags:
	case Unit::HuntFlags: {
		std::vector<std::string> words;
		uint32_t left = uint32_t(value);
		for (const FlagWord &flag : row.flags())
			if (left & flag.bit) {
				words.push_back(flag.word);
				left &= ~flag.bit;
			}
		if (left) return {};
		return words;
	}
	case Unit::Weapon: break;
	}
	if (word.empty()) return {};
	return {word};
}

Profile parse_profile(const uint8_t *text, size_t size) { return parse(text, size, nullptr, nullptr); }

Profile parse_profile(const uint8_t *text, size_t size, textlayout::Notes &notes, std::vector<UnreadLine> *unread) {
	return parse(text, size, &notes, unread);
}

bool same_profile(const Profile &a, const Profile &b) {
	const auto same_block = [](const WeaponBlock &x, const WeaponBlock &y) {
		return x.ammo == y.ammo && x.rate_ticks == y.rate_ticks && x.cone_bam == y.cone_bam && x.range == y.range &&
		       x.flags == y.flags && x.facing_bam == y.facing_bam && x.pitch_bam == y.pitch_bam && x.weapon == y.weapon;
	};
	return a.type == b.type && a.subtype == b.subtype && a.default_state == b.default_state &&
	       a.drive_skill == b.drive_skill && a.alert == b.alert && a.rank == b.rank &&
	       a.check_six_rate == b.check_six_rate && a.target_eval_rate == b.target_eval_rate &&
	       a.radio_distance == b.radio_distance && a.radio_delay == b.radio_delay && a.hunt_limit == b.hunt_limit &&
	       a.ground_patrol_speed == b.ground_patrol_speed && a.ground_combat_speed == b.ground_combat_speed &&
	       a.has_ground_patrol_speed == b.has_ground_patrol_speed &&
	       a.has_ground_combat_speed == b.has_ground_combat_speed && a.aim_skill == b.aim_skill &&
	       a.view_fov_bam == b.view_fov_bam && a.view_dist == b.view_dist && a.radar_fov_bam == b.radar_fov_bam &&
	       a.radar_dist == b.radar_dist && a.priority_air == b.priority_air && a.priority_ground == b.priority_ground &&
	       a.priority_organics == b.priority_organics && a.priority_decorations == b.priority_decorations &&
	       a.evade_flags == b.evade_flags && a.combat_flags == b.combat_flags && a.react_ticks == b.react_ticks &&
	       a.tether_dist == b.tether_dist && a.min_chase == b.min_chase && a.max_chase == b.max_chase &&
	       same_block(a.primary, b.primary) && same_block(a.secondary, b.secondary) &&
	       a.patrol_speed == b.patrol_speed && a.combat_speed == b.combat_speed &&
	       a.helo_patrol_speed == b.helo_patrol_speed && a.helo_patrol_altitude == b.helo_patrol_altitude &&
	       a.helo_patrol_climb == b.helo_patrol_climb && a.helo_combat_speed == b.helo_combat_speed &&
	       a.helo_combat_altitude == b.helo_combat_altitude && a.helo_combat_climb == b.helo_combat_climb &&
	       a.turn_rate_bam_tick == b.turn_rate_bam_tick && a.accel_ticks == b.accel_ticks &&
	       a.hunt_flags == b.hunt_flags && a.use_waypoint_z == b.use_waypoint_z && a.min_agl == b.min_agl &&
	       a.min_speed == b.min_speed;
}

bool write_profile(const Profile &profile, const textlayout::Notes *notes, std::string &text, std::string &error,
                   bool *rewritten) {
	if (rewritten) *rewritten = false;
	textlayout::OutRecord now;
	if (!records_of(profile, now, error, false)) return false;
	const std::string eol = "\r\n"; // the one break the walk splits at [orig: File_ParseASCIIFile @ 0x53D8C7]
	text = textlayout::compose(notes, now, textlayout::cut_ascii_walk, notes ? textlayout::file_eol(*notes, eol) : eol);
	const Profile again = parse_profile(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	if (same_profile(again, profile)) return true;
	if (notes && profile.note) {
		Profile unnoted = profile;
		unnoted.note = 0;
		if (!write_profile(unnoted, nullptr, text, error)) return false;
		if (rewritten) *rewritten = true;
		return true;
	}
	error = "The profile does not read back as it is (a value no line of the writer's carries).";
	return false;
}

ClassSpeeds class_speed_words(const Profile &profile, bool helicopter_init) {
	ClassSpeeds out;
	if (profile.type == 1) {
		if (helicopter_init) {
			out.speed_a = profile.helo_combat_speed; // +0xD4
			out.speed_b = profile.helo_patrol_speed; // +0xC8
		} else {
			out.speed_a = profile.hunt_limit;                       // +0xC4
			out.speed_b = static_cast<int32_t>(profile.hunt_flags); // +0xC0
		}
	} else if (profile.type == 2) {
		if (helicopter_init) {
			out.speed_a = profile.radio_delay;        // +0xD4
			out.speed_b = profile.turn_rate_bam_tick; // +0xC8
		} else {
			out.speed_a = profile.ground_combat_speed; // +0xC4
			out.speed_b = profile.ground_patrol_speed; // +0xC0
		}
	}
	return out;
}

} // namespace opennova::aip
