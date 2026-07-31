// opennova::audio::AmbientMixer: the placed-marker ambient emitter system pushed
// down from the Godot layer (nova_mission_audio.gd tick / nova_sound_bank.gd curve
// statics). Curve integers mirror the GUT pins in godot/tests/sound_runtime_test.gd;
// the cadence cases pin the witnessed split clock (docs/audio/lwf-dbf-sound-re.md
// §driver cadence, D-SND-16): staggered tick&7 registration
// [orig: Entity_UpdateAllEntities @ 0x4c225a; Entity_UpdateEnvSoundEmitter
// @ 0x4a8080], tick-unit keep-alives, and the per-frame live-slot mix
// [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0].
#include "audio/ambient_mixer.h"
#include "common/test_expect.h"

#include <cmath>
#include <cstdint>
#include <vector>

using opennova::audio::AmbientCandidate;
using opennova::audio::AmbientMixer;
using opennova::audio::calc_distance_volume;
using opennova::audio::crossfade_volume_byte;
using opennova::audio::emitter_layer_volume;
using opennova::audio::time_of_day_region;

namespace {

// One marker with a single one-layer set in the given regions.
int add_simple_marker(AmbientMixer &mx, float x, int32_t candidate_id,
                      int32_t stagger_slot, const int32_t slot_keys[4],
                      int32_t falloff_u = 200, int32_t member_vol = 255) {
    const float pos[3] = {x, 0.0f, 0.0f};
    AmbientMixer::LayerDesc ld;
    ld.candidate_id = candidate_id;
    ld.falloff_u = falloff_u;
    ld.member_vol = member_vol;
    std::vector<std::vector<AmbientMixer::LayerDesc>> sets;
    sets.push_back({ld});
    return mx.add_marker(pos, 0, stagger_slot, 0, slot_keys, std::move(sets));
}

int64_t doubling_occlusion(void *ctx, const float[3], const float[3],
                           int64_t dist_q16, int64_t) {
    ++*static_cast<int *>(ctx);
    return dist_q16 * 2;
}

} // namespace

int main() {
    const float kOrigin[3] = {0.0f, 0.0f, 0.0f};

    // --- The curve family (the sound_runtime_test.gd integer pins, natively) ---
    {
        TEST_EXPECT(calc_distance_volume(0, 200LL << 16, 255, 255) == 253);
        TEST_EXPECT(calc_distance_volume(100LL << 16, 200LL << 16, 255, 255) == 63);
        TEST_EXPECT(calc_distance_volume(200LL << 16, 200LL << 16, 255, 255) == 0);
        TEST_EXPECT(calc_distance_volume(300LL << 16, 200LL << 16, 255, 255) == 0);
        TEST_EXPECT(calc_distance_volume(0, 200LL << 16, 255, 100) == 100);
        TEST_EXPECT(calc_distance_volume(0, 200LL << 16, 128, 255) == 126);
        TEST_EXPECT(calc_distance_volume(0, 0, 255, 255) == 0);

        TEST_EXPECT(emitter_layer_volume(0, 200, 0, 255, 255, 255) == 252);
        TEST_EXPECT(emitter_layer_volume(100LL << 16, 200, 0, 255, 255, 255) == 63);
        TEST_EXPECT(emitter_layer_volume(200LL << 16, 200, 0, 255, 255, 255) == 0);
        TEST_EXPECT(emitter_layer_volume(50LL << 16, 200, 50, 255, 255, 255) == 252);
        TEST_EXPECT(emitter_layer_volume(125LL << 16, 200, 50, 255, 255, 255) == 63);
        TEST_EXPECT(emitter_layer_volume(25LL << 16, 200, 50, 255, 255, 255) == 63);
        // The proximity arm subtracts in Q16 FIRST: floor(min - d), not
        // min - floor(d) [orig: @ 0x528691].
        TEST_EXPECT(emitter_layer_volume(25LL * 65536 + 32768, 200, 50, 255, 255, 255) == 68);
        TEST_EXPECT(emitter_layer_volume(0, 200, 0, 128, 255, 255) == 125);
        TEST_EXPECT(emitter_layer_volume(0, 0, 0, 255, 255, 255) == 0);

        TEST_EXPECT(crossfade_volume_byte(1.0f) == 255);
        TEST_EXPECT(crossfade_volume_byte(0.5f) == 128);
        TEST_EXPECT(crossfade_volume_byte(0.0f) == 0);

        TEST_EXPECT(time_of_day_region(6.0f).region == 0);
        TEST_EXPECT(time_of_day_region(12.0f).region == 1);
        TEST_EXPECT(time_of_day_region(18.0f).region == 2);
        TEST_EXPECT(time_of_day_region(23.0f).region == 3);
        // The exact cut instant reads night at full blend (open-low intervals)
        // [orig: @ 0x408175/0x4081b0].
        TEST_EXPECT(time_of_day_region(10.0f).region == 3);
        TEST_EXPECT(time_of_day_region(10.0f).blend == 1.0f);
        // One game-minute past the 10h cut: day, fading in from morning.
        const opennova::audio::TimeOfDayRegion fade = time_of_day_region(10.0f + 1.0f / 60.0f);
        TEST_EXPECT(fade.region == 1);
        TEST_EXPECT(fade.adjacent == 0);
        TEST_EXPECT(std::fabs(fade.blend - 0.2001f) < 0.005f);
    }

    // --- Cohort stagger: a marker registers only when its cohort's tick elapses ---
    {
        AmbientMixer mx;
        const int32_t all_regions[4] = {0, 0, 0, 0};
        add_simple_marker(mx, 0.0f, 1, 0, all_regions);
        add_simple_marker(mx, 0.0f, 2, 5, all_regions);
        mx.set_time_of_day_hours(12.0f);
        mx.advance_to_tick(0); // first call from a fresh clock: cohort 0 only
        TEST_EXPECT(mx.live_slot_count() == 1);
        mx.advance_to_tick(4);
        TEST_EXPECT(mx.live_slot_count() == 1); // cohort 5 not yet visited
        mx.advance_to_tick(5);
        TEST_EXPECT(mx.live_slot_count() == 2);
        // Re-registration reuses the keyed slot — the table does not grow.
        mx.advance_to_tick(64);
        TEST_EXPECT(mx.live_slot_count() == 2);
    }

    // --- A long catch-up covers every cohort once (the 8-tick collapse) ---
    {
        AmbientMixer mx;
        const int32_t all_regions[4] = {0, 0, 0, 0};
        for (int i = 0; i < 8; ++i) {
            add_simple_marker(mx, 0.0f, 10 + i, i, all_regions);
        }
        mx.set_time_of_day_hours(12.0f);
        mx.advance_to_tick(1000);
        TEST_EXPECT(mx.live_slot_count() == 8);
    }

    // --- Tick-unit keep-alive: a no-longer-registered slot survives its lifetime,
    //     mixes on the tick its lifetime reaches zero, and clears at the next mix
    //     entry [orig: decrement @ 0x528541; entry clear @ 0x528571] ---
    {
        AmbientMixer mx;
        const int32_t morning_only[4] = {0, -1, -1, -1};
        add_simple_marker(mx, 0.0f, 1, 0, morning_only);
        mx.set_time_of_day_hours(6.0f); // morning: registers (lifetime 10 default)
        mx.advance_to_tick(8);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1);
        mx.set_time_of_day_hours(12.0f); // day slot is null: re-evals register nothing
        mx.advance_to_tick(16);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1); // lifetime 10 -> 2 (delta 8)
        mx.advance_to_tick(24);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1); // 2 -> 0, still alive this frame
        TEST_EXPECT(mx.mix(kOrigin).empty());     // 0 at entry -> slot cleared
        TEST_EXPECT(mx.live_slot_count() == 0);
    }

    // --- Region flip: both regions' slots overlap until the old lifetime expires
    //     (the witnessed audible crossfade mechanism) ---
    {
        AmbientMixer mx;
        const float pos[3] = {0.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc a;
        a.candidate_id = 1;
        a.falloff_u = 200;
        AmbientMixer::LayerDesc b = a;
        b.candidate_id = 2;
        std::vector<std::vector<AmbientMixer::LayerDesc>> sets;
        sets.push_back({a});
        sets.push_back({b});
        const int32_t keys[4] = {0, 1, -1, -1}; // morning A, day B
        mx.add_marker(pos, 0, 0, 0, keys, std::move(sets));
        mx.set_time_of_day_hours(6.0f);
        mx.advance_to_tick(8);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1);
        mx.set_time_of_day_hours(12.0f);
        mx.advance_to_tick(16);
        TEST_EXPECT(mx.mix(kOrigin).size() == 2); // A (expiring) + B (fresh)
        mx.advance_to_tick(24); // B re-registers; A decays to 0 and mixes once more
        mx.mix(kOrigin);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1); // A cleared at entry; B lives on
        TEST_EXPECT(mx.mix(kOrigin)[0].candidate_id == 2);
    }

    // --- Same-set neighbours suppress the crossfade dip [orig: @ 0x4a819d] ---
    {
        AmbientMixer mx;
        const int32_t same[4] = {0, 0, 0, 0};
        add_simple_marker(mx, 0.0f, 1, 0, same);
        const int32_t distinct_keys[4] = {0, 1, 0, 0};
        {
            const float pos[3] = {0.0f, 0.0f, 0.0f};
            AmbientMixer::LayerDesc a;
            a.candidate_id = 2;
            a.falloff_u = 200;
            AmbientMixer::LayerDesc b = a;
            b.candidate_id = 3;
            std::vector<std::vector<AmbientMixer::LayerDesc>> sets;
            sets.push_back({a});
            sets.push_back({b});
            mx.add_marker(pos, 0, 0, 0, distinct_keys, std::move(sets));
        }
        // Inside the 10h fade-in window (blend < 1): stagger nibble 0 on both.
        mx.set_time_of_day_hours(10.0f + 1.0f / 60.0f);
        mx.advance_to_tick(8);
        const std::vector<AmbientCandidate> out = mx.mix(kOrigin);
        TEST_EXPECT(out.size() == 2);
        int32_t same_vol = -1;
        int32_t distinct_vol = -1;
        for (const AmbientCandidate &c : out) {
            if (c.candidate_id == 1) {
                same_vol = c.vol;
            } else if (c.candidate_id == 3) {
                distinct_vol = c.vol;
            }
        }
        TEST_EXPECT(same_vol == 252); // full volume through the suppress
        TEST_EXPECT(distinct_vol > 0 && distinct_vol < same_vol);
        const int32_t expect_faded = emitter_layer_volume(
                0, 200, 0,
                crossfade_volume_byte(time_of_day_region(10.0f + 1.0f / 60.0f).blend),
                255, 255);
        TEST_EXPECT(distinct_vol == expect_faded);
    }

    // --- Ranking: loudest first, candidate-id tie-break; beyond-falloff culled ---
    {
        AmbientMixer mx;
        const int32_t all_regions[4] = {0, 0, 0, 0};
        add_simple_marker(mx, 50.0f, 1, 0, all_regions);
        add_simple_marker(mx, 150.0f, 2, 0, all_regions);
        add_simple_marker(mx, 50.0f, 3, 0, all_regions);  // ties with candidate 1
        add_simple_marker(mx, 500.0f, 4, 0, all_regions); // beyond falloff 200
        mx.set_time_of_day_hours(12.0f);
        mx.advance_to_tick(8);
        const std::vector<AmbientCandidate> out = mx.mix(kOrigin);
        TEST_EXPECT(out.size() == 3);
        TEST_EXPECT(out[0].candidate_id == 1);
        TEST_EXPECT(out[1].candidate_id == 3);
        TEST_EXPECT(out[2].candidate_id == 2);
        TEST_EXPECT(out[0].vol == out[1].vol);
        TEST_EXPECT(out[2].vol < out[1].vol);
    }

    // --- Occlusion: one ray per marker per mix, only when raw-audible; the
    //     inflated distance re-runs the curve ---
    {
        AmbientMixer mx;
        const float pos[3] = {50.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc a;
        a.candidate_id = 1;
        a.falloff_u = 200;
        AmbientMixer::LayerDesc b = a;
        b.candidate_id = 2;
        std::vector<std::vector<AmbientMixer::LayerDesc>> sets;
        sets.push_back({a, b}); // TWO layers of one marker: still one ray
        const int32_t keys[4] = {0, 0, 0, 0};
        mx.add_marker(pos, 7, 0, 0, keys, std::move(sets));
        const int32_t silent_keys[4] = {0, 0, 0, 0};
        add_simple_marker(mx, 5000.0f, 3, 0, silent_keys); // raw-silent: no ray
        mx.set_time_of_day_hours(12.0f);
        mx.advance_to_tick(8);
        int rays = 0;
        const std::vector<AmbientCandidate> out =
                mx.mix(kOrigin, &doubling_occlusion, &rays);
        TEST_EXPECT(rays == 1);
        TEST_EXPECT(out.size() == 2);
        const int32_t expect_occluded =
                emitter_layer_volume((50LL << 16) * 2, 200, 0, 255, 255, 255);
        TEST_EXPECT(out[0].vol == expect_occluded);
    }

    // --- A zero crossfade byte clears the region's slots immediately
    //     [orig: the vol-0 register arm @ 0x528340] ---
    {
        AmbientMixer mx;
        const int32_t distinct_keys[4] = {0, 1, 0, 0};
        const float pos[3] = {0.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc a;
        a.candidate_id = 1;
        a.falloff_u = 200;
        AmbientMixer::LayerDesc b = a;
        b.candidate_id = 2;
        std::vector<std::vector<AmbientMixer::LayerDesc>> sets;
        sets.push_back({a});
        sets.push_back({b});
        mx.add_marker(pos, 0, 0, 0, distinct_keys, std::move(sets));
        mx.set_time_of_day_hours(12.0f); // mid-day: full-volume day set
        mx.advance_to_tick(8);
        TEST_EXPECT(mx.live_slot_count() == 1);
        // The last instant of the day fade-out: blend small enough to round the
        // volume byte to zero -> the re-registration CLEARS instead.
        mx.set_time_of_day_hours(17.0f - (5460.0f / 65536.0f) * 0.001f);
        mx.advance_to_tick(16);
        TEST_EXPECT(mx.live_slot_count() == 0);
    }

    // --- Entity-attached emitters share the retail slot table and loudest-first
    //     ranking with placed markers; pitch survives into the embedder candidate ---
    {
        AmbientMixer mx;
        const int32_t all_regions[4] = {0, 0, 0, 0};
        add_simple_marker(mx, 100.0f, 1, 0, all_regions);
        mx.set_time_of_day_hours(12.0f);
        mx.advance_to_tick(8);

        const float vehicle_pos[3] = {25.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc vehicle_idle;
        vehicle_idle.candidate_id = 2;
        vehicle_idle.falloff_u = 200;
        mx.register_emitter(0x01000003u, 0, vehicle_pos, 77, 30,
                            0x14000, 0xFFFF, {vehicle_idle});

        const std::vector<AmbientCandidate> out = mx.mix(kOrigin);
        TEST_EXPECT(out.size() == 2);
        TEST_EXPECT(out[0].candidate_id == 2); // nearer dynamic emitter wins
        TEST_EXPECT(out[1].candidate_id == 1);
        TEST_EXPECT(out[0].vol > out[1].vol);
        TEST_EXPECT(out[0].pitch_q16 == 0x14000);
        TEST_EXPECT(out[1].pitch_q16 == 0x10000); // placed markers register at unity
    }

    // --- Dynamic registration is keyed by (source, lane, layer): a per-tick
    //     refresh updates in place, lanes coexist, and pitch/volume zero clears
    //     only the addressed source+lane.
    //     [orig: SoundEmitter_RegisterSetLayers @ 0x528340;
    //      SoundEmitter_ClearByEntityAndSlot @ 0x527a50] ---
    {
        AmbientMixer mx;
        const float idle_pos[3] = {20.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc idle;
        idle.candidate_id = 10;
        idle.falloff_u = 200;
        mx.register_emitter(501, 0, idle_pos, 77, 30,
                            0x10000, 0xFFFF, {idle});
        TEST_EXPECT(mx.live_slot_count() == 1);

        const float moved_pos[3] = {40.0f, 3.0f, 1.0f};
        mx.register_emitter(501, 0, moved_pos, 77, 30,
                            0x12000, 0x8000, {idle});
        TEST_EXPECT(mx.live_slot_count() == 1); // keyed update, not allocation
        const std::vector<AmbientCandidate> updated = mx.mix(kOrigin);
        TEST_EXPECT(updated.size() == 1);
        TEST_EXPECT(updated[0].pos[0] == 40.0f);
        TEST_EXPECT(updated[0].pos[1] == 3.0f);
        TEST_EXPECT(updated[0].pos[2] == 1.0f);
        TEST_EXPECT(updated[0].pitch_q16 == 0x12000);

        AmbientMixer::LayerDesc forward;
        forward.candidate_id = 11;
        forward.falloff_u = 200;
        mx.register_emitter(501, 10, moved_pos, 77, 30,
                            0x10000, 0xFFFF, {forward});
        TEST_EXPECT(mx.live_slot_count() == 2); // another retail lane coexists

        mx.register_emitter(501, 0, moved_pos, 77, 30,
                            0, 0xFFFF, {idle});
        TEST_EXPECT(mx.live_slot_count() == 1);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1);
        TEST_EXPECT(mx.mix(kOrigin)[0].candidate_id == 11);

        mx.register_emitter(501, 10, moved_pos, 77, 30,
                            0x10000, 0, {forward});
        TEST_EXPECT(mx.live_slot_count() == 0);
        TEST_EXPECT(mx.mix(kOrigin).empty());
    }

    // --- Pose is source-owned across lanes. Clearing one lane at a moved pose
    //     carries an unrefreshed lane with the entity, but does not renew that
    //     lane's original keep-alive.
    {
        AmbientMixer mx;
        const float initial_pos[3] = {20.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc idle;
        idle.candidate_id = 12;
        idle.falloff_u = 200;
        AmbientMixer::LayerDesc forward = idle;
        forward.candidate_id = 13;
        mx.register_emitter(502, 0, initial_pos, 77, 3,
                            0x10000, 0xFFFF, {idle});
        mx.register_emitter(502, 10, initial_pos, 77, 3,
                            0x10000, 0xFFFF, {forward});
        TEST_EXPECT(mx.mix(kOrigin).size() == 2);

        mx.advance_to_tick(2);
        const float moved_pos[3] = {40.0f, 3.0f, 1.0f};
        mx.register_emitter(502, 10, moved_pos, 78, 3, 0, 0, {});
        const std::vector<AmbientCandidate> moved = mx.mix(kOrigin);
        TEST_EXPECT(moved.size() == 1);
        TEST_EXPECT(moved[0].candidate_id == 12);
        TEST_EXPECT(moved[0].pos[0] == 40.0f);
        TEST_EXPECT(moved[0].pos[1] == 3.0f);
        TEST_EXPECT(moved[0].pos[2] == 1.0f);

        mx.advance_to_tick(3);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1);
        TEST_EXPECT(mx.mix(kOrigin).empty());
    }

    // --- The source key retains the complete registry-lifetime serial. A
    //     generation beyond 32 bits must not alias an older entity in the same
    //     lane.
    {
        AmbientMixer mx;
        const float pos[3] = {10.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc old_generation;
        old_generation.candidate_id = 20;
        old_generation.falloff_u = 200;
        AmbientMixer::LayerDesc new_generation = old_generation;
        new_generation.candidate_id = 21;
        mx.register_emitter(1, 0, pos, 77, 30, 0x10000, 0xFFFF,
                            {old_generation});
        mx.register_emitter(0x100000001ULL, 0, pos, 77, 30, 0x10000,
                            0xFFFF, {new_generation});
        TEST_EXPECT(mx.live_slot_count() == 2);
        const std::vector<AmbientCandidate> out = mx.mix(kOrigin);
        TEST_EXPECT(out.size() == 2);
    }

    // --- Registration happens at the current logic tick. A catch-up interval
    //     since the preceding render mix cannot retroactively consume a newly
    //     refreshed source's keep-alive.
    {
        AmbientMixer mx;
        mx.advance_to_tick(10);
        TEST_EXPECT(mx.mix(kOrigin).empty());
        mx.advance_to_tick(100);
        const float pos[3] = {10.0f, 0.0f, 0.0f};
        AmbientMixer::LayerDesc layer;
        layer.candidate_id = 30;
        layer.falloff_u = 200;
        mx.register_emitter(99, 0, pos, 77, 30, 0x10000, 0xFFFF, {layer});
        TEST_EXPECT(mx.mix(kOrigin).size() == 1);
        mx.advance_to_tick(129);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1);
        mx.advance_to_tick(131);
        TEST_EXPECT(mx.mix(kOrigin).size() == 1); // reaches zero after this service
        TEST_EXPECT(mx.mix(kOrigin).empty());
        TEST_EXPECT(mx.live_slot_count() == 0);
    }

    // --- The autonomous clock derives 62.5 Hz ticks from wall seconds ---
    {
        AmbientMixer mx;
        const int32_t all_regions[4] = {0, 0, 0, 0};
        add_simple_marker(mx, 0.0f, 1, 3, all_regions);
        mx.set_time_of_day_hours(12.0f);
        mx.advance_seconds(0.2f); // 12.5 ticks: every cohort visited
        TEST_EXPECT(mx.live_slot_count() == 1);
        TEST_EXPECT(mx.clock_tick() == 12);
    }

    return 0;
}
