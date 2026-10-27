#include <editor/session/novaworld_json.h>

#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

JsonValue change_or_null(const std::optional<NovaWorldChange> &change) {
	return change ? json_string(novaworld_change_name(*change)) : JsonValue::make_null();
}

} // namespace

JsonValue novaworld_section(const SessionView &view) {
	const NovaWorldView &novaworld = view.novaworld;
	JsonValue out = JsonValue::make_object();
	out.set("available", JsonValue::make_bool(novaworld.available));
	JsonValue account = JsonValue::make_object();
	account.set("server", json_string(novaworld.server));
	account.set("username", json_string(novaworld.username));
	account.set("role", json_string(novaworld.role));
	account.set("signed_in", JsonValue::make_bool(novaworld.signed_in));
	out.set("account", account);
	out.set("calling", JsonValue::make_bool(novaworld.calling));
	JsonValue projects = JsonValue::make_array();
	for (const RemoteProject &p : novaworld.projects) {
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
	out.set("projects_error", json_string(novaworld.projects_error));
	JsonValue binding = JsonValue::make_object();
	binding.set("bound", JsonValue::make_bool(novaworld.bound));
	binding.set("key", json_string(novaworld.key));
	binding.set("revision", json_number(double(novaworld.revision)));
	out.set("binding", binding);
	JsonValue standing = JsonValue::make_object();
	standing.set("checked", JsonValue::make_bool(novaworld.checked));
	standing.set("head_known", JsonValue::make_bool(novaworld.head_known));
	standing.set("head", json_number(double(novaworld.head)));
	standing.set("head_error", json_string(novaworld.head_error));
	JsonValue changes = JsonValue::make_array();
	for (const LocalChange &c : novaworld.changes) {
		JsonValue change = JsonValue::make_object();
		change.set("path", json_string(c.path));
		change.set("change", json_string(novaworld_change_name(c.change)));
		changes.push(change);
	}
	standing.set("changes", changes);
	standing.set("problems", io::json_string_array(novaworld.problems));
	out.set("standing", standing);
	JsonValue pull = JsonValue::make_object();
	pull.set("revision", json_number(double(novaworld.pull_revision)));
	JsonValue conflicts = JsonValue::make_array();
	for (const NovaWorldConflictView &c : novaworld.conflicts) {
		JsonValue conflict = JsonValue::make_object();
		conflict.set("path", json_string(c.path));
		conflict.set("mine", change_or_null(c.mine));
		conflict.set("theirs", change_or_null(c.theirs));
		conflicts.push(conflict);
	}
	pull.set("conflicts", conflicts);
	out.set("pull", pull);
	JsonValue history = JsonValue::make_array();
	for (const RemoteRevision &revision : novaworld.history) {
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
	out.set("history_error", json_string(novaworld.history_error));
	out.set("last", json_string(novaworld.last));
	return out;
}

} // namespace opennova::editor
