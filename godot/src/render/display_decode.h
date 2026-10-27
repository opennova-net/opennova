#pragma once

// The decode-only view's node, split from FrameFx's TU pair.

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include "render/frame_fx_compositor_effect.h"

namespace godot {

// The display decode for a 3D view that has no FrameFx (the menu avatar
// preview, probes). Every engine shader
// writes gamma-domain numbers into the scene target and relies on exactly one
// terminal decode before Godot's sRGB output encode; without it a viewport
// shows the scene encoded twice. This node installs a decode-only
// FrameFxCompositorEffect (no Q3 source, so no FrameFX composite) on the
// nearest WorldEnvironment, or directly on the viewport's World3D when the
// view has none.
class DisplayDecode : public Node3D {
	GDCLASS(DisplayDecode, Node3D)

private:
	Ref<FrameFxCompositorEffect> effect_;
	// The owning WorldEnvironment by identity: an embedder may free it before
	// this node leaves the tree (preview teardown), so never a raw pointer.
	ObjectID world_environment_id_;
	Ref<World3D> world_;
	Ref<Compositor> previous_compositor_;
	Ref<Compositor> installed_compositor_;

	void install();
	void uninstall();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// The compositor a DisplayDecode installed on `p_world`'s scenario (a view
	// with no WorldEnvironment), else null: what a camera with no compositor of
	// its own renders with there, as a WorldEnvironment's compositor is where
	// the view has one. A leg that gives a camera its own compositor composes
	// it around this one (the particle renderer keeps the decode last behind
	// its passes), or the view loses its one decode while it draws. Godot binds
	// no getter for a scenario's compositor.
	static Ref<Compositor> scenario_compositor(const Ref<World3D> &p_world);
};

} // namespace godot
