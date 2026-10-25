#include <editor/session/content_json.h>

#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

JsonValue change_or_null(const std::optional<ContentChange> &change) {
	return change ? json_string(content_change_name(*change)) : JsonValue::make_null();
}

} // namespace

JsonValue content_section(const SessionView &view) {
	const ContentView &content = view.content;
	JsonValue out = JsonValue::make_object();
	out.set("available", JsonValue::make_bool(content.available));
	JsonValue account = JsonValue::make_object();
	account.set("server", json_string(content.server));
	account.set("username", json_string(content.username));
	account.set("role", json_string(content.role));
	account.set("signed_in", JsonValue::make_bool(content.signed_in));
	out.set("account", account);
	out.set("calling", JsonValue::make_bool(content.calling));
	JsonValue projects = JsonValue::make_array();
	for (const RemoteProject &p : content.projects) {
		JsonValue project = JsonValue::make_object();
		project.set("key", json_string(p.key));
		project.set("title", json_string(p.title));
		project.set("public", JsonValue::make_bool(p.is_public));
		project.set("retail", JsonValue::make_bool(p.retail));
		project.set("base", json_string(p.base));
		project.set("head", json_number(double(p.head)));
		project.set("can_push", JsonValue::make_bool(p.can_push));
		projects.push(project);
	}
	out.set("projects", projects);
	out.set("projects_error", json_string(content.projects_error));
	JsonValue binding = JsonValue::make_object();
	binding.set("bound", JsonValue::make_bool(content.bound));
	binding.set("key", json_string(content.key));
	binding.set("revision", json_number(double(content.revision)));
	out.set("binding", binding);
	JsonValue standing = JsonValue::make_object();
	standing.set("checked", JsonValue::make_bool(content.checked));
	standing.set("head_known", JsonValue::make_bool(content.head_known));
	standing.set("head", json_number(double(content.head)));
	standing.set("head_error", json_string(content.head_error));
	JsonValue changes = JsonValue::make_array();
	for (const LocalChange &c : content.changes) {
		JsonValue change = JsonValue::make_object();
		change.set("path", json_string(c.path));
		change.set("change", json_string(content_change_name(c.change)));
		changes.push(change);
	}
	standing.set("changes", changes);
	standing.set("problems", io::json_string_array(content.problems));
	out.set("standing", standing);
	JsonValue pull = JsonValue::make_object();
	pull.set("revision", json_number(double(content.pull_revision)));
	JsonValue conflicts = JsonValue::make_array();
	for (const ContentConflictView &c : content.conflicts) {
		JsonValue conflict = JsonValue::make_object();
		conflict.set("path", json_string(c.path));
		conflict.set("mine", change_or_null(c.mine));
		conflict.set("theirs", change_or_null(c.theirs));
		conflicts.push(conflict);
	}
	pull.set("conflicts", conflicts);
	out.set("pull", pull);
	JsonValue history = JsonValue::make_array();
	for (const RemoteRevision &revision : content.history) {
		JsonValue row = JsonValue::make_object();
		row.set("number", json_number(double(revision.number)));
		row.set("author", json_string(revision.author));
		row.set("message", json_string(revision.message));
		row.set("files", json_number(double(revision.file_count)));
		row.set("bytes", json_number(double(revision.bytes)));
		row.set("created_at", json_string(revision.created_at));
		history.push(std::move(row));
	}
	out.set("history", history);
	out.set("history_error", json_string(content.history_error));
	out.set("last", json_string(content.last));
	return out;
}

} // namespace opennova::editor
