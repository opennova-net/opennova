#include "server_logs.h"

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace opennova::serve {

bool WorkingDirectoryFiles::exists(const std::string &name) {
	std::error_code ec;
	return std::filesystem::is_regular_file(io::os_path(name), ec);
}

void WorkingDirectoryFiles::remove(const std::string &name) {
	std::error_code ec;
	std::filesystem::remove(io::os_path(name), ec);
}

bool WorkingDirectoryFiles::write(const std::string &name, std::string_view bytes, bool append) {
	std::FILE *f = io::fopen_utf8(name.c_str(), append ? "ab" : "wb");
	if (f == nullptr) return false;
	const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	return std::fclose(f) == 0 && ok;
}

int parse_log_switch(const std::vector<std::string> &args, size_t i, LogSwitches &out,
		std::string &error) {
	const std::string &a = args[i];
	if (strutil::iequals(a, "/PROFILE")) {
		// The next token is the path [orig: @0x4a77a1..0x4a77b9].
		if (i + 1 >= args.size()) {
			error = a + " needs a value";
			return -1;
		}
		out.profile_path = args[i + 1];
		return 2;
	}
	if (strutil::iequals(a, "/PUNT.TXT") || strutil::iequals(a, "/PUNTLOG")) {
		out.punt_log = true;
		return 1;
	}
	if (strutil::iequals(a, "/CHEATLOG")) {
		out.cheat_log = true;
		return 1;
	}
	return 0;
}

void ServerLogDevices::arm(const LogSwitches &switches) {
	if (!switches.profile_path.empty())
		profile_ = std::make_unique<inmatch::ServerLogRecorder>(files_, switches.profile_path);
	if (switches.punt_log || switches.cheat_log) {
		punt_ = std::make_unique<inmatch::PuntLog>(files_);
		// [orig: the switch order in Game_ParseCommandLineAndInit — /CHEATLOG
		//  @0x4a77fc ahead of /PUNT.TXT @0x4a782d and /PUNTLOG @0x4a7863]
		if (switches.cheat_log) punt_->arm_cheat_log();
		if (switches.punt_log) punt_->arm_punt_log();
	}
}

inmatch::ServerLogs ServerLogDevices::logs() {
	inmatch::ServerLogs logs;
	logs.profile = profile_.get();
	logs.punt = punt_.get();
	return logs;
}

} // namespace opennova::serve
