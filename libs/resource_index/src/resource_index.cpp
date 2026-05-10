#include "resource_index/resource_index.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace opennova {

namespace {

std::string to_lower_ascii(std::string value) {
	for (char &ch : value) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return value;
}

std::string trim_copy(const std::string &value) {
	size_t begin = 0;
	while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
		++begin;
	}
	size_t end = value.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	return value.substr(begin, end - begin);
}

std::string kind_for_extension(const fs::path &path) {
	const std::string extension = to_lower_ascii(path.extension().string());
	if (extension == ".bms") {
		return "mission";
	}
	if (extension == ".trn") {
		return "terrain";
	}
	if (extension == ".env") {
		return "environment";
	}
	if (extension == ".3dp") {
		return "object_project";
	}
	if (extension == ".3di") {
		return "object_model";
	}
	if (extension == ".ase") {
		return "object_scene";
	}
	if (extension == ".kda") {
		return "credits";
	}
	return "";
}

std::string normalize_kind(const std::string &kind) {
	const std::string key = to_lower_ascii(trim_copy(kind));
	if (key.empty() || key == "*" || key == "all") {
		return "";
	}
	if (key == "bms") {
		return "mission";
	}
	if (key == "trn") {
		return "terrain";
	}
	if (key == "env") {
		return "environment";
	}
	if (key == "3dp" || key == "tdp" || key == "object_workspace") {
		return "object_project";
	}
	if (key == "3di") {
		return "object_model";
	}
	if (key == "ase" || key == "scene") {
		return "object_scene";
	}
	if (key == "kda") {
		return "credits";
	}
	return key;
}

bool is_object_kind(const std::string &kind) {
	return kind == "object_project" || kind == "object_model" || kind == "object_scene";
}

std::string display_name_from_path(const fs::path &path) {
	const std::string stem = path.stem().string();
	return stem.empty() ? path.filename().string() : stem;
}

std::string relative_path_string(const fs::path &path, const fs::path &root) {
	std::error_code ec;
	const fs::path relative = fs::relative(path, root, ec);
	if (!ec && !relative.empty()) {
		return relative.generic_string();
	}
	return path.filename().generic_string();
}

ResourceFileEntry entry_from_path(const fs::path &path, const fs::path &root) {
	ResourceFileEntry entry;
	entry.kind = kind_for_extension(path);
	entry.path = path.string();
	entry.display_name = display_name_from_path(path);
	entry.relative_path = relative_path_string(path, root);
	return entry;
}

} // namespace

struct ResourceIndex::Impl {
	std::string root_dir;
	std::string last_error;
	std::vector<ResourceFileEntry> files;
};

ResourceIndex::ResourceIndex() : impl_(std::make_unique<Impl>()) {}
ResourceIndex::~ResourceIndex() = default;
ResourceIndex::ResourceIndex(ResourceIndex &&) noexcept = default;
ResourceIndex &ResourceIndex::operator=(ResourceIndex &&) noexcept = default;

bool ResourceIndex::scan(const std::string &root_dir, bool recursive) {
	clear();
	const std::string clean = trim_copy(root_dir);
	if (clean.empty()) {
		impl_->last_error = "Resource directory is empty";
		return false;
	}

	std::error_code ec;
	const fs::path root(clean);
	if (!fs::is_directory(root, ec)) {
		impl_->last_error = "Resource directory does not exist: " + clean;
		return false;
	}

	impl_->root_dir = root.string();
	if (recursive) {
		for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
			if (ec) {
				break;
			}
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			const std::string kind = kind_for_extension(entry.path());
			if (!kind.empty()) {
				impl_->files.push_back(entry_from_path(entry.path(), root));
			}
		}
	} else {
		for (const fs::directory_entry &entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
			if (ec) {
				break;
			}
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			const std::string kind = kind_for_extension(entry.path());
			if (!kind.empty()) {
				impl_->files.push_back(entry_from_path(entry.path(), root));
			}
		}
	}

	std::sort(impl_->files.begin(), impl_->files.end(), [](const ResourceFileEntry &a, const ResourceFileEntry &b) {
		const std::string a_key = a.kind + ":" + to_lower_ascii(a.relative_path);
		const std::string b_key = b.kind + ":" + to_lower_ascii(b.relative_path);
		return a_key < b_key;
	});
	return true;
}

void ResourceIndex::clear() {
	impl_->root_dir.clear();
	impl_->last_error.clear();
	impl_->files.clear();
}

std::vector<ResourceFileEntry> ResourceIndex::resource_files(const std::string &kind) const {
	const std::string filter = normalize_kind(kind);
	if (filter.empty()) {
		return impl_->files;
	}
	std::vector<ResourceFileEntry> out;
	for (const ResourceFileEntry &entry : impl_->files) {
		if (entry.kind == filter || (filter == "object" && is_object_kind(entry.kind))) {
			out.push_back(entry);
		}
	}
	return out;
}

const std::string &ResourceIndex::root_dir() const {
	return impl_->root_dir;
}

const std::string &ResourceIndex::last_error() const {
	return impl_->last_error;
}

} // namespace opennova
