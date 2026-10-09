#include <runtime/assets/mission_fixed_files.h>

#include <cstdint>
#include <set>
#include <string>

#include <formats/env/env.h>
#include <formats/pff/pff.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/mission/promote.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/renderer/tracer_frame.h>
#include <runtime/renderer/water_wake_frame.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/teammate_operations.h>

namespace opennova::assets {

namespace {

// The name as an archive keys it (pff_norm_name's rule over a buffer of the name's own length plus
// its terminator, so nothing past a fixed size is cut off).
std::string archive_key(const std::string &name) {
	std::string key(name.size() + 1, '\0');
	pff::pff_norm_name(name.data(), name.size(), &key[0], key.size());
	key.resize(std::char_traits<char>::length(key.c_str()));
	return key;
}

std::vector<MissionFixedFile> collect() {
	std::vector<MissionFixedFile> out;
	std::set<std::string> seen;
	const auto add = [&out, &seen](const std::string &name, const char *what) {
		if (!name.empty() && seen.insert(archive_key(name)).second) out.push_back({ name, what });
	};
	// [orig: HUD_LoadAllTextures @0x59dda0; the carried flag and item @0x59de53, @0x59de64; the
	// crosshair styles @0x59e3d6]
	for (const hud::HudFixedTexture &texture : hud::kHudFixedTextures) add(texture.name, "for the HUD");
	add(hud::kHudCargoFlagTexture, "for the HUD's carried flag");
	add(hud::kHudCargoDocumentTexture, "for the HUD's carried item");
	for (int style = hud::kHudCrosshairStyleMin; style <= hud::kHudCrosshairStyleMax; ++style)
		add(hud::hud_crosshair_texture_name(style), "for a crosshair style");
	// [orig: ViewFx_InitShadersAndTextures @0x5cf8e0]
	for (const char *name : hud::kViewEffectTextureNames) add(name, "for the binoculars, the night vision and the damage vignette");
	// [orig: WeatherParticle_LoadTextures @ 0x5de840]
	add(renderer::kRainTexture, "for rain");
	add(renderer::kSnowTexture, "for snow");
	// [orig: CEffectEmitterPool_CreateShaders @ 0x5DC8F0, the name @ 0x5dc909]
	add(renderer::kEmitterPoolTexture, "for smoke trails");
	// [orig: WaterRing_LoadResources @ 0x5DDC90]
	add(renderer::kWakeTexture, "for the water rings");
	add(renderer::kWakeGradientTexture, "for the water rings");
	// [orig: Scar_LoadTextures @0x5CC2E0]
	for (int strip = 0; strip < world::kScarTextureStripCount; ++strip)
		add(world::scar_texture_strip_name(strip), "for an impact scar");
	// [orig: Terrain_LoadScorchTextures @0x604CE0]
	for (int index = 0; index <= UINT8_MAX; ++index)
		add(std::string(terrain::terrain_scorch_texture_name(static_cast<uint8_t>(index))), "for a scorch mark");
	// [orig: Environment_LoadTimeOfDayConfig @ 0x57db30, the name @ 0x57dc0c]
	add(env::kOvercastFile, "for the overcast sky");
	// [orig: Entity_InitHelicopterAIFromDef @0x4683C0]
	add(std::string(mission::kFallbackAiProfile) + ".aip", "for a vehicle that names no AI profile");
	add(mission::kTeammateHelicopterAiProfile, "for the teammates' helicopter");
	add(anim::kDefaultAdmName, "for a weapon whose animation map is missing");
	// [orig: HeliLift_SpawnPickup @0x4525e0, the name @0x4526fe]
	add(world::kPickupCallWave, "for a teammate's call when a pickup lands");
	return out;
}

} // namespace

const std::vector<MissionFixedFile> &mission_fixed_files() {
	static const std::vector<MissionFixedFile> files = collect();
	return files;
}

} // namespace opennova::assets
