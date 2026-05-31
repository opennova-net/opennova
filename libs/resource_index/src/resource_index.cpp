#include "resource_index/resource_index.h"

#include <pff/pff.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>

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

bool has_rtxt_magic(const fs::path &path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	char magic[4] = {};
	file.read(magic, sizeof(magic));
	return file.gcount() == sizeof(magic) &&
	       magic[0] == 'R' &&
	       magic[1] == 'T' &&
	       magic[2] == 'X' &&
	       magic[3] == 'T';
}

bool has_rtxt_magic(const std::vector<uint8_t> &bytes) {
	return bytes.size() >= 4 &&
	       bytes[0] == 'R' &&
	       bytes[1] == 'T' &&
	       bytes[2] == 'X' &&
	       bytes[3] == 'T';
}

std::string extension_for_name(const std::string &name) {
	return to_lower_ascii(fs::path(name).extension().string());
}

std::string kind_for_name_and_rtxt(const std::string &name, bool is_rtxt_bin) {
	const std::string extension = extension_for_name(name);
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
	if (extension == ".fnt") {
		return "font";
	}
	if (extension == ".bin" && is_rtxt_bin) {
		return "strings";
	}
	return "";
}

std::string kind_for_path(const fs::path &path) {
	return kind_for_name_and_rtxt(path.filename().string(), has_rtxt_magic(path));
}

std::string kind_for_bytes(const std::string &name, const std::vector<uint8_t> &bytes) {
	return kind_for_name_and_rtxt(name, has_rtxt_magic(bytes));
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
	if (key == "fnt" || key == "fonts") {
		return "font";
	}
	if (key == "bin" || key == "rtxt") {
		return "strings";
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

std::string display_name_from_name(const std::string &name) {
	const fs::path path(name);
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
	entry.kind = kind_for_path(path);
	entry.path = path.string();
	entry.logical_name = path.filename().string();
	entry.display_name = display_name_from_path(path);
	entry.relative_path = relative_path_string(path, root);
	entry.source_type = "file";
	return entry;
}

std::string flat_lookup_key(const std::string &name) {
	const std::string clean = trim_copy(name);
	if (clean.empty()) {
		return "";
	}
	return to_lower_ascii(fs::path(clean).filename().string());
}

std::string pff_entry_name(const PffEntry &entry) {
	size_t len = 0;
	while (len < PFF_NAME_SIZE && entry.filename[len] != '\0') {
		++len;
	}
	while (len > 0 && entry.filename[len - 1] == ' ') {
		--len;
	}
	return std::string(entry.filename, entry.filename + len);
}

bool read_file_bytes(const fs::path &path, std::vector<uint8_t> &out) {
	out.clear();
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		return false;
	}
	const std::streamoff size = file.tellg();
	if (size < 0) {
		return false;
	}
	file.seekg(0, std::ios::beg);
	out.resize(static_cast<size_t>(size));
	if (out.empty()) {
		return true;
	}
	file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return static_cast<size_t>(file.gcount()) == out.size();
}

bool read_file_range(const fs::path &path, uint32_t offset, uint32_t size, std::vector<uint8_t> &out) {
	out.clear();
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	file.seekg(0, std::ios::end);
	const std::streamoff file_size = file.tellg();
	if (file_size < 0 || static_cast<uint64_t>(offset) + static_cast<uint64_t>(size) > static_cast<uint64_t>(file_size)) {
		return false;
	}
	file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	out.resize(size);
	if (out.empty()) {
		return true;
	}
	file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return static_cast<size_t>(file.gcount()) == out.size();
}

} // namespace

struct ResourceRecord {
	ResourceFileEntry entry;
	std::string lookup_key;
	uint32_t archive_offset = 0;
	uint32_t archive_size = 0;
};

struct ResourceIndex::Impl {
	std::string root_dir;
	std::string last_error;
	std::vector<ResourceRecord> records;
	std::unordered_map<std::string, size_t> lookup;
};

ResourceIndex::ResourceIndex() : impl_(std::make_unique<Impl>()) {}
ResourceIndex::~ResourceIndex() = default;
ResourceIndex::ResourceIndex(ResourceIndex &&) noexcept = default;
ResourceIndex &ResourceIndex::operator=(ResourceIndex &&) noexcept = default;

bool ResourceIndex::scan(const std::string &root_dir) {
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
	std::vector<fs::path> pff_paths;
	for (const fs::directory_entry &entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
		if (ec) {
			break;
		}
		if (!entry.is_regular_file(ec)) {
			continue;
		}
		if (extension_for_name(entry.path().filename().string()) == ".pff") {
			pff_paths.push_back(entry.path());
		}
		const std::string kind = kind_for_path(entry.path());
		if (!kind.empty()) {
			ResourceRecord record;
			record.entry = entry_from_path(entry.path(), root);
			record.lookup_key = flat_lookup_key(record.entry.logical_name);
			if (!record.lookup_key.empty() && impl_->lookup.find(record.lookup_key) == impl_->lookup.end()) {
				impl_->lookup.emplace(record.lookup_key, impl_->records.size());
				impl_->records.push_back(std::move(record));
			}
		}
	}

	std::sort(pff_paths.begin(), pff_paths.end(), [](const fs::path &a, const fs::path &b) {
		return to_lower_ascii(a.filename().string()) < to_lower_ascii(b.filename().string());
	});

	for (const fs::path &pff_path : pff_paths) {
		PffArchive archive = {};
		if (pff_open(&archive, pff_path.string().c_str()) != 0) {
			continue;
		}
		for (uint32_t i = 0; i < archive.entry_count; ++i) {
			const PffEntry &pff_entry = archive.entries[i];
			const std::string logical_name = pff_entry_name(pff_entry);
			const std::string lookup_key = flat_lookup_key(logical_name);
			if (lookup_key.empty() || impl_->lookup.find(lookup_key) != impl_->lookup.end()) {
				continue;
			}
			std::vector<uint8_t> bytes;
			if (!read_file_range(pff_path, pff_entry.offset, pff_entry.size, bytes)) {
				continue;
			}
			const std::string kind = kind_for_bytes(logical_name, bytes);
			if (kind.empty()) {
				continue;
			}
			ResourceRecord record;
			record.entry.kind = kind;
			record.entry.logical_name = logical_name;
			record.entry.display_name = display_name_from_name(logical_name);
			record.entry.relative_path = logical_name;
			record.entry.source_type = "pff";
			record.entry.archive_path = pff_path.string();
			record.lookup_key = lookup_key;
			record.archive_offset = pff_entry.offset;
			record.archive_size = pff_entry.size;
			impl_->lookup.emplace(record.lookup_key, impl_->records.size());
			impl_->records.push_back(std::move(record));
		}
		pff_close(&archive);
	}

	std::sort(impl_->records.begin(), impl_->records.end(), [](const ResourceRecord &a, const ResourceRecord &b) {
		const std::string a_key = a.entry.kind + ":" + to_lower_ascii(a.entry.relative_path);
		const std::string b_key = b.entry.kind + ":" + to_lower_ascii(b.entry.relative_path);
		return a_key < b_key;
	});
	impl_->lookup.clear();
	for (size_t i = 0; i < impl_->records.size(); ++i) {
		impl_->lookup.emplace(impl_->records[i].lookup_key, i);
	}
	return true;
}

void ResourceIndex::clear() {
	impl_->root_dir.clear();
	impl_->last_error.clear();
	impl_->lookup.clear();
	impl_->records.clear();
}

std::vector<ResourceFileEntry> ResourceIndex::resource_files(const std::string &kind) const {
	const std::string filter = normalize_kind(kind);
	std::vector<ResourceFileEntry> out;
	if (filter.empty()) {
		out.reserve(impl_->records.size());
		for (const ResourceRecord &record : impl_->records) {
			out.push_back(record.entry);
		}
		return out;
	}
	for (const ResourceRecord &record : impl_->records) {
		const ResourceFileEntry &entry = record.entry;
		if (entry.kind == filter || (filter == "object" && is_object_kind(entry.kind))) {
			out.push_back(entry);
		}
	}
	return out;
}

bool ResourceIndex::read_file(const std::string &name, std::vector<uint8_t> &out) const {
	out.clear();
	const std::string key = flat_lookup_key(name);
	if (key.empty()) {
		return false;
	}
	const auto it = impl_->lookup.find(key);
	if (it == impl_->lookup.end()) {
		return false;
	}
	const ResourceRecord &record = impl_->records[it->second];
	if (record.entry.source_type == "file") {
		return read_file_bytes(record.entry.path, out);
	}
	return read_file_range(record.entry.archive_path, record.archive_offset, record.archive_size, out);
}

const std::string &ResourceIndex::root_dir() const {
	return impl_->root_dir;
}

const std::string &ResourceIndex::last_error() const {
	return impl_->last_error;
}

} // namespace opennova
