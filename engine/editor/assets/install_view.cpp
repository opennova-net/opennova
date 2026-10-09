#include <editor/assets/install_view.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <base/gameprofile/player_files.h>
#include <editor/project/expansion_files.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The version text, which has one reader, the CRC a joiner must match: the project makes its own, so
// the view never offers the install's, wherever it lies (S16).
bool version_text(const std::string &name) {
	const ExpansionFileRow &row = expansion_file_row(ExpansionFileRole::Version);
	return strutil::iequals(name, expansion_file_name(row, std::string()));
}

// What the project calls the install's files the game reads by an expansion's name: under its own
// expansion's (`project`), from the installed one's (`installed`, "" the base game, whose music pairs the
// game reads in their place). Keyed by the install's name, normalized.
std::map<std::string, std::string> renames_for(const std::string &installed, const std::string &project) {
	std::map<std::string, std::string> out;
	if (project.empty()) return out;
	for (size_t i = 0; i < kExpansionFileRoleCount; ++i) {
		const ExpansionFileRow &row = expansion_file_row(static_cast<ExpansionFileRole>(i));
		if (row.fixed()) continue;
		const std::string from = installed.empty() ? (row.replaces() ? row.replaces() : "") : expansion_file_name(row, installed);
		if (from.empty()) continue;
		out[pff::normalized_logical_name(from)] = expansion_file_name(row, project);
	}
	return out;
}

// The base game's music pairs, which no stock launch under /exp reads.
bool base_music(const std::string &name) {
	for (size_t i = 0; i < kExpansionFileRoleCount; ++i) {
		const ExpansionFileRow &row = expansion_file_row(static_cast<ExpansionFileRole>(i));
		if (row.replaces() && strutil::iequals(row.replaces(), name)) return true;
	}
	return false;
}

} // namespace

InstallSpec install_spec(const std::string &root, const ProjectDocument &document) {
	InstallSpec spec = base_install_spec(root, document);
	spec.expansion = document.expansion.builds_on;
	spec.project_expansion = document.expansion.name;
	return spec;
}

InstallSpec base_install_spec(const std::string &root, const ProjectDocument &document) {
	InstallSpec spec;
	spec.root = root;
	spec.game = document.target_game;
	return spec;
}

bool InstallView::open(const InstallSpec &spec, std::string &error) {
	spec_ = spec;
	open_ = false;
	files_.clear();
	by_name_.clear();
	vfs_.clear();
	LaunchFlags stock; // no /d: a stock launch of the project's game, with /exp when it names one
	stock.game = spec.game;
	stock.expansion = spec.expansion;
	if (spec.root.empty() || !mount_install(vfs_, spec.root, stock)) {
		error = "No game archives found under " + spec.root + ".";
		return false;
	}
	if (!strutil::iequals(vfs_.mounted_expansion(), spec.expansion)) {
		error = "The game install has no expansion '" + spec.expansion + "' (no expansion\\" + spec.expansion + "\\" +
		        spec.expansion + ".pff under " + spec.root + ").";
		vfs_.clear();
		return false;
	}
	const std::string expansion_dir =
			spec.expansion.empty() ? std::string() : join_path(join_path(spec.root, "expansion"), spec.expansion);
	const std::map<std::string, std::string> renames = renames_for(spec.expansion, spec.project_expansion);
	std::set<std::string> targets; // the names the renamed files take, which no other file of the view has
	for (const auto &entry : renames) targets.insert(pff::normalized_logical_name(entry.second));
	const auto add = [&](InstallFile file) {
		if (version_text(file.member)) return;
		const auto renamed = renames.find(pff::normalized_logical_name(file.member));
		if (renamed != renames.end()) file.name = renamed->second;
		else if (targets.count(pff::normalized_logical_name(file.name))) return;
		if (!spec.expansion.empty() && base_music(file.member)) return;
		if (!by_name_.emplace(pff::normalized_logical_name(file.name), files_.size()).second) return;
		files_.push_back(std::move(file));
	};
	// The files the game reads from the expansion's folder first: each the copy `/exp` serves, so it
	// stands over an archive's or the root's of the same name (the first added keeps the name).
	if (!expansion_dir.empty()) {
		std::error_code ec;
		std::vector<std::string> folder;
		for (const fs::directory_entry &entry : fs::directory_iterator(system_path(expansion_dir), ec)) {
			std::error_code kind;
			const std::string name = utf8_of(entry.path().filename());
			if (entry.is_regular_file(kind) && vfs_read_from_expansion_folder(name, spec.expansion)) folder.push_back(name);
		}
		std::sort(folder.begin(), folder.end(), [](const std::string &a, const std::string &b) {
			return pff::normalized_logical_name(a) < pff::normalized_logical_name(b);
		});
		for (const std::string &loose : folder)
			add({ loose, loose, InstallFile::Layer::Expansion, join_path(expansion_dir, loose) });
	}
	// The archives' files as the mount resolves them (the archives themselves left out, never the
	// player's own files), each from the expansion's two archives or the base's.
	const auto expansion_archive = [&spec](const std::string &path) {
		const std::string archive = utf8_of(path_of(path).filename());
		return !spec.expansion.empty() &&
		       (strutil::iequals(archive, spec.expansion + ".pff") || strutil::iequals(archive, spec.expansion + "L.pff"));
	};
	for (const VfsFileLocation &location : vfs_.list_files()) {
		if (strutil::ends_with_icase(location.logical_name, ".pff") || gameprofile::is_player_file(location.logical_name)) continue;
		InstallFile file;
		file.name = location.logical_name;
		file.member = location.logical_name;
		file.layer = expansion_archive(location.source_path) ? InstallFile::Layer::Expansion : InstallFile::Layer::Base;
		add(std::move(file));
	}
	// The loose files the game reads by path from the install's folder.
	for (const std::string &loose : list_install_loose_files(spec.root))
		add({ loose, loose, InstallFile::Layer::Base, join_path(spec.root, loose) });
	open_ = true;
	return true;
}

const InstallFile *InstallView::find(const std::string &project_name) const {
	const auto found = by_name_.find(pff::normalized_logical_name(project_name));
	return found == by_name_.end() ? nullptr : &files_[found->second];
}

bool InstallView::read(const InstallFile &file, std::vector<uint8_t> &out) const {
	if (!open_) return false;
	if (file.loose_path.empty()) return read_served(vfs_, file.member, out);
	std::string error;
	return io::read_file_bytes(file.loose_path, out, error);
}

uint64_t InstallView::size(const InstallFile &file) const {
	if (!open_) return 0;
	if (file.loose_path.empty()) {
		uint64_t stored = 0;
		return vfs_.file_size(file.member, stored) ? stored : 0;
	}
	std::error_code ec;
	const auto on_disk = fs::file_size(system_path(file.loose_path), ec);
	return ec ? 0 : static_cast<uint64_t>(on_disk);
}

ImportChoice install_choice(const std::string &root, const InstallFile &file) {
	ImportChoice choice;
	choice.path = root;
	choice.entry = file.member;
	choice.install = true;
	if (file.name != file.member) choice.as = file.name;
	return choice;
}

} // namespace opennova::editor
