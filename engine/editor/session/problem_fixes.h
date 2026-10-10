#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <editor/graph/reference_queries.h>
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
// game reads (a catalog or stylesheet that defines nothing yet; a sound set's bank the game
// searches, a menu SOUND's own; Show it in Files when the editor does not edit it); first,
// where that file's type defines names of the kind (DocumentType::define_symbol, ADR 0046
// DI-15), Add it there: a record of the kind named as referenced, born as the type's Add makes
// one, where the lookup finds it (a string id in its section, a variable in a stylesheet the
// game reads, a weapon, ammo, item (on its id, by the reserved-id rule) or powerup row, a sound
// set, a sound profile, a menu's screen or window, an effect block), an edit of that file's
// document opened first that selects it, one step its Undo takes back (the file naming it is
// not edited), offered where the batch applies and the file, as its Save would write it,
// defines the name where the game's lookup finds it. An animation map with no anim_reset row: Add one (an edit of
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
// rewrite drops or normalizes (a Rewrite row: style.line_ending, animation_map.ignored_input,
// strings.regrouped): Rewrite the file, the
// row's rewrite_does saying what that does, unless a finding of the file says it does not
// serialize (a blocks_save row: *.unserializable, *.invalid_input; its Save is refused). An
// item on an id the engine keeps for another kind (catalog.reserved_kind): Use an id of its own;
// one named as a place the engine finds by an id the project lacks (catalog.reserved_name): Use
// that id; each a Rename everywhere of the item's id (never in bulk); an item on an id an earlier item
// has (catalog.item_identity): Use an id of its own, the project's free id set as its id (an edit of
// its document: nothing reaches it by the id). An items.def whose first row
// is no Null marker (catalog.first_row): Add a Null marker first (an edit of its document). A
// finding whose maker planned an edit of its own file (an EditRecord row, ADR 0046 DI-11:
// Diagnostic::planned, made with the file's records at hand: an SSN, a zone id, a name or a key
// of its own for a record no lookup finds, a string removed that no lookup reads, an end pose's
// trigger moved onto the last frame the game plays): each, an edit of its document (edit_fix). A
// stylesheet variable no menu names (style.unused): Remove the line, where no menu's text names it
// inside a longer text either (the render check's variables). Each edit of a document is opened
// first, one step Undo takes back, never in bulk.
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

	// The document a missing name's Add it there plans over (ADR 0046 DI-15): the open one as it stands, else
	// the file read from the disk once for the index (an open of it gives its records the same identities, the
	// validation cache's rule), its trial copy; null for a file that does not read.
	const DocumentBase *definer(const SessionView &view, const AssetEntry &file) const;
	// The copy a plan is tried on (applied, read, and undone, so it stands as read between tries), kept with the
	// index: a closed file read from the disk; an open document read again from what it would save, while it
	// stands at that state (its instance, its load, its revision). Null for one that does not read.
	DocumentBase *trial(const SessionView &view, const AssetEntry &file) const;

private:
	struct Copy {
		std::shared_ptr<DocumentBase> document;
		uint64_t identity = 0, generation = 0, revision = 0; // the open document's state it copies (0: the file's)
		bool read = false;
	};
	mutable std::map<std::string, Copy> copies_;
};

// The fixes for a finding over the view as it is, the first the one a click applies.
std::vector<ProblemFix> fixes_for(const Diagnostic &diagnostic, const SessionView &view,
                                  const ProblemFixIndex *index = nullptr);

// A planned edit of the file at `path` as the fix Problems offers (ADR 0046 DI-11): its words, saying
// Undo takes it back, and an edit_record of its batch, the document opened first; never in bulk. What an
// EditRecord row's finding offers (Diagnostic::planned), and what a fix planning an edit of another
// file than its finding's makes of it.
ProblemFix edit_fix(const std::string &path, const PlannedFix &planned);

// Where a Go to on a name nothing resolves lands (the deep-integration plan's DI-17: a Go to always lands
// somewhere): a symbol's (a string id, a style variable, a menu screen or window, a weapon, ammo or item, a
// particle effect, a user point) the file where it belongs, as its fix opens it (the file its scope names,
// the one defining its kind's other names, the table of its kind the game reads), `missing` set; false for a
// symbol of a kind no file of the project defines, and for a file, which has no place in the project
// before it is made (its finding's fixes in Problems: Import, Create). `name` as the reference writes it.
bool missing_target(const ReferenceSubject &missing, const SessionView &view, ReferenceTarget &out);
// The same for a field's value where it resolves to nothing (graph/reference_queries' reference_status).
bool missing_target(const FieldUse &field, const Value &value, const SessionView &view, ReferenceTarget &out);
// The project file where a missing symbol belongs: the one its scope names (its row's scope_names_file: a
// string id's table, the menu an ACTION's screen or window is looked up in, the model whose user points an
// item's particle slot names), a sound set's bank (a menu SOUND's own, else one on the chain the game
// searches), else the file that defines the other symbols of its kind that a lookup finds, else the table its
// kind goes in (a catalog or a stylesheet that defines nothing yet: the one the game reads by name, else the
// project's first of its kind); null when the project has none. Where an item a model makes goes too (ADR
// 0046 DI-12, preview/model_placement.h).
const AssetEntry *defining_file(const ReferenceSubject &missing, const SessionView &view);
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
