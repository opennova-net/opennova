#pragma once

// What a working copy's files are to NovaWorld's revisions (ADR 0069): its changes since
// its base (a status), and what pulling a revision does to it, planned a path at a time over three
// file lists: the base the folder was last made the same as, the folder as it is now
// (ContentScan), and the revision pulled ("theirs"). A file only the revision changed is taken; a
// file only the folder changed is kept, a change to push; a file both changed alike needs nothing;
// and a file both changed otherwise is a conflict the author settles, keeping the folder's or
// taking the revision's. Files are whole: nothing is merged. Two files are the same when both are
// absent or both have one SHA-256.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <base/project/project_manifest.h>

namespace opennova::editor {

enum class ContentChange : uint8_t { Added, Modified, Deleted };
const char *content_change_name(ContentChange change); // "added", "modified", "deleted"

// One file the folder changed since its base.
struct LocalChange {
	std::string path;
	ContentChange change = ContentChange::Modified;
};

// The folder's changes since its base, by path. `unread` (ContentScan::unread) are files the folder
// holds though their bytes could not be read: never taken for deleted. With `listed_whole` false (a
// scan that could not list every file, ContentScan::incomplete) no base file is taken for deleted:
// one the list lacks may be one the walk could not tell, so only the files it read are changes.
std::vector<LocalChange> local_changes(const std::vector<project::ProjectFile> &base,
		const std::vector<project::ProjectFile> &local, const std::vector<std::string> &unread,
		bool listed_whole = true);

// One path of a pull.
enum class PullStep : uint8_t {
	// The revision's file put at the path: the revision added or changed it, the folder did not.
	Write,
	// The folder's file removed: the revision deleted it, the folder did not change it.
	Remove,
	// Both changed it otherwise: the author's to settle (resolve_conflict).
	Conflict,
};

struct PullItem {
	std::string path;
	PullStep step = PullStep::Write;
	// The revision's file (none when the revision has no file at the path).
	std::optional<project::ProjectFile> theirs;
	// The folder's file as the plan saw it (none when the folder had none): what the apply checks
	// is still there before it moves anything.
	std::optional<project::ProjectFile> mine;
	// What the folder and the revision each did to the base's file, for a conflict's row.
	std::optional<ContentChange> mine_change;
	std::optional<ContentChange> theirs_change;
	// The folder holds a file here whose bytes the scan could not read (ContentScan::unread):
	// `mine` is none, and the apply replaces the file as it finds it.
	bool unread = false;
};

struct PullPlan {
	std::vector<PullItem> items; // by path

	size_t conflicts() const;
	// The revision's files the writes put in place, each SHA-256 once: what a pull downloads.
	std::vector<project::ProjectFile> downloads() const;
	const PullItem *find(const std::string &path) const;
};

// The pull of `theirs` into a folder whose base is `base` and whose files are `local` (with
// `unread` held though unread: a change the revision made to one is a conflict, since the folder's
// own change to it cannot be told).
PullPlan plan_pull(const std::vector<project::ProjectFile> &base,
		const std::vector<project::ProjectFile> &local, const std::vector<std::string> &unread,
		const std::vector<project::ProjectFile> &theirs);

// Each of `paths` put back as `revision` has it, whatever the folder did to it: the revision's file
// written where the folder's differs (an unread one included), the folder's removed where the
// revision has none. No conflict: a restore is the author's own ask (Discard changes is a restore
// to the folder's base; an older revision's file, restored, is a change to push).
PullPlan plan_restore(const std::vector<project::ProjectFile> &local,
		const std::vector<std::string> &unread, const std::vector<project::ProjectFile> &revision,
		const std::vector<std::string> &paths);

enum class Resolution : uint8_t {
	// The folder's file stays as it is: a change to push over the revision's.
	KeepMine,
	// The revision's file put in its place (the folder's removed, when the revision has none).
	TakeTheirs,
};
// The conflict at `path` settled: false when the plan holds no conflict there.
bool resolve_conflict(PullPlan &plan, const std::string &path, Resolution resolution);

} // namespace opennova::editor
