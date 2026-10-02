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

// The game-text table (loaded at boot [orig: Game_InitSubsystems @ 0x4A6CD0]) and the
// sections the game reads from it by name, each witnessed where it is read: the HUD's
// overlay lines, the weapon names, the waypoint names, the kill-feed sentences, the
// client lines, the loading-screen text and the objective header.
inline constexpr const char *kGameTextTable = "gametext.bin";
inline constexpr const char *kGameTextOverlays = "Overlays";
inline constexpr const char *kGameTextWepDes = "WepDes";
inline constexpr const char *kGameTextWPNames = "WPNames";
inline constexpr const char *kGameTextCannedMsg = "Canned Msg";
inline constexpr const char *kGameTextClient = "Client";
inline constexpr const char *kGameTextLoadingText = "LoadingText";
inline constexpr const char *kGameTextMisc = "Misc";

// An EMPTY function is the "no string table" binding: every consumer answers
// the fallback through it, so a builder never tests the target before calling
// (call through game_text or the same guard, never the bare function).
using GameTextLookup = std::function<std::string(
		const char *section, const char *key, const char *fallback)>;

inline std::string game_text(const GameTextLookup &lookup, const char *section, const char *key,
		const char *fallback) {
	return lookup ? lookup(section, key, fallback) : std::string(fallback);
}

} // namespace opennova::hud
