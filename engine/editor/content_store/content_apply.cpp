#include <editor/content_store/content_apply.h>

#include <filesystem>
#include <fstream>
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

std::string incoming_path(const ContentPaths &paths, const std::string &sha256) {
	return join_path(paths.incoming_dir, sha256);
}

bool apply_pull(const ProjectPaths &paths, const PullPlan &plan,
		const std::vector<content::ContentFile> &theirs, const ContentBinding &binding,
		ApplyResult &result, std::string &error) {
	result = ApplyResult();
	const ContentPaths content_paths = ContentPaths::for_project(paths);
	const fs::path root = system_path(paths.root);
	if (const size_t conflicts = plan.conflicts()) {
		error = std::to_string(conflicts) +
				" conflict(s) are not settled: keep yours or take theirs first";
		return false;
	}
	std::error_code ec;
	for (const content::ContentFile &file : plan.downloads()) {
		const fs::path landed = system_path(incoming_path(content_paths, file.sha256));
		if (!fs::is_regular_file(landed, ec) || fs::file_size(landed, ec) != file.size) {
			error = "the download of " + file.path + " is missing";
			return false;
		}
	}
	for (const PullItem &item : plan.items)
		if (!unchanged_since_plan(root, item))
			result.changed_since.push_back(item.path);
	if (!result.changed_since.empty()) {
		error = std::to_string(result.changed_since.size()) +
				" file(s) changed since the pull was planned: plan again";
		return false;
	}

	// What the folder holds at a path the pull writes or removes goes to the trash first, in one
	// batch.
	std::vector<std::string> trashed;
	for (const PullItem &item : plan.items)
		if (fs::is_regular_file(root / path_of(item.path).make_preferred(), ec))
			trashed.push_back(item.path);
	if (!trashed.empty() && !trash_paths(paths, trashed, result.trash, error))
		return false;

	for (const PullItem &item : plan.items) {
		const fs::path target = root / path_of(item.path).make_preferred();
		if (item.step == PullStep::Remove) {
			remove_empty_folders(root, target);
			++result.removed;
			continue;
		}
		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			error = "cannot make the folder of " + item.path + ": " + ec.message();
			return false;
		}
		// Copied (one download may be several paths' file) beside the target, then renamed over it.
		const fs::path staged = fs::path(target).concat(".tmp");
		fs::copy_file(system_path(incoming_path(content_paths, item.theirs->sha256)), staged,
				fs::copy_options::overwrite_existing, ec);
		if (ec) {
			error = "cannot write " + item.path + ": " + ec.message();
			return false;
		}
		if (!io::replace_file(io::utf8_path(staged), io::utf8_path(target), error))
			return false;
		++result.written;
	}

	if (!write_content_base(content_paths, theirs, error) ||
			!write_content_binding(content_paths, binding, error))
		return false;
	fs::remove_all(system_path(content_paths.incoming_dir), ec); // a download kept is only space
	return true;
}

} // namespace opennova::editor
