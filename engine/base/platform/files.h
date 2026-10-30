#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The platform's files (ADR 0077 d2: the engine's platform API, backend-neutral): the two
// directories only the platform knows (the one the executable runs from, the user's own data
// directory for the game), a stream over one file, and the whole-file read and the atomic
// whole-file write every save takes. Paths are UTF-8 strings, '/'-separated. A desktop backend's
// file access is base/io's (disk_files.h DiskFiles), so a backend supplies the directories
// alone.
//
// The design follows SDL3's filesystem and IOStream APIs (SDL_GetBasePath, SDL_GetPrefPath,
// SDL_IOFromFile with SDL_ReadIO / SDL_WriteIO / SDL_SeekIO / SDL_TellIO / SDL_GetIOSize,
// SDL_LoadFile, SDL_SaveFile); GLFW and sokol_app leave files to the C library, as base/io's
// stdio calls do.
namespace opennova::platform {

// Read: an existing file from its start. Write: created, or truncated to nothing.
enum class FileMode : uint8_t { Read, Write };
enum class SeekFrom : uint8_t { Begin, Current, End };

class FileStream {
public:
	virtual ~FileStream() = default; // closes the file (a write stream's bytes flushed)
	// Up to `size` bytes into `data` from the position, which advances by the count returned; fewer
	// at the end of the file or on an error.
	virtual size_t read(void *data, size_t size) = 0;
	// `size` bytes from `data` at the position; the count written, fewer on an error.
	virtual size_t write(const void *data, size_t size) = 0;
	// The position moved `offset` bytes from `from`; false when it cannot be.
	virtual bool seek(int64_t offset, SeekFrom from) = 0;
	// The position, or -1 when it cannot be read.
	virtual int64_t tell() = 0;
	// The file's size in bytes, or -1 when it cannot be read.
	virtual int64_t size() = 0;
};

class Files {
public:
	virtual ~Files() = default;

	// The directory the running executable sits in, with no trailing separator.
	virtual std::string executable_dir() const = 0;
	// The user's own writable data directory for the game (SDL_GetPrefPath's), created when
	// missing, with no trailing separator; empty when the platform has none.
	virtual std::string user_data_dir() const = 0;

	// The file at `path` opened as `mode` asks; null with `error` when it cannot be.
	virtual std::unique_ptr<FileStream> open(
			const std::string &path, FileMode mode, std::string &error) = 0;

	// The whole file at `path` into `out`; false with `error` when it cannot be read (`out` then
	// empty).
	virtual bool read_file(
			const std::string &path, std::vector<uint8_t> &out, std::string &error) = 0;

	// `size` bytes from `data` as the whole file at `path`, atomically: a reader (a crash
	// included) sees the file as it was or as it is now, never a part. False with `error` when it
	// cannot be written; the file is then as it was.
	virtual bool write_file(
			const std::string &path, const void *data, size_t size, std::string &error) = 0;
};

} // namespace opennova::platform
