#pragma once

#include <cstdint>
#include <string>

namespace opennova::editor {

// How Play runs a project's build (Play mode, CONTEXT.md): in the OpenNova runtime, or in the game
// install as a player's drop-in (Strict Play: the build and the install's program alone, no /d). A
// project's own choice, kept in its local settings (LocalSettings::play_mode, local_settings.h). Each
// is spelled by its token in local.json and on the wire, the name of its run directories too
// (run/run_directory.h's kRunMode*). (S26 K1: the install mode, the install's configuration and saves
// beside the build launched with /d, is gone: neither a player's launch nor OpenNova's.)
enum class PlayMode : uint8_t { Runtime, Strict };

inline constexpr PlayMode kPlayModes[] = {PlayMode::Runtime, PlayMode::Strict};

// A mode's token: "runtime" or "strict".
constexpr const char *play_mode_token(PlayMode mode) { return mode == PlayMode::Strict ? "strict" : "runtime"; }

// The mode `token` names; false for a token no mode has (`out` left as it was).
inline bool play_mode_from_token(const std::string &token, PlayMode &out) {
	for (const PlayMode mode : kPlayModes) {
		if (token == play_mode_token(mode)) {
			out = mode;
			return true;
		}
	}
	return false;
}

// Whether a Play of `mode` runs the game install's program (Strict Play) rather than the OpenNova runtime.
constexpr bool plays_in_install(PlayMode mode) { return mode == PlayMode::Strict; }

} // namespace opennova::editor
