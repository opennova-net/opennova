#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace opennova::editor {

// `value` on the grid of `snap` (the nearest step; as it is with no snap): where a drag, a box or a
// handle lands with the snap on.
inline double snapped(double value, float snap) {
	return snap > 0.0f ? std::round(value / double(snap)) * double(snap) : value;
}

// A length in the preview's words: `value` metres to `decimals` places ("2.50 m"), each caller's
// own precision.
inline std::string metres(double value, int decimals) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.*f m", decimals, value);
	return text;
}

} // namespace opennova::editor
