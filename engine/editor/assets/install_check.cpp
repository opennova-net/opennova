#include <editor/assets/install_check.h>

#include <filesystem>
#include <set>
#include <system_error>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/assets/install_view.h>
#include <editor/model/field_text.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/run/launch_plan.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// "a", "a or b", "a, b or c" (`last` the word before the last).
std::string either(const std::vector<std::string> &names, const char *last = " or ") {
	std::string out;
	for (size_t i = 0; i < names.size(); ++i) {
		if (i > 0) out += i + 1 == names.size() ? last : ", ";
		out += names[i];
	}
	return out;
}

} // namespace

std::string InstallCheck::words() const {
	if (root.empty()) return "No game install chosen: the project can still take files from the disk.";
	if (!exists) return "There is no folder at " + root + ".";
	if (build) return "This is a build the editor made of a project, not the game install.";
	if (!mounts)
		return archives_present ? "The game's archives here do not open: " + why
		                        : std::string("No game here: the folder holds none of the game's archives.");
	if (!missing_archives.empty())
		return "Not the whole game: no " + either(missing_archives) + " here (a language pack, or an install in part).";
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(game.c_str());
	std::string out = std::string(profile ? profile->display_name : "The game") + ": " + counted(files, "file");
	for (size_t i = 0; i < expansions.size(); ++i) {
		const Expansion &each = expansions[i];
		out += i == 0 ? (expansions.size() == 1 ? ", the expansion " : ", the expansions ")
		       : i + 1 == expansions.size() ? " and " : ", ";
		out += each.title.empty() || each.title == each.name ? each.name : each.title + " (" + each.name + ")";
	}
	out += ".";
	if (!executable)
		out += std::string(" No ") + kInstallExecutable + " beside them: Play in the game install needs it.";
	return out;
}

InstallCheck check_install(const std::string &root, const std::string &game) {
	InstallCheck out;
	out.root = absolute_install_path(root);
	out.game = game.empty() ? std::string(kDefaultTargetGame) : game;
	if (out.root.empty()) return out;
	std::error_code ec;
	out.exists = fs::is_directory(system_path(out.root), ec);
	if (!out.exists) return out;
	// The folder's own files by name, any case: the boot table's archives, a build's record.
	std::set<std::string> names;
	for (fs::directory_iterator it(system_path(out.root), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
		std::error_code status;
		if (it->is_regular_file(status)) names.insert(strutil::to_lower(utf8_of(it->path().filename())));
	}
	std::vector<std::string> present;
	for (const char *archive : kBootArchiveTable) {
		if (names.count(archive)) present.push_back(archive);
		else out.missing_archives.push_back(archive);
	}
	out.archives_present = !present.empty();
	// A build the editor made: its record beside its archives, or anywhere inside a project's cache.
	out.build = names.count(strutil::to_lower(kBuildRecordFileName)) > 0;
	for (const fs::path &part : path_of(out.root))
		if (utf8_of(part) == ".opennova") out.build = true;
	// The base game's view, as an import of a standalone project lists it: what every import reads.
	InstallSpec spec;
	spec.root = out.root;
	spec.game = out.game;
	InstallView view;
	if (!view.open(spec, out.why)) {
		// Archives there that do not open (one held by another program, an update stopped half way).
		if (out.archives_present) out.why = either(present, " and ") + (present.size() == 1 ? " is there" : " are there") +
		                                    " but none opens as the game's archive.";
		return out;
	}
	out.mounts = true;
	out.files = view.files().size();
	out.executable = fs::is_regular_file(system_path(join_path(out.root, kInstallExecutable)), ec);
	for (const std::string &name : vfs_list_expansions(out.root))
		out.expansions.push_back({ name, vfs_expansion_info(out.root, name).name });
	return out;
}

} // namespace opennova::editor
