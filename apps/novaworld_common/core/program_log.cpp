#include "program_log.h"

#include "loggable.h"
#include "text_util.h"

#include <base/io/strutil.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace opennova::novaworld_server::program {

namespace {

// A line that never ends is cut here, as a line of its own.
constexpr size_t kMaxLine = 64 * 1024;

std::atomic<LogTee *> g_tee{ nullptr };

bool is_group_char(char c) {
	return is_alnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '#';
}

// "2026-10-18T14:05:03.123Z", the wall clock now.
std::string now_stamp() {
	using namespace std::chrono;
	const auto now = system_clock::now();
	const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
	const std::time_t secs = static_cast<std::time_t>(ms / 1000);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &secs);
#else
	gmtime_r(&secs, &tm);
#endif
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
			tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			static_cast<int>(ms % 1000));
	return buf;
}

#ifndef _WIN32
// The file's note of `n` lines it never got, in their place, at `stamp`.
std::string dropped_note(const std::string &stamp, size_t n) {
	return stamp + " [log] " + std::to_string(n) + (n == 1 ? " line" : " lines") +
			" not copied here: they came faster than the disk took them (the console has "
			"them)\n";
}

// Every byte of `data` written to `fd`; false when the descriptor refuses.
bool write_all(int fd, const char *data, size_t size) {
	while (size > 0) {
		const ssize_t n = ::write(fd, data, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		data += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}
#endif

} // namespace

bool parse_log_store(std::string_view text, LogStoreSetting &out, std::string &error) {
	out = LogStoreSetting();
	const std::string_view value = strutil::trim_view(text);
	if (value.empty())
		return true;
	if (has_control_byte(value)) {
		error = std::string(kLogStoreVariable) + " holds a control byte";
		return false;
	}
	std::string location;
	LogStoreSetting::Kind kind = LogStoreSetting::Kind::None;
	if (starts_with(value, "cloudwatch:")) {
		kind = LogStoreSetting::Kind::CloudWatch;
		location = std::string(value.substr(11));
		while (location.size() > 1 && location.back() == '/')
			location.pop_back();
		// The longest group beneath it (/machines) stays within CloudWatch's 512.
		if (location.size() < 2 || location.size() > 400 || location[0] != '/' ||
				!all_of(location, is_group_char) || location.find("//") != std::string::npos) {
			error = std::string(kLogStoreVariable) +
					" names no log group prefix (cloudwatch:/opennova/<deployment>)";
			return false;
		}
	} else if (starts_with(value, "file://")) {
		kind = LogStoreSetting::Kind::Directory;
		location = std::string(value.substr(7));
		while (location.size() > 1 && location.back() == '/')
			location.pop_back();
		if (location.size() < 2 || location[0] != '/' || location.size() > 1024 ||
				("/" + location + "/").find("/../") != std::string::npos) {
			error = std::string(kLogStoreVariable) +
					" names no absolute directory (file:///var/log/opennova)";
			return false;
		}
	} else {
		error = std::string(kLogStoreVariable) +
				" is neither cloudwatch:<group prefix> nor file://<directory>";
		return false;
	}
	out.kind = kind;
	out.location = std::move(location);
	return true;
}

LogStoreSetting log_store_from_env(std::string &error) {
	const char *v = std::getenv("ONNET_LOG_STORE");
	LogStoreSetting out;
	if (v != nullptr && !parse_log_store(v, out, error))
		out = LogStoreSetting();
	return out;
}

std::string program_log_path(std::string_view program, std::string_view task) {
	return "programs/" + std::string(program) + "/" + std::string(task) + ".log";
}

void set_log_tee(LogTee *tee) {
	g_tee.store(tee);
}

void drain_log_tee(int timeout_ms) {
	if (LogTee *tee = g_tee.load())
		tee->drain(timeout_ms);
}

#ifdef _WIN32

bool LogTee::start(const std::string &, std::string_view, std::string_view, std::string &error) {
	error = "a program's log copy needs POSIX pipes";
	return false;
}
void LogTee::stop() {}
void LogTee::drain(int) {}
void LogTee::pump() {}
void LogTee::copy_lines(Stream &, std::string_view, bool) {}
void LogTee::write_file() {}

#else

bool LogTee::start(const std::string &dir, std::string_view program, std::string_view task,
		std::string &error) {
	if (running_)
		return true;
	const std::filesystem::path file =
			std::filesystem::u8path(dir) / std::filesystem::u8path(program_log_path(program, task));
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	file_path_ = file.u8string();
	file_fd_ = ::open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (file_fd_ < 0) {
		error = "cannot open " + file.u8string() + ": " + std::strerror(errno);
		return false;
	}
	int pipes[2][2] = { { -1, -1 }, { -1, -1 } };
	int saved[2] = { -1, -1 };
	bool ok = true;
	for (int i = 0; i < 2 && ok; ++i) {
		saved[i] = ::fcntl(i + 1, F_DUPFD_CLOEXEC, 3);
		ok = saved[i] >= 0 && ::pipe2(pipes[i], O_CLOEXEC) == 0;
	}
	if (!ok) {
		error = std::string("cannot make the log's pipes: ") + std::strerror(errno);
		for (int i = 0; i < 2; ++i) {
			for (const int fd : { pipes[i][0], pipes[i][1], saved[i] })
				if (fd >= 0)
					::close(fd);
		}
		::close(file_fd_);
		file_fd_ = -1;
		return false;
	}
	std::fflush(stdout);
	std::fflush(stderr);
	for (int i = 0; i < 2; ++i) {
		Stream &s = streams_[i];
		s.fd = i + 1;
		s.read_fd = pipes[i][0];
		s.out_fd = saved[i];
		// The pipe's write end in the descriptor's place (no close-on-exec, as before).
		::dup2(pipes[i][1], s.fd);
		::close(pipes[i][1]);
	}
	running_ = true;
	writer_ = std::thread([this] { write_file(); });
	reader_ = std::thread([this] { pump(); });
	return true;
}

void LogTee::pump() {
	char buf[16 * 1024];
	pollfd fds[2] = {};
	bool open[2] = { true, true };
	while (open[0] || open[1]) {
		for (int i = 0; i < 2; ++i) {
			fds[i].fd = open[i] ? streams_[i].read_fd : -1;
			fds[i].events = POLLIN;
			fds[i].revents = 0;
		}
		if (::poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		busy_.store(true);
		// stdout before stderr when both wait: a line printed, then a warning
		// after it, keeps its order.
		for (int i = 0; i < 2; ++i) {
			if (!open[i] || fds[i].revents == 0)
				continue;
			Stream &s = streams_[i];
			const ssize_t n = ::read(s.read_fd, buf, sizeof(buf));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				// Its last write end closed (stop()): what is left of a line goes too.
				copy_lines(s, {}, true);
				open[i] = false;
				continue;
			}
			// The descriptor's own reader first (the container's log), then the file.
			write_all(s.out_fd, buf, static_cast<size_t>(n));
			copy_lines(s, std::string_view(buf, static_cast<size_t>(n)), false);
		}
		busy_.store(false);
	}
}

void LogTee::copy_lines(Stream &s, std::string_view chunk, bool flush) {
	s.partial.append(chunk.data(), chunk.size());
	std::string out;
	size_t lines = 0;
	size_t at = 0;
	const std::string stamp = now_stamp();
	for (;;) {
		const size_t eol = s.partial.find('\n', at);
		const bool cut = eol == std::string::npos && s.partial.size() - at >= kMaxLine;
		if (eol == std::string::npos && !cut && !(flush && at < s.partial.size()))
			break;
		const size_t end = eol != std::string::npos ? eol : s.partial.size();
		std::string_view line(s.partial.data() + at, end - at);
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		out += stamp;
		out.push_back(' ');
		out.append(line.data(), line.size());
		out.push_back('\n');
		++lines;
		at = eol != std::string::npos ? eol + 1 : end;
	}
	s.partial.erase(0, at);
	if (out.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(file_mu_);
		if (file_fd_ < 0)
			return;
		// A disk slower than the lines: past kMaxQueuedBytes waiting, they are dropped
		// and counted, the count noted in their place once lines fit again.
		if (queued_.size() + out.size() > kMaxQueuedBytes) {
			dropped_ += lines;
			return;
		}
		if (dropped_ > 0) {
			queued_ += dropped_note(stamp, dropped_);
			dropped_ = 0;
		}
		queued_ += out;
	}
	file_cv_.notify_one();
}

void LogTee::write_file() {
	std::unique_lock<std::mutex> lock(file_mu_);
	for (;;) {
		file_cv_.wait(lock, [this] { return !queued_.empty() || closing_; });
		// Lines dropped with none after them: noted as the copy ends.
		if (closing_ && dropped_ > 0) {
			queued_ += dropped_note(now_stamp(), dropped_);
			dropped_ = 0;
		}
		if (queued_.empty())
			return; // stop() asked, and nothing is left
		std::string out;
		out.swap(queued_);
		writing_ = true;
		const int fd = file_fd_;
		lock.unlock();
		const bool written = write_all(fd, out.data(), out.size());
		const int error = errno;
		lock.lock();
		writing_ = false;
		if (!written) {
			// A file that no longer takes lines (a full disk): the copy stops, said once
			// on the console's stderr, which goes on with every line.
			::close(file_fd_);
			file_fd_ = -1;
			queued_.clear();
			dropped_ = 0;
			lock.unlock();
			const std::string line = "[log] the copy into " + file_path_ +
					" stopped: " + std::strerror(error) + "; the console keeps every line\n";
			write_all(streams_[1].out_fd, line.data(), line.size());
			return;
		}
	}
}

void LogTee::drain(int timeout_ms) {
	if (!running_)
		return;
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < until) {
		bool pending = busy_.load();
		for (Stream &s : streams_) {
			int queued = 0;
			if (::ioctl(s.read_fd, FIONREAD, &queued) == 0 && queued > 0)
				pending = true;
		}
		{
			std::lock_guard<std::mutex> lock(file_mu_);
			pending = pending || writing_ || !queued_.empty();
		}
		if (!pending)
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

void LogTee::stop() {
	if (!running_)
		return;
	std::fflush(stdout);
	std::fflush(stderr);
	// The descriptors back as they were: the pipes' last write ends close, so each
	// reader copies what is left and ends.
	for (Stream &s : streams_)
		::dup2(s.out_fd, s.fd);
	if (reader_.joinable())
		reader_.join();
	// The writer writes what waits for the file, then ends.
	{
		std::lock_guard<std::mutex> lock(file_mu_);
		closing_ = true;
	}
	file_cv_.notify_one();
	if (writer_.joinable())
		writer_.join();
	for (Stream &s : streams_) {
		::close(s.read_fd);
		::close(s.out_fd);
		s.read_fd = -1;
		s.out_fd = -1;
		s.partial.clear();
	}
	if (file_fd_ >= 0)
		::close(file_fd_);
	file_fd_ = -1;
	closing_ = false;
	running_ = false;
}

#endif

} // namespace opennova::novaworld_server::program
