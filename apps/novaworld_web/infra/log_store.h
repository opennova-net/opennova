#pragma once

// The logs the admin's pages read (log_routes.h, GET /api/admin/logs), from the
// store ONNET_LOG_STORE names (apps/novaworld_common/program_log.h):
//
//   CloudWatchLogStore  a deployment's groups under its prefix, through web's task
//                       role (aws_api.h): /servers (a hosted server's start, the
//                       stream its row names, <server id>/g<generation>), /machines
//                       (a machine's hostd, the stream its row names) and /service
//                       (a program's task, <program>/<program>/<task id>; or every
//                       task of it, that prefix). A known stream's newest lines are
//                       GetLogEvents; a text filter or a program's every task is
//                       FilterLogEvents over windows of time walking back from the
//                       newest. Each read is bounded: at most kMaxLimit lines, kMaxCalls
//                       calls and kDeadlineMs; a read like one within the last
//                       kCacheMs is answered from memory (a page's 5 s follow and
//                       several admins on one log cost AWS one read).
//   DirectoryLogStore   the local stack's directory: servers/<server id>/
//                       g<generation>.log, machines/<machine id>/hostd.log and
//                       programs/<program>/<task>.log, read backwards from their end,
//                       each line after its time where it has one
//                       ("2026-10-18T14:05:03.123Z <line>").
//
// What a read is of is resolved by the route from rows (a start's log stream, a
// machine's, a program's known task), never from the request's text: no request
// names a group, a stream or a path. A page is the newest `limit` lines older than
// its cursor, oldest first; its `before` is the cursor of the page before it, ""
// when nothing older is left. Cursors are the store's own, opaque to the page.
// Service policy, not a port.

#include "aws_api.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::infra {

struct LogLine {
	int64_t at_ms = -1; // Unix milliseconds; -1 when the line carries no time
	std::string text;
};

// What a read is of.
struct LogSource {
	enum class Subject { Server, Machine, Program };
	Subject subject = Subject::Server;
	int64_t id = 0; // the server's or the machine's
	int64_t generation = 0; // the server's start
	std::string program; // a program's name (one the service runs)
	std::string task; // a program's task (one it has run); "" for every task of it
	// The CloudWatch stream a row names: a start's log_stream, a machine's
	// agent_log_stream; "" when the row names none (nothing to read there yet).
	std::string stream;
};

struct LogQuery {
	LogSource source;
	std::string before; // an earlier page's cursor; "" for the newest lines
	size_t limit = 200; // 1..kMaxLimit
	std::string q; // only the lines holding this text (as it is); "" every line
};

struct LogPage {
	std::vector<LogLine> lines; // oldest first
	std::string before; // the page before this one's cursor; "" for none
	// Where the store read: a CloudWatch group, its directory; and the stream (or a
	// program's every task's prefix), the file. Both "" when the row names no stream
	// yet (nothing to read).
	std::string group;
	std::string stream;
	std::string error; // why the read failed (AWS's words; a file that does not read)
	bool bad_cursor = false; // `before` was none this store gave for this read
};

class LogStore {
public:
	static constexpr size_t kMaxLimit = 500;
	// A line longer than this is cut there.
	static constexpr size_t kMaxLineBytes = 16 * 1024;

	virtual ~LogStore() = default;
	// "cloudwatch" or "file", the replies' `source`.
	virtual const char *source() const = 0;
	virtual LogPage read(const LogQuery &query) = 0;
};

class CloudWatchLogStore : public LogStore {
public:
	// One read's bounds.
	static constexpr int kMaxCalls = 8;
	static constexpr int64_t kDeadlineMs = 10 * 1000;
	static constexpr int64_t kCacheMs = 3 * 1000;
	static constexpr size_t kCacheEntries = 64;
	// The first window FilterLogEvents reads back from the newest, each next one
	// four times as long, back to kHorizonMs (the longest the groups keep).
	static constexpr int64_t kFirstWindowMs = 15 * 60 * 1000;
	static constexpr int64_t kHorizonMs = 31LL * 24 * 60 * 60 * 1000;

	// Milliseconds: the wall clock (the events' times), and a steady one (the
	// deadline, the cache).
	using Clock = std::function<int64_t()>;

	// `prefix` the groups' ("/opennova/prod"). `runner` must outlive the store.
	CloudWatchLogStore(proc::CommandRunner &runner, AwsApiConfig aws, std::string prefix,
			Clock wall_ms = {}, Clock steady_ms = {});

	const char *source() const override { return "cloudwatch"; }
	LogPage read(const LogQuery &query) override;

private:
	LogPage read_uncached(const LogQuery &query);
	// A known stream's lines with no filter: GetLogEvents backwards.
	void read_stream(const AwsCredentials &creds, const std::string &group,
			const std::string &stream, const LogQuery &query, LogPage &page);
	// FilterLogEvents over `group`'s `stream` (or every stream under `prefix`).
	void read_filtered(const AwsCredentials &creds, const std::string &group,
			const std::string &stream, const std::string &prefix, const LogQuery &query,
			LogPage &page);

	AwsApi api_;
	const std::string prefix_;
	Clock wall_;
	Clock steady_;
	std::mutex mu_;
	struct Cached {
		int64_t at = 0;
		LogPage page;
	};
	std::map<std::string, Cached> cache_;
};

class DirectoryLogStore : public LogStore {
public:
	// The bytes one read scans of one file at most.
	static constexpr int64_t kScanBytes = 16 * 1024 * 1024;

	explicit DirectoryLogStore(std::string dir) : dir_(std::move(dir)) {}

	const char *source() const override { return "file"; }
	LogPage read(const LogQuery &query) override;

private:
	const std::string dir_;
};

// The store ONNET_LOG_STORE names: a directory's, or CloudWatch's through `runner`
// with the infrastructure view's region and task role (ONNET_INFRA_REGION,
// AWS_CONTAINER_CREDENTIALS_RELATIVE_URI) and its files under `scratch_dir`. Null
// with `why` (for the boot line and the route's refusal) when there is none, or
// when CloudWatch's settings do not read.
std::unique_ptr<LogStore> make_log_store(
		proc::CommandRunner &runner, const std::string &scratch_dir, std::string &why);

// The stamp the programs' copies put before each line ("2026-10-18T14:05:03.123Z ",
// program_log.h) read off `line`: its time in Unix milliseconds and the text after
// it; -1 and the whole line when it opens with none.
LogLine parse_stamped_line(std::string_view line);

} // namespace opennova::novaworld_server::infra
