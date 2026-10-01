#include <editor/session/outcome_json.h>

#include <string>
#include <utility>
#include <vector>

#include <editor/session/session_json.h>

namespace opennova::editor {

namespace {

io::JsonValue strings_to_json(const std::vector<std::string> &values) {
	io::JsonValue out = io::JsonValue::make_array();
	for (const std::string &value : values) out.push(io::json_string(value));
	return out;
}

} // namespace

io::JsonValue action_outcome_to_json(const ActionOutcome &outcome) {
	io::JsonValue out = io::JsonValue::make_object();
	out.set("done", io::JsonValue::make_bool(outcome.done()));
	out.set("unsaved_prompt", io::JsonValue::make_bool(outcome.unsaved_prompt));
	out.set("operation", io::json_number(double(outcome.operation)));
	out.set("findings", diagnostics_to_json(outcome.findings));
	io::JsonValue added = io::JsonValue::make_array();
	for (const NodeId id : outcome.added) added.push(io::json_number(double(id)));
	out.set("added", std::move(added));
	if (!outcome.imported.empty()) out.set("imported", strings_to_json(outcome.imported));
	if (!outcome.not_imported.empty()) out.set("not_imported", strings_to_json(outcome.not_imported));
	return out;
}

} // namespace opennova::editor
