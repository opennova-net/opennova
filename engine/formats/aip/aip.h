#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova::aip {

// The .aip AI-profile text format — PARTIAL PORT, and honest about it: only
// the two witnessed fields are parsed ([orig: AIProfile_ParseProperty
// "patrol_speed" @ 0x45E6DF..0x45E717 / "combat_speed" -> profile+0xC4]).
// The remainder of the profile is unported and ledgered under the D-AI-11
// residuals (docs/divergence-ledger.md); newly witnessed fields land HERE,
// not in a runtime resolver (ADR 0030). -1 = the field was absent.
struct ProfileSpeeds {
    int32_t patrol_speed = -1;
    int32_t combat_speed = -1;
};

// Parse the two witnessed keys from .aip text: line-oriented "<key> <value>"
// with tabs as spaces, keys compared case-insensitively, atoi-shaped numeric
// reads (leading sign + digits, junk tails ignored).
ProfileSpeeds parse_profile_speeds(const uint8_t *text, size_t size);

}  // namespace opennova::aip
