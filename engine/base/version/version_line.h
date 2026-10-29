// The three versions a running game has, on one line: the game's release, ONE's build and the
// protocol it speaks, "OP: Acid Gambit 0.1.4 · ONE 0.0.22 (a1b2c3d) · JO protocol 16".
//
// The runtime's (godot/src LaunchFlags::version_line, which the STARTUP screen's VERSION label
// shows), the editor's (editor_version_line: Help and a menu tried in Try mode) and
// opennova-serve's startup log.
//
// Infrastructure, not a port.
#pragma once

#include <optional>
#include <string>

#include <base/version/one_build.h>
#include <base/version/version.h>

namespace opennova {

struct Versions {
	// The game's name ("OP: Acid Gambit", "Joint Operations"); empty when none is known.
	std::string game;
	// Its installed release (the command file's [HEADER] VERSION); none when no release is
	// installed.
	std::optional<Version> game_version;
	// The ONE that runs it (one_build() for this program's own).
	OneBuild one;
	// The protocol's name ("JO", "DFX2"); empty when none is known.
	std::string protocol;
	// The protocol's revision, its PV2 ("16"); empty when none is known.
	std::string protocol_version;
};

// UTF-8 "<game> <game version> · <ONE label> · <protocol> protocol <protocol version>", the
// parts joined by " · " (U+00B7). A part that is unknown is left out with its separator: no
// installed release is "Joint Operations · ONE 0.0.22 · JO protocol 16", no protocol ends at
// ONE's label.
std::string version_line(const Versions &versions);

} // namespace opennova
