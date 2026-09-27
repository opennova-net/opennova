#include <runtime/renderer/light_runtime.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearly_equal(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

template <std::size_t N>
void expect_vector(const std::array<float, N>& actual,
                   const std::array<float, N>& expected,
                   const char* message,
                   float epsilon = 0.0001f) {
    for (std::size_t i = 0; i < N; ++i) {
        if (!nearly_equal(actual[i], expected[i], epsilon)) {
            std::cerr << message << " at channel " << i << ": expected "
                      << expected[i] << ", got " << actual[i] << '\n';
            std::exit(1);
        }
    }
}

}  // namespace

int main() {
    using namespace opennova::renderer;

    expect_vector(unpack_modulator_scale(0x00804020u),
                  std::array<float, 3>{2.0f, 1.0f, 0.5f},
                  "modulator bytes should unpack with 64 as identity");

    WorldLightingInputs base;
    base.light_packed = 0x00FF8040u;
    base.sky_packed = 0x00604020u;
    base.ground_packed = 0x00204060u;
    base.ceiling_packed = 0x00102030u;
    base.floor_packed = 0x00302010u;
    base.light_dir = {3.0f, 4.0f, 0.0f};
    const auto world = build_world_lighting(base);
    expect(world.dir_enabled, "base world lighting should enable the directional light");
    expect_vector(world.dir_color,
                  std::array<float, 3>{1.0f, 128.0f / 255.0f, 64.0f / 255.0f},
                  "world directional color should unpack 0x00RRGGBB");
    expect_vector(world.dir, std::array<float, 3>{-0.6f, -0.8f, 0.0f},
                  "world light direction should be negated and normalized");
    expect_vector(world.outdoor_ambient,
                  std::array<float, 3>{64.0f / 255.0f, 64.0f / 255.0f,
                                       64.0f / 255.0f},
                  "outdoor ambient should average sky and ground");
    expect_vector(world.indoor_ambient,
                  std::array<float, 3>{32.0f / 255.0f, 32.0f / 255.0f,
                                       32.0f / 255.0f},
                  "indoor ambient should average ceiling and floor");

    {
        auto nvg = base;
        nvg.sky_packed = nvg.ground_packed = 0;
        nvg.ceiling_packed = nvg.floor_packed = 0;
        nvg.nvg_hemi_rewrite = true;
        nvg.nvg_level = 4;
        nvg.modulator_packed = 0x00404040u;
        const auto lit = build_world_lighting(nvg);
        expect_vector(lit.hemi_sky, std::array<float, 3>{0.1f, 0.1f, 0.1f},
                      "NVG level five should add raw modulator bytes at f/640");
        expect_vector(lit.floor_color, std::array<float, 3>{0.1f, 0.1f, 0.1f},
                      "NVG rewrite should reach all four hemisphere blocks");
    }

    {
        // A tinted modulator separates the two NVG term forms: sky/ground take
        // the per-byte terms, ceiling/floor reuse the R term on every channel
        // [orig: @0x5c8258..0x5c82a1 vs @0x5c82a5..0x5c82e9].
        auto nvg = base;
        nvg.sky_packed = nvg.ground_packed = 0;
        nvg.ceiling_packed = nvg.floor_packed = 0;
        nvg.nvg_hemi_rewrite = true;
        nvg.nvg_level = 4;
        nvg.modulator_packed = 0x00402010u;
        const auto lit = build_world_lighting(nvg);
        expect_vector(lit.hemi_sky, std::array<float, 3>{0.1f, 0.05f, 0.025f},
                      "NVG sky/ground should take the per-channel modulator terms");
        expect_vector(lit.ceiling_color, std::array<float, 3>{0.1f, 0.1f, 0.1f},
                      "NVG ceiling should reuse the modulator R term on all channels");
        expect_vector(lit.floor_color, std::array<float, 3>{0.1f, 0.1f, 0.1f},
                      "NVG floor should reuse the modulator R term on all channels");
    }

    {
        auto thermal = base;
        thermal.thermal_grey = true;
        const auto lit = build_world_lighting(thermal);
        expect(!lit.dir_enabled, "thermal grey override should disable direction");
        expect_vector(lit.dir_color, std::array<float, 3>{0.0f, 0.0f, 0.0f},
                      "disabled direction should be zeroed at the context store");
        expect_vector(lit.hemi_sky, std::array<float, 3>{0.5f, 0.5f, 0.5f},
                      "thermal grey override should use neutral half-grey hemisphere");
        expect_vector(lit.floor_color, std::array<float, 3>{0.5f, 0.5f, 0.5f},
                      "thermal grey override should reach the interior pair");
    }

    {
        auto wave_dim = base;
        wave_dim.thermal_wave_dim = true;
        const auto lit = build_world_lighting(wave_dim);
        expect(!lit.dir_enabled, "thermal wave dim should disable direction");
        expect_vector(lit.hemi_ground, std::array<float, 3>{0.25f, 0.25f, 0.25f},
                      "thermal wave dim should force quarter-intensity ambience");
    }

    {
        const auto outdoor = compute_entity_lighting(world, 0.5f, false, 0.0f);
        expect_vector(outdoor.dir_color,
                      std::array<float, 3>{0.5f, 64.0f / 255.0f, 32.0f / 255.0f},
                      "entity effect scale should attenuate only directional color");
        expect_vector(outdoor.hemi_sky, world.hemi_sky,
                      "outdoor entity should inherit the outdoor sky block");

        const auto closed = compute_entity_lighting(world, 0.5f, true, 0.0f);
        expect_vector(closed.dir_color, std::array<float, 3>{0.0f, 0.0f, 0.0f},
                      "closed interior should receive no sun");
        expect_vector(closed.hemi_ground, world.floor_color,
                      "closed interior ground should use the floor block");
        expect_vector(closed.hemi_sky, world.ceiling_color,
                      "closed interior sky should use the ceiling block");

        const auto open = compute_entity_lighting(world, 0.5f, true, 1.0f);
        expect_vector(open.hemi_ground, world.hemi_ground,
                      "fully open interior should reach outdoor ground");
        expect_vector(open.hemi_sky, world.hemi_sky,
                      "fully open interior should reach outdoor sky");
    }

    // The static-row lane [orig: Terrain_RenderSectorModels @ 0x5c5df2..
    // 0x5c5e00 + Render_CollectRenderObjectsForBatch @ 0x5d9156..0x5d9162 for
    // buildings; Terrain_RenderSectorEntities @ 0x5c7c05..0x5c7c14 over the
    // zeroed stack-base aux for every other static].
    {
        const auto shell = static_row_entity_lighting(true, 0, 0.6f, false);
        expect(shell.effect_scale == 1.0f && !shell.interior_lerp,
               "a building's exterior shell (ROBJ 0) never lerps");
        const auto room = static_row_entity_lighting(true, 2, 0.6f, false);
        expect(room.effect_scale == 1.0f && room.interior_lerp && room.interior_daylight == 0.6f,
               "a building's ROBJ 1+ lerps by its own daylight");
        const auto closed = static_row_entity_lighting(true, 1, 0.0f, false);
        expect(closed.interior_lerp && closed.interior_daylight == 0.0f,
               "a closed building's rooms take pure floor/ceiling");
        const auto crate_in = static_row_entity_lighting(false, 0, 0.6f, true);
        expect(crate_in.effect_scale == 1.0f && crate_in.interior_lerp &&
                       crate_in.interior_daylight == 0.0f,
               "a contained static lerps with the stack-base t = 0");
        const auto crate_out = static_row_entity_lighting(false, 0, 0.6f, false);
        expect(crate_out.effect_scale == 1.0f && !crate_out.interior_lerp,
               "an outdoor static stays on the outdoor path");
    }

    {
        EntityLightingUniforms u;
        u.dir_color = {0.4f, 0.1f, 0.0f};
        u.hemi_ground = {0.1f, 0.2f, 0.3f};
        u.hemi_sky = {0.6f, 0.7f, 0.8f};
        u.ambient = {0.2f, 0.3f, 0.4f};
        expect_vector(ff_vertex_light(u, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}),
                      u.hemi_sky,
                      "up normal should resolve exactly to hemisphere sky");
        expect_vector(ff_vertex_light(u, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}),
                      u.hemi_ground,
                      "down normal should resolve exactly to hemisphere ground");
        expect_vector(ff_vertex_light(u, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}),
                      std::array<float, 3>{0.6f, 0.4f, 0.4f},
                      "horizon normal should combine ambient and directional light");
        u.dir_color = {2.0f, 2.0f, 2.0f};
        expect_vector(ff_vertex_light(u, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}),
                      std::array<float, 3>{1.0f, 1.0f, 1.0f},
                      "fixed-function vertex diffuse should saturate");
    }

    expect(nearly_equal(sun_visibility_factor(-10), 1.0f),
           "negative blocked-ray count should clamp to fully sunlit");
    expect(nearly_equal(sun_visibility_factor(0), 1.0f),
           "zero blocked rays should be fully sunlit");
    expect(nearly_equal(sun_visibility_factor(1), 0.75f),
           "one blocked ray should leave three quarters sun");
    expect(nearly_equal(sun_visibility_factor(3), 0.25f),
           "three blocked rays should retain the witnessed quarter floor");
    expect(nearly_equal(sun_visibility_factor(99), 0.25f),
           "blocked-ray overflow should clamp to the quarter floor");

    expect_vector(point_light_color({0.5f, 0.25f, 1.0f}, 2.0f,
                                    {1.0f, 0.5f, 0.25f}, false),
                  std::array<float, 3>{1.0f, 0.25f, 0.5f},
                  "shader point-light path should apply intensity and modulator");
    expect_vector(point_light_color({0.5f, 0.25f, 1.0f}, 2.0f,
                                    {1.0f, 0.5f, 0.25f}, true),
                  std::array<float, 3>{1.5f, 0.375f, 0.75f},
                  "D3D point-light path should add the witnessed 1.5 boost");

    expect_vector(terrain_surface_light(0.25f, {0.8f, 0.4f, 0.2f},
                                        {0.1f, 0.2f, 0.3f}),
                  std::array<float, 3>{0.3f, 0.3f, 0.35f},
                  "terrain alpha should scale light and add the sky block");

    return 0;
}
