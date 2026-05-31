#pragma once

#include <godot_cpp/classes/control.hpp>

#include <mnu/mnu.h>

namespace godot {

class NovaResourceRoot;
class MnsStyleSheet;
class RtxtStringFile;
class NovaMnuMenu;

// Shared inputs for building a live menu tree. Asset resolvers are optional:
// when null the builder falls back to placeholder visuals (used before asset
// resolution lands in M4). owner/edit_mode drive interactivity (M5).
struct MnuBuildContext {
	NovaResourceRoot *root = nullptr;
	MnsStyleSheet *stylesheet = nullptr;
	RtxtStringFile *text = nullptr;
	NovaMnuMenu *owner = nullptr;
	bool edit_mode = false;
};

// Build one screen's live Control tree (a NovaMnuScreen with its widget
// children). Returns a newly created Control the caller owns (parent it under
// the NovaMnuMenu). Never returns null for a valid screen.
Control *mnu_build_screen(const mnu::Screen &screen, const MnuBuildContext &ctx);

} // namespace godot
