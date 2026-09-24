// mission::script_debug_report (the F3 Script window's record) over the
// synthetic mission: the WAC program and VM state, the per-event fired
// state, the BMS events, the variable banks the script writes, and the
// first compile error (the retail script-state page's line) for a script
// that does not compile; and the local player report beside it.
#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/script_debug_report.h>
#include <runtime/world/inspect_local_player.h>

#include <cstdio>
#include <map>
#include <string>

using namespace opennova;

static int failures = 0;
#define CHECK(c) \
	do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

bool boot(mission::MissionKernel &kernel, std::map<std::string, std::string> &files) {
	kernel.open_document(test_mission::synthetic_mission(), "synth", test_boot::source_over(&files));
	std::string error;
	mission::KernelBootOptions options;
	if (!kernel.boot(options, error)) {
		std::printf("boot failed: %s\n", error.c_str());
		return false;
	}
	return true;
}

}  // namespace

int main() {
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if elapse(1) then set(v1,1) endif\nif elapse(2) then inc(v1) endif\n";
		mission::MissionKernel kernel;
		CHECK(boot(kernel, files));
		inmatch::LocalRole role;
		role.bind(kernel);
		for (int i = 0; i < 200; ++i) role.run_tick(inmatch::TickInput{});

		const mission::ScriptDebugReport r = mission::script_debug_report(kernel);
		CHECK(r.wac_loaded);
		CHECK(r.first_error.empty());
		CHECK(r.code_words > 0);
		CHECK(r.wac_events.size() == 2);
		CHECK(r.wac_events.size() == 2 && r.wac_events[0].ever_fired && r.wac_events[0].fired_count > 0);
		CHECK(r.mission_vars[1] != 0);            // the script wrote V1
		CHECK(r.bms_events.size() == 1);          // the synthetic mission's one event
		CHECK(r.dispatch_count > 0);
		CHECK(r.wac_initial_executed);

		world::inspect::LocalPlayerReport player;
		const bool has_player = world::inspect::local_player_report(kernel.world, kernel.local, &kernel.collision, player);
		CHECK(has_player == kernel.local.has_local_player());
		CHECK(!has_player || player.valid);
	}
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if elapse(1) then nosuchcommand(v1) endif\n";
		mission::MissionKernel kernel;
		boot(kernel, files);  // a script that fails to compile still boots the world
		const mission::ScriptDebugReport r = mission::script_debug_report(kernel);
		CHECK(!r.first_error.empty());
		CHECK(!r.diagnostics.empty());            // the page's line is the first diagnostic
		CHECK(r.first_error.find("synth.wac") != std::string::npos);
	}
	std::printf("script_debug_report: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
