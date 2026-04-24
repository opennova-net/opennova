#include <terrain/lighting.h>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
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

	std::printf("OK: terrain foliage light modulation matches recovered packed-color math\n");
	return 0;
}
