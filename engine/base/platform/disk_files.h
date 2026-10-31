#pragma once

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/platform/files.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// The disk's files as the platform's (files.h), over base/io: the stream is stdio through
// io::fopen_utf8 and the 64-bit seek (io::fseek64 / io::ftell64), the whole-file read is
// io::read_file_bytes and the atomic write io::write_file_atomic, so a path outside the ANSI code
// page or past MAX_PATH reads and writes as any other. A desktop backend derives from it and
// supplies the two directories (SDL_GetBasePath, SDL_GetPrefPath). Platform file I/O, not a port.
//
// STAGED, NOT WIRED: no backend derives from it yet (the backend swap is deferred, ADR 0077).
namespace opennova::platform {

class DiskFileStream final : public FileStream {
public:
	explicit DiskFileStream(std::FILE *file) : file_(file) {}
	~DiskFileStream() override {
		if (file_ != nullptr)
			std::fclose(file_);
	}
	DiskFileStream(const DiskFileStream &) = delete;
	DiskFileStream &operator=(const DiskFileStream &) = delete;

	size_t read(void *data, size_t size) override { return std::fread(data, 1, size, file_); }
	size_t write(const void *data, size_t size) override {
		return std::fwrite(data, 1, size, file_);
	}
	bool seek(int64_t offset, SeekFrom from) override {
		const int origin = from == SeekFrom::Begin ? SEEK_SET
				: from == SeekFrom::Current		   ? SEEK_CUR
												   : SEEK_END;
		return io::fseek64(file_, offset, origin) == 0;
	}
	int64_t tell() override { return io::ftell64(file_); }
	int64_t size() override {
		const int64_t at = io::ftell64(file_);
		if (at < 0 || io::fseek64(file_, 0, SEEK_END) != 0)
			return -1;
		const int64_t end = io::ftell64(file_);
		return io::fseek64(file_, at, SEEK_SET) == 0 ? end : -1;
	}

private:
	std::FILE *file_;
};

class DiskFiles : public Files {
public:
	std::unique_ptr<FileStream> open(
			const std::string &path, FileMode mode, std::string &error) override {
		std::FILE *file = io::fopen_utf8(path.c_str(), mode == FileMode::Read ? "rb" : "wb");
		if (file == nullptr) {
			error = (mode == FileMode::Read ? "cannot open " : "cannot create ") + path;
			return nullptr;
		}
		return std::make_unique<DiskFileStream>(file);
	}
	bool read_file(
			const std::string &path, std::vector<uint8_t> &out, std::string &error) override {
		return io::read_file_bytes(path, out, error);
	}
	bool write_file(
			const std::string &path, const void *data, size_t size, std::string &error) override {
		return io::write_file_atomic(path, data, size, error);
	}
};

} // namespace opennova::platform
