#pragma once

#include <cstdint>

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

// The frame's vertical projection scale (flt_8409E8): the selected ratio over
// the surface's own for the four literal modes, 1.0 for a native mode. The
// world pass and the first-person viewmodel pass both push it as their
// viewportScaleY, so one scale stretches every frustum of the frame.
// [orig: Render_SetAspectRatioMode @0x58d8a7 (modes 0..3) / @0x58d8d9 (native);
//  Render_SetViewProjectionWithDefaults @0x58f6b0; Player_RenderViewModelIfAlive @0x4e0154]
inline float aspect_viewport_scale_y(int mode, float width, float height) {
    if (mode < 0 || mode > 3 || width <= 0.0f || height <= 0.0f) return 1.0f;
    return aspect_height_over_width(mode, width, height) / (height / width);
}

// The first launch's seed of the cfg's display_16x9 word, the 16x9DISPLAY spin's
// value (1 = its widescreen row = mode 1 above, 0 = its 4:3 row). With no cfg
// the adapter name/GUID stay empty after Config_SetDefaults' memset, so
// Game_InitSubsystems runs the video test, which samples the primary desktop
// (GetSystemMetrics SM_CXSCREEN / SM_CYSCREEN), presets the word to 1 and drops
// it to 0 unless width / height exceeds the single-precision literal 1.34: an
// x87 fild/fidiv quotient against flt_7D15AC through fcom + test ah,5 / jnp,
// the strict ordered compare (the unordered 0/0 surface also drops to 0). The
// verdict is saved with the whole config at once, so a later launch reads the
// word back through the settings parser and never samples the desktop again.
// [orig: Game_InitSubsystems @0x4a711c / @0x4a7125; Game_RunVideoTestDialog
//  @0x53ecd8 / @0x53ece2 (the desktop sample), @0x53ed3e / @0x53ed4e (fild/fidiv),
//  @0x53ed52 (preset 1), @0x53ed5c (flt_7D15AC = 1.34f), @0x53ed62 (fcom),
//  @0x53ed69 (jnp), @0x53ed6b (0), @0x53edb6 (Game_SaveConfig)]
inline int fresh_profile_aspect_mode(int32_t width, int32_t height) {
    const double ratio = static_cast<double>(width) / static_cast<double>(height);
    return ratio > static_cast<double>(1.34f) ? 1 : 0;
}

} // namespace opennova::renderer
