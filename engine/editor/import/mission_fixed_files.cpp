#include <editor/import/mission_fixed_files.h>

#include <cstdint>
#include <set>

#include <editor/assets/asset_registry.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/environment/environment_state.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/mission/promote.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/renderer/tracer_frame.h>
#include <runtime/renderer/water_wake_frame.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/teammate_operations.h>

namespace opennova::editor {

namespace {

std::vector<MissionFixedFile> collect() {
	std::vector<MissionFixedFile> out;
	std::set<std::string> seen;
	const auto add = [&out, &seen](const std::string &name, const char *what) {
		if (!name.empty() && seen.insert(pff::normalized_logical_name(name)).second) out.push_back({ name, what });
	};
	for (const hud::HudFixedTexture &texture : hud::kHudFixedTextures) add(texture.name, "for the HUD");
	add(hud::kHudCargoFlagTexture, "for the HUD's carried flag");
	add(hud::kHudCargoDocumentTexture, "for the HUD's carried item");
	for (int style = hud::kHudCrosshairStyleMin; style <= hud::kHudCrosshairStyleMax; ++style)
		add(hud::hud_crosshair_texture_name(style), "for a crosshair style");
	for (const char *name : hud::kViewEffectTextureNames) add(name, "for the binoculars, the night vision and the damage vignette");
	add(renderer::kRainTexture, "for rain");
	add(renderer::kSnowTexture, "for snow");
	add(renderer::kEmitterPoolTexture, "for smoke trails");
	add(renderer::kWakeTexture, "for the water rings");
	add(renderer::kWakeGradientTexture, "for the water rings");
	for (int strip = 0; strip < world::kScarTextureStripCount; ++strip)
		add(world::scar_texture_strip_name(strip), "for an impact scar");
	for (int index = 0; index <= UINT8_MAX; ++index)
		add(std::string(terrain::terrain_scorch_texture_name(static_cast<uint8_t>(index))), "for a scorch mark");
	add(env::kOvercastFile, "for the overcast sky");
	add(std::string(mission::kFallbackAiProfile) + ".aip", "for a vehicle that names no AI profile");
	add(mission::kTeammateHelicopterAiProfile, "for the teammates' helicopter");
	add(anim::kDefaultAdmName, "for a weapon whose animation map is missing");
	add(world::kPickupCallWave, "for a teammate's call when a pickup lands");
	return out;
}

} // namespace

const std::vector<MissionFixedFile> &mission_fixed_files() {
	static const std::vector<MissionFixedFile> files = collect();
	return files;
}

} // namespace opennova::editor
