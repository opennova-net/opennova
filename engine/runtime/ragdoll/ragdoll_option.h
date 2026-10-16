#pragma once

// The opt-in ragdoll corpses' switch (ADR 0060, D-INF-34): bit 1 of game.cfg's
// `NoBlood` word. Retail parses the word and copies its low byte at each
// session start, and nothing in retail reads the copy, so any value is inert
// in Jointops.exe and its save writes the word back as read; OpenNova's own
// reader and writer keep it verbatim the same way. Bit 0 stays retail's
// (inert) no-blood flag; bit 1 turns OpenNova's ragdolls on. Like the other
// session settings, a mission start copies it and the session keeps that copy.
// [orig: Config_ParseSettingsLine @ 0x54F740 (the 'n' arm's `NoBlood`, game.cfg
//  +0x174); the /NOBLOOD switch Game_ParseCommandLineAndInit @ 0x4A7952; the
//  session copy Game_ApplySessionSettingsToGlobals @ 0x551553 (g_SessionNoBlood,
//  which no instruction reads)]

#include <cstdint>

namespace opennova::ragdoll {

inline constexpr int32_t kRagdollNoBloodBit = 0x2;

inline constexpr bool ragdolls_enabled(int32_t no_blood) {
	return (no_blood & kRagdollNoBloodBit) != 0;
}

} // namespace opennova::ragdoll
