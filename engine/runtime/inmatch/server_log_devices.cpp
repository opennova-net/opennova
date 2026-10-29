#include <runtime/inmatch/server_log_devices.h>

#include <base/io/os_path.h>
#include <base/title/title.h>
#include <base/version/one_build.h>
#include <net/novaworld/lobby_identity.h> // read_exe_basename
#include <net/protocol/protocol.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace opennova::inmatch {

std::string WorkingDirectoryFiles::path(const std::string &name) const {
	// A relative name resolves against the working directory, as the CRT's open of it does.
	if (dir_.empty() || io::os_path(name).is_absolute()) return name;
	const char last = dir_.back();
	return last == '/' || last == '\\' ? dir_ + name : dir_ + "/" + name;
}

bool WorkingDirectoryFiles::exists(const std::string &name) {
	std::error_code ec;
	return std::filesystem::is_regular_file(io::os_path(path(name)), ec);
}

void WorkingDirectoryFiles::remove(const std::string &name) {
	std::error_code ec;
	std::filesystem::remove(io::os_path(path(name)), ec);
}

bool WorkingDirectoryFiles::write(const std::string &name, std::string_view bytes, bool append) {
	std::FILE *f = io::fopen_utf8(path(name).c_str(), append ? "ab" : "wb");
	if (f == nullptr) return false;
	const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	return std::fclose(f) == 0 && ok;
}

ConnectLogHeader connect_log_header(std::string_view title_code) {
	std::string unwitnessed;
	const Title *title =
			resolve_title_protocol(title_code, unwitnessed) != nullptr ? title_by_code(title_code) : nullptr;
	if (title == nullptr) title = &default_title();
	ConnectLogHeader header;
	header.exe = read_exe_basename();
	header.title = title->title_text != nullptr ? title->title_text : "";
	// VERSION, g_VersionText: the engine build's, ONE's wire version text (punt_log.h).
	header.version = one_build().wire_version().retail_text();
	if (const Protocol *protocol = title_protocol(title->code)) {
		header.ver1 = protocol->ver1 != nullptr ? protocol->ver1 : "";
		header.ver2 = protocol->ver2 != nullptr ? protocol->ver2 : "";
	}
	return header;
}

void ServerLogDevices::arm(const LogSwitches &switches, const ConnectLogHeader &connect_header) {
	// [orig: Game_StartMission's test of g_RunningWithProfile alone @0x524475 before
	//  ServerLog_OpenForWrite(g_ProfileLogPath) @0x524487]
	if (switches.profile_path)
		profile_ = std::make_unique<ServerLogRecorder>(files_, *switches.profile_path);
	if (switches.punt_log || switches.cheat_log) {
		punt_ = std::make_unique<PuntLog>(files_);
		// [orig: the switch order in Game_ParseCommandLineAndInit - /CHEATLOG
		//  @0x4a77fc ahead of /PUNT.TXT @0x4a782d and /PUNTLOG @0x4a7863]
		if (switches.cheat_log) punt_->arm_cheat_log();
		if (switches.punt_log) punt_->arm_punt_log();
	}
	// CNapiNetwork_SetNetLogFile(1) for the switch, then again for the file
	// [orig: @0x4A7A23..0x4A7A29; Napi_FileExists("_DONETLOG.TXT") @0x4A7CBC..0x4A7CD3].
	if (switches.net_log) net_.set_enabled(true);
	if (files_.exists(kNetLogArmFileName)) net_.set_enabled(true);
	// g_InoutLogEnabled, which the socket open hands the protocol's log [orig: @0x4A744F].
	inout_switch_ = switches.inout;
	if (switches.connect_log) connect_.start(connect_header);
}

void ServerLogDevices::dump_punt_log() {
	write_punt_log_dump(files_, punt_ != nullptr ? punt_->records() : std::vector<PuntRecord>{});
}

ServerLogs ServerLogDevices::logs() {
	ServerLogs logs;
	logs.profile = profile_.get();
	logs.punt = punt_.get();
	logs.net = &net_;
	logs.inout = &inout_;
	logs.inout_switch = inout_switch_;
	logs.connect = &connect_;
	logs.files = &files_;
	return logs;
}

} // namespace opennova::inmatch
