#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

// The shell's gameplay camera seam (ADR 0043 slice G8): the lock verb
// LocalPlayerPresenter drives on the camera it presents through -- the
// gameplay lock (a local player owns the view, the death screen's spectator
// included: free flight, orbit and pan stand down while the presenter stamps
// the composed view). The behavior is the camera script's: FlyCamera
// (godot/game/fly_camera.gd, GDScript by ADR 0043) extends this class and
// overrides the virtual hook; a preview that flies no camera passes the
// presenter null and the lock is a no-op.
class GameplayCamera : public Camera3D {
	GDCLASS(GameplayCamera, Camera3D)

protected:
	static void _bind_methods();

	GDVIRTUAL1(_set_gameplay_locked, bool)

public:
	// The local player's live-view lock: while locked, the free-camera legs
	// yield to the presenter's stamps.
	void set_gameplay_locked(bool p_locked);
};

} // namespace godot
