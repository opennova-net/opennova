// Height brush kernels (libs/terrain/brush) ported from
// godot/modtools/terrain/terrain_editor_brushes.gd (apply_raise_lower /
// apply_smooth / apply_flatten + the shared _for_each_brush_pixel falloff).
//
// Parity notes locked here:
//   * raise_lower weights by falloff*falloff; smooth and flatten use linear falloff.
//   * the smoothstep falloff: t = 1 - dist/radius; shoulder = 1 - clamp(hardness,0,1);
//     falloff = 1 if shoulder<=0.0001 or t>=shoulder, else smoothstep(t/shoulder).
//   * raise_lower clamps height to [0, 255.996]; flatten/smooth lerp (no clamp).
//   * smooth is a true two-pass: snapshot the brush footprint, then average a 3x3
//     window clamped to IMAGE bounds (not the clip rect), dividing by the running
//     neighbour count (3..9); in-footprint neighbours read the snapshot, others read
//     the (unmutated) live buffer.
//   * math runs in double (GDScript float is 64-bit) and is stored back as float32.

#include "terrain/brush.h"

#include <cmath>
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

static void check_close(double got, double want, double tol, const char *msg) {
    if (!(std::fabs(got - want) <= tol)) {
        std::fprintf(stderr, "FAIL: %s (got %.10f want %.10f)\n", msg, got, want);
        g_fail = 1;
    }
}

static const BrushRect NO_CLIP{0, 0, 0, 0};

static std::vector<float> filled(int w, int h, float v) {
    return std::vector<float>(static_cast<size_t>(w) * h, v);
}

static float at(const std::vector<float> &buf, int w, int x, int z) {
    return buf[static_cast<size_t>(z) * w + x];
}

int main() {
    // --- falloff primitive ---
    check_close(brush_falloff(1.0, 0.0), 1.0, 0.0, "falloff: hardness 1 (shoulder 0) -> 1");
    check_close(brush_falloff(0.0, 0.0), 1.0, 0.0, "falloff: shoulder<=eps -> 1 even at edge");
    // hardness 0 (shoulder 1): t>=1 -> 1; t=0.5 -> smoothstep(0.5)=0.5; t=0 -> 0.
    check_close(brush_falloff(1.0, 1.0), 1.0, 0.0, "falloff: center t=1 -> 1");
    check_close(brush_falloff(0.5, 1.0), 0.5, 1e-12, "falloff: t=0.5 shoulder=1 -> smoothstep 0.5");
    check_close(brush_falloff(0.0, 1.0), 0.0, 0.0, "falloff: edge t=0 -> 0");

    // --- "touched" return: false when the dab hits nothing, true otherwise ---
    {
        std::vector<float> buf = filled(16, 16, 10.0f);
        check(brush_raise_lower(buf.data(), 16, 16, 8, 8, 3, 5.0, 1.0, NO_CLIP), "raise: in-bounds dab reports touched");
        check(!brush_raise_lower(buf.data(), 16, 16, 100, 100, 3, 5.0, 1.0, NO_CLIP), "raise: off-image center touches nothing");
        check(!brush_raise_lower(buf.data(), 16, 16, 8, 8, 0, 5.0, 1.0, NO_CLIP), "raise: radius 0 touches nothing");
        check(!brush_raise_lower(buf.data(), 16, 16, 8, 8, 3, 5.0, 1.0, BrushRect{100, 100, 4, 4}),
              "raise: clip fully outside touches nothing");
        check(!brush_smooth(buf.data(), 16, 16, 100, 100, 3, 1.0, 1.0, NO_CLIP), "smooth: off-image touches nothing");
        check(!brush_flatten(buf.data(), 16, 16, 100, 100, 3, 1.0, 1.0, 1.0, NO_CLIP), "flatten: off-image touches nothing");
    }

    // --- raise_lower, hardness 1 (uniform +amount inside the disc) ---
    {
        std::vector<float> buf = filled(16, 16, 10.0f);
        brush_raise_lower(buf.data(), 16, 16, 8, 8, 3, 5.0, 1.0, NO_CLIP);
        check_close(at(buf, 16, 8, 8), 15.0, 0.0, "raise: center +5");
        check_close(at(buf, 16, 11, 8), 15.0, 0.0, "raise: edge dist=3 (==radius) included +5");
        check_close(at(buf, 16, 8, 11), 15.0, 0.0, "raise: edge dist=3 vertical +5");
        check_close(at(buf, 16, 11, 11), 10.0, 0.0, "raise: corner dist^2=18>9 untouched");
        check_close(at(buf, 16, 5, 5), 10.0, 0.0, "raise: corner untouched");
    }

    // --- raise_lower clamps to [0, 255.996] ---
    {
        std::vector<float> hi = filled(8, 8, 10.0f);
        brush_raise_lower(hi.data(), 8, 8, 4, 4, 2, 1000.0, 1.0, NO_CLIP);
        check_close(at(hi, 8, 4, 4), static_cast<double>(255.996f), 1e-3, "raise: clamps high to 255.996");
        std::vector<float> lo = filled(8, 8, 10.0f);
        brush_raise_lower(lo.data(), 8, 8, 4, 4, 2, -1000.0, 1.0, NO_CLIP);
        check_close(at(lo, 8, 4, 4), 0.0, 0.0, "lower: clamps low to 0");
    }

    // --- raise_lower honours the clip rect ---
    {
        std::vector<float> buf = filled(16, 16, 10.0f);
        // clip to x<9, z<9: pixels at x>=9 stay 10 even though they are in radius.
        brush_raise_lower(buf.data(), 16, 16, 8, 8, 3, 5.0, 1.0, BrushRect{0, 0, 9, 9});
        check_close(at(buf, 16, 8, 8), 15.0, 0.0, "raise+clip: center inside clip +5");
        check_close(at(buf, 16, 9, 8), 10.0, 0.0, "raise+clip: x=9 outside clip untouched");
    }

    // --- flatten, hardness 1 strength 1 -> exactly the target inside the disc ---
    {
        std::vector<float> buf = filled(16, 16, 10.0f);
        brush_flatten(buf.data(), 16, 16, 8, 8, 3, 42.0, 1.0, 1.0, NO_CLIP);
        check_close(at(buf, 16, 8, 8), 42.0, 0.0, "flatten: center -> target");
        check_close(at(buf, 16, 11, 8), 42.0, 0.0, "flatten: edge -> target");
        check_close(at(buf, 16, 11, 11), 10.0, 0.0, "flatten: outside disc untouched");
    }

    // --- smooth of a uniform field is a no-op ---
    {
        std::vector<float> buf = filled(16, 16, 7.0f);
        brush_smooth(buf.data(), 16, 16, 8, 8, 3, 1.0, 1.0, NO_CLIP);
        check_close(at(buf, 16, 8, 8), 7.0, 0.0, "smooth: uniform stays uniform (center)");
        check_close(at(buf, 16, 6, 9), 7.0, 0.0, "smooth: uniform stays uniform (off-center)");
    }

    // --- smooth of a vertical ridge (column x=8 == 9, else 0) ---
    // Each ridge pixel's 3x3 has exactly three 9s -> avg 27/9 = 3; strength 1 -> 3.
    // Off-ridge pixels whose 3x3 contains no ridge stay 0.
    {
        std::vector<float> buf = filled(16, 16, 0.0f);
        for (int z = 0; z < 16; ++z) {
            buf[static_cast<size_t>(z) * 16 + 8] = 9.0f;
        }
        brush_smooth(buf.data(), 16, 16, 8, 8, 2, 1.0, 1.0, NO_CLIP);
        check_close(at(buf, 16, 8, 8), 3.0, 1e-6, "smooth ridge: center 9 -> 3 (3x3 avg)");
        check_close(at(buf, 16, 8, 6), 3.0, 1e-6, "smooth ridge: ridge pixel -> 3");
        check_close(at(buf, 16, 10, 8), 0.0, 0.0, "smooth ridge: off-ridge (no 9 in 3x3) stays 0");
    }

    if (g_fail) {
        std::fprintf(stderr, "brush_height_test: FAILED\n");
        return 1;
    }
    std::printf("brush_height_test: OK\n");
    return 0;
}
