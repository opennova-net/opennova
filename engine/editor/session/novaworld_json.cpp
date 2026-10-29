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

// A protocol as the service names it: {code, name, version}.
JsonValue protocol_json(const RemoteProtocol &protocol) {
	JsonValue out = JsonValue::make_object();
	out.set("code", json_string(protocol.code));
	out.set("name", json_string(protocol.name));
	out.set("version", json_string(protocol.version));
	return out;
}

JsonValue project_json(const RemoteProject &p) {
	JsonValue project = JsonValue::make_object();
	project.set("key", json_string(p.key));
	project.set("title", json_string(p.title));
	project.set("kind", json_string(p.kind));
	project.set("game", json_string(p.game));
	project.set("expansion_code", json_string(p.expansion_code));
	project.set("owner", p.owner.empty() ? JsonValue::make_null() : json_string(p.owner));
	project.set("public", JsonValue::make_bool(p.is_public));
	project.set("base", json_string(p.base));
	project.set("head", json_number(double(p.head)));
	project.set("can_push", JsonValue::make_bool(p.can_push));
	project.set("can_manage", JsonValue::make_bool(p.can_manage));
	project.set("can_release", JsonValue::make_bool(p.can_release));
	project.set("lobby_locked", JsonValue::make_bool(p.lobby_locked));
	JsonValue lobby = JsonValue::make_null();
	if (p.lobby) {
		lobby = JsonValue::make_object();
		lobby.set("slug", json_string(p.lobby->slug));
		lobby.set("display_name", json_string(p.lobby->display_name));
		lobby.set("protocol", protocol_json(p.lobby->protocol));
	}
	project.set("lobby", lobby);
	JsonValue release = JsonValue::make_null();
	if (p.release_id != 0) {
		release = JsonValue::make_object();
		release.set("id", json_number(double(p.release_id)));
		release.set("version", json_string(p.release_version));
		release.set("one_version", json_string(p.release_one_version));
	}
	project.set("release", release);
	return project;
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
	out.set("waiting", json_string(novaworld.waiting));
	JsonValue projects = JsonValue::make_array();
	for (const RemoteProject &p : novaworld.projects)
		projects.push(project_json(p));
	out.set("projects", projects);
	out.set("projects_error", json_string(novaworld.projects_error));
	JsonValue games = JsonValue::make_array();
	for (const RemoteGame &g : novaworld.games) {
		JsonValue game = JsonValue::make_object();
		game.set("key", json_string(g.key));
		game.set("title", json_string(g.title));
		game.set("game", json_string(g.game));
		games.push(std::move(game));
	}
	out.set("games", games);
	JsonValue lobbies = JsonValue::make_array();
	for (const auto &lobby : novaworld.lobbies) {
		JsonValue row = JsonValue::make_object();
		row.set("slug", json_string(lobby.slug));
		row.set("display_name", json_string(lobby.display_name));
		row.set("protocol", protocol_json(lobby.protocol));
		lobbies.push(std::move(row));
	}
	out.set("lobbies", lobbies);
	out.set("titles", io::json_string_array(novaworld.titles));
	JsonValue copy = JsonValue::make_object();
	copy.set("key", json_string(novaworld.key));
	copy.set("tracked", JsonValue::make_bool(novaworld.tracked));
	copy.set("revision", json_number(double(novaworld.revision)));
	out.set("working_copy", copy);
	out.set("project",
			novaworld.remote_known ? project_json(novaworld.remote) : JsonValue::make_null());
	JsonValue put = JsonValue::make_object();
	put.set("kind", json_string(novaworld.put.kind));
	put.set("game", json_string(novaworld.put.game));
	put.set("base", json_string(novaworld.put.base));
	put.set("expansion_code", json_string(novaworld.put.expansion_code));
	put.set("refusal", json_string(novaworld.put.refusal));
	put.set("refused", json_string(novaworld.put_refusal));
	out.set("put", put);
	JsonValue base = JsonValue::make_object();
	base.set("key", json_string(novaworld.new_base.key));
	base.set("for_dir", json_string(novaworld.new_base.for_dir));
	base.set("dir", json_string(novaworld.new_base.dir));
	base.set("found", JsonValue::make_bool(novaworld.new_base.found));
	base.set("from_install", JsonValue::make_bool(novaworld.new_base.from_install));
	out.set("new_base", base);
	out.set("missing_base", json_string(novaworld.missing_base));
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
	JsonValue releases = JsonValue::make_array();
	for (const RemoteRelease &release : novaworld.releases) {
		JsonValue row = JsonValue::make_object();
		row.set("id", json_number(double(release.id)));
		row.set("name", json_string(release.name));
		row.set("version", json_string(release.version));
		row.set("one_version", json_string(release.one_version));
		row.set("status", json_string(release.status));
		row.set("review_note", json_string(release.review_note));
		row.set("error", json_string(release.error));
		releases.push(std::move(row));
	}
	out.set("releases", releases);
	out.set("releases_error", json_string(novaworld.releases_error));
	out.set("history_error", json_string(novaworld.history_error));
	out.set("last", json_string(novaworld.last));
	return out;
}

} // namespace opennova::editor
