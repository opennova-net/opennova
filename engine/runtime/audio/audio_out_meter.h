// The software mixer's output meter: the level the speaking entity's head kick, its TALK CTRL
// register and its friendly tag's pulse read. Retail's mixer follows channel zero (the
// scripted voice) every device frame and, every 0x400th frame, turns the follower into a
// block level it halves twice into two words. Ours mixes in Godot's audio server, so the
// follower's input is the scripted-voice bus's peak the device hands over once a frame
// (audio_out_accumulator_from_peak); the block stage and its smoothing are retail's.
// [orig: the mix loop's channel-zero follower @0x7BDE22..0x7BDE2C (`cmp ebx, esi; setg al;
//  shl eax, 4; add esi, eax`), the block stage @0x7BEF16..0x7BEF55, both words zeroed by
//  AudioMixer_Init @0x7BD405]
#pragma once

#include <cstdint>

namespace opennova::audio {

// The device frames per block: the stage runs when the mixer's frame counter is a multiple
// of 0x400 [orig: `test ecx, 3FFh` @0x7BEF16, the counter's `inc ecx` @0x7BEF66].
inline constexpr uint32_t kAudioOutBlockFrames = 0x400;
// The follower's reach in one block: it steps 16 a frame while channel zero's sample stands
// above it and restarts at 0 each block [orig: `shl eax, 4` @0x7BDE29; `xor esi, esi`
// @0x7BEF46], and a 16-bit sample never reaches past it.
inline constexpr int32_t kAudioOutAccumulatorReach = 16 * static_cast<int32_t>(kAudioOutBlockFrames);
// The device rate the block cadence counts in: the config's audio_rate, 44100 by default
// [orig: Config_SetDefaults @0x54D15B; read by Game_InitSubsystems @0x4A727E].
inline constexpr uint32_t kAudioOutDeviceRate = 44100;

// One block's level from the follower's value: (acc * acc, its low dword) >> 15, less
// 0x400, clamped to 0..0x3FF [orig: `mul eax` @0x7BEF22, `shr eax, 0Fh` @0x7BEF26, `sub eax,
// 400h; jnb` @0x7BEF29..0x7BEF2E, `cmp eax, 3FFh; jbe` @0x7BEF32..0x7BEF39].
int32_t audio_out_block_level(int32_t accumulator);

// A bus peak (linear amplitude, 0 silent, 1 full scale) as the follower's value: full scale
// lands on the follower's reach (kAudioOutAccumulatorReach). The one divergence of the meter:
// retail's follower reads channel zero's samples, ours the bus's peak.
int32_t audio_out_accumulator_from_peak(float peak);

class AudioOutMeter {
public:
    // Both words and the frame counter at 0 [orig: AudioMixer_Init @0x7BD405].
    void reset();
    // `seconds` of device time mixed since the last call, the bus's peak over them: the
    // device frames at kAudioOutDeviceRate (the fraction carried to the next call), each
    // block they end staged with the peak's follower value.
    void advance(double seconds, float peak);
    // `frames` device frames with the follower at `accumulator`: the stage runs at every
    // frame whose counter is a multiple of 0x400, then the counter steps.
    void mix(uint32_t frames, int32_t accumulator);
    // g_AudioOutLevelStage1 @0x3346FA4: (block + stage1) >> 1 [orig: @0x7BEF3E..0x7BEF48].
    int32_t stage1() const { return stage1_; }
    // g_AudioOutLevel @0x3346FA8: (stage1 + level) >> 1 [orig: @0x7BEF4D..0x7BEF55].
    int32_t level() const { return level_; }

private:
    int32_t stage1_ = 0;
    int32_t level_ = 0;
    uint32_t frame_ = 0;       // the mixer's frame counter (ecx, kept at dword_79CB20)
    double frame_carry_ = 0.0; // the device frames not yet whole
};

} // namespace opennova::audio
