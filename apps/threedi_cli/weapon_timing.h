// opennova-3di weapon: the weapon.def half of a first-person weapon.
//
//   weapon timing <timing.txt> -o <edits.txt>
//   weapon merge  <weapon.def> <edits.txt> -o <out.def>
//
// `timing` compiles a rig's authored action windows (the Blender add-on's
// Action-local markers, as seconds), the weapon.def entries that share its
// clips (each with its fire mode and shot period) and the eye positions an
// author placed, into the weapon.def keys to set, and measures every entry
// with the engine's own weapon FSM (runtime/world/weapon_fsm.h). `merge` sets
// those keys in a copy of a weapon.def and leaves every other byte as it was.
// No imported weapon or animation is an input (ADR 0047 decision 14). Both
// grammars and the JSON `timing` prints are docs/anim/weapon-timing-format.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/world/weapon_fsm.h>

namespace threedi_cli {

// A weapon.def entry name's field: 31 characters and its terminator
// [orig: WeaponDefs_ParseLineCallback, strncpy 32 into entry+0x14 @ 0x543737].
inline constexpr size_t kWeaponEntryNameMax = 31;
// A plain name: 1 to `max_chars` letters, digits, `_`, `-` or `.`.
bool weapon_plain_name(const std::string &s, size_t max_chars);
// The weapon action a suffix names (any case), or -1.
int weapon_action_named(const std::string &suffix);

// One authored action: its suffix (a weapon_action id), its active phase (entry
// to the marked pose) and its recovery after it, in seconds of clip.
struct WeaponTimingAction {
	int action = 0;
	double active_seconds = 0;
	double recovery_seconds = 0;
};

// One weapon.def entry that plays the clips: its name, its fire mode (semi,
// auto or burst, which must agree with the entry's FLAGS) and the requested
// shot-to-shot period.
struct WeaponTimingEntry {
	std::string name;
	std::string mode;
	double cycle_seconds = 0;
};

// An eye the author placed: metres, the model's mission axes (x forward, y
// left, z up), relative to the model's origin.
struct WeaponTimingEye {
	bool given = false;
	double metres[3] = {};
};

struct WeaponTimingRequest {
	std::vector<WeaponTimingAction> actions;
	std::vector<WeaponTimingEntry> entries;
	WeaponTimingEye pos;  // the hip view
	WeaponTimingEye tpos; // the aimed view
};

// The keys one ACTION block sets: ANIM (empty = leave it as it is, the
// recoil nobody authored), DELAYSTART and DELAYEND.
struct WeaponTimingRow {
	int action = 0;
	std::string anim;
	int32_t delaystart = 0;
	int32_t delayend = 0;
};

// One FSM observation: `scenario` is the action the measured run requested
// (fire, or the non-firing action previewed on its own); tick 0 is its entry.
// `clip_ticks` counts the ticks of pose the viewmodel's clip has advanced.
struct WeaponTimingEvent {
	int tick = 0;
	int action = 0;
	std::string kind;
	int clip_ticks = 0;
	int scenario = opennova::world::weapon_action::kFire;
};

// The furthest an action's clip got before another clip replaced it or the
// run ended: the last pose of it anyone sees.
struct WeaponTimingShown {
	int action = 0;
	int scenario = 0;
	int clip_ticks = 0;
};

struct WeaponTimingEntryPlan {
	std::string name;
	std::string mode;
	std::vector<WeaponTimingRow> rows; // in request order, the recoil last when unauthored
	std::vector<WeaponTimingEvent> events;
	std::vector<WeaponTimingShown> shown;
	int cycle_ticks = 0;
	int ready_tick = -1;
};

struct WeaponTimingPlan {
	std::vector<WeaponTimingEntryPlan> entries;
	bool pos_given = false;
	bool tpos_given = false;
	double pos_units[3] = {}; // the weapon.def `pos` columns (raw def units)
	double tpos_units[3] = {};
};

// The raw weapon.def `pos`/`tpos` position an eye at `metres` takes.
void weapon_def_view_from_eye(const double metres[3], double units[3]);

bool plan_weapon_timing(const WeaponTimingRequest &request, WeaponTimingPlan &out, std::string &error);
// The edits file `timing` writes and `merge` reads.
std::string weapon_edits_text(const WeaponTimingPlan &plan);

// One entry of an edits file: the keys to set and nothing else.
struct WeaponEditKey {
	int action = 0;
	std::string key;   // anim, delaystart or delayend
	std::string value; // as the def will hold it
};

struct WeaponEditEntry {
	std::string name;
	std::string mode;
	bool pos_given = false;
	bool tpos_given = false;
	std::string pos[3];
	std::string tpos[3];
	std::vector<WeaponEditKey> keys;
};

bool parse_weapon_edits(const std::string &text, std::vector<WeaponEditEntry> &out, std::string &error);

// Set `edits` in a copy of the weapon.def `def`: every other byte is kept.
// `notes` receives what the author should know about an entry the edits do
// not change (a SIGHTS card that replaces the aimed model).
bool merge_weapon_def(const std::string &def, const std::vector<WeaponEditEntry> &edits,
		std::string &out, std::vector<std::string> &notes, std::string &error);

int cmd_weapon_timing(const char *input, const char *output);
int cmd_weapon_merge(const char *def_path, const char *edits_path, const char *output);

} // namespace threedi_cli
