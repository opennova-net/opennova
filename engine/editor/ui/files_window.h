#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/session/view/view_revisions.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

struct AssetEntry;
struct FileCard;
struct SessionView;

// The name a new file of a free-form kind takes (Files' New: a string table, a menu, a
// font, a mission), asked in a modal the workspace draws every frame: the name, checked as it
// is typed by the project's name rules, then what the kind's blank takes beside it (ADR 0046
// S14, BlankFactory::params: a mission's title, and its terrain and environment, each chosen
// among the project's files of that kind), then Create, which raises CreateFile for it once every
// required value is given. The prompt is the workspace's (the MCP gaps lane: workspace.new_file, its
// kind, name and values): open while it names a kind, its fields the session's.
class NewFilePrompt {
public:
	// Asks for a new file of `kind` (the workspace's prompt opened on it), in `folder` (a folder's New here,
	// DI-25: "/" the top level; "" where the placement rule puts a file of its kind).
	static void ask(Workspace &workspace, AssetKind kind, const std::string &folder = std::string());
	void draw(Workspace &workspace);

private:
	ui_kit::HeldPopup popup_;
	ui_kit::HeldText<kWorkspaceFileName> name_;
	// Per param of the kind's blank: the text typed, the file chosen (the session's values, as last taken).
	std::vector<std::string> values_;
	std::vector<std::pair<std::string, std::string>> values_seen_;
	AssetKind kind_seen_ = AssetKind::kCount;
};

// The project's files (ADR 0046 d6, S11d): the scan as a tree of its folders, each open
// until folded (an imported file under its source's folder), and a filter and a kind whose
// matches list flat (the UX round's project lane: a name, a folder, or a kind's word, "texture"
// listing the textures: match_files), how many files they list beside them; per file its name,
// its kind and its size in columns that resize, its path, kind and size in its tooltip. An open
// document's name is drawn in the open colour with a dot while it has unsaved changes, and a
// file's errors and warnings are counted after its name. A click selects a file, a double click
// opens it when the editor edits its kind and shows its card otherwise (what it is, what it
// names and who names it, a wave's sound with Play), and a row dragged onto a reference field
// whose kind loads the file sets it there. The toolbar imports (files from the disk, from the
// game data, every source again), makes a new file (every blank factory: a free-form kind asks
// for a name, a file the game reads by name is made at once and is not offered while the project
// has it) and reads the folder again; a file's menu opens it, renames it (F2 too; what the rename
// rewrites, or why it is refused, previewed as the name is typed: PreviewRename), shows its card
// (About this file...), shows it in the OS file manager and imports an image source again. A
// Problems row about a file the editor does not open (or about a file's name) shows it here
// (ShowInFiles, a RevealFile view event; an AboutFile's opens its card too).
class FilesWindow : public devtools::Window {
public:
	explicit FilesWindow(Workspace &workspace) : workspace_(workspace) { open = true; }

	const char *title() const override { return "Files"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Left;
	}
	// With no project open it stands aside for the welcome page (aside_for_welcome).
	bool stands_aside() const override;
	void show_anyway() override { welcome_asked_ = true; }
	// Coming back (a project opened over the welcome page) it leaves the keyboard where it is.
	bool focus_on_appearing() const override { return false; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;
	// The file's card, a window of its own drawn every frame by the workspace (with its modals), whether
	// Files draws or not (collapsed, behind a tab, standing aside): the workspace's card (an AboutFile from
	// Files, Problems or the wire opens it, a set_workspace closes it), shown at once. The session closes it
	// with its project, and stops the sound it played as it closes.
	void draw_card_window();
	// Rename... shows (drawn the frame before).
	bool rename_shown() const { return rename_popup_.shown(); }
	// Delete... shows (drawn the frame before).
	bool delete_shown() const { return delete_popup_.shown(); }

	// The file a click selected (project-relative; "" = none).
	const std::string &selected() const { return selected_; }
	// How many times the tree and the counts were made again (refresh): once per change of what
	// they read, never for a change of anything else (a line of Output, a build's step).
	size_t rebuilds() const { return rebuilds_; }
	// A RevealFile event (a ShowInFiles): Files comes forward at once, whether it draws this frame
	// or not, and holds the event until it draws; then it selects the file, clears a filter that
	// hides it (the workspace's), opens its folders and scrolls to it (a show_in_files that asks the
	// name opens the workspace's Rename... on it). Each event is shown once, the same file asked again
	// shown again; of the events held when it draws only the newest is.
	void receive(const ViewEvent &event);
	// The events it holds until it draws.
	const ViewEventMailbox<> &events() const { return events_; }

private:
	// A folder of the tree: its folders (sorted by name) and its files (the scan's order,
	// indices into the scan's entries).
	struct Folder {
		std::string name;
		std::string path;
		std::vector<size_t> folders;
		std::vector<size_t> files;
	};
	// A file's findings, for the counts after its name: the modder's, as Problems counts them; a file
	// that is the game's own data (S15) has its errors and warnings counted apart, said in its tooltip.
	struct Counts {
		size_t errors = 0;
		size_t warnings = 0;
		size_t original_errors = 0;
		size_t original_warnings = 0;
	};

	void refresh(const SessionView &view);
	// The files the filter matches (their scan indices), found again only when the filter or the
	// scan moved; each file's path as the filter compares it made only while a filter is set,
	// once a scan.
	const std::vector<size_t> &matching(const SessionView &view);
	void show_revealed(const SessionView &view, const ViewEvent &event);
	void draw_toolbar(const SessionView &view);
	void draw_folder(const SessionView &view, const Folder &folder);
	void draw_file(const SessionView &view, const AssetEntry &entry, bool in_tree);
	void draw_file_menu(const SessionView &view, const AssetEntry &entry);
	// Move to folder (DI-03, a file's menu): the project's folders, the top level first, and a new one by
	// name, each a move_asset; a file dragged onto a folder's row moves there too (accept_move), and while
	// a file of a folder is dragged, a row for the top level stands above the tree (draw_top_level_drop).
	void draw_move_menu(const SessionView &view, const AssetEntry &entry);
	void accept_move(const SessionView &view, const std::string &folder);
	void draw_top_level_drop(const SessionView &view);
	// Files' chores (DI-25, files_window_chores.cpp). The New entries, every blank factory's (a free-form kind
	// asks a name; a file the game reads by name is made at once), in `folder` ("" where the placement rule
	// puts a file of its kind: the toolbar's New; a folder's New here names it, "/" the top level).
	void draw_new_entries(const SessionView &view, const std::string &folder);
	// A folder's menu (a right click on its row; on the list's empty room, the top level's, ""): New here, a new
	// folder in it, its rename, its delete (with what it holds, once asked), each typed in the menu; the top
	// level's Empty the trash....
	void draw_folder_menu(const SessionView &view, const std::string &folder);
	// A file's Duplicate (an import source's with its record, or alone) and Delete... entries, of every row
	// selected when the file is among them.
	void draw_chore_entries(const SessionView &view, const AssetEntry &entry);
	// Delete... (the workspace's file_delete): who names the files, what goes with them (a mission's
	// companions, an import source's outputs or, alone, what it keeps), then Delete (anyway, where something
	// names them) or Cancel.
	void start_delete(const AssetEntry &entry, const std::vector<std::string> &others = {});
	void draw_delete(const SessionView &view);
	// A folder deleted with what it holds, and the trash emptied: each asked first in a dialog of its own.
	void draw_folder_delete(const SessionView &view);
	void draw_empty_trash(const SessionView &view);
	// The rows selected with `path` besides it (Ctrl+click adds or takes one, Shift+click a run of them): none
	// when `path` is not among them, so a chore acts on the row it was asked of alone.
	std::vector<std::string> others_of(const std::string &path) const;
	bool chosen(const std::string &path) const;
	// A click on a row: Ctrl adds or takes it, Shift the run from the selected row to it, else it alone.
	void choose(const std::string &path);
	// Rename... (the file's menu, F2, the card's): the workspace's Rename... opened on the file.
	void start_rename(const AssetEntry &entry);
	void draw_rename(const SessionView &view);
	// The kind filter beside the text: every kind, or one the project has files of (with how many).
	void draw_kind_filter(const SessionView &view, float width);
	// The filter, the kind and the order, as the workspace holds them (files {filter, kind, by_cost}): taken
	// when the session's moved; one the person sets goes to the session.
	void follow_filter(const SessionView &view);
	void send_filter(bool filter, bool kind, bool by_cost = false);
	// A file's card (the UX round's project lane: session/file_card.h), a window of its own in the editor's:
	// what the file is, where a build puts it, a wave's sound with Play and Stop (and how it goes), what it
	// names and who names it, each a click away. The workspace's card (the MCP gaps lane): opened by an
	// AboutFile (a double click on a file the editor opens no document of, its menu's About this file...),
	// closed by a set_workspace (its X), and by the session as its file goes (workspace_tidies).
	void draw_card(const SessionView &view);
	void close_card();

	Workspace &workspace_;
	mutable bool welcome_asked_ = false; // shown with no project open at the author's ask
	ui_kit::HeldText<kWorkspaceText> filter_;
	ui_kit::Held<AssetKind> kind_held_;
	std::string selected_;
	// The other rows selected with it (DI-25: a chore acts on them together), and the rows in the order they
	// were last drawn, which a Shift+click's run follows.
	std::vector<std::string> also_;
	std::vector<std::string> rows_, rows_drawing_;
	// What refresh() makes of the view, kept while what it reads stands (cache_key); the files the
	// filter last matched and the filter and kind they matched (matching()'s).
	const SessionView *view_ = nullptr;
	RevisionKey key_;
	size_t rebuilds_ = 0;
	std::vector<Folder> folders_; // [0]: the project's own folder
	std::unordered_map<std::string, Counts> counts_;
	std::string matched_;
	bool matches_made_ = false;
	std::vector<size_t> matches_;
	// The graph the matches were found over (its generation): a file is matched by a record naming it
	// too (a model by its item's name, ADR 0046 S17), each such file's record by its path.
	uint64_t matched_generation_ = 0;
	std::unordered_map<std::string, std::string> via_;
	// The kind the list is narrowed to (kCount: every kind).
	AssetKind kind_shown_ = AssetKind::kCount;
	// Whether the files listed flat go by what the game's textures of each cost (the workspace's by_cost, S18):
	// each matched file's cost at full detail (texture_budget_list's textures of it, summed) and the matches'
	// total, made with the matches.
	ui_kit::Held<bool> by_cost_held_;
	bool by_cost_ = false;
	std::unordered_map<std::string, uint64_t> costs_;
	uint64_t matched_cost_ = 0;
	// The width, in the font's ems, under which the Kind column gives way to the name, and whether the
	// dock had it when last drawn.
	static constexpr float kKindRoomEm = 19.0f;
	bool kind_fitted_ = true;
	// The file the card was last drawn for (the workspace's card's: "" none) and the card, made again when the
	// files or the graph move (a wave's sound kept while its file stands); whether it comes forward at its next
	// draw (it opened); the card whose close was asked (once).
	std::string card_path_;
	bool card_focus_ = false;
	RevisionKey card_key_;
	std::shared_ptr<const FileCard> card_;
	std::string card_closing_;
	// The project the selection is for: another one starts it afresh (the session starts its filter and
	// kind afresh).
	std::string shown_root_;
	// Rename... (the workspace's file_rename: the file and the name typed), and the name its preview was
	// last asked for.
	ui_kit::HeldPopup rename_popup_;
	ui_kit::HeldText<kWorkspaceFileName> rename_;
	std::string previewed_;
	// Move to folder's new folder, as typed.
	char new_folder_[kWorkspaceFileName] = {};
	// A folder menu's new folder and new name, as typed (DI-25), and the folder whose menu shows them.
	char folder_new_[kWorkspaceFileName] = {};
	char folder_rename_[kWorkspaceFileName] = {};
	std::string folder_menu_;
	// Delete... (the workspace's file_delete), and what it lists, made again when the file, whether it goes
	// alone, or the files and the graph move: who names what goes (in words, a few), how many, and what goes
	// with it or is kept.
	ui_kit::HeldPopup delete_popup_;
	std::string delete_for_;
	RevisionKey delete_key_;
	std::vector<std::string> delete_uses_;
	size_t delete_use_count_ = 0;
	std::vector<std::string> delete_with_;
	std::vector<std::string> delete_kept_;
	std::vector<std::string> delete_refusals_;
	// A folder's delete with what it holds, and Empty the trash, each held open by its asker until answered:
	// the folder and what it lists (made again as the files move), and how many files the trash holds.
	ui_kit::HeldPopup folder_delete_popup_, trash_popup_;
	std::string folder_delete_;
	RevisionKey folder_delete_key_;
	size_t folder_delete_files_ = 0, folder_delete_use_count_ = 0;
	std::vector<std::string> folder_delete_uses_, folder_delete_refusals_;
	bool trash_asked_ = false;
	size_t trash_files_ = 0;
	// The RevealFile events held until Files draws, and then the file to scroll to and the place
	// whose folders open on the way.
	ViewEventMailbox<> events_;
	std::string scroll_to_;
	std::string open_to_;
	// The session's file selection as Files last showed it (S18: select_file, from a row's click or
	// from the wire): a selection the session changed is the row selected here.
	std::string followed_;
};

} // namespace opennova::editor
