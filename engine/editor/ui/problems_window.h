#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// Every finding about the project (ADR 0046 d9, S11c). A filter bar: the three severities
// with how many of each the project has (a hidden one dimmed), a text, the scope (the
// project, the active file, the open files), the grouping (none, by file, by kind: the
// window's first), only those with a fix, and how many of all are shown. While required
// files are missing, a line saying the game cannot start, with the Fix alls that make them.
// Then the findings asked for (problem_query), worst first, a line each: the severity, the
// message cut to the line (whole in its tooltip), where it is and its first fix (More lists
// them all; a column too narrow for both has one Fix... that lists them); grouped, each
// group under a header with its counts and, when several of its findings have one, a Fix
// all, a group of notes alone folded the first time the project shows it (S11e: the list
// opens on the errors and warnings) and opened again when an error or a warning joins it,
// unless the user folded it. A click selects a finding and goes to its place
// (problem_location) when it names one: its record in the file it opens, or the file in
// Files (a file the editor does not open, a file's name); the selected finding shows its
// whole message, every fix with what it does, and its code (a click on it folds it back).
// A Use fix and a Fix all
// say what they will do and wait for Apply, remade from the findings they are for while
// they wait. The query is the window's for the session.
//
// A finding is known by what it is about (its key: the code, the file, the record, the
// field, the line and what it names), never by its place among the findings, which a
// validation between a press and its release can change; a fix applies only when the
// release is on the fix the press was on.
class ProblemsWindow : public devtools::Window {
public:
	explicit ProblemsWindow(EditorHost &host) : host_(host) {
		open = true;
		query_.grouping = ProblemGrouping::Kind;
	}

	const char *title() const override { return "Problems"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Bottom;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

	// How many findings the window has asked the fixes of while what they read stands
	// (fixes_for, each planned once): the lines it drew, the selected one and More's, never
	// every one.
	size_t fixes_asked() const { return fixes_.size(); }

private:
	// A line of the list: a group's header, or a finding (its index in the view's findings).
	struct Line {
		bool header = false;
		size_t group = 0;
		size_t finding = 0;
	};
	// A finding the window keeps across validations: its project and its key.
	struct FindingRef {
		std::string root;
		std::string key;
	};
	// What a confirmation is about: its project, the findings it acts on (their keys) and
	// which of their fixes: each one's first bulk fix, merged (a Fix all); the merged request
	// of one kind among those (a summary button); the fix a label names (a Use fix).
	struct Confirmation {
		enum class Of { FixAll, Kind, Fix };
		Of of = Of::FixAll;
		std::string root;
		std::vector<std::string> keys;
		EditorRequestKind kind = EditorRequestKind::CreateMissing;
		std::string label;
	};
	// What a confirmation says and what its Apply raises, and how many findings gave a fix.
	struct Proposal {
		std::vector<std::string> lines;
		std::vector<EditorRequest> requests;
		size_t findings = 0;
	};

	const ProblemAnswer &refresh(const SessionView &view);
	void draw_problems(const SessionView &view);
	const ProblemAnswer &draw_filters(const SessionView &view);
	void draw_summary(const SessionView &view);
	void draw_list(const SessionView &view, const ProblemAnswer &answer);
	void draw_lines(const SessionView &view, const ProblemAnswer &answer, size_t first, size_t last);
	void draw_header(const SessionView &view, const ProblemAnswer &answer, const Line &line);
	void draw_finding(const SessionView &view, const Line &line, bool expanded);
	void draw_fixes(const SessionView &view, size_t finding, const std::vector<ProblemFix> &fixes);
	void draw_more(const SessionView &view);
	void draw_confirm(const SessionView &view);
	size_t resolve(const SessionView &view, const FindingRef &ref) const;
	std::string fix_id(const SessionView &view, size_t finding, const ProblemFix &fix) const;
	bool released_on(bool clicked, const std::string &id);
	Proposal propose(const SessionView &view, const Confirmation &confirmation);
	Confirmation fix_all_of(const SessionView &view, const std::vector<size_t> &findings) const;
	void apply(const SessionView &view, size_t finding, const ProblemFix &fix);
	void ask(const SessionView &view, Confirmation confirmation);
	void open_more(const SessionView &view, size_t finding);

	EditorHost &host_;
	ProblemQuery query_;
	char text_[128]{}; // the filter box, query_.text
	ProblemQueryCache answers_;
	ProblemFixCache fixes_;
	// What refresh() makes of the view, kept while what it reads (cache_key), the query and the
	// folded groups stand: each finding's key and the finding a key names, the lines of the list,
	// each group's Fix all and the summary's (the missing required files').
	const SessionView *view_ = nullptr;
	RevisionKey key_;
	ProblemQuery refreshed_;
	bool stale_ = true;
	std::vector<std::string> keys_;
	std::unordered_map<std::string, size_t> index_;
	std::vector<Line> lines_;
	std::vector<Proposal> group_fixes_;
	std::vector<size_t> required_;
	Proposal required_fixes_;
	std::set<std::string> folded_;      // the keys of the groups folded away
	std::set<std::string> auto_folded_; // those folded because they held notes alone, untouched since
	std::set<std::string> seen_groups_; // the keys of the groups shown so far (a group of notes alone folds when first seen)
	std::string groups_root_;           // the project they are of
	FindingRef selected_;
	size_t selected_index_ = SIZE_MAX;
	FindingRef more_; // the finding whose fixes More lists
	bool open_more_ = false;
	std::string pressed_; // the fix control a press was on: its finding and its request
	// The confirmation: what it is about, what it shows (remade when the view moves; the
	// version moves when that changes) and the version Apply was pressed on.
	bool open_confirm_ = false;
	Confirmation confirm_;
	Proposal shown_;
	const SessionView *shown_view_ = nullptr;
	RevisionKey shown_key_;
	uint64_t shown_version_ = 0;
	uint64_t apply_pressed_ = 0;
	bool shown_changed_ = false;
};

} // namespace opennova::editor
