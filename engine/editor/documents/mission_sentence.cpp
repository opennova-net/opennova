#include "mission_sentence.h"

// The words of mission_sentence.h. Each trigger sub-type and each action type is a row: what it is
// called alone, its group, its line, and its words over its parameters: {1}..{4} the parameter as its
// kind words it (formats/mission/mission_params.h), and a choice [a|b] the words where the trigger holds
// (a) or is negated (b). The meanings are the witness record's (docs/mission/bms-event-runtime-re.md:
// 3a, 3b and 7.4 for the triggers, 7.5 and 8.2 for the actions, 1.2 to 1.6 for an event's timing).

#include <cstdio>
#include <memory>

#include <base/io/tick_rate.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/mission_field.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

using K = ParamKind;
using A = bms::ActionType;

// --- the groups -------------------------------------------------------------------------------------

// A trigger's.
constexpr const char *kLife = "Life and death";
constexpr const char *kSight = "Sight and combat";
constexpr const char *kAlert = "Alert";
constexpr const char *kPlace = "Areas and waypoints";
constexpr const char *kCarry = "Carrying and riding";
constexpr const char *kLogic = "Events and variables";
constexpr const char *kPlayer = "The player";
constexpr const char *kDialog = "Dialog";
constexpr const char *kMates = "Teammates";
// An action's (kLogic and kMates too).
constexpr const char *kGoals = "Objectives and the end";
constexpr const char *kText = "Text and sound";
constexpr const char *kKill = "Killing and removing";
constexpr const char *kMove = "Movement";
constexpr const char *kAiGroup = "AI";
constexpr const char *kTargets = "Targets";
constexpr const char *kTeams = "Teams and groups";
constexpr const char *kWorld = "The world";
constexpr const char *kOther = "Does nothing";

// --- the triggers (sections 3a, 3b, 7.4) --------------------------------------------------------------

struct TriggerRow {
	int32_t main, sub;
	const char *title, *group, *tip, *words;
};

constexpr int32_t kGroup = 1, kSingle = 2, kEvent = 3, kVariable = 4, kSecond = 5, kTeammate = 6, kPlayerType = 7;

constexpr TriggerRow kTriggers[] = {
	// Group [orig: EventTrigger_EvaluateCondition cat 1 @0x45364a]. The sees, targeted and shot records
	// are kept from the mission's start (section 3a); the counts are taken every 62 ticks.
	{kGroup, 4, "Group is wiped out", kLife, "No unit of the group is alive.", "{1} [has no unit left alive|still has a unit alive]"},
	{kGroup, 5, "Group has a unit alive", kLife, "At least one unit of the group is alive.", "{1} [still has a unit alive|has no unit left alive]"},
	{kGroup, 6, "Group has lost units", kLife, "The group has lost at least this many of the units it started with.",
	 "{1} [has|has not] lost {2} units or more"},
	{kGroup, 9, "Group is intact", kLife, "Every unit the group started with is alive.", "{1} [has lost no unit|has lost a unit]"},
	{kGroup, 12, "Group has units left", kLife, "At least this many of the group's units are alive.",
	 "{1} [has {2} units or more alive|has fewer than {2} units alive]"},
	{kGroup, 1, "Group has seen group", kSight, "A unit of the first group has seen a unit of the second since the mission started.",
	 "{1} [has|has not] seen {2}"},
	{kGroup, 2, "Group has targeted group", kSight, "A unit of the first group has taken a unit of the second as its target.",
	 "{1} [has|has not] targeted {2}"},
	{kGroup, 13, "Group has shot group", kSight, "A unit of the first group has hurt a unit of the second.", "{1} [has|has not] shot {2}"},
	{kGroup, 16, "Group has seen entity", kSight, "A unit of the group has seen the entity since the mission started.",
	 "{1} [has|has not] seen {2}"},
	{kGroup, 15, "Group has targeted entity", kSight, "A unit of the group has taken the entity as its target.",
	 "{1} [has|has not] targeted {2}"},
	{kGroup, 17, "Group has shot entity", kSight, "A unit of the group has hurt the entity.", "{1} [has|has not] shot {2}"},
	{kGroup, 3, "Group is at red alert", kAlert, "The group's alert is red.", "{1} [is|is not] at red alert"},
	{kGroup, 14, "Group is at yellow alert", kAlert, "The group's alert is yellow.", "{1} [is|is not] at yellow alert"},
	{kGroup, 10, "Group is in an area", kPlace, "A unit of the group stands inside the area.", "{1} [has a unit in|has no unit in] {2}"},
	{kGroup, 7, "Group has passed a waypoint", kPlace,
	 "A unit of the group has passed this waypoint of the path (kept until a Forget visited waypoints action).",
	 "{1} [has|has not] passed {3} of {2}"},
	{kGroup, 11, "Group carries an object of group", kCarry, "A soldier of the first group carries an object of the second.",
	 "a soldier of {1} [carries|does not carry] an object of {2}"},
	// Single [orig: cat 2 @0x4537f8]: the entity by its SSN.
	{kSingle, 4, "Entity is destroyed", kLife, "No organic, item or building of the SSN is alive (dead, removed, or never placed).",
	 "{1} [is|is not] destroyed"},
	{kSingle, 5, "Entity is alive", kLife, "An organic, item or building of the SSN is alive.", "{1} [is|is not] alive"},
	{kSingle, 6, "Entity has lost health", kLife, "The entity has lost at least this many hit points (a dead one too).",
	 "{1} [has|has not] lost {2} hit points or more"},
	{kSingle, 9, "Entity has full health", kLife, "The entity has all its hit points.", "{1} [has|does not have] full health"},
	{kSingle, 12, "Entity has health", kLife, "The entity has at least this many hit points.",
	 "{1} [has {2} hit points or more|has fewer than {2} hit points]"},
	{kSingle, 16, "Entity has seen entity", kSight, "The first has seen the second since the mission started.", "{1} [has|has not] seen {2}"},
	{kSingle, 15, "Entity has targeted entity", kSight, "The first has taken the second as its target.", "{1} [has|has not] targeted {2}"},
	{kSingle, 17, "Entity has shot entity", kSight, "The first has hurt the second.", "{1} [has|has not] shot {2}"},
	{kSingle, 1, "Entity has seen group", kSight, "The entity has seen a unit of the group since the mission started.",
	 "{1} [has|has not] seen {2}"},
	{kSingle, 2, "Entity has targeted group", kSight, "The entity has taken a unit of the group as its target.",
	 "{1} [has|has not] targeted {2}"},
	{kSingle, 13, "Entity has shot group", kSight, "The entity has hurt a unit of the group.", "{1} [has|has not] shot {2}"},
	// The raw tests are positive: the format's names for 43 to 45 say the opposite, which the trigger's
	// negation says (sections 3b, 8.2).
	{kSingle, 44, "Entity has a line of sight to entity", kSight,
	 "Nothing blocks a line between the two and they are within this distance (the format calls it No LOS: negate it for that).",
	 "{1} [has a clear line of sight to|has no clear line of sight to] {2} within {3}"},
	{kSingle, 45, "Entity sees entity", kSight,
	 "As the line of sight, and the second is within 30 degrees of where the first faces (the format names the opposite).",
	 "{1} [sees|does not see] {2} within {3}"},
	{kSingle, 3, "Entity is at red alert", kAlert, "The entity's own alert is red.", "{1} [is|is not] at red alert"},
	{kSingle, 14, "Entity is at yellow alert", kAlert, "The entity's own alert is yellow.", "{1} [is|is not] at yellow alert"},
	{kSingle, 10, "Entity is in an area", kPlace, "An organic or item of the SSN stands inside the area.", "{1} [is|is not] in {2}"},
	{kSingle, 43, "Entities are near", kPlace, "The two are within this distance of each other (the format calls it Farther than: negate it for that).",
	 "{1} [is|is not] within {3} of {2}"},
	{kSingle, 7, "Entity has passed a waypoint", kPlace,
	 "The entity has passed this waypoint of the path (kept until a Forget visited waypoints action).",
	 "{1} [has|has not] passed {3} of {2}"},
	{kSingle, 42, "Entity is on top of entity", kCarry, "The first stands or rides on the second (up to three steps down).",
	 "{1} [is|is not] on top of {2}"},
	{kSingle, 11, "Entity carries an object of group", kCarry, "The soldier carries an object of the group.",
	 "{1} [carries|does not carry] an object of {2}"},
	// Event: the event's latch window, whatever the sub-type [orig: cat 3 @0x453a75].
	{kEvent, 0, "Event has fired", kLogic, "The event's actions have run and it has not been re-armed since.",
	 "{1} [has|has not] fired"},
	// MissionVariable [orig: cat 4: 1 ==, 2 <, 3 >, 4 <=, 5 >=].
	{kVariable, 1, "Variable equals", kLogic, "A mission variable equals a number.", "{1} [equals|does not equal] {2}"},
	{kVariable, 2, "Variable is less than", kLogic, "A mission variable is less than a number.", "{1} [is|is not] less than {2}"},
	{kVariable, 3, "Variable is greater than", kLogic, "A mission variable is greater than a number.", "{1} [is|is not] greater than {2}"},
	{kVariable, 4, "Variable is at most", kLogic, "A mission variable is less than or equal to a number.", "{1} [is|is not] at most {2}"},
	{kVariable, 5, "Variable is at least", kLogic, "A mission variable is greater than or equal to a number.", "{1} [is|is not] at least {2}"},
	// The load parity: 0 at a session's first load, 1 after a restart [orig: cat 5 @0x453B24; D-EVT-3].
	{kSecond, 0, "Second time through", kLogic,
	 "The game flips this at every mission load of the session, a campaign's next mission too: the first load reads false, the "
	 "second true, the third false again.",
	 "it [is|is not] the second time through"},
	// Teammate [orig: cat 6: sub 1 @0x453b53, @0x453b67; subs 2 and 3 both the heli-lift count @0x453b42].
	{kTeammate, 1, "Teammates are on", kMates, "Single player with the teammates option on (never in a multiplayer session).",
	 "teammates [are|are not] on"},
	{kTeammate, 2, "A medevac is under way", kMates, "A helicopter lift is under way (the game reads the same as Evacuating).",
	 "a helicopter lift [is|is not] under way"},
	{kTeammate, 3, "An evacuation is under way", kMates, "A helicopter lift is under way (the game reads the same as Medic assisting).",
	 "a helicopter lift [is|is not] under way"},
	// Player [orig: cat 7 @0x453b7e].
	{kPlayerType, 39, "Player is on an entity", kCarry, "The player rides the entity (a vehicle, in any seat).", "the player [is|is not] on {1}"},
	{kPlayerType, 40, "Player drives an entity", kCarry, "The player drives the vehicle.", "the player [drives|does not drive] {1}"},
	{kPlayerType, 41, "Player is on an entity's gun", kCarry, "The player mans the gun.", "the player [is|is not] on {1}'s gun"},
	{kPlayerType, 38, "Player is attached to an entity", kCarry, "The player is attached to the entity.",
	 "the player [is|is not] attached to {1}"},
	{kPlayerType, 37, "Satchel charge in an area", kPlace, "A placed satchel charge lies inside the area.",
	 "a satchel charge [lies|does not lie] in {1}"},
	{kPlayerType, 36, "Player has left the mission area", kPlace,
	 "The player has been outside every mission area for at least this many steps of 64 ticks (about 1.02 s each).",
	 "the player [has|has not] been outside the mission area for {1}"},
	{kPlayerType, 34, "Dialog is not playing", kDialog, "The dialog is not among those playing now.", "{1} [is not|is] playing"},
	{kPlayerType, 35, "Dialog has played", kDialog, "The dialog has played and is not playing now.",
	 "{1} [has played and finished|has not played to its end]"},
	// The bit is answered raw, 0x200, and the chain folds it bitwise [section 1.3].
	{kPlayerType, 18, "Player is berserk", kPlayer,
	 "The local player's berserk AI bit is set. The game reads it as a raw 0x200: negated it is always true, and-joined with "
	 "a true trigger never true, or-else-joined it acts as or.",
	 "the player [is|is not] berserk"},
	{kPlayerType, 19, "Player chose first person", kPlayer, "The player pressed the first-person view key since the last check.",
	 "the player [has|has not] chosen the first-person view"},
	{kPlayerType, 20, "Player chose third person", kPlayer, "The player pressed the chase view key since the last check.",
	 "the player [has|has not] chosen the third-person view"},
	{kPlayerType, 21, "Player chose the gun view", kPlayer, "The player pressed the view-with-gun key since the last check.",
	 "the player [has|has not] chosen the view with the gun"},
	{kPlayerType, 32, "Player input bit", kPlayer, "A bit of the player's input word is set (bit index).",
	 "the player's input bit {1} [is|is not] set"},
	{kPlayerType, 33, "Player input bit past 15", kPlayer, "A bit of the player's input word, counted from bit 15, is set.",
	 "the player's input bit {1} + 15 [is|is not] set"},
	// The bits no key sets, the look byte's bit 0 never set [section 1.4]: what the game reads them as.
	{kPlayerType, 22, "Input bit 10 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 10 [is|is not] set (never, in the game)"},
	{kPlayerType, 23, "Input bit 11 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 11 [is|is not] set (never, in the game)"},
	{kPlayerType, 24, "Input bit 12 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 12 [is|is not] set (never, in the game)"},
	{kPlayerType, 25, "Input bit 13 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 13 [is|is not] set (never, in the game)"},
	{kPlayerType, 26, "Look bit clear (always true)", kPlayer, "The HUD's look byte's bit 0 is clear, which it always is.",
	 "the look bit [is clear (always, in the game)|is set (never, in the game)]"},
	{kPlayerType, 27, "Look bit set (never true)", kPlayer, "The HUD's look byte's bit 0 is set, which it never is.",
	 "the look bit [is set (never, in the game)|is clear (always, in the game)]"},
	{kPlayerType, 28, "Input bit 29 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 29 [is|is not] set (never, in the game)"},
	{kPlayerType, 29, "Input bit 14 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 14 [is|is not] set (never, in the game)"},
	{kPlayerType, 30, "Input bit 15 (never set)", kPlayer, "Nothing in the game sets this bit: it reads false.",
	 "the player's input bit 15 [is|is not] set (never, in the game)"},
};

const TriggerRow *trigger_row(int32_t main, int32_t sub) {
	// Event's and SecondTimeThrough's cases read no sub-type: their one row whatever it holds.
	if (main == kEvent || main == kSecond) sub = 0;
	for (const TriggerRow &row : kTriggers)
		if (row.main == main && row.sub == sub) return &row;
	return nullptr;
}

// --- the actions (sections 7.5, 8.2, 10, 11) -----------------------------------------------------------

struct ActionRow {
	int32_t type;
	const char *title, *group, *tip, *words;
};

constexpr int32_t t(A type) { return int32_t(type); }

// The words of the actions whose sub-type selects nothing; the others (Redirect, the AI changes,
// MisvarChange, SpecialSubType, Teammates) are worded in code below.
constexpr ActionRow kActions[] = {
	{t(A::SubGoalWon), "Win a sub-goal", kGoals, "Marks the objective won and shows its win message.", "win {1}"},
	{t(A::SubGoalLost), "Lose a sub-goal", kGoals, "Marks the objective lost and shows its lose message.", "lose {1}"},
	{t(A::ShowWinSubgoal), "Show or hide a win objective", kGoals,
	 "Shows (with its directive and the new-objective sound) or hides a win objective on the player's list.", ""},
	{t(A::ShowLoseSubgoal), "Show or hide a lose objective", kGoals,
	 "Shows (with its directive) or hides a lose objective on the player's list.", ""},
	// [orig: Server_ProcessRoundEnd(1/2/0); docs/world/world-wac-ai-re.md 20.1: 1 = blue, 0 = green]
	{t(A::BlueWin), "Blue team wins", kGoals, "Ends the round, the blue team (team 1) the winner.",
	 "end the round: the blue team (team 1) wins"},
	{t(A::RedWin), "Red team wins", kGoals, "Ends the round, the red team (team 2) the winner.",
	 "end the round: the red team (team 2) wins"},
	{t(A::GreenWin), "Green team wins", kGoals, "Ends the round, the green team (team 0) the winner.",
	 "end the round: the green team (team 0) wins"},
	{t(A::OutputText), "Show text", kText, "Shows a line of the mission's Triggered Text on the HUD.", ""},
	{t(A::PlayWavList), "Play dialog", kText,
	 "Plays a dialog of the mission's bank; after the round is over only when Always is on.", ""},
	{t(A::KillGroup), "Kill group", kKill, "Kills every unit of the group, dead ones included.", "kill {1}"},
	{t(A::KillSingle), "Kill entity", kKill, "Kills the entity.", "kill {1}"},
	{t(A::VaporizeGroup), "Remove group", kKill, "Takes every unit of the group out of the mission.", "remove {1} from the mission"},
	{t(A::VaporizeSingle), "Remove entity", kKill, "Takes the entity out of the mission.", "remove {1} from the mission"},
	{t(A::RedirectGroupTo), "Send group along a path", kMove, "Gives the group's units a waypoint path (or a command: board an entity, follow the player).", ""},
	{t(A::RedirectSingleTo), "Send entity along a path", kMove, "Gives the entity a waypoint path (or a command: board an entity, follow the player).", ""},
	{t(A::GroupTeleportAction), "Teleport group", kMove, "Moves the group's units to a teleport target (a placed type-6088 marker).",
	 "teleport {1} to {2}"},
	{t(A::SingleTeleportAction), "Teleport entity", kMove, "Moves the entity to a teleport target (a placed type-6088 marker).",
	 "teleport {1} to {2}"},
	{t(A::GroupResetHasVisited), "Forget a group's visited waypoints", kMove,
	 "Clears the waypoints the group has passed (paths 0 to 31), for the waypoint triggers.", "forget the waypoints {1} has passed"},
	{t(A::SingleResetHasVisited), "Forget an entity's visited waypoints", kMove,
	 "Clears the waypoints the entity has passed (paths 0 to 31), for the waypoint triggers.", "forget the waypoints {1} has passed"},
	{t(A::AttachToEmplaced), "Board the ordered vehicle", kMove,
	 "Puts the entity into the vehicle a board order named for it.", "put {1} into the vehicle it was ordered to board"},
	{t(A::GroupVelocity), "Set group speed (unused)", kMove, "Stores a speed for the group that the game never reads.",
	 "set {1}'s speed to {2} (the game never reads it)"},
	{t(A::SingleVelocity), "Set entity speed (does nothing)", kMove, "Does nothing in the game.", "set {1}'s speed to {2} (does nothing in the game)"},
	{t(A::ChangeGroupAI), "Change group AI", kAiGroup, "Changes an AI setting of every unit of the group.", ""},
	{t(A::ChangeSingleAI), "Change entity AI", kAiGroup, "Changes an AI setting of the entity.", ""},
	{t(A::AreaAiRed), "Change AI of the red team in an area", kAiGroup,
	 "Changes an AI setting of the red team's soldiers in the area (the game pairs the area's corners oddly: section 7.3).", ""},
	{t(A::AreaAiBlue), "Change AI of the blue team in an area", kAiGroup,
	 "Changes an AI setting of the blue team's soldiers in the area (the game pairs the area's corners oddly: section 7.3).", ""},
	{t(A::SsnTargetSsnPri), "Entity prefers to target entity", kTargets, "The first entity picks the second as its target first.",
	 "make {1} target {2} first"},
	{t(A::SsnTargetSsnExc), "Entity never targets entity", kTargets, "The first entity never picks the second as its target.",
	 "make {1} never target {2}"},
	{t(A::SsnTargetGroupPri), "Entity prefers to target group", kTargets, "The entity picks the group's units as targets first.",
	 "make {1} target {2} first"},
	{t(A::SsnTargetGroupExc), "Entity never targets group", kTargets, "The entity never picks the group's units as targets.",
	 "make {1} never target {2}"},
	{t(A::GroupTargetSsnPri), "Group prefers to target entity", kTargets, "The group's units pick the entity as their target first.",
	 "make {1} target {2} first"},
	{t(A::GroupTargetSsnExc), "Group never targets entity", kTargets, "The group's units never pick the entity as their target.",
	 "make {1} never target {2}"},
	{t(A::GroupTargetGroupPri), "Group prefers to target group", kTargets,
	 "The first group's units pick the second's as targets first.", "make {1} target {2} first"},
	{t(A::GroupTargetGroupExc), "Group never targets group", kTargets, "The first group's units never pick the second's as targets.",
	 "make {1} never target {2}"},
	{t(A::ChangeGTeamAction), "Change group's team", kTeams, "Moves the group's units to a team.", "move {1} to {2}"},
	{t(A::ChangeSteamAction), "Change entity's team", kTeams, "Moves the entity to a team.", "move {1} to {2}"},
	{t(A::ChangeGroupAction), "Move group into group", kTeams, "Moves every unit of the first group into the second.",
	 "move every unit of {1} into {2}"},
	{t(A::SingleChangeGroup), "Move entity into group", kTeams, "Moves the entity into the group.", "move {1} into {2}"},
	{t(A::MisvarChange), "Change a variable", kLogic, "Sets, adds to, takes from, adds 1 to or takes 1 from a mission variable.", ""},
	{t(A::ResetEvent), "Re-arm event", kLogic, "Lets the event fire again (its timers keep running).", "re-arm {1}"},
	{t(A::GroupOpenDoorAction), "Open group's doors", kWorld, "Opens the doors of every building and item of the group.",
	 "open the doors of {1}"},
	{t(A::GroupCloseDoorAction), "Close group's doors", kWorld, "Closes the doors of every building and item of the group.",
	 "close the doors of {1}"},
	{t(A::ParticleEffectAction), "Play effect at markers", kWorld,
	 "Plays the effect each type-6088 marker of this number names, at the marker.", "play the effect at the markers numbered {1}"},
	{t(A::SetLightState), "Switch a light channel", kWorld, "Turns one of the four light-group channels on or off (what reads it is unknown).",
	 "turn {1} {2}"},
	{t(A::ShowWaypoints), "Show or hide waypoints", kWorld, "Shows or hides the waypoints on the player's HUD.", ""},
	{t(A::SpecialSubType), "Special", kWorld, "A HUD item flash, clearing the player's view keys, or a stored flag nothing reads.", ""},
	{t(A::Teammates), "Teammate helicopter", kMates, "Calls a medevac or a flyover for a soldier to a marker.", ""},
	{t(A::Null), "Nothing", kOther, "The dispatcher has no case: nothing happens.", "do nothing"},
	{t(A::ExecuteWac), "Execute WAC (does nothing)", kOther, "The dispatcher has no case for it: nothing happens.",
	 "do nothing (Execute WAC has no effect)"},
};

const ActionRow *action_row(int32_t type) {
	for (const ActionRow &row : kActions)
		if (row.type == type) return &row;
	return nullptr;
}

// The actions whose sub-type selects what they do, offered one type per sub-type [orig:
// EventAction_Dispatch case 5 @0x45437e: 1 Set, 2 Add, 3 Sub, 4 Inc, 5 Dec; EventAction_HandleSpecialTypes
// @0x4535a0: 37, 38, 39, every other returning; case 39: 1 HeliLift_SpawnPickup, 2 HeliLift_SpawnFlyover,
// any other no arm].
struct SubRow {
	int32_t type, sub;
	const char *title, *group, *tip;
};
constexpr SubRow kActionSubs[] = {
	{t(A::MisvarChange), 1, "Set a variable", kLogic, "Sets a mission variable to a number."},
	{t(A::MisvarChange), 2, "Add to a variable", kLogic, "Adds a number to a mission variable (it wraps past 32 bits)."},
	{t(A::MisvarChange), 3, "Take from a variable", kLogic, "Takes a number from a mission variable (it wraps past 32 bits)."},
	{t(A::MisvarChange), 4, "Add 1 to a variable", kLogic, "Adds one to a mission variable."},
	{t(A::MisvarChange), 5, "Take 1 from a variable", kLogic, "Takes one from a mission variable."},
	{t(A::SpecialSubType), 37, "Flash a HUD item", kWorld, "Flashes one of the HUD's items (0 to 15) for a number of ticks."},
	{t(A::SpecialSubType), 38, "Clear the player's view keys", kPlayer,
	 "Clears the view keys the player pressed (what the view triggers read)."},
	{t(A::SpecialSubType), 39, "Store a flag (read by nothing)", kOther, "Stores whether a number is 0; nothing in the game reads it."},
	{t(A::Teammates), 1, "Call a medevac", kMates, "Sends a helicopter to pick up a soldier at a marker."},
	{t(A::Teammates), 2, "Call a flyover", kMates, "Sends a helicopter over a soldier to a marker."},
};

bool has_sub_rows(int32_t type) {
	for (const SubRow &row : kActionSubs)
		if (row.type == type) return true;
	return false;
}

// The subject an AI action's commands are offered under.
const char *ai_subject(int32_t type) {
	switch (static_cast<A>(type)) {
	case A::ChangeGroupAI: return "Group AI";
	case A::ChangeSingleAI: return "Entity AI";
	case A::AreaAiRed: return "Red team in an area";
	case A::AreaAiBlue: return "Blue team in an area";
	default: break;
	}
	return "AI";
}

// The AI commands of the four AI actions [orig: Entity_ApplyCommand @0x43ab60; dfx2med's tokens,
// section 8.2]: what each does to its subject, {s} the subject ("group 4", "Hostage #10034"), {2}..{4}
// the action's parameters.
struct AiRow {
	int32_t sub;
	const char *title, *tip, *words;
};
constexpr AiRow kAiCommands[] = {
	{5, "Red alert", "Puts them on red alert.", "put {s} on red alert"},
	{22, "Yellow alert", "Puts them on yellow alert.", "put {s} on yellow alert"},
	{6, "Green alert", "Puts them on green alert.", "put {s} on green alert"},
	{8, "Accuracy", "Sets their accuracy (0 to 100).", "set {s}'s accuracy to {2}"},
	{27, "Aiming skill", "Sets their aiming skill.", "set {s}'s aiming skill to {2}"},
	{26, "Driving skill", "Sets their driving skill.", "set {s}'s driving skill to {2}"},
	{29, "Combat speed", "Sets their speed in combat.", "set {s}'s combat speed to {2}"},
	{30, "Patrol speed", "Sets their speed on patrol.", "set {s}'s patrol speed to {2}"},
	{41, "Attack distance", "Sets the distance they attack from.", "set {s}'s attack distance to {2}"},
	{42, "Engage distance", "Sets the distances they engage between.", "set {s}'s engage distance to {2} to {3}"},
	{46, "Firing angle", "Sets their firing angle.", "set {s}'s firing angle to {2}"},
	{44, "Target entity", "Gives them an entity to target.", "make {s} target {2}"},
	{31, "Find and use", "Gives them an object to find and use (0 clears it).", "make {s} find and use {2}"},
	{45, "Start firing", "Turns their firing on or off.", "turn {s}'s firing {2}"},
	{2, "Guard", "Turns their guard behaviour on or off.", "turn {s}'s guard behaviour {2}"},
	{15, "Blind", "Turns their blindness on or off.", "turn {s}'s blindness {2}"},
	{16, "Berserk", "Turns their berserk behaviour on or off.", "turn {s}'s berserk behaviour {2}"},
	{17, "Climber", "Turns their climbing on or off.", "turn {s}'s climbing {2}"},
	{21, "Coward", "Turns their cowardice on or off.", "turn {s}'s cowardice {2}"},
	{40, "Node path", "Turns their node-path following on or off.", "turn {s}'s node-path following {2}"},
	{43, "Indestructible", "Makes them indestructible or not (an AI-less building takes no command).",
	 "turn {s}'s indestructibility {2}"},
	{28, "AI state", "Sets their AI state to a number (what each state is: unknown here).", "set {s}'s AI state to {2}"},
	{34, "Play part animation", "Plays an animation on them.", "play part animation {2} on {s} (play type {3}, time {4})"},
	{32, "Use waypoint zones (AIUSEWPZ)", "Stores a constant; what it does is unknown.", "apply AIUSEWPZ to {s} (what it does is unknown)"},
	{33, "Clear waypoint zones (AICLEARWPZ)", "Stores a constant; what it does is unknown.",
	 "apply AICLEARWPZ to {s} (what it does is unknown)"},
	{37, "HUD item (no effect)", "This binary has no arm for it: nothing happens.", "do nothing to {s} (HUDITEM has no effect)"},
	{39, "Teammate status (no effect)", "This binary has no arm for it: nothing happens.", "do nothing to {s} (TMATESTATUS has no effect)"},
};

const AiRow *ai_row(int32_t sub) {
	for (const AiRow &row : kAiCommands)
		if (row.sub == sub) return &row;
	return nullptr;
}

bool is_ai_action(int32_t type) {
	return type == t(A::ChangeGroupAI) || type == t(A::ChangeSingleAI) || type == t(A::AreaAiRed) || type == t(A::AreaAiBlue);
}

// --- the words of a value -------------------------------------------------------------------------

std::string number(int64_t value) { return std::to_string(value); }

// A text quoted, cut past 60 characters.
std::string quoted(const std::string &text) {
	constexpr size_t kMost = 60;
	if (text.size() <= kMost) return "'" + text + "'";
	return "'" + text.substr(0, kMost - 3) + "...'";
}

std::string choice_word(ParamKind kind, int64_t value) {
	const mission::MissionChoices choices = mission::param_choices(kind);
	if (const char *name = mission::mission_choice_name(choices.rows, choices.count, value)) return name;
	return number(value);
}

// A parameter as its kind words it.
std::string param_word(ParamKind kind, int64_t value, const MissionNames &names) {
	switch (kind) {
	case K::Group: return names.group(value);
	case K::Entity: return names.entity(value);
	case K::Zone: return names.zone(value);
	case K::Event: return names.event(value);
	case K::Path: return names.path(value);
	case K::PathNode:
		// A waypoint trigger's stop, by the game's number (0 the first): the bit of the visited word, which
		// holds stops 0 to 31 [section 3a]. (A Redirect's node is worded by redirect_words.)
		return value >= 0 && value < 32 ? "stop " + number(value)
		                                : "stop " + number(value) + " (never recorded: the game keeps stops 0 to 31)";
	case K::MissionVar: return "variable " + number(value);
	case K::Dialog: return "dialog " + number(value);
	case K::DistanceM: return number(value) + " m";
	case K::Seconds: return number(value) + " s";
	case K::SpeedKph: return number(value) + " km/h";
	case K::Bool: return value ? "on" : "off";
	case K::Team: return team_words(value);
	case K::SubGoal: return "sub-goal " + number(value);
	case K::HudTimer: return "HUD item " + number(value);
	case K::LightChannel: return "light channel " + number(value);
	case K::TeleportTarget: return "teleport target " + number(value);
	case K::WpNumber: return number(value);
	case K::Count:
	case K::Hp:
	case K::Bit:
	case K::Raw:
	case K::Unused: break;
	}
	return number(value);
}

int64_t param_of(const bms::Trigger &trigger, int slot) {
	const int32_t params[4] = {trigger.param1, trigger.param2, trigger.param3, trigger.param4};
	return slot >= 0 && slot < 4 ? params[slot] : 0;
}
int64_t param_of(const bms::Action &action, int slot) {
	const int32_t params[4] = {action.param1, action.param2, action.param3, action.param4};
	return slot >= 0 && slot < 4 ? params[slot] : 0;
}

// A template's words: [a|b] the first where `negated` is false, else the second; {1}..{4} the
// parameter (`word(slot)`), {s} `subject`. A negated template with no choice says "not" before it.
template <class Word>
std::string fill(const char *words, bool negated, const std::string &subject, const Word &word) {
	std::string out;
	bool chose = false;
	for (const char *c = words; *c; ++c) {
		if (*c == '[') {
			const char *bar = c + 1;
			while (*bar && *bar != '|' && *bar != ']') ++bar;
			const char *end = bar;
			while (*end && *end != ']') ++end;
			const std::string first(c + 1, bar), second(*bar == '|' ? bar + 1 : bar, end);
			out += negated ? second : first;
			chose = true;
			c = *end ? end : end - 1;
			continue;
		}
		out += *c;
	}
	std::string filled;
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i] == '{' && i + 2 < out.size() && out[i + 2] == '}') {
			const char key = out[i + 1];
			if (key >= '1' && key <= '4') {
				filled += word(key - '1');
				i += 2;
				continue;
			}
			if (key == 's') {
				filled += subject;
				i += 2;
				continue;
			}
		}
		filled += out[i];
	}
	return negated && !chose ? "not: " + filled : filled;
}

// The sub-goal of a slot (1..8) with the text its key reads where the names know it: the objective's
// own line (STRWINCOND%03i of the slot's id, the objectives panel's [orig: HUD_DrawWinConditions
// @0x5ba940]) for a win slot, and the message or the directive the action shows.
std::string slot_text(const MissionNames &names, const bms::Header *header, bool win, int64_t slot, const char *section,
                      const char *key) {
	if (!header || slot < 1 || slot > 8) return std::string();
	const uint8_t id = (win ? header->win_conditions : header->lose_conditions)[slot - 1];
	char name[32];
	std::snprintf(name, sizeof(name), "%s%03i", key, int(id));
	return names.text(section, name);
}

// A Redirect's words [orig: Entity_SetWaypointByTeam @0x43CD20, Entity_SetWaypointForTeam @0x43DD00:
// the path or the command, then the stop or the SSN; docs/world/world-wac-ai-re.md 11].
std::string redirect_words(const bms::Action &action, const MissionNames &names, const std::string &subject) {
	const int64_t path = action.param2;
	if (path == 0) return "stop " + subject + " (no path)";
	if (mission::path_command_names_entity(path)) {
		// [world-wac-ai-re.md 9.1: 123 admits the passenger seats alone, 124 every seat but the controller's
		// (the driver's, weighted lowest, is taken first), 125 any]
		const char *seat = path == 123 ? " (a passenger seat)" : path == 124 ? " (any seat but the controller's)" : " (any seat)";
		return "send " + subject + " to board " + names.entity(action.param3) + seat;
	}
	// [world-wac-ai-re.md 3.2: 126 parks the walk within 10 m @ 0x4baabd; the rest is not witnessed]
	if (path == 126)
		return "send " + subject + " on command 126 (Goto group): it holds within 10 m; what else it does is unknown";
	if (path == 127) return "send " + subject + " to follow the player";
	// The Redirect's node: -1 the nearest, else the stop by its number from 0 [world-wac-ai-re.md: only
	// node -1 requests the nearest, `Entity_SetWaypointByTeam @0x43CD20`].
	return "send " + subject + " along " + names.path(path) + ", from " +
	       (action.param3 == -1 ? std::string("the nearest stop") : "stop " + number(action.param3));
}

} // namespace

// --- the names ------------------------------------------------------------------------------------

std::string MissionNames::entity(int64_t ssn) const {
	if (ssn == mission::kPlayerSsn) return "the player";
	return "SSN " + number(ssn);
}

std::string MissionNames::zone(int64_t id) const { return "zone " + number(id); }

std::string MissionNames::group(int64_t group) const { return group == 0 ? "no group" : "group " + number(group); }

const char *path_command_words(int64_t number_) {
	// As the game acts on them [docs/world/world-wac-ai-re.md 3.2, 9.1: the board order's seat admit term
	// `(cmd != 124 || type != ctrlx) && (cmd != 123 || type == sitex)` @ 0x4353e6, the lowest weight
	// winning (the controller's and the driver's seats lowest); 126 parks its walk within 10 m
	// @ 0x4baabd; 127 follows the local player].
	switch (number_) {
	case 123: return "Goto SSN (passenger seat only)";
	case 124: return "Goto SSN (not the controller seat)";
	case 125: return "Goto SSN (any seat)";
	case 126: return "Goto group";
	case 127: return "Goto player";
	default: return nullptr;
	}
}

std::string MissionNames::path(int64_t number_) const {
	if (number_ == 0) return "no path";
	if (const char *command = path_command_words(number_)) return command;
	return "path " + number(number_);
}

std::string MissionNames::event(int64_t index) const { return "event " + number(index + 1); }

std::string MissionNames::text(const std::string &, const std::string &) const { return std::string(); }

bool MissionNames::zone_resolves(int64_t) const { return true; }

bool DocumentMissionNames::zone_resolves(int64_t id) const {
	// The first area of the id; a missing one, or a flat box (x_min == x_max or y_min == y_max), does not
	// resolve [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000, the scan @0x453077, the box test
	// @0x453093; section 7.3].
	const Node *row = document_.row(document_.zone_holder(id));
	if (!row) return false;
	const bms::AreaTrigger &area = static_cast<const AreaRow &>(*row).native;
	return area.x_min != area.x_max && area.y_min != area.y_max;
}

std::string DocumentMissionNames::entity(int64_t ssn) const {
	// The player's SSN names no record of the file [bms-event-runtime-re.md 7.3].
	if (ssn == mission::kPlayerSsn) return MissionNames::entity(ssn);
	const NodeId holder = document_.entity_holder(ssn);
	const Node *row = holder ? document_.row(holder) : nullptr;
	if (!row) return "SSN " + number(ssn) + " (no entity has it)";
	const NodeAddress address{holder, row->kind, 0};
	const std::string title = document_.record_title(address);
	// A title that is the bare SSN (the row's own name): the kind with it.
	if (title.empty() || title == number(ssn)) return std::string(document_.kind_label(address.kind)) + " " + number(ssn);
	return title;
}

std::string DocumentMissionNames::zone(int64_t id) const {
	const NodeId holder = document_.zone_holder(id);
	if (!holder) return "zone " + number(id) + " (no area has it)";
	const std::string title = document_.record_title({holder, node_kind(MissionKind::Area), 0});
	return title.empty() ? MissionNames::zone(id) : title;
}

std::string DocumentMissionNames::event(int64_t index) const {
	if (!counted_) {
		counted_ = true;
		for (const auto &row : document_.rows()) events_ += row && row->kind == node_kind(MissionKind::Event);
	}
	if (index < 0 || size_t(index) >= events_) return "event " + number(index + 1) + " (the mission has no such event)";
	return MissionNames::event(index);
}

// --- the words ------------------------------------------------------------------------------------

LogicJoin trigger_join(const bms::Trigger &trigger) {
	if (trigger.is_or()) return LogicJoin::Or;
	if (trigger.is_xor()) return LogicJoin::Xor;
	return LogicJoin::And;
}

const char *logic_join_words(LogicJoin join) {
	switch (join) {
	case LogicJoin::Or: return "or";
	case LogicJoin::Xor: return "or else";
	case LogicJoin::And: break;
	}
	return "and";
}

std::string trigger_words(const bms::Trigger &trigger, const MissionNames &names) {
	const int32_t main = int32_t(trigger.main_type);
	const bool negated = trigger.is_negated();
	const int32_t sub = trigger.sub_type;
	const auto word = [&](int slot) {
		// The AWOL counter counts 64-tick steps, not seconds [D-EVT-2]: its time as they come to.
		if (main == kPlayerType && sub == 36 && slot == 0) {
			char text[32];
			std::snprintf(text, sizeof(text), "%.1f s", logic_units_seconds(param_of(trigger, slot)));
			return std::string(text);
		}
		std::string out = param_word(mission::trigger_param_kind(trigger, slot), param_of(trigger, slot), names);
		if (mission::trigger_ssn_unrecorded(trigger, slot)) out += " (never recorded: the game keeps SSNs below 128)";
		return out;
	};
	if (const TriggerRow *row = trigger_row(main, sub)) {
		std::string out = fill(row->words, negated, std::string(), word);
		// An area trigger naming a zone that does not resolve is dropped at the mission's start: it reads
		// false, so negated it reads true [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000; section 7.3].
		const int zone_slot = (main == kGroup || main == kSingle) && sub == 10 ? 1 : main == kPlayerType && sub == 37 ? 0 : -1;
		if (zone_slot >= 0 && !names.zone_resolves(param_of(trigger, zone_slot)))
			out += negated ? " (always true in the game: it drops a trigger whose area does not resolve, and reads it false)"
			               : " (never true in the game: it drops a trigger whose area does not resolve)";
		// The berserk bit is read raw, 0x200, so negated (0x201) it is always true [section 1.3].
		if (main == kPlayerType && sub == 18 && negated) out += " (always true in the game: it reads the berserk bit as a raw 0x200)";
		return out;
	}
	// A type the evaluator has no case for reads false [orig: EventTrigger_EvaluateCondition's default],
	// a main type 0 the load's neutered zone reference among them.
	std::string out;
	if (main == 0) out = "a trigger of no type (false)";
	else if (main >= kGroup && main <= kPlayerType)
		out = "a trigger of unknown kind " + number(trigger.sub_type) + " (false)";
	else out = "a trigger of unknown type " + number(main) + " (false)";
	return negated ? "not: " + out : out;
}

std::string action_words(const bms::Action &action, const MissionNames &names, const bms::Header *header) {
	const int32_t type = int32_t(action.action_type);
	const int32_t sub = action.action_sub_type;
	const auto word = [&](int slot) { return param_word(mission::action_param_kind(action, slot), param_of(action, slot), names); };
	switch (action.action_type) {
	case A::RedirectGroupTo:
	case A::RedirectSingleTo: return redirect_words(action, names, word(0));
	case A::MisvarChange: {
		// [orig: EventAction_Dispatch case 5: 1 Set, 2 Add, 3 Sub, 4 Inc, 5 Dec; the add and the sub wrap]
		const std::string variable = word(0), value = number(action.param2);
		switch (sub) {
		case 1: return "set " + variable + " to " + value;
		case 2: return "add " + value + " to " + variable;
		case 3: return "take " + value + " from " + variable;
		case 4: return "add 1 to " + variable;
		case 5: return "take 1 from " + variable;
		default: return "do nothing to " + variable + " (change " + number(sub) + " is none the game has)";
		}
	}
	case A::OutputText: {
		// [orig: HUD_DisplayTriggeredText @0x51F190: Triggered Text's ID%03i]
		char key[32];
		std::snprintf(key, sizeof(key), "ID%03i", int(action.param1));
		const std::string text = names.text("Triggered Text", key);
		return "show text " + number(action.param1) + (text.empty() ? std::string() : ": " + quoted(text));
	}
	case A::PlayWavList:
		return "play " + word(0) + (action.param2 == 1 ? " (even after the round is over)" : std::string());
	case A::SubGoalWon:
	case A::SubGoalLost: {
		const bool win = action.action_type == A::SubGoalWon;
		const std::string goal = slot_text(names, header, win, action.param1, win ? "WinConditions" : "LoseConditions",
		                                   win ? "STRWINCOND" : "STRLOSECOND");
		const std::string message = slot_text(names, header, win, action.param1, win ? "WinConditions" : "LoseConditions",
		                                      win ? "STRWINMSG" : "STRLOSEMSG");
		std::string out = std::string(win ? "win " : "lose ") + word(0);
		if (!goal.empty()) out += " (" + quoted(goal) + ")";
		if (!message.empty()) out += ", saying " + quoted(message);
		return out;
	}
	case A::ShowWinSubgoal:
	case A::ShowLoseSubgoal: {
		const bool win = action.action_type == A::ShowWinSubgoal;
		const std::string what = std::string(win ? "win objective " : "lose objective ") + number(action.param1);
		if (action.param2 == 0) return "hide " + what;
		const std::string directive = slot_text(names, header, win, action.param1, win ? "WinConditions" : "LoseConditions",
		                                        win ? "STRWINDIRECTIVE" : "STRLOSEDIRECTIVE");
		return "show " + what + (directive.empty() ? std::string() : ": " + quoted(directive));
	}
	case A::ShowWaypoints: return action.param1 ? "show the waypoints" : "hide the waypoints";
	case A::SpecialSubType:
		// [orig: EventAction_HandleSpecialTypes @0x4535a0: 37 the HUD item flash, 38 the input bits cleared,
		// 39 a store nothing reads; every other returns]
		switch (sub) {
		case 37: return "flash " + word(0) + " for " + number(action.param2) + " ticks";
		case 38: return "clear the player's view keys";
		case 39: return "store whether " + number(action.param1) + " is 0 (nothing reads it)";
		default: return "do nothing (special " + number(sub) + " has no effect)";
		}
	case A::Teammates:
		// [orig: EventAction_Dispatch case 39: 1 HeliLift_SpawnPickup, 2 HeliLift_SpawnFlyover, any other none]
		switch (sub) {
		// [section 7.5: the first pool-3 type-6088 marker of the number]
		case 1: return "call a medevac for " + word(0) + " to the first teleport marker numbered " + number(action.param2);
		case 2: return "call a flyover for " + word(0) + " to the first teleport marker numbered " + number(action.param2);
		default: return "do nothing (teammate call " + number(sub) + " has no effect)";
		}
	default: break;
	}
	if (is_ai_action(type)) {
		// [orig: Entity_HandleAlertCommand @0x43CF10, Entity_KillTeamInBounds @0x43D030, Entity_HandleAlertStateEvent
		// @0x43DEE0: sub-type 0 does nothing]
		std::string subject = word(0);
		const bool area = action.action_type == A::AreaAiRed || action.action_type == A::AreaAiBlue;
		if (action.action_type == A::AreaAiRed) subject = "the red team's soldiers in " + names.zone(action.param1);
		if (action.action_type == A::AreaAiBlue) subject = "the blue team's soldiers in " + names.zone(action.param1);
		// An area action naming a zone that does not resolve is dropped at the mission's start, and the load
		// writes the area's box over the action's third and fourth values, which the command then reads
		// [orig: EventTrigger_ResolveZoneActionRefs @0x453100, @0x4531B1 / @0x4531C6; section 7.3].
		if (area && !names.zone_resolves(action.param1))
			return "do nothing (the game drops this action: no area resolves zone " + number(action.param1) + ")";
		const auto area_word = [&](int slot) { return area && slot >= 2 ? std::string("(a value of the area's box)") : word(slot); };
		if (sub == 0) return "do nothing to " + subject + " (no AI change)";
		if (const AiRow *row = ai_row(sub)) return fill(row->words, false, subject, area_word);
		return "apply AI command " + number(sub) + " to " + subject + " (what it does is unknown)";
	}
	if (const ActionRow *row = action_row(type); row && *row->words) return fill(row->words, false, std::string(), word);
	return "an action of unknown type " + number(type) + " (does nothing)";
}

double logic_units_seconds(int64_t units) { return double(units) * bms::kEventStepTicks / io::kTickHz; }

namespace {

std::string seconds_words(int64_t units) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1f s", logic_units_seconds(units));
	return text;
}

} // namespace

std::string logic_steps_words(int64_t steps) {
	// The countdown is the steps << 6 in a word the game tests as signed after each 64-tick decrement:
	// past 512 steps it is negative after the first one, so it ends on the next pass [orig:
	// EventTrigger_UpdateEntry @0x454cef, @0x454d40; bms-event-runtime-re.md 1.2].
	if (bms::event_steps_wrap(steps)) return "about " + seconds_words(1) + " (past 512 steps the game's countdown wraps)";
	return seconds_words(steps);
}

EventWords event_words(const mission::EventChain &chain, const MissionNames &names, const bms::Header *header) {
	EventWords out;
	const uint32_t flags = uint32_t(chain.event.flags);
	const bool pre = (flags & uint32_t(bms::EventFlags::PreMission)) != 0;
	const bool post = (flags & uint32_t(bms::EventFlags::PostMission)) != 0;
	// The chain's fold: flat, left to right, each join the previous trigger's [orig:
	// EventTrigger_EvaluateChain @0x454050]: a join other than the one before it holds what came before
	// it together.
	// The berserk trigger answers a raw 0x200 the fold keeps bitwise [section 1.3]: an and of it with a true
	// trigger reads 0, an or else of it reads true whatever the other is (an or). `raw`: the fold so far may
	// hold that bit.
	const auto plain_berserk = [](const bms::Trigger &trigger) {
		return int32_t(trigger.main_type) == kPlayerType && trigger.sub_type == 18 && !trigger.is_negated();
	};
	std::string condition;
	LogicJoin last = LogicJoin::And;
	bool joined = false;
	bool raw = false;
	for (size_t i = 0; i < chain.triggers.size(); ++i) {
		const std::string words = trigger_words(chain.triggers[i], names);
		const bool berserk = plain_berserk(chain.triggers[i]);
		if (i == 0) {
			condition = words;
			raw = berserk;
			continue;
		}
		const LogicJoin join = trigger_join(chain.triggers[i - 1]);
		if (joined && join != last) condition = "(" + condition + ")";
		if (join == LogicJoin::Xor) {
			condition = "either " + condition + " or else " + words +
			            (raw || berserk ? " (in the game either or both: it reads berserk as a raw 0x200)" : " (not both)");
			raw = raw || berserk;
		} else {
			condition += std::string(" ") + logic_join_words(join) + " " + words;
			if (join == LogicJoin::And && raw != berserk)
				condition += " (never true in the game: it reads berserk as a raw 0x200, which an and with a true trigger clears)";
			raw = join == LogicJoin::And ? raw && berserk : raw || berserk;
		}
		last = join;
		joined = true;
	}
	// A start event is checked once by the PreMission pass, an end event once by the PostMission pass, and
	// the normal pass skips both [orig: EventTrigger_UpdateAllWithFlag2 @0x454dc0, UpdateAllWithFlag4
	// @0x454e00; the quarter pass @0x454d50 takes (flags & 6) == 0; bms-event-runtime-re.md 1.6]. One with
	// both bits is checked by both: at the end again only if its latch is clear (it did not fire at the
	// start, or it repeats with no wait, which clears the latch as it fires [1.2]).
	const bool repeats = (flags & uint32_t(bms::EventFlags::ResetAfter)) != 0;
	if (pre && post) {
		const bool again = repeats && chain.event.reset_after == 0;
		out.when = again ? "At the mission's start and again at its end"
		                 : "At the mission's start, and at its end if it did not fire then";
		if (!condition.empty()) out.when += ", if " + condition;
	} else if (pre) out.when = condition.empty() ? "At the mission's start" : "At the mission's start, if " + condition;
	else if (post) out.when = condition.empty() ? "At the mission's end" : "At the mission's end, if " + condition;
	else out.when = condition.empty() ? "Right away" : "When " + condition;
	for (size_t i = 0; i < chain.actions.size(); ++i)
		out.then += (i ? "; " : "") + action_words(chain.actions[i], names, header);
	if (out.then.empty()) out.then = "nothing";
	// The delay: the actions run its steps after the triggers hold [orig: @0x454c30, the reload armed from
	// +18]; past 512 steps the countdown wraps and ends on the next pass (logic_steps_words). A start or
	// end event is never processed again after its one check, so a delay there never ends: its actions
	// never run [1.2, 1.6].
	const bool once = pre || post;
	if (chain.event.delay > 0)
		out.delay = once ? std::string("never (a start or end event is checked once, so its wait never ends)")
		                 : "after " + logic_steps_words(chain.event.delay);
	// The repeat: the cooldown is armed in the call that latches, so it counts from when the triggers held;
	// its end clears the latch and the chain is checked on the pass after [orig: the cooldown armed from +14
	// when bit 0 is set, decremented with the delay; 0 clears the latch at once]. A start or end event's
	// cooldown is never counted down: it is never checked again by it.
	if (repeats && !once) {
		const int64_t reset = chain.event.reset_after;
		const bool wraps = bms::event_steps_wrap(reset);
		if (reset == 0) out.repeat = "Checked again each pass after its triggers held.";
		else
			out.repeat = "Checked again about " + seconds_words((wraps ? 1 : reset) + 1) + " after its triggers held" +
			             (wraps ? " (past 512 steps the game's countdown wraps)." : ".");
	}
	out.sentence = out.when + ", " + (out.delay.empty() ? std::string("then ") : "then, " + out.delay + ", ") + out.then + ".";
	if (!out.repeat.empty()) out.sentence += " " + out.repeat;
	return out;
}

// --- the types ------------------------------------------------------------------------------------

namespace {

std::vector<LogicType> make_types(bool actions) {
	std::vector<LogicType> out;
	if (!actions) {
		for (const TriggerRow &row : kTriggers) out.push_back({false, row.main, row.sub, row.title, row.group, row.tip});
	} else {
		// The AI commands' titles with their subject ("Group AI: Red alert"), kept for the process.
		static std::vector<std::unique_ptr<std::string>> titles;
		for (const ActionRow &row : kActions) {
			if (is_ai_action(row.type)) {
				// An AI action is offered by its command: one type per AI sub-type.
				for (const AiRow &ai : kAiCommands) {
					titles.push_back(std::make_unique<std::string>(std::string(ai_subject(row.type)) + ": " + ai.title));
					out.push_back({true, row.type, ai.sub, titles.back()->c_str(), kAiGroup, ai.tip});
				}
				continue;
			}
			if (has_sub_rows(row.type)) {
				for (const SubRow &sub : kActionSubs)
					if (sub.type == row.type) out.push_back({true, sub.type, sub.sub, sub.title, sub.group, sub.tip});
				continue;
			}
			out.push_back({true, row.type, 0, row.title, row.group, row.tip});
		}
	}
	// In their groups' order (the first row of each group sets its place), stable within one.
	std::vector<const char *> groups;
	for (const LogicType &type : out) {
		bool seen = false;
		for (const char *group : groups) seen = seen || group == type.group;
		if (!seen) groups.push_back(type.group);
	}
	std::vector<LogicType> sorted;
	sorted.reserve(out.size());
	for (const char *group : groups)
		for (const LogicType &type : out)
			if (type.group == group) sorted.push_back(type);
	return sorted;
}

} // namespace

const std::vector<LogicType> &logic_types(bool actions) {
	static const std::vector<LogicType> triggers = make_types(false), all_actions = make_types(true);
	return actions ? all_actions : triggers;
}

std::string team_words(int64_t team) {
	// By the colour the round's end names it [orig: Server_ProcessRoundEnd, RedWin 2, BlueWin 1, GreenWin 0;
	// docs/world/world-wac-ai-re.md 20.1; AreaAiRed team 2, AreaAiBlue team 1, Entity_KillTeamInBounds
	// @0x43D03B]; the original editor's words for them (neutral, good, evil) are its own.
	switch (team) {
	case 0: return "the green team (team 0)";
	case 1: return "the blue team (team 1)";
	case 2: return "the red team (team 2)";
	default: return "team " + number(team);
	}
}

const char *logic_action_title(int32_t type) {
	const ActionRow *row = action_row(type);
	return row ? row->title : nullptr;
}

const LogicType *logic_type(bool action, int32_t type, int32_t sub) {
	// A type whose case reads no sub-type is one type whatever its record holds there.
	if (!action && (type == kEvent || type == kSecond)) sub = 0;
	if (action && !is_ai_action(type) && !has_sub_rows(type)) sub = 0;
	for (const LogicType &row : logic_types(action))
		if (row.type == type && row.sub == sub) return &row;
	return nullptr;
}

} // namespace opennova::editor
