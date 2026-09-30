#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/hud/hud_map_view.h>

namespace godot {

// The windowed map views' pan/zoom state as the process-lifetime object a
// presenter holds across mission rebuilds: retail keeps both views in
// globals that no mission load resets — the DEATH view's zoom seeds only
// while it is still 0.0 and its load latch clears on unload, the CMAP view's
// init latch clears only at shutdown — so a new MapViewWindow over the same
// state reopens where the last one left off. Witness record:
// docs/interface/hud-re.md (D-HUD-19).
class MapViewState : public RefCounted {
	GDCLASS(MapViewState, RefCounted)

public:
	opennova::hud::DeathMapView death;
	opennova::hud::CommandMapView command;

protected:
	static void _bind_methods() {}
};

} // namespace godot
