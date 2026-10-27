#include "zip_files.h"

#include <base/io/os_path.h>

#include <miniz.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

namespace fs = std::filesystem;

// Whether `name`, an entry's, names a path under the folder: relative and '/'-separated, every
// part neither empty, "." nor "..", no backslash, colon or control byte (a directory entry's
// trailing '/' aside).
bool safe_entry_name(const std::string &name) {
	if (name.empty() || name.front() == '/')
		return false;
	for (size_t start = 0; start < name.size();) {
		const size_t end = std::min(name.find('/', start), name.size());
		const std::string part = name.substr(start, end - start);
		if (part.empty() || part == "." || part == "..")
			return false;
		for (const char c : part)
			if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':')
				return false;
		start = end + 1;
	}
	return true;
}

// An archive read and ended with the scope.
struct Reader {
	mz_zip_archive archive{};
	bool open = false;
	~Reader() {
		if (open)
			mz_zip_reader_end(&archive);
	}
};

} // namespace

bool unzip_files(const std::string &zip, const std::string &dir, std::string &error) {
	Reader reader;
	reader.open = mz_zip_reader_init_file(&reader.archive, zip.c_str(), 0) != 0;
	if (!reader.open) {
		error = zip + " is no zip archive";
		return false;
	}
	// Every entry checked before a byte is written.
	const mz_uint count = mz_zip_reader_get_num_files(&reader.archive);
	std::vector<std::string> names;
	names.reserve(count);
	for (mz_uint i = 0; i < count; ++i) {
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&reader.archive, i, &stat)) {
			error = "entry " + std::to_string(i) + " of " + zip + " does not read";
			return false;
		}
		std::string name = stat.m_filename;
		const bool directory = mz_zip_reader_is_file_a_directory(&reader.archive, i) != 0;
		if (!safe_entry_name(directory && !name.empty() && name.back() == '/'
							? name.substr(0, name.size() - 1)
							: name)) {
			error = "the entry \"" + name + "\" names no path under the folder";
			return false;
		}
		if (mz_zip_reader_is_file_encrypted(&reader.archive, i) ||
				!mz_zip_reader_is_file_supported(&reader.archive, i)) {
			error = "the entry \"" + name + "\" is encrypted or packed by an unread method";
			return false;
		}
		names.push_back(std::move(name));
	}
	std::error_code ec;
	fs::create_directories(io::os_path(dir), ec);
	if (ec) {
		error = "cannot make " + dir + ": " + ec.message();
		return false;
	}
	for (mz_uint i = 0; i < count; ++i) {
		const fs::path to = io::os_path(io::utf8_join(dir, names[i]));
		if (mz_zip_reader_is_file_a_directory(&reader.archive, i)) {
			fs::create_directories(to, ec);
			if (ec) {
				error = "cannot make " + io::utf8_path(to) + ": " + ec.message();
				return false;
			}
			continue;
		}
		fs::create_directories(to.parent_path(), ec);
		if (ec ||
				!mz_zip_reader_extract_to_file(&reader.archive, i, io::utf8_path(to).c_str(), 0)) {
			error = "cannot unpack " + names[i] + " from " + zip;
			return false;
		}
	}
	return true;
}

bool zip_directory(const std::string &dir, const std::string &zip, std::string &error) {
	const fs::path root = io::os_path(dir);
	std::vector<std::pair<std::string, std::string>> files; // the entry's name, the file's path
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(root, ec);
			!ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		std::error_code kind;
		if (it->is_regular_file(kind))
			files.emplace_back(io::utf8_generic_path(it->path().lexically_relative(root)),
					io::utf8_path(it->path()));
	}
	if (ec) {
		error = "cannot list " + dir + ": " + ec.message();
		return false;
	}
	std::sort(files.begin(), files.end());
	mz_zip_archive archive{};
	if (!mz_zip_writer_init_file(&archive, zip.c_str(), 0)) {
		error = "cannot write " + zip;
		return false;
	}
	bool ok = true;
	for (const auto &[name, path] : files)
		if (!mz_zip_writer_add_file(
					&archive, name.c_str(), path.c_str(), nullptr, 0, MZ_DEFAULT_LEVEL)) {
			error = "cannot pack " + name + " into " + zip;
			ok = false;
			break;
		}
	if (ok && !mz_zip_writer_finalize_archive(&archive)) {
		error = "cannot finish " + zip;
		ok = false;
	}
	mz_zip_writer_end(&archive);
	if (!ok)
		fs::remove(io::os_path(zip), ec);
	return ok;
}

} // namespace opennova::novaworld_server
