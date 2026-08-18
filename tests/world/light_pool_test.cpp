// Dynamic light pool unit tests [orig: Light_InstanceTable @0x2732E28;
// LightPool_SpawnGlowEffect @0x5A8D50; EffectWorld_TickInstancesAndLightScale @0x5AA170;
// Light_FillD3DPointLight @0x5AA450; the mutators sub_5A8F80 @0x5A8F80 /
// CEffectInstance_SetBlendAmount @0x5A8EE0 / SetPositionAndBounds @0x5A9070 /
// ModifyRenderFlags @0x5A8F20].
//
// Covers the three feeds' semantics: the impact flash (state 2 = fade then FREE), the
// in-flight round glow (state 1 with ticks -1 = constant, never expires) and the muzzle
// glow (spawn state 3 parked, re-armed to state 4 with 5 ticks per shot), plus the fade
// ARITHMETIC (linear on intensity, radius constant), the free-slot reuse walk, the
// handle encoding, and the D3D fill (Range = 1.25 x radius, atten2 = 15 / Range^2 =>
// exactly 1/16 at Range).
#include <cmath>
#include <cstdio>

#include "world/light_pool.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                          \
    do {                                                                               \
        if (!(std::fabs((a) - (b)) <= (eps))) {                                        \
            std::printf("FAIL %s:%d  %s ~= %s (%f vs %f)\n", __FILE__, __LINE__, #a,   \
                        #b, (double)(a), (double)(b));                                 \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

namespace {

constexpr int32_t kUnit = 65536;   // 1.0 in Q16

// revx02 `grenadehe`: light_impact 15.0 255 192 96 0.4 -> 25 ticks
// (62 * 0.4 = 24.8, round-half-up).
constexpr int32_t kGrenadeRadius = 15 * kUnit;
constexpr uint32_t kWarm = 0xFFC060u;   // 255 192 96

void test_spawn_records_the_witnessed_fields() {
    LightPool pool;
    const int32_t pos[3] = { 10 * kUnit, -20 * kUnit, 3 * kUnit };
    const int h = pool.spawn(pos, kGrenadeRadius, kWarm, LightPool::kStateFadeFree, 25);
    // handle = index | 0x8000, and the first spawn takes index 0 [orig: @0x5A8D50 tail].
    CHECK(h == (0 | LightPool::kHandleBit));
    const LightPool::Light *e = pool.get(h);
    CHECK(e != nullptr);
    if (e == nullptr) return;
    CHECK(e->flags == LightPool::kFlagLive);
    CHECK(e->pos[0] == pos[0] && e->pos[1] == pos[1] && e->pos[2] == pos[2]);
    // The AABB is position +- radius on every axis [orig: @0x5A8D50].
    CHECK(e->bbox_min[0] == pos[0] - kGrenadeRadius);
    CHECK(e->bbox_max[2] == pos[2] + kGrenadeRadius);
    CHECK(e->radius == kGrenadeRadius);
    // Colour bytes divide by 256, not 255 [orig: `* 0.00390625`].
    CHECK_NEAR(e->rgb[0], 255.0f / 256.0f, 1e-6f);
    CHECK_NEAR(e->rgb[1], 192.0f / 256.0f, 1e-6f);
    CHECK_NEAR(e->rgb[2], 96.0f / 256.0f, 1e-6f);
    CHECK_NEAR(e->blend, 1.0f, 1e-6f);   // blend starts at full [orig: `+14 = 1.0`]
    CHECK(e->state == LightPool::kStateFadeFree);
    CHECK(e->ticks_left == 25 && e->ticks_total == 25);
    CHECK(pool.active_count() == 1);
}

// State 2 (the impact flash): blend falls LINEARLY with ticks_left/ticks_total, the
// radius never moves, and the tick that reaches zero FREES the slot.
void test_state2_fades_linearly_then_frees() {
    LightPool pool;
    const int32_t pos[3] = { 0, 0, 0 };
    const int h = pool.spawn(pos, kGrenadeRadius, kWarm, LightPool::kStateFadeFree, 4);
    for (int t = 3; t >= 1; --t) {
        pool.tick();
        const LightPool::Light *e = pool.get(h);
        CHECK(e != nullptr);
        if (e == nullptr) return;
        CHECK(e->ticks_left == t);
        CHECK_NEAR(e->blend, static_cast<float>(t) / 4.0f, 1e-6f);
        CHECK(e->radius == kGrenadeRadius);   // RADIUS CONSTANT — only intensity fades
    }
    pool.tick();   // the tick that takes ticks_left 1 -> 0
    CHECK(pool.get(h) == nullptr);            // state != 5 -> the record is memset
    // The active prefix is computed from the flags read at LOOP ENTRY, so a slot freed
    // during a tick still counts for that tick and compacts on the NEXT one
    // [orig: `new_count = index + 1` runs before the memset @0x5AA170].
    CHECK(pool.active_count() == 1);
    pool.tick();
    CHECK(pool.active_count() == 0);
}

// State 5: the same fade, but the zero tick sets the disabled flag and KEEPS the slot.
void test_state5_disables_and_keeps_the_slot() {
    LightPool pool;
    const int32_t pos[3] = { 0, 0, 0 };
    const int h = pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateFadeDisable, 2);
    pool.tick();
    const LightPool::Light *e = pool.get(h);
    CHECK(e != nullptr && e->renderable());
    pool.tick();
    e = pool.get(h);
    CHECK(e != nullptr);                        // still live
    if (e == nullptr) return;
    CHECK((e->flags & LightPool::kFlagDisabled) != 0);
    CHECK(!e->renderable());                    // fills nothing [orig: @0x5AA450 head]
    CHECK_NEAR(e->blend, 0.0f, 1e-6f);
    CHECK(pool.active_count() == 1);            // the slot is NOT returned to the pool
}

// State 1 with ticks -1 — the `light_move` in-flight glow: it never expires and its
// blend is never recomputed.
void test_state1_constant_never_expires() {
    LightPool pool;
    const int32_t pos[3] = { 0, 0, 0 };
    const int h = pool.spawn(pos, 25 * kUnit, 0x807850u, LightPool::kStateConstant, -1);
    for (int i = 0; i < 500; ++i) pool.tick();
    const LightPool::Light *e = pool.get(h);
    CHECK(e != nullptr);
    if (e == nullptr) return;
    CHECK(e->ticks_left == -1);
    CHECK_NEAR(e->blend, 1.0f, 1e-6f);
    // The round carries it: reposition follows, the release frees it.
    const int32_t moved[3] = { 5 * kUnit, 0, 0 };
    pool.set_position(h, moved);
    e = pool.get(h);
    CHECK(e != nullptr && e->pos[0] == moved[0]);
    CHECK(e != nullptr && e->bbox_max[0] == moved[0] + 25 * kUnit);
    pool.release(h);
    CHECK(pool.get(h) == nullptr);
}

// The muzzle glow's exact two-call shape [orig: Entity_UpdateMuzzleGlowEffect @0x56C960]:
// spawn once per shooter (state 3, ticks -1, radius 0x18000 = 1.5 u, colour 0xFFE0A0),
// then per shot re-arm to state 4 with 5 ticks and blend 1.0.
void test_muzzle_glow_spawn_then_rearm() {
    LightPool pool;
    const int32_t pos[3] = { 0, 2 * kUnit, 0 };
    const int h = pool.spawn(pos, 0x18000, 0xFFE0A0u, LightPool::kStateMuzzleParked, -1);
    const LightPool::Light *e = pool.get(h);
    CHECK(e != nullptr);
    if (e == nullptr) return;
    CHECK(e->state == LightPool::kStateMuzzleParked && e->ticks_left == -1);
    CHECK_NEAR(static_cast<float>(e->radius) / 65536.0f, 1.5f, 1e-6f);
    CHECK_NEAR(e->rgb[0], 255.0f / 256.0f, 1e-6f);
    CHECK_NEAR(e->rgb[1], 224.0f / 256.0f, 1e-6f);
    CHECK_NEAR(e->rgb[2], 160.0f / 256.0f, 1e-6f);

    pool.set_state_and_ticks(h, LightPool::kStateMuzzleShot, 5);
    pool.set_group(h, 0x1234, 0);
    pool.set_blend(h, 1.0f);
    e = pool.get(h);
    CHECK(e != nullptr);
    if (e == nullptr) return;
    CHECK(e->state == LightPool::kStateMuzzleShot);
    CHECK(e->ticks_left == 5 && e->ticks_total == 5);
    CHECK(e->group_entity == 0x1234);
    // State 4 is NOT in the fade set — the glow stays at full blend for its 5 ticks and
    // is then FREED (state != 5), which is the witnessed ~80 ms muzzle pop.
    for (int i = 0; i < 4; ++i) pool.tick();
    e = pool.get(h);
    CHECK(e != nullptr);
    if (e == nullptr) return;
    CHECK_NEAR(e->blend, 1.0f, 1e-6f);
    CHECK(e->ticks_left == 1);
    pool.tick();
    CHECK(pool.get(h) == nullptr);
}

// set_blend's own disable leg [orig: CEffectInstance_SetBlendAmount @0x5A8EE0].
void test_set_blend_drives_the_disabled_bit() {
    LightPool pool;
    const int32_t pos[3] = { 0, 0, 0 };
    const int h = pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1);
    pool.set_blend(h, 0.0f);
    CHECK(pool.get(h) != nullptr && !pool.get(h)->renderable());
    pool.set_blend(h, 1.0f);
    CHECK(pool.get(h) != nullptr && pool.get(h)->renderable());
    // The impact flash's render-flag stamp, carried but consumer-less.
    pool.modify_render_flags(h, LightPool::kFlagImpactRender, 0);
    CHECK((pool.get(h)->flags & LightPool::kFlagImpactRender) != 0);
    CHECK(pool.get(h)->renderable());   // 0x100 gates nothing here
}

// The allocator reuses the first FREE slot inside the active prefix before appending
// [orig: the `while (*slot_flags_ptr)` walk @0x5A8D50].
void test_free_slot_is_reused_before_appending() {
    LightPool pool;
    const int32_t pos[3] = { 0, 0, 0 };
    const int a = pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1);
    const int b = pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1);
    CHECK((a & LightPool::kHandleIndexMask) == 0);
    CHECK((b & LightPool::kHandleIndexMask) == 1);
    pool.release(a);
    const int c = pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1);
    CHECK((c & LightPool::kHandleIndexMask) == 0);   // slot 0 came back
    CHECK(pool.active_count() == 2);
    CHECK(pool.live_count() == 2);
}

// A full pool answers 0 — retail's own "no light" [orig: `if (active >= 4096) return 0`].
void test_pool_full_returns_zero() {
    LightPool pool;
    const int32_t pos[3] = { 0, 0, 0 };
    for (int i = 0; i < LightPool::kMaxLights; ++i)
        CHECK(pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1) != 0);
    CHECK(pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1) == 0);
    CHECK(pool.active_count() == LightPool::kMaxLights);
    pool.reset();
    CHECK(pool.active_count() == 0);
}

// The D3D fill [orig: Light_FillD3DPointLight @0x5AA450].
void test_fill_point_light_matches_the_d3d_math() {
    LightPool pool;
    const int32_t pos[3] = { 2 * kUnit, 4 * kUnit, 8 * kUnit };
    const int h = pool.spawn(pos, kGrenadeRadius, kWarm, LightPool::kStateFadeFree, 4);
    const float ambient[3] = { 1.0f, 1.0f, 1.0f };
    LightPool::PointLight pl;
    CHECK(pool.fill_point_light(h, ambient, &pl));
    CHECK_NEAR(pl.position[0], 2.0f, 1e-5f);
    CHECK_NEAR(pl.position[2], 8.0f, 1e-5f);
    // Range = 1.25 x radius units.
    CHECK_NEAR(pl.range, 15.0f * 1.25f, 1e-4f);
    // Diffuse = rgb x blend x ambient x 1.5 (blend is still 1.0 before the first tick).
    CHECK_NEAR(pl.diffuse[0], 255.0f / 256.0f * 1.5f, 1e-5f);
    CHECK_NEAR(pl.diffuse[2], 96.0f / 256.0f * 1.5f, 1e-5f);
    // atten = (1, 0, 15 / Range^2) -> exactly 1/16 brightness at Range.
    CHECK_NEAR(pl.atten_const, 1.0f, 1e-6f);
    CHECK_NEAR(pl.atten_linear, 0.0f, 1e-6f);
    CHECK_NEAR(pl.atten_quadratic, 15.0f / (pl.range * pl.range), 1e-6f);
    const float at_range = 1.0f / (pl.atten_const + pl.atten_quadratic * pl.range * pl.range);
    CHECK_NEAR(at_range, 1.0f / 16.0f, 1e-6f);
    // Half the fade in, the diffuse has halved and the range has NOT moved.
    pool.tick();
    pool.tick();
    CHECK(pool.fill_point_light(h, ambient, &pl));
    CHECK_NEAR(pl.diffuse[0], 255.0f / 256.0f * 0.5f * 1.5f, 1e-5f);
    CHECK_NEAR(pl.range, 15.0f * 1.25f, 1e-4f);
    // A disabled slot fills nothing.
    pool.set_blend(h, 0.0f);
    CHECK(!pool.fill_point_light(h, ambient, &pl));
    // So does a handle without the 0x8000 bit.
    CHECK(!pool.fill_point_light(0, ambient, &pl));
}

// The per-frame render selection: nearest-first by squared centre distance, capped at
// retail's four [orig: update_light_slots @0x5ABC50; order from the
// collect_nearby_zones_by_aabb @0x5AA250 tail bubble sort].
void test_collect_render_lights_nearest_first_capped_at_four() {
    LightPool pool;
    // Six renderable lights at increasing distance from the origin on X, spawned in
    // shuffled order so the sort has work to do.
    const float order[6] = { 40.0f, 10.0f, 60.0f, 20.0f, 50.0f, 30.0f };
    for (float x : order) {
        const int32_t pos[3] = { static_cast<int32_t>(x * kUnit), 0, 0 };
        CHECK(pool.spawn(pos, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1) != 0);
    }
    const float cam[3] = { 0.0f, 0.0f, 0.0f };
    const float ambient[3] = { 1.0f, 1.0f, 1.0f };
    LightPool::PointLight out[LightPool::kMaxSimultaneous];
    const int n = pool.collect_render_lights(cam, ambient, out, LightPool::kMaxSimultaneous);
    CHECK(n == 4);   // the hard cap [orig: @0x5ABC50]
    CHECK_NEAR(out[0].position[0], 10.0f, 1e-4f);
    CHECK_NEAR(out[1].position[0], 20.0f, 1e-4f);
    CHECK_NEAR(out[2].position[0], 30.0f, 1e-4f);
    CHECK_NEAR(out[3].position[0], 40.0f, 1e-4f);
    // A disabled slot never makes the list.
    LightPool pool2;
    const int32_t p0[3] = { 0, 0, 0 };
    const int h = pool2.spawn(p0, kUnit, 0xFFFFFFu, LightPool::kStateConstant, -1);
    pool2.set_blend(h, 0.0f);
    CHECK(pool2.collect_render_lights(cam, ambient, out, 4) == 0);
}

} // namespace

int main() {
    test_spawn_records_the_witnessed_fields();
    test_state2_fades_linearly_then_frees();
    test_state5_disables_and_keeps_the_slot();
    test_state1_constant_never_expires();
    test_muzzle_glow_spawn_then_rearm();
    test_set_blend_drives_the_disabled_bit();
    test_free_slot_is_reused_before_appending();
    test_pool_full_returns_zero();
    test_fill_point_light_matches_the_d3d_math();
    test_collect_render_lights_nearest_first_capped_at_four();
    if (failures == 0) std::printf("light_pool_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
