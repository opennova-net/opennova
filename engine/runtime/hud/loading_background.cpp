#include <runtime/hud/loading_background.h>

#include <runtime/hud/loading_screen.h>

namespace opennova::hud {

LoadingBackground loading_background(const std::string &mission_file,
		const std::function<bool(const std::string &)> &file_exists) {
	// The sidecar when the probe finds it, else the stock image
	// [orig: Render_LoadingScreen @0x521db5..0x521e94, the fallback @0x521e20].
	LoadingBackground out;
	const std::string sidecar = loading_sidecar_image_name(mission_file);
	if (file_exists && file_exists(sidecar)) {
		out.image = sidecar;
		out.custom = true;
		return out;
	}
	out.image = kLoadingFallbackImage;
	return out;
}

int loading_band_right(bool custom_background) {
	return custom_background ? kLoadingBandRightCustom : kLoadingBandRightStock;
}

bool start_mission_splash_due(bool in_session, bool custom_background) {
	// [orig: Game_StartMission @0x525d38..0x525d48]
	return custom_background && !in_session;
}

} // namespace opennova::hud
