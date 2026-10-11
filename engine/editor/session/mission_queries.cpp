#include <editor/session/mission_queries.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_uses.h>
#include <editor/documents/mission_variable_names.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/mission_board_json.h>
#include <editor/session/mission_logic_json.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// The mission and the record an op names by `id`, or false with `error`.
bool mission_record(const QueryContext &context, const QueryArgs &args, const MissionDocument *&mission,
                    NodeAddress &address, std::string &error) {
	const std::string op = args.text("op");
	mission = mission_of(context, args, error);
	if (!mission) return false;
	if (!args.has("id")) {
		error = "op " + op + " needs \"id\", the record by its identity.";
		return false;
	}
	return record_of(*mission, args, address, error);
}

// --- the ops ------------------------------------------------------------------------------------------

JsonValue answer_types(const QueryContext &, const QueryArgs &args, std::string &error) {
	if (!args.has("list")) {
		error = "op types needs \"list\": trigger or action.";
		return JsonValue::make_null();
	}
	return logic_types_to_json(args.text("list") == "action", page_of(args));
}

JsonValue answer_form(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = nullptr;
	NodeAddress address;
	if (!mission_record(context, args, mission, address, error)) return JsonValue::make_null();
	const MissionWords names(context, *mission);
	if (!address.child && address.kind == node_kind(MissionKind::Event)) {
		LogicEventForm form;
		logic_event_form(*mission, address.row, *names, form);
		return logic_event_form_to_json(form);
	}
	LogicForm form;
	if (!logic_form(*mission, address, *names, form)) {
		error = "record " + std::to_string(identity_of(address)) + " is no event, trigger or action.";
		return JsonValue::make_null();
	}
	return logic_form_to_json(form);
}

// add and retype: a record of a type the types list offers.
JsonValue answer_typed(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = nullptr;
	NodeAddress address;
	if (!mission_record(context, args, mission, address, error)) return JsonValue::make_null();
	const std::string op = args.text("op");
	const bool add = op == "add";
	if (!args.has("type") || (add && !args.has("list"))) {
		error = "op " + op + " needs \"type\"" + (add ? std::string(" and \"list\"") : std::string()) + ".";
		return JsonValue::make_null();
	}
	const bool actions = add ? args.text("list") == "action" : address.kind == node_kind(MissionKind::Action);
	const LogicType *type = logic_type(actions, int32_t(args.integer("type")), int32_t(args.integer("sub")));
	if (!type) {
		error = "no " + std::string(actions ? "action" : "trigger") + " type " + std::to_string(args.integer("type")) +
		        "/" + std::to_string(args.integer("sub")) + " is offered (op types lists them).";
		return JsonValue::make_null();
	}
	const size_t position = args.has("position") ? size_t(args.integer("position")) : SIZE_MAX;
	std::vector<Edit> edits;
	std::string refusal;
	const bool ok = add ? logic_add_edits(*mission, address.row, *type, position, edits, refusal)
	                    : logic_retype_edits(*mission, address, *type, edits, refusal);
	return planned_edits(*mission, ok, edits, refusal);
}

JsonValue answer_move(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = nullptr;
	NodeAddress address;
	if (!mission_record(context, args, mission, address, error)) return JsonValue::make_null();
	if (!args.has("to")) {
		error = "op move needs \"to\", the event by its identity.";
		return JsonValue::make_null();
	}
	const size_t position = args.has("position") ? size_t(args.integer("position")) : SIZE_MAX;
	std::vector<Edit> edits;
	std::string refusal;
	const bool ok = logic_move_edits(*mission, address, NodeId(args.integer("to")), position, edits, refusal);
	return planned_edits(*mission, ok, edits, refusal);
}

// negate and join: one edit of a trigger's flags.
JsonValue answer_flag(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = nullptr;
	NodeAddress address;
	if (!mission_record(context, args, mission, address, error)) return JsonValue::make_null();
	const bool negate = args.text("op") == "negate";
	LogicJoin join = LogicJoin::And;
	if (!negate && !logic_join_from_token(args.text("join"), join)) {
		error = "op join needs \"join\": and, or or or_else.";
		return JsonValue::make_null();
	}
	Edit edit;
	const bool ok = negate ? logic_negate_edit(*mission, address, args.boolean("negated"), edit)
	                       : logic_join_edit(*mission, address, join, edit);
	std::vector<Edit> edits;
	if (ok) edits.push_back(edit);
	return planned_edits(*mission, ok, edits, "That is no trigger of the mission.");
}

// variables: the names the mission gives its variables, and with `variable` the edits naming one.
JsonValue answer_variables(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = mission_of(context, args, error);
	if (!mission) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	if (args.has("variable")) {
		std::vector<Edit> edits;
		std::string refusal;
		const bool ok = mission_variable_name_edits(*mission, args.integer("variable"), args.text("name"), edits, refusal);
		out = planned_edits(*mission, ok, edits, refusal);
	}
	JsonValue names = JsonValue::make_array();
	for (const mission::VariableName &name : mission_variable_names(*mission)) {
		JsonValue entry = JsonValue::make_object();
		entry.set("index", json_number(double(name.index)));
		entry.set("name", json_string(name.name));
		names.push(std::move(entry));
	}
	out.set("names", std::move(names));
	const mission::LayerTable table = mission_variable_table(*mission);
	out.set("table", json_string(table == mission::LayerTable::Empty           ? "empty"
	                             : table == mission::LayerTable::VariableNames ? "names"
	                                                                           : "layer_names"));
	out.set("left", json_number(double(mission_variable_names_left(*mission))));
	return out;
}

// The ops, in the op param's order. S24 lanes append one row each here: the board's (lane A), the live
// logic's (lane C).
constexpr MissionLogicOp kMissionLogicOps[] = {
	{ "form", answer_form },
	{ "types", answer_types },
	{ "add", answer_typed },
	{ "retype", answer_typed },
	{ "move", answer_move },
	{ "negate", answer_flag },
	{ "join", answer_flag },
	{ "board", answer_mission_board },
	{ "variables", answer_variables },
};

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Each op a token of its own and an answer.
constexpr bool ops_named() {
	for (size_t i = 0; i < std::size(kMissionLogicOps); ++i) {
		if (!kMissionLogicOps[i].token[0] || !kMissionLogicOps[i].answer) return false;
		for (size_t j = i + 1; j < std::size(kMissionLogicOps); ++j)
			if (same_text(kMissionLogicOps[i].token, kMissionLogicOps[j].token)) return false;
	}
	return true;
}
static_assert(ops_named(), "each mission_logic op has a token of its own and an answer");

constexpr const char *kLogicLists[] = { "trigger", "action" };

} // namespace

const char *mission_logic_op_choice(size_t index) {
	return index < std::size(kMissionLogicOps) ? kMissionLogicOps[index].token : nullptr;
}

const char *mission_logic_list_choice(size_t index) {
	return index < std::size(kLogicLists) ? kLogicLists[index] : nullptr;
}

const char *mission_logic_join_choice(size_t index) {
	constexpr LogicJoin joins[] = { LogicJoin::And, LogicJoin::Or, LogicJoin::Xor };
	return index < std::size(joins) ? logic_join_token(joins[index]) : nullptr;
}

const MissionDocument *mission_of(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const Document *document = document_of(context, args, error);
	const auto *mission = dynamic_cast<const MissionDocument *>(document);
	if (document && !mission) error = document->path() + " is no mission.";
	return mission;
}

MissionWords::MissionWords(const QueryContext &context, const MissionDocument &mission) {
	if (const AssetGraph *graph = context.core.view().findings.graph.get()) source_.emplace(*graph);
	names_ = mission_label_names(mission, source_ ? &*source_ : nullptr);
}

JsonValue answer_mission_logic(const QueryContext &context, const QueryArgs &args, std::string &error) {
	// The op's row (the choices check refused any other token before the handler ran).
	const std::string op = args.text("op");
	for (const MissionLogicOp &row : kMissionLogicOps)
		if (op == row.token) return row.answer(context, args, error);
	error = "no op " + op + ".";
	return JsonValue::make_null();
}

JsonValue answer_mission_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = mission_of(context, args, error);
	NodeAddress address;
	if (!mission || !record_of(*mission, args, address, error)) return JsonValue::make_null();
	return mission_uses_to_json(mission_uses(*mission, address, *MissionWords(context, *mission)), page_of(args));
}

} // namespace opennova::editor
