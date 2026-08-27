// The EffectWorld dynamic light pool (D-RLIT-4 implementation): spawn/query/select
// semantics pinned against the witnessed originals —
// [orig: LightPool_SpawnGlowEffect @ 0x5a8d50;
//  collect_nearby_zones_by_aabb @ 0x5aa250; Light_PassesActiveGroups @ 0x5a9120;
//  Light_SelectAndEnableForDraw @ 0x5ab9d0; the batch-entry 3-cap
//  collect_render_objects_for_batch @ 0x5d9229;
//  Light_GetPointLightParams @ 0x5a9180; Light_TickGenBlock @ 0x5a8ae0].
#include <runtime/renderer/light_scene.h>

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

renderer::LightSpawnParams barrel_params(int32_t x_fixed, int32_t y_fixed,
                                         int32_t z_fixed) {
    // The FireBrl3 shape: atten_end 8 -> radius_fixed 8<<16, white record
    // color, intensity 1, style-113 gen block.
    renderer::LightSpawnParams params;
    params.position_fixed = {x_fixed, y_fixed, z_fixed};
    params.radius_fixed = 8 << 16;
    params.rgb = {255, 255, 255};
    params.has_gen = true;
    params.gen.style = 113;
    params.gen.color_start = {255, 200, 120, 255};
    params.gen.color_end = {80, 40, 10, 255};
    return params;
}

}  // namespace

int main() {
    using namespace renderer;

    // Spawn: retail handle form, first-free reuse, no stale resurrection.
    {
        LightScene scene;
        const LightHandle a = scene.spawn(barrel_params(0, 0, 0));
        expect((a.retail_value & 0x8000) != 0,
               "handles carry the retail 0x8000 flag");
        expect(a.generation != 0, "live handles carry a nonzero generation");
        expect(scene.alive(a), "a spawned light is alive");
        const LightHandle b = scene.spawn(barrel_params(1 << 16, 0, 0));
        scene.despawn(a);
        expect(!scene.alive(a), "despawn kills the handle");
        expect(scene.alive(b), "other slots survive a despawn");
        const LightHandle c = scene.spawn(barrel_params(2 << 16, 0, 0));
        expect(c.retail_value == a.retail_value,
               "the first free retail slot is reused");
        expect(c.generation != a.generation,
               "a reused retail slot receives a new generation");
        expect(!scene.alive(a), "the stale generation stays dead after reuse");
        expect(scene.alive(c), "the reused slot is alive");
        expect(scene.inspect().live == 2, "live census counts reuse once");

        // Every stale mutator must be inert after reuse. If any call below
        // aliases c, it will hide, move, group-gate, kill, or arm c to die.
        scene.set_position(a, {99 << 16, 0, 0});
        scene.set_fade(a, 2, 1);
        scene.set_blend(a, 0.0f);
        scene.set_owner(a, 1234, 7);
        scene.despawn(a);
        scene.tick();
        expect(scene.alive(c), "stale mutation and despawn do not touch reuse");
        std::array<LightHandle, LightScene::kQueryLimit> reused_handles{};
        expect(scene.query({-(10 << 16), -(10 << 16), -(10 << 16)},
                       {10 << 16, 10 << 16, 10 << 16}, reused_handles) == 2,
               "stale setters leave the reused light visible and in place");
        std::array<SelectedLight, LightScene::kSelectLimit> reused_out{};
        expect(scene.select(reused_handles.data(), 2, LightActiveGroups{},
                       LightSelectionOptions{}, {1.0f, 1.0f, 1.0f},
                       LightFlickerInputs{}, false, reused_out) == 2,
               "stale owner changes do not group-gate the reused light");
        std::array<LightHandle, 1> stale_handles = {a};
        expect(scene.select(stale_handles.data(), stale_handles.size(),
                       LightActiveGroups{}, LightSelectionOptions{},
                       {1.0f, 1.0f, 1.0f}, LightFlickerInputs{}, false,
                       reused_out) == 0,
               "select rejects an old generation for a reused retail slot");
    }

    // clear() invalidates all handles without resetting the generation source.
    {
        LightScene scene;
        const LightHandle before = scene.spawn(barrel_params(0, 0, 0));
        scene.clear();
        expect(!scene.alive(before), "clear invalidates the old handle");
        const LightHandle after = scene.spawn(barrel_params(0, 0, 0));
        expect(after.retail_value == before.retail_value,
               "clear permits reuse of the retail slot number");
        expect(after.generation != before.generation,
               "the generation source survives clear");
        scene.set_position(before, {99 << 16, 0, 0});
        scene.set_fade(before, 2, 1);
        scene.set_blend(before, 0.0f);
        scene.set_owner(before, 1234, 7);
        scene.despawn(before);
        scene.tick();
        expect(scene.alive(after), "pre-clear mutations cannot touch new slots");
    }

    // Query: AABB overlap + nearest-first ordering from the box center.
    {
        LightScene scene;
        const LightHandle far_light =
                scene.spawn(barrel_params(40 << 16, 0, 0));
        const LightHandle near_light =
                scene.spawn(barrel_params(10 << 16, 0, 0));
        scene.spawn(barrel_params(500 << 16, 0, 0));  // outside the query box
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        const std::array<int32_t, 3> qmin = {-(50 << 16), -(50 << 16),
                                             -(50 << 16)};
        const std::array<int32_t, 3> qmax = {50 << 16, 50 << 16, 50 << 16};
        const size_t count = scene.query(qmin, qmax, handles);
        expect(count == 2, "only overlapping lights are collected");
        expect(handles[0] == near_light,
               "the nearest light sorts first [orig: @ 0x5aa3a8 bubble]");
        expect(handles[1] == far_light, "the farther light sorts second");
    }

    // The squared 16.16 metric must not overflow at ordinary distances.
    // 181/182 units straddle the old signed-int32 distance-term limit.
    {
        LightScene scene;
        const std::array<int32_t, 9> positions = {
                200, 4, 2, 1, 3, 512, 182, 256, 181};
        std::array<LightHandle, positions.size()> spawned{};
        for (size_t i = 0; i < positions.size(); ++i) {
            spawned[i] = scene.spawn(
                    barrel_params(positions[i] << 16, 0, 0));
        }
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        const size_t count = scene.query({-(600 << 16), -1, -1},
                {600 << 16, 1, 1}, handles);
        expect(count == positions.size(), "the ordering probe overlaps all lights");
        const std::array<int32_t, 9> expected = {
                1, 2, 3, 4, 181, 182, 200, 256, 512};
        for (size_t rank = 0; rank < expected.size(); ++rank) {
            bool matches = false;
            for (size_t i = 0; i < positions.size(); ++i) {
                if (positions[i] == expected[rank] &&
                        handles[rank] == spawned[i]) {
                    matches = true;
                    break;
                }
            }
            expect(matches, "distance ordering stays monotonic past 181 units");
        }
    }

    // Retail's witnessed per-draw query stops after its first 64 overlaps.
    // The camera-global adapter needs a separately named query that considers
    // every overlap before returning the nearest 64.
    {
        LightScene scene;
        std::array<LightHandle, LightScene::kQueryLimit> witnessed{};
        for (size_t i = 0; i < LightScene::kQueryLimit; ++i) {
            scene.spawn(barrel_params(
                    static_cast<int32_t>((100 + i) << 16), 0, 0));
        }
        const LightHandle near = scene.spawn(barrel_params(0, 0, 0));
        const std::array<int32_t, 3> qmin = {-(1000 << 16), -1, -1};
        const std::array<int32_t, 3> qmax = {1000 << 16, 1, 1};
        expect(scene.query(qmin, qmax, witnessed) == LightScene::kQueryLimit,
               "the witnessed query retains its first-64 scan cap");
        bool witnessed_has_near = false;
        for (const LightHandle handle : witnessed) {
            witnessed_has_near = witnessed_has_near || handle == near;
        }
        expect(!witnessed_has_near,
               "the witnessed query does not reach overlap number 65");

        std::array<LightHandle, LightScene::kQueryLimit> camera{};
        expect(scene.query_camera_global(qmin, qmax, camera) ==
                       LightScene::kQueryLimit,
               "the camera-global query also returns at most 64");
        expect(camera[0] == near,
               "the camera-global query considers all overlaps before capping");
    }

    // Clamped AABBs and unsigned distance math tolerate the full int32 range.
    {
        LightScene scene;
        LightSpawnParams low = barrel_params(INT32_MIN, 0, 0);
        low.radius_fixed = INT32_MAX;
        LightSpawnParams high = barrel_params(INT32_MAX, 0, 0);
        high.radius_fixed = INT32_MAX;
        const LightHandle low_handle = scene.spawn(low);
        const LightHandle high_handle = scene.spawn(high);
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        const size_t count = scene.query_camera_global(
                {INT32_MIN, INT32_MIN, INT32_MIN},
                {INT32_MAX, INT32_MAX, INT32_MAX}, handles);
        expect(count == 2, "extreme clamped AABBs remain queryable");
        expect((handles[0] == low_handle || handles[0] == high_handle) &&
                       (handles[1] == low_handle || handles[1] == high_handle) &&
                       handles[0] != handles[1],
               "extreme distance calculation returns both unique handles");
    }

    // Select: witnessed params — position/w, color x ambient x 1.5 (d3d
    // path), the {1, 0, 15/r^2, 1} attenuation, range = fixed*1.25/65536.
    {
        LightScene scene;
        LightSpawnParams params = barrel_params(4 << 16, 8 << 16, 12 << 16);
        params.has_gen = false;  // static color for the exact-value pin
        params.rgb = {128, 64, 32};
        const LightHandle handle = scene.spawn(params);
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        handles[0] = handle;
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        const std::array<float, 3> ambient = {1.0f, 1.0f, 1.0f};
        const size_t selected = scene.select(handles.data(), 1,
                LightActiveGroups{}, LightSelectionOptions{}, ambient,
                LightFlickerInputs{}, true, out);
        expect(selected == 1, "one light selects");
        expect(nearly_equal(out[0].position[0], 4.0f) &&
               nearly_equal(out[0].position[1], 8.0f) &&
               nearly_equal(out[0].position[2], 12.0f),
               "position converts 16.16 -> float world");
        expect(nearly_equal(out[0].position_w, 65536.0f / (8 << 16)),
               "position w = 65536/radius_fixed [orig: @ 0x5a91d4]");
        // record bytes /256 [orig: @ 0x5a8e51], then the 1.5x d3d boost.
        expect(nearly_equal(out[0].color[0], 128.0f / 256.0f * 1.5f) &&
               nearly_equal(out[0].color[1], 64.0f / 256.0f * 1.5f) &&
               nearly_equal(out[0].color[2], 32.0f / 256.0f * 1.5f),
               "color = bytes/256 x ambient x 1.5 on the d3d path");
        expect(nearly_equal(out[0].range, 10.0f),
               "range = radius_fixed * 1.25 / 65536");
        expect(nearly_equal(out[0].attenuation[0], 1.0f) &&
               nearly_equal(out[0].attenuation[1], 0.0f) &&
               nearly_equal(out[0].attenuation[2], 0.15f) &&
               nearly_equal(out[0].attenuation[3], 1.0f),
               "attenuation = {1, 0, 15/range^2, 1}");
    }

    // Select cap: exactly three of six pass — the batch entry stores three
    // handles and breaks the visible walk there [orig:
    // collect_render_objects_for_batch @ 0x5d9226..0x5d9229;
    // collect_render_batches_for_entity @ 0x5d96e6..0x5d96e9]. The > 4 clamp
    // @ 0x5abbeb is the transient D3D LightEnable count FlushBatches tears
    // down per entry (@ 0x5da5de), never a lit-strip count.
    {
        expect(LightScene::kSelectLimit == 3 && kLightSelectLimit == 3,
               "the select cap is the witnessed batch-entry three");
        LightScene scene;
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        for (int i = 0; i < 6; ++i) {
            handles[static_cast<size_t>(i)] =
                    scene.spawn(barrel_params(i << 16, 0, 0));
        }
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        const size_t selected = scene.select(handles.data(), 6,
                LightActiveGroups{}, LightSelectionOptions{},
                {1.0f, 1.0f, 1.0f}, LightFlickerInputs{}, false, out);
        expect(selected == 3, "at most three lights select");
        for (size_t i = 0; i < selected; ++i) {
            expect(out[i].handle == handles[i],
                   "the three survivors are the first three in walk order");
        }
    }

    // Target-disable flags filter before the three-light cap. A disabled near
    // light must not starve the third eligible light behind it
    // [orig: the objects-enable gate @ 0x5a9010 runs inside the walk
    // @ 0x5d920c, before the store that counts toward the cap @ 0x5d921f].
    {
        LightScene scene;
        std::array<LightHandle, 5> handles{};
        LightSpawnParams disabled = barrel_params(0, 0, 0);
        disabled.disable_objects = true;
        handles[0] = scene.spawn(disabled);
        for (size_t i = 1; i < handles.size(); ++i) {
            handles[i] = scene.spawn(
                    barrel_params(static_cast<int32_t>(i << 16), 0, 0));
        }
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        LightSelectionOptions object_options;
        object_options.target = LightSelectionTarget::Objects;
        size_t selected = scene.select(handles.data(), handles.size(),
                LightActiveGroups{}, object_options, {1.0f, 1.0f, 1.0f},
                LightFlickerInputs{}, false, out);
        expect(selected == 3, "three eligible object lights survive pre-cap filtering");
        for (const SelectedLight &light : out) {
            expect(light.handle != handles[0],
                   "disable_objects excludes a light from the object pass");
        }

        LightScene all_disabled;
        std::array<LightHandle, 5> disabled_handles{};
        for (size_t i = 0; i < disabled_handles.size(); ++i) {
            disabled_handles[i] = all_disabled.spawn(disabled);
        }
        selected = all_disabled.select(disabled_handles.data(),
                disabled_handles.size(), LightActiveGroups{}, object_options,
                {1.0f, 1.0f, 1.0f}, LightFlickerInputs{}, false, out);
        expect(selected == 0, "an all-disable_objects list selects no object lights");

        LightScene terrain_scene;
        LightSpawnParams no_terrain = barrel_params(0, 0, 0);
        no_terrain.disable_terrain = true;
        LightSpawnParams no_objects = barrel_params(1 << 16, 0, 0);
        no_objects.disable_objects = true;
        std::array<LightHandle, 2> target_handles = {
                terrain_scene.spawn(no_terrain), terrain_scene.spawn(no_objects)};
        LightSelectionOptions terrain_options;
        terrain_options.target = LightSelectionTarget::Terrain;
        selected = terrain_scene.select(target_handles.data(),
                target_handles.size(), LightActiveGroups{}, terrain_options,
                {1.0f, 1.0f, 1.0f}, LightFlickerInputs{}, false, out);
        expect(selected == 1 && out[0].handle == target_handles[1],
               "terrain selection ignores disable_objects but honors disable_terrain");
    }

    // Group gate [orig: Light_PassesActiveGroups @ 0x5a9120..0x5a916e]: unowned
    // lights always pass; owned lights need the matching active group.
    {
        LightScene scene;
        LightSpawnParams owned = barrel_params(0, 0, 0);
        owned.owner_entity = 77;
        owned.owner_section = 3;
        const LightHandle owned_handle = scene.spawn(owned);
        const LightHandle free_handle =
                scene.spawn(barrel_params(1 << 16, 0, 0));
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        handles[0] = owned_handle;
        handles[1] = free_handle;
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        LightActiveGroups groups{};
        size_t selected = scene.select(handles.data(), 2, groups,
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f},
                LightFlickerInputs{}, false, out);
        expect(selected == 1 && out[0].handle == free_handle,
               "an owned light is gated out with no active group");
        groups.interior_group_entity = 77;
        groups.interior_group_section = 3;
        selected = scene.select(handles.data(), 2, groups,
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f},
                LightFlickerInputs{}, false, out);
        expect(selected == 2, "the matching interior group admits the light");
        groups.interior_group_section = 9;
        selected = scene.select(handles.data(), 2, groups,
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f},
                LightFlickerInputs{}, false, out);
        expect(selected == 1, "a mismatched section gates the light out");
        groups = LightActiveGroups{};
        groups.owner_group_entity = 77;
        selected = scene.select(handles.data(), 2, groups,
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f},
                LightFlickerInputs{}, false, out);
        expect(selected == 2, "the owner group admits its light");
        groups = LightActiveGroups{};
        LightSelectionOptions fallback;
        fallback.admit_owned_unscoped = true;
        selected = scene.select(handles.data(), 2, groups, fallback,
                {1.0f, 1.0f, 1.0f}, LightFlickerInputs{}, false, out);
        expect(selected == 2,
               "the explicit camera-global fallback admits owned lights");
    }

    // Flicker: style 113 reads the position-hashed wave-ring value as its
    // ctrl [orig: Light_TickGenBlock @ 0x5a8ae0 -> ctrl slot FLICKER].
    {
        LightScene scene;
        LightSpawnParams params = barrel_params(0, 0, 0);
        const LightHandle handle = scene.spawn(params);
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        handles[0] = handle;
        std::array<int32_t, 256> ring{};
        LightFlickerInputs flicker;
        flicker.amp_ring = ring.data();
        flicker.amp_ring_size = ring.size();
        flicker.ring_index = 0;
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        // ctrl 0 -> the start color; ctrl 65536 -> the end color.
        ring.fill(0);
        scene.select(handles.data(), 1, LightActiveGroups{},
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f}, flicker, false,
                out);
        const float start_r = out[0].color[0];
        ring.fill(65536);
        scene.select(handles.data(), 1, LightActiveGroups{},
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f}, flicker, false,
                out);
        const float end_r = out[0].color[0];
        expect(start_r > end_r,
               "style 113 interpolates start -> end with the ring value");
        // The position hash: two lights at different positions read different
        // ring cells.
        for (size_t i = 0; i < ring.size(); ++i) {
            ring[i] = static_cast<int32_t>(i << 8);
        }
        const int32_t at_origin =
                light_flicker_value({0, 0, 0}, flicker);
        const int32_t offset_pos =
                light_flicker_value({1 << 20, 0, 0}, flicker);
        expect(at_origin != offset_pos,
               "the flicker phase is position-hashed [orig: @ 0x5a8b00]");
    }

    {
        // The fade lifecycle [orig: EffectWorld_TickInstancesAndLightScale
        // @ 0x5aa170]. Mode 2 blends down d16/d17 per tick and dies at zero
        // (the impact/death flash shape).
        LightScene scene;
        renderer::LightSpawnParams params = barrel_params(0, 0, 0);
        params.has_gen = false;
        params.fade_mode = 2;
        params.fade_duration = 4;
        const renderer::LightHandle handle = scene.spawn(params);
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        renderer::LightFlickerInputs flicker;
        scene.tick();  // counter 4 -> 3, blend 3/4
        size_t found = scene.query({-1, -1, -1}, {1, 1, 1}, handles);
        expect(found == 1, "a fading light still queries");
        scene.select(handles.data(), found, LightActiveGroups{},
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f}, flicker, false,
                out);
        expect(nearly_equal(out[0].color[0], (255.0f / 256.0f) * 0.75f),
               "mode 2 renders blend = counter / initial [orig: @ 0x5aa1de]");
        scene.tick();  // 3 -> 2
        scene.tick();  // 2 -> 1
        expect(scene.alive(handle), "the light survives to the last tick");
        scene.tick();  // 1 -> 0: mode 2 dies [orig: memset @ 0x5aa1b9]
        expect(!scene.alive(handle), "an expired mode-2 light despawns");
        expect(scene.query({-1, -1, -1}, {1, 1, 1}, handles) == 0,
               "an expired light stops querying");
    }

    {
        // Mode 5 hides at expiry instead of dying, and SetBlendAmount >= 0.001
        // re-shows the kept slot [orig: @ 0x5aa1c3; CEffectInstance_
        // SetBlendAmount @ 0x5a8f11].
        LightScene scene;
        renderer::LightSpawnParams params = barrel_params(0, 0, 0);
        params.has_gen = false;
        params.fade_mode = 5;
        params.fade_duration = 2;
        const renderer::LightHandle handle = scene.spawn(params);
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        scene.tick();
        scene.tick();
        expect(scene.alive(handle), "an expired mode-5 light keeps its slot");
        expect(scene.query({-1, -1, -1}, {1, 1, 1}, handles) == 0,
               "an expired mode-5 light is hidden from queries");
        scene.set_blend(handle, 1.0f);
        expect(scene.query({-1, -1, -1}, {1, 1, 1}, handles) == 1,
               "SetBlendAmount >= 0.001 re-shows a hidden slot");
    }

    {
        // The muzzle-glow arm sequence [orig: Entity_UpdateMuzzleGlowEffect
        // @ 0x56c960]: spawn (mode 3, duration -1), re-arm to mode 4 /
        // duration 5 + blend 1 — five ticks later the slot dies (mode 4 is
        // not the keep-slot mode).
        LightScene scene;
        renderer::LightSpawnParams params;
        params.position_fixed = {0, 0, 0};
        params.radius_fixed = 98304;  // 1.5 [orig: @ 0x56c98f]
        params.rgb = {0xFF, 0xE0, 0xA0};
        params.fade_mode = 3;
        params.fade_duration = -1;
        const renderer::LightHandle handle = scene.spawn(params);
        scene.set_fade(handle, 4, 5);
        scene.set_blend(handle, 1.0f);
        for (int i = 0; i < 4; ++i) {
            scene.tick();
        }
        expect(scene.alive(handle), "the muzzle glow lives through tick 4");
        scene.set_fade(handle, 4, 5);  // a next shot re-arms the countdown
        for (int i = 0; i < 4; ++i) {
            scene.tick();
        }
        expect(scene.alive(handle), "a re-armed muzzle glow keeps living");
        scene.tick();
        expect(!scene.alive(handle),
               "five ticks after the last shot the muzzle glow dies");
        // The stale handle is now inert (our liveness gate; retail writes
        // through it — the tracked divergence).
        scene.set_fade(handle, 4, 5);
        scene.set_blend(handle, 1.0f);
        expect(!scene.alive(handle), "re-arming a dead handle is a no-op");
    }

    {
        // Mode 1 / duration -1 (the model-light and round-glow shape) never
        // decays [orig: d16 <= 0 skips the decrement @ 0x5aa1a2].
        LightScene scene;
        renderer::LightSpawnParams params = barrel_params(0, 0, 0);
        params.has_gen = false;
        const renderer::LightHandle handle = scene.spawn(params);
        for (int i = 0; i < 100; ++i) {
            scene.tick();
        }
        expect(scene.alive(handle), "a permanent light survives the tick");
        std::array<LightHandle, LightScene::kQueryLimit> handles{};
        std::array<SelectedLight, LightScene::kSelectLimit> out{};
        renderer::LightFlickerInputs flicker;
        const size_t found = scene.query({-1, -1, -1}, {1, 1, 1}, handles);
        scene.select(handles.data(), found, LightActiveGroups{},
                LightSelectionOptions{}, {1.0f, 1.0f, 1.0f}, flicker, false,
                out);
        expect(nearly_equal(out[0].color[0], 255.0f / 256.0f),
               "a permanent light's blend stays 1.0");
    }

    // The fixed 4096-slot capacity remains exact, and reuse at capacity gets
    // a fresh generation without increasing the high-water mark.
    {
        LightScene scene;
        std::array<LightHandle, LightScene::kCapacity> handles{};
        LightSpawnParams params = barrel_params(0, 0, 0);
        for (size_t i = 0; i < handles.size(); ++i) {
            handles[i] = scene.spawn(params);
            expect(!handles[i].is_null(), "all 4096 retail slots can spawn");
        }
        expect(scene.spawn(params).is_null(),
               "spawn 4097 returns a null handle");
        expect(scene.inspect().live == LightScene::kCapacity &&
                       scene.inspect().high_water == LightScene::kCapacity,
               "capacity census and high-water are exact");
        const LightHandle stale = handles[2048];
        scene.despawn(stale);
        const LightHandle reused = scene.spawn(params);
        expect(reused.retail_value == stale.retail_value &&
                       reused.generation != stale.generation,
               "capacity reuse keeps the retail slot and advances generation");
        expect(!scene.alive(stale) && scene.alive(reused),
               "capacity reuse rejects the stale generation");
    }

    // select_for_draws: per-draw owner isolation over one snapshot
    // [orig: the per-draw collect @ 0x5aa250 gated by Light_PassesActiveGroups
    // @ 0x5a9120 with that draw's owner/interior groups].
    {
        LightScene scene;
        LightSpawnParams world_light = barrel_params(0, 0, 0);
        const LightHandle world_handle = scene.spawn(world_light);
        LightSpawnParams owned = barrel_params(1 << 16, 0, 0);
        owned.has_gen = false;
        owned.owner_entity = 77;
        const LightHandle owned_handle = scene.spawn(owned);
        expect(scene.alive(world_handle) && scene.alive(owned_handle),
               "fixture lights spawn");

        std::array<LightDrawContext, 2> draws{};
        // Draw 0: entity 77's own draw context.
        draws[0].aabb_min_fixed = {-(16 << 16), -(16 << 16), -(16 << 16)};
        draws[0].aabb_max_fixed = {16 << 16, 16 << 16, 16 << 16};
        draws[0].groups.owner_group_entity = 77;
        // Draw 1: an unrelated entity in the same space.
        draws[1] = draws[0];
        draws[1].groups.owner_group_entity = 12;

        std::array<LightDrawSelection, 2> out{};
        LightSelectionOptions options;
        options.target = LightSelectionTarget::Objects;
        options.admit_owned_unscoped = false;
        const std::array<float, 3> ambient = {1.0f, 1.0f, 1.0f};
        scene.select_for_draws(draws.data(), draws.size(), options, ambient,
                               LightFlickerInputs{}, false, out.data());
        expect(out[0].count == 2,
               "the owner draw receives the world light AND its owned light");
        expect(out[1].count == 1,
               "an unrelated draw receives only the world light");
        bool owner_saw_owned = false;
        for (size_t i = 0; i < out[0].count; ++i) {
            if (out[0].lights[i].handle == owned_handle) {
                owner_saw_owned = true;
            }
        }
        expect(owner_saw_owned, "the owned light reaches its owner draw");
        for (size_t i = 0; i < out[1].count; ++i) {
            expect(out[1].lights[i].handle != owned_handle,
                   "the owned light never reaches another entity's draw");
        }
    }

    // select_for_draws: target disables gate at select, hidden slots gate at
    // collection, and the per-draw 3-cap applies after the owner filter.
    {
        LightScene scene;
        LightSpawnParams hidden_light = barrel_params(0, 0, 0);
        const LightHandle hidden_handle = scene.spawn(hidden_light);
        scene.set_blend(hidden_handle, 0.0f);  // flag bit 2: out of collection
        LightSpawnParams no_objects = barrel_params(0, 0, 0);
        no_objects.has_gen = false;
        no_objects.disable_objects = true;
        scene.spawn(no_objects);
        // Five world lights at increasing distance from the draw center; the
        // two farthest must lose the 3-cap.
        std::array<LightHandle, 5> world{};
        for (int i = 0; i < 5; ++i) {
            LightSpawnParams params = barrel_params((i + 1) << 16, 0, 0);
            params.has_gen = false;
            world[static_cast<size_t>(i)] = scene.spawn(params);
        }
        LightDrawContext draw{};
        draw.aabb_min_fixed = {-(64 << 16), -(64 << 16), -(64 << 16)};
        draw.aabb_max_fixed = {64 << 16, 64 << 16, 64 << 16};
        LightDrawSelection out{};
        LightSelectionOptions options;
        options.target = LightSelectionTarget::Objects;
        options.admit_owned_unscoped = false;
        const std::array<float, 3> ambient = {1.0f, 1.0f, 1.0f};
        scene.select_for_draws(&draw, 1, options, ambient,
                               LightFlickerInputs{}, false, &out);
        expect(out.count == LightScene::kSelectLimit && out.count == 3,
               "the per-draw cap is the witnessed three");
        for (size_t i = 0; i < out.count; ++i) {
            expect(out.lights[i].handle != hidden_handle,
                   "hidden slots never collect");
            expect(out.lights[i].lights_objects,
                   "object-disabled lights never pass the object select");
            expect(out.lights[i].handle != world[3] &&
                           out.lights[i].handle != world[4],
                   "nearest-first ordering drops the farthest lights at the cap");
        }
        // Nearest ordering: selections come back ascending by distance.
        for (size_t i = 1; i < out.count; ++i) {
            expect(out.lights[i - 1].position[0] <= out.lights[i].position[0],
                   "selection preserves the nearest-first order");
        }
    }

    // select_for_draws: the witnessed two-level shape — ONE collect per drawn
    // entity (its own position -/+ boundRadius box @ 0x5c74fb..0x5c753a,
    // nearest-first) and then a per-ROBJ re-gate of that same ordered list
    // through the owner group + the 3-cap [orig: Lighting_SetOwnerLightGroup
    // (0, robjIndex) @ 0x5d8ff7 then the walk @ 0x5d91e0..0x5d9229]. Two ROBJ
    // draws stamped with the entity box must see the same world order and
    // differ only by their owned light; a cap-displaced world light proves the
    // owned light competes in nearest order, not as an extra slot.
    {
        LightScene scene;
        // World lights at 1, 2, 4 wu; owned lights per ROBJ section at 3 wu.
        const LightHandle w1 = scene.spawn(barrel_params(1 << 16, 0, 0));
        const LightHandle w2 = scene.spawn(barrel_params(2 << 16, 0, 0));
        const LightHandle w4 = scene.spawn(barrel_params(4 << 16, 0, 0));
        LightSpawnParams sec1 = barrel_params(3 << 16, 0, 0);
        sec1.has_gen = false;
        sec1.owner_entity = 500;
        sec1.owner_section = 1;
        const LightHandle owned1 = scene.spawn(sec1);
        LightSpawnParams sec2 = sec1;
        sec2.owner_section = 2;
        const LightHandle owned2 = scene.spawn(sec2);

        // The building's entity box (position 0, boundRadius 16) stamped on
        // BOTH ROBJ draws; interior group = the building at section 0, owner
        // group = (0, robjIndex) — the Terrain_RenderSectorModels shape.
        std::array<LightDrawContext, 2> draws{};
        for (size_t d = 0; d < draws.size(); ++d) {
            draws[d].aabb_min_fixed = {-(16 << 16), -(16 << 16), -(16 << 16)};
            draws[d].aabb_max_fixed = {16 << 16, 16 << 16, 16 << 16};
            draws[d].groups.interior_group_entity = 500;
            draws[d].groups.interior_group_section = 0;
            draws[d].groups.owner_group_entity = 0;
            draws[d].groups.owner_group_section = static_cast<int32_t>(d + 1);
        }
        std::array<LightDrawSelection, 2> out{};
        LightSelectionOptions options;
        options.target = LightSelectionTarget::Objects;
        options.admit_owned_unscoped = false;
        const std::array<float, 3> ambient = {1.0f, 1.0f, 1.0f};
        scene.select_for_draws(draws.data(), draws.size(), options, ambient,
                               LightFlickerInputs{}, false, out.data());
        expect(out[0].count == 3 && out[1].count == 3,
               "each ROBJ draw fills the three-light cap");
        expect(out[0].lights[0].handle == w1 && out[0].lights[1].handle == w2,
               "ROBJ 1 sees the entity-box nearest order first");
        expect(out[0].lights[2].handle == owned1,
               "ROBJ 1's third slot is its own section light (3 wu beats 4 wu)");
        expect(out[1].lights[0].handle == w1 && out[1].lights[1].handle == w2,
               "ROBJ 2 sees the same entity-box order");
        expect(out[1].lights[2].handle == owned2,
               "ROBJ 2's third slot is ITS section light");
        for (size_t d = 0; d < out.size(); ++d) {
            for (size_t i = 0; i < out[d].count; ++i) {
                expect(out[d].lights[i].handle != w4,
                       "the 4 wu world light loses the cap to the owned 3 wu light");
                expect(out[d].lights[i].handle != (d == 0 ? owned2 : owned1),
                       "a section light never reaches the other ROBJ");
            }
        }
    }

    // Corona quads [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40]:
    // three shrinking segments marching toward the camera, the plane fade,
    // the disable gate, the 100-wu cull, and the frame jitter.
    {
        LightScene scene;
        LightSpawnParams params;
        params.position_fixed = {0, 0, 0};
        params.radius_fixed = 4 << 16;  // radius 4 -> base half-size 2
        params.rgb = {128, 64, 32};
        const LightHandle handle = scene.spawn(params);
        expect(scene.alive(handle), "the corona light spawns");

        LightCoronaFrameInputs inputs;
        inputs.camera_fixed = {0, 0, 10 << 16};  // 10 wu away along +z
        // Depth plane facing the light from the camera: depth grows away
        // from the camera along -z.
        inputs.depth_plane_normal = {0.0f, 0.0f, -1.0f};
        inputs.depth_plane_w = 10.0f;
        std::vector<LightCoronaQuad> quads;
        expect(scene.collect_corona_quads(inputs, quads) == 3,
               "an enabled corona draws the witnessed three segments");
        // Segment centers march 0.1 x radius = 0.4 wu toward the camera.
        expect(nearly_equal(quads[0].center[2], 0.4f, 0.01f) &&
                       nearly_equal(quads[1].center[2], 0.8f, 0.01f) &&
                       nearly_equal(quads[2].center[2], 1.2f, 0.01f),
               "segments step 0.1 x radius toward the camera");
        // Half-sizes: 2.0, then x0.66 per segment.
        expect(nearly_equal(quads[0].half_size, 2.0f) &&
                       nearly_equal(quads[1].half_size, 1.32f) &&
                       nearly_equal(quads[2].half_size, 0.8712f),
               "segment half-sizes start at radius/2 and shrink x0.66");
        // Deep in front of the plane the fade clamps to 1: color =
        // bytes/256 x 1/16.
        expect(nearly_equal(quads[0].rgb[0], 128.0f / 256.0f / 16.0f) &&
                       nearly_equal(quads[0].rgb[1], 64.0f / 256.0f / 16.0f) &&
                       nearly_equal(quads[0].rgb[2], 32.0f / 256.0f / 16.0f),
               "corona color is record rgb x 1/16 at full fade");
        // The x jitter rides frame & 3 (512 fixed = 1/128 wu).
        inputs.frame_index = 1;
        scene.collect_corona_quads(inputs, quads);
        // The march toward the camera adds a sub-millimeter x component on
        // top of the jitter; assert within half the jitter magnitude.
        expect(nearly_equal(quads[0].center[0], -512.0f / 65536.0f, 4e-3f) &&
                       quads[0].center[0] < 0.0f,
               "odd frames jitter x by -512 fixed");

        // A camera plane near the light fades the segments by
        // depth / (radius/2) and skips non-positive depths.
        inputs.frame_index = 0;
        inputs.depth_plane_w = -0.6f;  // depth(z) = -z - 0.6 + 10 - 10...
        inputs.depth_plane_normal = {0.0f, 0.0f, 1.0f};
        // depth(center) = z - 0.6: segment 1 at 0.4 -> -0.2 skipped,
        // segment 2 at 0.8 -> 0.2 -> fade 0.1, segment 3 at 1.2 -> 0.6 ->
        // fade 0.3.
        expect(scene.collect_corona_quads(inputs, quads) == 2,
               "segments behind the camera plane are skipped");
        expect(nearly_equal(quads[0].rgb[0], 128.0f / 256.0f / 16.0f * 0.1f,
                       1e-4f),
               "the plane fade scales the corona color by depth/(radius/2)");

        // The authored corona disable and the 100-wu cull.
        LightSpawnParams disabled = params;
        disabled.position_fixed = {8 << 16, 0, 0};
        disabled.disable_corona = true;
        scene.spawn(disabled);
        inputs.depth_plane_normal = {0.0f, 0.0f, -1.0f};
        inputs.depth_plane_w = 10.0f;
        expect(scene.collect_corona_quads(inputs, quads) == 3,
               "a corona-disabled record draws no quads");
        LightSpawnParams far_light = params;
        far_light.position_fixed = {0, 0, -(120 << 16)};  // 130 wu away
        scene.spawn(far_light);
        expect(scene.collect_corona_quads(inputs, quads) == 3,
               "coronas cull beyond 100 wu from the camera");
    }

    // Corona owner visible-section gate [orig: the sectorFilter leg
    // @ 0x5ab027 -> Terrain_IsBuildingSectionBitSet @ 0x5c6960]: an owned
    // corona draws only when its owner's section bit is set; owners absent
    // from the mask table pass like retail's non-pool-2 owners.
    {
        LightScene scene;
        LightSpawnParams params;
        params.position_fixed = {0, 0, 0};
        params.radius_fixed = 4 << 16;
        params.rgb = {255, 255, 255};
        params.owner_entity = 42;
        params.owner_section = 2;
        scene.spawn(params);

        LightCoronaFrameInputs inputs;
        inputs.camera_fixed = {0, 0, 10 << 16};
        inputs.depth_plane_normal = {0.0f, 0.0f, -1.0f};
        inputs.depth_plane_w = 10.0f;
        std::vector<LightCoronaQuad> quads;
        expect(scene.collect_corona_quads(inputs, quads) == 3,
               "an owned corona with no mask table passes the gate");
        LightCoronaOwnerMask mask_row{42, ~(1u << 2)};
        inputs.owner_masks = &mask_row;
        inputs.owner_mask_count = 1;
        expect(scene.collect_corona_quads(inputs, quads) == 0,
               "a hidden owner section suppresses the owned corona");
        mask_row.section_mask = 1u << 2;
        expect(scene.collect_corona_quads(inputs, quads) == 3,
               "a visible owner section admits the owned corona");
    }

    // The flag-0x100 impact re-center [orig: @ 0x5ab037..0x5ab05c] and the
    // fog-to-black fold [orig: CD3DDevice_SetFogAndBlendMode(dev, 2)
    // @ 0x5aafb6].
    {
        LightScene scene;
        LightSpawnParams params;
        params.position_fixed = {0, 0, 0};
        params.radius_fixed = 4 << 16;
        params.rgb = {128, 128, 128};
        params.corona_lower_half_radius = true;
        scene.spawn(params);

        LightCoronaFrameInputs inputs;
        inputs.camera_fixed = {0, 0, 10 << 16};
        inputs.depth_plane_normal = {0.0f, 0.0f, -1.0f};
        inputs.depth_plane_w = 10.0f;
        std::vector<LightCoronaQuad> quads;
        expect(scene.collect_corona_quads(inputs, quads) == 3,
               "the re-centered corona still draws its segments");
        // The light drops radius/2 = 2 wu on the height axis, then marches
        // toward the camera: first segment at -2 + step.
        expect(quads[0].center[2] < -1.0f,
               "flag-0x100 coronas re-center radius/2 below the light");

        // Fog type 1 (linear, authored start): start 5, end 15; the first
        // segment sits ~11.6 wu from the camera (the drop moved the light
        // away) -> visibility (15 - 11.6) / 10 = 0.34.
        inputs.fog_enabled = true;
        inputs.fog_type = 1;
        inputs.fog_start = 5.0f;
        inputs.fog_end = 15.0f;
        std::vector<LightCoronaQuad> fogged;
        scene.collect_corona_quads(inputs, fogged);
        expect(fogged.size() == 3, "fogged coronas keep their segments");
        expect(nearly_equal(fogged[0].rgb[0], quads[0].rgb[0] * 0.34f,
                       quads[0].rgb[0] * 0.02f),
               "the fog-to-black fold scales the corona color by the "
               "primary fog factor");
        // Past the fog end the corona is fully black.
        inputs.fog_end = 8.0f;
        inputs.fog_start = 2.0f;
        scene.collect_corona_quads(inputs, fogged);
        expect(fogged[0].rgb[0] == 0.0f && fogged[0].rgb[1] == 0.0f,
               "a corona past the fog end fades fully out");
    }

    // The procedural corona texture [orig: Lighting_InitTextures
    // @ 0x5a973a..0x5a97ff — ftol TRUNCATION, gray under alpha 255, the
    // border written 0x00000000].
    {
        expect(corona_texture_argb(64, 64) == 0xFF666666u,
               "the corona center is trunc(255 * 0.4) = 102 = 0x66 under alpha 255");
        expect(corona_texture_argb(32, 64) == 0xFF2C2C2Cu,
               "half radius: trunc(255 * (0.4 - 0.225)) = 44 = 0x2C, not rounded 45");
        expect(corona_texture_argb(0, 64) == 0u && corona_texture_argb(64, 127) == 0u,
               "the border row/column is the transparent 0x00000000 word");
        expect(corona_texture_argb(1, 1) == 0xFF000000u,
               "past the 0.4/0.45 radius the clamp holds opaque black");
    }

    // The model-light owner attach: attach bone, blink box, or unowned
    // [orig: Entity_SpawnGlowEffects @ 0x56c7ec / @ 0x56c89f / @ 0x56c8bd].
    {
        ModelLightOwnerInputs in;
        in.spawning_entity = 99;
        in.attach_bone = 4;
        in.blink_hit = true;
        in.blink_owner_entity = 7;
        in.blink_section = 2;
        ModelLightOwner owner = resolve_model_light_owner(in);
        expect(owner.entity == 99 && owner.section == 4,
               "an attached record is owned by its own entity + subobject, "
               "even inside a blink box");

        // The same record without an attach bone takes the containing
        // building + that volume's section.
        in.attach_bone = 0;
        owner = resolve_model_light_owner(in);
        expect(owner.entity == 7 && owner.section == 2,
               "an unattached record inside a blink box is owned by the "
               "containing building + section");

        // A building never runs the query, so its own unattached records stay
        // world lights even though its blink volumes contain them.
        in.spawner_is_building = true;
        owner = resolve_model_light_owner(in);
        expect(owner.entity == 0 && owner.section == 0,
               "a building's unattached records spawn unowned");
        // ...but its attached records still bind to itself.
        in.attach_bone = 3;
        owner = resolve_model_light_owner(in);
        expect(owner.entity == 99 && owner.section == 3,
               "a building's attached records still own themselves");

        // Outdoors: no hit, no owner — the fire-barrel world light.
        in.spawner_is_building = false;
        in.attach_bone = 0;
        in.blink_hit = false;
        owner = resolve_model_light_owner(in);
        expect(owner.entity == 0 && owner.section == 0,
               "an unattached record outside every blink box lights the world");
    }

    // The interior group admits a blink-box-owned light onto the draws of the
    // entities standing in that building section, and nothing else
    // [orig: setup_terrain_effect_for_entity @ 0x5c74a0 ->
    // Lighting_SetInteriorLightGroup @ 0x5a90e0; the gate
    // Light_PassesActiveGroups @ 0x5a9120].
    {
        LightScene scene;
        LightSpawnParams lamp = barrel_params(0, 0, 0);
        // A ceiling lamp inside building 7, room section 2.
        lamp.owner_entity = 7;
        lamp.owner_section = 2;
        scene.spawn(lamp);

        const std::array<int32_t, 3> qmin{-(4 << 16), -(4 << 16), -(4 << 16)};
        const std::array<int32_t, 3> qmax{4 << 16, 4 << 16, 4 << 16};
        LightSelectionOptions options;
        options.admit_owned_unscoped = false;
        LightFlickerInputs flicker;
        const std::array<float, 3> ambient{1.0f, 1.0f, 1.0f};

        // Three draws in the lamp's reach: a soldier in room 2, a soldier in
        // room 5 of the same building, and the building's own draw.
        LightDrawContext draws[3];
        for (LightDrawContext &draw : draws) {
            draw.aabb_min_fixed = qmin;
            draw.aabb_max_fixed = qmax;
        }
        draws[0].groups.owner_group_entity = 31;  // the soldier's own draw
        draws[0].groups.interior_group_entity = 7;
        draws[0].groups.interior_group_section = 2;
        draws[1].groups.owner_group_entity = 32;
        draws[1].groups.interior_group_entity = 7;
        draws[1].groups.interior_group_section = 5;
        draws[2].groups.owner_group_entity = 7;  // the building itself

        LightDrawSelection selections[3];
        scene.select_for_draws(draws, 3, options, ambient, flicker,
                               /*d3d_light_path=*/true, selections);
        expect(selections[0].count == 1,
               "the lamp reaches the soldier standing in its own room");
        expect(selections[1].count == 0,
               "the lamp does not reach a soldier in another room of the "
               "same building");
        expect(selections[2].count == 1,
               "the lamp reaches its owner building's own draw");

        // An entity outside every blink box (no interior group) sees nothing.
        LightDrawContext outdoor;
        outdoor.aabb_min_fixed = qmin;
        outdoor.aabb_max_fixed = qmax;
        outdoor.groups.owner_group_entity = 33;
        LightDrawSelection outdoor_selection;
        scene.select_for_draws(&outdoor, 1, options, ambient, flicker,
                               /*d3d_light_path=*/true, &outdoor_selection);
        expect(outdoor_selection.count == 0,
               "an interior lamp never leaks onto an outdoor draw");
    }

    // The witnessed transient-spawner constants the presenting director
    // builds its muzzle-glow / death-flash spawns from.
    expect(LightScene::kMuzzleGlowRadiusFixed == 98304, "muzzle glow radius 1.5 wu");
    expect(LightScene::kMuzzleGlowColorRgb == 0xFFE0A0u, "muzzle glow color");
    expect(LightScene::kMuzzleGlowFadeMode == 4 && LightScene::kMuzzleGlowFadeTicks == 5,
           "muzzle glow mode 4 / 5 ticks");
    expect(LightScene::kDeathFlashColorRgb == 0xFFC080u, "death flash color");
    expect(LightScene::kDeathFlashFadeMode == 2 && LightScene::kDeathFlashFadeTicks == 31,
           "death flash mode 2 / 31 ticks");

    std::cout << "light_scene_test passed\n";
    return 0;
}
