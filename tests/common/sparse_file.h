// Sparse files for the tests that need a file of gigabytes without writing gigabytes: a file of a
// given size that takes no room on the disk (NTFS's FSCTL_SET_SPARSE, a POSIX truncate's hole), and
// bytes written at an offset into one, which allocates only the clusters they land on. Infrastructure
// only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <base/io/os_path.h>

namespace test_sparse {

// A file of `size` bytes at the UTF-8 `path` that takes no room on the disk: false when the file
// system cannot make one (a FAT volume refuses FSCTL_SET_SPARSE), so a test can skip rather than
// write the bytes.
inline bool write_sparse(const std::string &path, uint64_t size) {
#ifdef _WIN32
	HANDLE file = CreateFileW(opennova::io::os_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                          FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD returned = 0;
	LARGE_INTEGER end;
	end.QuadPart = LONGLONG(size);
	const bool ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
	                SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
	CloseHandle(file);
	return ok;
#else
	const int file = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (file < 0) return false;
	const bool ok = ::ftruncate(file, off_t(size)) == 0;
	::close(file);
	return ok;
#endif
}

// `size` bytes of `data` written at `offset` into the existing file at the UTF-8 `path` (a sparse
// one keeps its holes), the file's size grown when they end past it.
inline bool write_at(const std::string &path, uint64_t offset, const void *data, size_t size) {
	std::FILE *f = opennova::io::fopen_utf8(path.c_str(), "r+b");
	if (!f) return false;
	const bool ok = opennova::io::fseek64(f, int64_t(offset), SEEK_SET) == 0 && std::fwrite(data, 1, size, f) == size;
	return std::fclose(f) == 0 && ok;
}

} // namespace test_sparse
