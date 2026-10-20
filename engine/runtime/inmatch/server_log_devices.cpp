#include <runtime/inmatch/server_log_devices.h>

#include <base/io/os_path.h>

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

void ServerLogDevices::arm(const LogSwitches &switches) {
	if (!switches.profile_path.empty())
		profile_ = std::make_unique<ServerLogRecorder>(files_, switches.profile_path);
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
}

ServerLogs ServerLogDevices::logs() {
	ServerLogs logs;
	logs.profile = profile_.get();
	logs.punt = punt_.get();
	logs.net = &net_;
	logs.inout = &inout_;
	logs.inout_switch = inout_switch_;
	logs.files = &files_;
	return logs;
}

} // namespace opennova::inmatch
