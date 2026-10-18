// Where the logs are and a program's copy of its own (apps/novaworld_common/program_log.h):
//
//   - ONNET_LOG_STORE's two forms (a CloudWatch group prefix, a directory; a trailing '/'
//     dropped), none, and every malformed one refused;
//   - the tee in this very process: its stdout and stderr lines copied into
//     programs/<program>/<task>.log under a temp directory, each after its time, a line
//     the process never ended copied as it stops, the lines there once drained; the
//     descriptors back as they were after stop (a later line is not copied); a directory
//     it cannot make refused with nothing changed;
//   - its file refusing a write (a link to /dev/full): one line on the console's stderr
//     naming why, every line still on the console;
//   - its file slower than the lines (a FIFO nobody reads yet): the process's writes never
//     wait on it, every line on the console, the file's lines past what may wait dropped
//     and counted in its notes, each line once, in order.
//
// Database-free.

#include "program_log.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <base/io/file_io.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace program = opennova::novaworld_server::program;
namespace io = opennova::io;

namespace {

using Kind = program::LogStoreSetting::Kind;

std::string read_all(const std::string &path) {
	std::string text;
	std::string error;
	io::read_file_text(path, text, error);
	return text;
}

std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t eol = text.find('\n', at);
		out.push_back(text.substr(at, eol == std::string::npos ? std::string::npos : eol - at));
		if (eol == std::string::npos)
			break;
		at = eol + 1;
	}
	return out;
}

// "2026-10-18T14:05:03.123Z <text>": the line's text after a well-formed time; "" else.
std::string text_after_stamp(const std::string &line) {
	if (line.size() < 25 || line[4] != '-' || line[10] != 'T' || line[19] != '.' ||
			line[23] != 'Z' || line[24] != ' ')
		return {};
	return line.substr(25);
}

int test_parse() {
	program::LogStoreSetting s;
	std::string error;
	TEST_EXPECT(program::parse_log_store("cloudwatch:/opennova/prod", s, error));
	TEST_EXPECT(s.kind == Kind::CloudWatch && s.location == "/opennova/prod");
	TEST_EXPECT(program::parse_log_store(" file:///var/log/opennova/ ", s, error));
	TEST_EXPECT(s.kind == Kind::Directory && s.location == "/var/log/opennova");
	TEST_EXPECT(program::parse_log_store("", s, error) && s.kind == Kind::None);
	for (const char *bad : { "cloudwatch:opennova/prod", "cloudwatch:/", "cloudwatch:/a//b",
				 "cloudwatch:/a b", "file://var/log", "file:///var/../etc", "file:///",
				 "s3://bucket/logs", "/var/log/opennova" }) {
		error.clear();
		TEST_EXPECT(
				!program::parse_log_store(bad, s, error) && !error.empty() && s.kind == Kind::None);
	}
	TEST_EXPECT(
			program::program_log_path("web", "0a1b2c3d4e5f") == "programs/web/0a1b2c3d4e5f.log");
	std::printf("  ONNET_LOG_STORE: a group prefix, a directory, none; the malformed refused\n");
	return 0;
}

int test_tee() {
	test_temp::TempDir dir("program_log_tee");
	const std::string file = dir.root() + "/programs/lobby/c0ffee123456.log";
	{
		program::LogTee tee;
		std::string error;
		TEST_EXPECT(tee.start(dir.root(), "lobby", "c0ffee123456", error));
		std::printf("[boot] ready. Ctrl+C to stop.\n");
		std::fprintf(stderr, "[heartbeat] WARN a line on stderr\n");
		std::printf("two\nlines in one write\n");
		tee.drain(2000);
		const std::vector<std::string> drained = lines_of(read_all(file));
		TEST_EXPECT(drained.size() == 4);
		std::printf("a line never ended");
		tee.stop();
	}
	std::printf("\nafter the copy stopped\n");
	const std::vector<std::string> lines = lines_of(read_all(file));
	TEST_EXPECT(lines.size() == 5);
	std::vector<std::string> out;
	std::string err;
	for (const std::string &line : lines) {
		const std::string text = text_after_stamp(line);
		TEST_EXPECT(!text.empty());
		if (text.find("stderr") != std::string::npos)
			err = text;
		else
			out.push_back(text);
	}
	TEST_EXPECT(err == "[heartbeat] WARN a line on stderr");
	TEST_EXPECT(out.size() == 4 && out[0] == "[boot] ready. Ctrl+C to stop." && out[1] == "two" &&
			out[2] == "lines in one write" && out[3] == "a line never ended");

	// A directory it cannot make: refused, the descriptors untouched.
	const std::string blocker = dir.root() + "/blocker";
	std::string error;
	TEST_EXPECT(io::write_file_atomic(blocker, "a file, not a directory", error));
	program::LogTee refused;
	TEST_EXPECT(!refused.start(blocker, "web", "x", error) && !error.empty());
	std::printf("  the copy: both descriptors' lines after their times, an unended line at "
				"the stop, nothing after it, a directory it cannot make refused\n");
	return 0;
}

// This process's stdout and stderr into files of their own while it lives (a
// container's log, as the tee finds it), then back.
struct Console {
	std::string out_path;
	std::string err_path;
	int saved[2] = { -1, -1 };

	explicit Console(const std::string &dir) :
			out_path(dir + "/console.out"), err_path(dir + "/console.err") {
		const std::string *paths[2] = { &out_path, &err_path };
		for (int i = 0; i < 2; ++i) {
			saved[i] = ::dup(i + 1);
			const int fd =
					::open(paths[i]->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
			::dup2(fd, i + 1);
			::close(fd);
		}
	}
	~Console() {
		for (int i = 0; i < 2; ++i) {
			::dup2(saved[i], i + 1);
			::close(saved[i]);
		}
	}
};

int test_tee_write_error() {
	test_temp::TempDir dir("program_log_full");
	const std::string file = dir.root() + "/programs/web/full.log";
	std::error_code ec;
	std::filesystem::create_directories(dir.root() + "/programs/web", ec);
	std::filesystem::create_symlink("/dev/full", file, ec);
	TEST_EXPECT(!ec);
	bool started = false;
	{
		const Console console(dir.root());
		program::LogTee tee;
		std::string error;
		started = tee.start(dir.root(), "web", "full", error);
		std::printf("the first line\n");
		tee.drain(2000);
		std::printf("a line after the copy stopped\n");
		tee.stop();
	}
	TEST_EXPECT(started);
	TEST_EXPECT(read_all(dir.root() + "/console.out") ==
			"the first line\na line after the copy stopped\n");
	const std::vector<std::string> notes = lines_of(read_all(dir.root() + "/console.err"));
	TEST_EXPECT(notes.size() == 1 &&
			notes[0] ==
					"[log] the copy into " + file + " stopped: " + std::strerror(ENOSPC) +
							"; the console keeps every line");
	std::printf("  a file that refuses a write: one line on stderr naming why, the console "
				"goes on\n");
	return 0;
}

int test_tee_slow_file() {
	test_temp::TempDir dir("program_log_slow");
	const std::string file = dir.root() + "/programs/web/slow.log";
	std::error_code ec;
	std::filesystem::create_directories(dir.root() + "/programs/web", ec);
	TEST_EXPECT(::mkfifo(file.c_str(), 0600) == 0);
	// Its reader first, so the copy's open finds one; it reads nothing until every line is
	// written.
	const int fifo = ::open(file.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	TEST_EXPECT(fifo >= 0);
	// Three times what may wait for the file, and more: a FIFO holds 64 KiB.
	const std::string padding(100, 'x');
	const size_t total = 3 * program::LogTee::kMaxQueuedBytes / padding.size();
	bool started = false;
	std::string copied;
	{
		const Console console(dir.root());
		program::LogTee tee;
		std::string error;
		started = tee.start(dir.root(), "web", "slow", error);
		for (size_t i = 0; i < total; ++i)
			std::printf("line %zu %s\n", i, padding.c_str());
		// Now the file is read, to its end as the copy stops.
		std::thread reader([fifo, &copied] {
			::fcntl(fifo, F_SETFL, 0);
			char buf[64 * 1024];
			for (;;) {
				const ssize_t n = ::read(fifo, buf, sizeof(buf));
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					break;
				copied.append(buf, static_cast<size_t>(n));
			}
		});
		std::printf("the last line\n");
		tee.stop();
		reader.join();
	}
	::close(fifo);
	TEST_EXPECT(started);
	// The console has every line.
	const std::vector<std::string> console = lines_of(read_all(dir.root() + "/console.out"));
	TEST_EXPECT(console.size() == total + 1 && console.back() == "the last line");
	// The file: the lines it took in order, a note of those it dropped where they were (or
	// at its end), each line once.
	size_t lines = 0;
	size_t dropped = 0;
	size_t notes = 0;
	size_t next = 0;
	for (const std::string &line : lines_of(copied)) {
		const std::string text = text_after_stamp(line);
		TEST_EXPECT(!text.empty());
		if (text.rfind("[log] ", 0) == 0) {
			TEST_EXPECT(text.find(" not copied here: they came faster than the disk took "
								  "them (the console has them)") != std::string::npos);
			dropped += std::stoul(text.substr(6));
			++notes;
		} else if (text == "the last line") {
			++lines;
		} else {
			TEST_EXPECT(text.rfind("line ", 0) == 0);
			const size_t i = std::stoul(text.substr(5));
			TEST_EXPECT(i >= next && i < total);
			next = i + 1;
			++lines;
		}
	}
	TEST_EXPECT(notes >= 1 && dropped > 0 && lines + dropped == total + 1);
	std::printf("  a file slower than the lines: the console never waits, the file's lines "
				"past what may wait dropped and counted\n");
	return 0;
}

} // namespace

int main() {
	// As run_program writes: every printf reaches its descriptor at once.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "parse", test_parse },
		{ "tee", test_tee },
		{ "tee_write_error", test_tee_write_error },
		{ "tee_slow_file", test_tee_slow_file },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: where the logs are, and a program's copy of its own\n");
	return 0;
}
