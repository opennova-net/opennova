#include <editor/session/workspace_parts.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mnu_table.h>
#include <editor/graph/reference_kinds.h>
#include <editor/import/import_plan.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/file_card.h>
#include <editor/session/problem_confirmation.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using J = WorkspaceJson;

JsonValue text(const std::string &value) { return JsonValue::make_string(value); }
JsonValue flag(bool value) { return JsonValue::make_bool(value); }
JsonValue number(double value) { return JsonValue::make_number(value); }

// --- the table ----------------------------------------------------------------------------------

constexpr size_t kTextLongest = kWorkspaceText - 1;
constexpr size_t kPathLongest = kWorkspacePath - 1;
constexpr size_t kFileNameLongest = kWorkspaceFileName - 1;
constexpr size_t kExpansionLongest = kWorkspaceExpansion - 1;

constexpr WorkspaceMember kCard[] = {
	{ "path", J::String,
			"The project file whose card shows (a project-relative path or a logical name; \"\" closes it, and stops "
			"the sound it played): what it is, where a build puts it, what it names and who names it, a wave's "
			"sound. about_file opens it too, Files coming forward; the session closes it as the file goes, and a "
			"rename of the file moves it." },
};
constexpr WorkspaceMember kBuildResult[] = {
	{ "open", J::Boolean,
			"The build result's panel shows (what the last build came to: its folder, what refused it). A build's "
			"end opens it, unless a Play waits on the build or the build was asked with report false." },
};
constexpr WorkspaceMember kNewProject[] = {
	{ "open", J::Boolean,
			"File > New project...'s modal is open (the welcome page shows the form whenever no project is "
			"open). A new_project closes it as the session takes it." },
	{ "title", J::String, "The form's Name.", kTextLongest },
	{ "dir", J::String, "The form's Folder: where the project is made.", kPathLongest },
	{ "game_install", J::String,
			"The form's Game install, named (left unnamed, the form shows the editor's own); the window checks it "
			"as typing stops (check_install), its words under the field.", kPathLongest },
	{ "builds_on", J::String,
			"The installed expansion it builds on, by its folder's name (\"\" the base game); building on one "
			"builds as an expansion." },
	{ "as_expansion", J::Boolean, "Build as an expansion: the project builds as expansion\\<expansion>\\." },
	{ "expansion", J::String, "The expansion's name, which the form checks as the game's rule takes it.", kExpansionLongest },
};
constexpr WorkspaceMember kSettings[] = {
	{ "open", J::Boolean,
			"File > Project settings... is open (a project open). Opening it fills its fields with the settings in "
			"effect, before the other members the change names; its Apply is apply_project_settings with a "
			"serial, which closes it once every setting is written." },
	{ "title", J::String, "The project's name.", kTextLongest },
	{ "mission", J::Boolean, "Missions: the project holds missions." },
	{ "multiplayer", J::Boolean, "Multiplayer." },
	{ "builds_on", J::String, "The installed expansion it builds on (\"\" the base game); building on one builds as one." },
	{ "as_expansion", J::Boolean, "Build as an expansion." },
	{ "expansion", J::String, "The expansion's name.", kExpansionLongest },
	{ "game_install", J::String, "This computer's game install folder.", kPathLongest },
	{ "runtime", J::String, "The OpenNova runtime Play runs (\"\" the one packaged beside the editor).", kPathLongest },
	{ "play_mode", J::String,
			"How the project plays here (its own, kept in its .opennova/local.json): runtime (the OpenNova runtime), "
			"install (Play in the game install) or strict (Play in the game install as a player's drop-in: the build "
			"and the install's program alone, launched without /d)." },
};
constexpr WorkspaceMember kNewFile[] = {
	{ "kind", J::String,
			"The kind of file the prompt makes, an asset kind's token among those Files' New asks a name of (a string "
			"table, a menu, a font, a mission; \"\" closes it). Opening it empties its name and values." },
	{ "name", J::String, "The new file's name, checked as it is typed.", kFileNameLongest },
	{ "values", J::Object,
			"What the kind's blank takes beside the name, by its params' tokens (a mission's title, terrain, "
			"environment), each a string.", kFileNameLongest },
	{ "folder", J::String,
			"The folder a folder's New here makes it in (DI-25: \"\" where the placement rule puts a file of its kind, "
			"\"/\" the top level); opening another kind empties it.", kPathLongest },
};
constexpr WorkspaceMember kFileRename[] = {
	{ "path", J::String,
			"The project file Files' Rename... renames (\"\" closes it; opening it starts its name as the file's). "
			"show_in_files with ask_name opens it too; the session closes it as the file goes." },
	{ "name", J::String, "The new name typed; the window previews the rename as it changes (preview_rename).",
			kFileNameLongest },
};
constexpr WorkspaceMember kFileDelete[] = {
	{ "path", J::String,
			"The project file Files' Delete... deletes (\"\" closes it): who names it listed first (the used_by query's "
			"uses), its Delete a delete_asset, with force where something names it. The session closes it as the file "
			"goes.", kPathLongest },
	{ "alone", J::Boolean, "An import source deleted alone: its outputs kept as files of the project." },
};
constexpr WorkspaceMember kRename[] = {
	{ "open", J::Boolean,
			"Rename everywhere is open over the rename preview_rename planned (a preview_rename with ask_name opens it; "
			"another rename planned in its place closes it)." },
	{ "name", J::String, "The new name typed; the window plans the rename again as it changes (preview_rename).",
			kTextLongest },
};
constexpr WorkspaceMember kRenameBack[] = {
	{ "open", J::Boolean,
			"Rename back is open over the plan preview_rename_back made (its ask_name opens it; a plan that is no way "
			"back in its place closes it)." },
};
constexpr WorkspaceMember kFind[] = {
	{ "open", J::Boolean, "The Document window's find bar is open over the active document (Ctrl+F)." },
	{ "text", J::String, "What it finds: its hits are the document_search query's; open_document at a hit shows it.",
			kTextLongest },
	{ "match_case", J::Boolean, "Aa: case counts." },
};
constexpr WorkspaceMember kProjectFind[] = {
	{ "open", J::Boolean,
			"The project's finder is open (a project open): Find in project (Ctrl+Shift+F), Go to file (Ctrl+P), Go to "
			"name (Ctrl+T) or Find usages (Shift+F12), as its scope says." },
	{ "text", J::String,
			"What it finds: the project_search query's hits of its scope; Find usages' uses whose line holds it. Opened "
			"on another scope or another subject, it starts empty unless the change names it.", kTextLongest },
	{ "scope", J::String,
			"What it lists: all (every file and name, Find in project), files (the files alone, Go to file), names (the "
			"names the files define alone, Go to name), or usages (the uses of path, or of its record at locator: the "
			"usages query's, Find usages)." },
	{ "path", J::String,
			"Find usages' file, a project file by its path or logical name (scope usages needs it).", kPathLongest },
	{ "locator", J::String,
			"Find usages' record in that file, by its locator (a native file's record by its path, as a Go to names "
			"it); \"\" the file itself.", kPathLongest },
};
constexpr WorkspaceMember kFiles[] = {
	{ "filter", J::String,
			"Files' filter: the files whose paths hold the text, then those of a kind it names (\"texture\"), "
			"listed flat; the files query with text lists the same.", kTextLongest },
	{ "kind", J::String, "The kind Files lists alone, an asset kind's token (\"\" every kind)." },
	{ "by_cost", J::Boolean,
			"Files lists what it lists flat (a filter or a kind) by what the game's textures of each file cost at "
			"full detail, the costliest first, the files the game makes no model texture of after them (the "
			"texture_budget query's textures)." },
};
constexpr WorkspaceMember kImport[] = {
	{ "filter", J::String,
			"The import dialog's filter of the files to choose from (a name, a folder, or a kind's word), with an "
			"import preview open.", kTextLongest },
	{ "choice_kind", J::String, "The kind of the files to choose from it lists alone, an asset kind's token (\"\" every kind)." },
	{ "rows_filter", J::String, "Its filter of the plan's rows (a filter or a kind shown lists them flat).", kTextLongest },
	{ "kind_shown", J::String, "The kind of the plan's rows it shows alone, an asset kind's token (\"\" every kind)." },
	{ "replace_existing", J::Boolean,
			"Replace existing files: every file the project holds already checked (each can be unchecked alone), "
			"or unchecked again." },
	{ "plan", J::Integer,
			"The plan check and uncheck index (the import_preview query's plan): they need it, and one the dialog no "
			"longer shows (planned again since) is refused, nothing changed." },
	{ "check", J::Integers,
			"The plan's rows checked, by their index (the import_preview query's index); a converter's outputs come "
			"together: check one, check them all." },
	{ "uncheck", J::Integers, "The plan's rows unchecked, by their index." },
};
constexpr WorkspaceMember kProblems[] = {
	{ "severities", J::Strings, "The severities shown, of error, warning and info." },
	{ "text", J::String, "Only the problems whose message, file, record, field or code holds it.", kTextLongest },
	{ "scope", J::String,
			"Whose problems: project, active_file (none while no document is active) or open_files." },
	{ "group", J::String, "How they group: none, file or kind." },
	{ "fixable", J::Boolean, "Only fixable: the problems the editor offers a fix for." },
	{ "blocking", J::Boolean,
			"Blocks the build: only what a build is refused for, every one (the other filters set aside until it "
			"is turned off)." },
	{ "confirm", J::Object,
			"The confirmation a Fix all or a Use fix waits in for Apply: {group} a group's Fix all by its key (the "
			"problems query's groups, with this grouping and these filters), {required} the summary's Fix all of a "
			"request kind (create_missing, preview_install_import), or {finding, label} the fix of that label of "
			"the finding at that index (the problems query's index, a whole number); {} closes it. One that names "
			"nothing Problems offers now is refused. The workspace section shows what it proposes; "
			"apply_confirmation (or its Apply) raises that." },
};
constexpr WorkspaceMember kDocument[] = {
	{ "path", J::String, "The document (its path or logical name; left out, the active one), open." },
	{ "filter", J::String, "Its outline's filter (a stylesheet's lines' too): the rows and records whose words hold it.",
			kTextLongest },
	{ "kinds", J::Strings,
			"The kinds of row its outline lists, by their tokens (a mission's chips: a mission's alone); every kind "
			"when all are named." },
	{ "all_rows", J::Boolean, "Its outline lists every row, those its type leaves out too (a mission's empty paths)." },
	{ "sort", J::Boolean, "Its list sorted by name." },
	{ "every", J::Boolean, "Master and detail: the filter over every row's detail records, not the selected row's alone." },
	{ "inspector_filter", J::String, "The Inspector's filter over its fields and lists.", kTextLongest },
	{ "new_window_type", J::String, "A menu's type for the next Add window, one of its TYPE's names (static, button, ...)." },
	{ "remove_screen", J::Integer,
			"A menu's screen whose Remove waits on its confirmation, by its record id (0: none): a screen of the menu "
			"while it keeps a second." },
	{ "remap_from", J::Integer, "An 8-bit PCX's palette index to move (0 to 255): a .pcx texture's." },
	{ "remap_to", J::Integer, "The palette index it moves to (0 to 255)." },
};

constexpr WorkspacePartRow kParts[] = {
	{ "card", kCard, std::size(kCard), "Files' card of a file." },
	{ "build_result", kBuildResult, std::size(kBuildResult), "The build result's panel." },
	{ "new_project", kNewProject, std::size(kNewProject), "The new-project form (the welcome page's, File > New project...'s)." },
	{ "settings", kSettings, std::size(kSettings), "File > Project settings...: its fields until Apply." },
	{ "new_file", kNewFile, std::size(kNewFile), "Files' New file prompt." },
	{ "file_rename", kFileRename, std::size(kFileRename), "Files' Rename... of a file." },
	{ "file_delete", kFileDelete, std::size(kFileDelete), "Files' Delete... of a file (DI-25)." },
	{ "rename", kRename, std::size(kRename), "Rename everywhere." },
	{ "rename_back", kRenameBack, std::size(kRenameBack), "Rename back." },
	{ "find", kFind, std::size(kFind), "The Document window's find bar." },
	{ "project_find", kProjectFind, std::size(kProjectFind),
	  "The project's finder: Find in project, Go to file, Go to name, Find usages." },
	{ "files", kFiles, std::size(kFiles), "Files' filter, kind and order." },
	{ "import", kImport, std::size(kImport), "The import dialog's filters, Replace existing files and its checks." },
	{ "problems", kProblems, std::size(kProblems), "Problems' filters and its confirmation." },
	{ "document", kDocument, std::size(kDocument), "What a document's views show of it: filters, kinds, order, a menu's and a texture's fields." },
};

// The windows `focus` brings forward: each one's token and its title (devtools::Window::title()).
struct FocusRow {
	const char *token;
	const char *title;
};
constexpr FocusRow kWindows[] = {
	{ "files", "Files" },     { "document", "Document" }, { "preview", "Preview" },
	{ "inspector", "Inspector" }, { "problems", "Problems" }, { "output", "Output" },
};

const WorkspacePartRow *part_of(const std::string &token) {
	for (const WorkspacePartRow &row : kParts)
		if (token == row.token) return &row;
	return nullptr;
}

const WorkspaceMember *member_of(const WorkspacePartRow &part, const std::string &token) {
	for (size_t i = 0; i < part.member_count; ++i)
		if (token == part.members[i].token) return &part.members[i];
	return nullptr;
}

std::string tokens_of(const WorkspacePartRow &part) {
	std::string out;
	for (size_t i = 0; i < part.member_count; ++i) out += (i ? ", " : "") + std::string(part.members[i].token);
	return out;
}

std::string parts_taken() {
	std::string out;
	for (const WorkspacePartRow &row : kParts) out += (out.empty() ? "" : ", ") + std::string(row.token);
	return out + ", focus";
}

bool whole(const JsonValue &value) {
	return value.is_number() && value.number >= 0.0 && value.number == std::floor(value.number) &&
	       value.number <= 9007199254740992.0;
}

bool typed(const JsonValue &value, WorkspaceJson json) {
	switch (json) {
		case J::String:
			return value.is_string();
		case J::Boolean:
			return value.is_bool();
		case J::Integer:
			return whole(value);
		case J::Strings:
			if (!value.is_array()) return false;
			for (const JsonValue &item : value.array)
				if (!item.is_string()) return false;
			return true;
		case J::Integers:
			if (!value.is_array()) return false;
			for (const JsonValue &item : value.array)
				if (!whole(item)) return false;
			return true;
		case J::Object:
			if (!value.is_object()) return false;
			for (const io::JsonMember &member : value.object)
				if (!member.value.is_string() && !whole(member.value)) return false;
			return true;
	}
	return false;
}

const char *type_words(WorkspaceJson json) {
	switch (json) {
		case J::String:
			return "a string";
		case J::Boolean:
			return "true or false";
		case J::Integer:
			return "a whole number, 0 or more";
		case J::Strings:
			return "an array of strings";
		case J::Integers:
			return "an array of whole numbers, 0 or more";
		case J::Object:
			return "an object of strings and whole numbers";
	}
	return "a string";
}

// The longest string of a value (each string of an array or an object's).
size_t longest_of(const JsonValue &value) {
	size_t out = value.is_string() ? value.string.size() : 0;
	for (const JsonValue &item : value.array) out = std::max(out, longest_of(item));
	for (const io::JsonMember &member : value.object) out = std::max(out, longest_of(member.value));
	return out;
}

// --- what each part does ------------------------------------------------------------------------

// A change being applied: the view it changes and the refusals it makes (each part refused changes
// nothing of its own).
struct Change {
	SessionView &view;
	std::vector<WorkspaceRefusal> &refusals;
	bool refuse(const std::string &message, const std::string &asset = std::string()) {
		refusals.push_back(WorkspaceRefusal{ message, asset });
		return false;
	}
	// A member set where its part is closed: refused, saying what opens it.
	bool closed(const char *part, const char *opens) { return refuse(std::string(part) + " is not open: " + opens + "."); }
	WorkspaceView &workspace() { return view.workspace; }
};

// The project file `name` names (a project-relative path in any case, or a logical name), the scan's.
const AssetEntry *project_file(const SessionView &view, const std::string &name) {
	return view.project.open && view.project.scan ? view.project.scan->named(name) : nullptr;
}

// The open document `name` names (its path or a logical name; "" the active one).
const DocumentBase *open_document(const SessionView &view, const std::string &name) {
	std::string path = name.empty() ? view.documents.active : name;
	if (const AssetEntry *file = project_file(view, path)) path = file->relative_path;
	for (const auto &document : view.documents.open)
		if (document->path() == path) return document.get();
	return nullptr;
}

// The sound the editor plays stopped (the card closing, stop_sound): a play under way or done says so.
bool stop_sound(WorkspaceView::Sound &sound) {
	if (sound.state != WorkspaceView::SoundState::Starting && sound.state != WorkspaceView::SoundState::Playing)
		return false;
	sound.state = WorkspaceView::SoundState::Stopped;
	return true;
}


// The menu the document is, and whether `screen` is a screen of it its Remove may ask of: the menu keeps a
// second screen.
const MnuDocument *menu_of(const DocumentBase &document) { return dynamic_cast<const MnuDocument *>(records_of(document)); }
bool removable_screen(const DocumentBase &document, uint64_t screen) {
	const MnuDocument *menu = menu_of(document);
	return menu && menu->row(NodeId(screen)) && menu->rows().size() >= 2;
}

// A menu window's TYPE choices (what Add window's type takes).
const FieldSchema *window_type_field(const MnuDocument &menu) {
	for (const FieldSchema &field : menu.fields(node_kind(MenuKind::Window)))
		if (field.id == "type") return &field;
	return nullptr;
}

bool set_card(Change &change, const JsonValue &part) {
	const JsonValue *path = part.get("path");
	if (!path) return false;
	WorkspaceView &workspace = change.workspace();
	if (path->string.empty()) {
		if (workspace.card.path.empty()) return false;
		workspace.card.path.clear();
		stop_sound(workspace.sound);
		return true;
	}
	if (!change.view.project.open) return change.refuse("No project is open: a card is of a project file.", path->string);
	const AssetEntry *file = project_file(change.view, path->string);
	if (!file) return change.refuse("The project has no file " + path->string + " to show the card of.", path->string);
	return show_card(workspace, file->relative_path);
}

bool set_build_result(Change &change, const JsonValue &part) {
	const JsonValue *open = part.get("open");
	if (!open || change.workspace().build_result.open == open->boolean) return false;
	change.workspace().build_result.open = open->boolean;
	return true;
}

bool set_new_project(Change &change, const JsonValue &part) {
	WorkspaceView::NewProject form = change.workspace().new_project;
	if (const JsonValue *open = part.get("open")) form.open = open->boolean;
	if (const JsonValue *title = part.get("title")) form.title = title->string;
	if (const JsonValue *dir = part.get("dir")) form.dir = dir->string;
	if (const JsonValue *install = part.get("game_install")) {
		form.game_install = install->string;
		form.install_named = true;
	}
	if (const JsonValue *builds_on = part.get("builds_on")) form.builds_on = builds_on->string;
	if (const JsonValue *as_expansion = part.get("as_expansion")) form.as_expansion = as_expansion->boolean;
	if (const JsonValue *expansion = part.get("expansion")) form.expansion = expansion->string;
	// The game reads an expansion's own files only under /exp: a project on an installed expansion builds as one.
	if (!form.builds_on.empty()) form.as_expansion = true;
	WorkspaceView::NewProject &held = change.workspace().new_project;
	const bool moved = form.open != held.open || form.title != held.title || form.dir != held.dir ||
	                   form.game_install != held.game_install || form.install_named != held.install_named ||
	                   form.builds_on != held.builds_on || form.as_expansion != held.as_expansion ||
	                   form.expansion != held.expansion;
	held = std::move(form);
	return moved;
}

bool same_settings(const WorkspaceView::Settings &a, const WorkspaceView::Settings &b) {
	return a.open == b.open && a.title == b.title && a.mission == b.mission && a.multiplayer == b.multiplayer &&
	       a.builds_on == b.builds_on && a.as_expansion == b.as_expansion && a.expansion == b.expansion &&
	       a.game_install == b.game_install && a.runtime == b.runtime && a.play_mode == b.play_mode;
}

// Project settings: opened over the settings in effect (the project's document, the install in effect, the
// editor's runtime and how the project plays), then the members the change names; a field set while it
// is closed is refused, nothing changed, and so is a play_mode no mode has.
bool set_settings(Change &change, const JsonValue &part) {
	const SessionView &view = change.view;
	WorkspaceView::Settings settings = view.workspace.settings;
	const JsonValue *open = part.get("open");
	if (open && open->boolean && !settings.open) {
		if (!view.project.open) return change.refuse("No project is open: its settings are a project's.");
		const ProjectDocument &doc = *view.project.document;
		settings = WorkspaceView::Settings();
		settings.open = true;
		settings.title = doc.title;
		settings.mission = doc.features.mission;
		settings.multiplayer = doc.features.multiplayer;
		settings.builds_on = doc.expansion.builds_on;
		settings.as_expansion = !doc.expansion.standalone();
		settings.expansion = doc.expansion.name;
		settings.game_install = view.project.retail_directory;
		settings.runtime = view.project.runtime_setting;
		settings.play_mode = view.project.play_mode;
	} else if (open) {
		settings.open = open->boolean;
	}
	if (part.object.size() > (open ? 1u : 0u) && !settings.open)
		return change.closed("Project settings", "open it first (settings.open true)");
	if (const JsonValue *title = part.get("title")) settings.title = title->string;
	if (const JsonValue *mission = part.get("mission")) settings.mission = mission->boolean;
	if (const JsonValue *multiplayer = part.get("multiplayer")) settings.multiplayer = multiplayer->boolean;
	if (const JsonValue *builds_on = part.get("builds_on")) settings.builds_on = builds_on->string;
	if (const JsonValue *as_expansion = part.get("as_expansion")) settings.as_expansion = as_expansion->boolean;
	if (const JsonValue *expansion = part.get("expansion")) settings.expansion = expansion->string;
	if (const JsonValue *install = part.get("game_install")) settings.game_install = install->string;
	if (const JsonValue *runtime = part.get("runtime")) settings.runtime = runtime->string;
	if (const JsonValue *mode = part.get("play_mode"); mode && !play_mode_from_token(mode->string, settings.play_mode))
		return change.refuse("\"settings.play_mode\" is runtime, install or strict, not \"" + mode->string + "\".");
	if (!settings.builds_on.empty()) settings.as_expansion = true;
	if (!settings.open) settings = WorkspaceView::Settings();
	if (same_settings(settings, view.workspace.settings)) return false;
	change.workspace().settings = std::move(settings);
	return true;
}

// The kind an asset kind's token names, kCount for "" (none: closed, or every kind); false for a token no
// kind has.
bool kind_named(const std::string &token, AssetKind &out) {
	if (token.empty()) {
		out = AssetKind::kCount;
		return true;
	}
	out = asset_kind_from_token(token);
	return out != AssetKind::Unknown;
}

// The New file prompt: opened on a kind Files' New asks a name of (its name and values emptied), its fields set
// while it is open, its values by its kind's params alone.
bool set_new_file(Change &change, const JsonValue &part) {
	WorkspaceView::NewFile prompt = change.workspace().new_file;
	if (const JsonValue *kind = part.get("kind")) {
		AssetKind wanted = AssetKind::kCount;
		if (!kind_named(kind->string, wanted)) return change.refuse("No kind of file is \"" + kind->string + "\".");
		if (wanted != AssetKind::kCount && !change.view.project.open) return change.closed("The project", "open or make one first");
		size_t count = 0;
		bool asks = false;
		new_file_params(wanted, count, &asks);
		if (wanted != AssetKind::kCount && !asks) {
			std::string offered;
			for (size_t i = 0; i < blank_factory_count(); ++i) {
				const BlankFactory &factory = *blank_factory_at(i);
				if (factory.free_form && factory.role[0] == '\0') offered += (offered.empty() ? "" : ", ") + std::string(asset_kind_token(factory.kind));
			}
			// A terrain made from images (S20) is asked too: its images and numbers.
			offered += ", " + std::string(asset_kind_token(AssetKind::Terrain));
			return change.refuse("Files' New asks no name of a " + kind->string + " (it does of " + offered + ").");
		}
		if (wanted != prompt.kind) prompt = WorkspaceView::NewFile{ wanted, std::string(), {} };
	}
	if ((part.get("name") || part.get("values") || part.get("folder")) && prompt.kind == AssetKind::kCount)
		return change.closed("The New file prompt", "name its kind first (new_file.kind)");
	if (const JsonValue *name = part.get("name")) prompt.name = name->string;
	if (const JsonValue *folder = part.get("folder")) prompt.folder = folder->string;
	if (const JsonValue *values = part.get("values")) {
		size_t count = 0;
		const BlankParam *taken = new_file_params(prompt.kind, count);
		std::string params;
		for (size_t i = 0; i < count; ++i) params += (i ? ", " : "") + std::string(taken[i].token);
		prompt.values.clear();
		for (const io::JsonMember &value : values->object) {
			bool known = false;
			for (size_t i = 0; i < count; ++i) known = known || value.key == taken[i].token;
			if (!known)
				return change.refuse("A new " + std::string(asset_kind_token(prompt.kind)) + " takes no value \"" + value.key + "\" (" +
				                     (params.empty() ? std::string("it takes none") : "it takes " + params) + ").");
			if (!value.value.is_string()) return change.refuse("new_file.values." + value.key + " is a string.");
			prompt.values.emplace_back(value.key, value.value.string);
		}
	}
	WorkspaceView::NewFile &held = change.workspace().new_file;
	if (prompt.kind == held.kind && prompt.name == held.name && prompt.values == held.values && prompt.folder == held.folder)
		return false;
	held = std::move(prompt);
	return true;
}

// Files' Rename... of a file: opened on the file (its name starting as the file's), the name typed.
bool set_file_rename(Change &change, const JsonValue &part) {
	WorkspaceView::FileRename rename = change.workspace().file_rename;
	if (const JsonValue *path = part.get("path")) {
		if (path->string.empty()) {
			rename = WorkspaceView::FileRename();
		} else {
			const AssetEntry *file = project_file(change.view, path->string);
			if (!file) return change.refuse("The project has no file " + path->string + " to rename.", path->string);
			if (file->relative_path != rename.path) rename = WorkspaceView::FileRename{ file->relative_path, file->logical_name };
		}
	}
	if (const JsonValue *name = part.get("name")) {
		if (rename.path.empty()) return change.closed("Rename...", "name the file first (file_rename.path)");
		rename.name = name->string;
	}
	WorkspaceView::FileRename &held = change.workspace().file_rename;
	if (rename.path == held.path && rename.name == held.name) return false;
	held = std::move(rename);
	return true;
}

// Files' Delete... of a file (DI-25): opened on the file, whether an import source goes alone.
bool set_file_delete(Change &change, const JsonValue &part) {
	WorkspaceView::FileDelete asked = change.workspace().file_delete;
	if (const JsonValue *path = part.get("path")) {
		if (path->string.empty()) {
			asked = WorkspaceView::FileDelete();
		} else {
			const AssetEntry *file = project_file(change.view, path->string);
			if (!file) return change.refuse("The project has no file " + path->string + " to delete.", path->string);
			if (file->relative_path != asked.path) asked = WorkspaceView::FileDelete{ file->relative_path, false };
		}
	}
	if (const JsonValue *alone = part.get("alone")) {
		if (asked.path.empty()) return change.closed("Delete...", "name the file first (file_delete.path)");
		asked.alone = alone->boolean;
	}
	WorkspaceView::FileDelete &held = change.workspace().file_delete;
	if (asked.path == held.path && asked.alone == held.alone) return false;
	held = std::move(asked);
	return true;
}

// Rename everywhere opened over the rename the view's preview plans (a name's, not a way back): its target
// and the name it was asked for.
void open_rename(WorkspaceView::Rename &rename, const DialogsView::RenamePreview &plan) {
	rename.open = true;
	rename.path = plan.path;
	rename.locator = plan.locator;
	rename.field = plan.field;
	rename.old_name = plan.old_name;
	rename.kind = plan.kind;
	rename.name = plan.requested;
}

bool same_rename(const WorkspaceView::Rename &a, const WorkspaceView::Rename &b) {
	return a.open == b.open && a.path == b.path && a.locator == b.locator && a.field == b.field &&
	       a.old_name == b.old_name && a.kind == b.kind && a.name == b.name;
}

// Whether the view's rename plan is the open Rename everywhere's: a name's (no way back) of its field.
bool plan_of(const WorkspaceView::Rename &rename, const DialogsView::RenamePreview &plan) {
	return plan.symbol && !plan.back && plan.path == rename.path && plan.locator == rename.locator && plan.field == rename.field;
}

bool set_rename(Change &change, const JsonValue &part) {
	WorkspaceView::Rename rename = change.workspace().rename;
	const DialogsView::RenamePreview &plan = change.view.dialogs.rename_preview;
	if (const JsonValue *open = part.get("open")) {
		if (open->boolean && !rename.open) {
			if (!plan.symbol || plan.back) return change.closed("Rename everywhere", "plan a name's rename first (preview_rename)");
			open_rename(rename, plan);
		} else if (!open->boolean) {
			rename = WorkspaceView::Rename();
		}
	}
	if (const JsonValue *name = part.get("name")) {
		if (!rename.open) return change.closed("Rename everywhere", "open it first (preview_rename with ask_name)");
		rename.name = name->string;
	}
	if (same_rename(rename, change.workspace().rename)) return false;
	change.workspace().rename = std::move(rename);
	return true;
}

bool set_rename_back(Change &change, const JsonValue &part) {
	const JsonValue *open = part.get("open");
	if (!open || open->boolean == change.workspace().rename_back.open) return false;
	if (open->boolean && !change.view.dialogs.rename_preview.back)
		return change.closed("Rename back", "plan it first (preview_rename_back)");
	change.workspace().rename_back.open = open->boolean;
	return true;
}

bool set_find(Change &change, const JsonValue &part) {
	WorkspaceView::Find find = change.workspace().find;
	if (const JsonValue *open = part.get("open")) find.open = open->boolean;
	if (const JsonValue *text = part.get("text")) find.text = text->string;
	if (const JsonValue *match = part.get("match_case")) find.match_case = match->boolean;
	WorkspaceView::Find &held = change.workspace().find;
	if (find.open == held.open && find.text == held.text && find.match_case == held.match_case) return false;
	held = std::move(find);
	return true;
}

// The project's finder (DI-18: four scopes over one modal). Find usages names a project file, its record by a
// locator; another scope keeps no subject. Opened on another scope or subject, the text starts empty unless the
// change names one.
bool set_project_find(Change &change, const JsonValue &part) {
	WorkspaceView::ProjectFind find = change.workspace().project_find;
	if (const JsonValue *open = part.get("open")) {
		if (open->boolean && !change.view.project.open) return change.closed("The project", "open or make one first");
		find.open = open->boolean;
	}
	if (const JsonValue *scope = part.get("scope"); scope && !find_scope_from_token(scope->string, find.scope))
		return change.refuse("The finder has no scope \"" + scope->string + "\" (all, files, names, usages).");
	if (const JsonValue *path = part.get("path")) find.path = path->string;
	if (const JsonValue *locator = part.get("locator")) find.locator = locator->string;
	if (find.scope != WorkspaceView::FindScope::Usages) {
		find.path.clear();
		find.locator.clear();
	} else if (const AssetEntry *file = project_file(change.view, find.path)) {
		find.path = file->relative_path;
	} else if (find.open) {
		return change.refuse(find.path.empty() ? std::string("Find usages names a project file (path).")
		                                        : "No project file is " + find.path + ".", find.path);
	}
	WorkspaceView::ProjectFind &held = change.workspace().project_find;
	const bool elsewhere = find.scope != held.scope || find.path != held.path || find.locator != held.locator;
	if (const JsonValue *text = part.get("text")) find.text = text->string;
	else if (elsewhere) find.text.clear();
	if (find.open == held.open && find.text == held.text && !elsewhere) return false;
	held = std::move(find);
	return true;
}

bool set_files(Change &change, const JsonValue &part) {
	WorkspaceView::Files files = change.workspace().files;
	if (const JsonValue *filter = part.get("filter")) files.filter = filter->string;
	if (const JsonValue *kind = part.get("kind"); kind && !kind_named(kind->string, files.kind))
		return change.refuse("No kind of file is \"" + kind->string + "\".");
	if (const JsonValue *by_cost = part.get("by_cost")) files.by_cost = by_cost->boolean;
	WorkspaceView::Files &held = change.workspace().files;
	if (files.filter == held.filter && files.kind == held.kind && files.by_cost == held.by_cost) return false;
	held = std::move(files);
	return true;
}

// The import dialog's own: refused with no import preview open. Replace existing files sets the check of
// every file the project holds that the import can take; check and uncheck name rows by index in the plan
// they name (the one shown, else refused), a converter's outputs coming together.
bool set_import(Change &change, const JsonValue &part) {
	const DialogsView::ImportPreview &preview = change.view.dialogs.import_preview;
	if (!preview.open || !preview.plan) return change.closed("The import dialog", "preview an import first");
	WorkspaceView::Import import = change.workspace().import;
	if (const JsonValue *filter = part.get("filter")) import.filter = filter->string;
	if (const JsonValue *filter = part.get("rows_filter")) import.rows_filter = filter->string;
	for (const char *member : { "choice_kind", "kind_shown" }) {
		const JsonValue *kind = part.get(member);
		if (kind && !kind_named(kind->string, std::string(member) == "choice_kind" ? import.choice_kind : import.kind_shown))
			return change.refuse("No kind of file is \"" + kind->string + "\".");
	}
	const ImportPlan &plan = *preview.plan;
	const JsonValue *named_plan = part.get("plan");
	if (named_plan && uint64_t(named_plan->number) != preview.plan_serial)
		return change.refuse("The import was planned again: plan " + std::to_string(uint64_t(named_plan->number)) +
		                     " is gone, the dialog shows plan " + std::to_string(preview.plan_serial) +
		                     " (read import_preview again for its rows).");
	if ((part.get("check") || part.get("uncheck")) && !named_plan)
		return change.refuse("import.check and import.uncheck index a plan's rows: name the plan (import.plan, the "
		                     "import_preview query's plan; it is " + std::to_string(preview.plan_serial) + " now).");
	if (import.checked.size() != plan.rows.size()) import.checked = import_default_checks(plan, import.replace_existing);
	bool checks = false;
	if (const JsonValue *replace = part.get("replace_existing"); replace && replace->boolean != import.replace_existing) {
		import.replace_existing = replace->boolean;
		for (size_t i = 0; i < plan.rows.size(); ++i)
			if (plan.rows[i].held && import_row_refusal(plan, i).empty()) import.checked[i] = import.replace_existing;
		checks = true;
	}
	for (const char *member : { "check", "uncheck" }) {
		const JsonValue *rows = part.get(member);
		if (!rows) continue;
		const bool on = std::string(member) == "check";
		for (const JsonValue &token : rows->array) {
			const size_t index = size_t(token.number);
			if (index >= plan.rows.size())
				return change.refuse("import." + std::string(member) + " names the plan's rows by their index (0 to " +
				                     std::to_string(plan.rows.size() ? plan.rows.size() - 1 : 0) + "), not " +
				                     std::to_string(index) + ".");
			const ImportPlanRow &row = plan.rows[index];
			if (row.state == ImportPlanRow::State::NotFound && on)
				return change.refuse("import.check: " + row.name + " was found nowhere: nothing to import.");
			// A row the project cannot take is not checked (a chosen one can only be unchecked).
			if (on && !import_row_refusal(plan, index).empty())
				return change.refuse("import.check: " + row.name + " cannot be imported: " + import_row_refusal(plan, index));
			// The files one converter source makes come together.
			for (size_t i = 0; i < plan.rows.size(); ++i)
				if (i == index || (!row.made_from.empty() && plan.rows[i].made_from == row.made_from &&
				                   plan.rows[i].source == row.source))
					import.checked[i] = on;
			checks = true;
		}
	}
	WorkspaceView::Import &held = change.workspace().import;
	if (checks && import.checked != held.checked) ++import.serial;
	const bool moved = import.filter != held.filter || import.choice_kind != held.choice_kind ||
	                   import.rows_filter != held.rows_filter || import.kind_shown != held.kind_shown ||
	                   import.replace_existing != held.replace_existing || import.serial != held.serial;
	held = std::move(import);
	return moved;
}

bool same_problems(const WorkspaceView::Problems &a, const WorkspaceView::Problems &b) {
	return a.errors == b.errors && a.warnings == b.warnings && a.infos == b.infos && a.text == b.text &&
	       a.scope == b.scope && a.fixable == b.fixable && a.grouping == b.grouping && a.blocking == b.blocking &&
	       a.confirm_serial == b.confirm_serial;
}

// Problems' filters and its confirmation. Blocks the build turned on sets aside the filters that could hide
// a refusal (every severity, no text, the whole project, not only the fixable), kept until it is turned off,
// when they come back as they were with what changed of them meanwhile. A confirmation is asked only of what
// Problems offers now (propose_confirmation), its finding known by its key from then on.
bool set_problems(Change &change, const JsonValue &part) {
	WorkspaceView::Problems problems = change.workspace().problems;
	if (const JsonValue *severities = part.get("severities")) {
		problems.errors = problems.warnings = problems.infos = false;
		for (const JsonValue &token : severities->array) {
			DiagnosticSeverity severity = DiagnosticSeverity::Error;
			if (!diagnostic_severity_from_token(token.string, severity))
				return change.refuse("problems.severities takes error, warning and info, not \"" + token.string + "\".");
			(severity == DiagnosticSeverity::Error     ? problems.errors
			 : severity == DiagnosticSeverity::Warning ? problems.warnings
			                                           : problems.infos) = true;
		}
	}
	if (const JsonValue *text = part.get("text")) problems.text = text->string;
	if (const JsonValue *scope = part.get("scope"); scope && !problem_scope_from_token(scope->string, problems.scope))
		return change.refuse("problems.scope is project, active_file or open_files, not \"" + scope->string + "\".");
	if (const JsonValue *group = part.get("group"); group && !problem_grouping_from_token(group->string, problems.grouping))
		return change.refuse("problems.group is none, file or kind, not \"" + group->string + "\".");
	if (const JsonValue *fixable = part.get("fixable")) problems.fixable = fixable->boolean;
	if (const JsonValue *blocking = part.get("blocking"); blocking && blocking->boolean != problems.blocking) {
		if (blocking->boolean) {
			problems.before_blocking = WorkspaceView::Problems::Filters{ problems.errors, problems.warnings, problems.infos,
			                                                             problems.text, problems.scope, problems.fixable };
			problems.errors = problems.warnings = problems.infos = true;
			problems.text.clear();
			problems.scope = ProblemScope::Project;
			problems.fixable = false;
		} else if (problems.before_blocking) {
			const WorkspaceView::Problems::Filters before = *problems.before_blocking;
			problems.errors = before.errors;
			problems.warnings = before.warnings;
			problems.infos = before.infos;
			problems.text = before.text;
			problems.scope = before.scope;
			problems.fixable = before.fixable;
			problems.before_blocking.reset();
		}
		problems.blocking = blocking->boolean;
	}
	if (const JsonValue *confirm = part.get("confirm")) {
		WorkspaceView::Problems::Confirm asked;
		for (const io::JsonMember &member : confirm->object) {
			if (member.key == "finding") {
				if (!whole(member.value))
					return change.refuse("problems.confirm.finding is the index of a finding (the problems query's index), a whole number.");
				asked.finding = std::to_string(uint64_t(member.value.number));
				continue;
			}
			std::string *slot = member.key == "group" ? &asked.group : member.key == "required" ? &asked.required
			                    : member.key == "label"                                        ? &asked.label
			                                                                                    : nullptr;
			if (!slot) return change.refuse("problems.confirm takes group, required, or finding and label, not \"" + member.key + "\".");
			if (!member.value.is_string()) return change.refuse("problems.confirm." + member.key + " is a string.");
			*slot = member.value.string;
		}
		const int asks = int(!asked.group.empty()) + int(!asked.required.empty()) + int(!asked.finding.empty());
		if (asks > 1) return change.refuse("problems.confirm asks one confirmation: a group's, a required kind's or a finding's fix.");
		if (!asked.finding.empty() && asked.label.empty())
			return change.refuse("problems.confirm names the fix of the finding by its label.");
		if (asked.open() && !change.view.project.open) return change.closed("The project", "open one first");
		if (asked.open()) {
			if (!asked.finding.empty())
				asked.finding_key = problem_finding_key(change.view, size_t(strutil::parse_ulong(asked.finding).value_or(0)));
			// What Problems offers now, under the filters this change leaves: a confirmation of nothing is refused.
			ProblemQueryCache answers;
			ProblemFixCache fixes;
			ConfirmationProposal proposal;
			std::string why;
			if (!propose_confirmation(change.view, asked, problems, answers, fixes, proposal, why)) return change.refuse(why);
		}
		problems.confirm = asked;
		++problems.confirm_serial;
	}
	if (same_problems(problems, change.workspace().problems)) return false;
	change.workspace().problems = std::move(problems);
	return true;
}

// What a document's views show of it: the document named (the active one when none is), open; each member
// what the document's views take of it (kinds and every row a mission's outline, a window type its menu's
// TYPE, a Remove prompt a screen of its menu, a remap a PCX texture's).
bool set_document(Change &change, const JsonValue &part) {
	const JsonValue *path = part.get("path");
	const std::string named = path ? path->string : std::string();
	const DocumentBase *document = open_document(change.view, named);
	if (!document)
		return change.refuse(named.empty() ? std::string("No document is active.") : "No document is open at " + named + ".", named);
	const std::string &at = document->path();
	WorkspaceView::DocumentView shown = change.workspace().document(at);
	if (const JsonValue *filter = part.get("filter")) shown.filter = filter->string;
	const bool mission = document->kind() == AssetKind::Mission;
	if ((part.get("kinds") || part.get("all_rows")) && !mission)
		return change.refuse(at + " lists no kinds of row to choose and no rows left out (a mission's outline does).", at);
	if (const JsonValue *kinds = part.get("kinds")) {
		const Document *records = records_of(*document);
		if (!records) return change.refuse(at + " lists no kinds of row.", at);
		const std::vector<RecordKindRow> &rows = records->kinds();
		const size_t count = std::min<size_t>(rows.size(), 64);
		uint64_t mask = 0;
		for (const JsonValue &token : kinds->array) {
			size_t found = count;
			for (size_t i = 0; i < count; ++i)
				if (token.string == rows[i].token) found = i;
			if (found == count) return change.refuse(at + " has no kind of row \"" + token.string + "\".", at);
			mask |= uint64_t(1) << found;
		}
		const uint64_t all = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
		shown.kinds = (mask & all) == all ? ~uint64_t(0) : mask;
	}
	if (const JsonValue *all_rows = part.get("all_rows")) shown.all_rows = all_rows->boolean;
	if (const JsonValue *sort = part.get("sort")) shown.sort = sort->boolean;
	if (const JsonValue *every = part.get("every")) shown.every = every->boolean;
	if (const JsonValue *filter = part.get("inspector_filter")) shown.inspector_filter = filter->string;
	if (const JsonValue *type = part.get("new_window_type")) {
		const MnuDocument *menu = menu_of(*document);
		const FieldSchema *field = menu ? window_type_field(*menu) : nullptr;
		if (!field) return change.refuse(at + " is no menu: a window type is a menu's.", at);
		std::string names;
		bool known = false;
		for (const FieldChoice &choice : field->choices) {
			known = known || choice.name == type->string;
			names += (names.empty() ? "" : ", ") + choice.name;
		}
		if (!known) return change.refuse("A menu's window type is one of " + names + ", not \"" + type->string + "\".", at);
		shown.new_window_type = type->string;
	}
	if (const JsonValue *screen = part.get("remove_screen")) {
		const uint64_t id = uint64_t(screen->number);
		if (id != 0 && !menu_of(*document)) return change.refuse(at + " is no menu: Remove screen is a menu's.", at);
		if (id != 0 && !removable_screen(*document, id))
			return change.refuse(menu_of(*document)->row(NodeId(id)) ? at + " keeps at least one screen: its last is not removed."
			                                                          : at + " has no screen " + std::to_string(id) + ".",
			                     at);
		shown.remove_screen = id;
	}
	for (const char *member : { "remap_from", "remap_to" }) {
		const JsonValue *index = part.get(member);
		if (!index) continue;
		const size_t dot = at.rfind('.');
		if (document->kind() != AssetKind::Texture || dot == std::string::npos || strutil::to_lower(at.substr(dot)) != ".pcx")
			return change.refuse(at + " is no 8-bit PCX texture: a palette remap is one's.", at);
		if (index->number > 255.0) return change.refuse(std::string("document.") + member + " is a palette index, 0 to 255.", at);
		(std::string(member) == "remap_from" ? shown.remap_from : shown.remap_to) = int(index->number);
	}
	WorkspaceView::DocumentView &held = change.workspace().documents[at];
	const bool moved = held.filter != shown.filter || held.kinds != shown.kinds || held.all_rows != shown.all_rows ||
	                   held.sort != shown.sort || held.every != shown.every || held.inspector_filter != shown.inspector_filter ||
	                   held.new_window_type != shown.new_window_type || held.remove_screen != shown.remove_screen ||
	                   held.remap_from != shown.remap_from || held.remap_to != shown.remap_to;
	held = std::move(shown);
	return moved;
}

// `focus`: the window it names opened, shown where it stands aside, and brought forward (a focus_window view event
// the workspace takes, as the Windows menu's tick). The workspace holds nothing of it: the events concern moves with
// the post.
bool focus_window(Change &change, const std::string &token) {
	ViewEvent focus;
	focus.kind = ViewEventKind::FocusWindow;
	focus.path = token;
	change.view.events.post(std::move(focus));
	return false;
}

// What the workspace holds open, for telling a dialog opened (or opened on another target) since.
struct Openings {
	bool new_project = false, settings = false, rename_back = false;
	std::string project_find; // its scope and subject while open
	AssetKind new_file = AssetKind::kCount;
	std::string file_rename, file_delete, rename;
	uint64_t confirm = 0;
	std::map<std::string, uint64_t> remove;
};
Openings openings_of(const WorkspaceView &w) {
	Openings out;
	out.new_project = w.new_project.open;
	out.settings = w.settings.open;
	if (w.project_find.open)
		out.project_find = std::string(find_scope_token(w.project_find.scope)) + '\n' + w.project_find.path + '\n' +
		                   w.project_find.locator;
	out.rename_back = w.rename_back.open;
	out.new_file = w.new_file.kind;
	out.file_rename = w.file_rename.path;
	out.file_delete = w.file_delete.path;
	if (w.rename.open) out.rename = w.rename.path + '\n' + w.rename.locator + '\n' + w.rename.field;
	out.confirm = w.problems.confirm.open() ? w.problems.confirm_serial : 0;
	for (const auto &[path, shown] : w.documents)
		if (shown.remove_screen) out.remove.emplace(path, shown.remove_screen);
	return out;
}
bool opened_since(const Openings &before, const Openings &after) {
	if ((after.new_project && !before.new_project) || (after.settings && !before.settings) ||
	    (after.rename_back && !before.rename_back))
		return true;
	if (!after.project_find.empty() && after.project_find != before.project_find) return true;
	if (after.new_file != AssetKind::kCount && after.new_file != before.new_file) return true;
	if (!after.file_rename.empty() && after.file_rename != before.file_rename) return true;
	if (!after.file_delete.empty() && after.file_delete != before.file_delete) return true;
	if (!after.rename.empty() && after.rename != before.rename) return true;
	if (after.confirm && after.confirm != before.confirm) return true;
	for (const auto &[path, screen] : after.remove) {
		const auto was = before.remove.find(path);
		if (was == before.remove.end() || was->second != screen) return true;
	}
	return false;
}

JsonValue sound_json(const WorkspaceView::Sound &sound) {
	JsonValue out = JsonValue::make_object();
	out.set("path", text(sound.path));
	out.set("state", text(sound_state_token(sound.state)));
	out.set("serial", number(double(sound.serial)));
	if (!sound.error.empty()) out.set("error", text(sound.error));
	// A set's or a slot's play (the sound lane): the set, its bank, what it plays in words, and each voice.
	if (!sound.set.empty()) out.set("set", text(sound.set));
	if (!sound.bank.empty()) out.set("bank", text(sound.bank));
	if (!sound.words.empty()) out.set("words", text(sound.words));
	JsonValue voices = JsonValue::make_array();
	for (const WorkspaceView::Voice &voice : sound.voices) {
		JsonValue item = JsonValue::make_object();
		item.set("path", text(voice.path));
		item.set("pitch", number(double(voice.pitch_q16) / 65536.0));
		item.set("volume", number(double(voice.volume)));
		// A voice that starts after the play does (a dialog's later lines, DI-32): when, in seconds.
		if (voice.start_ms > 0) item.set("at", number(double(voice.start_ms) / 1000.0));
		voices.push(std::move(item));
	}
	out.set("voices", std::move(voices));
	return out;
}

JsonValue kind_json(AssetKind kind) { return text(kind == AssetKind::kCount ? "" : asset_kind_token(kind)); }

JsonValue document_json(const SessionView &view, const DocumentBase &document, const WorkspaceView::DocumentView &shown) {
	JsonValue out = JsonValue::make_object();
	out.set("path", text(document.path()));
	out.set("filter", text(shown.filter));
	if (const Document *records = records_of(document)) {
		JsonValue kinds = JsonValue::make_array();
		const std::vector<RecordKindRow> &rows = records->kinds();
		for (size_t i = 0; i < rows.size() && i < 64; ++i)
			if (shown.kinds & (uint64_t(1) << i)) kinds.push(text(rows[i].token));
		out.set("kinds", std::move(kinds));
	}
	out.set("all_rows", flag(shown.all_rows));
	out.set("sort", flag(shown.sort));
	out.set("every", flag(shown.every));
	out.set("inspector_filter", text(shown.inspector_filter));
	out.set("new_window_type", text(shown.new_window_type));
	out.set("remove_screen", number(double(shown.remove_screen)));
	out.set("remap_from", number(shown.remap_from));
	out.set("remap_to", number(shown.remap_to));
	out.set("active", flag(document.path() == view.documents.active));
	return out;
}

// A row's identity across plans: the source it comes from and where it lands.
std::string row_identity(const ImportPlanRow &row) {
	return row.source.path + '\n' + row.source.entry + '\n' + (row.source.install ? '1' : '0') + (row.source.native ? '1' : '0') +
	       '\n' + row.source.as + '\n' + row.destination;
}

} // namespace

const char *workspace_json_token(WorkspaceJson json) {
	switch (json) {
		case J::String:
			return "string";
		case J::Boolean:
			return "boolean";
		case J::Integer:
			return "integer";
		case J::Strings:
			return "string[]";
		case J::Integers:
			return "integer[]";
		case J::Object:
			return "object";
	}
	return "string";
}

const WorkspacePartRow *workspace_parts(size_t &count) {
	count = std::size(kParts);
	return kParts;
}

const char *workspace_window_token(size_t index) { return index < std::size(kWindows) ? kWindows[index].token : nullptr; }
const char *workspace_window_title(size_t index) { return index < std::size(kWindows) ? kWindows[index].title : nullptr; }

bool check_workspace_change(const JsonValue &json, std::string &error) {
	if (!json.is_object()) {
		error = "\"workspace\" is an object of its parts (" + parts_taken() + ").";
		return false;
	}
	for (const io::JsonMember &member : json.object) {
		if (member.key == "focus") {
			bool known = false;
			std::string windows;
			for (size_t i = 0; const char *token = workspace_window_token(i); ++i) {
				known = known || (member.value.is_string() && member.value.string == token);
				windows += (i ? ", " : "") + std::string(token);
			}
			if (!known) {
				error = "\"workspace.focus\" names a window: " + windows + ".";
				return false;
			}
			continue;
		}
		const WorkspacePartRow *part = part_of(member.key);
		if (!part) {
			error = "the workspace has no part \"" + member.key + "\" (it has " + parts_taken() + ").";
			return false;
		}
		if (!member.value.is_object()) {
			error = "\"workspace." + member.key + "\" is an object of its members (" + tokens_of(*part) + ").";
			return false;
		}
		for (const io::JsonMember &value : member.value.object) {
			const WorkspaceMember *row = member_of(*part, value.key);
			if (!row) {
				error = "\"workspace." + member.key + "\" takes no \"" + value.key + "\" (it takes " + tokens_of(*part) + ").";
				return false;
			}
			if (!typed(value.value, row->json)) {
				error = "\"workspace." + member.key + "." + value.key + "\" must be " + type_words(row->json) + ".";
				return false;
			}
			// What the window's field holds at most (its buffer): a longer text would show, and come back, cut.
			if (row->longest && longest_of(value.value) > row->longest) {
				error = "\"workspace." + member.key + "." + value.key + "\" is at most " + std::to_string(row->longest) +
				        " characters (what its window's field holds).";
				return false;
			}
		}
	}
	return true;
}

bool apply_workspace_change(SessionView &view, const std::string &json, std::vector<WorkspaceRefusal> &refusals) {
	JsonValue parsed;
	std::string error;
	if (!io::json_parse(json, parsed, error)) {
		refusals.push_back(WorkspaceRefusal{ "The workspace's change is not JSON: " + error, std::string() });
		return false;
	}
	if (!check_workspace_change(parsed, error)) {
		refusals.push_back(WorkspaceRefusal{ "The workspace's change " + error, std::string() });
		return false;
	}
	Change change{ view, refusals };
	using Setter = bool (*)(Change &, const JsonValue &);
	struct Part {
		const char *token;
		Setter set;
	};
	static constexpr Part kSetters[] = {
		{ "card", set_card },
		{ "build_result", set_build_result },
		{ "new_project", set_new_project },
		{ "settings", set_settings },
		{ "new_file", set_new_file },
		{ "file_rename", set_file_rename },
		{ "file_delete", set_file_delete },
		{ "rename", set_rename },
		{ "rename_back", set_rename_back },
		{ "find", set_find },
		{ "project_find", set_project_find },
		{ "files", set_files },
		{ "import", set_import },
		{ "problems", set_problems },
		{ "document", set_document },
	};
	static_assert(std::size(kSetters) == std::size(kParts), "a setter per part of the table");
	const Openings before = openings_of(view.workspace);
	bool moved = false;
	for (const io::JsonMember &member : parsed.object) {
		if (member.key == "focus") {
			moved = focus_window(change, member.value.string) || moved;
			continue;
		}
		for (const Part &part : kSetters)
			if (member.key == part.token) moved = part.set(change, member.value) || moved;
	}
	if (moved && opened_since(before, openings_of(view.workspace))) ++view.workspace.opened;
	return moved;
}

void set_workspace(SessionCore &core, const std::string &change) {
	std::vector<WorkspaceRefusal> refusals;
	const uint64_t events = core.view().events.next_seq();
	const bool moved = apply_workspace_change(core.view(), change, refusals);
	for (const WorkspaceRefusal &refusal : refusals) core.refuse_quietly(CoreFinding::WorkspaceRefused, refusal.message, refusal.asset);
	if (moved) core.touch(ViewConcern::Workspace);
	// A focus posted (a focus_window event): the events move with the dialogs' concern, as a request's ask does;
	// the workspace holds nothing of it.
	if (core.view().events.next_seq() != events) core.touch(ViewConcern::Dialogs);
}

void workspace_follows(SessionCore &core, const EditorRequest &request) {
	SessionView &view = core.view();
	WorkspaceView &workspace = view.workspace;
	const Openings before = openings_of(workspace);
	bool moved = false;
	switch (request.kind) {
		case EditorRequestKind::ShowInFiles:
			// Rename... asked of the file Files shows: its name starting as the file's.
			if (request.ask_name)
				if (const AssetEntry *file = project_file(view, request.path)) {
					moved = workspace.file_rename.path != file->relative_path || workspace.file_rename.name != file->logical_name;
					workspace.file_rename = WorkspaceView::FileRename{ file->relative_path, file->logical_name };
				}
			break;
		case EditorRequestKind::PreviewRename: {
			const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
			if (!plan.symbol || plan.back) break;
			WorkspaceView::Rename rename = workspace.rename;
			// One that asks the name opens Rename everywhere over it; a plan of the open one's name (typed, or
			// asked over the wire) is the name typed.
			if (request.ask_name) open_rename(rename, plan);
			else if (rename.open && plan_of(rename, plan)) rename.name = plan.requested;
			moved = !same_rename(rename, workspace.rename);
			workspace.rename = std::move(rename);
			break;
		}
		case EditorRequestKind::PreviewRenameBack:
			if (request.ask_name && view.dialogs.rename_preview.back && !workspace.rename_back.open) {
				workspace.rename_back.open = true;
				moved = true;
			}
			break;
		default:
			break;
	}
	if (moved && opened_since(before, openings_of(workspace))) ++workspace.opened;
	if (moved) core.touch(ViewConcern::Workspace);
}

void workspace_closes_for(SessionCore &core, const EditorRequest &request) {
	SessionView &view = core.view();
	WorkspaceView &workspace = view.workspace;
	bool moved = false;
	switch (request.kind) {
		case EditorRequestKind::RenameSymbol:
			moved = workspace.rename.open;
			workspace.rename = WorkspaceView::Rename();
			break;
		case EditorRequestKind::RenameBack:
			moved = workspace.rename_back.open;
			workspace.rename_back.open = false;
			break;
		case EditorRequestKind::RenameAsset: {
			// Rename...'s Rename: the file it names renamed (or refused, the reasons in Problems).
			const AssetEntry *file = project_file(view, request.path);
			if (!workspace.file_rename.path.empty() && workspace.file_rename.path == (file ? file->relative_path : request.path)) {
				workspace.file_rename = WorkspaceView::FileRename();
				moved = true;
			}
			break;
		}
		case EditorRequestKind::DeleteAsset: {
			// Delete...'s Delete: the file it names deleted (or refused, the reasons in the outcome).
			const AssetEntry *file = project_file(view, request.path);
			if (!workspace.file_delete.path.empty() && workspace.file_delete.path == (file ? file->relative_path : request.path)) {
				workspace.file_delete = WorkspaceView::FileDelete();
				moved = true;
			}
			break;
		}
		case EditorRequestKind::CreateFile:
			// The prompt's Create: the file it names made (or refused, the reasons in Problems).
			if (workspace.new_file.kind != AssetKind::kCount && request.path == workspace.new_file.name) {
				workspace.new_file = WorkspaceView::NewFile();
				moved = true;
			}
			break;
		case EditorRequestKind::NewProject:
			// Create project: the form's modal closes as the session takes it (made, refused, or waiting on the
			// unsaved prompt, which then shows alone); its fields stay for File > New project... to open on again.
			moved = workspace.new_project.open;
			workspace.new_project.open = false;
			break;
		default:
			break;
	}
	if (moved) core.touch(ViewConcern::Workspace);
}

void workspace_tidies(SessionCore &core) {
	SessionView &view = core.view();
	WorkspaceView &workspace = view.workspace;
	bool moved = false;
	if (!workspace.card.path.empty() && !project_file(view, workspace.card.path)) {
		workspace.card.path.clear();
		stop_sound(workspace.sound);
		moved = true;
	}
	if (!workspace.file_rename.path.empty() && !project_file(view, workspace.file_rename.path)) {
		workspace.file_rename = WorkspaceView::FileRename();
		moved = true;
	}
	if (!workspace.file_delete.path.empty() && !project_file(view, workspace.file_delete.path)) {
		workspace.file_delete = WorkspaceView::FileDelete();
		moved = true;
	}
	// Find usages of a file the files no longer have closes.
	WorkspaceView::ProjectFind &find = workspace.project_find;
	if (find.open && find.scope == WorkspaceView::FindScope::Usages && !project_file(view, find.path)) {
		find.open = false;
		moved = true;
	}
	for (auto &[path, shown] : workspace.documents) {
		if (!shown.remove_screen) continue;
		const DocumentBase *document = open_document(view, path);
		if (document && removable_screen(*document, shown.remove_screen)) continue;
		shown.remove_screen = 0;
		moved = true;
	}
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	if (workspace.rename.open && !plan_of(workspace.rename, plan)) {
		workspace.rename = WorkspaceView::Rename();
		moved = true;
	}
	if (workspace.rename_back.open && !plan.back) {
		workspace.rename_back.open = false;
		moved = true;
	}
	if (moved) core.touch(ViewConcern::Workspace);
}

bool workspace_follows_moves(WorkspaceView &workspace, const std::vector<std::pair<std::string, std::string>> &moved) {
	bool followed = false;
	for (const auto &[from, to] : moved) {
		if (!workspace.card.path.empty() && workspace.card.path == from) {
			workspace.card.path = to;
			followed = true;
		}
		// Find usages of a file renamed lists the uses of it under its new name.
		if (!workspace.project_find.path.empty() && workspace.project_find.path == from) {
			workspace.project_find.path = to;
			followed = true;
		}
	}
	return followed;
}

ShownModal shown_modal(const SessionView &view) {
	const WorkspaceView &w = view.workspace;
	const bool project = view.project.open;
	using M = HeldModal;
	if (view.dialogs.unsaved_prompt.open) return { M::Unsaved, {} };
	if (view.dialogs.import_preview.open) return { M::Import, {} };
	if (view.dialogs.texture_source.open && project) return { M::TextureSource, {} };
	if (w.settings.open && project) return { M::Settings, {} };
	if (w.new_project.open) return { M::NewProject, {} };
	if (w.new_file.kind != AssetKind::kCount && project) return { M::NewFile, {} };
	if (!w.file_rename.path.empty() && project) return { M::FileRename, {} };
	if (!w.file_delete.path.empty() && project) return { M::FileDelete, {} };
	if (w.rename.open && project) return { M::Rename, {} };
	if (w.rename_back.open && project && view.dialogs.rename_preview.back) return { M::RenameBack, {} };
	if (w.project_find.open && project) return { M::ProjectFind, {} };
	if (w.problems.confirm.open() && project) return { M::Confirm, {} };
	for (const auto &[path, shown] : w.documents)
		if (shown.remove_screen) return { M::RemoveScreen, path };
	return {};
}

bool modal_may_show(const SessionView &view, HeldModal modal, const std::string &path) {
	const ShownModal shown = shown_modal(view);
	return shown.modal == HeldModal::None || (shown.modal == modal && shown.path == path);
}

const char *held_modal_token(HeldModal modal) {
	switch (modal) {
		case HeldModal::None: return "";
		case HeldModal::Unsaved: return "unsaved";
		case HeldModal::Import: return "import";
		case HeldModal::TextureSource: return "texture_source";
		case HeldModal::Settings: return "settings";
		case HeldModal::NewProject: return "new_project";
		case HeldModal::NewFile: return "new_file";
		case HeldModal::FileRename: return "file_rename";
		case HeldModal::FileDelete: return "file_delete";
		case HeldModal::Rename: return "rename";
		case HeldModal::RenameBack: return "rename_back";
		case HeldModal::ProjectFind: return "project_find";
		case HeldModal::Confirm: return "confirm";
		case HeldModal::RemoveScreen: return "remove_screen";
	}
	return "";
}

JsonValue workspace_to_json(const SessionView &view) {
	const WorkspaceView &workspace = view.workspace;
	JsonValue out = JsonValue::make_object();
	// The dialog that shows of those that take the whole editor (shown_modal), "" none; a Remove prompt's menu.
	const ShownModal modal = shown_modal(view);
	out.set("modal", text(held_modal_token(modal.modal)));
	if (!modal.path.empty()) out.set("modal_document", text(modal.path));
	out.set("opened", number(double(workspace.opened)));
	JsonValue card = JsonValue::make_object();
	card.set("path", text(workspace.card.path));
	out.set("card", std::move(card));
	out.set("sound", sound_json(workspace.sound));
	JsonValue build = JsonValue::make_object();
	build.set("open", flag(workspace.build_result.open));
	out.set("build_result", std::move(build));
	const WorkspaceView::NewProject &form = workspace.new_project;
	JsonValue made = JsonValue::make_object();
	made.set("open", flag(form.open));
	made.set("title", text(form.title));
	made.set("dir", text(form.dir));
	// As the form shows it: the one named, else the editor's own.
	made.set("game_install", text(form.install_named ? form.game_install : view.project.editor_install));
	made.set("install_named", flag(form.install_named));
	made.set("builds_on", text(form.builds_on));
	made.set("as_expansion", flag(form.as_expansion));
	made.set("expansion", text(form.expansion));
	out.set("new_project", std::move(made));
	const WorkspaceView::Settings &settings = workspace.settings;
	JsonValue dialog = JsonValue::make_object();
	dialog.set("open", flag(settings.open));
	if (settings.open) {
		dialog.set("title", text(settings.title));
		dialog.set("mission", flag(settings.mission));
		dialog.set("multiplayer", flag(settings.multiplayer));
		dialog.set("builds_on", text(settings.builds_on));
		dialog.set("as_expansion", flag(settings.as_expansion));
		dialog.set("expansion", text(settings.expansion));
		dialog.set("game_install", text(settings.game_install));
		dialog.set("runtime", text(settings.runtime));
		dialog.set("play_mode", text(play_mode_token(settings.play_mode)));
	}
	out.set("settings", std::move(dialog));
	JsonValue prompt = JsonValue::make_object();
	prompt.set("kind", kind_json(workspace.new_file.kind));
	if (workspace.new_file.kind != AssetKind::kCount) {
		prompt.set("name", text(workspace.new_file.name));
		JsonValue values = JsonValue::make_object();
		for (const auto &[token, value] : workspace.new_file.values) values.set(token, text(value));
		prompt.set("values", std::move(values));
		prompt.set("folder", text(workspace.new_file.folder));
	}
	out.set("new_file", std::move(prompt));
	JsonValue file_rename = JsonValue::make_object();
	file_rename.set("path", text(workspace.file_rename.path));
	if (!workspace.file_rename.path.empty()) file_rename.set("name", text(workspace.file_rename.name));
	out.set("file_rename", std::move(file_rename));
	JsonValue file_delete = JsonValue::make_object();
	file_delete.set("path", text(workspace.file_delete.path));
	if (!workspace.file_delete.path.empty()) file_delete.set("alone", flag(workspace.file_delete.alone));
	out.set("file_delete", std::move(file_delete));
	JsonValue rename = JsonValue::make_object();
	rename.set("open", flag(workspace.rename.open));
	if (workspace.rename.open) {
		rename.set("path", text(workspace.rename.path));
		rename.set("locator", text(workspace.rename.locator));
		rename.set("field", text(workspace.rename.field));
		rename.set("old_name", text(workspace.rename.old_name));
		rename.set("kind", text(reference_row(workspace.rename.kind).token));
		rename.set("name", text(workspace.rename.name));
	}
	out.set("rename", std::move(rename));
	JsonValue back = JsonValue::make_object();
	back.set("open", flag(workspace.rename_back.open));
	out.set("rename_back", std::move(back));
	JsonValue find = JsonValue::make_object();
	find.set("open", flag(workspace.find.open));
	find.set("text", text(workspace.find.text));
	find.set("match_case", flag(workspace.find.match_case));
	out.set("find", std::move(find));
	JsonValue project_find = JsonValue::make_object();
	project_find.set("open", flag(workspace.project_find.open));
	project_find.set("text", text(workspace.project_find.text));
	project_find.set("scope", text(find_scope_token(workspace.project_find.scope)));
	if (workspace.project_find.scope == WorkspaceView::FindScope::Usages) {
		project_find.set("path", text(workspace.project_find.path));
		project_find.set("locator", text(workspace.project_find.locator));
	}
	out.set("project_find", std::move(project_find));
	JsonValue files = JsonValue::make_object();
	files.set("filter", text(workspace.files.filter));
	files.set("kind", kind_json(workspace.files.kind));
	files.set("by_cost", flag(workspace.files.by_cost));
	out.set("files", std::move(files));
	// The import dialog's, with the checks counted (the import_preview query pages each row's) and the plan they
	// index.
	const WorkspaceView::Import &import = workspace.import;
	JsonValue dialog_import = JsonValue::make_object();
	dialog_import.set("filter", text(import.filter));
	dialog_import.set("choice_kind", kind_json(import.choice_kind));
	dialog_import.set("rows_filter", text(import.rows_filter));
	dialog_import.set("kind_shown", kind_json(import.kind_shown));
	dialog_import.set("replace_existing", flag(import.replace_existing));
	size_t checked = 0;
	for (const bool on : import.checked) checked += on ? 1 : 0;
	dialog_import.set("checked", number(double(checked)));
	dialog_import.set("serial", number(double(import.serial)));
	dialog_import.set("plan", number(double(view.dialogs.import_preview.open ? view.dialogs.import_preview.plan_serial : 0)));
	out.set("import", std::move(dialog_import));
	const WorkspaceView::Problems &problems = workspace.problems;
	JsonValue filters = JsonValue::make_object();
	JsonValue severities = JsonValue::make_array();
	for (const auto &[on, severity] : { std::make_pair(problems.errors, DiagnosticSeverity::Error),
	                                    std::make_pair(problems.warnings, DiagnosticSeverity::Warning),
	                                    std::make_pair(problems.infos, DiagnosticSeverity::Info) })
		if (on) severities.push(text(diagnostic_severity_label(severity)));
	filters.set("severities", std::move(severities));
	filters.set("text", text(problems.text));
	filters.set("scope", text(problem_scope_token(problems.scope)));
	filters.set("group", text(problem_grouping_token(problems.grouping)));
	filters.set("fixable", flag(problems.fixable));
	filters.set("blocking", flag(problems.blocking));
	JsonValue confirm = JsonValue::make_object();
	for (const auto &[token, value] : { std::make_pair("group", &problems.confirm.group), std::make_pair("required", &problems.confirm.required),
	                                    std::make_pair("label", &problems.confirm.label) })
		if (!value->empty()) confirm.set(token, text(*value));
	if (!problems.confirm.finding.empty()) {
		// The finding where it is now (a validation moves the findings: its key finds it).
		const size_t at = problem_finding_at(view, problems.confirm.finding_key);
		confirm.set("finding", number(double(at != SIZE_MAX ? at : strutil::parse_ulong(problems.confirm.finding).value_or(0))));
	}
	// What its Apply raises now (apply_confirmation), and what it says.
	if (problems.confirm.open()) {
		ProblemQueryCache answers;
		ProblemFixCache fixes;
		ConfirmationProposal proposal;
		std::string why;
		JsonValue proposed = JsonValue::make_object();
		if (propose_confirmation(view, problems.confirm, problems, answers, fixes, proposal, why)) {
			JsonValue lines = JsonValue::make_array();
			for (const std::string &line : proposal.lines) lines.push(text(line));
			proposed.set("lines", std::move(lines));
			JsonValue requests = JsonValue::make_array();
			for (const EditorRequest &request : proposal.requests) requests.push(editor_request_to_json(request));
			proposed.set("requests", std::move(requests));
			proposed.set("findings", number(double(proposal.findings)));
		} else {
			proposed.set("gone", text(why));
		}
		confirm.set("proposal", std::move(proposed));
	}
	filters.set("confirm", std::move(confirm));
	filters.set("confirm_serial", number(double(problems.confirm_serial)));
	out.set("problems", std::move(filters));
	// Every open document's, the active one's marked.
	JsonValue documents = JsonValue::make_array();
	for (const auto &document : view.documents.open)
		if (document) documents.push(document_json(view, *document, workspace.document(document->path())));
	out.set("documents", std::move(documents));
	return out;
}

bool show_card(WorkspaceView &workspace, const std::string &path) {
	if (workspace.card.path == path) return false;
	if (!workspace.card.path.empty()) stop_sound(workspace.sound);
	workspace.card.path = path;
	return true;
}

void play_sound(SessionCore &core, const std::string &path) {
	const SessionView &view = core.view();
	const AssetEntry *entry = project_file(view, path);
	if (!entry || entry->kind != AssetKind::Wave)
		return core.refuse_now(CoreFinding::WorkspaceRefused, "No wave of the project plays as " + path + ".", path);
	if (entry->size_bytes > kWaveCardBytes)
		return core.refuse_now(CoreFinding::WorkspaceRefused,
		                       entry->relative_path + " is too large to play here (" + std::to_string(entry->size_bytes >> 20) + " MB).",
		                       entry->relative_path);
	WorkspaceView::Sound &sound = core.view().workspace.sound;
	sound.path = entry->relative_path;
	// One voice, as recorded (a set's play has one a layer: session/sound_play.h).
	sound.voices = {{entry->relative_path, 0x10000, 255}};
	sound.set.clear();
	sound.bank.clear();
	sound.words.clear();
	++sound.serial;
	sound.state = WorkspaceView::SoundState::Starting;
	sound.error.clear();
	core.touch(ViewConcern::Workspace);
}

void stop_sound(SessionCore &core) {
	if (stop_sound(core.view().workspace.sound)) core.touch(ViewConcern::Workspace);
}

bool report_sound(WorkspaceView &workspace, uint64_t serial, WorkspaceView::SoundState state, const std::string &error) {
	WorkspaceView::Sound &sound = workspace.sound;
	if (serial != sound.serial || (sound.state != WorkspaceView::SoundState::Starting && sound.state != WorkspaceView::SoundState::Playing))
		return false;
	if (sound.state == state && sound.error == error) return false;
	sound.state = state;
	sound.error = error;
	return true;
}

bool forget_document_workspace(WorkspaceView &workspace, const std::string &path) { return workspace.documents.erase(path) != 0; }

void take_import_checks(WorkspaceView &workspace, const ImportPlan &plan, const ImportPlan *before) {
	std::vector<bool> checks = import_default_checks(plan, workspace.import.replace_existing);
	if (before && workspace.import.checked.size() == before->rows.size() && !before->rows.empty()) {
		std::map<std::string, bool> held;
		for (size_t i = 0; i < before->rows.size(); ++i) held.emplace(row_identity(before->rows[i]), bool(workspace.import.checked[i]));
		for (size_t i = 0; i < plan.rows.size(); ++i) {
			const auto was = held.find(row_identity(plan.rows[i]));
			if (was == held.end()) continue;
			// Unchecked stays unchecked; checked stays checked where the project can take it (a chosen file the
			// project cannot take is checked as a plan's own are, so the import waits on it).
			checks[i] = was->second && (checks[i] || import_row_refusal(plan, i).empty());
		}
	}
	workspace.import.checked = std::move(checks);
	++workspace.import.serial;
}

void forget_import_workspace(WorkspaceView &workspace) {
	const uint64_t serial = workspace.import.serial;
	workspace.import = WorkspaceView::Import();
	workspace.import.serial = serial + 1;
}

void forget_project_workspace(WorkspaceView &workspace) {
	workspace.card = WorkspaceView::Card();
	workspace.build_result = WorkspaceView::BuildResult();
	workspace.settings = WorkspaceView::Settings();
	workspace.new_file = WorkspaceView::NewFile();
	workspace.file_rename = WorkspaceView::FileRename();
	workspace.file_delete = WorkspaceView::FileDelete();
	workspace.rename = WorkspaceView::Rename();
	workspace.rename_back = WorkspaceView::RenameBack();
	workspace.find.open = false;
	workspace.project_find.open = false;
	// Find usages' subject is the project's: the finder opens again on another project's files in Find in project.
	if (workspace.project_find.scope == WorkspaceView::FindScope::Usages) {
		workspace.project_find.scope = WorkspaceView::FindScope::All;
		workspace.project_find.path.clear();
		workspace.project_find.locator.clear();
	}
	// Another project lists its own files: the filter and the kind start afresh.
	workspace.files = WorkspaceView::Files();
	workspace.problems.confirm = WorkspaceView::Problems::Confirm();
	++workspace.problems.confirm_serial;
	workspace.documents.clear();
	stop_sound(workspace.sound);
}

} // namespace opennova::editor
