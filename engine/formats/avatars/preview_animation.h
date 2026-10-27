#pragma once

// The menu player-preview animation constants — witnessed values the shell's
// avatar preview device animates with.
//
// The preview motion, stepped once a menu update, is runtime/menu/player_info_avatars.h's
// (player_info_preview_blend_step / player_info_preview_yaw_step); the zoom-in scale here
// is the shell's camera framing of the blend.
//
// Skeletal idle [orig: PlayerInfo_InitPreviewModel @ 0x5600d0]: the original
// binds the rest skeleton Dt1rst.bad + the looping idle clip PI_Idle.BAD
// (raw .bad files, no .adm) and plays the idle on the skinned parts.

namespace opennova::avatars {

inline constexpr float kPreviewZoomInScale = 0.78f;
inline constexpr const char *kPreviewSkeletonBad = "Dt1rst.bad";
inline constexpr const char *kPreviewIdleBad = "PI_Idle.BAD";
// A fresh preview model spawns at a random initial yaw in [0, 180) degrees —
// rand() % 180 [orig: PlayerInfo_InitPreviewModel @ 0x5600d0].
inline constexpr int kPreviewInitialYawRangeDeg = 180;

}  // namespace opennova::avatars
