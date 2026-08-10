#pragma once

// The menu player-preview animation constants — witnessed values the shell's
// avatar preview device animates with.
//
// Preview motion [orig: update_player_preview_animation @ 0x55dba0]: a damped
// zoom on hover (blend += (target - blend) * 0.05 per ~62.5 Hz tick) plus a
// continuous idle rotation (0x800000 BAM per frame; BAM angles map 2^32 =
// 360 deg) that gains a sinusoidal sway on hover (sin(GetTickCount * 0.0008)
// at 2^28 BAM amplitude).
//
// Skeletal idle [orig: PlayerInfo_InitPreviewModel @ 0x5600d0]: the original
// binds the rest skeleton Dt1rst.bad + the looping idle clip PI_Idle.BAD
// (raw .bad files, no .adm) and plays the idle on the skinned parts.

namespace opennova::avatars {

inline constexpr float kPreviewZoomDampPerTick = 0.05f;  // orig 0.95/0.05
inline constexpr float kPreviewZoomInScale = 0.78f;
inline constexpr float kPreviewIdleSpeedDegPerSec = 43.9f;  // 0x800000 BAM x 62.5 Hz
inline constexpr float kPreviewSwayFreqRadPerSec = 0.8f;    // 0.0008 / ms
inline constexpr float kPreviewSwayAmpDeg = 22.5f;          // 2^28 BAM
inline constexpr const char *kPreviewSkeletonBad = "Dt1rst.bad";
inline constexpr const char *kPreviewIdleBad = "PI_Idle.BAD";

}  // namespace opennova::avatars
