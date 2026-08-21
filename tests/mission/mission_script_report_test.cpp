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

#include <algorithm>
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

	// Record the tick each event FIRST fires. "Which events fire" was not enough
	// to compare against retail: 05TRcoop's ten scripted kills all land inside
	// the first second, while retail's capture carries its twelve partway through
	// the session, and only a timestamp distinguishes "fires unconditionally at
	// boot" from "fires when the player gets there".
	std::vector<int> first_fire(events.events().size(), -1);
	std::vector<int> death_tick;
	size_t seen_deaths = 0;
	for (int t = 0; t < ticks; ++t) {
		world.run_logic_tick(/*is_authority=*/true);
		for (size_t i = 0; i < first_fire.size(); ++i)
			if (first_fire[i] < 0 && events.event_fired(i)) first_fire[i] = t;
		// Nothing drains the death list headless, so its growth timestamps the
		// kills themselves rather than merely the events that ordered them.
		while (seen_deaths < world.round_sim.deaths.size()) {
			death_tick.push_back(t);
			++seen_deaths;
		}
	}

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
	// The scripted kills: when they land, and what let them through. Retail's
	// baseline shows twelve of these partway through a session; if ours all fire
	// at boot with no player, their triggers are passing when they should not.
	{
		// Who is in group 1? Players carry commandGroup 1, so a group-1 area
		// trigger is meant to mean "a player got here". If AUTHORED AI also sit
		// in group 1 the trigger fires with no player at all, which is what a
		// boot-time scripted kill would look like.
		{
			std::vector<w::EntityHandle> g1;
			world.registry.by_group(1, g1);
			std::printf("group 1 membership: %zu entit(ies)", g1.size());
			int shown_g = 0;
			for (w::EntityHandle h : g1) {
				const w::Entity *e = world.registry.get(h);
				if (e == nullptr || shown_g >= 6) continue;
				++shown_g;
				std::printf("\n    net_id %d team %d alive %d pos (%.0f, %.0f)",
				            e->net_id, e->team, e->alive ? 1 : 0, e->position.x,
				            e->position.y);
				// Retail skips a pool-0 entity with Flags & 1 - carried by a
				// vehicle, or standing on something destroyed. We do not model
				// bit 0x1 at all. [orig: Entity_IsTeamInTriggerBounds @0x43c730
				//  `(entity[36] & 1) == 0`; set by Entity_AttachToVehicle]
				if ((e->flags & 1u) != 0) std::printf("  [Flags&1 - retail skips]");
			}
			std::printf("\n");
		}
		// Does this mission offer deploy-selectable spawn zones? If it does, retail
		// marks EVERY joining player pending (Flags 0x1) until they pick one, which
		// keeps them out of the humans count and so holds the whole script. If it
		// does not, retail's host counts itself immediately and its script runs at
		// boot exactly like ours - and the boot-time kills are parity, not a gap.
		{
			int zones = 0;
			world.registry.for_each([&](const w::Entity &e) {
				const int pool = e.handle.pool();
				if ((pool == 1 || pool == 2) && e.is_spawn_point && e.alive) ++zones;
			});
			const char *note =
					zones == 0
							? "  (no deploy hold: retail's host counts itself from mission start too)"
							: "  (retail holds every player pending until a pick)";
			std::printf("deploy-selectable spawn zones: %d%s\n", zones, note);
		}
		std::printf("scripted kills: %zu death(s) raised", death_tick.size());
		if (!death_tick.empty())
			std::printf(", ticks %d..%d (%.1f s..%.1f s)", death_tick.front(),
			            death_tick.back(), death_tick.front() / 62.0,
			            death_tick.back() / 62.0);
		std::printf("\n");
		for (size_t i = 0; i < evs.size(); ++i) {
			bool kills = false;
			for (const bms::Action &a : evs[i].actions)
				if (a.action_type == bms::ActionType::KillGroup) kills = true;
			if (!kills || first_fire[i] < 0) continue;
			std::printf("  event %zu fired at tick %d (%.1f s):", i, first_fire[i],
			            first_fire[i] / 62.0);
			if (evs[i].triggers.empty()) std::printf("  NO TRIGGERS (fires unconditionally)");
			for (const bms::Trigger &t : evs[i].triggers)
				std::printf(" [main %d sub %d p(%d,%d,%d) cond 0x%x%s]",
				            static_cast<int32_t>(t.main_type), t.sub_type,
				            t.param1, t.param2, t.param3, t.condition_flags,
				            (t.condition_flags & bms::Trigger::kConditionNegated) ? " NEGATED" : "");
			std::printf("\n");
		}
	}

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
	// ZONE SWEEP, then a TOUR. Most of what stays shut in a headless run is
	// waiting on a player standing somewhere, and guessing which zone costs a
	// three-minute live round per guess. Do it offline instead: run the mission
	// with a stand-in group-1 body visiting zone centres and report what extra
	// script that unlocks. Players carry commandGroup 1 (player_spawn.cpp), and
	// retail's Entity_IsTeamInTriggerBounds @0x43c730 scans the player pool as
	// well as the AI pool, so a pool-1 body with group_id 1 is exactly what the
	// group-in-area triggers look for.
	//
	// A single-zone sweep only ever finds the mission's FIRST gate, because a
	// linear mission opens each phase with the previous one. So the tour moves
	// the body from zone to zone, and a greedy search picks each next hop by how
	// much it unlocks - walking the mission forward the way a player would.
	{
		const char *sweep_env = std::getenv("OPENNOVA_ZONE_SWEEP");
		const int hop_ticks = (sweep_env && *sweep_env) ? std::atoi(sweep_env) : 4000;
		int registered = 0;
		while (world.registry.area(registered) != nullptr) ++registered;
		std::vector<std::pair<float, float>> centre(registered);
		for (int i = 0; i < registered; ++i) {
			const w::Area *a = world.registry.area(i);
			centre[i] = {0.5f * (a->bounds.min.x + a->bounds.max.x),
			             0.5f * (a->bounds.min.y + a->bounds.max.y)};
		}
		auto run_tour = [&](const std::vector<int> &stops, std::map<std::string, int> *acts) {
			w::World w2;
			w::AiSystem ai2;
			w2.ai = &ai2;
			mission::BmsEventSystem ev2;
			ev2.load(m.events, m.triggers, m.actions);
			mission::promote_mission(m, w2, ai2, {});
			w::EntityHandle body{};
			if (!stops.empty()) {
				w2.registry.configure_pool(1, 4);
				w::Entity p{};
				p.kind = w::EntityKind::Organic;
				p.team = 1;
				p.health = 100;
				p.alive = true;
				p.group_id = 1;
				p.net_id = 0x7000;
				p.flags = 0x100u; // player classifier, movement gate CLEAR
				p.position = w::Vec3{centre[stops[0]].first, centre[stops[0]].second, 0.0f};
				body = w2.registry.spawn(1, p);
			}
			w2.add_system(&ev2);
			w2.add_system(&ai2);
			w2.load_systems();
			const size_t hops = stops.empty() ? 1u : stops.size();
			for (size_t h = 0; h < hops; ++h) {
				if (!stops.empty()) {
					if (w::Entity *e = w2.registry.get(body)) {
						e->position.x = centre[stops[h]].first;
						e->position.y = centre[stops[h]].second;
					}
				}
				for (int t = 0; t < hop_ticks; ++t) w2.run_logic_tick(true);
			}
			int n = 0;
			for (size_t i = 0; i < ev2.events().size(); ++i) {
				if (!ev2.event_fired(i)) continue;
				++n;
				if (acts != nullptr)
					for (const bms::Action &a : ev2.events()[i].actions)
						(*acts)[action_name(a.action_type)] += 1;
			}
			// An event running a KillGroup does not mean anyone DIED - the group may
			// be empty or already dead. Deaths accumulate here because nothing drains
			// them headless (the host's route_round_deaths is what clears the list),
			// so the final size is the run's death total. Retail's baseline capture
			// shows twelve, so this is the number to compare against.
			if (acts != nullptr)
				(*acts)["<deaths raised>"] = static_cast<int>(w2.round_sim.deaths.size());
			return n;
		};
		std::map<std::string, int> base_acts;
		const int base = run_tour({}, &base_acts);
		std::printf("zone tour (%d ticks per stop): baseline with no player = %d events,"
		            " deaths raised %d\n",
		            hop_ticks, base, base_acts["<deaths raised>"]);
		std::vector<int> tour;
		int have = base;
		for (int step = 0; step < 8; ++step) {
			int best = -1, best_n = have;
			for (int z = 0; z < registered; ++z) {
				if (std::find(tour.begin(), tour.end(), z) != tour.end()) continue;
				std::vector<int> cand = tour;
				cand.push_back(z);
				const int n = run_tour(cand, nullptr);
				if (n > best_n) { best_n = n; best = z; }
			}
			if (best < 0) {
				std::printf("  no further zone unlocks anything - tour ends at %d events\n", have);
				break;
			}
			tour.push_back(best);
			have = best_n;
			std::map<std::string, int> acts;
			run_tour(tour, &acts);
			std::printf("  stop %d: zone id %2d centre (%6.0f, %6.0f) -> %d events",
			            step + 1, world.registry.area(best)->zone_id,
			            centre[best].first, centre[best].second, have);
			for (const char *k : {"RedirectGroupTo", "ChangeGroupAI", "KillGroup", "<deaths raised>"})
				if (acts.count(k)) std::printf("  %s x%d", k, acts[k]);
			std::printf("\n");
		}
		if (!tour.empty()) {
			std::printf("  tour route (mission coords):");
			for (int z : tour) std::printf(" (%.0f,%.0f)", centre[z].first, centre[z].second);
			std::printf("\n");
		}
	}

	std::printf("mission script report: done (diagnostic only)\n");
	return 0;
}
