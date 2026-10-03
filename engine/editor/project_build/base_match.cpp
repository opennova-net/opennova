#include <editor/project_build/base_match.h>

#include <filesystem>
#include <system_error>
#include <vector>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

uint64_t hash_of(const std::vector<uint8_t> &bytes) {
	return io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
}

} // namespace

bool BaseMatch::open(const std::string &install, const std::string &game, std::string &error) {
	install_ = install;
	game_ = game;
	std::error_code ec;
	if (install.empty() || !fs::is_directory(system_path(install), ec)) {
		error = install.empty() ? std::string("no game install is set") : "the game install " + install + " is no folder";
		return false;
	}
	InstallSpec spec; // the base game: no expansion, no renames (base_install_spec)
	spec.root = install;
	spec.game = game;
	std::string why;
	if (!view_.open(spec, why)) {
		error = "the game install " + install + " does not mount: " + why;
		return false;
	}
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(install), ec)) {
		std::error_code kind;
		if (entry.is_regular_file(kind)) loose_[normalized_logical_name(utf8_of(entry.path().filename()))] = utf8_of(entry.path());
	}
	for (const char *archive : kBootArchiveTable) {
		const auto found = loose_.find(normalized_logical_name(archive));
		archives_stamp_ += (found == loose_.end() ? std::string("-") : stamp_of(found->second)) + ";";
	}
	return true;
}

std::string BaseMatch::stamp_of(const std::string &path) {
	std::error_code ec;
	const uint64_t size = fs::file_size(system_path(path), ec);
	const int64_t written = ec ? 0 : io::file_modified_ticks(system_path(path));
	return std::to_string(size) + ":" + std::to_string(written);
}

bool BaseMatch::cached(const std::string &key, const std::string &stamp, BaseCopy &out) {
	const auto found = cache_.find(key);
	if (found == cache_.end() || found->second.stamp != stamp) return false;
	out = found->second.copy;
	kept_[key] = found->second;
	return true;
}

bool BaseMatch::archive_copy(const std::string &name, BaseCopy &out, uint64_t &read_bytes) {
	const InstallFile *file = view_.find(name);
	if (!file || !file->loose_path.empty()) return false;
	const std::string key = "archive:" + normalized_logical_name(name);
	if (cached(key, archives_stamp_, out)) return true;
	std::vector<uint8_t> bytes;
	if (!view_.vfs().read_file_raw(file->member, bytes)) return false;
	read_bytes += bytes.size();
	out.size = bytes.size();
	out.raw = hash_of(bytes);
	if (view_.read(*file, bytes)) {
		out.served = hash_of(bytes);
		out.served_size = bytes.size();
	} else {
		out.served = out.raw;
		out.served_size = out.size;
	}
	kept_[key] = Cached{archives_stamp_, out};
	return true;
}

bool BaseMatch::root_copy(const std::string &name, BaseCopy &out, uint64_t &read_bytes) {
	const auto found = loose_.find(normalized_logical_name(name));
	if (found == loose_.end()) return false;
	const std::string key = "loose:" + normalized_logical_name(name);
	const std::string stamp = stamp_of(found->second);
	if (cached(key, stamp, out)) return true;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!read_file_bytes(found->second, bytes, error)) return false;
	read_bytes += bytes.size();
	out.size = out.served_size = bytes.size();
	out.raw = out.served = hash_of(bytes);
	kept_[key] = Cached{stamp, out};
	return true;
}

void BaseMatch::load(const io::JsonValue &section) {
	cache_.clear();
	if (!section.is_object() || section.get_string("install", "") != install_ || section.get_string("game", "") != game_)
		return;
	const io::JsonValue *files = section.get("files");
	if (!files || !files->is_array()) return;
	for (const io::JsonValue &item : files->array) {
		if (!item.is_object()) continue;
		Cached cached;
		const std::string key = item.get_string("key", "");
		cached.stamp = item.get_string("stamp", "");
		cached.copy.size = uint64_t(item.get_number("size", 0));
		cached.copy.served_size = uint64_t(item.get_number("served_size", 0));
		if (key.empty() || cached.stamp.empty() || !io::parse_hex64(item.get_string("raw", ""), cached.copy.raw) ||
		    !io::parse_hex64(item.get_string("served", ""), cached.copy.served))
			continue;
		cache_[key] = cached;
	}
}

io::JsonValue BaseMatch::save() const {
	io::JsonValue section = io::JsonValue::make_object();
	section.set("install", io::JsonValue::make_string(install_));
	section.set("game", io::JsonValue::make_string(game_));
	io::JsonValue files = io::JsonValue::make_array();
	for (const auto &[key, cached] : kept_) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("key", io::JsonValue::make_string(key));
		item.set("stamp", io::JsonValue::make_string(cached.stamp));
		item.set("size", io::JsonValue::make_number(double(cached.copy.size)));
		item.set("served_size", io::JsonValue::make_number(double(cached.copy.served_size)));
		item.set("raw", io::JsonValue::make_string(io::hex64(cached.copy.raw)));
		item.set("served", io::JsonValue::make_string(io::hex64(cached.copy.served)));
		files.push(std::move(item));
	}
	section.set("files", std::move(files));
	return section;
}

} // namespace opennova::editor
