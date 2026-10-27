#include <editor/novaworld/novaworld_apply.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The SHA-256 of the file at `path` (a system path), "" when it cannot be read.
std::string sha256_of(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	io::Sha256 hash;
	std::vector<char> block(1 << 20);
	while (in) {
		in.read(block.data(), static_cast<std::streamsize>(block.size()));
		if (in.gcount() > 0)
			hash.update(block.data(), static_cast<size_t>(in.gcount()));
	}
	if (in.bad())
		return {};
	return io::sha256_hex(hash.finish());
}

// Whether the folder still holds at `item.path` what the plan saw there.
bool unchanged_since_plan(const fs::path &root, const PullItem &item) {
	if (item.unread)
		return true; // the author chose the revision's over a file the scan could not read
	std::error_code ec;
	const fs::path target = root / path_of(item.path).make_preferred();
	const bool there = fs::is_regular_file(target, ec);
	if (!item.mine)
		return !there;
	return there && fs::file_size(target, ec) == item.mine->size &&
			sha256_of(target) == item.mine->sha256;
}

// The folders above a removed file that it left empty, up to (not including) the root, removed.
void remove_empty_folders(const fs::path &root, const fs::path &file) {
	std::error_code ec;
	for (fs::path dir = file.parent_path(); !dir.empty() && dir != root; dir = dir.parent_path()) {
		if (!fs::is_directory(dir, ec) ||
				fs::directory_iterator(dir, ec) != fs::directory_iterator())
			return;
		if (!fs::remove(dir, ec))
			return;
	}
}

} // namespace

std::string incoming_path(const NovaWorldPaths &paths, const std::string &sha256) {
	return join_path(paths.incoming_dir, sha256);
}

bool apply_pull(const ProjectPaths &paths, const PullPlan &plan,
		const std::vector<project::ProjectFile> &theirs, const NovaWorldBinding &binding,
		ApplyResult &result, std::string &error) {
	result = ApplyResult();
	const NovaWorldPaths novaworld_paths = NovaWorldPaths::for_project(paths);
	const fs::path root = system_path(paths.root);
	if (const size_t conflicts = plan.conflicts()) {
		error = std::to_string(conflicts) +
				" conflict(s) are not settled: keep yours or take theirs first";
		return false;
	}
	std::error_code ec;
	for (const project::ProjectFile &file : plan.downloads()) {
		const fs::path landed = system_path(incoming_path(novaworld_paths, file.sha256));
		if (!fs::is_regular_file(landed, ec) || fs::file_size(landed, ec) != file.size) {
			error = "the download of " + file.path + " is missing";
			return false;
		}
	}
	// A write whose path is a removal's with only its letters' case changed finds the removal's
	// file at its path on Windows and macOS (one file there): the removal's check is that file's.
	std::set<std::string> removed;
	for (const PullItem &item : plan.items)
		if (item.step == PullStep::Remove)
			removed.insert(project::folded_path(item.path));
	for (const PullItem &item : plan.items) {
		const bool recased = item.step == PullStep::Write && !item.mine &&
				removed.count(project::folded_path(item.path)) != 0;
		if (!recased && !unchanged_since_plan(root, item))
			result.changed_since.push_back(item.path);
	}
	if (!result.changed_since.empty()) {
		error = std::to_string(result.changed_since.size()) +
				" file(s) changed since the pull was planned: plan again";
		return false;
	}

	// Every file the pull writes copied from its download first (one download may be several paths'
	// file), so a copy that fails moves nothing and only renames follow the trash. Staged beside
	// the downloads, on the folder's volume as the trash is: a write's folder may stand where a
	// file the pull removes is until that file goes.
	const fs::path staged_dir = system_path(join_path(novaworld_paths.incoming_dir, "staged"));
	std::vector<fs::path> staged(plan.items.size());
	for (size_t i = 0; i < plan.items.size(); ++i) {
		const PullItem &item = plan.items[i];
		if (item.step != PullStep::Write)
			continue;
		staged[i] = staged_dir / std::to_string(i);
		fs::create_directories(staged_dir, ec);
		fs::copy_file(system_path(incoming_path(novaworld_paths, item.theirs->sha256)), staged[i],
				fs::copy_options::overwrite_existing, ec);
		if (ec) {
			error = "cannot write " + item.path + ": " + ec.message();
			fs::remove_all(staged_dir, ec);
			return false;
		}
	}

	// What the folder holds at a path the pull writes or removes goes to the trash, in one batch: a
	// file once, though a write's path and a removal's name it (a change of its letters' case).
	std::vector<std::string> trashed;
	std::map<std::string, fs::path> trashed_at; // by folded path
	for (const PullItem &item : plan.items) {
		const fs::path target = root / path_of(item.path).make_preferred();
		if (!fs::is_regular_file(target, ec))
			continue;
		const auto [at, added] = trashed_at.emplace(project::folded_path(item.path), target);
		if (!added && fs::equivalent(at->second, target, ec))
			continue;
		trashed.push_back(item.path);
	}
	if (!trashed.empty() && !trash_paths(paths, trashed, result.trash, error)) {
		fs::remove_all(staged_dir, ec);
		return false;
	}

	// The removals first: a folder one empties goes before a write puts a file in its place (a
	// folder the revision replaced by a file), or makes it again in a write's new spelling.
	for (const PullItem &item : plan.items) {
		if (item.step != PullStep::Remove)
			continue;
		remove_empty_folders(root, root / path_of(item.path).make_preferred());
		++result.removed;
	}

	// Then each staged file renamed into place. One that does not go brings the files it and the
	// writes after it would replace back out of the trash; the ones written before it stay (the
	// base is the old one, so a pull again finishes).
	const auto put_back = [&](size_t first) {
		fs::remove_all(staged_dir, ec);
		const std::set<std::string> in_trash(result.trash.paths.begin(), result.trash.paths.end());
		TrashBatch back;
		back.id = result.trash.id;
		for (size_t i = first; i < plan.items.size(); ++i)
			if (plan.items[i].step == PullStep::Write && in_trash.count(plan.items[i].path))
				back.paths.push_back(plan.items[i].path);
		if (back.paths.empty())
			return;
		std::string why;
		if (!restore_trash(paths, back, why)) {
			error += "; the files it would replace stay in the project's trash: " + why;
			return;
		}
		const std::set<std::string> restored(back.paths.begin(), back.paths.end());
		result.trash.paths.erase(
				std::remove_if(result.trash.paths.begin(), result.trash.paths.end(),
						[&](const std::string &path) { return restored.count(path) != 0; }),
				result.trash.paths.end());
		if (result.trash.paths.empty())
			result.trash.id = 0;
	};
	for (size_t i = 0; i < plan.items.size(); ++i) {
		const PullItem &item = plan.items[i];
		if (item.step != PullStep::Write)
			continue;
		const fs::path target = root / path_of(item.path).make_preferred();
		bool placed = false;
		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			error = "cannot make the folder of " + item.path + ": " + ec.message();
		} else {
			// An empty folder in the file's place goes (the rename says why when one is not empty).
			if (fs::is_directory(target, ec))
				fs::remove(target, ec);
			placed = io::replace_file(io::utf8_path(staged[i]), io::utf8_path(target), error);
		}
		if (!placed) {
			put_back(i);
			return false;
		}
		++result.written;
	}

	if (!write_novaworld_base(novaworld_paths, theirs, error) ||
			!write_novaworld_binding(novaworld_paths, binding, error))
		return false;
	fs::remove_all(system_path(novaworld_paths.incoming_dir), ec); // a download kept is only space
	return true;
}

} // namespace opennova::editor
