#include <editor/project/project_files.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string os_error(const char *what, const std::string &path, const std::error_code &ec) {
	return std::string(what) + " " + path + ": " + ec.message();
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
	std::FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f) {
		error = "cannot create " + tmp;
		return false;
	}
	const bool written = size == 0 || std::fwrite(data, 1, size, f) == size;
	const bool closed = std::fclose(f) == 0;
	if (!written || !closed) {
		std::error_code ignored;
		fs::remove(tmp, ignored);
		error = "cannot write " + tmp;
		return false;
	}
	// std::filesystem::rename replaces an existing target on every platform (unlike C
	// rename on Windows), so the swap is one call.
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if (ec) {
		std::error_code ignored;
		fs::remove(tmp, ignored);
		error = os_error("cannot replace", path, ec);
		return false;
	}
	return true;
}

bool write_file_atomic(const std::string &path, const std::string &text, std::string &error) {
	return write_file_atomic(path, text.data(), text.size(), error);
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

} // namespace opennova::editor
