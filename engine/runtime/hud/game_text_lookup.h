#pragma once

// The ONE game-text seam the engine's feed builders read through: the shape
// of retail's GameText_GetStringWithFallback — a table section, a key, and
// the fallback returned when the key is absent (an absent table answers the
// fallback too). The embedder binds it over its string-table document; the
// builders never see the table. The end-round ladder keeps its own
// presence-aware form (hud/end_round_overlay.h EndRoundTextLookup) because
// its present-but-empty fold is witnessed.

#include <functional>
#include <string>

namespace opennova::hud {

using GameTextLookup = std::function<std::string(
		const char *section, const char *key, const char *fallback)>;

} // namespace opennova::hud
