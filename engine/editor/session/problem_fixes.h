#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

// What Problems offers to do about a finding (ADR 0046 S11b). A fix is an ordinary typed
// request, so the window, the editor MCP and a test apply it the same way, and the
// session's handler checks again before it acts (a file that appeared since is refused,
// never overwritten). `label` is the offer in plain words, `detail` its tooltip and what a
// confirmation says it will do (a fix that acts on the files, which Undo cannot take back,
// says so; one that only shows a place, or edits a document, does not); a `bulk` fix is one
// a Fix all may apply with the others. Which of the fixes below a finding gets is its code's
// row's (session/finding_codes.h: FindingCodeRow::fixes), and what it is about is the finding's
// subject.
//
// A file the project lacks that the game reads by name (requirement.missing,
// requirement.optional_missing, play.boot_missing), while its row is missing: Create it
// from its role's factory (placeholder content), or Import it when the game install has
// it; a required one (the game cannot start without it) also: Use a file of the kind and
// extension as it (a rename, with what it rewrites; never in bulk, a few files at most,
// none another requirement names or an import makes). Its name taken by a file of another
// kind (requirement.wrong_kind): Import the game's own (the import dialog replaces), or
// Rename... that file (Create and Use would be refused while the name is taken). A file
// name the archives cannot take or another file has (asset.name.too_long,
// asset.name.duplicate, build.name_unstorable): Rename... it (Files asks the new name). A
// missing symbol (a string id, a style variable, a menu screen or window, a weapon, ammo
// or item, a particle effect, a user point): Open the file where it belongs, the one its
// scope names, the one that defines its kind's other names, or the table of its kind the
// game reads (a catalog or stylesheet that defines nothing yet; Show it in Files when the
// editor does not edit it). An animation map with no anim_reset row: Add one (an edit of
// its document, which Undo takes back). A missing reference to a file:
// Import a name its loader reads when the game install has one, or Create it blank when
// its kind has a free-form factory and the name is free and one the project's name rules
// take (not in bulk); a texture: Create a placeholder, the game's own missing-texture
// checkerboard as the file the reference's loader opens, in the format that loader reads it
// as (in bulk; none for a model's chunk row, a name the texture factory refuses, a model row
// whose reader would read the file in another format, or a texture of no model row whose
// own extension the factory cannot write). A texture an import's model names that the
// import did not bring (import.texture_not_imported): the same, while the project still
// lacks it. An import whose output is missing: import its source again. An open document
// whose file changed outside the editor (document.conflict: its Save is refused): Reload
// it, which asks about its unsaved edits first, or, while it has them, Keep my edits and save
// over it, a Save that writes over the file once Problems confirmed it (ADR 0046 DI-01; neither
// in bulk). Input a
// rewrite drops or normalizes (a Rewrite row: style.line_ending, menu.ignored_input,
// animation_map.ignored_input, strings.regrouped): Rewrite the file, the
// row's rewrite_does saying what that does, unless a finding of the file says it does not
// serialize (a blocks_save row: *.unserializable, *.invalid_input; its Save is refused). An
// item on an id the engine keeps for another kind (catalog.reserved_kind): Use an id of its own;
// one named as a place the engine finds by an id the project lacks (catalog.reserved_name): Use
// that id; each a Rename everywhere of the item's id (never in bulk). An items.def whose first row
// is no Null marker (catalog.first_row): Add a Null marker first (an edit of its document).
// Every other finding has none: Problems goes to its place.
struct ProblemFix {
	std::string label;
	std::string detail;
	EditorRequest request;
	bool bulk = false;
};

// What the fixes read of a view's findings as a whole, found in one pass: the files whose
// own finding says they do not serialize (a blocks_save row's: no Rewrite for them). A caller asking about many
// findings of one view finds it once and hands it to each ask (answer_problems, the fix
// cache); an ask without one finds it for itself.
struct ProblemFixIndex {
	explicit ProblemFixIndex(const SessionView &view);
	std::unordered_set<std::string> unserializable; // project-relative paths
};

// The fixes for a finding over the view as it is, the first the one a click applies.
std::vector<ProblemFix> fixes_for(const Diagnostic &diagnostic, const SessionView &view,
                                  const ProblemFixIndex *index = nullptr);
// Whether the finding has a fix, without planning a Use fix's rename (fixes_for plans it).
bool has_fixes(const Diagnostic &diagnostic, const SessionView &view, const ProblemFixIndex *index = nullptr);
// The finding's bulk fixes, in order, without planning a Use fix's rename (a Use fix is
// never in bulk): what a Fix all over many findings reads.
std::vector<ProblemFix> bulk_fixes_for(const Diagnostic &diagnostic, const SessionView &view,
                                       const ProblemFixIndex *index = nullptr);
// A Fix all: the bulk fixes among `fixes` (the caller passes each finding's first bulk fix)
// as the requests to raise, in the order the fixes come: one CreateMissing naming every
// role, one game-data import list naming every file, and every other request once; an import's
// preview last (it starts an operation over the project's files, which a request after it would
// find busy).
std::vector<EditorRequest> merge_fixes(const std::vector<ProblemFix> &fixes);

// What the fixes of a view's findings read, as a cache's key (view_revisions.h): the
// findings; the project (open, its folder); the files (the scan, the requirements, the game
// install's file names); the graph (where a symbol belongs, a Use fix's rename); which
// documents are open and unsaved (a Reload, a Rewrite's unsaved edits: DocumentSet, which an
// edit that leaves a document as unsaved as it was does not move); the editor's settings (an
// Import's dependencies).
RevisionKey problem_fix_key(const SessionView &view);

// The fixes of a view's findings, kept while what they read stands (problem_fix_key): the
// window asks for the rows it shows every frame and the editor MCP for a page on every call,
// and a Use fix plans a rename (the graph's edges walked, the name checked on the disk). The
// view's index is found once for it.
class ProblemFixCache {
public:
	// The fixes of view.findings.diagnostics[index] (fixes_for).
	const std::vector<ProblemFix> &fixes(const SessionView &view, size_t index);
	// Its bulk fixes (bulk_fixes_for: cheap, not kept).
	std::vector<ProblemFix> bulk(const SessionView &view, size_t index);
	// How many findings' fixes it holds for the key it keeps (each planned once).
	size_t size() const { return fixes_.size(); }
	// How many times it has started again (the view or its key moved), `view` followed first:
	// what a reader made of the fixes (the Problems list's Fix alls) is made again when this
	// moves.
	uint64_t generation(const SessionView &view) {
		follow(view);
		return generation_;
	}

private:
	// Forgets what it keeps when the view or its key moved; the view's index.
	const ProblemFixIndex &follow(const SessionView &view);

	const SessionView *view_ = nullptr;
	RevisionKey key_;
	std::unique_ptr<ProblemFixIndex> index_;
	std::map<size_t, std::vector<ProblemFix>> fixes_;
	uint64_t generation_ = 0;
};

} // namespace opennova::editor
