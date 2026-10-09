#include <editor/project/base_project.h>

#include <filesystem>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

bool fail(Diagnostic &error, std::string message) {
	error = make_finding(CoreFinding::ProjectBaseProject, DiagnosticSeverity::Error, std::move(message));
	return false;
}

} // namespace

std::string base_project_root(const std::string &project_root, const ProjectExpansion &expansion) {
	if (!expansion.on_base_project()) return std::string();
	fs::path base = path_of(expansion.base_project);
	if (base.is_relative()) base = path_of(project_root) / base;
	return io::without_trailing_separator(utf8_of(base.lexically_normal()));
}

bool base_project_game_dir(const std::string &project_root, const ProjectExpansion &expansion,
                           const std::string &target_game, std::string &out, Diagnostic &error) {
	out.clear();
	const std::string root = base_project_root(project_root, expansion);
	if (root.empty()) return fail(error, "The project names no base game's project.");
	ProjectDocument base;
	Diagnostic why;
	if (!open_project(root, base, why))
		return fail(error, "The base game's project " + expansion.base_project + " (" + root +
		                           ") does not open: " + why.message);
	out = ProjectPaths::for_root(root).export_dir(base);
	if (!base.expansion.standalone())
		return fail(error, "The base game's project " + expansion.base_project + " builds as the expansion " +
		                           base.expansion.name +
		                           ": an expansion plays over a base game, and the game mounts one expansion at a time.");
	if (!strutil::iequals(base.target_game, target_game))
		return fail(error, "The base game's project " + expansion.base_project + " is a project of \"" +
		                           base.target_game + "\", and this one of \"" + target_game + "\".");
	return true;
}

} // namespace opennova::editor
