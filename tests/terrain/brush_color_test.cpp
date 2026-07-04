// Colour / blend brush kernels (libs/terrain/brush) ported from
// terrain_editor_brushes.gd apply_blend_paint / apply_colormap_paint /
// apply_colormap_clone / sample_colormap. BYTE-PARITY CRITICAL: these run on
// RGBA8 buffers and must reproduce Godot's Image FORMAT_RGBA8 conversions
// exactly. The encode/decode constants were verified against a live Godot probe:
//   decode: float(byte / 255.0); encode: trunc(clamp(component * 255.0, 0, 255)).
// The GUT terrain_color_brush_test additionally pins these against REAL Godot
// set_pixel/get_pixel output; this ctest locks the kernel math + quantization.

#include "terrain/brush.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova::terrain;

static int g_fail = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        g_fail = 1;
    }
}

static const BrushRect NO_CLIP{0, 0, 0, 0};

static std::vector<uint8_t> filled_rgba(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        buf[i * 4 + 0] = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = a;
    }
    return buf;
}

static const uint8_t *px(const std::vector<uint8_t> &buf, int w, int x, int z) {
    return &buf[(static_cast<size_t>(z) * w + x) * 4];
}

int main() {
    // --- byte -> float -> byte round-trip pin (the parity-critical fixed point) ---
    for (int b : {0, 1, 127, 128, 200, 255}) {
        const uint8_t rt = float_to_rgba8(rgba8_to_float(static_cast<uint8_t>(b)));
        check(rt == static_cast<uint8_t>(b), "rgba8 round-trip is identity");
    }
    // --- encode truncates at .5 boundaries (matches Godot) ---
    check(float_to_rgba8(static_cast<float>(0.5 / 255.0)) == 0, "encode 0.5/255 -> 0 (trunc)");
    check(float_to_rgba8(static_cast<float>(1.5 / 255.0)) == 1, "encode 1.5/255 -> 1 (trunc)");
    check(float_to_rgba8(static_cast<float>(127.5 / 255.0)) == 127, "encode 127.5/255 -> 127 (trunc)");
    check(float_to_rgba8(2.0f) == 255, "encode clamps high");
    check(float_to_rgba8(-1.0f) == 0, "encode clamps low");

    // --- colormap_paint: hardness 1 strength 1 -> exactly the target inside the disc ---
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 0, 0, 0, 255);
        const bool touched = brush_colormap_paint(buf.data(), 16, 16, 1.0f, 1.0f, 1.0f, 1.0f, 8, 8, 3, 1.0, 1.0, NO_CLIP);
        check(touched, "colormap_paint touched");
        const uint8_t *c = px(buf, 16, 8, 8);
        check(c[0] == 255 && c[1] == 255 && c[2] == 255 && c[3] == 255, "colormap_paint: center -> white target");
        const uint8_t *corner = px(buf, 16, 11, 11); // dist^2 = 18 > 9
        check(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255, "colormap_paint: outside disc unchanged");
    }

    // --- colormap_paint: mid-weight lerp (t=0.5 at center: black -> white halfway) ---
    // center falloff is 1 at any hardness, so t = strength = 0.5; lerp(0,1,0.5)=0.5 -> 127.
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 0, 0, 0, 255);
        brush_colormap_paint(buf.data(), 16, 16, 1.0f, 1.0f, 1.0f, 1.0f, 8, 8, 3, 0.5, 1.0, NO_CLIP);
        const uint8_t *c = px(buf, 16, 8, 8);
        check(c[0] == 127 && c[1] == 127 && c[2] == 127 && c[3] == 255, "colormap_paint: t=0.5 black->white -> 127 (mid-weight lerp)");
    }

    // --- colormap_paint: weight 0 (strength 0) leaves the buffer byte-identical ---
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 40, 80, 120, 255);
        brush_colormap_paint(buf.data(), 16, 16, 1.0f, 1.0f, 1.0f, 1.0f, 8, 8, 3, 0.0, 1.0, NO_CLIP);
        const uint8_t *c = px(buf, 16, 8, 8);
        check(c[0] == 40 && c[1] == 80 && c[2] == 120, "colormap_paint: strength 0 round-trips unchanged");
    }

    // --- blend_paint: add to green over a pure-red weight, renormalize ---
    // red=1,green=0,blue=0; +1 green -> (1,1,0); /2 -> (0.5,0.5,0) -> bytes (127,127,0).
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 255, 0, 0, 255);
        const bool touched = brush_blend_paint(buf.data(), 16, 16, 1, 8, 8, 3, 1.0, 1.0, NO_CLIP);
        check(touched, "blend_paint touched");
        const uint8_t *c = px(buf, 16, 8, 8);
        check(c[0] == 127 && c[1] == 127 && c[2] == 0 && c[3] == 255, "blend_paint: (1,0,0)+green -> (127,127,0,255)");
        const uint8_t *corner = px(buf, 16, 11, 11);
        check(corner[0] == 255 && corner[1] == 0 && corner[2] == 0, "blend_paint: outside disc unchanged");
    }

    // --- blend_paint collapse branch: total <= 0.001 resets to the channel ---
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 0, 0, 0, 255);
        // strength 0 -> amount 0 -> total 0 -> reset channel 2 (blue) to 1.
        brush_blend_paint(buf.data(), 16, 16, 2, 8, 8, 3, 0.0, 1.0, NO_CLIP);
        const uint8_t *c = px(buf, 16, 8, 8);
        check(c[0] == 0 && c[1] == 0 && c[2] == 255, "blend_paint: collapsed sum resets to channel (blue)");
    }

    // --- colormap_clone: hardness 1 strength 1 copies the offset source pixel ---
    {
        std::vector<uint8_t> dst = filled_rgba(16, 16, 0, 0, 0, 255);
        std::vector<uint8_t> src = filled_rgba(16, 16, 10, 20, 30, 255);
        // mark the source pixel that maps to dest center (8,8): sx = x - dst_cx + src_cx.
        // with dst_cx=8, src_cx=4 -> sx = x - 4; at x=8, sx=4. Set src(4,4) distinctly.
        src[(4 * 16 + 4) * 4 + 0] = 200;
        src[(4 * 16 + 4) * 4 + 1] = 150;
        src[(4 * 16 + 4) * 4 + 2] = 100;
        const bool touched = brush_colormap_clone(dst.data(), 16, 16, src.data(), 16, 16, 4, 4, 8, 8, 3, 1.0, 1.0, NO_CLIP);
        check(touched, "colormap_clone touched");
        const uint8_t *c = px(dst, 16, 8, 8);
        check(c[0] == 200 && c[1] == 150 && c[2] == 100, "colormap_clone: center copies offset source pixel");
    }

    // --- sample_color: nearest pixel, edge-clamped ---
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 5, 6, 7, 255);
        buf[(3 * 16 + 2) * 4 + 0] = 99;
        uint8_t out[4];
        brush_sample_color(buf.data(), 16, 16, 2.7, 3.2, out); // trunc -> (2,3)
        check(out[0] == 99 && out[1] == 6 && out[2] == 7 && out[3] == 255, "sample_color: nearest (trunc) pixel");
        brush_sample_color(buf.data(), 16, 16, -5.0, 100.0, out); // clamp -> (0,15)
        check(out[0] == 5 && out[1] == 6, "sample_color: edge-clamped");
    }

    // --- clip honoured ---
    {
        std::vector<uint8_t> buf = filled_rgba(16, 16, 0, 0, 0, 255);
        brush_colormap_paint(buf.data(), 16, 16, 1.0f, 1.0f, 1.0f, 1.0f, 8, 8, 3, 1.0, 1.0, BrushRect{0, 0, 9, 9});
        const uint8_t *inside = px(buf, 16, 8, 8);
        check(inside[0] == 255, "colormap_paint+clip: inside clip painted");
        const uint8_t *outside = px(buf, 16, 9, 8); // x=9 outside clip (x<9)
        check(outside[0] == 0, "colormap_paint+clip: outside clip untouched");
    }

    if (g_fail) {
        std::fprintf(stderr, "brush_color_test: FAILED\n");
        return 1;
    }
    std::printf("brush_color_test: OK\n");
    return 0;
}
