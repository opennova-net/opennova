#pragma once

// The process runtime's copy of each server's output into the log directory
// (--log-dir, the local stack's log store), each line after the time it was
// written ("2026-10-18T14:05:03.123Z <line>", as the programs' copies are,
// apps/novaworld_common/program_log.h), so the site's log view reads a time for
// every line. A server's output goes into a pipe of its own, which one thread
// drains into the server's file; a line not ended by the server's exit is
// written then. Linux only, as hostd. Platform plumbing, not a port.

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace opennova::hostd {

// "2026-10-18T14:05:03.123Z": the wall clock now, as each copied line opens.
std::string line_stamp();

// `text`'s ended lines, each after `stamp` and a space, into `out`; what is
// left unended is kept in `partial` for the next.
void stamp_lines(
		std::string_view text, const std::string &stamp, std::string &partial, std::string &out);

class StampedOutput {
public:
	StampedOutput() = default;
	// Every pipe drained and closed, the thread joined.
	~StampedOutput();
	StampedOutput(const StampedOutput &) = delete;
	StampedOutput &operator=(const StampedOutput &) = delete;

	// A pipe whose lines go to `path` (appended, made owner-only with its
	// directories): the path a child's output is opened at
	// (/proc/self/fd/<the pipe's write end>, which the spawn opens before it
	// forks) in `child_path`, and the write end in `write_fd`, which the caller
	// closes once the child holds it. False with `error`.
	bool open(const std::string &path, std::string &child_path, int &write_fd, std::string &error);

private:
	struct Drain {
		int read_fd = -1;
		int file_fd = -1;
		std::string partial;
	};
	void run();
	// What a read of `d` found; false at its end (the server's exit), its last
	// line written and its descriptors closed.
	static bool pump(Drain &d);

	std::mutex mu_;
	std::vector<Drain> drains_;
	std::atomic<bool> stop_{ false };
	std::thread thread_;
};

} // namespace opennova::hostd
