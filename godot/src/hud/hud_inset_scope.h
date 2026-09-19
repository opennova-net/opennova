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

protected:
	static void _bind_methods();
	void _notification(int what);

public:
	void update_view(const Ref<PlayerLocalView> &view, Camera3D *source, int aspect_mode);
	bool is_scope_active() const { return active_; }
	SubViewport *get_render_viewport() const { return target_; }
	Rect2 get_render_bounds() const;
	void _draw() override;
};
} // namespace godot
