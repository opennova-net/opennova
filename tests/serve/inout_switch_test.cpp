// opennova-serve's `/INOUT` switch (D-NET-356): parsed in any case, handed to the host context
// with the protocol's _inout.txt log, which the socket open then turns on.
// [orig: Game_ParseCommandLineAndInit @0x4A743D..0x4A744F (g_InoutLogEnabled);
//  CNapiNetwork_OpenTransportSocket @0x4C6D02..0x4C6D15]
#include <base/resource_index/boot_policy.h>
#include <runtime/inmatch/server_log_devices.h>

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_console.h>

#include <cstdio>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova;

int main() {
	int failures = 0;
	const auto check = [&](bool ok, const char *what) {
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++failures;
		}
	};
	const LogSwitches sw = walk_retail_switches({"opennova-serve", "/inout"}).log_switches;
	check(sw.inout, "/INOUT in any case");
	LogSwitches off;
	{
		inmatch::ServerLogDevices devices;
		devices.arm(off);
		const inmatch::ServerLogs logs = devices.logs();
		check(logs.inout != nullptr && !logs.inout_switch, "the log exists, the switch unset");
		inmatch::NapiNPServerCtx ctx;
		test_titles::jo_config(ctx.config);
		ctx.logs = logs;
		inmatch::open_inout_log(ctx);
		check(!logs.inout->enabled(), "unset: the socket open leaves it off");
	}
	{
		inmatch::ServerLogDevices devices;
		devices.arm(sw);
		const inmatch::ServerLogs logs = devices.logs();
		check(logs.inout != nullptr && logs.inout_switch, "the switch reaches the host context");
		inmatch::NapiNPServerCtx ctx;
		test_titles::jo_config(ctx.config);
		ctx.logs = logs;
		inmatch::open_inout_log(ctx);
		check(logs.inout->enabled() &&
		              logs.inout->pending().find("NapiLog Started!\r\n") != std::string::npos,
		      "set: the socket open turns it on with its start line");
	}
	if (failures != 0) return 1;
	std::printf("inout_switch: ok\n");
	return 0;
}
