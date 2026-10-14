#pragma once

// opennova-serve's log devices (ADR 0051 PR7): the working-directory file seam
// and the logs retail's switches arm. The writers are the engine's
// (inmatch/server_log_recorder.h, inmatch/punt_log.h); this file owns the
// switch spellings and the directory.

#include <runtime/inmatch/punt_log.h>
#include <runtime/inmatch/server_console.h> // NetLog
#include <runtime/inmatch/server_files.h>
#include <runtime/inmatch/server_log_recorder.h>

#include <memory>
#include <string>
#include <vector>

namespace opennova::serve {

// The server's working directory, where retail's dedicated host keeps its
// logs: every name resolves against it.
class WorkingDirectoryFiles final : public inmatch::ServerFileSink {
public:
	bool exists(const std::string &name) override;
	void remove(const std::string &name) override;
	bool write(const std::string &name, std::string_view bytes, bool append) override;
};

// The log switches in retail's spellings, matched without regard to case:
// `/PROFILE <path>` the .sph server log, `/PUNT.TXT` or `/PUNTLOG` the punt
// log, `/CHEATLOG` the cheat log, `/NETLOG` the net log on from the start
// [orig: Game_ParseCommandLineAndInit @0x4a778f (/PROFILE), @0x4a782d
// (/PUNT.TXT), @0x4a7863 (/PUNTLOG), @0x4a77fc (/CHEATLOG), @0x4a7a11 (/NETLOG)].
struct LogSwitches {
	std::string profile_path;
	bool punt_log = false;
	bool cheat_log = false;
	bool net_log = false;
};

// Parse one log switch at args[i]: the arguments it took (1, or 2 with its
// value), 0 when args[i] is not a log switch, -1 when a value is missing
// (`error` says why).
int parse_log_switch(const std::vector<std::string> &args, size_t i, LogSwitches &out,
		std::string &error);

// The devices the switches arm, alive for the whole run as retail's globals.
// The net log is the protocol manager's, which the process always has.
class ServerLogDevices {
public:
	// Arm what the switches name: the punt and cheat logs write their START
	// lines now, as retail's switch parse does; `/NETLOG`, then a
	// `_DONETLOG.TXT` in the working directory, turn the net log on.
	void arm(const LogSwitches &switches);
	// The process's network shut down: the net log stops and flushes
	// [orig: Game_ShutdownSubsystems @0x4A5398 -> NapiLib_Shutdown -> the
	//  manager's NapiLog_Shutdown @0x62B739]. `now_ms` is the host's clock.
	void close(uint32_t now_ms) { net_.shutdown(now_ms); }
	// The devices for the host context (null = off).
	inmatch::ServerLogs logs();
	inmatch::ServerLogRecorder *profile() { return profile_.get(); }
	inmatch::PuntLog *punt() { return punt_.get(); }

private:
	WorkingDirectoryFiles files_;
	std::unique_ptr<inmatch::ServerLogRecorder> profile_;
	std::unique_ptr<inmatch::PuntLog> punt_;
	inmatch::NetLog net_{files_};
};

} // namespace opennova::serve
