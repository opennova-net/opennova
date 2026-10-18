// Where the logs are and a program's copy of its own (apps/novaworld_common/program_log.h):
//
//   - ONNET_LOG_STORE's two forms (a CloudWatch group prefix, a directory; a trailing '/'
//     dropped), none, and every malformed one refused;
//   - the tee in this very process: its stdout and stderr lines copied into
//     programs/<program>/<task>.log under a temp directory, each after its time, a line
//     the process never ended copied as it stops, the lines there once drained; the
//     descriptors back as they were after stop (a later line is not copied); a directory
//     it cannot make refused with nothing changed.
//
// Database-free.

#include "program_log.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <base/io/file_io.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

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
