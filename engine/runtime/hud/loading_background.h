#pragma once

#include <functional>
#include <string>

// The mission loading screen's background pick and the SP start-mission splash
// gate its custom-background flag drives (the session band's right edge, the
// flag's other reader, is loading_screen.h's loading_band_right, with the
// layout values and the sidecar name rule).
// [orig: Render_LoadingScreen @0x521d10; Game_StartMission @0x524360]
// Witness record: docs/interface/loading-screen-re.md.

namespace opennova::hud {

// The image the screen loads and whether it is the mission's own sidecar (the
// flag the band and the splash read).
struct LoadingBackground {
	std::string image;
	bool custom = false;
};

// The mission's sidecar image (loading_sidecar_image_name) when the file
// probe finds it, else the stock loadscrn.pcx; `file_exists` is the embedder's
// probe (the UI image lookup, loose files first)
// [orig: Render_LoadingScreen -- the sidecar name @0x521dab, the probe
//  FileSystem_FileExists @0x521db5, g_LoadScreenHasCustomBg = 1 @0x521e92..0x521e94;
//  the miss clears it @0x521dcc and takes "loadscrn.pcx" @0x521e20 (both arms of
//  the g_MissionExitReason == 8 test end there)].
LoadingBackground loading_background(const std::string &mission_file,
		const std::function<bool(const std::string &)> &file_exists);

// The SP start-mission splash runs after the load when the screen holds a
// custom background and the load is not a session's
// [orig: Game_StartMission @0x525d38 (g_LoadScreenHasCustomBg) and @0x525d40
//  (g_NapiNPCtx.is_in_session) ahead of Game_ShowStartMissionSplash @0x525d48].
bool start_mission_splash_due(bool in_session, bool custom_background);

} // namespace opennova::hud
