// opennova-3di weapon timing: Blender authors clip-relative phase markers and
// a firing cadence; this compiles them into explicit ACTION delays and
// measures the result with weapon_fsm_tick. The authoring policy lives here;
// runtime behavior stays in the engine.

#include "weapon_timing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include <base/io/tick_rate.h>

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

int role_id(const std::string &name) {
	for (int i = 0; i < wa::kCount; ++i)
		if (name == kWeaponActionSuffixes[i]) return i;
	return -1;
}

bool identifier(const std::string &s, size_t limit, bool empty = false) {
	if (s.empty()) return empty;
	if (s.size() >= limit) return false;
	for (unsigned char c : s)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '-' || c == '.'))
			return false;
	return true;
}

double rpm(int cycle_ticks) { return cycle_ticks ? kRoundsPerMinutePerTick / cycle_ticks : 0; }

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

// Synthetic inputs exercise a full magazine in ideal conditions. Semi presses
// again only once Idle admits the edge; burst measures the within-burst rate.
bool measure(const std::vector<WeaponFsmActionRow> &rows, const std::string &mode, WeaponTimingPlan &out,
             int requested_action = wa::kFire) {
	const auto def = bake(rows, mode);
	WeaponSlotState slot;
	slot.clip = 10000;
	slot.reserve = 10000;
	slot.phase = weapon_phase::kDone;
	if (requested_action != wa::kFire) slot.next = requested_action;
	int first_shot = -1, last_shot = -1, shots = 0, clip_ticks = 0;
	out.events.clear();
	out.ready_tick = -1;
	out.cycle_ticks = 0;
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
		if (ev.play_anim) clip_ticks = 0;
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
			if (requested_action != wa::kFire || mode == "burst") return true;
		}
		if (ev.fired) {
			record("shot");
			if (first_shot < 0) first_shot = tick;
			if (shots == 1) out.cycle_ticks = tick - last_shot;
			last_shot = tick;
			if (++shots == 3 && mode != "burst") return true;
		}
	}
	return false;
}

std::string json(const WeaponTimingRequest &request, const WeaponTimingPlan &plan) {
	std::ostringstream s;
	s << "{\"mode\":\"" << request.mode << "\",\"tick_rate\":" << io::kTickHz
	  << ",\"cycle_ticks\":" << plan.cycle_ticks << ",\"rpm\":" << rpm(plan.cycle_ticks)
	  << ",\"ready_tick\":" << plan.ready_tick << ",\"rows\":[";
	const char *sep = "";
	for (const auto &r : plan.rows) {
		s << sep << "{\"role\":\"" << r.name << "\",\"anim\":\"" << r.anim << "\",\"delaystart\":" << r.delaystart
		  << ",\"delayend\":" << r.delayend << "}";
		sep = ",";
	}
	s << "],\"events\":[";
	sep = "";
	for (const auto &e : plan.events) {
		s << sep << "{\"tick\":" << e.tick << ",\"action\":\"" << kWeaponActionSuffixes[e.action]
		  << "\",\"scenario\":\"" << kWeaponActionSuffixes[e.scenario] << "\",\"kind\":\"" << e.kind
		  << "\",\"clip_seconds\":" << e.clip_ticks / kClipTicksPerSecond << "}";
		sep = ",";
	}
	s << "]}";
	return s.str();
}

// Every shipped ACTION row names its own suffix's handler, wpn_std_<suffix>
// [orig: g_ActionFuncDefTable @ 0x829E58; docs/net/novaworld-net-re.md, the
// ACTION-row keys].
std::string snippet(const WeaponTimingRequest &request, const WeaponTimingPlan &plan) {
	std::ostringstream s;
	s << "// Generated weapon.def ACTION blocks. Merge into your weapon entry.\n"
	  << "// Fire mode: " << request.mode << ". Preserve other weapon flags; Auto="
	  << (request.mode == "auto" ? "on" : "off") << ", Burst=" << (request.mode == "burst" ? "on" : "off")
	  << ".\n"
	  << "// Actual cycle: " << plan.cycle_ticks << " ticks at " << io::kTickHz << " Hz; "
	  << rpm(plan.cycle_ticks) << " RPM.\n"
	  << "// Reload ammo is applied on entry. Switch handlers also use their built-in timer.\n";
	for (const auto &r : plan.rows) {
		s << "\nACTION \"" << r.name << "\"\n"
		  << "    FUNCTION wpn_std_" << r.name << "\n";
		if (r.anim[0]) s << "    ANIM " << r.anim << "\n";
		s << "    DELAYSTART " << r.delaystart << "\n    DELAYEND " << r.delayend << "\n";
		for (auto item : {std::pair<const char *, const char *>("SOUNDSET", r.soundset),
		                  {"SOUNDSETEND", r.soundsetend},
		                  {"PARTICLE", r.particle},
		                  {"PARTICLEUSERPOINT", r.particleuserpoint}})
			if (*item.second) s << "    " << item.first << " " << item.second << "\n";
		s << "END\n";
	}
	return s.str();
}

} // namespace

bool plan_weapon_timing(const WeaponTimingRequest &request, WeaponTimingPlan &out, std::string &error) {
	out = {};
	auto fail = [&](const std::string &why) {
		error = why;
		return false;
	};
	if (request.mode != "auto" && request.mode != "semi" && request.mode != "burst")
		return fail("mode must be auto, semi or burst");
	if (!std::isfinite(request.cycle_seconds) || request.cycle_seconds <= 0 || request.cycle_seconds > 120)
		return fail("the requested shot interval must be greater than zero and at most 120 seconds");
	std::set<int> roles;
	int fire_row = -1;
	for (const auto &a : request.actions) {
		const int role = role_id(a.row.name);
		if (role < 0 || !roles.insert(role).second) return fail("unknown or duplicate weapon action role");
		// OVERHEATED binds the idle handler, has no wpn_std_ function of its own
		// and no wpn_ anim state for its clip to resolve by, and no shipped
		// weapon.def authors it. [orig: the suffix table @ 0x830B90;
		// g_ActionFuncDefTable @ 0x829E58; g_AnimStateNameTable @ 0x8135F0]
		if (role == wa::kOverheated) return fail("overheated is not an authorable weapon action");
		for (const char *name : {a.row.anim, a.row.soundset, a.row.soundsetend, a.row.particle, a.row.particleuserpoint})
			if (!identifier(name, 64, true))
				return fail("animation, sound and effect references must be identifiers under 64 characters");
		if (!std::isfinite(a.active_seconds) || !std::isfinite(a.recovery_seconds) || a.active_seconds < 0 ||
		    a.recovery_seconds < 0 || a.active_seconds > 120 || a.recovery_seconds > 120)
			return fail("phase times must be finite, nonnegative and at most 120 seconds");
		auto row = a.row;
		// Active-end markers refer to the visible clip pose. The entry tick
		// plays frame zero and the counter-zero tick does not advance it:
		// positive boundaries need one extra counter tick (begin_active).
		row.delaystart = a.active_seconds == 0 ? 0 : int(std::lround(a.active_seconds * kClipTicksPerSecond)) + 1;
		row.delayend = int(std::lround(a.recovery_seconds * io::kTickHz));
		if (role == wa::kSwitchTo || role == wa::kSwitchFrom) {
			if (a.active_seconds != 0 || a.recovery_seconds != 0)
				return fail("draw and holster use the fixed switch timer; they do not accept phase markers");
			// Enter the timer leg on the entry tick. A two-tick counter keeps
			// the channel advancing when the pump decrements it, while the
			// switch handler itself refreshes that counter each tick.
			// This is an authoring preset over the existing handler, not a
			// replacement timing rule. The actual duration is in the trace.
			row.delaystart = 0;
			row.delayend = 2;
		}
		if (role == wa::kFire) fire_row = int(out.rows.size());
		out.rows.push_back(row);
	}
	if (fire_row < 0) return fail("assign a Fire action before previewing weapon timing");
	if (!roles.count(wa::kRecoil)) {
		WeaponFsmActionRow row;
		std::strcpy(row.name, kWeaponActionSuffixes[wa::kRecoil]);
		row.delaystart = row.delayend = 0;
		out.rows.push_back(row);
	}
	out.rows[fire_row].delayend = 0;
	WeaponTimingPlan probe;
	if (!measure(out.rows, request.mode, probe) || probe.cycle_ticks <= 0)
		return fail("these action windows do not complete a firing cycle in the weapon FSM");
	const int wanted = int(std::lround(request.cycle_seconds * io::kTickHz));
	if (wanted < probe.cycle_ticks)
		return fail("the Shot / Recoil markers require at least " + std::to_string(probe.cycle_ticks) +
		            " ticks per shot; move them earlier or lower the requested RPM");
	// Solve against the runtime, including semi's Idle edge and burst's
	// continuation. No front end keeps a second copy of the state machine.
	int lo = 0, hi = wanted;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		out.rows[fire_row].delayend = mid;
		if (!measure(out.rows, request.mode, probe)) return fail("the firing cycle stalls");
		if (probe.cycle_ticks < wanted) lo = mid + 1;
		else hi = mid;
	}
	out.rows[fire_row].delayend = lo;
	if (!measure(out.rows, request.mode, probe)) return fail("the firing cycle stalls");
	out.cycle_ticks = probe.cycle_ticks;
	out.ready_tick = probe.ready_tick;
	out.events = probe.events;
	// Also preview each authored non-firing action independently. Its tick
	// zero is its own entry, identified by its action field in the event.
	for (const auto &row : out.rows) {
		const int role = role_id(row.name);
		if (role == wa::kFire || role == wa::kRecoil || role == wa::kIdle || role == wa::kEmptyIdle) continue;
		measure(out.rows, request.mode, probe, role);
		for (const auto &event : probe.events)
			if (out.events.size() < 256) out.events.push_back(event);
	}
	return true;
}

int cmd_weapon_timing(const char *input, const char *output) {
	std::ifstream file(input);
	if (!file) {
		std::fprintf(stderr, "cannot read weapon timing input\n");
		return 1;
	}
	WeaponTimingRequest request;
	std::string raw, error;
	int line = 0;
	bool header = false, mode = false, cycle = false;
	while (std::getline(file, raw)) {
		++line;
		SceneLine in(strip_comment(raw), SceneNumbers::finite);
		if (in.tokens.empty()) continue;
		bool ok = in.bad.empty();
		if (!header) {
			long long version = 0;
			ok = ok && in.key() == "weapon_timing" && in.integer(version, 1, 1);
			header = ok;
		} else if (in.key() == "mode") {
			ok = ok && !mode && in.name(request.mode);
			mode = true;
		} else if (in.key() == "cycle") {
			ok = ok && !cycle && in.number(request.cycle_seconds);
			cycle = true;
		} else if (in.key() == "action") {
			WeaponTimingAction action;
			std::string role, anim, sound, end_sound, particle, point;
			ok = ok && in.name(role) && in.name(anim) && in.number(action.active_seconds) &&
			     in.number(action.recovery_seconds) && in.name(sound) && in.name(end_sound) && in.name(particle) &&
			     in.name(point);
			ok = ok && identifier(role, 64) && identifier(anim, 64, true) && identifier(sound, 64, true) &&
			     identifier(end_sound, 64, true) && identifier(particle, 64, true) && identifier(point, 64, true);
			if (ok) {
				std::strcpy(action.row.name, role.c_str());
				std::strcpy(action.row.anim, anim.c_str());
				std::strcpy(action.row.soundset, sound.c_str());
				std::strcpy(action.row.soundsetend, end_sound.c_str());
				std::strcpy(action.row.particle, particle.c_str());
				std::strcpy(action.row.particleuserpoint, point.c_str());
				request.actions.push_back(action);
			}
		} else {
			ok = false;
		}
		if (!ok || in.more()) {
			std::fprintf(stderr, "weapon timing line %d: malformed or unknown record\n", line);
			return 1;
		}
	}
	WeaponTimingPlan plan;
	if (!header || !mode || !cycle || !plan_weapon_timing(request, plan, error)) {
		std::fprintf(stderr, "weapon timing: %s\n", error.empty() ? "missing header, mode or cycle" : error.c_str());
		return 1;
	}
	const auto text = snippet(request, plan);
	if (!write_output(output, text.data(), text.size())) return 1;
	std::puts(json(request, plan).c_str());
	return 0;
}

} // namespace threedi_cli
