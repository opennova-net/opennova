#pragma once

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
// and says whether its findings moved, so a validation that moves nothing composes nothing. A use
// check (graph/use_checks.h) is the other cross-file finding: stateless, a function of the asset
// graph made again with every composition. A project check's findings are Problems rows after the
// build's gate (project/project_findings.h), never in it: what the game makes of the files there
// never blocks a build.
//
// A type's registry row makes its check (DocumentType::project_check). The row is constexpr, so the
// instance lives with whoever validates, one per type by its DocumentTypeId (ProjectChecks: the
// session's ProblemsService keeps its checks from one validation to the next and lets go of what
// they held when the project closes; the command line makes them for its one validation).
//
// A validation stepped over several polls (S13 A3) runs the checks after the last file's own
// findings (a check reads which files' records their own checks read), each check's update one
// step, in the registry's order: update is the run-to-end form, and a check whose work is large
// (the render check's menus) takes a cursor of its own then. The scan and the open documents stay
// as the first step saw them until the last check ran: an edit between two steps starts the
// validation again.
class ProjectCheck {
public:
	virtual ~ProjectCheck() = default;
	// Brought to the project as `input` has it: true when its findings may have moved since its
	// last update (the rows are composed again only then).
	virtual bool update(const ProjectCheckInput &input) = 0;
	// Its findings as its last update left them, on the records and fields that cause them.
	virtual const std::vector<Diagnostic> &findings() const = 0;
	// The project closed: what it held of it goes, and the next update starts over.
	virtual void clear() = 0;
};

} // namespace opennova::editor
