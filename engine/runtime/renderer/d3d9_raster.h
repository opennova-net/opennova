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

} // namespace opennova::renderer
