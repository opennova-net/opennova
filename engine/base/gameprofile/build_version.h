#pragma once

// The binary's own version text: the STARTUP screen's VERSION label and the joiner's
// VERSIONSTRING upload. Game_ParseCommandLineAndInit prints the build's four numbers,
// sprintf("V%i.%i.%i.%i", 1, 7, 5, 7), into two buffers: byte_B4C070 the menu's (read by
// UI_OnStartupScreenActivate @ 0x555852) and byte_B4C0B0 the session's (the ClientAuth copy
// @ 0x569b56, the metrics, the connect log).
// [orig: Game_ParseCommandLineAndInit @ 0x4a7310 — the menu's @ 0x4a7d5c..0x4a7d6d, the
//  session's @ 0x4a7d75..0x4a7d90]

namespace opennova::gameprofile {

inline constexpr char kBuildVersionText[] = "V1.7.5.7";

} // namespace opennova::gameprofile
