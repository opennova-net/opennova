#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "mission/mission_root.h"
#include "player/local_player_presenter.h"
#include "world/game_world.h"

namespace godot {

// The shell seam of the debug-control table (ADR 0043 d12): the live owner
// suppliers the rows resolve on EVERY call (a world reload is picked up with
// no retained object, and a fresh mission gets fresh debug state) and the two
// shell verbs the table cannot own — the gated return-to-menu leg and the
// session-role authority fact (ROLE_JOINER is the one non-authoritative
// role). The base answers none; the game's GameDebugAdapter implements the
// hooks over the GameShell it adopted; a test fakes it by overriding the
// hooks (ADR 0043 rule 11: an interface class faked through its public verbs).
class DebugShellHost : public RefCounted {
	GDCLASS(DebugShellHost, RefCounted)

protected:
	static void _bind_methods();

	// The permanent world node (a loaded mission is `is_loaded()`).
	GDVIRTUAL0R(GameWorld *, _world)
	// The loaded world's runtime (its MissionRoot), null between missions.
	GDVIRTUAL0R(MissionRoot *, _runtime)
	GDVIRTUAL0R(LocalPlayerPresenter *, _player_presenter)
	// The render viewport the viewport rows mutate (null off-tree).
	GDVIRTUAL0R(Viewport *, _viewport)
	// The shell's one gated return leg.
	GDVIRTUAL0R(Error, _return_to_menu)
	// Authority is the session-role fact: every role but the joiner owns the
	// world.
	GDVIRTUAL0R(bool, _has_debug_authority)

public:
	GameWorld *world();
	MissionRoot *runtime();
	LocalPlayerPresenter *player_presenter();
	Viewport *viewport();
	Error return_to_menu();
	bool has_debug_authority();
};

} // namespace godot
