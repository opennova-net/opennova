#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

class SessionCore;
struct ImportPlan;
struct SessionView;

// What the windows show of their own, on the wire (ADR 0046, the MCP gaps lane; session/view/
// workspace_view.h): one row per part of the workspace a person's controls change (a card, a panel, a
// form), each with the members a set_workspace may name, their JSON types and what they mean. The
// request carries {<part>: {<member>: value, ...}, ...}: a part or a member left out stays as it is;
// `focus` is no part but an ask, the window it names brought forward (a focus_window view event).
// The catalog query lists the table, from which the editor MCP makes the request's schema.
enum class WorkspaceJson : uint8_t { String, Boolean, Integer, Strings, Integers, Object };

// A JSON type's word: "string", "boolean", "integer", "string[]", "integer[]", "object".
const char *workspace_json_token(WorkspaceJson json);

// `longest` (the window's field's buffer, workspace_view.h's kWorkspaceText and the rest, less its
// terminator): a string's (each string's of an array or an object) most characters, 0 for no bound.
struct WorkspaceMember {
	const char *token = "";
	WorkspaceJson json = WorkspaceJson::String;
	const char *doc = "";
	size_t longest = 0;
};

struct WorkspacePartRow {
	const char *token = "";
	const WorkspaceMember *members = nullptr;
	size_t member_count = 0;
	const char *doc = "";
};

// The table's rows, in their order, and their count.
const WorkspacePartRow *workspace_parts(size_t &count);
// The windows `focus` brings forward, by token (files, document, preview, inspector, problems,
// output), and each one's title, in their order; null past the last.
const char *workspace_window_token(size_t index);
const char *workspace_window_title(size_t index);

// The change `json` (set_workspace's workspace) checked against the table, nothing changed: an object
// of parts, each an object of its members, each of its type, `focus` one of the windows. False with
// `error` naming the part or the member at fault and what it takes.
bool check_workspace_change(const io::JsonValue &json, std::string &error);

// What a change refused: why, and the project file it is about ("" none).
struct WorkspaceRefusal {
	std::string message;
	std::string asset;
};

// `change` (set_workspace's JSON text) checked, then each part it names set in `view`'s workspace as it says,
// a part refused (`refusals`) left as it was (its members are set together, or none of them): the session's
// view, or a test's hand-made one, as the session serves the request. True when the workspace moved (the
// caller's to touch). What a part does beside its members: the card a project file's (refused for one the
// project lacks), its close stopping the sound; the new-project form's install named once given, its building
// on an expansion building as one; the settings filled from those in effect as they open; the New file
// prompt's name and values emptied as it opens on a kind; Rename... starting with the file's name; Rename
// everywhere opened over the name's rename planned; Blocks the build setting aside the filters that could hide
// a refusal, which come back as it is turned off; a confirmation asked moving its serial.
bool apply_workspace_change(SessionView &view, const std::string &change, std::vector<WorkspaceRefusal> &refusals);
// SetWorkspace: apply_workspace_change over the session's view, each refusal a workspace.refused warning in the
// request's outcome alone (view state, not the project's: no Problems row, no status line).
void set_workspace(SessionCore &core, const std::string &change);
// What a request just served opens of the workspace beside its own work, as a person's gesture does:
// show_in_files with ask_name opens Rename... on the file, preview_rename with ask_name Rename everywhere (a
// plan of the open one's name is the name typed), preview_rename_back with ask_name Rename back.
void workspace_follows(SessionCore &core, const EditorRequest &request);
// What a request closes as the session takes it (past the busy gate, before an unsaved-changes prompt it
// may wait on), as the dialog's own button does: rename_asset of Rename...'s file closes it, rename_symbol
// Rename everywhere, rename_back Rename back, create_file of the name the New file prompt holds the prompt,
// new_project the New project form's modal.
void workspace_closes_for(SessionCore &core, const EditorRequest &request);
// What the workspace holds kept true to the project after anything moved it (each request served, each
// operation's end): a card, Rename... or Find usages whose file the files no longer have closes; a menu's Remove prompt
// whose screen is gone (or is its menu's last) closes; Rename everywhere whose plan is no longer its name's
// (another rename planned in its place) closes, and so does Rename back once the plan is no longer a way back.
void workspace_tidies(SessionCore &core);
// The files a rename moved (each from, to): a card of one shows it at its new path, Find usages of one lists its
// uses under it. True when either moved.
bool workspace_follows_moves(WorkspaceView &workspace, const std::vector<std::pair<std::string, std::string>> &moved);

// The dialogs that take the whole editor while they show (ADR 0046, the MCP gaps lane): one shows at a time,
// the first the session holds open in this order, and the others wait, held open still, until it closes (a
// held dialog never opens itself over another). The unsaved-changes prompt (a request waits on its answer),
// the import dialog, the texture source dialog, Project settings, File > New project..., Files' New file
// prompt and Rename..., Rename everywhere, Rename back, Find in project, Problems' confirmation, a menu's
// Remove screen prompt (the first menu holding one, by path). Find in project is the project's finder in any of
// its scopes (Go to file, Go to name, Find usages too).
enum class HeldModal : uint8_t {
	None,
	Unsaved,
	Import,
	TextureSource,
	Settings,
	NewProject,
	NewFile,
	FileRename,
	FileDelete,
	Rename,
	RenameBack,
	ProjectFind,
	Confirm,
	RemoveScreen,
};
// The one that shows, and for a Remove screen prompt its menu's path.
struct ShownModal {
	HeldModal modal = HeldModal::None;
	std::string path;
};
ShownModal shown_modal(const SessionView &view);
// Whether `modal` (of the document at `path`, a Remove screen prompt's) is the one that shows, or none does
// (a window's own ask, shown at once, the frame it is made).
bool modal_may_show(const SessionView &view, HeldModal modal, const std::string &path = std::string());
// "" none; unsaved, import, texture_source, settings, new_project, new_file, file_rename, file_delete, rename,
// rename_back, project_find, confirm, remove_screen.
const char *held_modal_token(HeldModal modal);

// The workspace section: each part as the windows show it, the sound, and each open document's views.
io::JsonValue workspace_to_json(const SessionView &view);

// The card of the project file at `path` (project-relative) shown, a card of another file closing (the
// sound it played stopped): true when the card moved. An about_file's, a set_workspace's.
bool show_card(WorkspaceView &workspace, const std::string &path);

// PlaySound: the project's wave at `path` (a path or a logical name) played, the sound's serial moved and
// its state Starting until the Shell reports it; refused (workspace.refused, nothing changed) for a name no
// wave of the project has and for one past kWaveCardBytes. StopSound: the sound stopped.
void play_sound(SessionCore &core, const std::string &path);
void stop_sound(SessionCore &core);
// What the Shell reports of the play of `serial` (ProjectSession::report_sound): Playing once decoded and
// playing, Ended once played through, Failed with why; a report of a play since stopped or replaced is
// passed over. True when the sound moved.
bool report_sound(WorkspaceView &workspace, uint64_t serial, WorkspaceView::SoundState state, const std::string &error);

// A document closing: what its views showed of it goes with it. True when the workspace held any.
bool forget_document_workspace(WorkspaceView &workspace, const std::string &path);
// The import dialog's checks taken anew for a plan made (import_default_checks, with its Replace existing
// files), its serial moved; a row the plan before it (`before`, the plan the checks were of) had too, the same
// source to the same place, keeps its check (a row unchecked stays unchecked through a re-plan; one checked
// stays checked where the project can take it). And the dialog's own forgotten (a new preview's start, its
// close).
void take_import_checks(WorkspaceView &workspace, const ImportPlan &plan, const ImportPlan *before);
void forget_import_workspace(WorkspaceView &workspace);
// The project closing: what its windows showed of it goes with it (its card, its build's panel, its dialogs
// and prompts, a confirmation, its documents' views, Files' filter, the sound it played, Find usages' subject); the
// find bars close, keeping their text.
void forget_project_workspace(WorkspaceView &workspace);

} // namespace opennova::editor
