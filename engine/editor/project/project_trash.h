#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/project/project_document.h>

namespace opennova::editor {

// The project's trash (DI-25, the deep-integration plan): where a file the editor deletes goes, so a delete
// is a step the file history takes back (session/file_chores.h). A numbered folder under the cache's trash
// (ProjectPaths::trash_dir, `.opennova/trash/<n>/`) per batch of paths put there, each at its project-relative
// path beneath it. The project's walk never enters the cache (is_dot_directory), so nothing in the trash is a
// file of the project; the editor never empties it (a person removes `.opennova/trash/` for the room), so no
// delete is ever a permanent one.
struct TrashBatch {
	uint64_t id = 0;                // its folder's number under the trash (0: none)
	std::vector<std::string> paths; // project-relative, as they were: files, or a folder under the cache
};

// The paths `relative` (project-relative: files of the project, an import record, a source's outputs' folder
// under the cache) moved into a new batch, all or none: false with `error` (the path and the system's reason)
// when one does not go, the ones moved before it put back. A path with nothing there is refused, nothing moved.
bool trash_paths(const ProjectPaths &paths, const std::vector<std::string> &relative, TrashBatch &batch,
                 std::string &error);
// A batch's paths put back where they were, all or none: refused, nothing moved, when something sits at one of
// those places or the batch lacks one (`error` names it); the batch's folder removed once it is empty.
bool restore_trash(const ProjectPaths &paths, const TrashBatch &batch, std::string &error);
// Where a batch's paths are: its folder under the trash.
std::string trash_batch_dir(const ProjectPaths &paths, uint64_t id);

} // namespace opennova::editor
