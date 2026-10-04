#include <editor/assets/install_check.h>

#include <filesystem>
#include <system_error>

#include <base/gameprofile/gameprofile.h>
#include <base/vfs/vfs.h>
#include <editor/assets/install_view.h>
#include <editor/model/field_text.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/run/launch_plan.h>

namespace fs = std::filesystem;

namespace opennova::editor {

std::string InstallCheck::words() const {
	if (root.empty()) return "No game install chosen: the project can still take files from the disk.";
	if (!exists) return "There is no folder at " + root + ".";
	if (!mounts) return "No game here: the folder holds none of the game's archives.";
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
	// The base game's view, as an import of a standalone project lists it: what every import reads.
	InstallSpec spec;
	spec.root = out.root;
	spec.game = out.game;
	InstallView view;
	if (!view.open(spec, out.why)) return out;
	out.mounts = true;
	out.files = view.files().size();
	out.executable = fs::is_regular_file(system_path(join_path(out.root, kInstallExecutable)), ec);
	for (const std::string &name : vfs_list_expansions(out.root))
		out.expansions.push_back({ name, vfs_expansion_info(out.root, name).name });
	return out;
}

} // namespace opennova::editor
