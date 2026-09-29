#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// The name a new file of a free-form kind takes (Files' New: a string table, a menu, a
// font), asked in a modal the workspace draws every frame: the name, checked as it is
// typed by the project's name rules, then Create, which raises CreateFile for it.
class NewFilePrompt {
public:
	// Asks on the next draw for a new file of `kind`.
	void ask(AssetKind kind);
	void draw(EditorHost &host);

private:
	bool ask_ = false;
	AssetKind kind_ = AssetKind::Unknown;
	char name_[64]{};
};

// The project's files (ADR 0046 d6, S11d): the scan as a tree of its folders, each open
// until folded (an imported file under its source's folder), and a filter whose matches
// list flat, how many files the project has beside it; per file its name and its size in
// columns that resize (its kind a third, hidden until the header's menu shows it), its
// path, kind and size in its tooltip. An open document's name is drawn in the open colour
// with a dot while it has unsaved changes, and a file's errors and warnings are counted
// after its name. A click selects a file, a double click opens it when the editor edits its
// kind, and a row dragged onto a reference field whose kind loads the file sets it there. The toolbar imports (files from the disk, from the game data, every source again),
// makes a new file (every blank factory: a free-form kind asks for a name, a file the game
// reads by name is made at once and is not offered while the project has it) and reads the
// folder again; a file's menu opens it, renames it (F2 too; what the rename rewrites, or why it
// is refused, previewed as the name is typed: PreviewRename), lists its references both ways
// (each field by the name the inspector shows), shows it in the OS file manager and
// imports an image source again. A Problems row about a file the editor does not open (or
// about a file's name) shows it here (ShowInFiles).
class FilesWindow : public devtools::Window {
public:
	FilesWindow(EditorHost &host, NewFilePrompt &new_file) : host_(host), new_file_(new_file) { open = true; }

	const char *title() const override { return "Files"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Left;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

	// The file a click selected (project-relative; "" = none).
	const std::string &selected() const { return selected_; }
	// The file a ShowInFiles asked for (the view's reveal_file, each ask by its serial): Files
	// comes forward (the workspace asks every frame, whether Files draws or not), then selects
	// it, clears a filter that hides it, opens its folders, scrolls to it and, when the ask
	// says so, opens Rename... on it.
	void follow_reveal(const SessionView &view);

private:
	// A folder of the tree: its folders (sorted by name) and its files (the scan's order,
	// indices into the scan's entries).
	struct Folder {
		std::string name;
		std::string path;
		std::vector<size_t> folders;
		std::vector<size_t> files;
	};
	// A file's findings, for the counts after its name.
	struct Counts {
		size_t errors = 0;
		size_t warnings = 0;
	};

	// What the window keeps its caches by: the view's revision, which moves with the scan and
	// the findings.
	static uint64_t cache_key(const SessionView &view) { return view.revision; }
	void refresh(const SessionView &view);
	// The files the filter matches (their scan indices), found again only when the filter or the
	// scan moved.
	const std::vector<size_t> &matching(const SessionView &view);
	void show_revealed(const SessionView &view);
	void draw_toolbar(const SessionView &view);
	void draw_folder(const SessionView &view, const Folder &folder);
	void draw_file(const SessionView &view, const AssetEntry &entry, bool in_tree);
	void draw_file_menu(const SessionView &view, const AssetEntry &entry);
	// Rename... (the file's menu, F2): asks the new name on the next draw.
	void start_rename(const AssetEntry &entry);
	void draw_rename(const SessionView &view);
	void draw_references(const SessionView &view);

	EditorHost &host_;
	NewFilePrompt &new_file_;
	char filter_[128]{};
	std::string selected_;
	// What refresh() makes of the view, kept while its cache key stands; each file's path as the
	// filter compares it; the files the filter last matched and the filter they matched.
	const SessionView *view_ = nullptr;
	uint64_t key_ = 0;
	std::vector<Folder> folders_; // [0]: the project's own folder
	std::unordered_map<std::string, Counts> counts_;
	std::vector<std::string> compared_;
	std::string matched_;
	bool matches_made_ = false;
	std::vector<size_t> matches_;
	// The file a menu asked to rename or to list the references of, and the name typed.
	std::string renaming_;
	std::string references_;
	bool open_rename_ = false;
	bool open_references_ = false;
	char rename_[64]{};
	std::string previewed_; // the name Rename...'s preview was last asked for
	// A ShowInFiles ask: its serial, the file until Files draws it, and then the file to scroll
	// to and the place whose folders open on the way.
	uint64_t reveal_serial_ = 0;
	std::string reveal_;
	bool reveal_rename_ = false;
	std::string scroll_to_;
	std::string open_to_;
};

} // namespace opennova::editor
