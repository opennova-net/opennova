#pragma once

#include <cstdint>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/documents/validation_cache.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// What a project check reads (ADR 0046 S13 V9), the same for every check at every validation: the
// project's files as the validation read them (the paths, the project, the scan, whose rows carry
// each file's size, last write and kind, and the open documents standing in for their files);
// which of them the files' own checks read the records of (ValidationCache::records_checked: a
// file that did not load, or that a source error blocks, reports that alone); and the files as
// the game looks them up by name, the open documents standing in (their bytes, and a stamp that
// moves whenever they may read otherwise).
struct ProjectCheckInput {
	const ValidationInput &validation;
	const ValidationCache &cache;
	const FileSource &files;
};

// A document type's own check across the project's files (ADR 0046 S13 V9), run with every
// validation after each file's own findings: the menu type's render check
// (preview/menu_render_check.h), every screen of every menu compiled the way the game draws it.
// It reads the files itself, keeps what it made from one validation to the next, makes again only
// what moved (a menu renders again when it, a file its screens read or a variable it names moved)
// and says whether its findings moved, so a validation that moves nothing composes nothing.
//
// A use check (graph/use_checks.h) is the other cross-file finding. Which one a finding is, is
// chosen by whether it may block a build: a use check's rows are inside the build's gate, a project
// check's never are (they are Problems rows after the gate, project/project_findings.h: what the
// game makes of the files there never blocks a build). The rest follows from that. A use check
// takes the asset graph and the validation cache, keeps nothing and is made again with every
// composition, one per asset kind (kUseChecks); a project check takes the files and no graph,
// keeps its state, one per document type. A project check never repeats a use check's finding or
// the graph's: the notes the render check leaves out, its rows whose problem is None (the menu
// type's finding codes, which menu_note_problem reads: a name the project lacks is the graph's
// reference.missing, and so on), are the instance. One fact is read both ways today: which
// variables a menu uses, as the graph's StyleVar edges and as the render check's variables_named
// (the %NAME%s of the menu's saved text). The graph owns it: style.unused and Rename read the
// edges; variables_named only decides which menus to render again.
//
// A type's registry row makes its check (DocumentType::project_check). The row is constexpr, so the
// instance lives with whoever validates, one per type by its DocumentTypeId (ProjectChecks: the
// session's ProblemsService keeps its checks from one validation to the next and lets go of what
// they held when the project closes; the command line makes them for its one validation).
//
// A validation stepped over several polls (S13 A3) brings the checks up after the last file's own
// findings (a check reads which files' records their own checks read), in the registry's order,
// each a step at a time within the poll's budget: begin() starts a check's update, step() goes on
// until it is up, and update() is the two run to their end. A check whose work is small does it
// whole in its one step (the default); the render check renders a menu a step, keeping what an
// update started again has not made yet (a stylesheet's variables that changed, a menu rendered
// before it ended). The scan and the open documents stay as the first step saw them until the last
// check is up: an edit between two steps starts the validation again.
class ProjectCheck {
public:
	virtual ~ProjectCheck() = default;
	// Brought to the project as `input` has it: true when its findings may have moved since its
	// last update (the rows are composed again only then).
	virtual bool update(const ProjectCheckInput &input) = 0;
	// The update started over, then gone on with a step at a time within `budget` bytes: true once
	// the check is up, `moved` then saying what update says.
	virtual void begin() {}
	virtual bool step(const ProjectCheckInput &input, uint64_t budget, bool &moved) {
		(void)budget;
		moved = update(input);
		return true;
	}
	// Its findings as its last update left them, on the records and fields that cause them.
	virtual const std::vector<Diagnostic> &findings() const = 0;
	// What it finds of one document alone, standing in for its file, the rest of the project as `input`
	// has it, keeping nothing: what the game's own data's fold asks of a file's install copy
	// (session/original_files.h). Nothing for a document the check does not read.
	virtual void findings_of(const DocumentBase &document, const ProjectCheckInput &input,
	                         std::vector<Diagnostic> &out) const {
		(void)document;
		(void)input;
		(void)out;
	}
	// The project closed: what it held of it goes, and the next update starts over.
	virtual void clear() = 0;
};

} // namespace opennova::editor
