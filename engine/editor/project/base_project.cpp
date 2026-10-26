#include <editor/project/base_project.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/model/core_finding.h>
#include <editor/project/project_files.h>
#include <editor/project_build/export_build.h>

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
                           const std::string &target_game, std::string &out, Diagnostic &error,
                           ProjectDocument *document) {
	out.clear();
	const std::string root = base_project_root(project_root, expansion);
	if (root.empty()) return fail(error, "The project names no base game's project.");
	ProjectDocument base;
	Diagnostic why;
	if (!open_project(root, base, why))
		return fail(error, "The base game's project " + expansion.base_project + " (" + root +
		                           ") does not open: " + why.message);
	out = ProjectPaths::for_root(root).export_dir(base);
	if (document) *document = base;
	if (!base.expansion.standalone())
		return fail(error, "The base game's project " + expansion.base_project + " builds as the expansion " +
		                           base.expansion.name +
		                           ": an expansion plays over a base game, and the game mounts one expansion at a time.");
	if (!strutil::iequals(base.target_game, target_game))
		return fail(error, "The base game's project " + expansion.base_project + " is a project of \"" +
		                           base.target_game + "\", and this one of \"" + target_game + "\".");
	return true;
}

std::string base_project_from(const std::string &project_root, const std::string &folder) {
	const fs::path path = path_of(folder);
	if (!path.is_absolute()) return folder;
	const fs::path root = path_of(io::without_trailing_separator(project_root)).lexically_normal();
	const fs::path relative = path.lexically_normal().lexically_relative(root);
	return relative.empty() ? folder : io::without_trailing_separator(utf8_of(relative));
}

std::string sibling_base_project(const std::string &new_root, const std::string &target_game) {
	if (new_root.empty()) return std::string();
	const fs::path root = path_of(io::without_trailing_separator(new_root)).lexically_normal();
	const fs::path parent = root.parent_path();
	if (parent.empty() || root.filename().empty()) return std::string();
	std::vector<std::string> names; // the folders beside it, by name
	std::error_code ec;
	for (fs::directory_iterator it(system_path(utf8_of(parent)), ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code is;
		if (!it->is_directory(is)) continue;
		const std::string name = utf8_of(it->path().filename());
		if (name != utf8_of(root.filename()) && !name.empty() && name.front() != '.') names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	for (const std::string &name : names) {
		ProjectDocument base;
		Diagnostic why;
		if (open_project(utf8_of(parent / path_of(name)), base, why) && base.expansion.standalone() &&
		    strutil::iequals(base.target_game, target_game))
			return "../" + name;
	}
	return std::string();
}

BaseExport base_export_of(const std::string &project_root, const ProjectExpansion &expansion,
                          const std::string &target_game) {
	BaseExport out;
	out.root = base_project_root(project_root, expansion);
	ProjectDocument base;
	if (!base_project_game_dir(project_root, expansion, target_game, out.export_dir, out.refusal, &base)) return out;
	out.replaceable = export_replaces(out.export_dir, base.project_id);
	std::string sources;
	out.current = export_sources_of(out.export_dir, base.project_id, sources) && !sources.empty() &&
	              sources == project_files_stamp(out.root, out.export_dir);
	return out;
}

} // namespace opennova::editor
