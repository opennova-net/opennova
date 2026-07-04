#include <terrain/lighting.h>

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	if (std::fabs(actual - expected) <= epsilon) {
		return true;
	}
	std::fprintf(stderr, "FAIL: expected %.6f, got %.6f\n", expected, actual);
	return false;
}

} // namespace

int main() {
	using opennova::terrain::terrain_average_four_argb;
	using opennova::terrain::terrain_fog_factor_for_distance;
	using opennova::terrain::terrain_fog_start_for_type;
	using opennova::terrain::terrain_light_color_from_ambient_diffuse_argb;
	using opennova::terrain::terrain_modulate_color_argb;

	// Terrain_GetModulatedColorAtPos@0x005C5FE0 preserves alpha and clamps each
	// RGB product after the >> 7 divide.
	if (!expect(terrain_modulate_color_argb(0x80402010u, 0xFF808080u) == 0x80402010u,
	            "light 0x80 should leave RGB unchanged after >>7")) return 1;
	if (!expect(terrain_modulate_color_argb(0x7FC08040u, 0xFFFFFFFFu) == 0x7FFFFF7Fu,
	            "full light should clamp high RGB products and preserve alpha")) return 1;
	if (!expect(terrain_modulate_color_argb(0xAABBCCDDu, 0xFF000000u) == 0xAA000000u,
	            "zero light should black out RGB and preserve alpha")) return 1;

	// Terrain_SetLightingColors@0x005C4B10 computes diffuse /
	// (ambient * 0.70700002 + diffuse), multiplied by 255 and truncated.
	if (!expect(terrain_light_color_from_ambient_diffuse_argb(0xFF000000u, 0xFF808080u) == 0xFFFFFFFFu,
	            "zero ambient should produce full light ratio")) return 1;
	if (!expect(terrain_light_color_from_ambient_diffuse_argb(0xFF808080u, 0xFF808080u) == 0xFF959595u,
	            "equal ambient/diffuse should match the 0.707 ratio truncation")) return 1;
	if (!expect(terrain_light_color_from_ambient_diffuse_argb(0xFFFFFFFFu, 0xFF000000u) == 0xFF000000u,
	            "zero diffuse with nonzero ambient should produce black light")) return 1;

	// Foliage render-emitter parity: average four packed Terrain_GetModulatedColorAtPos
	// samples with the nibble-preserving expression. The old 0x005BF5F0 anchor is stale.
	if (!expect(terrain_average_four_argb(0x40010203u, 0x80050607u, 0xC0090A0Bu, 0xFF0D0E0Fu) == 0x9F070809u,
	            "four-sample foliage color average should truncate each ARGB channel")) return 1;

	// Jointops.exe Render_SetFogState@0x58a950 adjusts start distance for
	// linear fog modes, then CD3DDevice_SetFogParameters@0x677960 uses
	// exp(-d*ln64/end) for mode 0 and linear fog otherwise.
	if (!expect(near(terrain_fog_start_for_type(1000.0f, 1), 0.5f), "fog type 1 keeps the caller's 0.5 start")) return 1;
	if (!expect(near(terrain_fog_start_for_type(1000.0f, 2), 500.0f), "fog type 2 starts at half end")) return 1;
	if (!expect(near(terrain_fog_start_for_type(1000.0f, 3), 250.0f), "fog type 3 starts at quarter end")) return 1;
	if (!expect(near(terrain_fog_factor_for_distance(1000.0f, 1000.0f, 0), 1.0f / 64.0f),
	            "fog type 0 should use exp density ln(64)/end")) return 1;
	if (!expect(near(terrain_fog_factor_for_distance(750.0f, 1000.0f, 2), 0.5f),
	            "fog type 2 should linearly fade between half end and end")) return 1;

	std::printf("OK: terrain lighting, foliage color averaging, and fog helpers match recovered math\n");
	return 0;
}
