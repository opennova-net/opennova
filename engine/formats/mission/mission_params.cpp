#include <formats/mission/mission_params.h>

// The rows of mission_params.h, transcribed from docs/mission/bms-event-runtime-re.md: section 7.4
// (the triggers), 7.5 (the actions), 8.2 (the AI sub-types and the slot names dfx2med gives) and 8.4
// (the value domains).

#include <iterator>
#include <vector>

namespace opennova::mission {

namespace {

using K = ParamKind;
constexpr K U = K::Unused;

#define MISSION_CHOICES(rows) MissionChoices{rows, std::size(rows)}

// --- the sub-types by name ---------------------------------------------------------------------------

// [orig: EventTrigger_EvaluateCondition cat 1 @0x45364a; dfx2med Med_TriggerConditionName @0x446330]
constexpr MissionChoice kGroupSubTypes[] = {
	{"Null", 0},                    {"GroupSeesGroup", 1},     {"GroupHasTargetedGroup", 2},
	{"GroupAtRedAlert", 3},         {"GroupDestroyed", 4},     {"GroupAlive", 5},
	{"GroupHasLostMoreUnits", 6},   {"GroupAtWaypoint", 7},    {"GroupIntact", 9},
	{"GroupIsWithinArea", 10},      {"GroupHoldingGroup", 11}, {"GroupHasMoreUnits", 12},
	{"GroupHasShotGroup", 13},      {"GroupAtYellowAlert", 14}, {"GroupHasTargetedSingle", 15},
	{"GroupSeesSingle", 16},        {"GroupHasShotSingle", 17},
};
// [orig: EventTrigger_EvaluateCondition cat 2; subs 42..45 are positive tests the names negate
//  (section 8.2)]
constexpr MissionChoice kSingleSubTypes[] = {
	{"Null", 0},                     {"SingleSeesGroup", 1},     {"SingleHasTargetedGroup", 2},
	{"SingleAtRedAlert", 3},         {"SingleDestroyed", 4},     {"SingleAlive", 5},
	{"SingleHasLostMoreUnits", 6},   {"SingleAtWaypoint", 7},    {"SingleIntact", 9},
	{"SingleIsWithinArea", 10},      {"SingleHoldingGroup", 11}, {"SingleHasMoreUnits", 12},
	{"SingleHasShotGroup", 13},      {"SingleAtYellowAlert", 14}, {"SingleHasTargetedSingle", 15},
	{"SingleSeesSingle", 16},        {"SingleHasShotSingle", 17}, {"SingleOnTopOf", 42},
	{"SingleFartherThan", 43},       {"SingleHasNoLOS", 44},     {"SingleDoesNotSeeOrFarther", 45},
};
// [orig: EventTrigger_EvaluateCondition cat 4: 1 ==, 2 <, 3 >, 4 <=, 5 >=]
constexpr MissionChoice kMissionVariableSubTypes[] = {
	{"MissionVariableIsEqual", 1},              {"MissionVariableIsLessThan", 2},
	{"MissionVariableIsGreaterThan", 3},        {"MissionVariableIsLessThanOrEqual", 4},
	{"MissionVariableIsGreaterThanOrEqual", 5},
};
constexpr MissionChoice kTeammateSubTypes[] = {
	{"TeammateIsEnabled", 1}, {"TeammateMedicAssisting", 2}, {"TeammateEvacuating", 3},
};
// [orig: EventTrigger_EvaluateCondition cat 7 @0x453b7e; 31 is absent from its jump table]
constexpr MissionChoice kPlayerSubTypes[] = {
	{"PlayerBerserk", 18},           {"PlayerFirstPerson", 19},        {"PlayerThirdPerson", 20},
	{"PlayerCockpitView", 21},       {"PlayerInputBit10", 22},         {"PlayerInputBit11", 23},
	{"PlayerInputBit12", 24},        {"PlayerInputBit13", 25},         {"PlayerLookByteBit0Clear", 26},
	{"PlayerLookByteBit0Set", 27},   {"PlayerInputBit29", 28},         {"PlayerInputBit14", 29},
	{"PlayerInputBit15", 30},        {"PlayerInputBitIndex", 32},      {"PlayerInputBitIndexPlus15", 33},
	{"PlayerDialogDone", 34},        {"PlayerDialogFinished", 35},     {"PlayerAwol", 36},
	{"PlayerSatchel", 37},           {"PlayerAttachedToSsn", 38},      {"PlayerOnSsn", 39},
	{"PlayerDrivingSsn", 40},        {"PlayerOnGun", 41},
};
// A type whose sub-type selects nothing: the sub-type every shipped record of it holds.
constexpr MissionChoice kNullSubType[] = {{"Null", 0}};

// The AI sub-types, shared by ChangeGroupAI (3), AreaAiRed / AreaAiBlue (12, 13) and ChangeSingleAI
// (21) [orig: Entity_ApplyCommand @0x43ab60; dfx2med Med_ActionSubTypeName @0x445EE0].
constexpr MissionChoice kAiSubTypes[] = {
	{"GuardBit", 2},      {"RedAlert", 5},       {"GreenAlert", 6},           {"Accuracy", 8},
	{"BlindBit", 15},     {"BerserkBit", 16},    {"ClimberBit", 17},          {"CowardBit", 21},
	{"YellowAlert", 22},  {"DriveSkill", 26},    {"AimSkill", 27},            {"AiSetState", 28},
	{"CombatSpeed", 29},  {"PatrolSpeed", 30},   {"FindAndUse", 31},          {"AiUseWpz", 32},
	{"AiClearWpz", 33},   {"PlayPartAnim", 34},  {"HudItem", 37},             {"TmateStatus", 39},
	{"AiNodePathBit", 40}, {"AttackDistanceValue", 41}, {"EngageDistanceMin", 42}, {"IndestructableBit", 43},
	{"TargetSsn", 44},    {"StartFiringBit", 45}, {"FiringAngle", 46},
};
// [orig: EventAction_Dispatch case 5 @0x45437e]
constexpr MissionChoice kMissionVariableActionSubTypes[] = {
	{"Null", 0}, {"Set", 1}, {"Add", 2}, {"Subtract", 3}, {"Increment", 4}, {"Decrement", 5},
};
// [orig: EventAction_Dispatch case 39: 1 pickup, 2 flyover, 3 evacuate]
constexpr MissionChoice kTeammateActionSubTypes[] = {
	{"Null", 0}, {"MedicAssist", 1}, {"EvacuateTt", 2}, {"EvacuateAt", 3},
};
// The three sub-types SpecialSubType (28) acts on, by what each does: every other one returns
// [orig: EventAction_HandleSpecialTypes @0x4535a0: 37 the HUD item flash (the AI sub-type table's
// HUDITEM), 38 clears g_InputActionBits @0x4535c2, 39 stores (param1 == 0) @0x4535bc]. The original
// editor marks action 28 unused and names none of them (bms-event-runtime-re.md 8.2): HudItem is the
// AI table's token for the same arm, the other two are this table's own names for what they do.
constexpr MissionChoice kSpecialSubTypes[] = {{"Null", 0}, {"HudItem", 37}, {"ClearInputActions", 38}, {"StoreIsZero", 39}};

// Each list's values are bms.h's.
static_assert(kGroupSubTypes[9].value == int64_t(bms::GroupTriggerType::GroupIsWithinArea));
static_assert(kSingleSubTypes[std::size(kSingleSubTypes) - 1].value == int64_t(bms::SingleTriggerType::SingleDoesNotSeeOrFarther));
static_assert(kMissionVariableSubTypes[2].value == int64_t(bms::MissionVariableTriggerType::MissionVariableIsGreaterThan));
static_assert(kTeammateSubTypes[2].value == int64_t(bms::TeammateTriggerType::TeammateEvacuating));
static_assert(kPlayerSubTypes[18].value == int64_t(bms::PlayerTriggerType::PlayerSatchel) &&
              kPlayerSubTypes[std::size(kPlayerSubTypes) - 1].value == int64_t(bms::PlayerTriggerType::PlayerOnGun));
static_assert(kAiSubTypes[24].value == int64_t(bms::AIActionSubType::TargetSsn) &&
              kAiSubTypes[std::size(kAiSubTypes) - 1].value == int64_t(bms::AIActionSubType::FiringAngle));
static_assert(kMissionVariableActionSubTypes[5].value == int64_t(bms::MissionVariableActionSubType::Decrement));
static_assert(kTeammateActionSubTypes[3].value == int64_t(bms::TeammateActionSubType::EvacuateAt));

// --- the value domains (section 8.4) -------------------------------------------------------------------

// [orig editor: Med_ParamTeam @0x449a90: 0 NEUTRALTEAM, 1 GOODTEAM, 2 EVILTEAM]
constexpr MissionChoice kTeams[] = {{"Neutral", 0}, {"Good", 1}, {"Evil", 2}};
constexpr MissionChoice kBools[] = {{"Off", 0}, {"On", 1}};
// [orig editor: Med_ParamSubGoalWon @0x449710, Med_ParamSubGoalLost @0x449830: 1..8]
constexpr MissionChoice kSubGoals[] = {{"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}};

// --- the triggers (section 7.4) ----------------------------------------------------------------------

constexpr int32_t kGroup = int32_t(bms::TriggerMainType::Group), kSingle = int32_t(bms::TriggerMainType::Single),
                  kEvent = int32_t(bms::TriggerMainType::Event), kVariable = int32_t(bms::TriggerMainType::MissionVariable),
                  kSecond = int32_t(bms::TriggerMainType::SecondTimeThrough), kTeammate = int32_t(bms::TriggerMainType::Teammate),
                  kPlayer = int32_t(bms::TriggerMainType::Player);
constexpr const char *kOtherGroup = "Other group", *kOtherEntity = "Other entity", *kValue = "Value";

constexpr ParamRow kTriggerRows[] = {
	// Group: a sub-type its jump table lacks reads false, its parameters unread.
	{kGroup, kAnySubType, {U, U, U, U}, {}},
	{kGroup, 1, {K::Group, K::Group, U, U}, {nullptr, kOtherGroup}},
	{kGroup, 2, {K::Group, K::Group, U, U}, {nullptr, kOtherGroup}},
	{kGroup, 3, {K::Group, U, U, U}, {}},
	{kGroup, 4, {K::Group, U, U, U}, {}},
	{kGroup, 5, {K::Group, U, U, U}, {}},
	{kGroup, 6, {K::Group, K::Count, U, U}, {}},
	{kGroup, 7, {K::Group, K::Path, K::PathNode, U}, {}},
	{kGroup, 9, {K::Group, U, U, U}, {}},
	{kGroup, 10, {K::Group, K::Zone, U, U}, {}},
	{kGroup, 11, {K::Group, K::Group, U, U}, {nullptr, "Item group"}},
	{kGroup, 12, {K::Group, K::Count, U, U}, {}},
	{kGroup, 13, {K::Group, K::Group, U, U}, {nullptr, kOtherGroup}},
	{kGroup, 14, {K::Group, U, U, U}, {}},
	{kGroup, 15, {K::Group, K::Entity, U, U}, {}},
	{kGroup, 16, {K::Group, K::Entity, U, U}, {}},
	{kGroup, 17, {K::Group, K::Entity, U, U}, {}},
	// Single.
	{kSingle, kAnySubType, {U, U, U, U}, {}},
	{kSingle, 1, {K::Entity, K::Group, U, U}, {}},
	{kSingle, 2, {K::Entity, K::Group, U, U}, {}},
	{kSingle, 3, {K::Entity, U, U, U}, {}},
	{kSingle, 4, {K::Entity, U, U, U}, {}},
	{kSingle, 5, {K::Entity, U, U, U}, {}},
	{kSingle, 6, {K::Entity, K::Hp, U, U}, {nullptr, "Hit points lost"}},
	{kSingle, 7, {K::Entity, K::Path, K::PathNode, U}, {}},
	{kSingle, 9, {K::Entity, U, U, U}, {}},
	{kSingle, 10, {K::Entity, K::Zone, U, U}, {}},
	{kSingle, 11, {K::Entity, K::Group, U, U}, {nullptr, "Item group"}},
	{kSingle, 12, {K::Entity, K::Hp, U, U}, {}},
	{kSingle, 13, {K::Entity, K::Group, U, U}, {}},
	{kSingle, 14, {K::Entity, U, U, U}, {}},
	{kSingle, 15, {K::Entity, K::Entity, U, U}, {nullptr, kOtherEntity}},
	{kSingle, 16, {K::Entity, K::Entity, U, U}, {nullptr, kOtherEntity}},
	{kSingle, 17, {K::Entity, K::Entity, U, U}, {nullptr, kOtherEntity}},
	{kSingle, 42, {K::Entity, K::Entity, U, U}, {nullptr, kOtherEntity}},
	{kSingle, 43, {K::Entity, K::Entity, K::DistanceM, U}, {nullptr, kOtherEntity}},
	{kSingle, 44, {K::Entity, K::Entity, K::DistanceM, U}, {nullptr, kOtherEntity}},
	{kSingle, 45, {K::Entity, K::Entity, K::DistanceM, U}, {nullptr, kOtherEntity}},
	// Event: events[param1]'s latch window, whatever the sub-type.
	{kEvent, kAnySubType, {K::Event, U, U, U}, {}},
	// MissionVariable: dword_C6B240[param1] <op> param2; a sub-type past 1..5 reads false.
	{kVariable, kAnySubType, {U, U, U, U}, {}},
	{kVariable, 1, {K::MissionVar, K::Raw, U, U}, {nullptr, kValue}},
	{kVariable, 2, {K::MissionVar, K::Raw, U, U}, {nullptr, kValue}},
	{kVariable, 3, {K::MissionVar, K::Raw, U, U}, {nullptr, kValue}},
	{kVariable, 4, {K::MissionVar, K::Raw, U, U}, {nullptr, kValue}},
	{kVariable, 5, {K::MissionVar, K::Raw, U, U}, {nullptr, kValue}},
	// SecondTimeThrough and Teammate read no parameter.
	{kSecond, kAnySubType, {U, U, U, U}, {}},
	{kTeammate, kAnySubType, {U, U, U, U}, {}},
	// Player: the view, input-bit and look-byte tests read none.
	{kPlayer, kAnySubType, {U, U, U, U}, {}},
	{kPlayer, 32, {K::Bit, U, U, U}, {}},
	{kPlayer, 33, {K::Bit, U, U, U}, {}},
	{kPlayer, 34, {K::Dialog, U, U, U}, {}},
	{kPlayer, 35, {K::Dialog, U, U, U}, {}},
	{kPlayer, 36, {K::Seconds, U, U, U}, {"Seconds outside the mission area"}},
	{kPlayer, 37, {K::Zone, U, U, U}, {}},
	{kPlayer, 38, {K::Entity, U, U, U}, {}},
	{kPlayer, 39, {K::Entity, U, U, U}, {}},
	{kPlayer, 40, {K::Entity, U, U, U}, {}},
	{kPlayer, 41, {K::Entity, U, U, U}, {}},
};

// --- the actions (section 7.5) -----------------------------------------------------------------------

constexpr int32_t a(bms::ActionType type) { return int32_t(type); }
using A = bms::ActionType;

// The rows of the action types whose parameters their type alone decides.
constexpr ParamRow kActionRows[] = {
	{a(A::Null), kAnySubType, {U, U, U, U}, {}},
	// The waypoint list's commands 123..125 make the third parameter an entity (action_param_kind).
	{a(A::RedirectGroupTo), kAnySubType, {K::Group, K::Path, K::PathNode, U}, {}},
	{a(A::KillGroup), kAnySubType, {K::Group, U, U, U}, {}},
	{a(A::VaporizeGroup), kAnySubType, {K::Group, U, U, U}, {}},
	{a(A::MisvarChange), kAnySubType, {K::MissionVar, K::Raw, U, U}, {nullptr, kValue}},
	// Increment and Decrement add and take one: the second parameter is unread [orig: @0x454394..0x454406].
	{a(A::MisvarChange), 4, {K::MissionVar, U, U, U}, {}},
	{a(A::MisvarChange), 5, {K::MissionVar, U, U, U}, {}},
	{a(A::OutputText), kAnySubType, {K::Raw, U, U, U}, {"Text"}},
	// The second: 1 plays it after the round-over latch too [orig: @0x45444A].
	{a(A::PlayWavList), kAnySubType, {K::Dialog, K::Bool, U, U}, {nullptr, "Always"}},
	{a(A::BlueWin), kAnySubType, {U, U, U, U}, {}},
	{a(A::RedWin), kAnySubType, {U, U, U, U}, {}},
	{a(A::GreenWin), kAnySubType, {U, U, U, U}, {}},
	{a(A::GroupVelocity), kAnySubType, {K::Group, K::SpeedKph, U, U}, {}},
	{a(A::SubGoalWon), kAnySubType, {K::SubGoal, U, U, U}, {}},
	{a(A::SubGoalLost), kAnySubType, {K::SubGoal, U, U, U}, {}},
	{a(A::ChangeGTeamAction), kAnySubType, {K::Group, K::Team, U, U}, {}},
	{a(A::ChangeGroupAction), kAnySubType, {K::Group, K::Group, U, U}, {nullptr, "New group"}},
	{a(A::GroupTeleportAction), kAnySubType, {K::Group, K::TeleportTarget, U, U}, {}},
	{a(A::RedirectSingleTo), kAnySubType, {K::Entity, K::Path, K::PathNode, U}, {}},
	{a(A::KillSingle), kAnySubType, {K::Entity, U, U, U}, {}},
	{a(A::VaporizeSingle), kAnySubType, {K::Entity, U, U, U}, {}},
	// A witnessed no-op [orig: EventAction_Dispatch case 23 -> sub_43DEA0 mutates nothing].
	{a(A::SingleVelocity), kAnySubType, {K::Entity, K::SpeedKph, U, U}, {}},
	{a(A::ChangeSteamAction), kAnySubType, {K::Entity, K::Team, U, U}, {}},
	{a(A::SingleChangeGroup), kAnySubType, {K::Entity, K::Group, U, U}, {nullptr, "New group"}},
	{a(A::SingleTeleportAction), kAnySubType, {K::Entity, K::TeleportTarget, U, U}, {}},
	{a(A::ParticleEffectAction), kAnySubType, {K::WpNumber, U, U, U}, {}},
	// SpecialSubType: three sub-types act, every other returns.
	{a(A::SpecialSubType), kAnySubType, {U, U, U, U}, {}},
	{a(A::SpecialSubType), 37, {K::HudTimer, K::Raw, U, U}, {nullptr, "Ticks"}},
	{a(A::SpecialSubType), 39, {K::Raw, U, U, U}, {kValue}},
	{a(A::GroupOpenDoorAction), kAnySubType, {K::Group, U, U, U}, {}},
	{a(A::GroupCloseDoorAction), kAnySubType, {K::Group, U, U, U}, {}},
	{a(A::GroupResetHasVisited), kAnySubType, {K::Group, U, U, U}, {}},
	{a(A::SingleResetHasVisited), kAnySubType, {K::Entity, U, U, U}, {}},
	{a(A::ResetEvent), kAnySubType, {K::Event, U, U, U}, {}},
	{a(A::ShowWinSubgoal), kAnySubType, {K::SubGoal, K::Bool, U, U}, {nullptr, "Show"}},
	{a(A::ShowLoseSubgoal), kAnySubType, {K::SubGoal, K::Bool, U, U}, {nullptr, "Show"}},
	{a(A::AttachToEmplaced), kAnySubType, {K::Entity, U, U, U}, {}},
	{a(A::SetLightState), kAnySubType, {K::LightChannel, K::Bool, U, U}, {}},
	// The medevac (1) and the flyover (2) take the patient by its SSN (the first pool-0 row of it) and
	// the destination by the wp_number of a pool-3 type-6088 marker; any other sub-type has no arm and
	// reads nothing [orig: EventAction_Dispatch case 39 @0x4549DC..0x454A15 -> HeliLift_SpawnPickup
	// @0x4525E0, HeliLift_SpawnFlyover @0x452730; docs/world/world-wac-ai-re.md 33.32;
	// runtime/world/teammate_operations.cpp TeammateOperations::start].
	{a(A::Teammates), kAnySubType, {U, U, U, U}, {}},
	{a(A::Teammates), 1, {K::Entity, K::WpNumber, U, U}, {"Patient", "Marker number"}},
	{a(A::Teammates), 2, {K::Entity, K::WpNumber, U, U}, {"Patient", "Marker number"}},
	{a(A::ShowWaypoints), kAnySubType, {K::Bool, U, U, U}, {"Show"}},
	// The dispatcher has no case 41: the default returns, reading none of its parameters [orig:
	// EventAction_Dispatch @0x4542E0; runtime/mission/event_runtime.cpp].
	{a(A::ExecuteWac), kAnySubType, {U, U, U, U}, {}},
	{a(A::SsnTargetSsnPri), kAnySubType, {K::Entity, K::Entity, U, U}, {nullptr, "Target entity"}},
	{a(A::SsnTargetSsnExc), kAnySubType, {K::Entity, K::Entity, U, U}, {nullptr, "Target entity"}},
	{a(A::SsnTargetGroupPri), kAnySubType, {K::Entity, K::Group, U, U}, {nullptr, "Target group"}},
	{a(A::SsnTargetGroupExc), kAnySubType, {K::Entity, K::Group, U, U}, {nullptr, "Target group"}},
	{a(A::GroupTargetSsnPri), kAnySubType, {K::Group, K::Entity, U, U}, {nullptr, "Target entity"}},
	{a(A::GroupTargetSsnExc), kAnySubType, {K::Group, K::Entity, U, U}, {nullptr, "Target entity"}},
	{a(A::GroupTargetGroupPri), kAnySubType, {K::Group, K::Group, U, U}, {nullptr, "Target group"}},
	{a(A::GroupTargetGroupExc), kAnySubType, {K::Group, K::Group, U, U}, {nullptr, "Target group"}},
};

// An AI sub-type's parameters, the action's second to fourth [orig: Entity_ApplyCommand @0x43ab60;
// slot names dfx2med Med_AiSubTypeParams @0x44a920]. A sub-type with no row here (the arms with no
// editor token, an unknown one) is Raw.
struct AiSubRow {
	int32_t sub;
	ParamKind p[3];
	const char *label[3];
};
constexpr AiSubRow kAiSubRows[] = {
	{2, {K::Bool, U, U}, {}},
	// The alerts store a fixed level: no parameter is read [orig: @0x43ac2d, @0x43acfd, @0x43ac8d].
	{5, {U, U, U}, {}},
	{6, {U, U, U}, {}},
	{8, {K::Raw, U, U}, {"Accuracy"}},
	{15, {K::Bool, U, U}, {}},
	{16, {K::Bool, U, U}, {}},
	{17, {K::Bool, U, U}, {}},
	{21, {K::Bool, U, U}, {}},
	{22, {U, U, U}, {}},
	{26, {K::Raw, U, U}, {"Skill"}},
	{27, {K::Raw, U, U}, {"Skill"}},
	{28, {K::Raw, U, U}, {"State"}},
	{29, {K::SpeedKph, U, U}, {}},
	{30, {K::SpeedKph, U, U}, {}},
	// The pool-1 row of that SSN to attach to, 0 clearing it [orig: Entity_FindAttachBone @0x4B9580].
	{31, {K::Entity, U, U}, {}},
	// Each stores a constant [orig: @0x43B164, @0x43B183].
	{32, {U, U, U}, {}},
	{33, {U, U, U}, {}},
	{34, {K::Raw, K::Raw, K::Raw}, {"Anim number", "Anim play type", "Anim time"}},
	// No arm in this binary [orig: Entity_ApplyCommand @0x43AB60].
	{37, {U, U, U}, {}},
	{39, {U, U, U}, {}},
	{40, {K::Bool, U, U}, {}},
	{41, {K::DistanceM, U, U}, {}},
	{42, {K::DistanceM, K::DistanceM, U}, {"Minimum distance", "Maximum distance"}},
	{43, {K::Bool, U, U}, {}},
	{44, {K::Entity, U, U}, {"Target entity"}},
	{45, {K::Bool, U, U}, {}},
	{46, {K::Raw, U, U}, {"Angle"}},
};

// The action types an AI sub-type selects the command of, and what their first parameter is. An area
// action's third and fourth parameters are overwritten at mission start with the zone's corners
// [orig: EventTrigger_ResolveZoneActionRefs @0x453100], so the file's are never read.
struct AiActionRow {
	int32_t type;
	ParamKind target;
	bool area;
};
constexpr AiActionRow kAiActions[] = {
	{a(A::ChangeGroupAI), K::Group, false},
	{a(A::AreaAiRed), K::Zone, true},
	{a(A::AreaAiBlue), K::Zone, true},
	{a(A::ChangeSingleAI), K::Entity, false},
};

// Every action row: kActionRows, then for each AI action type its row for any sub-type and one per
// AI sub-type.
const std::vector<ParamRow> &action_rows() {
	static const std::vector<ParamRow> rows = [] {
		std::vector<ParamRow> out(std::begin(kActionRows), std::end(kActionRows));
		for (const AiActionRow &action : kAiActions) {
			const ParamKind tail = action.area ? U : K::Raw;
			out.push_back({action.type, kAnySubType, {action.target, K::Raw, tail, tail}, {nullptr, kValue}});
			for (const AiSubRow &sub : kAiSubRows) {
				ParamRow row{action.type, sub.sub, {action.target, sub.p[0], sub.p[1], sub.p[2]},
				             {nullptr, sub.label[0], sub.label[1], sub.label[2]}};
				if (action.area) row.p[2] = row.p[3] = U;
				out.push_back(row);
			}
		}
		return out;
	}();
	return rows;
}

template <class Rows> const ParamRow *find_row(const Rows &rows, int32_t main, int32_t sub) {
	const ParamRow *any = nullptr;
	for (const ParamRow &row : rows) {
		if (row.main != main) continue;
		if (row.sub == sub) return &row;
		if (row.sub == kAnySubType) any = &row;
	}
	return any;
}

bool is_redirect(const bms::Action &action) {
	return action.action_type == A::RedirectGroupTo || action.action_type == A::RedirectSingleTo;
}

} // namespace

const ParamRow *trigger_params(int32_t main_type, int32_t sub_type) {
	return find_row(kTriggerRows, main_type, sub_type);
}

const ParamRow *action_params(int32_t action_type, int32_t sub_type) {
	return find_row(action_rows(), action_type, sub_type);
}

ParamKind trigger_param_kind(const bms::Trigger &trigger, int slot) {
	if (slot < 0 || slot > 3) return ParamKind::Unused;
	const ParamRow *row = trigger_params(int32_t(trigger.main_type), trigger.sub_type);
	return row ? row->p[slot] : ParamKind::Raw;
}

ParamKind action_param_kind(const bms::Action &action, int slot) {
	if (slot < 0 || slot > 3) return ParamKind::Unused;
	const ParamRow *row = action_params(int32_t(action.action_type), action.action_sub_type);
	if (!row) return ParamKind::Raw;
	if (slot == 2 && is_redirect(action) && path_command_names_entity(action.param2)) return ParamKind::Entity;
	return row->p[slot];
}

const char *trigger_param_label(const bms::Trigger &trigger, int slot) {
	if (slot < 0 || slot > 3) return "";
	const ParamRow *row = trigger_params(int32_t(trigger.main_type), trigger.sub_type);
	if (row && row->label[slot]) return row->label[slot];
	return param_kind_label(trigger_param_kind(trigger, slot));
}

const char *action_param_label(const bms::Action &action, int slot) {
	if (slot < 0 || slot > 3) return "";
	const ParamRow *row = action_params(int32_t(action.action_type), action.action_sub_type);
	const ParamKind kind = action_param_kind(action, slot);
	if (row && row->label[slot] && kind == row->p[slot]) return row->label[slot];
	return param_kind_label(kind);
}

const char *param_kind_label(ParamKind kind) {
	switch (kind) {
	case K::Unused: return "";
	case K::Group: return "Group";
	case K::Entity: return "Entity";
	case K::Zone: return "Zone";
	case K::Event: return "Event";
	case K::Path: return "Waypoint list";
	case K::PathNode: return "Waypoint number";
	case K::MissionVar: return "Variable";
	case K::Dialog: return "Dialog";
	case K::Count: return "Units";
	case K::Hp: return "Hit points";
	case K::DistanceM: return "Distance";
	case K::Seconds: return "Seconds";
	case K::SpeedKph: return "Speed";
	case K::Bool: return "On";
	case K::Team: return "Team";
	case K::SubGoal: return "Sub-goal";
	case K::Bit: return "Input bit";
	case K::HudTimer: return "HUD timer";
	case K::LightChannel: return "Light channel";
	case K::TeleportTarget: return "Teleport target";
	case K::WpNumber: return "Marker number";
	case K::Raw: return "Value";
	}
	return "";
}

MissionChoices param_choices(ParamKind kind) {
	switch (kind) {
	case K::Team: return MISSION_CHOICES(kTeams);
	case K::Bool: return MISSION_CHOICES(kBools);
	case K::SubGoal: return MISSION_CHOICES(kSubGoals);
	default: return {};
	}
}

MissionChoices trigger_sub_types(int32_t main_type) {
	switch (static_cast<bms::TriggerMainType>(main_type)) {
	case bms::TriggerMainType::Group: return MISSION_CHOICES(kGroupSubTypes);
	case bms::TriggerMainType::Single: return MISSION_CHOICES(kSingleSubTypes);
	case bms::TriggerMainType::MissionVariable: return MISSION_CHOICES(kMissionVariableSubTypes);
	case bms::TriggerMainType::Teammate: return MISSION_CHOICES(kTeammateSubTypes);
	case bms::TriggerMainType::Player: return MISSION_CHOICES(kPlayerSubTypes);
	case bms::TriggerMainType::Event:
	case bms::TriggerMainType::SecondTimeThrough: return MISSION_CHOICES(kNullSubType);
	}
	return {};
}

bool trigger_reads_sub_type(int32_t main_type) {
	switch (static_cast<bms::TriggerMainType>(main_type)) {
	case bms::TriggerMainType::Group:
	case bms::TriggerMainType::Single:
	case bms::TriggerMainType::MissionVariable:
	case bms::TriggerMainType::Teammate:
	case bms::TriggerMainType::Player: return true;
	// The event's latch whatever the sub-type [orig: EventTrigger_EvaluateCondition cat 3 @0x453a75];
	// the load parity, returned raw [orig: cat 5 @0x453B24].
	case bms::TriggerMainType::Event:
	case bms::TriggerMainType::SecondTimeThrough: return false;
	}
	return false;
}

MissionChoices action_sub_types(int32_t action_type) {
	switch (static_cast<bms::ActionType>(action_type)) {
	case A::ChangeGroupAI:
	case A::AreaAiRed:
	case A::AreaAiBlue:
	case A::ChangeSingleAI: return MISSION_CHOICES(kAiSubTypes);
	case A::MisvarChange: return MISSION_CHOICES(kMissionVariableActionSubTypes);
	case A::SpecialSubType: return MISSION_CHOICES(kSpecialSubTypes);
	case A::Teammates: return MISSION_CHOICES(kTeammateActionSubTypes);
	default: return MISSION_CHOICES(kNullSubType);
	}
}

#undef MISSION_CHOICES

} // namespace opennova::mission
