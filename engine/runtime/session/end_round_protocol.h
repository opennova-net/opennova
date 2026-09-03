#pragma once

#include <cstdint>

#include <net/npwire/ingame_decode.h>
#include <runtime/world/match.h>

namespace opennova::np {

// The one match-domain -> retail-scoreboard projection. Field names in
// EndRoundPlayerRow are historical presentation names; this boundary preserves
// the producer's actual seven-word wire order.
EndRoundStats build_end_round_stats(const world::MatchResult &result);
// non_team_form mirrors the serializer's `is_in_session && !(GameType &
// 0x10000)` pick [orig: EndRoundScoreboard_SerializeHeader @0x5052a6]; our
// SP-as-listen-server passes world.mp_session for the retail is_in_session.
EndRoundHeader build_end_round_header(const world::MatchResult &result,
		uint8_t recipient_slot, bool non_team_form);

} // namespace opennova::np
