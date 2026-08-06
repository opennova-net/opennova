/* engine/runtime/simassets AdmRootMotion — the engine-side IRootMotionSource
   (moved from the adapter's InfantryRootMotion), pinned headless over the
   committed fixtures: soldier.adm -> idle.bad/walk.bad through a mounted
   ResourceIndex. The .bad sampling semantics themselves are grilled in
   tests/anim/root_motion_test.cpp; this pins the registry/resolution/advance
   plumbing of the ported source. */

#include <cstdio>
#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <resource_index/resource_index.h>
#include <simassets/adm_root_motion.h>
#include <world/infantry.h>

int main() {
    using opennova::simassets::AdmRootMotion;
    using opennova::world::RootMotionFrame;

    const char *root = test_paths_repo_root(__FILE__);
    opennova::ResourceIndex index;
    TEST_EXPECT(index.scan(std::string(root) + "/fixtures/anim"));

    AdmRootMotion source;
    TEST_EXPECT(source.empty());

    // First successful registration is the default set (id 0); re-registering
    // the same name (any case) returns the cached id.
    const int soldier = source.register_adm(&index, "soldier.adm");
    TEST_EXPECT(soldier == 0);
    TEST_EXPECT(source.register_adm(&index, "SOLDIER.ADM") == 0);
    TEST_EXPECT(!source.empty());
    TEST_EXPECT(source.adm_name(0) == "soldier.adm");

    const int us01 = source.register_adm(&index, "US01.adm");
    TEST_EXPECT(us01 == 1);
    TEST_EXPECT(source.register_adm(&index, "missing.adm") == -1);
    TEST_EXPECT(source.set_count() == 2);

    std::printf("[adm] soldier clips=%d us01 clips=%d\n",
            source.clip_count(0), source.clip_count(1));
    TEST_EXPECT(source.clip_count(0) > 0);

    // State 0 (RESET) is authored in both fixture maps; an unauthored state
    // falls back to RESET's channel (the retail AnimMap registration rule).
    TEST_EXPECT(source.has_clip(0, opennova::world::anim_state::kReset));
    const int32_t reset_len =
            source.clip_length_ticks(0, opennova::world::anim_state::kReset);
    std::printf("[adm] reset clip_length_ticks=%d\n", reset_len);
    TEST_EXPECT(reset_len > 0);
    // Fallback: a state id far past the authored table still resolves (RESET).
    TEST_EXPECT(source.clip_length_ticks(0, 199) == reset_len);
    // An unregistered set has nothing.
    TEST_EXPECT(source.clip_length_ticks(7, 0) == -1);

    // advance(): the playhead half-frame convention — phase increments by one
    // per call, frames carry capsule extents (top gets the +0x2000 bias).
    int32_t phase = -1;
    RootMotionFrame frame{};
    TEST_EXPECT(source.advance(0, opennova::world::anim_state::kReset, phase, frame));
    TEST_EXPECT(phase == 0);
    const int32_t bottom0 = frame.capsule_bottom;
    const int32_t top0 = frame.capsule_top;
    std::printf("[adm] reset f0: dx=%d dy=%d dz=%d bottom=%d top=%d events=%u\n",
            frame.dx, frame.dy, frame.dz, frame.capsule_bottom,
            frame.capsule_top, frame.events);
    TEST_EXPECT(top0 > bottom0);

    // Determinism: a fresh playhead over the same clip reproduces frame 0.
    int32_t phase2 = -1;
    RootMotionFrame frame2{};
    TEST_EXPECT(source.advance(0, opennova::world::anim_state::kReset, phase2, frame2));
    TEST_EXPECT(frame2.dx == frame.dx && frame2.capsule_bottom == bottom0 &&
            frame2.capsule_top == top0);

    // Blended advance with weight 0 equals the primary-only sample.
    int32_t pa = -1, pb = -1;
    RootMotionFrame blended{};
    TEST_EXPECT(source.advance_blended(0,
            opennova::world::anim_state::kReset, pa,
            opennova::world::anim_state::kReset, pb, 0.0f, blended));
    TEST_EXPECT(blended.dx == frame.dx);
    TEST_EXPECT(blended.capsule_bottom == bottom0);
    TEST_EXPECT(blended.capsule_top == top0);

    // clear() empties the registry.
    source.clear();
    TEST_EXPECT(source.empty());
    TEST_EXPECT(source.clip_count(0) == 0);

    // A null index registers nothing, never crashes.
    AdmRootMotion no_index;
    TEST_EXPECT(no_index.register_adm(nullptr, "soldier.adm") == -1);

    std::printf("simassets adm root motion: OK\n");
    return 0;
}
