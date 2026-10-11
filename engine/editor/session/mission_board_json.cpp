#include <editor/session/mission_board_json.h>

#include <editor/documents/mission_document.h>
#include <editor/session/mission_logic_json.h>
#include <editor/session/mission_queries.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

using io::json_number;
using io::json_string;
using io::JsonValue;

namespace {

JsonValue boolean(bool value) { return JsonValue::make_bool(value); }
JsonValue number(double value) { return json_number(value); }
JsonValue identity(const NodeAddress &address) { return number(double(address.child ? address.child : address.row)); }

JsonValue chip_to_json(const BoardChip &chip) {
	JsonValue out = JsonValue::make_object();
	out.set("slot", number(chip.slot));
	out.set("field", json_string(chip.field));
	out.set("label", json_string(chip.label));
	out.set("words", json_string(chip.words));
	out.set("value", number(double(chip.value)));
	out.set("kind", json_string(param_kind_token(chip.kind)));
	if (*chip.unit) out.set("unit", json_string(chip.unit));
	out.set("picks", boolean(chip.picks));
	out.set("unset", boolean(chip.unset));
	out.set("missing", boolean(chip.missing));
	return out;
}

JsonValue line_to_json(const BoardLine &line) {
	JsonValue out = JsonValue::make_object();
	out.set("record", identity(line.record));
	out.set("index", number(double(line.index)));
	out.set("action", boolean(line.action));
	if (line.type) {
		JsonValue type = JsonValue::make_object();
		type.set("type", number(line.type->type));
		type.set("sub", number(line.type->sub));
		type.set("title", json_string(line.type->title));
		type.set("group", json_string(line.type->group));
		out.set("type", std::move(type));
	} else {
		out.set("type", JsonValue::make_null());
	}
	out.set("type_words", json_string(line.type_words));
	if (!line.action) {
		out.set("negated", boolean(line.negated));
		out.set("join", json_string(logic_join_token(line.join)));
	}
	out.set("last", boolean(line.last));
	out.set("words", json_string(logic_runs_text(line.runs)));
	JsonValue runs = JsonValue::make_array();
	for (const BoardRun &run : line.runs) {
		JsonValue item = JsonValue::make_object();
		item.set("text", json_string(run.text));
		item.set("slot", number(run.slot));
		runs.push(std::move(item));
	}
	out.set("runs", std::move(runs));
	JsonValue chips = JsonValue::make_array();
	for (const BoardChip &chip : line.chips) chips.push(chip_to_json(chip));
	out.set("chips", std::move(chips));
	return out;
}

} // namespace

JsonValue board_card_to_json(const BoardCard &card) {
	JsonValue out = JsonValue::make_object();
	out.set("event", identity(card.event));
	out.set("index", number(double(card.index)));
	out.set("title", json_string(card.title));
	out.set("sentence", json_string(card.words.sentence));
	out.set("when", json_string(card.words.when));
	out.set("then", json_string(card.words.then));
	out.set("delay", json_string(card.words.delay));
	out.set("repeat", json_string(card.words.repeat));
	out.set("repeats", boolean(card.repeats));
	out.set("at_start", boolean(card.at_start));
	out.set("at_end", boolean(card.at_end));
	out.set("delay_steps", number(double(card.delay_steps)));
	out.set("repeat_steps", number(double(card.repeat_steps)));
	out.set("delay_seconds", number(logic_units_seconds(card.delay_steps)));
	out.set("repeat_seconds", number(logic_units_seconds(card.repeat_steps)));
	out.set("folded", boolean(card.folded));
	out.set("trigger_refusal", json_string(card.trigger_refusal));
	out.set("action_refusal", json_string(card.action_refusal));
	for (const bool actions : {false, true}) {
		JsonValue lines = JsonValue::make_array();
		for (const BoardLine &line : actions ? card.actions : card.triggers) lines.push(line_to_json(line));
		out.set(actions ? "actions" : "triggers", std::move(lines));
	}
	JsonValue notes = JsonValue::make_array();
	for (const BoardNote &note : card.notes) {
		JsonValue item = JsonValue::make_object();
		item.set("record", identity(note.record));
		item.set("code", json_string(note.code));
		item.set("words", json_string(note.words));
		notes.push(std::move(item));
	}
	out.set("notes", std::move(notes));
	if (card.live) {
		JsonValue live = JsonValue::make_object();
		live.set("active", boolean(card.live->active));
		live.set("fired", boolean(card.live->fired));
		live.set("activate_countdown", number(double(card.live->activate_countdown)));
		live.set("repeat_countdown", number(double(card.live->repeat_countdown)));
		JsonValue truth = JsonValue::make_array();
		for (const int8_t held : card.live->truth) truth.push(number(held));
		live.set("truth", std::move(truth));
		live.set("last_checked_tick", number(double(card.live->last_checked_tick)));
		out.set("live", std::move(live));
	}
	return out;
}

JsonValue mission_recipe_to_json(const MissionRecipe &recipe) {
	JsonValue out = JsonValue::make_object();
	out.set("token", json_string(recipe.token));
	out.set("title", json_string(recipe.title));
	out.set("tip", json_string(recipe.tip));
	out.set("subject", json_string(recipe.subject == mission::ParamKind::Unused ? "" : param_kind_token(recipe.subject)));
	if (const LogicType *type = recipe_trigger_type(recipe)) {
		JsonValue trigger = JsonValue::make_object();
		trigger.set("type", number(type->type));
		trigger.set("sub", number(type->sub));
		trigger.set("title", json_string(type->title));
		out.set("trigger", std::move(trigger));
	} else {
		out.set("trigger", JsonValue::make_null());
	}
	out.set("start", boolean((recipe.flags & uint32_t(bms::EventFlags::PreMission)) != 0));
	out.set("events", number(double(recipe.events)));
	out.set("missions", number(double(recipe.missions)));
	out.set("seen_in", json_string(recipe.seen_in));
	return out;
}

JsonValue answer_mission_board(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = mission_of(context, args, error);
	if (!mission) return JsonValue::make_null();
	const SessionView &view = context.core.view();
	// The Logic pane's filter, folds and Needs attention, unless the args name their own.
	const WorkspaceView::DocumentView &shown = view.workspace.document(mission->path());
	BoardQuery query;
	query.filter = args.has("filter") ? args.text("filter") : shown.logic_filter;
	query.attention = args.has("attention") ? args.boolean("attention") : shown.logic_attention;
	if (args.has("folded")) {
		for (const int64_t id : args.integers("folded")) query.folded.push_back(NodeId(id));
	} else {
		query.folded = shown.logic_folded;
	}
	const LiveLogic *live = view.activity.live_logic.empty() ? nullptr : &view.activity.live_logic;
	const MissionBoard board = mission_board(*mission, *MissionWords(context, *mission), query, live);
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(mission->path()));
	out.set("events", number(double(board.events)));
	out.set("noted", number(double(board.noted)));
	out.set("filter", json_string(query.filter));
	out.set("attention", boolean(query.attention));
	set_page(out, page, board.cards.size());
	JsonValue cards = JsonValue::make_array();
	for (size_t i = page.first(board.cards.size()); i < page.last(board.cards.size()); ++i)
		cards.push(board_card_to_json(board.cards[i]));
	out.set("cards", std::move(cards));
	// What the type picker offers first, and what a new event is made from.
	for (const bool actions : {false, true}) {
		JsonValue common = JsonValue::make_array();
		for (const LogicType *type : logic_common_types(actions)) common.push(logic_type_to_json(*type));
		out.set(actions ? "common_actions" : "common_triggers", std::move(common));
	}
	JsonValue recipes = JsonValue::make_array();
	for (const MissionRecipe &recipe : mission_recipes()) recipes.push(mission_recipe_to_json(recipe));
	out.set("recipes", std::move(recipes));
	return out;
}

} // namespace opennova::editor
