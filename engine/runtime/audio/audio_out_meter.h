// The software mixer's output meter: the level the speaking entity's head kick, its TALK CTRL
// register and its friendly tag's pulse read. Retail's mixer follows channel zero (the
// scripted voice) every device frame and, every 0x400th frame, turns the follower into a
// block level it halves twice into two words. The engine's mixer runs the follower over channel
// zero's samples (audio::Mixer::render) and stages each block here.
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
// One block's level from the follower's value: (acc * acc, its low dword) >> 15, less
// 0x400, clamped to 0..0x3FF [orig: `mul eax` @0x7BEF22, `shr eax, 0Fh` @0x7BEF26, `sub eax,
// 400h; jnb` @0x7BEF29..0x7BEF2E, `cmp eax, 3FFh; jbe` @0x7BEF32..0x7BEF39].
int32_t audio_out_block_level(int32_t accumulator);

class AudioOutMeter {
public:
    // One block staged from the follower's value: its level halved into stage 1, and stage 1
    // halved into the level [orig: @0x7BEF1E..0x7BEF55].
    void stage(int32_t accumulator);
    // g_AudioOutLevelStage1 @0x3346FA4: (block + stage1) >> 1 [orig: @0x7BEF3E..0x7BEF48].
    int32_t stage1() const { return stage1_; }
    // g_AudioOutLevel @0x3346FA8: (stage1 + level) >> 1 [orig: @0x7BEF4D..0x7BEF55].
    int32_t level() const { return level_; }

private:
    int32_t stage1_ = 0;
    int32_t level_ = 0;
};

} // namespace opennova::audio
