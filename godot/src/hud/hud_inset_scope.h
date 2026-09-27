#pragma once
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <runtime/hud/inset_scope.h>
namespace godot {
class PlayerLocalView;
// Device owner of the weapon's additional Inset world render. Camera policy and
// ring geometry live in world/local_player_view and hud/inset_scope.
class HudInsetScope : public Control {
	GDCLASS(HudInsetScope, Control)
	SubViewport *target_ = nullptr;
	Camera3D *camera_ = nullptr;
	opennova::hud::InsetScopeGeometry geometry_;
	bool active_ = false;
	int aspect_mode_ = 0;

protected:
	static void _bind_methods();
	void _notification(int what);

public:
	void update_view(const Ref<PlayerLocalView> &view, Camera3D *source, int aspect_mode);
	// The world frame's refresh (GameWorld's local-view leg): the same update
	// over the view that leg just composed, through this surface's camera and
	// the aspect mode the HUD last passed. The engine composes the Inset pose
	// with the frame's view (world/local_player_view.h local_player_view_frame
	// carries the witness: retail composes the Inset camera inside the pass
	// that draws it), so every Inset leg reads this frame's camera.
	void refresh_view(const Ref<PlayerLocalView> &view);
	bool is_scope_active() const { return active_; }
	SubViewport *get_render_viewport() const { return target_; }
	// The second scene pass's camera while that pass renders, null otherwise
	// (scope down, no valid ring geometry, or hidden in tree): the view the
	// world's particle renderer compiles its second scene pair for.
	Camera3D *get_active_render_camera() const;
	Rect2 get_render_bounds() const;
	void _draw() override;
};
} // namespace godot
