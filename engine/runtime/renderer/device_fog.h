#pragma once

// The witnessed device fog evaluation shared by every retail pass that runs
// the primary fog state: the object/terrain/water shaders, the corona
// fog-to-black fold, and the Q3 bloom-source copies. `start` and `end` are the
// values Render_SetFogState already resolved per fog type (formats/env
// compute_fog_params: type 1 = the caller's 0.5, types 2/3 =
// (1 - overcast) * end * {0.5, 0.25}); this function never re-derives them.
// [orig: Render_SetFogState @ 0x58a950; CD3DDevice_SetFogParameters
// @ 0x677960 (useLinear = fog_type != 0, exponential density ln(64)/end)]

#include <algorithm>
#include <cmath>

namespace opennova::renderer {

// ln(64): the exponential density numerator (D3DFOG_EXP reaches 1/64 at end).
inline constexpr float kDeviceFogLn64 = 4.1588830833596715f;

// Visibility (1 = unfogged, 0 = fully fogged) at `dist` world units from the
// eye. Type 0 is exponential; every other type is linear from `start`.
inline float device_fog_visibility(float dist, float start, float end,
		int type, bool enabled) {
	if (!enabled) {
		return 1.0f;
	}
	const float safe_end = std::max(end, 1.0f);
	if (type == 0) {
		return std::clamp(
				std::exp(-std::max(dist, 0.0f) * (kDeviceFogLn64 / safe_end)),
				0.0f, 1.0f);
	}
	return std::clamp((safe_end - dist) / std::max(safe_end - start, 1.0f),
			0.0f, 1.0f);
}

} // namespace opennova::renderer
