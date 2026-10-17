#include "local_object_store.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace opennova::novaworld_server {

namespace fs = std::filesystem;

namespace {

constexpr const char *kUploadsDir = ".uploads";
constexpr int kMaxParts = 10000; // S3's own limit

bool copy_file_to(const fs::path &from, const fs::path &to, std::string &error) {
	std::error_code ec;
	fs::create_directories(to.parent_path(), ec);
	fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "cannot copy " + io::utf8_path(from) + " to " + io::utf8_path(to) + ": " +
				ec.message();
		return false;
	}
	return true;
}

} // namespace

std::string LocalObjectStore::path_of(std::string_view key) const {
	return io::utf8_join(root_, key);
}

std::string LocalObjectStore::url_of(std::string_view key) const {
	ObjectStoreConfig config;
	config.kind = ObjectStoreConfig::Kind::Local;
	config.root = root_;
	return object_store_url(config, key);
}

bool LocalObjectStore::put_file(std::string_view key, const std::string &path, std::string &error) {
	if (!is_store_key(key)) {
		error = "not a store key: " + std::string(key);
		return false;
	}
	return copy_file_to(io::os_path(path), io::os_path(path_of(key)), error);
}

bool LocalObjectStore::get_file(std::string_view key, const std::string &path, std::string &error) {
	return copy_file_to(io::os_path(path_of(key)), io::os_path(path), error);
}

bool LocalObjectStore::copy(std::string_view from_key, std::string_view to_key, std::string &error) {
	if (!is_store_key(to_key)) {
		error = "not a store key: " + std::string(to_key);
		return false;
	}
	return copy_file_to(io::os_path(path_of(from_key)), io::os_path(path_of(to_key)), error);
}

bool LocalObjectStore::copy_prefix(
		std::string_view from_prefix, std::string_view to_prefix, std::string &error) {
	std::vector<StoredObject> objects;
	if (!list(from_prefix, objects, error))
		return false;
	for (const StoredObject &object : objects) {
		const std::string to_key = std::string(to_prefix) + object.key.substr(from_prefix.size());
		if (!copy(object.key, to_key, error))
			return false;
	}
	return true;
}

bool LocalObjectStore::list(
		std::string_view prefix, std::vector<StoredObject> &out, std::string &error) {
	out.clear();
	std::error_code ec;
	const fs::path root = io::os_path(root_);
	if (!fs::is_directory(root, ec))
		return true;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code type_ec;
		if (it->is_directory(type_ec)) {
			if (it->path().filename() == kUploadsDir)
				it.disable_recursion_pending();
			continue;
		}
		if (!it->is_regular_file(type_ec))
			continue;
		const std::string key = io::utf8_generic_path(fs::relative(it->path(), root, type_ec));
		if (key.rfind(prefix, 0) != 0)
			continue;
		StoredObject object;
		object.key = key;
		object.size = static_cast<uint64_t>(fs::file_size(it->path(), type_ec));
		out.push_back(std::move(object));
	}
	if (ec) {
		error = "cannot list " + root_ + ": " + ec.message();
		return false;
	}
	return true;
}

bool LocalObjectStore::head(std::string_view key, bool &exists, uint64_t &size, std::string &error) {
	(void)error;
	std::error_code ec;
	const fs::path path = io::os_path(path_of(key));
	exists = fs::is_regular_file(path, ec);
	size = exists ? static_cast<uint64_t>(fs::file_size(path, ec)) : 0;
	return true;
}

bool LocalObjectStore::remove(std::string_view key, std::string &error) {
	std::error_code ec;
	fs::remove(io::os_path(path_of(key)), ec);
	if (ec) {
		error = "cannot remove " + std::string(key) + ": " + ec.message();
		return false;
	}
	return true;
}

bool LocalObjectStore::remove_prefix(std::string_view prefix, std::string &error) {
	std::vector<StoredObject> objects;
	if (!list(prefix, objects, error))
		return false;
	for (const StoredObject &object : objects)
		if (!remove(object.key, error))
			return false;
	// The emptied directories, up to the root.
	std::error_code ec;
	std::string_view dir = prefix;
	while (!dir.empty() && dir.back() == '/')
		dir.remove_suffix(1);
	if (!dir.empty())
		fs::remove_all(io::os_path(path_of(dir)), ec);
	return true;
}

std::string LocalObjectStore::upload_dir(std::string_view upload_id) const {
	return io::utf8_join(io::utf8_join(root_, kUploadsDir), upload_id);
}

bool LocalObjectStore::is_upload_id(std::string_view upload_id) {
	if (upload_id.size() != 36)
		return false;
	for (const char c : upload_id)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-'))
			return false;
	return true;
}

bool LocalObjectStore::multipart_begin(
		std::string_view key, std::string &upload_id, std::string &error) {
	if (!is_store_key(key)) {
		error = "not a store key: " + std::string(key);
		return false;
	}
	const std::string token = make_uuid_v4();
	const std::string dir = upload_dir(token);
	std::error_code ec;
	fs::create_directories(io::os_path(dir), ec);
	if (ec || !io::write_file_atomic(io::utf8_join(dir, "key"), std::string(key), error)) {
		if (error.empty())
			error = "cannot make the upload directory " + dir;
		return false;
	}
	upload_id = token;
	return true;
}

bool LocalObjectStore::upload_part(std::string_view key, std::string_view upload_id,
		int part_number, std::string_view data, std::string &etag, std::string &error) {
	if (!is_upload_id(upload_id) || part_number < 1 || part_number > kMaxParts) {
		error = "no such upload or part";
		return false;
	}
	const std::string dir = upload_dir(upload_id);
	std::string stored_key;
	if (!io::read_file_text(io::utf8_join(dir, "key"), stored_key, error) || stored_key != key) {
		error = "no upload " + std::string(upload_id) + " of " + std::string(key);
		return false;
	}
	etag = io::sha256_hex(data.data(), data.size());
	return io::write_file_atomic(
				   io::utf8_join(dir, std::to_string(part_number)), data.data(), data.size(), error) &&
			io::write_file_atomic(io::utf8_join(dir, std::to_string(part_number) + ".etag"), etag, error);
}

bool LocalObjectStore::multipart_complete(std::string_view key, std::string_view upload_id,
		const std::vector<UploadPart> &parts, std::string &error) {
	if (!is_upload_id(upload_id)) {
		error = "no such upload";
		return false;
	}
	const std::string dir = upload_dir(upload_id);
	std::string stored_key;
	if (!io::read_file_text(io::utf8_join(dir, "key"), stored_key, error) || stored_key != key) {
		error = "the upload " + std::string(upload_id) + " is not of " + std::string(key);
		return false;
	}
	const std::string target = path_of(key);
	std::error_code ec;
	fs::create_directories(io::os_path(target).parent_path(), ec);
	const std::string tmp = target + ".tmp";
	{
		std::ofstream out(io::os_path(tmp), std::ios::binary | std::ios::trunc);
		int expected = 1;
		for (const UploadPart &part : parts) {
			if (part.number != expected) {
				error = "the parts are not 1..n in order";
				return false;
			}
			++expected;
			std::string etag;
			std::vector<uint8_t> bytes;
			if (!io::read_file_text(io::utf8_join(dir, std::to_string(part.number) + ".etag"), etag, error) ||
					etag != part.etag) {
				error = "part " + std::to_string(part.number) + " was not uploaded or its ETag differs";
				return false;
			}
			if (!io::read_file_bytes(io::utf8_join(dir, std::to_string(part.number)), bytes, error))
				return false;
			out.write(reinterpret_cast<const char *>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
		}
		if (!out) {
			error = "cannot write " + target;
			return false;
		}
	}
	fs::rename(io::os_path(tmp), io::os_path(target), ec);
	if (ec) {
		error = "cannot put " + target + " in place: " + ec.message();
		return false;
	}
	fs::remove_all(io::os_path(dir), ec);
	return true;
}

bool LocalObjectStore::multipart_abort(
		std::string_view key, std::string_view upload_id, std::string &error) {
	(void)key;
	if (!is_upload_id(upload_id)) {
		error = "no such upload";
		return false;
	}
	std::error_code ec;
	fs::remove_all(io::os_path(upload_dir(upload_id)), ec);
	return true;
}

bool LocalObjectStore::abort_uploads_under(std::string_view prefix, std::string &error) {
	std::error_code ec;
	const fs::path uploads = io::os_path(io::utf8_join(root_, kUploadsDir));
	if (!fs::is_directory(uploads, ec))
		return true;
	for (fs::directory_iterator it(uploads, ec), end; !ec && it != end; it.increment(ec)) {
		std::string stored_key, ignored;
		if (io::read_file_text(io::utf8_join(io::utf8_path(it->path()), "key"), stored_key, ignored) &&
				stored_key.rfind(prefix, 0) == 0) {
			std::error_code rm;
			fs::remove_all(it->path(), rm);
		}
	}
	(void)error;
	return true;
}

} // namespace opennova::novaworld_server
