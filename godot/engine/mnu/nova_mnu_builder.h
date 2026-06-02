#pragma once

#include <godot_cpp/classes/control.hpp>

#include <mnu/mnu.h>

#include <set>
#include <string>

namespace godot {

class NovaResourceRoot;
class MnsStyleSheet;
class RtxtStringFile;
class NovaMnuMenu;
class NovaMnuDocument;

// Shared inputs (and accumulated outputs) for building a live menu tree. The
// asset resolvers are optional: when a root/stylesheet/text resource is null the
// builder degrades gracefully (texture/font names that cannot be resolved fall
// back to text-only placeholders, %VAR% colors are left unapplied, string ids
// render as their literal id). owner/edit_mode drive interactivity (M5).
struct MnuBuildContext {
	NovaResourceRoot *root = nullptr;
	MnsStyleSheet *stylesheet = nullptr;
	RtxtStringFile *text = nullptr;
	NovaMnuMenu *owner = nullptr;
	// Source document, used only to tag each built widget Control with its stable id
	// (set_meta "mnu_widget_id") so the editor can map a Control back to a widget.
	// Null in pure-runtime builds with no document association (tagging is skipped).
	NovaMnuDocument *document = nullptr;
	bool edit_mode = false;
	// Interactive preview (ONED "play" mode): an edit_mode tree that nonetheless
	// wires its navigators (buttons/gotos) and lets them receive clicks, so an author
	// can click a tab and watch its window show/hide actions run. Only interactivity
	// is restored; the tree is still a non-runtime preview. See NovaMnuMenu::set_interactive.
	bool interactive = false;

	// True when the tree must be inert for authoring: click-through and nothing
	// wired. False both at runtime and in the interactive preview.
	bool inert() const { return edit_mode && !interactive; }

	// Distinct concrete asset names (textures/fonts, never %VAR% references) that
	// a resolver was asked for but could not load. Populated during the build so
	// NovaMnuMenu can surface an "N unresolved assets" count to the editor.
	std::set<std::string> unresolved_assets;
};

// Build one screen's live Control tree (a NovaMnuScreen with its widget
// children, real textures/fonts/colors/strings resolved through ctx). Returns a
// newly created Control the caller owns (parent it under the NovaMnuMenu). Never
// returns null for a valid screen. ctx is mutated to record unresolved assets.
// root_window_id is the document id of the screen's root window (or -1 when no
// document is associated); it threads the stable ids onto the built Controls.
Control *mnu_build_screen(const mnu::Screen &screen, MnuBuildContext &ctx, int root_window_id);

} // namespace godot
