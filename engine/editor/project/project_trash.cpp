#include <editor/project/project_trash.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <base/io/strutil.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace fs = std::filesystem;

namespace {

fs::path on_disk(const std::string &root, const std::string &relative) {
	return system_path(join_path(root, relative));
}

// The next batch's number: one past the largest a folder of the trash holds.
uint64_t next_batch(const ProjectPaths &paths) {
	uint64_t last = 0;
	std::error_code ec;
	for (fs::directory_iterator it(system_path(paths.trash_dir), ec), end; !ec && it != end; it.increment(ec)) {
		const std::string name = utf8_of(it->path().filename());
		if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
		if (const auto number = strutil::parse_ullong(name)) last = std::max<uint64_t>(last, *number);
	}
	return last + 1;
}

// The folders from `path`'s up to `top` (`top` included) removed while each is empty; never one outside `top`.
void remove_empty_up(const fs::path &path, const fs::path &top) {
	std::error_code ec;
	const fs::path::string_type &held = top.native();
	for (fs::path dir = path.parent_path(); !dir.empty() && dir.native().compare(0, held.size(), held) == 0;
	     dir = dir.parent_path()) {
		if (!fs::is_empty(dir, ec) || ec || !fs::remove(dir, ec)) return;
		if (dir == top) return;
	}
}

} // namespace

std::string trash_batch_dir(const ProjectPaths &paths, uint64_t id) {
	return join_path(paths.trash_dir, std::to_string(id));
}

bool trash_paths(const ProjectPaths &paths, const std::vector<std::string> &relative, TrashBatch &batch,
                 std::string &error) {
	batch = TrashBatch();
	std::error_code ec;
	for (const std::string &path : relative)
		if (!fs::exists(on_disk(paths.root, path), ec)) {
			error = "There is nothing at " + path + " to put in the trash.";
			return false;
		}
	if (!ensure_project_cache_dir(paths, error)) return false;
	batch.id = next_batch(paths);
	const std::string dir = trash_batch_dir(paths, batch.id);
	// Each path moved in turn; one that does not go puts the ones before it back.
	for (const std::string &path : relative) {
		const fs::path to = on_disk(dir, path);
		if (ensure_directory(utf8_of(to.parent_path()), error) && rename_with_retry(on_disk(paths.root, path), to, ec)) {
			batch.paths.push_back(path);
			continue;
		}
		if (error.empty()) error = path + " could not go to the trash: " + ec.message();
		else error = path + " could not go to the trash: " + error;
		for (const std::string &moved : batch.paths) {
			std::error_code back;
			if (rename_with_retry(on_disk(dir, moved), on_disk(paths.root, moved), back))
				remove_empty_up(on_disk(dir, moved), system_path(dir));
		}
		batch = TrashBatch();
		return false;
	}
	return true;
}

bool restore_trash(const ProjectPaths &paths, const TrashBatch &batch, std::string &error) {
	const std::string dir = trash_batch_dir(paths, batch.id);
	std::error_code ec;
	for (const std::string &path : batch.paths) {
		if (fs::exists(on_disk(paths.root, path), ec)) {
			error = "Something sits at " + path + " now: move or rename it first.";
			return false;
		}
		if (!fs::exists(on_disk(dir, path), ec)) {
			error = "The trash no longer holds " + path + ".";
			return false;
		}
	}
	std::vector<std::string> restored;
	for (const std::string &path : batch.paths) {
		const fs::path to = on_disk(paths.root, path);
		if (ensure_directory(utf8_of(to.parent_path()), error) && rename_with_retry(on_disk(dir, path), to, ec)) {
			restored.push_back(path);
			continue;
		}
		if (error.empty()) error = path + " could not come back from the trash: " + ec.message();
		else error = path + " could not come back from the trash: " + error;
		for (const std::string &back : restored) {
			std::error_code ignored;
			rename_with_retry(on_disk(paths.root, back), on_disk(dir, back), ignored);
		}
		return false;
	}
	// The batch's folders, emptied: only folders are left under it, each removed while empty.
	const fs::path top = system_path(dir);
	for (const std::string &path : batch.paths) remove_empty_up(on_disk(dir, path), top);
	return true;
}

} // namespace opennova::editor
