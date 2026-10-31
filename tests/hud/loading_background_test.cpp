// The loading screen's background pick and the splash gate that reads its custom
// flag (runtime/hud/loading_background.h): the sidecar when the probe finds it,
// else loadscrn.pcx; the SP splash gate (the band's edge, the flag's other
// reader, is the title's: loading_screen_test).
// [orig: Render_LoadingScreen @0x521db5..0x521e94, @0x521e20;
//  Game_StartMission @0x525d38..0x525d48]

#include <runtime/hud/loading_background.h>

#include <cstdio>
#include <set>
#include <string>

namespace {

namespace hud = opennova::hud;

int failures = 0;

void expect(bool cond, const char *msg) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
}

} // namespace

int main() {
	std::set<std::string> probed;
	const auto has_00trg = [&](const std::string &name) {
		probed.insert(name);
		return name == "00TRg.pcx";
	};
	hud::LoadingBackground bg = hud::loading_background("00TRg.bms", has_00trg);
	expect(bg.image == "00TRg.pcx" && bg.custom, "a found sidecar is the custom background");
	expect(probed.count("00TRg.pcx") == 1, "the probe asks for the sidecar name");
	bg = hud::loading_background("OTHER.bms", has_00trg);
	expect(bg.image == "loadscrn.pcx" && !bg.custom, "a missing sidecar falls back to the stock image");
	bg = hud::loading_background("00TRg.bms", nullptr);
	expect(bg.image == "loadscrn.pcx" && !bg.custom, "no probe finds nothing");
	probed.clear();
	bg = hud::loading_background("", has_00trg);
	expect(bg.image == "loadscrn.pcx" && !bg.custom && probed.count(".pcx") == 1,
			"no mission file probes the bare extension, then falls back");

	expect(hud::start_mission_splash_due(false, true), "SP over a custom background splashes");
	expect(!hud::start_mission_splash_due(true, true), "a session load never splashes");
	expect(!hud::start_mission_splash_due(false, false), "the stock background never splashes");
	expect(!hud::start_mission_splash_due(true, false), "neither leg");

	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_loading_background: ok\n");
	return 0;
}
