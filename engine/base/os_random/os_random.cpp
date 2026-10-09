#include <base/os_random/os_random.h>

#include <base/io/log.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h> // BCryptGenRandom; the system bcrypt.lib, not the vendored opennova_bcrypt
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif
#endif

// Windows calls BCryptGenRandom with the system-preferred RNG. Linux calls getrandom(2), retrying
// short reads and EINTR, and reads /dev/urandom only where getrandom is missing (a C library
// without <sys/random.h>, or a kernel or seccomp filter that refuses the call). The other POSIX
// targets (macOS, the BSDs, Emscripten) call getentropy, 256 bytes at a time.

namespace opennova {

namespace {

[[noreturn]] void os_random_failed(const char *call, long code) {
	io::logf(io::LogLevel::kError,
	         "os_random: %s failed (%ld); aborting rather than use weaker bytes", call, code);
	std::abort();
}

#if defined(__linux__)
// The kernel's /dev/urandom, for a system without getrandom(2).
void read_urandom(unsigned char *out, size_t size) {
	int fd = -1;
	do {
		fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0) os_random_failed("open /dev/urandom", errno);
	while (size > 0) {
		const ssize_t got = ::read(fd, out, size);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) {
			const int code = got < 0 ? errno : 0;
			::close(fd);
			os_random_failed("read /dev/urandom", code);
		}
		out += got;
		size -= static_cast<size_t>(got);
	}
	::close(fd);
}
#endif

} // namespace

void os_random_bytes(void *buffer, size_t size) {
	auto *out = static_cast<unsigned char *>(buffer);
#if defined(_WIN32)
	constexpr size_t kMaxChunk = size_t(1) << 30; // the call takes a 32-bit count
	while (size > 0) {
		const size_t chunk = size < kMaxChunk ? size : kMaxChunk;
		const NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(chunk),
		                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (status < 0) os_random_failed("BCryptGenRandom", status); // !NT_SUCCESS
		out += chunk;
		size -= chunk;
	}
#elif defined(__linux__)
#if __has_include(<sys/random.h>)
	constexpr size_t kMaxChunk = size_t(1) << 25; // getrandom answers larger requests short
	while (size > 0) {
		const ssize_t got = ::getrandom(out, size < kMaxChunk ? size : kMaxChunk, 0);
		if (got > 0) {
			out += got;
			size -= static_cast<size_t>(got);
		} else if (got < 0 && errno == EINTR) {
			continue;
		} else if (got < 0 && (errno == ENOSYS || errno == EPERM)) {
			// No getrandom here: a kernel before 3.17, or a seccomp filter that refuses it.
			read_urandom(out, size);
			return;
		} else {
			os_random_failed("getrandom", got < 0 ? errno : 0);
		}
	}
#else
	read_urandom(out, size);
#endif
#else
	constexpr size_t kMaxChunk = 256; // getentropy's per-call ceiling
	while (size > 0) {
		const size_t chunk = size < kMaxChunk ? size : kMaxChunk;
		if (::getentropy(out, chunk) != 0) os_random_failed("getentropy", errno);
		out += chunk;
		size -= chunk;
	}
#endif
}

uint32_t os_random_u32() {
	uint32_t value = 0;
	os_random_bytes(&value, sizeof(value));
	return value;
}

uint64_t os_random_u64() {
	uint64_t value = 0;
	os_random_bytes(&value, sizeof(value));
	return value;
}

uint32_t os_random_nonzero_u32() {
	uint32_t value = 0;
	while (value == 0) value = os_random_u32();
	return value;
}

std::string make_uuid_v4() {
	uint64_t hi = os_random_u64();
	uint64_t lo = os_random_u64();
	hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull; // version 4
	lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // variant 1
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
	              static_cast<unsigned>((hi >> 16) & 0xFFFF), static_cast<unsigned>(hi & 0xFFFF),
	              static_cast<unsigned>(lo >> 48),
	              static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
	return buf;
}

} // namespace opennova
