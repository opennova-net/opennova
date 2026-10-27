// The software mixer's output meter -- see audio_out_meter.h.
#include <runtime/audio/audio_out_meter.h>

#include <algorithm>
#include <cmath>

namespace opennova::audio {

int32_t audio_out_block_level(int32_t accumulator) {
    // `mul eax` keeps the low dword in eax; the stage reads it unsigned.
    const uint32_t square = static_cast<uint32_t>(accumulator) * static_cast<uint32_t>(accumulator);
    const uint32_t shifted = square >> 15;
    if (shifted < 0x400u) return 0;
    return static_cast<int32_t>(std::min<uint32_t>(shifted - 0x400u, 0x3FFu));
}

int32_t audio_out_accumulator_from_peak(float peak) {
    if (!(peak > 0.0f)) return 0;
    const float clamped = std::min(peak, 1.0f);
    return static_cast<int32_t>(std::lround(clamped * static_cast<float>(kAudioOutAccumulatorReach)));
}

void AudioOutMeter::reset() {
    stage1_ = 0;
    level_ = 0;
    frame_ = 0;
    frame_carry_ = 0.0;
}

void AudioOutMeter::advance(double seconds, float peak) {
    if (!(seconds > 0.0)) return;
    frame_carry_ += seconds * static_cast<double>(kAudioOutDeviceRate);
    const double whole = std::floor(frame_carry_);
    frame_carry_ -= whole;
    mix(static_cast<uint32_t>(std::min(whole, 4294967295.0)), audio_out_accumulator_from_peak(peak));
}

void AudioOutMeter::mix(uint32_t frames, int32_t accumulator) {
    const int32_t block = audio_out_block_level(accumulator);
    while (frames > 0) {
        // Each frame ends at the counter test: a multiple of 0x400 stages the block, then the
        // counter steps [orig: @0x7BEF16..0x7BEF1C, `inc ecx` @0x7BEF66].
        const uint32_t into = frame_ & (kAudioOutBlockFrames - 1u);
        const uint32_t to_stage = into == 0 ? 0u : kAudioOutBlockFrames - into;
        if (to_stage >= frames) {
            frame_ += frames;
            return;
        }
        frame_ += to_stage;
        frames -= to_stage;
        // [orig: `add eax, g_AudioOutLevelStage1; shr eax, 1` @0x7BEF3E..0x7BEF48, `add eax,
        //  g_AudioOutLevel; shr eax, 1` @0x7BEF4D..0x7BEF55]
        stage1_ = static_cast<int32_t>((static_cast<uint32_t>(block) + static_cast<uint32_t>(stage1_)) >> 1);
        level_ = static_cast<int32_t>((static_cast<uint32_t>(stage1_) + static_cast<uint32_t>(level_)) >> 1);
        ++frame_;
        --frames;
    }
}

} // namespace opennova::audio
