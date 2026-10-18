#pragma once

// Where the deployment's logs are, and a program's own copy of its lines into the
// local stack's store. ONNET_LOG_STORE names the store, one of:
//
//   cloudwatch:/opennova/<deployment>   a deployment's CloudWatch Logs groups under that
//                                       prefix: /service (the programs, through ECS's
//                                       awslogs driver), /machines (hostd) and /servers
//                                       (the hosted servers); unset off ECS
//   file:///var/log/opennova            a directory (the local stack's `logs` volume):
//                                       servers/<server id>/g<generation>.log and
//                                       machines/<machine id>/hostd.log (hostd's
//                                       --log-dir), programs/<program>/<task>.log (each
//                                       program's LogTee, below)
//
// Web reads it (apps/novaworld_web/infra/log_store.h: the admin's log view); every
// program reads it for its LogTee. Unset: no store (web's log view says so, no copy).
//
// A program with a directory store copies everything it writes to stdout and stderr,
// line by line with the time it was written ("2026-10-18T14:05:03.123Z <line>"), into
// programs/<program>/<task>.log, its task the hostname (program_usage.h
// local_task_id): its two descriptors become pipes, which a thread drains into the
// descriptors they were and into the file, so the container's own log (docker logs)
// keeps every line as before. A deployment's store is CloudWatch, which ECS's driver
// fills: no copy there. Service policy, not a port.

#include <atomic>
#include <string>
#include <string_view>
#include <thread>

namespace opennova::novaworld_server::program {

// The environment variable naming the store.
inline constexpr const char *kLogStoreVariable = "ONNET_LOG_STORE";

struct LogStoreSetting {
	enum class Kind { None, CloudWatch, Directory };
	Kind kind = Kind::None;
	// CloudWatch: the groups' prefix ("/opennova/prod", no trailing '/');
	// Directory: the absolute directory ("/var/log/opennova", no trailing '/').
	std::string location;
};

// `text` read into `out`: "" is no store; false with `error` when it is neither form
// (a prefix of '/'-led group-name characters; an absolute path with no '..').
bool parse_log_store(std::string_view text, LogStoreSetting &out, std::string &error);

// ONNET_LOG_STORE's; a malformed one is no store, with `error` saying why.
LogStoreSetting log_store_from_env(std::string &error);

// A program task's file under a directory store: programs/<program>/<task>.log.
std::string program_log_path(std::string_view program, std::string_view task);

// The copy of this process's stdout and stderr into a directory store's file. One per
// process (run_program's); not on Windows.
class LogTee {
public:
	LogTee() = default;
	~LogTee() { stop(); }
	LogTee(const LogTee &) = delete;
	LogTee &operator=(const LogTee &) = delete;

	// Opens <dir>/programs/<program>/<task>.log for appending (its directories made)
	// and puts the pipes in place: false with `error`, nothing changed, when it cannot.
	bool start(const std::string &dir, std::string_view program, std::string_view task,
			std::string &error);
	// The descriptors put back, every line read copied, the threads joined.
	void stop();
	// Waits (at most `timeout_ms`) for every line written so far to be copied: before an
	// exit that runs no destructor (the watchdog's).
	void drain(int timeout_ms);

private:
	struct Stream {
		int fd = -1; // the descriptor it stands in for (1, 2)
		int read_fd = -1; // the pipe's
		int out_fd = -1; // what `fd` was
		std::string partial; // a line not yet ended
	};

	// The reader: both pipes polled until both close.
	void pump();
	void copy_lines(Stream &s, std::string_view chunk, bool flush);

	int file_fd_ = -1;
	Stream streams_[2];
	std::thread reader_;
	std::atomic<bool> busy_{ false }; // the reader is copying what it read
	bool running_ = false;
};

// The process's LogTee while it runs (run_program sets it), or null; drain_log_tee
// drains it, if any.
void set_log_tee(LogTee *tee);
void drain_log_tee(int timeout_ms);

} // namespace opennova::novaworld_server::program
