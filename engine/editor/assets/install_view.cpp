#include <editor/assets/install_view.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/player_files.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

InstallSpec install_spec(const std::string &root, const ProjectDocument &document) {
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
	LaunchFlags stock; // no /d, no expansion: a stock launch of the project's game
	stock.game = spec.game;
	if (spec.root.empty() || !mount_install(vfs_, spec.root, stock)) {
		error = "No game archives found under " + spec.root + ".";
		return false;
	}
	const auto add = [this](InstallFile file) {
		if (!by_name_.emplace(normalized_logical_name(file.name), files_.size()).second) return;
		files_.push_back(std::move(file));
	};
	// The archives' files as the mount resolves them (the archives themselves left out, never the
	// player's own files), then the root's loose files of the kinds the game reads by path, where no
	// archive has the name.
	for (const VfsFileLocation &location : vfs_.list_files()) {
		if (strutil::ends_with_icase(location.logical_name, ".pff") || is_player_file(location.logical_name)) continue;
		add({ location.logical_name, location.logical_name, std::string() });
	}
	for (const std::string &loose : list_install_loose_files(spec.root))
		add({ loose, loose, join_path(spec.root, loose) });
	open_ = true;
	return true;
}

const InstallFile *InstallView::find(const std::string &project_name) const {
	const auto found = by_name_.find(normalized_logical_name(project_name));
	return found == by_name_.end() ? nullptr : &files_[found->second];
}

bool InstallView::read(const InstallFile &file, std::vector<uint8_t> &out) const {
	if (!open_) return false;
	if (file.loose_path.empty()) return read_served(vfs_, file.member, out);
	std::string error;
	return read_file_bytes(file.loose_path, out, error);
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
	return choice;
}

} // namespace opennova::editor
