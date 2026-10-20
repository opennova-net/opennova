#pragma once

// The server log devices retail's command-line switches arm (base/resource_index/boot_policy.h
// LogSwitches), alive for the whole process as retail's globals: the .sph server log, the punt
// and cheat logs, the protocol manager's net log and the protocol's _inout.txt log, every file
// in the working directory. The game's host and opennova-serve's dedicated host hand the same
// devices to each session (ServerLogs, server_files.h). The writers are the engine's
// (server_log_recorder.h, punt_log.h, server_console.h's NapiLog).

#include <base/resource_index/boot_policy.h> // LogSwitches
#include <runtime/inmatch/punt_log.h>
#include <runtime/inmatch/server_console.h> // NapiLog
#include <runtime/inmatch/server_files.h>
#include <runtime/inmatch/server_log_recorder.h>

namespace opennova {
struct TitleIdentity;
}

#include <cstdint>
#include <memory>
#include <string>

namespace opennova::inmatch {

// The process's working directory, where retail keeps its logs: every name resolves against
// `dir` (empty: the process's own current directory, as retail opens each log by its bare name).
class WorkingDirectoryFiles final : public ServerFileSink {
public:
	explicit WorkingDirectoryFiles(std::string dir = {}) : dir_(std::move(dir)) {}
	bool exists(const std::string &name) override;
	void remove(const std::string &name) override;
	bool write(const std::string &name, std::string_view bytes, bool append) override;
	const std::string &dir() const { return dir_; }

private:
	std::string path(const std::string &name) const;
	std::string dir_;
};

// The _connectlog.txt header of a process speaking `title` (null: JO's): the running
// executable's basename, the title's version text, its TITLE and its version pair.
ConnectLogHeader connect_log_header(const TitleIdentity *title);

// The devices the switches arm. The net log is the protocol manager's, which the process
// always has.
class ServerLogDevices {
public:
	explicit ServerLogDevices(std::string dir = {}) : files_(std::move(dir)) {}
	ServerLogDevices(const ServerLogDevices &) = delete;
	ServerLogDevices &operator=(const ServerLogDevices &) = delete;

	// Arm what the switches name: the punt and cheat logs write their START lines now, as
	// retail's switch parse does; `/NETLOG`, then a `_DONETLOG.TXT` in the working directory,
	// turn the net log on.
	// `/CONNECTLOG` starts _connectlog.txt with `connect_header` after the switch walk
	// [orig: Game_ParseCommandLineAndInit @0x4A7E37].
	void arm(const LogSwitches &switches, const ConnectLogHeader &connect_header = {});
	// The process's network shut down (the game world's exit from the tree, opennova-serve's
	// exit tail): the protocol's _inout.txt log, then the manager's net
	// log, each stops and flushes [orig: the protocol's NapiLog_Shutdown(protocol + 0xF80) in
	// NapiNPProtocol_Destroy @0x62B500 (the call @0x62B570); Game_ShutdownSubsystems @0x4A5398
	// -> NapiLib_Shutdown -> the manager's NapiLog_Shutdown @0x62B739]. `now_ms` is the host's
	// clock.
	void close(uint32_t now_ms) {
		inout_.shutdown(now_ms);
		net_.shutdown(now_ms);
	}
	// The devices for the host context (null = off).
	ServerLogs logs();
	ServerLogRecorder *profile() { return profile_.get(); }
	PuntLog *punt() { return punt_.get(); }
	ConnectLog &connect() { return connect_; }
	WorkingDirectoryFiles &files() { return files_; }

private:
	WorkingDirectoryFiles files_;
	std::unique_ptr<ServerLogRecorder> profile_;
	std::unique_ptr<PuntLog> punt_;
	NapiLog net_{files_, kNetLogFileName};
	// The protocol's _inout.txt log, which `/INOUT` turns on at the socket open.
	NapiLog inout_{files_, kInoutLogFileName};
	bool inout_switch_ = false;
	ConnectLog connect_{files_};
};

} // namespace opennova::inmatch
