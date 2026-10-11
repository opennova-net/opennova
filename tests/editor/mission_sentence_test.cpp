// A mission's logic in words (editor/documents/mission_sentence.h, ADR 0046 S15, Events and scripts):
// each trigger and action worded from its type and its typed parameters, the names of what they name
// from a MissionNames, the negation flipping a trigger's verb, the chain's flat left-to-right fold of the
// joins held together where a join changes, an event's flags, delay and repeat in its sentence, a type
// no row names and a value naming nothing said so; over the minted synth_logic mission, the document's
// titles (an event its sentence, a trigger its words after its join, an action its words) and its names
// (an entity by its kind and SSN, a zone by its record, an SSN or a zone no record holds said so); the
// words on the wire (the `record` query's title) and in Problems (a row's `record_title`); the
// types "Add trigger" and "Add action" offer, each with its title, group and line, every row of
// mission_params' triggers and actions among them. With the game install (a SKIP-LEG without
// OPENNOVA_JO_DIR), every event of every shipped mission worded, none of a type no row names, timed. S24: the
// words as runs (a parameter's words its slot's run, the type's own words the type's), joined the words, for the
// hand-worded types too and for every shipped record.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_sentence.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_chains.h>
#include <formats/mission/mission_field.h>
#include <formats/mission/mission_params.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// Two words alike, both printed where they differ.
bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

bms::Trigger trigger(bms::TriggerMainType main, int32_t sub, int32_t p1 = 0, int32_t p2 = 0, int32_t p3 = 0,
                     int32_t flags = 0) {
	bms::Trigger out{};
	out.condition_flags = flags;
	out.main_type = main;
	out.sub_type = sub;
	out.param1 = p1;
	out.param2 = p2;
	out.param3 = p3;
	return out;
}

bms::Action action(bms::ActionType type, int32_t sub = 0, int32_t p1 = 0, int32_t p2 = 0, int32_t p3 = 0) {
	bms::Action out{};
	out.action_type = type;
	out.action_sub_type = sub;
	out.param1 = p1;
	out.param2 = p2;
	out.param3 = p3;
	return out;
}

// The project's names over a few values, and a string table of one key.
class Names : public MissionNames {
public:
	std::string entity(int64_t ssn) const override {
		return ssn == 10034 ? "Hostage #10034" : ssn == 2200 ? "Truck #2200" : MissionNames::entity(ssn);
	}
	std::string zone(int64_t id) const override { return id == 3 ? "Zone 3 (bridge)" : MissionNames::zone(id); }
	std::string text(const std::string &section, const std::string &key) const override {
		if (section == "Triggered Text" && key == "ID004") return "Mission failed";
		if (section == "WinConditions" && key == "STRWINCOND007") return "Rescue the hostage";
		if (section == "WinConditions" && key == "STRWINMSG007") return "Hostage rescued";
		return std::string();
	}
};

// Names that know no area: every zone id fails to resolve.
class DocumentlessZones : public MissionNames {
public:
	bool zone_resolves(int64_t) const override { return false; }
};

// The words of every type over one record of it each (a trigger also negated), as the board's runs join them: a
// golden table, so the runs (S24 A2: the words built as runs, a parameter's run its slot's) cannot drift from the
// words the S15 sentences said. Made from the build whose runs matched the S15 words byte for byte over every
// trigger and action of the shipped missions; a change of wording is a change of this table.
struct GoldenWords {
	bool action;
	int32_t type, sub, flags;
	const char *words;
};
const GoldenWords kGoldenWords[] = {
	{false, 1, 4, 0, "group 10034 has no unit left alive"},
	{false, 1, 4, 1, "group 10034 still has a unit alive"},
	{false, 1, 5, 0, "group 10034 still has a unit alive"},
	{false, 1, 5, 1, "group 10034 has no unit left alive"},
	{false, 1, 6, 0, "group 10034 has lost 3 units or more"},
	{false, 1, 6, 1, "group 10034 has not lost 3 units or more"},
	{false, 1, 9, 0, "group 10034 has lost no unit"},
	{false, 1, 9, 1, "group 10034 has lost a unit"},
	{false, 1, 12, 0, "group 10034 has 3 units or more alive"},
	{false, 1, 12, 1, "group 10034 has fewer than 3 units alive"},
	{false, 2, 4, 0, "Hostage #10034 is destroyed"},
	{false, 2, 4, 1, "Hostage #10034 is not destroyed"},
	{false, 2, 5, 0, "Hostage #10034 is alive"},
	{false, 2, 5, 1, "Hostage #10034 is not alive"},
	{false, 2, 6, 0, "Hostage #10034 has lost 3 hit points or more"},
	{false, 2, 6, 1, "Hostage #10034 has not lost 3 hit points or more"},
	{false, 2, 9, 0, "Hostage #10034 has full health"},
	{false, 2, 9, 1, "Hostage #10034 does not have full health"},
	{false, 2, 12, 0, "Hostage #10034 has 3 hit points or more"},
	{false, 2, 12, 1, "Hostage #10034 has fewer than 3 hit points"},
	{false, 1, 1, 0, "group 10034 has seen group 3"},
	{false, 1, 1, 1, "group 10034 has not seen group 3"},
	{false, 1, 2, 0, "group 10034 has targeted group 3"},
	{false, 1, 2, 1, "group 10034 has not targeted group 3"},
	{false, 1, 13, 0, "group 10034 has shot group 3"},
	{false, 1, 13, 1, "group 10034 has not shot group 3"},
	{false, 1, 16, 0, "group 10034 has seen SSN 3"},
	{false, 1, 16, 1, "group 10034 has not seen SSN 3"},
	{false, 1, 15, 0, "group 10034 has targeted SSN 3"},
	{false, 1, 15, 1, "group 10034 has not targeted SSN 3"},
	{false, 1, 17, 0, "group 10034 has shot SSN 3"},
	{false, 1, 17, 1, "group 10034 has not shot SSN 3"},
	{false, 2, 16, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has seen SSN 3"},
	{false, 2, 16, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not seen SSN 3"},
	{false, 2, 15, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has targeted SSN 3"},
	{false, 2, 15, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not targeted SSN 3"},
	{false, 2, 17, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has shot SSN 3"},
	{false, 2, 17, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not shot SSN 3"},
	{false, 2, 1, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has seen group 3"},
	{false, 2, 1, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not seen group 3"},
	{false, 2, 2, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has targeted group 3"},
	{false, 2, 2, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not targeted group 3"},
	{false, 2, 13, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has shot group 3"},
	{false, 2, 13, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not shot group 3"},
	{false, 2, 44, 0, "Hostage #10034 has a clear line of sight to SSN 3 within 2 m"},
	{false, 2, 44, 1, "Hostage #10034 has no clear line of sight to SSN 3 within 2 m"},
	{false, 2, 45, 0, "Hostage #10034 sees SSN 3 within 2 m"},
	{false, 2, 45, 1, "Hostage #10034 does not see SSN 3 within 2 m"},
	{false, 1, 3, 0, "group 10034 is at red alert"},
	{false, 1, 3, 1, "group 10034 is not at red alert"},
	{false, 1, 14, 0, "group 10034 is at yellow alert"},
	{false, 1, 14, 1, "group 10034 is not at yellow alert"},
	{false, 2, 3, 0, "Hostage #10034 is at red alert"},
	{false, 2, 3, 1, "Hostage #10034 is not at red alert"},
	{false, 2, 14, 0, "Hostage #10034 is at yellow alert"},
	{false, 2, 14, 1, "Hostage #10034 is not at yellow alert"},
	{false, 1, 10, 0, "group 10034 has a unit in Zone 3 (bridge)"},
	{false, 1, 10, 1, "group 10034 has no unit in Zone 3 (bridge)"},
	{false, 1, 7, 0, "group 10034 has passed stop 2 of path 3"},
	{false, 1, 7, 1, "group 10034 has not passed stop 2 of path 3"},
	{false, 2, 10, 0, "Hostage #10034 is in Zone 3 (bridge)"},
	{false, 2, 10, 1, "Hostage #10034 is not in Zone 3 (bridge)"},
	{false, 2, 43, 0, "Hostage #10034 is within 2 m of SSN 3"},
	{false, 2, 43, 1, "Hostage #10034 is not within 2 m of SSN 3"},
	{false, 2, 7, 0, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has passed stop 2 of path 3"},
	{false, 2, 7, 1, "Hostage #10034 (never recorded: the game keeps SSNs below 128) has not passed stop 2 of path 3"},
	{false, 7, 37, 0, "a satchel charge lies in zone 10034"},
	{false, 7, 37, 1, "a satchel charge does not lie in zone 10034"},
	{false, 7, 36, 0, "the player has been outside the mission area for 10274.8 s"},
	{false, 7, 36, 1, "the player has not been outside the mission area for 10274.8 s"},
	{false, 1, 11, 0, "a soldier of group 10034 carries an object of group 3"},
	{false, 1, 11, 1, "a soldier of group 10034 does not carry an object of group 3"},
	{false, 2, 42, 0, "Hostage #10034 is on top of SSN 3"},
	{false, 2, 42, 1, "Hostage #10034 is not on top of SSN 3"},
	{false, 2, 11, 0, "Hostage #10034 carries an object of group 3"},
	{false, 2, 11, 1, "Hostage #10034 does not carry an object of group 3"},
	{false, 7, 39, 0, "the player is on Hostage #10034"},
	{false, 7, 39, 1, "the player is not on Hostage #10034"},
	{false, 7, 40, 0, "the player drives Hostage #10034"},
	{false, 7, 40, 1, "the player does not drive Hostage #10034"},
	{false, 7, 41, 0, "the player is on Hostage #10034's gun"},
	{false, 7, 41, 1, "the player is not on Hostage #10034's gun"},
	{false, 7, 38, 0, "the player is attached to Hostage #10034"},
	{false, 7, 38, 1, "the player is not attached to Hostage #10034"},
	{false, 3, 0, 0, "event 10035 has fired"},
	{false, 3, 0, 1, "event 10035 has not fired"},
	{false, 4, 1, 0, "variable 10034 equals 3"},
	{false, 4, 1, 1, "variable 10034 does not equal 3"},
	{false, 4, 2, 0, "variable 10034 is less than 3"},
	{false, 4, 2, 1, "variable 10034 is not less than 3"},
	{false, 4, 3, 0, "variable 10034 is greater than 3"},
	{false, 4, 3, 1, "variable 10034 is not greater than 3"},
	{false, 4, 4, 0, "variable 10034 is at most 3"},
	{false, 4, 4, 1, "variable 10034 is not at most 3"},
	{false, 4, 5, 0, "variable 10034 is at least 3"},
	{false, 4, 5, 1, "variable 10034 is not at least 3"},
	{false, 5, 0, 0, "it is the second time through"},
	{false, 5, 0, 1, "it is not the second time through"},
	{false, 6, 1, 0, "teammates are on"},
	{false, 6, 1, 1, "teammates are not on"},
	{false, 6, 2, 0, "a helicopter lift is under way"},
	{false, 6, 2, 1, "a helicopter lift is not under way"},
	{false, 6, 3, 0, "a helicopter lift is under way"},
	{false, 6, 3, 1, "a helicopter lift is not under way"},
	{false, 7, 34, 0, "dialog 10034 is not playing"},
	{false, 7, 34, 1, "dialog 10034 is playing"},
	{false, 7, 35, 0, "dialog 10034 has played and finished"},
	{false, 7, 35, 1, "dialog 10034 has not played to its end"},
	{false, 7, 18, 0, "the player is berserk"},
	{false, 7, 18, 1, "the player is not berserk (always true in the game: it reads the berserk bit as a raw 0x200)"},
	{false, 7, 19, 0, "the player has chosen the first-person view"},
	{false, 7, 19, 1, "the player has not chosen the first-person view"},
	{false, 7, 20, 0, "the player has chosen the third-person view"},
	{false, 7, 20, 1, "the player has not chosen the third-person view"},
	{false, 7, 21, 0, "the player has chosen the view with the gun"},
	{false, 7, 21, 1, "the player has not chosen the view with the gun"},
	{false, 7, 32, 0, "the player's input bit 10034 is set"},
	{false, 7, 32, 1, "the player's input bit 10034 is not set"},
	{false, 7, 33, 0, "the player's input bit 10034 + 15 is set"},
	{false, 7, 33, 1, "the player's input bit 10034 + 15 is not set"},
	{false, 7, 22, 0, "the player's input bit 10 is set (never, in the game)"},
	{false, 7, 22, 1, "the player's input bit 10 is not set (never, in the game)"},
	{false, 7, 23, 0, "the player's input bit 11 is set (never, in the game)"},
	{false, 7, 23, 1, "the player's input bit 11 is not set (never, in the game)"},
	{false, 7, 24, 0, "the player's input bit 12 is set (never, in the game)"},
	{false, 7, 24, 1, "the player's input bit 12 is not set (never, in the game)"},
	{false, 7, 25, 0, "the player's input bit 13 is set (never, in the game)"},
	{false, 7, 25, 1, "the player's input bit 13 is not set (never, in the game)"},
	{false, 7, 26, 0, "the look bit is clear (always, in the game)"},
	{false, 7, 26, 1, "the look bit is set (never, in the game)"},
	{false, 7, 27, 0, "the look bit is set (never, in the game)"},
	{false, 7, 27, 1, "the look bit is clear (always, in the game)"},
	{false, 7, 28, 0, "the player's input bit 29 is set (never, in the game)"},
	{false, 7, 28, 1, "the player's input bit 29 is not set (never, in the game)"},
	{false, 7, 29, 0, "the player's input bit 14 is set (never, in the game)"},
	{false, 7, 29, 1, "the player's input bit 14 is not set (never, in the game)"},
	{false, 7, 30, 0, "the player's input bit 15 is set (never, in the game)"},
	{false, 7, 30, 1, "the player's input bit 15 is not set (never, in the game)"},
	{true, 14, 0, 0, "win sub-goal 4"},
	{true, 15, 0, 0, "lose sub-goal 4"},
	{true, 35, 0, 0, "show win objective 4"},
	{true, 36, 0, 0, "show lose objective 4"},
	{true, 8, 0, 0, "end the round: the blue team (team 1) wins"},
	{true, 9, 0, 0, "end the round: the red team (team 2) wins"},
	{true, 10, 0, 0, "end the round: the green team (team 0) wins"},
	{true, 6, 0, 0, "show text 4: 'Mission failed'"},
	{true, 7, 0, 0, "play dialog 4"},
	{true, 2, 0, 0, "kill group 4"},
	{true, 20, 0, 0, "kill SSN 4"},
	{true, 4, 0, 0, "remove group 4 from the mission"},
	{true, 22, 0, 0, "remove SSN 4 from the mission"},
	{true, 1, 0, 0, "send group 4 along path 2200, from stop 3"},
	{true, 19, 0, 0, "send SSN 4 along path 2200, from stop 3"},
	{true, 18, 0, 0, "teleport group 4 to teleport target 2200"},
	{true, 26, 0, 0, "teleport SSN 4 to teleport target 2200"},
	{true, 32, 0, 0, "forget the waypoints group 4 has passed"},
	{true, 33, 0, 0, "forget the waypoints SSN 4 has passed"},
	{true, 37, 0, 0, "put SSN 4 into the vehicle it was ordered to board"},
	{true, 11, 0, 0, "set group 4's speed to 2200 km/h (the game never reads it)"},
	{true, 23, 0, 0, "set SSN 4's speed to 2200 km/h (does nothing in the game)"},
	{true, 3, 5, 0, "put group 4 on red alert"},
	{true, 3, 22, 0, "put group 4 on yellow alert"},
	{true, 3, 6, 0, "put group 4 on green alert"},
	{true, 3, 8, 0, "set group 4's accuracy to 2200"},
	{true, 3, 27, 0, "set group 4's aiming skill to 2200"},
	{true, 3, 26, 0, "set group 4's driving skill to 2200"},
	{true, 3, 29, 0, "set group 4's combat speed to 2200 km/h"},
	{true, 3, 30, 0, "set group 4's patrol speed to 2200 km/h"},
	{true, 3, 41, 0, "set group 4's attack distance to 2200 m"},
	{true, 3, 42, 0, "set group 4's engage distance to 2200 m to 3 m"},
	{true, 3, 46, 0, "set group 4's firing angle to 2200"},
	{true, 3, 44, 0, "make group 4 target Truck #2200"},
	{true, 3, 31, 0, "make group 4 find and use Truck #2200"},
	{true, 3, 45, 0, "turn group 4's firing on"},
	{true, 3, 2, 0, "turn group 4's guard behaviour on"},
	{true, 3, 15, 0, "turn group 4's blindness on"},
	{true, 3, 16, 0, "turn group 4's berserk behaviour on"},
	{true, 3, 17, 0, "turn group 4's climbing on"},
	{true, 3, 21, 0, "turn group 4's cowardice on"},
	{true, 3, 40, 0, "turn group 4's node-path following on"},
	{true, 3, 43, 0, "turn group 4's indestructibility on"},
	{true, 3, 28, 0, "set group 4's AI state to 2200"},
	{true, 3, 34, 0, "play part animation 2200 on group 4 (play type 3, time 0)"},
	{true, 3, 32, 0, "apply AIUSEWPZ to group 4 (what it does is unknown)"},
	{true, 3, 33, 0, "apply AICLEARWPZ to group 4 (what it does is unknown)"},
	{true, 3, 37, 0, "do nothing to group 4 (HUDITEM has no effect)"},
	{true, 3, 39, 0, "do nothing to group 4 (TMATESTATUS has no effect)"},
	{true, 21, 5, 0, "put SSN 4 on red alert"},
	{true, 21, 22, 0, "put SSN 4 on yellow alert"},
	{true, 21, 6, 0, "put SSN 4 on green alert"},
	{true, 21, 8, 0, "set SSN 4's accuracy to 2200"},
	{true, 21, 27, 0, "set SSN 4's aiming skill to 2200"},
	{true, 21, 26, 0, "set SSN 4's driving skill to 2200"},
	{true, 21, 29, 0, "set SSN 4's combat speed to 2200 km/h"},
	{true, 21, 30, 0, "set SSN 4's patrol speed to 2200 km/h"},
	{true, 21, 41, 0, "set SSN 4's attack distance to 2200 m"},
	{true, 21, 42, 0, "set SSN 4's engage distance to 2200 m to 3 m"},
	{true, 21, 46, 0, "set SSN 4's firing angle to 2200"},
	{true, 21, 44, 0, "make SSN 4 target Truck #2200"},
	{true, 21, 31, 0, "make SSN 4 find and use Truck #2200"},
	{true, 21, 45, 0, "turn SSN 4's firing on"},
	{true, 21, 2, 0, "turn SSN 4's guard behaviour on"},
	{true, 21, 15, 0, "turn SSN 4's blindness on"},
	{true, 21, 16, 0, "turn SSN 4's berserk behaviour on"},
	{true, 21, 17, 0, "turn SSN 4's climbing on"},
	{true, 21, 21, 0, "turn SSN 4's cowardice on"},
	{true, 21, 40, 0, "turn SSN 4's node-path following on"},
	{true, 21, 43, 0, "turn SSN 4's indestructibility on"},
	{true, 21, 28, 0, "set SSN 4's AI state to 2200"},
	{true, 21, 34, 0, "play part animation 2200 on SSN 4 (play type 3, time 0)"},
	{true, 21, 32, 0, "apply AIUSEWPZ to SSN 4 (what it does is unknown)"},
	{true, 21, 33, 0, "apply AICLEARWPZ to SSN 4 (what it does is unknown)"},
	{true, 21, 37, 0, "do nothing to SSN 4 (HUDITEM has no effect)"},
	{true, 21, 39, 0, "do nothing to SSN 4 (TMATESTATUS has no effect)"},
	{true, 12, 5, 0, "put the red team's soldiers in zone 4 on red alert"},
	{true, 12, 22, 0, "put the red team's soldiers in zone 4 on yellow alert"},
	{true, 12, 6, 0, "put the red team's soldiers in zone 4 on green alert"},
	{true, 12, 8, 0, "set the red team's soldiers in zone 4's accuracy to 2200"},
	{true, 12, 27, 0, "set the red team's soldiers in zone 4's aiming skill to 2200"},
	{true, 12, 26, 0, "set the red team's soldiers in zone 4's driving skill to 2200"},
	{true, 12, 29, 0, "set the red team's soldiers in zone 4's combat speed to 2200 km/h"},
	{true, 12, 30, 0, "set the red team's soldiers in zone 4's patrol speed to 2200 km/h"},
	{true, 12, 41, 0, "set the red team's soldiers in zone 4's attack distance to 2200 m"},
	{true, 12, 42, 0, "set the red team's soldiers in zone 4's engage distance to 2200 m to (a value of the area's box)"},
	{true, 12, 46, 0, "set the red team's soldiers in zone 4's firing angle to 2200"},
	{true, 12, 44, 0, "make the red team's soldiers in zone 4 target Truck #2200"},
	{true, 12, 31, 0, "make the red team's soldiers in zone 4 find and use Truck #2200"},
	{true, 12, 45, 0, "turn the red team's soldiers in zone 4's firing on"},
	{true, 12, 2, 0, "turn the red team's soldiers in zone 4's guard behaviour on"},
	{true, 12, 15, 0, "turn the red team's soldiers in zone 4's blindness on"},
	{true, 12, 16, 0, "turn the red team's soldiers in zone 4's berserk behaviour on"},
	{true, 12, 17, 0, "turn the red team's soldiers in zone 4's climbing on"},
	{true, 12, 21, 0, "turn the red team's soldiers in zone 4's cowardice on"},
	{true, 12, 40, 0, "turn the red team's soldiers in zone 4's node-path following on"},
	{true, 12, 43, 0, "turn the red team's soldiers in zone 4's indestructibility on"},
	{true, 12, 28, 0, "set the red team's soldiers in zone 4's AI state to 2200"},
	{true, 12, 34, 0, "play part animation 2200 on the red team's soldiers in zone 4 (play type (a value of the area's box), time (a value of the area's box))"},
	{true, 12, 32, 0, "apply AIUSEWPZ to the red team's soldiers in zone 4 (what it does is unknown)"},
	{true, 12, 33, 0, "apply AICLEARWPZ to the red team's soldiers in zone 4 (what it does is unknown)"},
	{true, 12, 37, 0, "do nothing to the red team's soldiers in zone 4 (HUDITEM has no effect)"},
	{true, 12, 39, 0, "do nothing to the red team's soldiers in zone 4 (TMATESTATUS has no effect)"},
	{true, 13, 5, 0, "put the blue team's soldiers in zone 4 on red alert"},
	{true, 13, 22, 0, "put the blue team's soldiers in zone 4 on yellow alert"},
	{true, 13, 6, 0, "put the blue team's soldiers in zone 4 on green alert"},
	{true, 13, 8, 0, "set the blue team's soldiers in zone 4's accuracy to 2200"},
	{true, 13, 27, 0, "set the blue team's soldiers in zone 4's aiming skill to 2200"},
	{true, 13, 26, 0, "set the blue team's soldiers in zone 4's driving skill to 2200"},
	{true, 13, 29, 0, "set the blue team's soldiers in zone 4's combat speed to 2200 km/h"},
	{true, 13, 30, 0, "set the blue team's soldiers in zone 4's patrol speed to 2200 km/h"},
	{true, 13, 41, 0, "set the blue team's soldiers in zone 4's attack distance to 2200 m"},
	{true, 13, 42, 0, "set the blue team's soldiers in zone 4's engage distance to 2200 m to (a value of the area's box)"},
	{true, 13, 46, 0, "set the blue team's soldiers in zone 4's firing angle to 2200"},
	{true, 13, 44, 0, "make the blue team's soldiers in zone 4 target Truck #2200"},
	{true, 13, 31, 0, "make the blue team's soldiers in zone 4 find and use Truck #2200"},
	{true, 13, 45, 0, "turn the blue team's soldiers in zone 4's firing on"},
	{true, 13, 2, 0, "turn the blue team's soldiers in zone 4's guard behaviour on"},
	{true, 13, 15, 0, "turn the blue team's soldiers in zone 4's blindness on"},
	{true, 13, 16, 0, "turn the blue team's soldiers in zone 4's berserk behaviour on"},
	{true, 13, 17, 0, "turn the blue team's soldiers in zone 4's climbing on"},
	{true, 13, 21, 0, "turn the blue team's soldiers in zone 4's cowardice on"},
	{true, 13, 40, 0, "turn the blue team's soldiers in zone 4's node-path following on"},
	{true, 13, 43, 0, "turn the blue team's soldiers in zone 4's indestructibility on"},
	{true, 13, 28, 0, "set the blue team's soldiers in zone 4's AI state to 2200"},
	{true, 13, 34, 0, "play part animation 2200 on the blue team's soldiers in zone 4 (play type (a value of the area's box), time (a value of the area's box))"},
	{true, 13, 32, 0, "apply AIUSEWPZ to the blue team's soldiers in zone 4 (what it does is unknown)"},
	{true, 13, 33, 0, "apply AICLEARWPZ to the blue team's soldiers in zone 4 (what it does is unknown)"},
	{true, 13, 37, 0, "do nothing to the blue team's soldiers in zone 4 (HUDITEM has no effect)"},
	{true, 13, 39, 0, "do nothing to the blue team's soldiers in zone 4 (TMATESTATUS has no effect)"},
	{true, 42, 0, 0, "make SSN 4 target Truck #2200 first"},
	{true, 43, 0, 0, "make SSN 4 never target Truck #2200"},
	{true, 44, 0, 0, "make SSN 4 target group 2200 first"},
	{true, 45, 0, 0, "make SSN 4 never target group 2200"},
	{true, 46, 0, 0, "make group 4 target Truck #2200 first"},
	{true, 47, 0, 0, "make group 4 never target Truck #2200"},
	{true, 48, 0, 0, "make group 4 target group 2200 first"},
	{true, 49, 0, 0, "make group 4 never target group 2200"},
	{true, 16, 0, 0, "move group 4 to team 2200"},
	{true, 24, 0, 0, "move SSN 4 to team 2200"},
	{true, 17, 0, 0, "move every unit of group 4 into group 2200"},
	{true, 25, 0, 0, "move SSN 4 into group 2200"},
	{true, 5, 1, 0, "set V4 to 2200"},
	{true, 5, 2, 0, "add 2200 to V4"},
	{true, 5, 3, 0, "take 2200 from V4"},
	{true, 5, 4, 0, "add 1 to V4"},
	{true, 5, 5, 0, "take 1 from V4"},
	{true, 34, 0, 0, "re-arm event 5"},
	{true, 30, 0, 0, "open the doors of group 4"},
	{true, 31, 0, 0, "close the doors of group 4"},
	{true, 27, 0, 0, "play the effect at the markers numbered 4"},
	{true, 38, 0, 0, "turn light channel 4 on"},
	{true, 40, 0, 0, "show the waypoints"},
	{true, 28, 37, 0, "flash HUD item 4 for 2200 ticks"},
	{true, 28, 38, 0, "clear the player's view keys"},
	{true, 28, 39, 0, "store whether 4 is 0 (nothing reads it)"},
	{true, 0, 0, 0, "do nothing"},
	{true, 41, 0, 0, "do nothing (Execute WAC has no effect)"},
	{true, 39, 1, 0, "call a medevac for SSN 4 to the first teleport marker numbered 2200"},
	{true, 39, 2, 0, "call a flyover for SSN 4 to the first teleport marker numbered 2200"},
};

int test_golden_words() {
	const Names names;
	size_t checked = 0;
	for (const GoldenWords &golden : kGoldenWords) {
		const std::string words =
		        golden.action ? action_words(action(static_cast<bms::ActionType>(golden.type), golden.sub, 4, 2200, 3), names, nullptr)
		                      : trigger_words(trigger(static_cast<bms::TriggerMainType>(golden.type), golden.sub, 10034, 3, 2, golden.flags), names);
		if (words != golden.words)
			std::printf("  %s %d/%d: \"%s\" where the table says \"%s\"\n", golden.action ? "action" : "trigger", golden.type, golden.sub,
			            words.c_str(), golden.words);
		TEST_EXPECT(words == golden.words);
		++checked;
	}
	// Every type the picker offers is in the table, each trigger both ways.
	TEST_EXPECT(checked == logic_types(false).size() * 2 + logic_types(true).size());
	return 0;
}

// The words as runs (S24, the board's chips): each parameter's words its slot's run, the type's own words the
// type's (kLogicTypeSlot), a note on what the game makes of it apart; joined, the words. The hand-worded types
// too: a variable change, a Redirect, Show text, an AI command over a group and over an area.
bool runs_are(const std::vector<LogicRun> &got, const std::vector<LogicRun> &want) {
	bool same_runs = got.size() == want.size();
	for (size_t i = 0; same_runs && i < got.size(); ++i) same_runs = got[i].text == want[i].text && got[i].slot == want[i].slot;
	if (!same_runs) {
		std::printf("  got: ");
		for (const LogicRun &run : got) std::printf(" [%s|%d]", run.text.c_str(), run.slot);
		std::printf("\n  want:");
		for (const LogicRun &run : want) std::printf(" [%s|%d]", run.text.c_str(), run.slot);
		std::printf("\n");
	}
	return same_runs;
}

int test_runs() {
	const Names names;
	using M = bms::TriggerMainType;
	using A = bms::ActionType;
	constexpr int T = kLogicTypeSlot;
	TEST_EXPECT(runs_are(trigger_runs(trigger(M::Single, 4, 10034), names), {{"Hostage #10034", 0}, {" is destroyed", T}}));
	TEST_EXPECT(runs_are(trigger_runs(trigger(M::Single, 43, 10034, 2200, 25), names),
	                     {{"Hostage #10034", 0}, {" is within ", T}, {"25 m", 2}, {" of ", T}, {"Truck #2200", 1}}));
	TEST_EXPECT(runs_are(trigger_runs(trigger(M::Single, 4, 10034, 0, 0, bms::Trigger::kConditionNegated), names),
	                     {{"Hostage #10034", 0}, {" is not destroyed", T}}));
	// A zone no area holds: the game drops the trigger, said apart as a note.
	const DocumentlessZones zones;
	const std::vector<LogicRun> dropped = trigger_runs(trigger(M::Single, 10, 10034, 9), zones);
	TEST_EXPECT(dropped.size() == 4 && dropped[3].slot == -1 && dropped[3].text.find("never true in the game") != std::string::npos);
	TEST_EXPECT(runs_are(action_runs(action(A::MisvarChange, 2, 1, 7), names, nullptr), {{"add ", T}, {"7", 1}, {" to ", T}, {"V1", 0}}));
	TEST_EXPECT(runs_are(action_runs(action(A::OutputText, 0, 4), names, nullptr), {{"show text ", T}, {"4: 'Mission failed'", 0}}));
	TEST_EXPECT(runs_are(action_runs(action(A::RedirectGroupTo, 0, 4, 5, 2), names, nullptr),
	                     {{"send ", T}, {"group 4", 0}, {" along ", T}, {"path 5", 1}, {", from ", T}, {"stop 2", 2}}));
	TEST_EXPECT(runs_are(action_runs(action(A::RedirectSingleTo, 0, 10034, 125, 2200), names, nullptr),
	                     {{"send ", T}, {"Hostage #10034", 0}, {" to board ", 1}, {"Truck #2200", 2}, {" (any seat)", 1}}));
	const std::vector<LogicRun> alert = action_runs(action(A::ChangeGroupAI, 5, 4), names, nullptr);
	TEST_EXPECT(!alert.empty() && logic_runs_text(alert) == action_words(action(A::ChangeGroupAI, 5, 4), names, nullptr));
	bool subject = false;
	for (const LogicRun &run : alert) subject = subject || (run.slot == 0 && run.text == "group 4");
	TEST_EXPECT(subject);
	const std::vector<LogicRun> area = action_runs(action(A::AreaAiRed, 5, 3), names, nullptr);
	bool zone = false;
	for (size_t i = 0; i + 1 < area.size(); ++i)
		zone = zone || (area[i].slot == T && area[i].text.find("the red team's soldiers in ") != std::string::npos &&
		                area[i + 1].slot == 0 && area[i + 1].text == "Zone 3 (bridge)");
	TEST_EXPECT(zone);
	return 0;
}

int test_words() {
	const Names names;
	using M = bms::TriggerMainType;
	using A = bms::ActionType;
	// A trigger by its type's words, its parameters named; negated, its verb flips.
	TEST_EXPECT(trigger_words(trigger(M::Single, 4, 10034), names) == "Hostage #10034 is destroyed");
	TEST_EXPECT(trigger_words(trigger(M::Single, 4, 10034, 0, 0, bms::Trigger::kConditionNegated), names) ==
	            "Hostage #10034 is not destroyed");
	TEST_EXPECT(trigger_words(trigger(M::Single, 10, 10000, 3), names) == "the player is in Zone 3 (bridge)");
	TEST_EXPECT(trigger_words(trigger(M::Group, 6, 4, 3), names) == "group 4 has lost 3 units or more");
	TEST_EXPECT(trigger_words(trigger(M::Group, 7, 4, 5, -1), names) == "group 4 has passed stop -1 (never recorded: the game keeps stops 0 to 31) of path 5");
	TEST_EXPECT(trigger_words(trigger(M::Single, 43, 10034, 2200, 25), names) == "Hostage #10034 is within 25 m of Truck #2200");
	TEST_EXPECT(trigger_words(trigger(M::Event, 9, 2), names) == "event 3 has fired"); // the sub-type unread
	TEST_EXPECT(trigger_words(trigger(M::MissionVariable, 3, 1, 5), names) == "V1 is greater than 5");
	TEST_EXPECT(trigger_words(trigger(M::Player, 36, 30), names) == "the player has been outside the mission area for 30.7 s");
	// Never a guess: a type no row names says so.
	TEST_EXPECT(trigger_words(trigger(M::Single, 8, 10034), names) == "a trigger of unknown kind 8 (false)");
	TEST_EXPECT(trigger_words(trigger(static_cast<M>(0), 0), names) == "a trigger of no type (false)");
	TEST_EXPECT(trigger_words(trigger(static_cast<M>(12), 1), names) == "a trigger of unknown type 12 (false)");

	// An action by its type's words; a text key's string quoted where the names know it.
	TEST_EXPECT(action_words(action(A::OutputText, 0, 4), names, nullptr) == "show text 4: 'Mission failed'");
	TEST_EXPECT(action_words(action(A::OutputText, 0, 5), names, nullptr) == "show text 5");
	TEST_EXPECT(action_words(action(A::KillSingle, 0, 10034), names, nullptr) == "kill Hostage #10034");
	TEST_EXPECT(action_words(action(A::ChangeGTeamAction, 0, 4, 2), names, nullptr) == "move group 4 to the red team (team 2)");
	TEST_EXPECT(action_words(action(A::MisvarChange, 2, 1, 7), names, nullptr) == "add 7 to V1");
	TEST_EXPECT(action_words(action(A::MisvarChange, 4, 1, 7), names, nullptr) == "add 1 to V1");
	// A Redirect: a path from a stop, or a command naming an entity [world-wac-ai-re.md 11].
	TEST_EXPECT(action_words(action(A::RedirectGroupTo, 0, 4, 5, 2), names, nullptr) ==
	            "send group 4 along path 5, from stop 2");
	TEST_EXPECT(action_words(action(A::RedirectSingleTo, 0, 10034, 125, 2200), names, nullptr) ==
	            "send Hostage #10034 to board Truck #2200 (any seat)");
	TEST_EXPECT(action_words(action(A::RedirectSingleTo, 0, 10034, 0, 0), names, nullptr) == "stop Hostage #10034 (no path)");
	// An AI change by its command; sub-type 0 does nothing; an area's soldiers by their team.
	TEST_EXPECT(action_words(action(A::ChangeGroupAI, 5, 4), names, nullptr) == "put group 4 on red alert");
	TEST_EXPECT(action_words(action(A::ChangeSingleAI, 8, 10034, 80), names, nullptr) == "set Hostage #10034's accuracy to 80");
	TEST_EXPECT(action_words(action(A::ChangeGroupAI, 43, 6, 1), names, nullptr) == "turn group 6's indestructibility on");
	TEST_EXPECT(action_words(action(A::ChangeGroupAI, 0, 4), names, nullptr) == "do nothing to group 4 (no AI change)");
	TEST_EXPECT(action_words(action(A::AreaAiRed, 5, 3), names, nullptr) ==
	            "put the red team's soldiers in Zone 3 (bridge) on red alert");
	TEST_EXPECT(action_words(action(A::ChangeSingleAI, 99, 10034), names, nullptr) ==
	            "apply AI command 99 to Hostage #10034 (what it does is unknown)");
	// A sub-goal by its slot, with the objective's line and its message where the header gives the id.
	bms::Header header{};
	header.win_conditions[1] = 7;
	TEST_EXPECT(action_words(action(A::SubGoalWon, 0, 2), names, &header) ==
	            "win sub-goal 2 ('Rescue the hostage'), saying 'Hostage rescued'");
	TEST_EXPECT(action_words(action(A::SubGoalWon, 0, 2), names, nullptr) == "win sub-goal 2");
	TEST_EXPECT(action_words(action(A::ShowWinSubgoal, 0, 2, 0), names, &header) == "hide win objective 2");
	TEST_EXPECT(action_words(action(static_cast<A>(29)), names, nullptr) == "an action of unknown type 29 (does nothing)");
	TEST_EXPECT(action_words(action(A::ExecuteWac), names, nullptr) == "do nothing (Execute WAC has no effect)");
	return 0;
}

int test_sentences() {
	const Names names;
	using M = bms::TriggerMainType;
	using A = bms::ActionType;
	mission::EventChain chain{};
	chain.triggers = {trigger(M::Single, 4, 10034)};
	chain.actions = {action(A::OutputText, 0, 4), action(A::SubGoalLost, 0, 2)};
	TEST_EXPECT(event_sentence(chain, names, nullptr) == "When Hostage #10034 is destroyed, then show text 4: 'Mission failed'; lose sub-goal 2.");
	// The joins fold left to right, each the previous trigger's [orig: EventTrigger_EvaluateChain
	// @0x454050]: a join that changes holds what came before it together.
	chain.triggers = {trigger(M::Group, 10, 3, 3), trigger(M::Player, 39, 2200, 0, 0, bms::Trigger::kConditionOr),
	                  trigger(M::Group, 10, 4, 3), trigger(M::Player, 39, 10034)};
	chain.actions = {action(A::PlayWavList, 0, 30)};
	TEST_EXPECT(event_sentence(chain, names, nullptr) ==
	            "When ((group 3 has a unit in Zone 3 (bridge) and the player is on Truck #2200) or group 4 has a unit in Zone 3 "
	            "(bridge)) and the player is on Hostage #10034, then play dialog 30.");
	chain.triggers = {trigger(M::Single, 5, 10034, 0, 0, bms::Trigger::kConditionXor), trigger(M::Single, 5, 2200)};
	chain.actions.clear();
	TEST_EXPECT(event_sentence(chain, names, nullptr) ==
	            "When either Hostage #10034 is alive or else Truck #2200 is alive (not both), then nothing.");
	// No trigger: at once; the flags, the delay and the repeat in words (a unit is 64 ticks: 1.024 s).
	chain.triggers.clear();
	chain.actions = {action(A::BlueWin)};
	TEST_EXPECT(event_sentence(chain, names, nullptr) == "Right away, then end the round: the blue team (team 1) wins.");
	chain.event.flags = bms::EventFlags::PreMission;
	TEST_EXPECT(event_sentence(chain, names, nullptr) == "At the mission's start, then end the round: the blue team (team 1) wins.");
	chain.event.flags = bms::EventFlags::ResetAfter;
	chain.event.delay = 5;
	chain.event.reset_after = 10;
	chain.triggers = {trigger(M::Single, 5, 10034)};
	const EventWords words = event_words(chain, names, nullptr);
	TEST_EXPECT(words.when == "When Hostage #10034 is alive" && words.delay == "after 5.1 s" &&
	            words.repeat == "Checked again about 11.3 s after its triggers held.");
	TEST_EXPECT(words.sentence ==
	            "When Hostage #10034 is alive, then, after 5.1 s, end the round: the blue team (team 1) wins. Checked again about "
	            "11.3 s after its triggers held.");
	TEST_EXPECT(logic_units_seconds(1) == 1.024);
	return 0;
}

// The game's reading where it differs from what the records seem to say (S15 review): a delay or a
// repeat past 512 steps wraps and ends on the next pass [bms-event-runtime-re.md 1.2]; a start or end
// event's delay never ends and its repeat never counts [1.6]; both passes; berserk's raw 0x200 [1.3];
// command 124's seats and 126's unknown rest [world-wac-ai-re.md 3.2, 9.1]; the green team's win; the
// stops by the game's number, a visited stop past 31 never recorded [3a]; an SSN of 128 or more never
// recorded by the sees, targeted, shot and visited records [3a]; the teammate's marker [7.5].
int test_game_reading() {
	const Names names;
	using M = bms::TriggerMainType;
	using A = bms::ActionType;
	mission::EventChain chain{};
	chain.triggers = {trigger(M::Single, 5, 10034)};
	chain.actions = {action(A::KillSingle, 0, 10034)};
	chain.event.delay = 600;
	TEST_EXPECT(same(event_words(chain, names, nullptr).delay, "after about 1.0 s (past 512 steps the game's countdown wraps)"));
	chain.event.delay = 512;
	TEST_EXPECT(same(event_words(chain, names, nullptr).delay, "after 524.3 s"));
	TEST_EXPECT(logic_steps_words(0) == "0.0 s");
	chain.event.delay = 0;
	chain.event.flags = bms::EventFlags::ResetAfter;
	chain.event.reset_after = 700;
	TEST_EXPECT(same(event_words(chain, names, nullptr).repeat,
	                 "Checked again about 2.0 s after its triggers held (past 512 steps the game's countdown wraps)."));
	chain.event.reset_after = 0;
	TEST_EXPECT(same(event_words(chain, names, nullptr).repeat, "Checked again each pass after its triggers held."));
	// A start event: its wait never ends, its repeat never counts; both passes worded.
	chain.event.flags = bms::EventFlags(uint32_t(bms::EventFlags::PreMission) | uint32_t(bms::EventFlags::ResetAfter));
	chain.event.delay = 5;
	chain.event.reset_after = 10;
	const EventWords start = event_words(chain, names, nullptr);
	TEST_EXPECT(same(start.delay, "never (a start or end event is checked once, so its wait never ends)") && start.repeat.empty());
	chain.event.flags = bms::EventFlags(uint32_t(bms::EventFlags::PreMission) | uint32_t(bms::EventFlags::PostMission));
	chain.event.delay = 0;
	TEST_EXPECT(same(event_words(chain, names, nullptr).when,
	                 "At the mission's start, and at its end if it did not fire then, if Hostage #10034 is alive"));
	// Berserk: negated always true; and-joined with a true trigger never; or-else-joined an or.
	chain.event = {};
	TEST_EXPECT(same(trigger_words(trigger(M::Player, 18, 0, 0, 0, bms::Trigger::kConditionNegated), names),
	                 "the player is not berserk (always true in the game: it reads the berserk bit as a raw 0x200)"));
	chain.triggers = {trigger(M::Player, 18), trigger(M::Single, 5, 10034)};
	TEST_EXPECT(event_words(chain, names, nullptr).when.find("never true in the game") != std::string::npos);
	chain.triggers = {trigger(M::Player, 18, 0, 0, 0, bms::Trigger::kConditionXor), trigger(M::Single, 5, 10034)};
	TEST_EXPECT(event_words(chain, names, nullptr).when.find("in the game either or both") != std::string::npos);
	// The board commands by their seats; 126's witnessed hold alone; the green team's win.
	TEST_EXPECT(same(action_words(action(A::RedirectSingleTo, 0, 10034, 124, 2200), names, nullptr),
	                 "send Hostage #10034 to board Truck #2200 (any seat but the controller's)"));
	TEST_EXPECT(same(action_words(action(A::RedirectSingleTo, 0, 10034, 126, 0), names, nullptr),
	                 "send Hostage #10034 on command 126 (Goto group): it holds within 10 m; what else it does is unknown"));
	TEST_EXPECT(same(action_words(action(A::RedirectGroupTo, 0, 4, 5, -1), names, nullptr), "send group 4 along path 5, from the nearest stop"));
	TEST_EXPECT(names.path(124) == "Goto SSN (not the controller seat)" && std::string(mission::path_command_editor_name(124)) == "Goto SSN (not driver)");
	TEST_EXPECT(same(action_words(action(A::GreenWin), names, nullptr), "end the round: the green team (team 0) wins"));
	// Stops by the game's number; a visited stop past 31 and an SSN of 128 or more never recorded.
	TEST_EXPECT(same(trigger_words(trigger(M::Single, 7, 12, 5, 8), names), "SSN 12 has passed stop 8 of path 5"));
	TEST_EXPECT(same(trigger_words(trigger(M::Single, 7, 12, 5, 40), names),
	                 "SSN 12 has passed stop 40 (never recorded: the game keeps stops 0 to 31) of path 5"));
	TEST_EXPECT(same(trigger_words(trigger(M::Single, 16, 10034, 12), names),
	                 "Hostage #10034 (never recorded: the game keeps SSNs below 128) has seen SSN 12"));
	// The teammate's marker; an area action's box values; an action type whose sub-type selects nothing.
	TEST_EXPECT(same(action_words(action(A::Teammates, 1, 10034, 3), names, nullptr),
	                 "call a medevac for Hostage #10034 to the first teleport marker numbered 3"));
	bms::Action engage = action(A::AreaAiRed, 42, 3, 50, 90);
	const std::string engaged = action_words(engage, names, nullptr);
	TEST_EXPECT(engaged.rfind("set the red team's soldiers in Zone 3 (bridge)'s engage distance to 50", 0) == 0 &&
	            engaged.find("to (a value of the area's box)") != std::string::npos);
	TEST_EXPECT(std::string(logic_action_title(int32_t(A::ChangeGroupAI))) == "Change group AI" && !logic_action_title(29));
	std::printf("game reading: wrapped and never-ending waits, berserk, the board seats, stops and SSNs never recorded\n");
	return 0;
}

std::unique_ptr<Document> open(const std::vector<uint8_t> &bytes, const char *name) {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, name, AssetKind::Mission, "jo", error)) return nullptr;
	return document;
}

// The minted mission's titles: event 1 the walker inside zone 20 re-arming event 2, event 2 event 1
// having fired killing group 2 after its delay of 5 units; the walker by its kind and SSN (its title,
// the display names' "Organic #SSN"), zone 30 as the mission's boundary.
int test_document_titles() {
	std::unique_ptr<Document> document =
	        open(test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms"),
	             "synth_logic.bms");
	TEST_EXPECT(document);
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event), organics = m.rows_of(MissionKind::Organic);
	TEST_EXPECT(events.size() == 2 && organics.size() == 2);
	const int32_t walker = static_cast<const EntityRow &>(*organics[0]).native.id;
	const NodeAddress first{events[0]->id, k(MissionKind::Event), 0}, second{events[1]->id, k(MissionKind::Event), 0};
	TEST_EXPECT(m.record_title(first) ==
	            "Event 1: When Organic #" + std::to_string(walker) + " is in Zone 20, then re-arm event 2.");
	TEST_EXPECT(m.record_title(second) == "Event 2: When event 1 has fired, then, after 5.1 s, kill group 2.");
	// The record's name is still its kind and place (the graph's, Problems', the wire's).
	TEST_EXPECT(m.record_name(first) == "Event 1");
	// A trigger and an action by their words.
	const std::vector<Document::Collection> lists = m.collections_of(first);
	TEST_EXPECT(lists.size() == 2 && lists[0].ids.size() == 1 && lists[1].ids.size() == 1);
	TEST_EXPECT(m.record_title({first.row, k(MissionKind::Trigger), lists[0].ids[0]}) ==
	            "Organic #" + std::to_string(walker) + " is in Zone 20");
	TEST_EXPECT(m.record_title({first.row, k(MissionKind::Action), lists[1].ids[0]}) == "re-arm event 2");
	// The document's names: what no record holds said so.
	const DocumentMissionNames names(m);
	TEST_EXPECT(same(names.entity(99999), "SSN 99999 (no entity has it)"));
	TEST_EXPECT(same(names.zone(7), "zone 7 (no area has it)"));
	TEST_EXPECT(same(names.event(5), "event 6 (the mission has no such event)"));
	TEST_EXPECT(names.entity(10000) == "the player");
	// An edit moves the words: zone 30 named instead.
	Edit edit;
	edit.operation = EditOperation::Set;
	edit.address = {first.row, k(MissionKind::Trigger), lists[0].ids[0]};
	edit.field = "param2";
	edit.value = int64_t(30);
	Diagnostic error;
	TEST_EXPECT(document->apply({edit}, error));
	TEST_EXPECT(m.record_title(first) ==
	            "Event 1: When Organic #" + std::to_string(walker) + " is in Zone 30 (mission area), then re-arm event 2.");
	return 0;
}

// The words on the wire and in Problems: over a project holding the minted mission with its first
// event's trigger naming an SSN no entity has, the `record` query's title of the event (its sentence)
// and of the trigger (its words) beside their names, and the `problems` query's row of the trigger's
// missing SSN naming the trigger in words (`record_title`) while the mission is open.
int test_wire_and_problems() {
	editor_test::TempProjectDir dir("opennova_mission_sentence_wire");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Words"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	{
		const std::vector<uint8_t> bytes =
		        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
		bms::File file;
		std::string message;
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		file.triggers[0].param1 = 4321;
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::write(file, out, message));
		TEST_EXPECT(editor_test::write_bytes(root + "/missions/words.bms", out));
	}
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/words.bms"));
	const Document *document = session.document_for("missions/words.bms");
	TEST_EXPECT(document);
	if (!document) return 1;
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const Node *event = m.rows_of(MissionKind::Event)[0];
	const NodeId trigger = m.collections_of({event->id, event->kind, 0})[0].ids[0];
	const auto ask = [&](const char *name, opennova::io::JsonValue args) {
		std::string error;
		opennova::io::JsonValue answer = session.query(name, args, error);
		if (!error.empty()) std::printf("  %s refused: %s\n", name, error.c_str());
		return answer;
	};
	opennova::io::JsonValue args = opennova::io::JsonValue::make_object();
	args.set("path", opennova::io::json_string("missions/words.bms"));
	args.set("id", opennova::io::JsonValue::make_number(double(event->id)));
	opennova::io::JsonValue answer = ask("record", args);
	TEST_EXPECT(same(answer.get_string("name", ""), "Event 1"));
	TEST_EXPECT(same(answer.get_string("title", ""), "Event 1: When SSN 4321 (no entity has it) is in Zone 20, then re-arm event 2."));
	args.set("id", opennova::io::JsonValue::make_number(double(trigger)));
	answer = ask("record", args);
	TEST_EXPECT(same(answer.get_string("title", ""), "SSN 4321 (no entity has it) is in Zone 20"));
	opennova::io::JsonValue problems_args = opennova::io::JsonValue::make_object();
	problems_args.set("text", opennova::io::json_string("4321"));
	const opennova::io::JsonValue problems = ask("problems", problems_args);
	const opennova::io::JsonValue *rows = problems.get("problems");
	size_t worded = 0;
	for (const opennova::io::JsonValue &row : rows ? rows->array : std::vector<opennova::io::JsonValue>()) {
		if (row.get_string("code", "") != "reference.missing" || row.get_string("field", "") != "param1") continue;
		TEST_EXPECT(row.get_string("record", "") == "Event 1/Trigger 1");
		TEST_EXPECT(same(row.get_string("record_title", ""), "SSN 4321 (no entity has it) is in Zone 20"));
		++worded;
	}
	TEST_EXPECT(worded == 1);
	return 0;
}

// The types "Add trigger" and "Add action" offer: each a title, a group and a line; every trigger
// sub-type and action type mission_params has a row for among them (the AI actions by command).
int test_types() {
	const std::vector<LogicType> &triggers = logic_types(false), &actions = logic_types(true);
	std::set<std::pair<int32_t, int32_t>> offered;
	for (const LogicType &type : triggers) {
		TEST_EXPECT(!type.action && *type.title && *type.group && *type.tip);
		TEST_EXPECT(offered.insert({type.type, type.sub}).second);
	}
	// Every sub-type the evaluator has a case for, per its main type.
	for (const mission::MissionChoice &main : [] {
		     const mission::MissionChoices all = mission::trigger_main_types();
		     return std::vector<mission::MissionChoice>(all.rows, all.rows + all.count);
	     }()) {
		const mission::MissionChoices subs = mission::trigger_sub_types(int32_t(main.value));
		for (size_t i = 0; i < subs.count; ++i) {
			const int32_t sub = int32_t(subs.rows[i].value);
			if (sub == 0 && mission::trigger_reads_sub_type(int32_t(main.value))) continue; // "Null": false
			TEST_EXPECT(logic_type(false, int32_t(main.value), sub) != nullptr);
		}
	}
	for (const LogicType &type : actions) TEST_EXPECT(type.action && *type.title && *type.group && *type.tip);
	// Every action type the dispatcher names; one whose sub-type selects what it does (an AI action's
	// command, a variable's change, a special, a teammate call) by each sub-type the dispatcher has an
	// arm for, its sub-type 0 none (it does nothing) and the teammates' 3 none (no arm).
	const mission::MissionChoices types = mission::action_types();
	for (size_t i = 0; i < types.count; ++i) {
		const int32_t type = int32_t(types.rows[i].value);
		const mission::MissionChoices subs = mission::action_sub_types(type);
		if (subs.count == 1) {
			TEST_EXPECT(logic_type(true, type, 0) != nullptr);
			continue;
		}
		for (size_t j = 0; j < subs.count; ++j) {
			const int32_t sub = int32_t(subs.rows[j].value);
			if (sub == 0) continue;
			TEST_EXPECT(logic_type(true, type, sub) != nullptr ||
			            (type == int32_t(bms::ActionType::Teammates) && sub == 3));
		}
		TEST_EXPECT(logic_type(true, type, 0) == nullptr);
	}
	const LogicType *red = logic_type(true, int32_t(bms::ActionType::ChangeGroupAI), 5);
	TEST_EXPECT(red && same(red->title, "Group AI: Red alert") && same(red->group, "AI"));
	const LogicType *set = logic_type(true, int32_t(bms::ActionType::MisvarChange), 1);
	TEST_EXPECT(set && same(set->title, "Set a variable"));
	// The groups stand together, in order.
	std::set<std::string> closed;
	std::string current;
	for (const LogicType &type : actions) {
		if (current == type.group) continue;
		TEST_EXPECT(!closed.count(type.group));
		if (!current.empty()) closed.insert(current);
		current = type.group;
	}
	return 0;
}

// Every event of every shipped mission (OPENNOVA_JO_DIR, base and each expansion through the VFS),
// worded by its document: none of a trigger or an action type no row names (the shipped missions use
// only witnessed types), the time it took.
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped event in words)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, events = 0, triggers = 0, actions = 0, unknown = 0, longest = 0, runs_made = 0;
	double took_ms = 0;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			std::unique_ptr<Document> document = open(bytes, file.logical_name.c_str());
			TEST_EXPECT(document && !document->blocked());
			if (!document) continue;
			++missions;
			const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
			const DocumentMissionNames names(m);
			const bms::Header *header = m.mission_row() ? &m.mission_row()->native.header : nullptr;
			const auto started = std::chrono::steady_clock::now();
			for (const Node *row : m.rows_of(MissionKind::Event)) {
				++events;
				const std::string sentence = m.record_title({row->id, row->kind, 0});
				longest = std::max(longest, sentence.size());
				const auto &chain = static_cast<const MissionRecordRow<mission::EventChain> &>(*row).native;
				for (const bms::Trigger &t : chain.triggers) {
					++triggers;
					if (!logic_type(false, int32_t(t.main_type), t.sub_type)) {
						++unknown;
						std::printf("  %s: trigger %d/%d\n", file.logical_name.c_str(), int(t.main_type), t.sub_type);
					}
				}
				for (const bms::Action &a : chain.actions) {
					++actions;
					if (!logic_type(true, int32_t(a.action_type), a.action_sub_type)) {
						++unknown;
						std::printf("  %s: action %d/%d\n", file.logical_name.c_str(), int(a.action_type), a.action_sub_type);
					}
				}
				TEST_EXPECT(sentence.find("unknown type") == std::string::npos);
				// The runs (S24): joined, the words; a parameter's run of a slot the type reads.
				for (const bms::Trigger &t : chain.triggers) {
					const std::vector<LogicRun> runs = trigger_runs(t, names);
					TEST_EXPECT(logic_runs_text(runs) == trigger_words(t, names));
					for (const LogicRun &run : runs) {
						TEST_EXPECT(run.slot >= -1 && run.slot <= kLogicTypeSlot && !run.text.empty());
						TEST_EXPECT(run.slot < 0 || run.slot == kLogicTypeSlot ||
						            mission::trigger_param_kind(t, run.slot) != mission::ParamKind::Unused);
						runs_made += 1;
					}
				}
				for (const bms::Action &a : chain.actions) {
					const std::vector<LogicRun> runs = action_runs(a, names, header);
					TEST_EXPECT(logic_runs_text(runs) == action_words(a, names, header));
					for (const LogicRun &run : runs) {
						TEST_EXPECT(run.slot >= -1 && run.slot <= kLogicTypeSlot && !run.text.empty());
						TEST_EXPECT(run.slot < 0 || run.slot == kLogicTypeSlot ||
						            mission::action_param_kind(a, run.slot) != mission::ParamKind::Unused);
						runs_made += 1;
					}
				}
			}
			took_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions, %zu events (%zu triggers, %zu actions) worded in %.1f ms, the longest sentence %zu "
	            "characters, %zu of a type no row names\n",
	            missions, events, triggers, actions, took_ms, longest, unknown);
	std::printf("retail: every trigger's and action's runs joined are its words (%zu runs)\n", runs_made);
	TEST_EXPECT(missions == 115 && unknown == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_words() != 0) return 1;
	if (test_runs() != 0) return 1;
	if (test_golden_words() != 0) return 1;
	if (test_sentences() != 0) return 1;
	if (test_game_reading() != 0) return 1;
	if (test_document_titles() != 0) return 1;
	if (test_wire_and_problems() != 0) return 1;
	if (test_types() != 0) return 1;
	return test_retail();
}
