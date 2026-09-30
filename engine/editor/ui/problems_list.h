#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/requirements/requirements.h>
#include <editor/session/editor_request.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

// What the Problems window shows of the project's findings and what it asks before it acts
// (ADR 0046 S11c, S11e; S13 V1: the model decides, ProblemsWindow draws). It answers the one
// Problems query (problem_query) and knows each finding by what it is about (its key: the
// code, the file, the record, the field, the line and what it names; findings alike in all of
// it told apart by their order), never by its place among the findings, which a validation
// between a press and its release can change.
//
// The list: the findings the query shows, worst first, a line each; grouped, a header per
// group before its findings. A group of notes alone starts folded the first time the project
// shows it (the list opens on the errors and warnings) and opens again once it holds an error
// or a warning, while one the user folded stays as the user left it; the groups folded and
// seen are the project's. A group has a Fix all when several of its findings have a fix that
// runs with the others; while required files are missing the summary says the game cannot
// start and offers the Fix alls that make them (one Create naming every file a factory makes,
// one import list of those the game data has). A finding selected (drawn whole with every
// fix) is kept by its key.
//
// A Fix all and a Use fix (a rename) wait in a confirmation for Apply: what it says and raises
// is proposed again from the findings it is for whenever the view moves, and its version moves
// when that changes, so a press of Apply on what it said before counts for nothing.
class ProblemsList {
public:
	// A line of the list: a group's header, or a finding (its index in the view's findings).
	struct Line {
		bool header = false;
		size_t group = 0;
		size_t finding = 0;
	};
	// A finding kept across validations: its project and its key.
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
	// A control's press held until its release: a mouse click counts only when the release is
	// on what the press was on (`id`: a fix of a finding, a confirmation's version), since the
	// view can move between the two and put another under the mouse; an activation pressed and
	// released in one frame (a key) is on what that frame shows.
	class PressLatch {
	public:
		// The control just drawn as `id`: `pressed` its activation this frame, `clicked` its
		// click, `mouse_released` whether the left button went up this frame. True when the
		// click counts.
		bool released_on(const std::string &id, bool pressed, bool clicked, bool mouse_released);

	private:
		std::string pressed_;
	};

	ProblemsList();

	// What the list keeps what it makes by for `query`, one key over all it reads of the view
	// (session_revisions.h): the answer's (problem_query_key: the findings; which document is
	// active for the Active file scope, which are open for the Open files scope) and the
	// fixes' (problem_fix_key: the findings, the project, the files (the scan, the
	// requirements, the game install's file names: whether a fix creates a file or imports
	// it), the graph (a Use fix's rename), which documents are open and unsaved (a Reload, a
	// Rewrite's unsaved edits), the settings (an Import's dependencies)); each finding's key,
	// the lines, the Fix alls and a confirmation's proposal read both. The answer and the fixes
	// are their caches' (problem_query, problem_fixes), each kept by its own key; the list is
	// made again whenever either is made anew as well, so a line never indexes an answer it
	// was not made from.
	static RevisionKey cache_key(const SessionView &view, const ProblemQuery &query) {
		return problem_query_key(view, query) | problem_fix_key(view);
	}

	// The query the window's filters set (grouped by kind until one is picked).
	ProblemQuery &query() { return query_; }
	// The query's answer: the rows it shows and every finding's counts, whatever it shows.
	const ProblemAnswer &answer(const SessionView &view) { return answers_.answer(query_, view); }
	// The answer, and what the list makes of it, made again when the view, its cache key, the
	// query or the folded groups moved, or the answer or the fixes were made anew: each
	// finding's key, the lines, each group's Fix all and the summary's.
	const ProblemAnswer &refresh(const SessionView &view);

	// --- after refresh ----------------------------------------------------------------------
	const std::vector<Line> &lines() const { return lines_; }
	const std::string &key(size_t finding) const { return keys_[finding]; }
	FindingRef ref(const SessionView &view, size_t finding) const {
		return {view.project_root, keys_[finding]};
	}
	// The finding a reference names now; SIZE_MAX when it is gone or of another project.
	size_t resolve(const SessionView &view, const FindingRef &ref) const;
	bool folded(const std::string &group) const { return folded_.count(group) != 0; }
	// A click on a group's header: folded away, or opened again; the user's from now on.
	void toggle_fold(const std::string &group);
	// A group's Fix all (in the answer's groups' order): shown while several of its findings
	// give a fix.
	const Proposal &group_fixes(size_t group) const { return group_fixes_[group]; }
	// The summary's sentence while required files are missing or are other kinds of file than
	// the game reads ("" while none is), and the Fix alls that make them.
	static std::string summary(const RequirementReport &report);
	const Proposal &required_fixes() const { return required_fixes_; }

	// --- the selected finding ---------------------------------------------------------------
	size_t selected() const { return selected_index_; }
	// A click on a finding's line: it is selected or, selected already, folded back to none.
	void toggle_selected(const SessionView &view, size_t finding);

	// --- fixes ------------------------------------------------------------------------------
	// A finding's fixes (fixes_for), planned only when asked: the lines the window draws, the
	// selected one and More's, never every one.
	const std::vector<ProblemFix> &fixes(const SessionView &view, size_t finding) {
		return fixes_.fixes(view, finding);
	}
	// How many findings' fixes are planned while what they read stands.
	size_t fixes_asked() const { return fixes_.size(); }
	// What a fix's control is known by for a press: its project, its finding's key, its request.
	std::string fix_id(const SessionView &view, size_t finding, const ProblemFix &fix) const;
	// Whether a fix waits in a confirmation before it acts (a Use fix renames a file and
	// rewrites what names it); any other fix is raised as it is, its detail in its tooltip.
	static bool asks_first(const ProblemFix &fix) {
		return fix.request.kind == EditorRequestKind::AssignRequirement;
	}
	// The confirmations: a Fix all over findings (a group's), the summary's of one kind, a Use fix.
	Confirmation fix_all_of(const SessionView &view, const std::vector<size_t> &findings) const;
	Confirmation required_fix(const SessionView &view, EditorRequestKind kind) const;
	Confirmation use_fix(const SessionView &view, size_t finding, const ProblemFix &fix) const;
	// What a confirmation shows and raises now: its findings found again by their keys (the
	// gone ones left out), each one's first bulk fix merged (the fix cache's bulk: no rename
	// planned) or, for a Use fix, the fix its label names with what the rename rewrites today.
	Proposal propose(const SessionView &view, const Confirmation &confirmation);

	// --- the confirmation waiting for Apply -------------------------------------------------
	void ask(const SessionView &view, Confirmation confirmation);
	// Follows the view while it shows: proposed again when the list was made again (refresh:
	// what it reads of the view moved), its version moving when what it shows changed
	// (changed() then says so). False when it belongs to another project, or none is open: it
	// closes.
	bool follow(const SessionView &view);
	const Proposal &shown() const { return shown_; }
	uint64_t version() const { return shown_version_; }
	bool changed() const { return shown_changed_; }
	// Applied or cancelled: it shows nothing any more.
	void close() { shown_ = Proposal(); }

	// --- words ------------------------------------------------------------------------------
	// Where a finding is, on one line: its file (the whole path, or its name) and line, the
	// record and the field ("items.def:12 - Marker - graphic"); for a finding no file of the
	// project is at fault for (a required file it lacks), the file it is about.
	static std::string location_of(const Diagnostic &diagnostic, bool whole_path);
	// "2 errors, 1 warning, 3 info": a group's counts, the zero ones left out.
	static std::string severity_counts(size_t errors, size_t warnings, size_t infos);
	// What a request of a Fix all does, in its confirmation's words.
	static std::string describe(const SessionView &view, const EditorRequest &request);
	// A Fix all's button in the summary: what it does and to how many files.
	static std::string fix_all_label(const EditorRequest &request);

private:
	ProblemQuery query_;
	ProblemQueryCache answers_;
	ProblemFixCache fixes_;
	// What refresh() made, kept while its view, cache key and query stand and the answer and
	// the fixes it read are their caches' still (their generations; stale_: a group folded or
	// opened since); made_ counts the times it was made.
	const SessionView *view_ = nullptr;
	RevisionKey key_;
	ProblemQuery refreshed_;
	uint64_t answered_ = 0;
	uint64_t fixed_ = 0;
	bool stale_ = true;
	uint64_t made_ = 0;
	std::vector<std::string> keys_;
	std::unordered_map<std::string, size_t> index_;
	std::vector<Line> lines_;
	std::vector<Proposal> group_fixes_;
	std::vector<size_t> required_;
	Proposal required_fixes_;
	std::set<std::string> folded_;      // the keys of the groups folded away
	std::set<std::string> auto_folded_; // those folded for holding notes alone, untouched since
	std::set<std::string> seen_groups_; // the keys of the groups shown so far
	std::string groups_root_;           // the project they are of
	FindingRef selected_;
	size_t selected_index_ = SIZE_MAX;
	// The confirmation: what it is about, what it shows (proposed again when the list is made
	// again; the version moves when that changes) and the list it was proposed from (made_).
	Confirmation confirm_;
	Proposal shown_;
	uint64_t shown_made_ = 0;
	uint64_t shown_version_ = 0;
	bool shown_changed_ = false;
};

} // namespace opennova::editor
