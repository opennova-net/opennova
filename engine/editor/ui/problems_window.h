#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <editor/session/problem_query.h>
#include <editor/ui/problems_list.h>
#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// Every finding about the project (ADR 0046 d9, S11c), drawn from its model (ProblemsList,
// S13 V1: what it lists, folds, proposes and asks is the model's). A filter bar: the three
// severities with how many of each the project has (a hidden one dimmed), a text, the scope
// (the project, the active file, the open files), the grouping (none, by file, by kind: the
// window's first), only those with a fix, only those a build is refused for (each such row marked
// "Blocks the build", why in its tooltip), and how many of all are shown. While required files are
// missing, what stops the game and what it starts without, with the Fix alls that bring them. Then the
// list's lines, clipped to what shows: a finding's severity, its message cut to the line
// (whole in its tooltip), where it is and its first fix (More lists them all; a column too
// narrow for both has one Fix... that lists them); a group's header with its counts and its
// Fix all. A click selects a finding and goes to its place (problem_location) when it names
// one: its record in the file it opens, or the file in Files (a file the editor does not open,
// a file's name); the selected finding shows its whole message, every fix with what it does,
// and its code (a click on it folds it back). A Use fix and a Fix all say what they will do
// and wait for Apply, which counts only when pressed on what the confirmation shows now; a fix
// applies only when its release is on the fix the press was on.
class ProblemsWindow : public devtools::Window {
public:
	explicit ProblemsWindow(Workspace &workspace) : workspace_(workspace) { open = true; }

	const char *title() const override { return "Problems"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Bottom;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

	// How many findings the window has asked the fixes of while what they read stands (the
	// lines it drew, the selected one and More's, never every one).
	size_t fixes_asked() const { return list_.fixes_asked(); }
	// Comes forward showing only what a build is refused for (the menu bar's "Build refused", the build
	// result's "Show them in Problems"): every one, whatever the scope, the severities, the text and Only
	// fixable hid, which come back when "Blocks the build" is turned off.
	void show_blocking();

private:
	using Line = ProblemsList::Line;

	const ProblemAnswer &draw_filters(const SessionView &view);
	void draw_summary(const SessionView &view);
	void draw_list(const SessionView &view, const ProblemAnswer &answer);
	void draw_lines(const SessionView &view, const ProblemAnswer &answer, size_t first, size_t last);
	void draw_header(const SessionView &view, const ProblemAnswer &answer, const Line &line);
	void draw_finding(const SessionView &view, const Line &line, bool expanded);
	// `note`: what each fix says first (a finding about the game's own data: a file the game ships).
	void draw_fixes(const SessionView &view, size_t finding, const std::vector<ProblemFix> &fixes, const std::string &note);
	void draw_more(const SessionView &view);
	void draw_confirm(const SessionView &view);
	// A fix's button (or More's row): true when its click counts (PressLatch).
	bool fix_pressed(const SessionView &view, size_t finding, const ProblemFix &fix, bool clicked);
	// A fix chosen: raised, or a Use fix's confirmation asked.
	void apply(const SessionView &view, size_t finding, const ProblemFix &fix);
	void ask(const SessionView &view, ProblemsList::Confirmation confirmation);
	// "Blocks the build" on: the filters that could hide a refusal set aside; off: put back.
	void set_blocking(bool on);

	Workspace &workspace_;
	ProblemsList list_;
	char text_[128]{}; // the filter box, the query's text
	// The query and the filter box's text as they were when "Blocks the build" was turned on.
	std::optional<ProblemQuery> before_blocking_;
	std::string text_before_;
	ProblemsList::FindingRef more_; // the finding whose fixes More lists
	bool open_more_ = false;
	bool open_confirm_ = false;
	ProblemsList::PressLatch fix_press_;   // the fix control a press was on
	ProblemsList::PressLatch apply_press_; // the confirmation's version Apply was pressed on
};

} // namespace opennova::editor
