#include <editor/assets/project_layout.h>

#include <map>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>

namespace opennova::editor {

namespace {

// A project-relative path's folder ("" for one at the top level).
std::string folder_of(const std::string &relative) {
	const size_t slash = relative.rfind('/');
	return slash == std::string::npos ? std::string() : relative.substr(0, slash);
}

// The kind a file of the project is placed as: an import source as the kind its name gives (a PNG
// sits with the textures it makes).
AssetKind placed_kind(const AssetEntry &entry) {
	return entry.kind == AssetKind::ImportSource ? file_kind_for_name(entry.logical_name) : entry.kind;
}

} // namespace

ProjectLayout project_layout(const AssetScan &scan) {
	size_t top = 0, by_kind = 0;
	for (const AssetEntry &entry : scan.entries) {
		if (!entry.imported_from.empty()) continue; // an output, under the cache
		const std::string kind_folder = asset_kind_row(placed_kind(entry)).folder;
		if (kind_folder.empty()) continue; // a kind kept at the top level either way
		const std::string folder = folder_of(entry.relative_path);
		if (folder.empty()) ++top;
		else if (strutil::iequals(folder, kind_folder)) ++by_kind;
	}
	return top > by_kind ? ProjectLayout::Flat : ProjectLayout::ByKind;
}

const char *project_layout_token(ProjectLayout layout) {
	return layout == ProjectLayout::Flat ? "flat" : "by_kind";
}

std::string placement_folder(const AssetScan &scan, AssetKind kind) {
	const std::string kind_folder = asset_kind_row(kind).folder;
	// The folders holding the project's files of the kind, each by its name without case (as the
	// file system compares them), spelled as the first file found there spells it.
	struct Held {
		std::string spelled;
		size_t files = 0;
	};
	std::map<std::string, Held> held;
	for (const AssetEntry &entry : scan.entries) {
		if (!entry.imported_from.empty() || placed_kind(entry) != kind) continue;
		const std::string folder = folder_of(entry.relative_path);
		Held &at = held[strutil::to_lower(folder)];
		if (at.files++ == 0) at.spelled = folder;
	}
	if (held.empty()) return project_layout(scan) == ProjectLayout::Flat ? std::string() : kind_folder;
	// The folder holding the most; of two holding as many, the kind's own, then the top level, then the
	// first by its path (the map's order).
	const auto rank = [&kind_folder](const std::string &key) {
		return strutil::iequals(key, kind_folder) ? 0 : key.empty() ? 1 : 2;
	};
	const std::pair<const std::string, Held> *best = nullptr;
	for (const auto &each : held) {
		if (!best || each.second.files > best->second.files ||
		    (each.second.files == best->second.files && rank(each.first) < rank(best->first)))
			best = &each;
	}
	return best->second.spelled;
}

std::string placement_path(const AssetScan &scan, const std::string &name, AssetKind kind) {
	const AssetKind placed = kind == AssetKind::ImportSource ? file_kind_for_name(name) : kind;
	const std::string folder = placement_folder(scan, placed);
	return folder.empty() ? name : folder + "/" + name;
}

bool normalize_project_folder(const std::string &text, std::string &out, std::string &why) {
	std::string path = text;
	for (char &c : path)
		if (c == '\\') c = '/';
	if ((path.size() >= 2 && path[1] == ':') || path.rfind("//", 0) == 0) {
		why = "'" + text + "' is a place on the disk: name a folder of the project, from its top level.";
		return false;
	}
	std::vector<std::string> steps;
	size_t start = 0;
	while (start <= path.size()) {
		size_t end = path.find('/', start);
		if (end == std::string::npos) end = path.size();
		const std::string step = path.substr(start, end - start);
		start = end + 1;
		if (step.empty() || step == ".") continue;
		if (step == "..") {
			why = "'" + text + "' leaves the project: name a folder inside it.";
			return false;
		}
		if (step[0] == '.') {
			why = "'" + step + "' is a folder the project's files never sit in (its name starts with a dot, as the "
			                   "editor's cache does): a file moved there would no longer be one of the project's.";
			return false;
		}
		if (step.find_first_of(":*?\"<>|") != std::string::npos) {
			why = "'" + step + "' is no folder name the file system takes.";
			return false;
		}
		steps.push_back(step);
	}
	out.clear();
	for (const std::string &step : steps) out += (out.empty() ? "" : "/") + step;
	return true;
}

} // namespace opennova::editor
