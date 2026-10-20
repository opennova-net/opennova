#include "mission_recipes.h"

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_uses.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

constexpr uint32_t kStart = uint32_t(bms::EventFlags::PreMission);
constexpr int32_t kGroup = int32_t(bms::TriggerMainType::Group);
constexpr int32_t kSingle = int32_t(bms::TriggerMainType::Single);
constexpr int32_t kEvent = int32_t(bms::TriggerMainType::Event);
constexpr int32_t kVariable = int32_t(bms::TriggerMainType::MissionVariable);

MissionRecipe recipe(const char *token, const char *title, const char *tip, ParamKind subject, int subject_slot,
                     bool has_trigger, int32_t type, int32_t sub, std::vector<RecipePreset> presets, uint32_t flags,
                     size_t events, size_t missions, const char *seen_in) {
	MissionRecipe out;
	out.token = token;
	out.title = title;
	out.tip = tip;
	out.subject = subject;
	out.subject_slot = subject_slot;
	out.has_trigger = has_trigger;
	out.type = type;
	out.sub = sub;
	out.presets = std::move(presets);
	out.flags = flags;
	out.events = events;
	out.missions = missions;
	out.seen_in = seen_in;
	return out;
}

// The shipped missions' compositions, the most used first: the events of the install's 115 missions holding
// exactly the recipe's trigger (or none) with its flags, and the missions holding them (measured once over them;
// the editor_mission_logic ctest's test_common_and_recipes pins these counts).
std::vector<MissionRecipe> make_recipes() {
	const ParamKind none = ParamKind::Unused;
	return {
		recipe("right_away", "Right away", "An event with no trigger: its actions run on the first check, after its "
		       "wait (a timer from the mission's start).", none, -1, false, 0, 0, {}, 0, 707, 97,
		       "00tra.bms, 00trb.bms, 00trc.bms"),
		recipe("destroyed", "When this is destroyed", "An organic, item or building of this SSN is no longer alive "
		       "(dead, removed, or never placed).", ParamKind::Entity, 0, true, kSingle, 4, {}, 0, 463, 36,
		       "00trb.bms, 00trc.bms, 00trd.bms"),
		recipe("group_at_stop", "When this group has passed a stop", "A unit of the group has passed a waypoint of a "
		       "path (kept until a Forget visited waypoints action).", ParamKind::Group, 0, true, kGroup, 7, {}, 0, 364, 33,
		       "00tra.bms, 00trb.bms, 00trc.bms"),
		recipe("group_in_area", "When a group is inside this area", "A unit of the group stands inside the area's box, "
		       "checked each time the event is.", ParamKind::Zone, 1, true, kGroup, 10, {}, 0, 242, 33,
		       "00tra.bms, 00trb.bms, 00trd.bms"),
		recipe("event_fired", "When this event has fired", "The event's actions have run and it has not been re-armed "
		       "since.", ParamKind::Event, 0, true, kEvent, 0, {}, 0, 159, 28, "00tre.bms, 00tri.bms, 01tr.bms"),
		recipe("group_lost", "When this group has lost units", "The group has lost at least this many of the units it "
		       "started with.", ParamKind::Group, 0, true, kGroup, 6, {}, 0, 139, 22,
		       "00trc.bms, 00tre.bms, 00trf.bms"),
		recipe("group_wiped_out", "When this group is wiped out", "No unit of the group is alive.", ParamKind::Group, 0,
		       true, kGroup, 4, {}, 0, 57, 19, "00trb.bms, 00trg.bms, 00trh.bms"),
		recipe("at_start", "At the mission's start", "Checked once, as the mission starts: its actions run then, its "
		       "wait never ends.", none, -1, false, 0, 0, {}, kStart, 35, 31, "00trb.bms, 00tre.bms, 00trf.bms"),
		recipe("variable_equals", "When a variable equals a number", "A mission variable equals a number.", none, -1, true,
		       kVariable, 1, {}, 0, 26, 5, "00tri.bms, 03tr.bms, 05tr.bms"),
		recipe("player_in_area", "When the player is inside this area", "The player stands inside the area's box, "
		       "checked each time the event is: a repeating event fires again while the player stays.",
		       ParamKind::Zone, 1, true, kSingle, 10, {{0, mission::kPlayerSsn}}, 0, 12, 2, "01tr.bms, 04tr.bms"),
	};
}

} // namespace

const std::vector<MissionRecipe> &mission_recipes() {
	static const std::vector<MissionRecipe> recipes = make_recipes();
	return recipes;
}

const MissionRecipe *mission_recipe(const std::string &token) {
	for (const MissionRecipe &recipe : mission_recipes())
		if (token == recipe.token) return &recipe;
	return nullptr;
}

std::vector<const MissionRecipe *> mission_recipes_for(ParamKind kind) {
	std::vector<const MissionRecipe *> out;
	for (const MissionRecipe &recipe : mission_recipes())
		if (recipe.subject == kind) out.push_back(&recipe);
	return out;
}

const LogicType *recipe_trigger_type(const MissionRecipe &recipe) {
	return recipe.has_trigger ? logic_type(false, recipe.type, recipe.sub) : nullptr;
}

bool recipe_matches(const MissionRecipe &recipe, const bms::Event &event, const std::vector<bms::Trigger> &triggers) {
	// The author's start and end bits as the recipe's; whether it repeats, and the format's internal bits, aside.
	constexpr uint32_t kPlaced = uint32_t(bms::EventFlags::PreMission) | uint32_t(bms::EventFlags::PostMission);
	if ((uint32_t(event.flags) & kPlaced) != recipe.flags) return false;
	if (!recipe.has_trigger) return triggers.empty();
	if (triggers.size() != 1) return false;
	const bms::Trigger &trigger = triggers[0];
	if (int32_t(trigger.main_type) != recipe.type || trigger.sub_type != recipe.sub || trigger.is_negated()) return false;
	const int32_t params[4] = {trigger.param1, trigger.param2, trigger.param3, trigger.param4};
	for (const RecipePreset &preset : recipe.presets)
		if (params[preset.slot] != preset.value) return false;
	return true;
}

bool recipe_event_edits(const MissionDocument &document, const MissionRecipe &recipe, const NodeAddress &subject,
                        std::vector<Edit> &out, std::string &error) {
	out.clear();
	std::vector<LogicPreset> presets;
	for (const RecipePreset &preset : recipe.presets) presets.push_back({preset.slot, preset.value});
	if (recipe.subject != ParamKind::Unused && subject.row) {
		ParamKind kind = ParamKind::Unused;
		int64_t value = 0;
		if (!mission_record_value(document, subject, kind, value) || kind != recipe.subject) {
			error = std::string("\"") + recipe.title + "\" is made over " + mission::param_kind_label(recipe.subject) +
			        ", which that record is not.";
			return false;
		}
		presets.push_back({recipe.subject_slot, value});
	}
	// The event at the end of the events, then its flags and its trigger, each naming what the batch made.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, node_kind(MissionKind::Event), 0};
	out.push_back(add);
	const NodeId event = batch_made(0);
	if (recipe.flags) {
		Edit flags;
		flags.address = {event, node_kind(MissionKind::Event), 0};
		flags.field = "flags";
		flags.value = int64_t(recipe.flags);
		out.push_back(flags);
	}
	if (const LogicType *type = recipe_trigger_type(recipe)) logic_record_edits(event, *type, SIZE_MAX, presets, out);
	return true;
}

} // namespace opennova::editor
