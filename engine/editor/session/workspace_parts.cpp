#include <editor/session/workspace_parts.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/import/import_plan.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/file_card.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using J = WorkspaceJson;

JsonValue text(const std::string &value) { return JsonValue::make_string(value); }
JsonValue flag(bool value) { return JsonValue::make_bool(value); }

// --- the table ----------------------------------------------------------------------------------

constexpr WorkspaceMember kCard[] = {
	{ "path", J::String,
			"The project file whose card shows (a project-relative path or a logical name; \"\" closes it, and stops "
			"the sound it played): what it is, where a build puts it, what it names and who names it, a wave's "
			"sound. about_file opens it too, Files coming forward." },
};
constexpr WorkspaceMember kBuildResult[] = {
	{ "open", J::Boolean,
			"The build result's panel shows (what the last build came to: its folder, what refused it). A build's "
			"end opens it, unless a Play waits on the build." },
};
constexpr WorkspaceMember kNewProject[] = {
	{ "open", J::Boolean,
			"File > New project...'s modal is open (the welcome page shows the form whenever no project is "
			"open). A new_project that makes the project closes it." },
	{ "title", J::String, "The form's Name." },
	{ "dir", J::String, "The form's Folder: where the project is made." },
	{ "game_install", J::String,
			"The form's Game install, named (left unnamed, the form shows the editor's own); the window checks it "
			"as typing stops (check_install), its words under the field." },
	{ "builds_on", J::String,
			"The installed expansion it builds on, by its folder's name (\"\" the base game); building on one "
			"builds as an expansion." },
	{ "as_expansion", J::Boolean, "Build as an expansion: the project builds as expansion\\<expansion>\\." },
	{ "expansion", J::String, "The expansion's name, which the form checks as the game's rule takes it." },
};
constexpr WorkspaceMember kSettings[] = {
	{ "open", J::Boolean,
			"File > Project settings... is open (a project open). Opening it fills its fields with the settings in "
			"effect, before the other members the change names; its Apply is apply_project_settings with a "
			"serial, which closes it once every setting is written." },
	{ "title", J::String, "The project's name." },
	{ "mission", J::Boolean, "Missions: the project holds missions." },
	{ "multiplayer", J::Boolean, "Multiplayer." },
	{ "builds_on", J::String, "The installed expansion it builds on (\"\" the base game); building on one builds as one." },
	{ "as_expansion", J::Boolean, "Build as an expansion." },
	{ "expansion", J::String, "The expansion's name." },
	{ "game_install", J::String, "This computer's game install folder." },
	{ "runtime", J::String, "The OpenNova runtime Play runs (\"\" the one packaged beside the editor)." },
	{ "play_in_install", J::Boolean, "Play in the game install." },
};
constexpr WorkspaceMember kNewFile[] = {
	{ "kind", J::String,
			"The kind of file the prompt makes, an asset kind's token (Files' New > a kind...: a string table, a "
			"menu, a font, a mission; \"\" closes it). Opening it empties its name and values." },
	{ "name", J::String, "The new file's name, checked as it is typed." },
	{ "values", J::Object, "What the kind's blank takes beside the name, by its params' tokens (a mission's title, terrain, environment)." },
};
constexpr WorkspaceMember kFileRename[] = {
	{ "path", J::String,
			"The project file Files' Rename... renames (\"\" closes it; opening it starts its name as the file's). "
			"show_in_files with ask_name opens it too." },
	{ "name", J::String, "The new name typed; the window previews the rename as it changes (preview_rename)." },
};
constexpr WorkspaceMember kRename[] = {
	{ "open", J::Boolean,
			"Rename everywhere is open over the rename preview_rename planned (a preview_rename with ask_name opens it)." },
	{ "name", J::String, "The new name typed; the window plans the rename again as it changes (preview_rename)." },
};
constexpr WorkspaceMember kRenameBack[] = {
	{ "open", J::Boolean, "Rename back is open over the plan preview_rename_back made (its ask_name opens it)." },
};
constexpr WorkspaceMember kFind[] = {
	{ "open", J::Boolean, "The Document window's find bar is open over the active document (Ctrl+F)." },
	{ "text", J::String, "What it finds: its hits are the document_search query's; open_document at a hit shows it." },
	{ "match_case", J::Boolean, "Aa: case counts." },
};
constexpr WorkspaceMember kProjectFind[] = {
	{ "open", J::Boolean, "Find in project is open (Ctrl+Shift+F; a project open)." },
	{ "text", J::String, "What it finds: the project_search query's hits." },
};
constexpr WorkspaceMember kFiles[] = {
	{ "filter", J::String,
			"Files' filter: the files whose paths hold the text, then those of a kind it names (\"texture\"), "
			"listed flat; the files query with text lists the same." },
	{ "kind", J::String, "The kind Files lists alone, an asset kind's token (\"\" every kind)." },
};
constexpr WorkspaceMember kImport[] = {
	{ "filter", J::String,
			"The import dialog's filter of the files to choose from (a name, a folder, or a kind's word), with an "
			"import preview open." },
	{ "choice_kind", J::String, "The kind of the files to choose from it lists alone, an asset kind's token (\"\" every kind)." },
	{ "rows_filter", J::String, "Its filter of the plan's rows (a filter or a kind shown lists them flat)." },
	{ "kind_shown", J::String, "The kind of the plan's rows it shows alone, an asset kind's token (\"\" every kind)." },
	{ "replace_existing", J::Boolean,
			"Replace existing files: every file the project holds already checked (each can be unchecked alone), "
			"or unchecked again." },
	{ "check", J::Strings,
			"The plan's rows checked, by their index (the import_preview query's index); a converter's outputs come "
			"together: check one, check them all." },
	{ "uncheck", J::Strings, "The plan's rows unchecked, by their index." },
};
constexpr WorkspaceMember kProblems[] = {
	{ "severities", J::Strings, "The severities shown, of error, warning and info." },
	{ "text", J::String, "Only the problems whose message, file, record, field or code holds it." },
	{ "scope", J::String, "Whose problems: project, active_file or open_files." },
	{ "group", J::String, "How they group: none, file or kind." },
	{ "fixable", J::Boolean, "Only fixable: the problems the editor offers a fix for." },
	{ "blocking", J::Boolean,
			"Blocks the build: only what a build is refused for, every one (the other filters set aside until it "
			"is turned off)." },
	{ "confirm", J::Object,
			"The confirmation a Fix all or a Use fix waits in for Apply: {group} a group's Fix all by its key (the "
			"problems query's groups, with this grouping), {required} the summary's Fix all of a request kind "
			"(create_missing, preview_install_import), or {finding, label} the fix of that label of the finding at "
			"that index (the problems query's index); {} closes it. Its Apply raises what it proposes." },
};
constexpr WorkspaceMember kDocument[] = {
	{ "path", J::String, "The document (its path or logical name; left out, the active one), open." },
	{ "filter", J::String, "Its outline's filter (a stylesheet's lines' too): the rows and records whose words hold it." },
	{ "kinds", J::Strings, "The kinds of row its outline lists, by their tokens (a mission's chips); every kind when all are named." },
	{ "all_rows", J::Boolean, "Its outline lists every row, those its type leaves out too (a mission's empty paths)." },
	{ "sort", J::Boolean, "Its list sorted by name." },
	{ "every", J::Boolean, "Master and detail: the filter over every row's detail records, not the selected row's alone." },
	{ "inspector_filter", J::String, "The Inspector's filter over its fields and lists." },
	{ "new_window_type", J::String, "A menu's type for the next Add window (its TYPE's name: static, button, ...)." },
	{ "remove_screen", J::Integer, "A menu's screen whose Remove waits on its confirmation, by its record id (0: none)." },
	{ "remap_from", J::Integer, "An 8-bit PCX's palette index to move (0 to 255)." },
	{ "remap_to", J::Integer, "The palette index it moves to (0 to 255)." },
};

constexpr WorkspacePartRow kParts[] = {
	{ "card", kCard, std::size(kCard), "Files' card of a file." },
	{ "build_result", kBuildResult, std::size(kBuildResult), "The build result's panel." },
	{ "new_project", kNewProject, std::size(kNewProject), "The new-project form (the welcome page's, File > New project...'s)." },
	{ "settings", kSettings, std::size(kSettings), "File > Project settings...: its fields until Apply." },
	{ "new_file", kNewFile, std::size(kNewFile), "Files' New file prompt." },
	{ "file_rename", kFileRename, std::size(kFileRename), "Files' Rename... of a file." },
	{ "rename", kRename, std::size(kRename), "Rename everywhere." },
	{ "rename_back", kRenameBack, std::size(kRenameBack), "Rename back." },
	{ "find", kFind, std::size(kFind), "The Document window's find bar." },
	{ "project_find", kProjectFind, std::size(kProjectFind), "Find in project." },
	{ "files", kFiles, std::size(kFiles), "Files' filter and kind." },
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

bool typed(const JsonValue &value, WorkspaceJson json) {
	switch (json) {
		case J::String:
			return value.is_string();
		case J::Boolean:
			return value.is_bool();
		case J::Integer:
			return value.is_number() && value.number >= 0.0 && value.number == std::floor(value.number) &&
			       value.number <= 9007199254740992.0;
		case J::Strings:
			if (!value.is_array()) return false;
			for (const JsonValue &item : value.array)
				if (!item.is_string()) return false;
			return true;
		case J::Object:
			if (!value.is_object()) return false;
			for (const io::JsonMember &member : value.object)
				if (!member.value.is_string()) return false;
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
		case J::Object:
			return "an object of strings";
	}
	return "a string";
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
	       a.game_install == b.game_install && a.runtime == b.runtime && a.play_in_install == b.play_in_install;
}

// Project settings: opened over the settings in effect (the project's document, the install in effect, the
// editor's runtime and Play in the game install), then the members the change names; a field set while it
// is closed is refused, nothing changed.
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
		settings.play_in_install = view.project.play_retail;
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
	if (const JsonValue *play = part.get("play_in_install")) settings.play_in_install = play->boolean;
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

// The New file prompt: opened on a kind (its name and values emptied), its fields set while it is open.
bool set_new_file(Change &change, const JsonValue &part) {
	WorkspaceView::NewFile prompt = change.workspace().new_file;
	if (const JsonValue *kind = part.get("kind")) {
		AssetKind wanted = AssetKind::kCount;
		if (!kind_named(kind->string, wanted)) return change.refuse("No kind of file is \"" + kind->string + "\".");
		if (wanted != AssetKind::kCount && !change.view.project.open) return change.closed("The project", "open or make one first");
		if (wanted != prompt.kind) prompt = WorkspaceView::NewFile{ wanted, std::string(), {} };
	}
	if ((part.get("name") || part.get("values")) && prompt.kind == AssetKind::kCount)
		return change.closed("The New file prompt", "name its kind first (new_file.kind)");
	if (const JsonValue *name = part.get("name")) prompt.name = name->string;
	if (const JsonValue *values = part.get("values")) {
		prompt.values.clear();
		for (const io::JsonMember &value : values->object) prompt.values.emplace_back(value.key, value.value.string);
	}
	WorkspaceView::NewFile &held = change.workspace().new_file;
	if (prompt.kind == held.kind && prompt.name == held.name && prompt.values == held.values) return false;
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

bool set_project_find(Change &change, const JsonValue &part) {
	WorkspaceView::ProjectFind find = change.workspace().project_find;
	if (const JsonValue *open = part.get("open")) {
		if (open->boolean && !change.view.project.open) return change.closed("The project", "open or make one first");
		find.open = open->boolean;
	}
	if (const JsonValue *text = part.get("text")) find.text = text->string;
	WorkspaceView::ProjectFind &held = change.workspace().project_find;
	if (find.open == held.open && find.text == held.text) return false;
	held = std::move(find);
	return true;
}

bool set_files(Change &change, const JsonValue &part) {
	WorkspaceView::Files files = change.workspace().files;
	if (const JsonValue *filter = part.get("filter")) files.filter = filter->string;
	if (const JsonValue *kind = part.get("kind"); kind && !kind_named(kind->string, files.kind))
		return change.refuse("No kind of file is \"" + kind->string + "\".");
	WorkspaceView::Files &held = change.workspace().files;
	if (files.filter == held.filter && files.kind == held.kind) return false;
	held = std::move(files);
	return true;
}

// The import dialog's own: refused with no import preview open. Replace existing files sets the check of
// every file the project holds that the import can take; check and uncheck name rows by index, a
// converter's outputs coming together.
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
			const std::optional<unsigned long> index = strutil::parse_ulong(token.string);
			if (!index || *index >= plan.rows.size())
				return change.refuse("import." + std::string(member) + " names the plan's rows by their index (0 to " +
				                     std::to_string(plan.rows.size()) + "), not \"" + token.string + "\".");
			const ImportPlanRow &row = plan.rows[*index];
			// A row the project cannot take is not checked (a chosen one can only be unchecked).
			if (on && !import_row_refusal(plan, *index).empty())
				return change.refuse("import.check: " + row.name + " cannot be imported: " + import_row_refusal(plan, *index));
			// The files one converter source makes come together.
			for (size_t i = 0; i < plan.rows.size(); ++i)
				if (i == *index || (!row.made_from.empty() && plan.rows[i].made_from == row.made_from &&
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
// when they come back as they were with what changed of them meanwhile.
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
			std::string *slot = member.key == "group"      ? &asked.group
			                    : member.key == "required" ? &asked.required
			                    : member.key == "finding"  ? &asked.finding
			                    : member.key == "label"    ? &asked.label
			                                               : nullptr;
			if (!slot) return change.refuse("problems.confirm takes group, required, or finding and label, not \"" + member.key + "\".");
			*slot = member.value.string;
		}
		const int asks = int(!asked.group.empty()) + int(!asked.required.empty()) + int(!asked.finding.empty());
		if (asks > 1) return change.refuse("problems.confirm asks one confirmation: a group's, a required kind's or a finding's fix.");
		if (!asked.finding.empty() && asked.label.empty())
			return change.refuse("problems.confirm names the fix of the finding by its label.");
		if (asked.open() && !change.view.project.open) return change.closed("The project", "open one first");
		if (!asked.required.empty()) {
			EditorRequestKind kind = EditorRequestKind::CreateMissing;
			if (!editor_request_kind_from_token(asked.required, kind))
				return change.refuse("problems.confirm.required names a request kind, not \"" + asked.required + "\".");
		}
		if (!asked.finding.empty()) {
			const std::optional<unsigned long> index = strutil::parse_ulong(asked.finding);
			if (!index || *index >= change.view.findings.diagnostics.size())
				return change.refuse("problems.confirm.finding is the index of a finding (the problems query's index), not \"" +
				                     asked.finding + "\".");
		}
		problems.confirm = asked;
		++problems.confirm_serial;
	}
	if (same_problems(problems, change.workspace().problems)) return false;
	change.workspace().problems = std::move(problems);
	return true;
}

// What a document's views show of it: the document named (the active one when none is), open; its kinds
// by their tokens, every kind when all are named.
bool set_document(Change &change, const JsonValue &part) {
	const JsonValue *path = part.get("path");
	const std::string named = path ? path->string : std::string();
	const DocumentBase *document = open_document(change.view, named);
	if (!document)
		return change.refuse(named.empty() ? std::string("No document is active.") : "No document is open at " + named + ".", named);
	WorkspaceView::DocumentView shown = change.workspace().document(document->path());
	if (const JsonValue *filter = part.get("filter")) shown.filter = filter->string;
	if (const JsonValue *kinds = part.get("kinds")) {
		const Document *records = records_of(*document);
		if (!records) return change.refuse(document->path() + " lists no kinds of row.", document->path());
		const std::vector<RecordKindRow> &rows = records->kinds();
		const size_t count = std::min<size_t>(rows.size(), 64);
		uint64_t mask = 0;
		for (const JsonValue &token : kinds->array) {
			size_t found = count;
			for (size_t i = 0; i < count; ++i)
				if (token.string == rows[i].token) found = i;
			if (found == count)
				return change.refuse(document->path() + " has no kind of row \"" + token.string + "\".", document->path());
			mask |= uint64_t(1) << found;
		}
		const uint64_t all = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
		shown.kinds = (mask & all) == all ? ~uint64_t(0) : mask;
	}
	if (const JsonValue *all_rows = part.get("all_rows")) shown.all_rows = all_rows->boolean;
	if (const JsonValue *sort = part.get("sort")) shown.sort = sort->boolean;
	if (const JsonValue *every = part.get("every")) shown.every = every->boolean;
	if (const JsonValue *filter = part.get("inspector_filter")) shown.inspector_filter = filter->string;
	if (const JsonValue *type = part.get("new_window_type")) shown.new_window_type = type->string;
	if (const JsonValue *screen = part.get("remove_screen")) shown.remove_screen = uint64_t(screen->number);
	for (const char *member : { "remap_from", "remap_to" }) {
		const JsonValue *index = part.get(member);
		if (!index) continue;
		if (index->number > 255.0) return change.refuse(std::string("document.") + member + " is a palette index, 0 to 255.", document->path());
		(std::string(member) == "remap_from" ? shown.remap_from : shown.remap_to) = int(index->number);
	}
	WorkspaceView::DocumentView &held = change.workspace().documents[document->path()];
	const bool moved = held.filter != shown.filter || held.kinds != shown.kinds || held.all_rows != shown.all_rows ||
	                   held.sort != shown.sort || held.every != shown.every || held.inspector_filter != shown.inspector_filter ||
	                   held.new_window_type != shown.new_window_type || held.remove_screen != shown.remove_screen ||
	                   held.remap_from != shown.remap_from || held.remap_to != shown.remap_to;
	held = std::move(shown);
	return moved;
}

// `focus`: the window it names brought forward (a focus_window view event the workspace takes).
bool focus_window(Change &change, const std::string &token) {
	ViewEvent focus;
	focus.kind = ViewEventKind::FocusWindow;
	focus.path = token;
	change.view.events.post(std::move(focus));
	return true;
}

JsonValue sound_json(const WorkspaceView::Sound &sound) {
	JsonValue out = JsonValue::make_object();
	out.set("path", text(sound.path));
	out.set("state", text(sound_state_token(sound.state)));
	out.set("serial", JsonValue::make_number(double(sound.serial)));
	if (!sound.error.empty()) out.set("error", text(sound.error));
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
	out.set("remove_screen", JsonValue::make_number(double(shown.remove_screen)));
	out.set("remap_from", JsonValue::make_number(shown.remap_from));
	out.set("remap_to", JsonValue::make_number(shown.remap_to));
	out.set("active", flag(document.path() == view.documents.active));
	return out;
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
	bool moved = false;
	for (const io::JsonMember &member : parsed.object) {
		if (member.key == "focus") {
			moved = focus_window(change, member.value.string) || moved;
			continue;
		}
		for (const Part &part : kSetters)
			if (member.key == part.token) moved = part.set(change, member.value) || moved;
	}
	return moved;
}

void set_workspace(SessionCore &core, const std::string &change) {
	std::vector<WorkspaceRefusal> refusals;
	const bool moved = apply_workspace_change(core.view(), change, refusals);
	for (const WorkspaceRefusal &refusal : refusals) core.refuse_now(CoreFinding::WorkspaceRefused, refusal.message, refusal.asset);
	if (moved) core.touch(ViewConcern::Workspace);
}

void workspace_follows(SessionCore &core, const EditorRequest &request) {
	SessionView &view = core.view();
	WorkspaceView &workspace = view.workspace;
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
			else if (rename.open && rename.path == plan.path && rename.locator == plan.locator && rename.field == plan.field)
				rename.name = plan.requested;
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
		case EditorRequestKind::CreateFile:
			// The prompt's Create: the file it names made (or refused, the reasons in Problems).
			if (workspace.new_file.kind != AssetKind::kCount && request.path == workspace.new_file.name) {
				workspace.new_file = WorkspaceView::NewFile();
				moved = true;
			}
			break;
		default:
			break;
	}
	if (moved) core.touch(ViewConcern::Workspace);
}

JsonValue workspace_to_json(const SessionView &view) {
	const WorkspaceView &workspace = view.workspace;
	JsonValue out = JsonValue::make_object();
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
		dialog.set("play_in_install", flag(settings.play_in_install));
	}
	out.set("settings", std::move(dialog));
	JsonValue prompt = JsonValue::make_object();
	prompt.set("kind", kind_json(workspace.new_file.kind));
	if (workspace.new_file.kind != AssetKind::kCount) {
		prompt.set("name", text(workspace.new_file.name));
		JsonValue values = JsonValue::make_object();
		for (const auto &[token, value] : workspace.new_file.values) values.set(token, text(value));
		prompt.set("values", std::move(values));
	}
	out.set("new_file", std::move(prompt));
	JsonValue file_rename = JsonValue::make_object();
	file_rename.set("path", text(workspace.file_rename.path));
	if (!workspace.file_rename.path.empty()) file_rename.set("name", text(workspace.file_rename.name));
	out.set("file_rename", std::move(file_rename));
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
	out.set("project_find", std::move(project_find));
	JsonValue files = JsonValue::make_object();
	files.set("filter", text(workspace.files.filter));
	files.set("kind", kind_json(workspace.files.kind));
	out.set("files", std::move(files));
	// The import dialog's, with the checks counted (the import_preview query pages each row's).
	const WorkspaceView::Import &import = workspace.import;
	JsonValue dialog_import = JsonValue::make_object();
	dialog_import.set("filter", text(import.filter));
	dialog_import.set("choice_kind", kind_json(import.choice_kind));
	dialog_import.set("rows_filter", text(import.rows_filter));
	dialog_import.set("kind_shown", kind_json(import.kind_shown));
	dialog_import.set("replace_existing", flag(import.replace_existing));
	size_t checked = 0;
	for (const bool on : import.checked) checked += on ? 1 : 0;
	dialog_import.set("checked", JsonValue::make_number(double(checked)));
	dialog_import.set("serial", JsonValue::make_number(double(import.serial)));
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
	                                    std::make_pair("finding", &problems.confirm.finding), std::make_pair("label", &problems.confirm.label) })
		if (!value->empty()) confirm.set(token, text(*value));
	filters.set("confirm", std::move(confirm));
	filters.set("confirm_serial", JsonValue::make_number(double(problems.confirm_serial)));
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

void take_import_checks(WorkspaceView &workspace, const ImportPlan &plan) {
	workspace.import.checked = import_default_checks(plan, workspace.import.replace_existing);
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
	workspace.rename = WorkspaceView::Rename();
	workspace.rename_back = WorkspaceView::RenameBack();
	workspace.find.open = false;
	workspace.project_find.open = false;
	// Another project lists its own files: the filter and the kind start afresh.
	workspace.files = WorkspaceView::Files();
	workspace.problems.confirm = WorkspaceView::Problems::Confirm();
	++workspace.problems.confirm_serial;
	workspace.documents.clear();
	stop_sound(workspace.sound);
}

} // namespace opennova::editor
