#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

// The shell's gameplay camera seam (ADR 0043 slice G8): the two lock verbs
// LocalPlayerPresenter drives on the camera it presents through -- the
// gameplay lock (a live local player owns the view; free flight, orbit and
// pan stand down) and the spectator hand-off (the free camera adopts the
// pose the presenter last stamped before taking ownership). The behavior is
// the camera script's: FlyCamera (godot/game/fly_camera.gd, GDScript by ADR
// 0043) extends this class and overrides the two virtual hooks; a preview
// that flies no camera passes the presenter null and the lock is a no-op.
class GameplayCamera : public Camera3D {
	GDCLASS(GameplayCamera, Camera3D)

protected:
	static void _bind_methods();

	GDVIRTUAL1(_set_gameplay_locked, bool)
	GDVIRTUAL1(_set_spectator_mode, bool)

public:
	// The local player's live-view lock: while locked, the free-camera legs
	// yield to the presenter's stamps.
	void set_gameplay_locked(bool p_locked);
	// The spectator edge: adopt the presented pose, then own the view.
	void set_spectator_mode(bool p_active);
};

} // namespace godot
