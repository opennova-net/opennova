// Root-motion track grill [orig: AnimMap_UpdateEntity @0x40b5f0 tail, disasm
// 0x40b82f..0x40b8a3]. The engine's per-frame root record IS the .bad "events"
// array (24-byte stride, fence-post: frame_count+1 records, lerped pairwise by
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
// The two stock gaits this reading was grilled on, witnessed 2026-10-19 (the
// retired retail row): I_walkf.bad (19 bones, 30 fps, 152 frames, a loop) steps
// a mean 0.0606943928 u/frame forward (1.82 u/s; 1988 per tick in 16.16), its
// capsule bottom 0.946..1.026 and top 1.683..1.767, its footsteps bit 0x2;
// E_RUNF.bad (43 frames) 0.176038387 u/frame (5.28 u/s; 5768 per tick), bottom
// 0.900..1.011, top 1.629..1.736, footsteps 0x1 and 0x2. Each is minted here at
// its witnessed shape (its mean step on every record, its capsule swinging
// across the witnessed range, footsteps on regular plant frames), read back,
// and played through the engine's own root source (anim::AdmRootMotion), whose
// per-tick step must be the witnessed one. `root_motion_test <clip.bad>...`
// runs the reading checks on other clips.
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/infantry.h>

#include "common/file_io.h"
#include "common/temp_dir.h"

using namespace opennova::bad;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct ClipCase {
    std::string path;
    double min_speed; // world units (~m) per second, gait plausibility window
    double max_speed;
};

// A witnessed gait: its length, its mean forward step, its capsule's range and
// its footstep bits, plant frames every `plant` frames (the second bit on the
// odd plants when `alternate`).
struct Gait {
    const char *name;
    uint32_t frames;
    float step;
    float bottom_min, bottom_max, top_min, top_max;
    uint32_t plant;
    bool alternate;
    int32_t step16; // the witnessed per-tick step
};

constexpr Gait kWalk = {"walkf", 152, 0.0606943928f, 0.946f, 1.026f, 1.683f, 1.767f, 15, false, 1988};
constexpr Gait kRun = {"runf", 43, 0.176038387f, 0.900f, 1.011f, 1.629f, 1.736f, 11, true, 5768};

// The gait as a 19-bone clip, every key the rest pose: only its records matter here.
bool write_gait(const std::string &path, const Gait &g) {
    constexpr uint32_t kBones = 19;
    std::vector<BadBone> bones(kBones);
    std::vector<BadChannel> channels(kBones);
    std::vector<uint16_t> lengths(g.frames + 1, 1);
    std::vector<BadQuaternion> keys(g.frames + 1, BadQuaternion{0.0f, 0.0f, 0.0f, 1.0f});
    for (uint32_t b = 0; b < kBones; ++b) {
        std::snprintf(bones[b].name, sizeof(bones[b].name), "BN%02u", b + 1);
        bones[b].parent_index = b == 0 ? -1 : 0;
        bones[b].rotation[0] = bones[b].rotation[4] = bones[b].rotation[8] = 1.0f;
        channels[b].frame_count = g.frames + 1;
        channels[b].frame_lengths = lengths.data();
        channels[b].rotations = keys.data();
    }
    std::vector<BadEvent> events(g.frames + 1);
    for (uint32_t f = 0; f <= g.frames; ++f) {
        BadEvent &e = events[f];
        e.velocity[2] = g.step;
        // The capsule swings across its range and back over each stride.
        const uint32_t stride = 2 * g.plant;
        const uint32_t at = f % stride;
        const float swing = static_cast<float>(at <= g.plant ? at : stride - at) / static_cast<float>(g.plant);
        e.bottom = g.bottom_min + (g.bottom_max - g.bottom_min) * swing;
        e.top = g.top_min + (g.top_max - g.top_min) * swing;
        if (f % g.plant == 0) e.trigger = g.alternate && (f / g.plant) % 2 == 1 ? 0x2 : (g.alternate ? 0x1 : 0x2);
    }
    BadFile bf = {};
    bf.version = 1;
    bf.fps = 30;
    bf.frame_count = g.frames;
    bf.flags = 1u; // a loop
    bf.bone_count = kBones;
    bf.bones = bones.data();
    bf.num_bones = kBones;
    bf.channels = channels.data();
    bf.num_channels = kBones;
    bf.events = events.data();
    bf.num_events = events.size();
    return bad_write(path.c_str(), &bf) == 0;
}

void check_reading(const ClipCase &c, int &tested) {
    BadFile bf{};
    CHECK(bad_parse(c.path.c_str(), &bf) == 0);
    if (bf.frame_count == 0) return;
    ++tested;

    std::printf("%s: fps=%u frames=%u bones=%zu events=%zu flags=0x%x\n", c.path.c_str(),
                bf.fps, bf.frame_count, bf.num_bones, bf.num_events, bf.flags);

    // Fence-post layout: the interpolator lerps record[frame] -> record[frame+1]
    // [orig: 0x40b230 reads rec+0 and rec+24], so N frames carry N+1 records.
    CHECK(bf.num_events == bf.frame_count + 1);
    if (bf.num_events != bf.frame_count + 1) {
        bad_free(&bf);
        return;
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

} // namespace

int main(int argc, char **argv) {
    int tested = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) check_reading({argv[i], 0.1, 10.0}, tested);
    } else {
        test_temp::TempDir root("opennova_root_motion");
        const std::string walk = root.file("walkf.bad"), run = root.file("runf.bad");
        CHECK(write_gait(walk, kWalk) && write_gait(run, kRun));
        check_reading({walk, 0.5, 3.0}, tested);
        check_reading({run, 2.0, 8.0}, tested);

        // The engine's own root source over the two: the per-tick forward step is
        // the witnessed one, the capsule the record's in 16.16 (the top +0x2000),
        // the footstep the plant frame's.
        const std::string adm = "anim_reset\t\"walkf\"\r\nanim_walk_forward\t\"walkf\"\r\nanim_run_forward\t\"runf\"\r\n";
        CHECK(test_io::write_file(root.file("gait.adm"), std::vector<uint8_t>(adm.begin(), adm.end())));
        opennova::ResourceIndex index;
        opennova::assets::AssetStore files{&index};
        CHECK(index.scan(root.root()));
        opennova::anim::AdmRootMotion source;
        const int set = source.register_adm(&files, "gait.adm");
        CHECK(set == 0);
        const struct {
            int state;
            const Gait &gait;
        } played[] = {{opennova::world::anim_state::kWalkForward, kWalk},
                      {opennova::world::anim_state::kRunForward, kRun}};
        for (const auto &p : played) {
            int32_t phase = -1;
            opennova::world::RootMotionFrame frame{};
            CHECK(source.has_clip(set, p.state));
            CHECK(source.advance(set, p.state, phase, frame));
            std::printf("%s: tick 0 dx=%d bottom=%d top=%d events=0x%x\n", p.gait.name, frame.dx,
                        frame.capsule_bottom, frame.capsule_top, frame.events);
            CHECK(phase == 0 && frame.dx == p.gait.step16);
            CHECK(frame.capsule_bottom == static_cast<int32_t>(p.gait.bottom_min * 65536.0f));
            CHECK(frame.capsule_top == static_cast<int32_t>(p.gait.top_min * 65536.0f) + 0x2000);
            CHECK((frame.events & 0x3u) != 0);
        }
    }

    if (tested == 0) {
        std::printf("root_motion_test: no clip read\n");
        return 1;
    }
    if (failures == 0) std::printf("root_motion_test: OK (%d clips)\n", tested);
    else std::printf("root_motion_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
