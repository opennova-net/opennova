// Root-motion track grill against real JO clips [orig: AnimMap_UpdateEntity @0x40b5f0
// tail, disasm 0x40b82f..0x40b8a3]. The engine's per-frame root record IS the .bad
// "events" array (24-byte stride, fence-post: frame_count+1 records, lerped pairwise by
// AnimChannel_InterpolateKeyframe @0x40b230 with trigger taken unlerped from the lower
// keyframe):
//   rec = {vel[3], capsule_bottom, capsule_top, trigger}     (bad.h BadEvent)
// Out-transform mapping (the RootMotionFrame the infantry motor integrates):
//   out[0] = vel[2] * 32768   [flt_7C32B4]  forward step per tick, 16.16. The 32768
//                              (= 65536/2) bakes in the ~2-sim-ticks-per-30fps-frame
//                              ratio: walk 0.066 * 32768 * 62Hz / 65536 ~= 2.0 u/s.
//   out[1] = vel[0] * 32768                  lateral step per tick.
//   out[2] = vel[1] * 32768 on the first update after a reset, else OVERWRITTEN with
//            delta(capsule_bottom * 65536)  [flt_7C32BC]  — the vertical root delta
//            (~0 in gaits; climb clips lift). The previous value lives in anim_slot[19]
//            and resets on climb (32..35) / death_grenade (176..179) pending states.
//   out[3] = capsule_bottom * 65536, out[4] = 0x2000 + capsule_top * 65536
//            [flt_7C32B8 = -65536]           — the per-frame collision capsule
//            (crouch/prone shrink), consumed by the resolver, not the motor.
//   events = trigger (dword_A2ED08: bit0/bit1 footsteps, 0x20..0x400 cloth/gear).
//
// This test parses real walk/run clips and pins that reading: per-frame forward
// velocity at plausible gait speeds, near-constant standing capsule, footstep bits
// present, and the 16.16 per-tick quantities the motor consumes. Asset-gated like
// wac_corpus_test: absent files skip (CI-safe).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bad/bad.h"

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct ClipCase {
    const char *path;
    double min_speed; // world units (~m) per second, gait plausibility window
    double max_speed;
};

bool file_exists(const char *p) {
    if (std::FILE *f = std::fopen(p, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<ClipCase> cases;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) cases.push_back({argv[i], 0.1, 10.0});
    } else {
        cases = {
            {"C:/Users/taylor/Desktop/JOX/I_walkf.bad", 0.5, 3.0},
            {"C:/Users/taylor/Desktop/JOX/E_RUNF.bad", 2.0, 8.0},
        };
    }

    int tested = 0;
    for (const ClipCase &c : cases) {
        if (!file_exists(c.path)) {
            std::printf("skip (absent): %s\n", c.path);
            continue;
        }
        BadFile bf{};
        CHECK(bad_parse(c.path, &bf) == 0);
        if (bf.frame_count == 0) continue;
        ++tested;

        std::printf("%s: fps=%u frames=%u bones=%zu events=%zu flags=0x%x\n", c.path,
                    bf.fps, bf.frame_count, bf.num_bones, bf.num_events, bf.flags);

        // Fence-post layout: the interpolator lerps record[frame] -> record[frame+1]
        // [orig: 0x40b230 reads rec+0 and rec+24], so N frames carry N+1 records.
        CHECK(bf.num_events == bf.frame_count + 1);
        if (bf.num_events != bf.frame_count + 1) {
            bad_free(&bf);
            continue;
        }

        // vel[2] = per-frame forward velocity: forward-only in a gait clip, and the
        // mean speed (vel * fps) lands in the gait window.
        double sum_fwd = 0.0, sum_lat = 0.0;
        double min_bottom = 1e9, max_bottom = -1e9, min_top = 1e9, max_top = -1e9;
        uint32_t event_union = 0;
        for (size_t i = 0; i < bf.num_events; ++i) {
            const BadEvent &r = bf.events[i];
            CHECK(r.velocity[2] >= 0.0f); // gait clips never step backward
            sum_fwd += r.velocity[2];
            sum_lat += std::fabs(r.velocity[0]);
            if (r.bottom < min_bottom) min_bottom = r.bottom;
            if (r.bottom > max_bottom) max_bottom = r.bottom;
            if (r.top < min_top) min_top = r.top;
            if (r.top > max_top) max_top = r.top;
            event_union |= static_cast<uint32_t>(r.trigger);
        }
        const double mean_fwd = sum_fwd / bf.num_events;
        const double speed = mean_fwd * (bf.fps ? bf.fps : 30);
        std::printf("  forward: mean=%.4f u/frame -> %.2f u/s (window %.1f..%.1f)\n",
                    mean_fwd, speed, c.min_speed, c.max_speed);
        CHECK(speed >= c.min_speed && speed <= c.max_speed);

        // The 16.16 per-tick step the motor integrates [orig: vel2 * flt_7C32B4=32768]:
        // a walk-speed clip steps 1000..4000 (0.015..0.06u) per tick at gait speeds.
        const int32_t mean_step16 = static_cast<int32_t>(mean_fwd * 32768.0);
        std::printf("  per-tick forward step: %d (16.16)\n", mean_step16);
        CHECK(mean_step16 > 500 && mean_step16 < 8000);

        // Lateral channel stays small in straight gaits.
        CHECK(sum_lat / bf.num_events < 0.05);

        // Standing-gait collision capsule: bottom ~waist, top ~head, near-constant
        // (the vertical root delta = delta(bottom*65536) stays under 0.125u/frame).
        std::printf("  capsule: bottom %.3f..%.3f top %.3f..%.3f\n", min_bottom,
                    max_bottom, min_top, max_top);
        CHECK(min_bottom > 0.5 && max_bottom < 1.5);
        CHECK(min_top > 1.2 && max_top < 2.2);
        CHECK(max_top > max_bottom);
        for (size_t i = 1; i < bf.num_events; ++i) {
            const int32_t dz16 = static_cast<int32_t>(bf.events[i].bottom * 65536.0f) -
                                 static_cast<int32_t>(bf.events[i - 1].bottom * 65536.0f);
            CHECK(dz16 > -0x2000 && dz16 < 0x2000);
        }

        // Footstep event bits appear at plant frames in every gait clip.
        std::printf("  event bits union: 0x%x\n", event_union);
        CHECK((event_union & 0x3u) != 0);

        bad_free(&bf);
    }

    if (tested == 0) {
        std::printf("root_motion_test: skipped (no assets)\n");
        return 0;
    }
    if (failures == 0) std::printf("root_motion_test: OK (%d clips)\n", tested);
    else std::printf("root_motion_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
