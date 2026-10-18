#include "slot_files.h"

#include "hosted/protocol/agent_protocol.h"
#include "hosted/protocol/slots.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace opennova::hostd {

namespace fs = std::filesystem;
namespace hosted = novaworld_server::hosted;

namespace {

std::string os_error(const char *what, const std::string &path) {
	return std::string(what) + " " + path + ": " + std::strerror(errno);
}

bool write_all(int fd, const std::string &bytes) {
	size_t done = 0;
	while (done < bytes.size()) {
		const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		done += static_cast<size_t>(n);
	}
	return true;
}

} // namespace

SlotFiles::SlotFiles(std::string root, bool chown) : root_(std::move(root)), chown_(chown) {}

std::string SlotFiles::dir(int slot) const {
	return root_ + "/" + hosted::slot_dir_name(slot);
}

bool SlotFiles::ensure_root(std::string &error) {
	if (::mkdir(root_.c_str(), 0755) != 0 && errno != EEXIST) {
		error = os_error("cannot make", root_);
		return false;
	}
	struct stat st{};
	if (::lstat(root_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
		error = root_ + " is no directory";
		return false;
	}
	return true;
}

bool SlotFiles::wipe(int slot, std::string &error) {
	if (!hosted::slot_valid(slot))
		return true;
	std::error_code ec;
	fs::remove_all(fs::path(dir(slot)), ec);
	if (ec) {
		error = "cannot remove " + dir(slot) + ": " + ec.message();
		return false;
	}
	return true;
}

bool SlotFiles::prepare(int slot, uint32_t uid, const std::map<std::string, std::string> &files,
		std::string &error) {
	if (!hosted::slot_valid(slot)) {
		error = "no slot " + std::to_string(slot);
		return false;
	}
	if (!wipe(slot, error))
		return false;
	const std::string path = dir(slot);
	if (::mkdir(path.c_str(), 0700) != 0) {
		error = os_error("cannot make", path);
		return false;
	}
	// The mode again past the umask, then the owner.
	if (::chmod(path.c_str(), 0700) != 0 || (chown_ && ::chown(path.c_str(), uid, uid) != 0)) {
		error = os_error("cannot hand over", path);
		return false;
	}
	for (const auto &file : files) {
		if (!hosted::is_slot_file_name(file.first)) {
			error = "a file's name is no bare file name";
			return false;
		}
		const std::string file_path = path + "/" + file.first;
		const int fd = ::open(
				file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0) {
			error = os_error("cannot create", file_path);
			return false;
		}
		const bool ok = ::fchmod(fd, 0600) == 0 && (!chown_ || ::fchown(fd, uid, uid) == 0) &&
				write_all(fd, file.second);
		const int saved = errno;
		::close(fd);
		if (!ok) {
			errno = saved;
			error = os_error("cannot write", file_path);
			return false;
		}
	}
	return true;
}

bool SlotFiles::read_written(int slot, const char *name, std::string &out,
		std::chrono::system_clock::time_point &written) const {
	out.clear();
	if (!hosted::slot_valid(slot))
		return false;
	const int dir_fd = ::open(dir(slot).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir_fd < 0)
		return false;
	const int fd = ::openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	::close(dir_fd);
	if (fd < 0)
		return false;
	struct stat st{};
	bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
			static_cast<uint64_t>(st.st_size) <= kMaxWrittenBytes;
	// One byte past the bound tells a file that grew past it since.
	char bytes[kMaxWrittenBytes + 1];
	size_t got = 0;
	while (ok && got < sizeof bytes) {
		const ssize_t n = ::read(fd, bytes + got, sizeof bytes - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ok = n == 0;
			break;
		}
		got += static_cast<size_t>(n);
	}
	::close(fd);
	if (!ok || got > kMaxWrittenBytes)
		return false;
	out.assign(bytes, got);
	written = std::chrono::system_clock::time_point(
			std::chrono::duration_cast<std::chrono::system_clock::duration>(
					std::chrono::seconds(st.st_mtim.tv_sec) +
					std::chrono::nanoseconds(st.st_mtim.tv_nsec)));
	return true;
}

} // namespace opennova::hostd
