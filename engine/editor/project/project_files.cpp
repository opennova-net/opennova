#include <editor/project/project_files.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/project_build/archive_routing.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string os_error(const char *what, const std::string &path, const std::error_code &ec) {
	return std::string(what) + " " + path + ": " + ec.message();
}

// The whole of `data` written to `path` (created or truncated); a partial file is removed.
bool write_whole(const std::string &path, const void *data, size_t size, std::string &error) {
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		error = "cannot create " + path;
		return false;
	}
	const bool written = size == 0 || std::fwrite(data, 1, size, f) == size;
	const bool closed = std::fclose(f) == 0;
	if (!written || !closed) {
		std::error_code ignored;
		fs::remove(path, ignored);
		error = "cannot write " + path;
		return false;
	}
	return true;
}

} // namespace

bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		error = "cannot open " + path;
		return false;
	}
	uint8_t buf[64 * 1024];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		out.insert(out.end(), buf, buf + n);
	}
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	if (!ok) {
		error = "cannot read " + path;
		out.clear();
	}
	return ok;
}

bool read_file_text(const std::string &path, std::string &out, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!read_file_bytes(path, bytes, error)) return false;
	out.assign(bytes.begin(), bytes.end());
	return true;
}

bool write_file_atomic(const std::string &path, const void *data, size_t size, std::string &error) {
	const std::string tmp = path + ".tmp";
	if (!write_whole(tmp, data, size, error)) return false;
	if (!replace_file(tmp, path, error)) {
		std::error_code ignored;
		fs::remove(tmp, ignored);
		return false;
	}
	return true;
}

bool write_file_atomic(const std::string &path, const std::string &text, std::string &error) {
	return write_file_atomic(path, text.data(), text.size(), error);
}

bool replace_file(const std::string &from, const std::string &to, std::string &error) {
	// std::filesystem::rename replaces an existing target on every platform (unlike C
	// rename on Windows), so the swap is one call.
	std::error_code ec;
	fs::rename(from, to, ec);
	if (ec) {
		error = os_error("cannot replace", to, ec);
		return false;
	}
	return true;
}

bool write_files_together(const std::vector<FileText> &files, std::vector<std::string> &problems,
                          const FileReplace &replace) {
	std::vector<std::vector<uint8_t>> originals(files.size());
	std::vector<std::string> staged;
	const auto remove_staged = [&](size_t from) {
		for (size_t i = from; i < staged.size(); ++i) {
			std::error_code ignored;
			fs::remove(staged[i], ignored);
		}
	};
	// What each file holds, to put back; then every text beside its file.
	for (size_t i = 0; i < files.size(); ++i) {
		std::string error;
		if (!read_file_bytes(files[i].path, originals[i], error)) {
			problems.push_back(error);
			return false;
		}
	}
	for (const FileText &file : files) {
		std::string error;
		const std::string tmp = file.path + ".tmp";
		if (!write_whole(tmp, file.text.data(), file.text.size(), error)) {
			remove_staged(0);
			problems.push_back(error);
			return false;
		}
		staged.push_back(tmp);
	}
	for (size_t i = 0; i < files.size(); ++i) {
		std::string error;
		if (replace(staged[i], files[i].path, error)) continue;
		problems.push_back(error.empty() ? "cannot replace " + files[i].path : error);
		remove_staged(i);
		for (size_t j = 0; j < i; ++j) {
			std::string restore;
			if (!write_file_atomic(files[j].path, originals[j].data(), originals[j].size(), restore))
				problems.push_back(files[j].path + " keeps the new text: " + restore);
		}
		return false;
	}
	return true;
}

bool ensure_directory(const std::string &path, std::string &error) {
	std::error_code ec;
	fs::create_directories(path, ec);
	if (ec && !fs::is_directory(path, ec)) {
		error = os_error("cannot create directory", path, ec);
		return false;
	}
	return true;
}

bool is_dot_directory(const fs::path &path) {
	const std::string name = path.filename().string();
	return !name.empty() && name[0] == '.';
}

std::string shown_path(const std::string &path, const std::string &root) {
	if (root.empty()) return path;
	const fs::path relative = fs::path(path).lexically_relative(root);
	if (relative.empty() || *relative.begin() == "..") return path;
	return relative.generic_string();
}

bool check_file_name(const std::string &name, AssetKind kind, std::string &problem, std::string &message) {
	if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
		problem = "name";
		message = "'" + name + "' is not a plain file name: give a name with no folders.";
		return false;
	}
	// The archive's name limit binds only a file the build packs: a loose kind (a video,
	// a music bank, a config) is copied beside the archives under any name.
	if (kind != AssetKind::Unknown && route_asset(kind) != ArchiveSlot::Loose && !logical_name_fits_archive(name)) {
		problem = "name";
		message = "'" + name + "' does not fit the game's archives: names are up to 16 characters.";
		return false;
	}
	if (kind != AssetKind::Unknown) {
		if (!asset_name_fits_kind(name, kind)) {
			problem = "kind";
			message = "The game reads '" + name + "' as " + asset_kind_label(classify_asset(name, nullptr)) + ", not " +
			          asset_kind_label(kind) + ": a file's name decides its kind.";
			return false;
		}
	}
	return true;
}

bool check_project_file_name(const std::string &root, const std::string &dir, const std::string &name, AssetKind kind,
                             std::string &problem, std::string &message) {
	if (!check_file_name(name, kind, problem, message)) return false;
	// Where it lands, symbolic links resolved, must stay under the project.
	std::error_code ec;
	fs::path within;
	if (!root.empty()) {
		const fs::path base = fs::weakly_canonical(fs::path(root), ec);
		if (!ec) {
			const fs::path resolved = fs::weakly_canonical(fs::path(root) / dir / name, ec);
			if (!ec) within = resolved.lexically_relative(base);
		}
	}
	if (within.empty() || within.is_absolute() || *within.begin() == "..") {
		problem = "path";
		message = "'" + (fs::path(dir) / name).generic_string() + "' would land outside the project.";
		return false;
	}
	return true;
}

} // namespace opennova::editor
