// Asset-gated mission-script report: run the shipped mission headless and
// report which authored BMS events fire, which never do, and what the
// unfired ones would have commanded.
//
// This is the "does our BMS/WAC execution match the authored mission"
// instrument. It is diagnostic (never fails on unfired events — plenty of a
// mission's script is legitimately conditional on players doing things), but
// it makes a whole class of silent divergence visible: a convoy that never
// receives its route order, a trigger family we evaluate as permanently
// false, an action type we drop on the floor.
//
// Gated on OPENNOVA_JO_DIR; mission selectable with OPENNOVA_BMS (default
// 05TRcoop.bms), tick budget with OPENNOVA_TICKS (default 40000 ≈ 10 min).
#include "mission/bms.h"
#include "mission/event_runtime.h"
#include "mission/promote.h"

#include "world/ai.h"
#include "world/world.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

const char *action_name(bms::ActionType t) {
	switch (t) {
		case bms::ActionType::Null: return "Null";
		case bms::ActionType::RedirectGroupTo: return "RedirectGroupTo";
		case bms::ActionType::KillGroup: return "KillGroup";
		case bms::ActionType::ChangeGroupAI: return "ChangeGroupAI";
		case bms::ActionType::VaporizeGroup: return "VaporizeGroup";
		case bms::ActionType::MisvarChange: return "MisvarChange";
		case bms::ActionType::OutputText: return "OutputText";
		case bms::ActionType::PlayWavList: return "PlayWavList";
		case bms::ActionType::BlueWin: return "BlueWin";
		case bms::ActionType::RedWin: return "RedWin";
		case bms::ActionType::GreenWin: return "GreenWin";
		case bms::ActionType::GroupVelocity: return "GroupVelocity";
		case bms::ActionType::AreaAiRed: return "AreaAiRed";
		case bms::ActionType::AreaAiBlue: return "AreaAiBlue";
		case bms::ActionType::SubGoalWon: return "SubGoalWon";
		case bms::ActionType::SubGoalLost: return "SubGoalLost";
		default: return "other";
	}
}

} // namespace

int main() {
	const char *dir = std::getenv("OPENNOVA_JO_DIR");
	if (dir == nullptr || *dir == '\0') {
		std::printf("mission script report: SKIP (OPENNOVA_JO_DIR not set)\n");
		return 0;
	}
	const char *bms_name = std::getenv("OPENNOVA_BMS");
	const std::string mission_file = bms_name && *bms_name ? bms_name : "05TRcoop.bms";
	const char *tick_env = std::getenv("OPENNOVA_TICKS");
	const int ticks = (tick_env && *tick_env) ? std::atoi(tick_env) : 40000;

	const std::string path = std::string(dir) + "/" + mission_file;
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::printf("mission script report: SKIP (no %s)\n", mission_file.c_str());
		return 0;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                           std::istreambuf_iterator<char>());
	bms::File m;
	std::string error;
	if (!bms::parse(bytes.data(), bytes.size(), m, error)) {
		std::fprintf(stderr, "FAIL: %s parse: %s\n", mission_file.c_str(), error.c_str());
		return 1;
	}

	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	mission::BmsEventSystem events;
	events.load(m.events, m.triggers, m.actions);
	mission::promote_mission(m, world, ai, {});
	world.add_system(&events);
	world.add_system(&ai);
	world.load_systems();

	for (int t = 0; t < ticks; ++t) world.run_logic_tick(/*is_authority=*/true);

	// Zone health. A within-area trigger whose zone id resolves to nothing (or
	// to a degenerate box) is NEUTERED at load and can never fire again - retail
	// does the same, so a neutered trigger is only correct if RETAIL would also
	// have found nothing. If the mission authors zones our promote drops, we
	// silently disable script the original runs, and the events gated behind
	// them (the group redirects that start fights) never arrive. Report the
	// authored-vs-registered counts so that stays visible rather than showing up
	// as an unexplained "main_type 0".
	{
		int registered = 0;
		while (world.registry.area(registered) != nullptr) ++registered;
		int degenerate = 0;
		for (int i = 0; i < registered; ++i) {
			const w::Area *a = world.registry.area(i);
			if (a->bounds.min.x == a->bounds.max.x || a->bounds.min.y == a->bounds.max.y)
				++degenerate;
		}
		std::printf("zones: %zu authored in the BMS, %d registered, %d of those degenerate\n",
		            m.area_triggers.size(), registered, degenerate);
		if (m.area_triggers.size() != static_cast<size_t>(registered))
			std::printf("   <== promote DROPPED %zu authored zone(s)\n",
			            m.area_triggers.size() - static_cast<size_t>(registered));
		// Separate authored main_type-0 padding from triggers WE neutered: the
		// two are indistinguishable after load, and only the second is a bug.
		int authored_zero = 0;
		for (const bms::Trigger &t : m.triggers)
			if (static_cast<int32_t>(t.main_type) == 0) ++authored_zero;
		int live_zero = 0;
		for (const mission::ScriptedEvent &se : events.events())
			for (const bms::Trigger &t : se.triggers)
				if (static_cast<int32_t>(t.main_type) == 0) ++live_zero;
		std::printf("type-0 triggers: %d authored as padding, %d live after load\n",
		            authored_zero, live_zero);
		// A neutered trigger is only OUR bug if the zone it wants actually
		// exists in the file. Zones the mission never authored are dangling
		// authoring references, and retail neuters those too - so split the
		// count that way rather than reporting every neuter as a defect.
		if (live_zero > authored_zero) {
			std::map<int32_t, int> want;
			for (const bms::Trigger &t : m.triggers) {
				const int32_t mt = static_cast<int32_t>(t.main_type);
				if ((mt == 1 || mt == 2) && t.sub_type == 10) want[t.param2] += 1;
				else if (mt == 7 && t.sub_type == 37) want[t.param1] += 1;
			}
			int dangling_refs = 0, resolvable_but_neutered = 0;
			std::string dangling_ids;
			for (const auto &kv : want) {
				bool present = false;
				for (const bms::AreaTrigger &at : m.area_triggers)
					if (at.id == kv.first) { present = true; break; }
				if (present) continue;
				dangling_refs += kv.second;
				dangling_ids += " " + std::to_string(kv.first);
			}
			resolvable_but_neutered = (live_zero - authored_zero) - dangling_refs;
			std::printf("   %d neutered because the mission never authored zone(s)%s"
			            " - retail neuters these too, not a defect\n",
			            dangling_refs, dangling_ids.c_str());
			if (resolvable_but_neutered > 0)
				std::printf("   <== %d neutered despite a LIVE zone - that IS our bug\n",
				            resolvable_but_neutered);
		}
	}

	const std::vector<mission::ScriptedEvent> &evs = events.events();
	size_t fired = 0;
	std::map<std::string, int> unfired_actions;
	std::map<int32_t, int> unfired_triggers;
	std::map<int32_t, int> fired_triggers;
	std::printf("mission script report: %s, %d ticks (%.0f s of mission time)\n",
	            mission_file.c_str(), ticks, ticks / 62.0);
	std::printf("authored events: %zu\n", evs.size());
	for (size_t i = 0; i < evs.size(); ++i) {
		if (events.event_fired(i)) {
			++fired;
			for (const bms::Trigger &t : evs[i].triggers)
				fired_triggers[static_cast<int32_t>(t.main_type)] += 1;
			continue;
		}
		// What did this event want to do, and what is it waiting on? Reporting
		// the trigger MAIN TYPE separates "waits for a player" (legitimately
		// idle in a headless run) from families we may simply evaluate as
		// permanently false.
		for (const bms::Action &a : evs[i].actions)
			unfired_actions[action_name(a.action_type)] += 1;
		for (const bms::Trigger &t : evs[i].triggers)
			unfired_triggers[static_cast<int32_t>(t.main_type)] += 1;
	}
	std::printf("fired: %zu   never fired: %zu\n", fired, evs.size() - fired);
	if (!unfired_actions.empty()) {
		std::printf("actions the unfired events would have run:\n");
		for (const auto &kv : unfired_actions)
			std::printf("   %-20s x%d\n", kv.first.c_str(), kv.second);
	}
	// Which trigger FAMILIES ever produced a fire? A family that appears only
	// in never-fired events is the signature of an unimplemented or
	// always-false evaluator — as opposed to a family that fires elsewhere and
	// is simply waiting on a player here.
	std::printf("trigger MAIN TYPES — fired events vs never-fired events:\n");
	std::map<int32_t, int> all_types;
	for (const auto &kv : fired_triggers) all_types[kv.first] = 0;
	for (const auto &kv : unfired_triggers) all_types[kv.first] = 0;
	for (const auto &kv : all_types) {
		const int f = fired_triggers.count(kv.first) ? fired_triggers.at(kv.first) : 0;
		const int u = unfired_triggers.count(kv.first) ? unfired_triggers.at(kv.first) : 0;
		std::printf("   main_type %3d: %4d in fired, %4d in never-fired%s\n",
		            kv.first, f, u,
		            (f == 0 && u > 0) ? "   <== never produced a fire" : "");
	}
	// The events that would start AI-vs-AI fights, and what is holding each one
	// shut. 05TRcoop opens with its two sides ~857u apart, so nothing shoots
	// until the script redirects a group - which makes "why has this not fired"
	// the difference between a broken evaluator and a mission waiting on a
	// player. Zone triggers print the zone's centre so a test round can put the
	// player there deliberately.
	std::printf("gates on the AI-vs-AI redirects (never-fired events only):\n");
	int shown = 0;
	for (size_t i = 0; i < evs.size() && shown < 12; ++i) {
		if (events.event_fired(i)) continue;
		bool interesting = false;
		for (const bms::Action &a : evs[i].actions)
			if (a.action_type == bms::ActionType::RedirectGroupTo ||
			    a.action_type == bms::ActionType::ChangeGroupAI) interesting = true;
		if (!interesting) continue;
		++shown;
		std::printf("  event %zu:", i);
		for (const bms::Trigger &t : evs[i].triggers) {
			const int32_t mt = static_cast<int32_t>(t.main_type);
			std::printf(" [main %d sub %d p(%d,%d,%d)", mt, t.sub_type,
			            t.param1, t.param2, t.param3);
			// param2 was rewritten to the area INDEX by resolve_zone_refs.
			if ((mt == 1 || mt == 2) && t.sub_type == 10) {
				if (const w::Area *a = world.registry.area(t.param2))
					std::printf(" zone id %d centre (%.0f, %.0f)", a->zone_id,
					            0.5 * (a->bounds.min.x + a->bounds.max.x),
					            0.5 * (a->bounds.min.y + a->bounds.max.y));
			}
			std::printf("]");
		}
		std::printf("\n");
	}
	std::printf("mission script report: done (diagnostic only)\n");
	return 0;
}
