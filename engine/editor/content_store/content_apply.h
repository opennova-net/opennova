#pragma once

// A pull's plan applied to its working copy (the content store, ADR 0069). Before anything moves:
// every conflict settled, every file the plan writes downloaded (ContentPaths::incoming_dir, each
// named by its SHA-256 and checked by it as it landed), every path the plan touches still holding
// the file the plan saw (its SHA-256 read again; a file the author changed since the plan stops
// the apply, which a new plan takes in), and every file it writes copied from its download beside
// the downloads. Then the files it replaces or removes go to the project's trash in one batch
// (project/project_trash.h: `.opennova/trash/<n>/`, kept until Files' Empty the trash); a folder a
// removal left empty goes; the copies are renamed into place (one that does not go brings the
// files it and the copies after it would replace back out of the trash); and the new base is
// written, then the binding, last. No journal: a crash part way leaves the old base, and the next
// pull plans from it again, finding the files already put in place to be the revision's own
// (the same: nothing to do) and finishing the rest.

#include <cstddef>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <editor/content_store/content_plan.h>
#include <editor/content_store/content_state.h>
#include <editor/project/project_trash.h>

namespace opennova::editor {

struct ProjectPaths;

struct ApplyResult {
	size_t written = 0;
	size_t removed = 0;
	TrashBatch trash; // the files it replaced or removed (id 0 when none)
	// The paths whose files changed since the plan was made: the apply moved nothing.
	std::vector<std::string> changed_since;
};

// `plan` applied to the folder at `paths.root`, whose files become the revision
// `binding.revision`'s `theirs` (its new base). False with `error` when it cannot: a conflict left,
// a download missing, a file changed since the plan (`result.changed_since`), or a file the system
// would not copy, move or rename (the files renamed into place before it stay; the base is the old
// one, so a pull again finishes).
bool apply_pull(const ProjectPaths &paths, const PullPlan &plan,
		const std::vector<content::ContentFile> &theirs, const ContentBinding &binding,
		ApplyResult &result, std::string &error);

// Where a pull lands the download of the file `sha256` (ContentPaths::incoming_dir/<sha256>).
std::string incoming_path(const ContentPaths &paths, const std::string &sha256);

} // namespace opennova::editor
