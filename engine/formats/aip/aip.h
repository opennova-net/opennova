#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/textlayout/text_layout.h>

namespace opennova::aip {

// An AI profile (`.aip`): the vehicle AI's settings, one profile a file, read when an item names it
// [orig: AIProfile_LoadOrFind @ 0x45FD80: a new slot of the 128 zeroed (memset 0xF8 @ 0x45FE09), its name
// copied, +24 cleared, then the file through the shared ASCII walk, File_ParseASCIIFile @ 0x45FE45, each
// line to AIProfile_ParseProperty @ 0x45DE70]. The reader is gated on the profile's type, which a `type`
// line sets: HELO (1) and GROUND (2) read their key sets, ORGANIC (3) and a profile of no type nothing more.
// A key no arm of the type reads is read for nothing (the shipped files' `description`, a GROUND file's
// `min_speed`). Keys compare without case; a value past the line's count reads "" (the reset token).

// One weapon block — profile+120 (primary) / +152 (secondary). The SM
// fire pump hands the block to the fire-transform solver, which reads
// +8 (cone), +16 (flags), +20 (facing) [orig: Entity_ComputeWeaponFireTransform_0
// @ 0x456980 reads weaponDef+8/+0x10/+0x14].
struct WeaponBlock {
    int32_t ammo = 0;        // +0: shot pool count  [orig: "primary_ammo" -> atol]
    int32_t rate_ticks = 0;  // +4: fire interval    [orig: "primary_rate" -> atof * 62.5, chop]
    int32_t cone_bam = 0;    // +8: aim cone half-angle [orig: "primary_fov" -> atof * 11930464.0 (deg->BAM32), chop]
    int32_t range = 0;       // +12: engage range 16.16 [orig: "primary_range" -> atol << 16]
    uint32_t flags = 0;      // +16: WEAPON_* mask (below) [orig: "primary_flags" token loop]
    int32_t facing_bam = 0;  // +20: yaw bias  [orig: "primary_facing" -> deg->BAM32]
    int32_t pitch_bam = 0;   // +24: pitch bias [orig: "primary_pitch" -> deg->BAM32]
    std::string weapon;      // +28 byte in retail: AmmoDef_LookupByName("primary_weap");
                             // the port keeps the authored name — the embedder resolves
                             // it against the loaded ammo table at the item-traits sweep.
};

// WEAPON_* flag names, verbatim from the parser's token compares
// [orig: AIProfile_ParseProperty primary_flags / secondary_flags loops, GROUND
//  @0x45E303 / @0x45E5B1, HELO @0x45F16D / @0x45F471]. The solver consumes them as:
// TURRET = track/stage the solution, SLOW/FAST = the two slew rates,
// PITCHLOCKED = commanded elevation (brain+0x314), PITCHLOCKED_MINUS45 =
// the fixed -45 deg elevation leg.
inline constexpr uint32_t kWeaponTurret = 0x1;
inline constexpr uint32_t kWeaponSlow = 0x2;
inline constexpr uint32_t kWeaponFast = 0x4;
inline constexpr uint32_t kWeaponPitchLocked = 0x8;
inline constexpr uint32_t kWeaponPitchLockedMinus45 = 0x10;

// COMBAT_FLAGS / EVADE_FLAGS names, verbatim [orig: the +100 / +96 token
// loops]: FOLLOW_WP 0x1, NO_ACTION 0x2, FLEE 0x4, NO_CAP 0x8, COUNTER 0x10,
// then COMBAT_FLAGS only: ATEAM 0x20, ATEAM_LOCK 0x40, RC_FIRE 0x80.
// (RC_FIRE is the stationary-fire mode the SM pump dispatches on.)

// A flag's word and its bit, in the order a writer puts them down.
struct FlagWord {
	const char *word;
	uint32_t bit;
};
const std::vector<FlagWord> &weapon_flag_words();
const std::vector<FlagWord> &evade_flag_words();  // EVADE_FLAGS: the first five
const std::vector<FlagWord> &combat_flag_words(); // COMBAT_FLAGS: all eight
const std::vector<FlagWord> &hunt_flag_words();   // hunt_flags: MAINTAIN_SPEED

// The AI states a profile's default_state names, as the name table holds them (its literal values: the
// table stores GROUND_FOLLOWWP = 17) [orig: AIState_LookupByName @ 0x457530, table @ 0x815198]; a word no
// row has reads 0.
struct StateName {
	const char *word;
	int32_t id;
};
const std::vector<StateName> &state_names();

inline constexpr int32_t kTypeHelo = 1;
inline constexpr int32_t kTypeGround = 2;
inline constexpr int32_t kTypeOrganic = 3;

struct Profile {
    int32_t type = 0;            // +16: HELO 1 / GROUND 2 / ORGANIC 3 [orig: "type"]
	int32_t subtype = 0; // +20: STD 0 / BOAT 1 / PLANE 2 / TRAIN 3
	int32_t default_state = 0; // +24, literal AIState_LookupByName value
	int32_t drive_skill = 0; // +32, drive_skill / flight_skill, clamped 0..4
	int32_t alert = 0; // +36, GREEN/YELLOW/RED
	int32_t rank = 0; // +60
	int32_t check_six_rate = 0; // +108, atof * 655.36
	int32_t target_eval_rate = 0; // +112, atof * 655.36
	int32_t radio_distance = 0; // HELO +240 / GROUND +208
	int32_t radio_delay = 0; // HELO +244 / GROUND +212
	int32_t hunt_limit = 0; // HELO +196, seconds *62.5
	int32_t ground_patrol_speed = 0; // parsed fractional speed, 16.16 u/tick
	int32_t ground_combat_speed = 0;
	bool has_ground_patrol_speed = false;
	bool has_ground_combat_speed = false;
	int32_t aim_skill = 0; // +28: atol clamped 0..4 [orig: "aim_skill"]
	int32_t view_fov_bam = 0;    // +64: deg->BAM32 [orig: "view_fov"]
    int32_t view_dist = 0;       // +68: atol << 16 [orig: "view_dist"]
    int32_t radar_fov_bam = 0;   // +72: deg->BAM32 [orig: "radar_fov"]
    int32_t radar_dist = 0;      // +76: atol << 16 [orig: "radar_dist"]
    int32_t priority_air = 0;        // +80  [orig: "priority_air" -> atol]
    int32_t priority_ground = 0;     // +84  [orig: "priority_ground"]
    int32_t priority_organics = 0;   // +88  [orig: "priority_organics"]
    int32_t priority_decorations = 0;// +92  [orig: "priority_decorations"]
    uint32_t evade_flags = 0;    // +96  [orig: "EVADE_FLAGS" token loop]
    uint32_t combat_flags = 0;   // +100 [orig: "COMBAT_FLAGS" token loop]
    int32_t react_ticks = 0;     // +104: atof * 62.5, chop [orig: "react_time"]
    int32_t tether_dist = 0;     // +116: atol << 16 [orig: "tether_dist"]
	int32_t min_chase = 0; // +184: atof * 65536, chop [orig: "min_chase_dist"]
	int32_t max_chase = 0; // +188: atof * 65536, chop [orig: "max_chase_dist"]
	WeaponBlock primary; // +120..+151 (+148 weap byte)
	WeaponBlock secondary;       // +152..+183 (+180 weap byte)
    // The GROUND speed keys as raw authored integers, -1 when absent: the boot
    // resolver's parsed-anything probe. Retail stores only the parsed value at
    // +192/+196 (ground_patrol_speed / ground_combat_speed above).
    int32_t patrol_speed = -1;   // [orig: "patrol_speed"]
    int32_t combat_speed = -1;   // [orig: "combat_speed"]

	// --- the HELO (type 1) flight set [orig: the type-1 branch of
	// AIProfile_ParseProperty @0x45f684..0x45f9eb — profile offsets +200..+236
	// (+56 for use_waypoint_z)]. Unlike the GROUND speed pair above, these
	// hold retail's PARSED values: a speed is km/h -> 16.16 units per tick
	// (atof * 1000.0 * 4.444444444444444e-06 * 65536.0, i.e. x65536/225), a
	// climb is atof * 0.016 * 65536.0, an altitude is atol << 16, min_agl is
	// atof * 65536, and turn_rate/accel_time carry their integer formulas.
	// These parsed fields seed the aircraft navigation and combat brain. Every
	// unauthored field is the zeroed record's 0 (the loader memsets the record).
	int32_t helo_patrol_speed = 0; // +200 [orig: "patrol_speed" (HELO branch) km/h -> 16.16 u/tick]
	int32_t helo_patrol_altitude = 0;  // +204 [orig: "patrol_altitude" atol << 16]
    int32_t helo_patrol_climb = 0;     // +208 [orig: "patrol_climb" atof * 0.016 * 65536]
    int32_t helo_combat_speed = 0;     // +212 [orig: "combat_speed" (HELO branch) km/h -> 16.16 u/tick]
    int32_t helo_combat_altitude = 0;  // +216 [orig: "combat_altitude" atol << 16]
    int32_t helo_combat_climb = 0;     // +220 [orig: "combat_climb" atof * 0.016 * 65536]
    int32_t turn_rate_bam_tick = 0;    // +224 [orig: "turn_rate" = 11930464 * deg / 62]
    int32_t accel_ticks = 0;           // +228 [orig: "accel_time" = 62 * seconds]
	uint32_t hunt_flags = 0; // +192, MAINTAIN_SPEED bit1 [orig: @0x45DE70]
	int32_t use_waypoint_z = 0; // +56  [orig: "use_waypoint_z" atol]
	int32_t min_agl = 0;               // +232 [orig: "min_agl" atof -> 16.16]
    int32_t min_speed = 0;             // +236 [orig: "min_speed" km/h -> 16.16 u/tick]

	// The file's modeled layout this profile was read with (textlayout: its note there, the file's own
	// record); 0 for a profile no file's layout names (written in the writer's form).
	uint64_t note = 0;
};

// Line-oriented, whitespace-tokenized, case-insensitive property parser.
Profile parse_profile(const uint8_t *text, size_t size);
// A line the reader reads nothing of (none of the walk's skipped lines: no token, or opening '/'): before any
// `type` (the profile of no type takes no key), under ORGANIC (which takes none), a key only the other type's set
// holds, or a key no arm has (every shipped file's `description`).
struct UnreadLine {
	enum class Why : uint8_t { NoType, Organic, OtherType, Unknown };
	Why why = Why::Unknown;
	size_t offset = 0; // the line's first byte
	std::string key;   // its first token
};
// The same parse with the file's layout modeled (`notes` filled: each line the arm that read it, as the
// writer's entry of its key, or none; the profile's note the file's own record), and, with `unread`, the lines
// it reads nothing of.
Profile parse_profile(const uint8_t *text, size_t size, textlayout::Notes &notes,
                      std::vector<UnreadLine> *unread = nullptr);

// What a key's value is in the file, the unit its arm reads it in [orig: AIProfile_ParseProperty @
// 0x45DE70's arms, cited per key in aip.cpp].
enum class Unit : uint8_t {
	Whole,      // atol
	Skill,      // atol clamped 0..4
	Metres,     // atol << 16
	Degrees,    // atof * 11930464.0, chopped to 64 bits, the low 32 kept (a BAM)
	Seconds,    // atof * 62.5, chopped (ticks): fistp, react_time and hunt_limit ftol
	Rate,       // atof * 655.36, chopped (ftol)
	Fixed,      // atof * 65536.0, chopped (ftol)
	Speed,      // km/h: atof * 1000.0 * 4.444444444444444e-06 * 65536.0, chopped (16.16 units a tick)
	Climb,      // atof * 0.016 * 65536.0, chopped
	TurnRate,   // 11930464 * atol / 62 (32-bit)
	AccelTime,  // 62 * atol
	State,      // an AI state's name (state_names)
	Alert,      // GREEN 0, YELLOW 1, RED 2 (any other word 0)
	Subtype,    // STD 0, BOAT 1 (GROUND), PLANE 2 (HELO), TRAIN 3 (GROUND); any other word keeps the value
	Flags,      // words, each a bit (flag_words), ORed in
	HuntFlags,  // MAINTAIN_SPEED; the line clears the mask first
	Weapon,     // an ammo's name
	Type,       // HELO 1, GROUND 2, ORGANIC 3
};

// A key the reader reads: its word as the reader compares it (`alias`, a second word the same arm reads,
// null for none), the types whose key set holds it (1 << type), its unit, and the profile's value it fills.
struct KeyRow {
	const char *key;
	const char *alias;
	uint8_t types;
	Unit unit;
	// The value the arm stores, as a whole number (a mask, a state id, a BAM, ...), and its store: the two
	// speeds a HELO profile's member or a GROUND profile's by the profile's type (a GROUND speed's store also
	// sets its raw whole number and that it was read). Null for a Weapon key (its name is `block`'s `name`).
	int32_t (*get)(const Profile &);
	void (*set)(Profile &, int32_t);
	std::string WeaponBlock::*name = nullptr; // a Weapon key's block member (with `block`)
	WeaponBlock Profile::*block = nullptr;
	// A Flags key's words.
	const std::vector<FlagWord> &(*flags)() = nullptr;
};
inline constexpr uint8_t kHeloKeys = 1u << kTypeHelo;
inline constexpr uint8_t kGroundKeys = 1u << kTypeGround;
// Every key, `type` first, in the order the writer puts them down.
const std::vector<KeyRow> &key_rows();
// The key row of `key` (any case, an alias included), null for none.
const KeyRow *key_row(const std::string &key);
// The value a key holds on a profile (0 for a Weapon key).
int32_t key_value(const KeyRow &row, const Profile &profile);
// Whether a profile of `type` reads `row`.
inline bool reads(const KeyRow &row, int32_t type) { return type >= 0 && type < 8 && (row.types & (1u << type)) != 0; }

// The value a key's arm stores from `word` (its first value; the Flags and HuntFlags keys read every value
// word: `words` is the line's values), over the value it held (a Subtype word no arm has keeps it, a Flags
// line ORs in).
int32_t read_value(const KeyRow &row, int32_t type, const std::vector<std::string> &words, int32_t held);
// The words the writer puts down for a key's value (its values after the key): the shortest decimal its arm
// reads back to the stored value, a whole number, a state's or a flag's words. Empty when no word reads back
// to it (a BAM no decimal reaches, a state id no name has).
std::vector<std::string> value_words(const KeyRow &row, int32_t type, int32_t value);

// The profile as the reader reads it again: the same type, every value the same (the layout aside).
bool same_profile(const Profile &a, const Profile &b);

// The profile's text from scratch (ADR 0003): `type` and then each key of its type whose value is other than
// the zeroed record's (the loader clears the slot first, AIProfile_LoadOrFind @ 0x45FE09), each `key<TAB>value`
// with CR LF after it (the walk splits at a CR LF pair alone), over the file's modeled layout where the
// profile has one (each line as the file had it but for the words of a changed value; a key put down anew
// after the key before it in the writer's order). The text is read again: one that would not read back as
// the profile is written in the writer's form, `rewritten` set. False with the reason for a value no word
// reads back to.
bool write_profile(const Profile &profile, const textlayout::Notes *notes, std::string &text, std::string &error,
                   bool *rewritten = nullptr);

// The two words a class init copies into brain[49]/brain[50], read at the
// profile offsets that init hard-codes, whatever key the profile's own type
// stored there. The helicopter init reads +0xD4/+0xC8 (a HELO profile's
// combat/patrol speed, a GROUND profile's radio_delay/turn_rate); the vehicle
// init reads +0xC4/+0xC0 (a GROUND profile's combat/patrol speed, a HELO
// profile's hunt_limit/hunt_flags). Any other type stores nothing there: the
// zeroed record's 0.
// [orig: Entity_InitHelicopterAIFromDef @0x468597..0x4685A9;
//  Entity_InitVehicleAIFromDef @0x4688C1..0x4688D3; the AIProfile_ParseProperty
//  stores GROUND +0xC0 @0x45E70B, +0xC4 @0x45F677, +0xC8 @0x45E7C4, +0xD4
//  @0x45E871 and HELO +0xC0 @0x45F5F6, +0xC4 @0x45F677, +0xC8 @0x45F70D,
//  +0xD4 @0x45F7DD]
struct ClassSpeeds {
    int32_t speed_a = 0; // brain[49]
    int32_t speed_b = 0; // brain[50]
};
ClassSpeeds class_speed_words(const Profile &profile, bool helicopter_init);

}  // namespace opennova::aip
