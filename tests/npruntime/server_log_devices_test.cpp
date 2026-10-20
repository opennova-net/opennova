// npruntime_server_log_devices: the server log switches every host the process runs honours
// (net parity lane L8, L8-s1's FOUND 5): the game's launch-flag walk reads retail's switches
// (/PROFILE <path>, /PUNT.TXT, /PUNTLOG, /CHEATLOG, /NETLOG, /INOUT, any case; /PROFILE steps
// onto its value token, which the walk's later tests then read), and the devices they arm
// write their files in the process's
// working directory: the game's --working-dir when it gives one, where retail opens each log by
// its bare name. The game world arms one set at its start and hands it to every hosted
// session; opennova-serve arms its own from the same parse.
// [orig: Game_ParseCommandLineAndInit @0x4a778f (/PROFILE, its value @0x4a77a1..0x4a77b9),
//  @0x4a782d (/PUNT.TXT), @0x4a7863 (/PUNTLOG), @0x4a77fc (/CHEATLOG), @0x4a7a11 (/NETLOG,
//  @0x4A7A23..0x4A7A29), @0x4a743d..0x4a744f (/INOUT); Napi_FileExists("_DONETLOG.TXT")
//  @0x4A7CBC..0x4A7CD3]

#include <base/resource_index/boot_policy.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/punt_log.h>
#include <runtime/inmatch/server_log_devices.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace inmatch = opennova::inmatch;

int failures = 0;
void expect(bool cond, const char *what) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++failures;
}

fs::path fresh_dir() {
	const fs::path dir = fs::temp_directory_path() /
			("opennova_log_devices_" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(dir);
	return dir;
}

void run_the_launch_walk_reads_the_switches() {
	const opennova::LaunchFlags f = opennova::parse_launch_flags(
			{"--lan-host", "x.bms", "/puntlog", "/Profile", "/NETLOG", "/inout", "/CHEATLOG"});
	expect(f.log_switches.punt_log && f.log_switches.cheat_log && f.log_switches.inout,
	       "the punt, cheat and inout switches in any case");
	expect(f.log_switches.profile_path == "/NETLOG" && f.log_switches.net_log,
	       "a /PROFILE's value token is its path, and the walk's later tests read it too");
	const opennova::LaunchFlags none = opennova::parse_launch_flags({"--lan-host", "x.bms"});
	expect(none.log_switches.profile_path.empty() && !none.log_switches.punt_log &&
	               !none.log_switches.net_log && !none.log_switches.inout,
	       "no switch arms nothing");
	const opennova::LaunchFlags tail = opennova::parse_launch_flags({"/NETLOG", "/PROFILE"});
	expect(tail.log_switches.net_log && tail.log_switches.profile_path.empty(),
	       "a /PROFILE with no value takes none");
}

void run_the_devices_write_in_the_working_directory() {
	const fs::path dir = fresh_dir();
	{
		inmatch::ServerLogDevices devices(dir.string());
		opennova::LogSwitches sw;
		sw.net_log = true;
		devices.arm(sw);
		const inmatch::ServerLogs logs = devices.logs();
		expect(logs.net != nullptr && logs.net->enabled(), "/NETLOG turns the net log on");
		logs.net->write("a line");
		devices.close(5000);
	}
	expect(fs::is_regular_file(dir / inmatch::kNetLogFileName),
	       "the net log lands in the working directory");

	const fs::path armed = fresh_dir();
	{
		std::ofstream(armed / inmatch::kNetLogArmFileName) << "";
		inmatch::ServerLogDevices devices(armed.string());
		devices.arm(opennova::LogSwitches{});
		expect(devices.logs().net->enabled(), "a _DONETLOG.TXT there turns it on unswitched");
	}
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::remove_all(armed, ec);
}

// _connectlog.txt (D-NET-NEW-L8-8): off by default; /CONNECTLOG on, /NOCONNECTLOG off, the
// last one read winning; the start deletes the file and writes the STARTED line and the EXE,
// VERSION, TITLE, VER1 and VER2 lines; every line is "<time> : <text>" CRLF; the server's
// first new connection writes its disabling line and turns the log off for the process.
// [orig: g_ConnectLogEnabled @0xB4C720 (0); Game_ParseCommandLineAndInit @0x4A74AF / @0x4A74C9,
//  @0x4A7E37..0x4A7F8A; NapiFile_AppendLine @0x61C020; NapiNPServer_HandleNewConnection
//  @0x4C8054..0x4C80AF; CNapiNetwork_OpenTransportSocket @0x4C6BA0..0x4C6BD7]
void run_the_connect_log() {
	expect(!opennova::parse_launch_flags({}).log_switches.connect_log, "off by default");
	expect(opennova::parse_launch_flags({"/connectlog"}).log_switches.connect_log,
	       "/CONNECTLOG in any case turns it on");
	expect(!opennova::parse_launch_flags({"/CONNECTLOG", "/NoConnectLog"}).log_switches.connect_log &&
	               opennova::parse_launch_flags({"/NOCONNECTLOG", "/CONNECTLOG"}).log_switches.connect_log,
	       "the last of the two wins");
	const fs::path dir = fresh_dir();
	std::ofstream(dir / inmatch::kConnectLogFile) << "stale";
	{
		inmatch::ServerLogDevices devices(dir.string());
		devices.connect().set_clock([] { return std::string("Tue Oct 20 12:00:00 2026"); });
		opennova::LogSwitches sw;
		sw.connect_log = true;
		inmatch::ConnectLogHeader header;
		header.exe = "Jointops.exe";
		header.version = "V1.7.5.7";
		header.title = "Joint Operations: Typhoon Rising";
		header.ver1 = "3";
		header.ver2 = "2345";
		devices.arm(sw, header);
		inmatch::ConnectLog *log = devices.logs().connect;
		expect(log != nullptr && log->enabled(), "the start turns it on");
		if (log != nullptr) {
			log->line("NET SETTING UP SOCKET....");
			log->disable_at_server_connection();
			expect(!log->enabled(), "the server's first connection turns it off");
			log->line("NET SOCKET SETUP [0.0.0.0:32768]");
		}
	}
	std::ifstream in(dir / inmatch::kConnectLogFile, std::ios::binary);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const std::string t = "Tue Oct 20 12:00:00 2026 : ";
	expect(text == t + "* * * * CONNECT LOG STARTED * * * *\r\n" +
	                       t + "EXE = \"Jointops.exe\"\r\n" +
	                       t + "VERSION = \"V1.7.5.7\"\r\n" +
	                       t + "TITLE = \"Joint Operations: Typhoon Rising\"\r\n" +
	                       t + "VER1 = \"3\"\r\n" +
	                       t + "VER2 = \"2345\"\r\n" +
	                       t + "NET SETTING UP SOCKET....\r\n" +
	                       t + "NET SERVER DISABLING CONNECTLOG BECAUSE WE DON'T NEED IT ANYMORE.\r\n",
	       "the header, a line, the disabling line, and nothing after it");
	in.close();
	std::error_code ec;
	fs::remove_all(dir, ec);
}

} // namespace

int main() {
	run_the_launch_walk_reads_the_switches();
	run_the_devices_write_in_the_working_directory();
	run_the_connect_log();
	if (failures == 0) std::printf("npruntime_server_log_devices: all passed\n");
	return failures == 0 ? 0 : 1;
}
