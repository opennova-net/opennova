#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/session/view/view_revisions.h>
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
// required value is given.
class NewFilePrompt {
public:
	// Asks on the next draw for a new file of `kind`.
	void ask(AssetKind kind);
	void draw(Workspace &workspace);

private:
	bool ask_ = false;
	AssetKind kind_ = AssetKind::Unknown;
	char name_[64]{};
	std::vector<std::string> values_; // per param of the kind's blank: the text typed, the file chosen
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
	FilesWindow(Workspace &workspace, NewFilePrompt &new_file) : workspace_(workspace), new_file_(new_file) { open = true; }

	const char *title() const override { return "Files"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Left;
	}
	// With no project open it stands aside for the welcome page (aside_for_welcome).
	bool stands_aside() const override;
	void show_anyway() override { welcome_asked_ = true; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

	// The file a click selected (project-relative; "" = none).
	const std::string &selected() const { return selected_; }
	// How many times the tree and the counts were made again (refresh): once per change of what
	// they read, never for a change of anything else (a line of Output, a build's step).
	size_t rebuilds() const { return rebuilds_; }
	// A RevealFile event (a ShowInFiles): Files comes forward at once, whether it draws this frame
	// or not, and holds the event until it draws; then it selects the file, clears a filter that
	// hides it, opens its folders, scrolls to it and, when the event asks, opens Rename... on it.
	// Each event is shown once, the same file asked again shown again; of the events held when
	// it draws only the newest is, an older ask (and its Rename...) passed over.
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
	// Rename... (the file's menu, F2): asks the new name on the next draw.
	void start_rename(const AssetEntry &entry);
	void draw_rename(const SessionView &view);
	// The kind filter beside the text: every kind, or one the project has files of (with how many).
	void draw_kind_filter(const SessionView &view, float width);
	// A file's card (the UX round's project lane: session/file_card.h), a window of its own in the editor's:
	// what the file is, where a build puts it, a wave's sound with Play and Stop, what it names and who
	// names it, each a click away. Opened by a double click on a file the editor opens no document of, its
	// menu's About this file..., and an AboutFile request (a RevealFile event with tag 1).
	void open_card(const std::string &path);
	void draw_card(const SessionView &view);

	Workspace &workspace_;
	NewFilePrompt &new_file_;
	mutable bool welcome_asked_ = false; // shown with no project open at the author's ask
	char filter_[128]{};
	std::string selected_;
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
	// The width, in the font's ems, under which the Kind column gives way to the name, and whether the
	// dock had it when last drawn.
	static constexpr float kKindRoomEm = 19.0f;
	bool kind_fitted_ = true;
	// The file whose card shows ("" none) and the card, made again when the files or the graph move.
	std::string card_path_;
	bool card_focus_ = false;
	RevisionKey card_key_;
	std::shared_ptr<const FileCard> card_;
	// The file a menu asked to rename, and the name typed.
	std::string renaming_;
	bool open_rename_ = false;
	char rename_[64]{};
	std::string previewed_; // the name Rename...'s preview was last asked for
	// The RevealFile events held until Files draws, and then the file to scroll to and the place
	// whose folders open on the way.
	ViewEventMailbox<> events_;
	std::string scroll_to_;
	std::string open_to_;
};

} // namespace opennova::editor
