// opennova-3di weapon timing: Blender authors clip-relative phase markers,
// the weapon.def entries that play the clips and the eyes it views them from;
// this compiles them into the ACTION delays and view positions each entry
// sets and measures the result with weapon_fsm_tick. The authoring policy
// lives here; runtime behavior stays in the engine.

#include "weapon_timing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_view.h>

#include "scene_text.h"
#include "threedi_cli.h"

namespace threedi_cli {

namespace {

using namespace opennova::world;
namespace wa = opennova::world::weapon_action;
namespace io = opennova::io;

// A clip's channel advances fps/62/frames a tick, so one clip second is 62
// ticks of pose; DELAYEND and the firing period count 62.5 Hz logic ticks.
// [orig: AnimChannel_InitFromData @ 0x410560; Game_MainLoop @ 0x52b630]
constexpr double kClipTicksPerSecond = io::kTicksPerSecondInt;
constexpr double kRoundsPerMinutePerTick = 60.0 * io::kTickHz;
// The longest window an author may give: two minutes of logic ticks.
constexpr double kMaxSeconds = 120.0;

double rpm(int cycle_ticks) { return cycle_ticks ? kRoundsPerMinutePerTick / cycle_ticks : 0; }

std::string rpm_text(int cycle_ticks) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.0f", rpm(cycle_ticks));
	return buf;
}

// A weapon.def number: three decimals, the zeros they do not need dropped.
std::string def_number(double v) {
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.3f", v);
	std::string s = buf;
	while (!s.empty() && s.back() == '0') s.pop_back();
	if (!s.empty() && s.back() == '.') s.pop_back();
	if (s == "-0" || s.empty()) s = "0";
	return s;
}

// The action's clip key: the slot table's wpn_* entry for it, which the add-on
// names its table row by. [orig: g_AnimStateNameTable @ 0x8135F0]
std::string anim_key(int action) { return infantry_anim_key(weapon_action_anim_slot(action)); }

WeaponFsmDef bake(const std::vector<WeaponFsmActionRow> &rows, const std::string &mode) {
	WeaponFsmDef def;
	weapon_fsm_bake(rows.data(), rows.size(), [](void *, const char *) { return 1; }, nullptr, nullptr, def);
	def.auto_fire = mode == "auto";
	def.burst3 = mode == "burst";
	def.clip_capacity = 10000;
	// The recoil decision's effect leg is the observable eject tick; it fires
	// only when the row names a particle, which gates nothing else.
	std::strcpy(def.actions[wa::kRecoil].particle, "timing_preview");
	return def;
}

struct Measured {
	std::vector<WeaponTimingEvent> events;
	std::vector<WeaponTimingShown> shown;
	int cycle_ticks = 0;
	int ready_tick = -1;
};

// Synthetic inputs exercise a full magazine in ideal conditions. Semi presses
// again only once Idle admits the edge; burst measures the within-burst rate.
bool measure(const std::vector<WeaponFsmActionRow> &rows, const std::string &mode, Measured &out,
             int requested_action = wa::kFire) {
	const auto def = bake(rows, mode);
	WeaponSlotState slot;
	slot.clip = 10000;
	slot.reserve = 10000;
	slot.phase = weapon_phase::kDone;
	if (requested_action != wa::kFire) slot.next = requested_action;
	int last_shot = -1, shots = 0, clip_ticks = 0, playing = -1;
	out = Measured{};
	// The clip an action started shows until the next play replaces it.
	const auto close_clip = [&] {
		if (playing < 0) return;
		for (auto &s : out.shown)
			if (s.action == playing) {
				s.clip_ticks = std::max(s.clip_ticks, clip_ticks);
				return;
			}
		out.shown.push_back({playing, requested_action, clip_ticks});
	};
	int budget = 128;
	for (const auto &row : rows) budget += 5 * (row.delaystart + row.delayend + 2);
	for (int tick = 0; tick < budget; ++tick) {
		WeaponFsmInputs in;
		in.current_tick = tick;
		if (requested_action == wa::kFire) {
			in.fire_pressed = tick == 0 || (mode == "semi" && slot.current == wa::kIdle);
			in.fire_held = mode == "auto";
		}
		WeaponFsmEvents ev;
		const int previous = slot.current;
		weapon_fsm_tick(def, slot, in, ev);
		if (ev.play_anim) {
			close_clip();
			playing = slot.current;
			clip_ticks = 0;
		}
		if (ev.advance_anim) ++clip_ticks;
		auto record = [&](const char *kind) {
			out.events.push_back({tick, slot.current, kind, clip_ticks, requested_action});
		};
		if (slot.current != previous) record("enter");
		if (ev.play_anim) record("play");
		if (ev.action_finished >= 0) record("active_end");
		if (ev.action_effect == wa::kRecoil) record("eject");
		if (ev.reload_applied) record("reload_ammo");
		if (ev.switch_completed) record("switch_complete");
		const bool settled = slot.current == wa::kIdle ||
		                     (requested_action == wa::kEmpty && slot.current == wa::kEmptyIdle);
		if (tick > 0 && settled && slot.current != previous) {
			if (out.ready_tick < 0) out.ready_tick = tick;
			record("ready");
			if (requested_action != wa::kFire || mode == "burst") {
				close_clip();
				return true;
			}
		}
		if (ev.fired) {
			record("shot");
			if (shots == 1) out.cycle_ticks = tick - last_shot;
			last_shot = tick;
			if (++shots == 3 && mode != "burst") {
				close_clip();
				return true;
			}
		}
	}
	return false;
}

std::string json_string(const std::string &s) {
	std::string out = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') out += '\\';
		out += c;
	}
	return out + "\"";
}

std::string json(const WeaponTimingPlan &plan) {
	std::ostringstream s;
	s << "{\"tick_rate\":" << io::kTickHz << ",\"clip_ticks_per_second\":" << kClipTicksPerSecond;
	const auto view = [&](const char *key, bool given, const double *units) {
		if (!given) return;
		s << ",\"" << key << "\":[" << def_number(units[0]) << "," << def_number(units[1]) << ","
		  << def_number(units[2]) << "]";
	};
	view("pos", plan.pos_given, plan.pos_units);
	view("tpos", plan.tpos_given, plan.tpos_units);
	s << ",\"entries\":[";
	const char *entry_sep = "";
	for (const auto &e : plan.entries) {
		s << entry_sep << "{\"name\":" << json_string(e.name) << ",\"mode\":" << json_string(e.mode)
		  << ",\"cycle_ticks\":" << e.cycle_ticks << ",\"rpm\":" << rpm(e.cycle_ticks)
		  << ",\"ready_tick\":" << e.ready_tick << ",\"rows\":[";
		entry_sep = ",";
		const char *sep = "";
		for (const auto &r : e.rows) {
			// The last clip time the action's own run shows: the firing run for
			// fire and recoil, its own run for the others; -1 when it has none
			// (the idles are not run, and a recoil nobody authored plays no clip).
			const int scenario = r.action == wa::kRecoil ? wa::kFire : r.action;
			double shown = -1;
			for (const auto &sh : e.shown)
				if (sh.action == r.action && sh.scenario == scenario) shown = sh.clip_ticks / kClipTicksPerSecond;
			s << sep << "{\"action\":" << json_string(kWeaponActionSuffixes[r.action])
			  << ",\"anim\":" << json_string(r.anim) << ",\"delaystart\":" << r.delaystart
			  << ",\"delayend\":" << r.delayend << ",\"shown_s\":" << shown << "}";
			sep = ",";
		}
		s << "],\"events\":[";
		sep = "";
		for (const auto &ev : e.events) {
			s << sep << "{\"tick\":" << ev.tick << ",\"action\":" << json_string(kWeaponActionSuffixes[ev.action])
			  << ",\"scenario\":" << json_string(kWeaponActionSuffixes[ev.scenario])
			  << ",\"kind\":" << json_string(ev.kind) << ",\"clip_seconds\":" << ev.clip_ticks / kClipTicksPerSecond
			  << "}";
			sep = ",";
		}
		s << "]}";
	}
	s << "]}";
	return s.str();
}

bool fail(std::string &error, const std::string &why) {
	error = why;
	return false;
}

} // namespace

bool weapon_plain_name(const std::string &s, size_t max_chars) {
	if (s.empty() || s.size() > max_chars) return false;
	for (unsigned char c : s)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '-' || c == '.'))
			return false;
	return true;
}

int weapon_action_named(const std::string &suffix) {
	for (int i = 0; i < wa::kCount; ++i)
		if (opennova::strutil::iequals(suffix, kWeaponActionSuffixes[i])) return i;
	return -1;
}

// The model is posed at the eye plus the entry's view offset, pos / 256 in
// the view frame (x forward, y left, z up), and the rig maps the model's
// mission axes onto that same frame, so the eye stands at -pos / 256 of the
// model: pos = -eye * 256. [orig: Player_UpdateFirstPersonCamera @ 0x4dd380,
// the def position ftol'd onto the view @ 0x4dd479..0x4dd490 and the
// view-local rotate @ 0x4dd5d8; the parser's * 256 @ 0x544770;
// world/player_view.h kWeaponDefPosScale]
void weapon_def_view_from_eye(const double metres[3], double units[3]) {
	for (int i = 0; i < 3; ++i) units[i] = -metres[i] * static_cast<double>(kWeaponDefPosScale);
}

bool plan_weapon_timing(const WeaponTimingRequest &request, WeaponTimingPlan &out, std::string &error) {
	out = {};
	// The authored rows: ANIM from the action's own slot, the delays from its
	// marked windows.
	std::vector<WeaponFsmActionRow> rows;
	std::vector<WeaponTimingRow> authored;
	std::set<int> seen;
	int fire_row = -1, recoil_row = -1;
	for (const auto &a : request.actions) {
		if (a.action < 0 || a.action >= wa::kCount) return fail(error, "unknown weapon action");
		const std::string suffix = kWeaponActionSuffixes[a.action];
		if (!seen.insert(a.action).second) return fail(error, suffix + " is authored twice");
		// OVERHEATED is never entered: no writer of a slot's next action
		// stores 11, so its row serves only the heat glow, which reads the
		// row's userpoint, and it names no anim slot of its own to author a
		// clip for. [orig: the heat-glow leg reads actionTable[11]
		// @ 0x5410A7..0x541108; WeaponSlot_RequestReload's dead == 11 test
		// @ 0x53F12D; g_AnimStateNameTable @ 0x8135F0]
		if (weapon_action_anim_slot(a.action) < 0)
			return fail(error, suffix + " is never entered (nothing queues it) and has no anim slot of its own; "
			                            "its weapon.def row only places the heat glow, so set it there by hand");
		if (!std::isfinite(a.active_seconds) || !std::isfinite(a.recovery_seconds) || a.active_seconds < 0 ||
		    a.recovery_seconds < 0 || a.active_seconds > kMaxSeconds || a.recovery_seconds > kMaxSeconds)
			return fail(error, suffix + ": phase times must be finite, nonnegative and at most 120 seconds");
		WeaponFsmActionRow row;
		std::strcpy(row.name, suffix.c_str());
		std::strcpy(row.anim, anim_key(a.action).c_str());
		// Active-end markers refer to the visible clip pose. The entry tick
		// plays frame zero and the counter-zero tick does not advance it:
		// positive boundaries need one extra counter tick (begin_active).
		row.delaystart = a.active_seconds == 0 ? 0 : int(std::lround(a.active_seconds * kClipTicksPerSecond)) + 1;
		row.delayend = int(std::lround(a.recovery_seconds * io::kTickHz));
		if (a.action == wa::kSwitchTo || a.action == wa::kSwitchFrom) {
			if (a.active_seconds != 0 || a.recovery_seconds != 0)
				return fail(error, suffix + ": draw and holster use the fixed switch timer; they take no phase times");
			// Enter the timer leg on the entry tick. A two-tick counter keeps
			// the channel advancing when the pump decrements it, while the
			// switch handler itself refreshes that counter each tick.
			// This is an authoring preset over the existing handler, not a
			// replacement timing rule. The actual duration is in the trace.
			row.delaystart = 0;
			row.delayend = 2;
		}
		if (a.action == wa::kFire) {
			// Each entry's shot period sets the fire recovery.
			if (a.recovery_seconds != 0)
				return fail(error, "fire: its recovery comes from each entry's shot period; give 0");
			fire_row = int(rows.size());
		}
		if (a.action == wa::kRecoil) recoil_row = int(rows.size());
		rows.push_back(row);
		authored.push_back({a.action, row.anim, row.delaystart, row.delayend});
	}
	if (fire_row < 0) return fail(error, "author a fire action: every entry's shot period is solved on it");
	// The firing cycle runs through the recoil decision, so its delays are
	// always set: the recoil nobody authored is a zero window whose ANIM the
	// entry keeps.
	if (recoil_row < 0) {
		WeaponFsmActionRow row;
		std::strcpy(row.name, kWeaponActionSuffixes[wa::kRecoil]);
		row.delaystart = row.delayend = 0;
		recoil_row = int(rows.size());
		rows.push_back(row);
		authored.push_back({wa::kRecoil, std::string(), 0, 0});
	}
	if (request.entries.empty()) return fail(error, "name at least one weapon.def entry that plays these clips");

	for (const WeaponTimingEye *eye : {&request.pos, &request.tpos}) {
		if (!eye->given) continue;
		for (double m : eye->metres)
			// The view position is 16.16 fixed point after the parser's * 256.
			if (!std::isfinite(m) || std::fabs(m) >= 32768.0)
				return fail(error, "an eye position must be finite and within 32768 metres of the model");
	}
	out.pos_given = request.pos.given;
	out.tpos_given = request.tpos.given;
	if (out.pos_given) weapon_def_view_from_eye(request.pos.metres, out.pos_units);
	if (out.tpos_given) weapon_def_view_from_eye(request.tpos.metres, out.tpos_units);

	std::set<std::string> names;
	for (const auto &entry : request.entries) {
		if (!weapon_plain_name(entry.name, kWeaponEntryNameMax))
			return fail(error, "entry '" + entry.name + "': a weapon.def entry name is 1 to 31 letters, digits, _, - or .");
		if (!names.insert(opennova::strutil::to_lower(entry.name)).second)
			return fail(error, "entry " + entry.name + " is named twice");
		if (entry.mode != "auto" && entry.mode != "semi" && entry.mode != "burst")
			return fail(error, entry.name + ": the fire mode is semi, auto or burst");
		if (!std::isfinite(entry.cycle_seconds) || entry.cycle_seconds <= 0 || entry.cycle_seconds > kMaxSeconds)
			return fail(error, entry.name + ": the shot period must be greater than zero and at most 120 seconds");
		WeaponFsmActionRow &recoil = rows[recoil_row];
		// Held auto fire re-arms only in the closing ticks of the recoil's
		// recovery (or on a zero-length recoil), so a recoil with an active
		// phase and no recovery fires one shot a press.
		// [orig: WeaponAction_Recoil @ 0x542dd0, the deferred refire
		//  @ 0x542e7f..0x542e9d; runtime/world/weapon_fsm.cpp handler_recoil]
		if (entry.mode == "auto" && recoil.delaystart > 0 && recoil.delayend == 0)
			return fail(error, entry.name + ": auto fire re-arms only during the recoil's recovery, so a recoil "
			                                "with an active phase (" + std::to_string(recoil.delaystart) +
			                                " ticks) needs a recovery of at least one tick (1/62.5 s)");
		const int wanted = int(std::lround(entry.cycle_seconds * io::kTickHz));
		// The fastest the FSM fires in this mode, with every window zero.
		std::vector<WeaponFsmActionRow> bare(2);
		std::strcpy(bare[0].name, kWeaponActionSuffixes[wa::kFire]);
		std::strcpy(bare[1].name, kWeaponActionSuffixes[wa::kRecoil]);
		bare[0].delaystart = bare[0].delayend = bare[1].delaystart = bare[1].delayend = 0;
		Measured floor, probe;
		if (!measure(bare, entry.mode, floor) || floor.cycle_ticks <= 0)
			return fail(error, entry.name + ": the weapon FSM does not complete a firing cycle");
		if (wanted < floor.cycle_ticks)
			return fail(error, entry.name + ": " + rpm_text(std::max(wanted, 1)) + " rounds a minute is past the weapon "
			                   "FSM, which fires " + entry.mode + " at most every " + std::to_string(floor.cycle_ticks) +
			                   " ticks (" + rpm_text(floor.cycle_ticks) + " rounds a minute)");
		auto entry_rows = rows;
		entry_rows[fire_row].delayend = 0;
		if (!measure(entry_rows, entry.mode, probe) || probe.cycle_ticks <= 0)
			return fail(error, entry.name + ": these action windows do not complete a firing cycle in the weapon FSM");
		if (wanted < probe.cycle_ticks)
			return fail(error, entry.name + ": the fire and recoil markers need at least " +
			                   std::to_string(probe.cycle_ticks) + " ticks a shot (" + rpm_text(probe.cycle_ticks) +
			                   " rounds a minute); move them earlier or lower the rate");
		// Solve against the runtime, including semi's Idle edge and burst's
		// continuation. No front end keeps a second copy of the state machine.
		int lo = 0, hi = wanted;
		while (lo < hi) {
			const int mid = (lo + hi) / 2;
			entry_rows[fire_row].delayend = mid;
			if (!measure(entry_rows, entry.mode, probe)) return fail(error, entry.name + ": the firing cycle stalls");
			if (probe.cycle_ticks < wanted) lo = mid + 1;
			else hi = mid;
		}
		entry_rows[fire_row].delayend = lo;
		if (!measure(entry_rows, entry.mode, probe)) return fail(error, entry.name + ": the firing cycle stalls");
		WeaponTimingEntryPlan plan;
		plan.name = entry.name;
		plan.mode = entry.mode;
		plan.cycle_ticks = probe.cycle_ticks;
		plan.ready_tick = probe.ready_tick;
		plan.events = probe.events;
		plan.shown = probe.shown;
		plan.rows = authored;
		plan.rows[fire_row].delayend = lo;
		// Also preview each authored non-firing action independently. Its
		// tick zero is its own entry, identified by its action field.
		for (const auto &row : entry_rows) {
			const int action = weapon_action_named(row.name);
			if (action == wa::kFire || action == wa::kRecoil || action == wa::kIdle || action == wa::kEmptyIdle)
				continue;
			Measured own;
			measure(entry_rows, entry.mode, own, action);
			for (const auto &event : own.events)
				if (plan.events.size() < 256) plan.events.push_back(event);
			plan.shown.insert(plan.shown.end(), own.shown.begin(), own.shown.end());
		}
		// What shows of the clips the author made (the idle handlers also
		// replay the idle clip by number, which is no authored clip).
		plan.shown.erase(std::remove_if(plan.shown.begin(), plan.shown.end(),
		                                [&](const WeaponTimingShown &s) {
			                                const WeaponTimingRow *r = nullptr;
			                                for (const auto &a : plan.rows)
				                                if (a.action == s.action) r = &a;
			                                return r == nullptr || r->anim.empty();
		                                }),
		                 plan.shown.end());
		out.entries.push_back(std::move(plan));
	}
	return true;
}

std::string weapon_edits_text(const WeaponTimingPlan &plan) {
	std::ostringstream s;
	s << "weapon_edits 1\n"
	  << "# opennova-3di weapon timing: the weapon.def keys each entry sets, and no others.\n"
	  << "# Apply them with: opennova-3di weapon merge <weapon.def> <this file> -o <out.def>\n";
	for (const auto &e : plan.entries) {
		s << "\nentry " << e.name << " " << e.mode << "\n"
		  << "# " << e.cycle_ticks << " ticks a shot at " << io::kTickHz << " Hz: " << rpm_text(e.cycle_ticks)
		  << " rounds a minute\n";
		if (plan.pos_given)
			s << "pos " << def_number(plan.pos_units[0]) << " " << def_number(plan.pos_units[1]) << " "
			  << def_number(plan.pos_units[2]) << "\n";
		if (plan.tpos_given)
			s << "tpos " << def_number(plan.tpos_units[0]) << " " << def_number(plan.tpos_units[1]) << " "
			  << def_number(plan.tpos_units[2]) << "\n";
		for (const auto &r : e.rows) {
			const char *suffix = kWeaponActionSuffixes[r.action];
			if (!r.anim.empty()) s << "action " << suffix << " anim " << r.anim << "\n";
			s << "action " << suffix << " delaystart " << r.delaystart << "\n"
			  << "action " << suffix << " delayend " << r.delayend << "\n";
		}
	}
	return s.str();
}

int cmd_weapon_timing(const char *input, const char *output) {
	std::ifstream file(input);
	if (!file) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", input);
		return 1;
	}
	WeaponTimingRequest request;
	std::string raw, error;
	int line = 0;
	bool header = false;
	while (std::getline(file, raw)) {
		++line;
		SceneLine in(strip_comment(raw), SceneNumbers::finite);
		if (in.tokens.empty() && in.bad.empty()) continue;
		std::string why = in.bad;
		const std::string key = in.bad.empty() ? in.key() : std::string();
		if (!why.empty()) {
		} else if (!header) {
			long long version = 0;
			if (key != "weapon_timing" || !in.integer(version, 2, 2) || in.more())
				why = "a timing request starts with `weapon_timing 2`";
			header = why.empty();
		} else if (key == "action") {
			WeaponTimingAction action;
			std::string suffix;
			if (!in.name(suffix) || !in.number(action.active_seconds) || !in.number(action.recovery_seconds) ||
			    in.more())
				why = "action needs a suffix, an active time and a recovery time";
			else if ((action.action = weapon_action_named(suffix)) < 0)
				why = "'" + suffix + "' is no weapon action suffix";
			else
				request.actions.push_back(action);
		} else if (key == "entry") {
			WeaponTimingEntry entry;
			if (!in.name(entry.name) || !in.name(entry.mode) || !in.number(entry.cycle_seconds) || in.more())
				why = "entry needs a weapon.def entry name, a fire mode and a shot period";
			else
				request.entries.push_back(entry);
		} else if (key == "view") {
			std::string which;
			double metres[3];
			if (!in.name(which) || (which != "pos" && which != "tpos") || !in.numbers(metres, 3) || in.more()) {
				why = "view needs pos or tpos and an eye position x y z";
			} else {
				WeaponTimingEye &eye = which == "pos" ? request.pos : request.tpos;
				if (eye.given) why = "view " + which + " is given twice";
				eye.given = true;
				std::copy(metres, metres + 3, eye.metres);
			}
		} else {
			why = "unknown record `" + key + "`";
		}
		if (!why.empty()) {
			std::fprintf(stderr, "%s:%d: %s\n", input, line, why.c_str());
			return 1;
		}
	}
	WeaponTimingPlan plan;
	if (!header) error = "the request is empty";
	if (!header || !plan_weapon_timing(request, plan, error)) {
		std::fprintf(stderr, "opennova-3di: weapon timing: %s\n", error.c_str());
		return 1;
	}
	const auto text = weapon_edits_text(plan);
	if (!write_output(output, text.data(), text.size())) return 1;
	std::puts(json(plan).c_str());
	return 0;
}

} // namespace threedi_cli
