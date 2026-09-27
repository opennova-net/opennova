#pragma once

#include <string>
#include <vector>

namespace opennova::audio {

// The global sound-bank load chain in the engine's slot order — the six
// 0x104-stride name slots [orig: g_SoundBankNameExpansionLocl @ 0x82A5B0]:
// <exp>L.lwf and <exp>.lwf (Expansion_LoadAssets fills them @ 0x4a4989 /
// @ 0x4a495e and clears them to empty when no expansion is active
// @ 0x4a4824 / @ 0x4a4818), then the static gamelocl.lwf / game.lwf /
// game3.lwf / game2.lwf slots @ 0x82A7B8..0x82AAC4. Game_StartMission walks
// all six slots load-if-exists [orig: Game_StartMission @ 0x525443]; the
// empty no-expansion slots are dropped here, folding that skip.
std::vector<std::string> global_bank_chain(const std::string &expansion_name);

} // namespace opennova::audio
