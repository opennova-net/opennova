#include <editor/session/outcome_json.h>

#include <utility>

#include <editor/session/session_json.h>

namespace opennova::editor {

io::JsonValue action_outcome_to_json(const ActionOutcome &outcome) {
	io::JsonValue out = io::JsonValue::make_object();
	out.set("done", io::JsonValue::make_bool(outcome.done()));
	out.set("unsaved_prompt", io::JsonValue::make_bool(outcome.unsaved_prompt));
	out.set("operation", io::json_number(double(outcome.operation)));
	out.set("findings", diagnostics_to_json(outcome.findings));
	io::JsonValue added = io::JsonValue::make_array();
	for (const NodeId id : outcome.added) added.push(io::json_number(double(id)));
	out.set("added", std::move(added));
	return out;
}

} // namespace opennova::editor
