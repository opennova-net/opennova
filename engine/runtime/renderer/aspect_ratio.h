#pragma once

namespace opennova::renderer {

// The selected ratio is H/W. Mode 3's literal is .625, even though the
// retail configuration labels it 5:4. Any other value uses the viewport.
// [orig: Render_SetAspectRatioMode @0x58d870]
inline float aspect_height_over_width(int mode, float width, float height) {
    switch (mode) {
        case 0: return 0.75f;
        case 1: return 0.60000002f;
        case 2: return 0.5625f;
        case 3: return 0.625f;
        default: return width > 0.0f ? height / width : 0.75f;
    }
}

} // namespace opennova::renderer
