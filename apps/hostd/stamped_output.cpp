#include "stamped_output.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace opennova::hostd {

namespace {

void write_all(int fd, const std::string &text) {
	size_t done = 0;
	while (done < text.size()) {
		const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		done += static_cast<size_t>(n);
	}
}

} // namespace

std::string line_stamp() {
	using namespace std::chrono;
	const int64_t ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
	std::tm utc{};
	gmtime_r(&seconds, &utc);
	char text[32];
	std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
			utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
			static_cast<int>(ms % 1000));
	return text;
}

void stamp_lines(
		std::string_view text, const std::string &stamp, std::string &partial, std::string &out) {
	partial.append(text.data(), text.size());
	size_t start = 0;
	for (size_t eol = partial.find('\n'); eol != std::string::npos;
			eol = partial.find('\n', start)) {
		size_t end = eol;
		if (end > start && partial[end - 1] == '\r')
			--end;
		out += stamp;
		out += ' ';
		out.append(partial, start, end - start);
		out += '\n';
		start = eol + 1;
	}
	partial.erase(0, start);
}

StampedOutput::~StampedOutput() {
	stop_.store(true);
	if (thread_.joinable())
		thread_.join();
	// What the servers wrote before they ended, to the last line.
	for (Drain &d : drains_) {
		char buffer[16 * 1024];
		std::string out;
		ssize_t n = 0;
		while ((n = ::read(d.read_fd, buffer, sizeof(buffer))) > 0)
			stamp_lines(
					std::string_view(buffer, static_cast<size_t>(n)), line_stamp(), d.partial, out);
		if (!d.partial.empty())
			stamp_lines("\n", line_stamp(), d.partial, out);
		write_all(d.file_fd, out);
		::close(d.read_fd);
		::close(d.file_fd);
	}
}

bool StampedOutput::open(
		const std::string &path, std::string &child_path, int &write_fd, std::string &error) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	Drain d;
	d.file_fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (d.file_fd < 0) {
		error = "cannot open " + path + ": " + std::strerror(errno);
		return false;
	}
	int ends[2] = { -1, -1 };
	if (::pipe2(ends, O_CLOEXEC) != 0) {
		error = std::string("pipe: ") + std::strerror(errno);
		::close(d.file_fd);
		return false;
	}
	::fcntl(ends[0], F_SETFL, ::fcntl(ends[0], F_GETFL) | O_NONBLOCK);
	d.read_fd = ends[0];
	write_fd = ends[1];
	child_path = "/proc/self/fd/" + std::to_string(ends[1]);
	{
		std::lock_guard<std::mutex> lock(mu_);
		drains_.push_back(std::move(d));
	}
	if (!thread_.joinable())
		thread_ = std::thread([this] { run(); });
	return true;
}

bool StampedOutput::pump(Drain &d) {
	char buffer[16 * 1024];
	const ssize_t n = ::read(d.read_fd, buffer, sizeof(buffer));
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return true;
	std::string out;
	if (n > 0) {
		stamp_lines(std::string_view(buffer, static_cast<size_t>(n)), line_stamp(), d.partial, out);
		write_all(d.file_fd, out);
		return true;
	}
	// Its end: the line it left unended, then the descriptors.
	if (!d.partial.empty()) {
		stamp_lines("\n", line_stamp(), d.partial, out);
		write_all(d.file_fd, out);
	}
	::close(d.read_fd);
	::close(d.file_fd);
	d.read_fd = d.file_fd = -1;
	return false;
}

void StampedOutput::run() {
	while (!stop_.load()) {
		std::vector<pollfd> fds;
		{
			std::lock_guard<std::mutex> lock(mu_);
			for (const Drain &d : drains_)
				fds.push_back({ d.read_fd, POLLIN, 0 });
		}
		if (fds.empty() || ::poll(fds.data(), fds.size(), 200) <= 0) {
			if (fds.empty())
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			continue;
		}
		std::lock_guard<std::mutex> lock(mu_);
		for (auto it = drains_.begin(); it != drains_.end();) {
			bool ready = false;
			for (const pollfd &p : fds)
				ready = ready || (p.fd == it->read_fd && p.revents != 0);
			if (ready && !pump(*it)) {
				it = drains_.erase(it);
				continue;
			}
			++it;
		}
	}
}

} // namespace opennova::hostd
