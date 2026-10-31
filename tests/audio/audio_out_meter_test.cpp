// The software mixer's output meter (runtime/audio/audio_out_meter.h) and its readers: the
// block stage over the follower's value, its 0x400-frame cadence and two-stage smoothing, the
// bus-peak scale, and the speaking entity's head kick and TALK register
// (world::speaking_head_kick, world::talk_ctrl_register_value).
// [orig: the block stage @0x7BEF16..0x7BEF55; the follower @0x7BDE22..0x7BDE2C;
//  Entity_BuildBoneTransformMatrices @0x4B17E8..0x4B17FB; BoneCallback_org0_Skin
//  @0x4E3624..0x4E364E]
#include <runtime/audio/audio_out_meter.h>
#include <runtime/world/script_voice.h>
#include <runtime/world/world.h>

#include "common/test_expect.h"

#include <cmath>
#include <cstdio>
#include "../common/title_fixture.h"

using namespace opennova;
using test_expect::expect;

namespace {

void block_levels() {
    // (acc * acc) >> 15 below 0x400 reads 0; the window opens at 5793 (1024) and closes at
    // 8192 (2048 - 0x400 = 1024, clamped to 0x3FF).
    expect(audio::audio_out_block_level(0) == 0, "a silent block reads 0");
    expect(audio::audio_out_block_level(5792) == 0, "a follower under the window reads 0");
    expect(audio::audio_out_block_level(5793) == 0, "the window's first value reads 0");
    expect(audio::audio_out_block_level(6000) == ((6000 * 6000) >> 15) - 0x400,
            "inside the window the level is the square's excess");
    expect(audio::audio_out_block_level(8192) == 0x3FF, "the window's top clamps at 0x3FF");
    expect(audio::audio_out_block_level(audio::kAudioOutAccumulatorReach) == 0x3FF,
            "the follower's reach clamps at 0x3FF");
}

void peak_scale() {
    expect(audio::audio_out_accumulator_from_peak(0.0f) == 0, "a silent bus is a 0 follower");
    expect(audio::audio_out_accumulator_from_peak(-1.0f) == 0, "a negative peak is a 0 follower");
    expect(audio::audio_out_accumulator_from_peak(std::nanf("")) == 0, "no peak is a 0 follower");
    expect(audio::audio_out_accumulator_from_peak(1.0f) == audio::kAudioOutAccumulatorReach,
            "a full-scale peak is the follower's reach");
    expect(audio::audio_out_accumulator_from_peak(4.0f) == audio::kAudioOutAccumulatorReach,
            "a peak past full scale is the follower's reach");
    expect(audio::audio_out_accumulator_from_peak(0.5f) == audio::kAudioOutAccumulatorReach / 2,
            "the scale is linear");
}

void cadence_and_smoothing() {
    audio::AudioOutMeter meter;
    // The counter starts at 0, a stage frame: the first frame stages the block.
    meter.mix(1, 8192);
    expect(meter.stage1() == 0x3FF >> 1, "the first frame halves the block into stage 1");
    expect(meter.level() == (0x3FF >> 1) >> 1, "and stage 1 into the level");
    // The next 1023 frames stage nothing.
    meter.mix(1023, 8192);
    expect(meter.stage1() == 0x3FF >> 1 && meter.level() == (0x3FF >> 1) >> 1,
            "frames inside a block stage nothing");
    // Frame 1024 stages again, each word halving toward the block.
    meter.mix(1, 8192);
    const int32_t s1 = (0x3FF + (0x3FF >> 1)) >> 1;
    expect(meter.stage1() == s1, "the next block's first frame halves toward the block");
    expect(meter.level() == (s1 + ((0x3FF >> 1) >> 1)) >> 1, "the level follows stage 1");
    // One call across several blocks stages each of them.
    audio::AudioOutMeter wide;
    wide.mix(3 * audio::kAudioOutBlockFrames, 8192);
    audio::AudioOutMeter steps;
    for (int i = 0; i < 3; ++i) steps.mix(audio::kAudioOutBlockFrames, 8192);
    expect(wide.stage1() == steps.stage1() && wide.level() == steps.level(),
            "one call over three blocks stages what three calls do");
    // Silence decays both words by halves.
    wide.mix(audio::kAudioOutBlockFrames, 0);
    expect(wide.stage1() == steps.stage1() >> 1, "a silent block halves stage 1");
    wide.reset();
    expect(wide.stage1() == 0 && wide.level() == 0, "the mixer init zeroes both words");
}

void device_time() {
    // advance counts device frames at 44100 Hz, carrying the fraction.
    // (Quarter-frame offsets keep each call's whole frames clear of rounding.)
    const double frame = 1.0 / audio::kAudioOutDeviceRate;
    audio::AudioOutMeter meter;
    meter.advance(0.5 * frame, 1.0f);
    expect(meter.stage1() == 0, "half a frame mixes nothing");
    meter.advance(0.75 * frame, 1.0f); // 1.25 frames: frame 0
    expect(meter.stage1() == 0x3FF >> 1, "the carried half completes the first frame");
    meter.advance(1022.5 * frame, 1.0f); // 1023.75 frames: frames 1..1022
    expect(meter.stage1() == 0x3FF >> 1, "the rest of the block stages nothing");
    meter.advance(1.5 * frame, 1.0f); // 1025.25 frames: frames 1023 and 1024
    expect(meter.stage1() == (0x3FF + (0x3FF >> 1)) >> 1, "the next block's first frame stages");
}

void readers() {
    world::World w;
    test_titles::jo_world(w);
    const world::EntityHandle speaker = world::EntityHandle::make(0, 3);
    const world::EntityHandle other = world::EntityHandle::make(0, 4);
    world::ScriptVoiceChannel::State state;
    state.portrait = speaker;
    w.script.voice.restore(state);
    w.audio_out_meter.mix(1, 8192); // stage 1 0x1FF, level 0xFF
    expect(world::speaking_head_kick(w, speaker) == (0xFF << 17),
            "the speaker's head kick is the level << 17");
    expect(world::talk_ctrl_register_value(w, speaker) == (0x1FF << 6),
            "the speaker's TALK is stage 1 << 6");
    expect(world::speaking_head_kick(w, other) == 0 && world::talk_ctrl_register_value(w, other) == 0,
            "any other body reads 0");
    expect(world::speaking_head_kick(w, world::EntityHandle{}) == 0,
            "no body reads 0");
    // The meter's top: stage 1 under 0x400, so TALK stays under the 0x10000 cap.
    for (int i = 0; i < 40; ++i) w.audio_out_meter.mix(audio::kAudioOutBlockFrames, 8192);
    expect(world::talk_ctrl_register_value(w, speaker) == (w.audio_out_meter.stage1() << 6) &&
                    world::talk_ctrl_register_value(w, speaker) <= 0x10000,
            "TALK follows stage 1 up to its cap");
    w.script.voice.restore(world::ScriptVoiceChannel::State{});
    expect(world::speaking_head_kick(w, speaker) == 0, "no speaker, no kick");
}

} // namespace

int main() {
    block_levels();
    peak_scale();
    cadence_and_smoothing();
    device_time();
    readers();
    if (test_expect::failures == 0) std::printf("audio_out_meter: all passed\n");
    return test_expect::failures == 0 ? 0 : 1;
}
