#pragma once

// Where the original's pixels sit. It rasterises through Direct3D 9, which puts a
// pixel's centre on the integer window coordinate: pixel i covers [i - 0.5, i + 0.5).
// A raster whose pixel centres sit at i + 0.5 (Godot's, and every Direct3D 10+ or
// Vulkan one) draws the same coordinates half a pixel up and to the left.
//
// The 3D pass builds its projection with no sub-pixel term (D3DXMatrixPerspectiveFovLH
// over the viewport's width and height) and sets an integer viewport, so a world point
// the projection maps to window x lands on pixel x's centre. The 2D draws are
// pre-transformed vertices (FVF 0x2C4: XYZRHW, diffuse, specular, two texture
// coordinates) at their screen coordinates as given; only the font drawer takes half a
// pixel off its glyph quads (runtime/hud/game_font.cpp), which lands each texel on a
// pixel centre under this convention.
// [orig: Render_SetViewAndProjectionMatrices @0x58D900 (D3DXMatrixPerspectiveFovLH
//  @0x58D9DE, SetTransform(D3DTS_PROJECTION) @0x58D9F3); Render_SetViewport @0x58A720
//  (integer X/Y/Width/Height); GDynamicVB_DrawPrimitive @0x6788E0 (SetFVF 0x2C4,
//  DrawPrimitiveUP); HUD_DrawTexturedQuad_0 @0x56B3E0 and HUD_DrawCrosshairCornerQuad
//  @0x590F50 (positions verbatim)]
//
// Measured on JO:CA at 1024x768 (2026-10-08): retail's crosshair, compass ring and HUD
// text sit 0.5 px right and 0.5 px down of OpenNova's before this rule.

#include <cmath>

namespace opennova::renderer {

// The offset from a D3D9 window coordinate to the same point on a raster whose
// pixel centres sit at i + 0.5.
inline constexpr float kD3d9PixelCentre = 0.5f;

// A translation in normalized device coordinates, x right and y up.
struct NdcShift {
    float x = 0.0f;
    float y = 0.0f;
};

// The translation that lands a projection's image on a raster_w x raster_h target
// where D3D9 rasterises it: half a pixel right and half a pixel down. A raster with
// no pixels takes none.
inline NdcShift d3d9_raster_ndc_shift(int raster_w, int raster_h) {
    if (raster_w <= 0 || raster_h <= 0) return {};
    return {2.0f * kD3d9PixelCentre / static_cast<float>(raster_w),
            -2.0f * kD3d9PixelCentre / static_cast<float>(raster_h)};
}

// The same translation as a frustum offset on the near plane: a frustum
// near_width x near_height wide on its near plane, moved by this, draws its image
// shifted by `shift` (the near plane's centre moves the opposite way).
struct NearPlaneOffset {
    float x = 0.0f;
    float y = 0.0f;
};
inline NearPlaneOffset near_plane_offset(const NdcShift &shift, float near_width,
                                         float near_height) {
    return {-shift.x * near_width * 0.5f, -shift.y * near_height * 0.5f};
}

// A block of whole pixels, [x0, x1) x [y0, y1).
struct PixelRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// The pixels Direct3D 9 covers with a pre-transformed quad from (x0, y0) to (x1, y1):
// those whose centres lie in [x0, x1) x [y0, y1) (its top-left fill rule), the first
// column ceil(x0) and the last ceil(x1) - 1. A quad on the integers covers the same
// pixels on either raster; one off them (the menu frame's border pieces,
// MenuFrameCompiler::emit_frame) is where the two disagree.
inline PixelRect d3d9_quad_pixels(float x0, float y0, float x1, float y1) {
    return {static_cast<int>(std::ceil(x0)), static_cast<int>(std::ceil(y0)),
            static_cast<int>(std::ceil(x1)), static_cast<int>(std::ceil(y1))};
}

// The pixels Direct3D 9 lights for a one-pixel line-list segment from a to b, both on
// the integers and the segment axis-aligned: its diamond-exit rule lights a's pixel and
// every one up to, not including, b's, which the next segment of an outline starts on.
// False for a segment that is diagonal, off the integers or of no length, which a
// device draws its own way. A raster that draws a line as a one-pixel-wide quad
// centred on it covers both ends alike, so an outline drawn that way loses the corner
// its last segment ends on [orig: draw_line_2d @ 0x6786d0 — the points verbatim,
// D3DPT_LINELIST; CUIElement_DrawOutlineRect @ 0x647fc0 strings four such segments
// round a rect].
inline bool d3d9_axis_line_pixels(float ax, float ay, float bx, float by, PixelRect *out) {
    if (ax != std::floor(ax) || ay != std::floor(ay) || bx != std::floor(bx) ||
        by != std::floor(by)) {
        return false;
    }
    const int x0 = static_cast<int>(ax);
    const int y0 = static_cast<int>(ay);
    const int x1 = static_cast<int>(bx);
    const int y1 = static_cast<int>(by);
    if (y0 == y1 && x0 != x1) {
        *out = x1 > x0 ? PixelRect{x0, y0, x1, y0 + 1} : PixelRect{x1 + 1, y0, x0 + 1, y0 + 1};
        return true;
    }
    if (x0 == x1 && y0 != y1) {
        *out = y1 > y0 ? PixelRect{x0, y0, x0 + 1, y1} : PixelRect{x0, y1 + 1, x0 + 1, y0 + 1};
        return true;
    }
    return false;
}

} // namespace opennova::renderer
