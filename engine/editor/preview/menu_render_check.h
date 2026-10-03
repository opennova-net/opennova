#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/project_asset_source.h>
#include <editor/documents/project_check.h>
#include <editor/documents/validation_cache.h>
#include <editor/model/diagnostic.h>
#include <editor/model/node.h>
#include <editor/preview/menu_screen_render.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>

namespace opennova::editor {

class MnuDocument;
class ProjectChecks;

// The sentence a compiler note shows (the engine keeps codes only): what the game does,
// "The game ..." for a witnessed rule; "OpenNova ..." for the port's own choice or a
// known gap in it (docs/mnu/menu-re.md "Compiler notes").
std::string menu_note_message(const menu::MenuFrameNote &note);
// Whether a note is a Problems row and at what severity, as its row says (the menu type's
// finding codes, finding_code(note).problem); false for the notes only the preview shows: a
// missing reference is the asset graph's finding (a %VAR%, a string id, a font, a texture or a
// string table the project lacks), and a few notes only explain the picture (a CUSTOM hook, a
// state held, a frame whose stencil did not load, a table or marquee filled at run time).
bool menu_note_problem(menu::MenuFrameNoteCode code, DiagnosticSeverity *severity);
// Where a note sits in the document: the screen row, the window, or the record of the
// window's list the note names (an empty field when that record is not there).
NodeAddress menu_note_address(const menu::MenuFrameNote &note, const MnuDocument &document, const Node &screen_row,
                              std::string *field);
// A note as a finding on the record and the field it names.
Diagnostic menu_note_diagnostic(const menu::MenuFrameNote &note, const MnuDocument &document, const Node &screen_row,
                                DiagnosticSeverity severity);

// What a step of the render check spends on a menu of the scan it does not render again (S13 A3);
// a menu it renders ends the step.
inline constexpr uint64_t kMenuStepCost = 4096;

// The render check (ADR 0046 S9j2), the menu type's project check (S13 V9: its registry row
// makes it, make_menu_render_check, and whoever validates keeps it among the types' checks,
// documents/project_checks.h): every screen of every menu in the project compiled headless the
// way the game draws it (MenuScreenRender), its notes that are a consequence the author may not
// mean as Problems rows (menu_note_problem). It runs with every validation; its findings are
// never part of the build's gate (a note never blocks a build). The graph reports what a menu
// names that the project lacks; the check reports what the rest comes to, never the same finding
// twice.
class MenuRenderCheck : public ProjectCheck {
public:
	// Every menu in the scan, the open documents standing in; a closed one the check reads
	// itself (one whose own checks read its records: the validation read it, and no source error
	// blocks it) and keeps while the scan lists its file as it was (its size, last write and kind,
	// and the project's game). A menu renders again only when its document state or a file one
	// of its screens read moved, or when the shell's stylesheets moved and a variable the menu
	// names (every %NAME% its saved text holds, as the game's expansion finds them) came, went or
	// took another value: a stylesheet edit that changes no variable's value renders nothing.
	// True when the notes may have moved: a menu rendered again, or one's notes went.
	bool update(const ProjectCheckInput &input) override;
	// A menu a step (S13 A3): each step goes through the scan's menus from where the last left off,
	// reusing those that did not move, and ends once it rendered one (or spent `budget`, a menu
	// kMenuStepCost); the step that reaches the end lets the menus no longer listed go and composes
	// the notes.
	void begin() override;
	bool step(const ProjectCheckInput &input, uint64_t budget, bool &moved) override;
	void clear() override;
	const std::vector<Diagnostic> &findings() const override { return diagnostics_; }
	// A menu document's screens compiled alone with the project's files and the shell's variables as the
	// last update read them, its notes as they would be Problems rows; nothing kept (an install copy's,
	// for the game's own data's fold).
	void findings_of(const DocumentBase &document, const ProjectCheckInput &input,
	                 std::vector<Diagnostic> &out) const override;
	// The render of a screen row of the menu at `path`, null when there is none.
	const MenuScreenRender *render(const std::string &path, NodeId screen_row) const;
	// The menu document the last update rendered for `path` (the open one, or the closed
	// file as the check read it), null when none.
	const MnuDocument *document(const std::string &path) const;
	// Menus rendered by the last update (not reused), for the tests.
	size_t rendered() const { return rendered_; }

private:
	struct Screen {
		NodeId row = 0;
		std::unique_ptr<MenuScreenRender> render;
	};
	struct Menu {
		// The closed file as the check read it (null: it does not load, or a source error blocks
		// it), and the scan's row it was read at; nothing while the menu is open.
		bool read = false;
		std::shared_ptr<const MnuDocument> closed;
		uint64_t size = 0;
		int64_t modified = 0;
		AssetKind kind = AssetKind::Unknown;
		std::string game;
		// The document the renders are of.
		std::shared_ptr<const MnuDocument> document;
		uint64_t identity = 0, load_generation = 0, revision = 0;
		std::vector<Screen> screens;
		std::vector<menu::MenuDependency> dependencies;
		std::vector<std::string> variables; // the variables its text names, sorted (upper case)
		std::vector<Diagnostic> findings;
		bool seen = false;
	};
	static void render_menu_(Menu &menu, const MnuDocument &document, const FileSource &files,
	                         const std::map<std::string, std::string> &vars);

	std::map<std::string, Menu> menus_; // by project-relative path
	menu::MenuStyleSource style_;
	std::vector<menu::MenuDependency> style_stamps_;
	std::map<std::string, std::string> vars_; // the shell's variables the renders were made with
	std::vector<Diagnostic> diagnostics_;
	size_t rendered_ = 0;
	// The update under way: whether it started (the variables read, the menus not seen yet), the
	// scan's file it goes on from, and the shell's variables it renders with.
	struct Cursor {
		bool started = false;
		size_t next = 0;
		std::map<std::string, std::string> vars;
	};
	Cursor cursor_;
	// Kept until an update ends, however often one starts again: the variables that changed (the
	// menus naming one render again) and whether a menu rendered or one's notes went (the notes are
	// composed again).
	std::vector<std::string> changed_;
	bool moved_ = false;
};

// The render check among a validation's project checks (the menu type's; the registry's hook that
// makes it is make_menu_render_check.h's): null when `checks` is null (a view no session made: a
// session makes its checks when it is made) or the menu type's check is not the render check (a
// test's stand-in in its place).
const MenuRenderCheck *menu_render_check(const ProjectChecks *checks);

} // namespace opennova::editor
