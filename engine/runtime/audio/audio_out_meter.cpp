// The software mixer's output meter -- see audio_out_meter.h.
#include <runtime/audio/audio_out_meter.h>

#include <algorithm>

namespace opennova::audio {

int32_t audio_out_block_level(int32_t accumulator) {
    // `mul eax` keeps the low dword in eax; the stage reads it unsigned.
    const uint32_t square = static_cast<uint32_t>(accumulator) * static_cast<uint32_t>(accumulator);
    const uint32_t shifted = square >> 15;
    if (shifted < 0x400u) return 0;
    return static_cast<int32_t>(std::min<uint32_t>(shifted - 0x400u, 0x3FFu));
}

void AudioOutMeter::stage(int32_t accumulator) {
    // [orig: `add eax, g_AudioOutLevelStage1; shr eax, 1` @0x7BEF3E..0x7BEF48, `add eax,
    //  g_AudioOutLevel; shr eax, 1` @0x7BEF4D..0x7BEF55]
    const int32_t block = audio_out_block_level(accumulator);
    stage1_ = static_cast<int32_t>((static_cast<uint32_t>(block) + static_cast<uint32_t>(stage1_)) >> 1);
    level_ = static_cast<int32_t>((static_cast<uint32_t>(stage1_) + static_cast<uint32_t>(level_)) >> 1);
}

} // namespace opennova::audio
