#pragma once

#include <cstdint>

#include <npwire/ingame_decode.h>
#include <world/match.h>

namespace opennova::np {

// The one match-domain -> retail-scoreboard projection. Field names in
// EndRoundPlayerRow are historical presentation names; this boundary preserves
// the producer's actual seven-word wire order.
EndRoundStats build_end_round_stats(const world::MatchResult &result);
EndRoundHeader build_end_round_header(const world::MatchResult &result,
		uint8_t recipient_slot);

} // namespace opennova::np
