#pragma once

#include <memory>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

class Compositor;
class WorldEnvironment;

// The frame's terminal compositor effect: the post-transparent device leg
// performing the sole display decode. Every engine shader writes gamma-domain
// numeric values into Godot's floating-point scene target (color.gdshaderinc
// carries the witnessed device contract); this effect copies the resolved
// color once and applies the display-backend transfer immediately before
// Godot's sRGB output encode. Canvas/viewmodel/HUD passes run afterward.
// Retail's FrameFX bloom bracket that used to ride this callback (the focused
// Q3 re-render, capture, blur, and additive composite) is retired under
// ADR 0043: Environment glow is the canonical bloom.
class DisplayDecodeEffect : public CompositorEffect {
	GDCLASS(DisplayDecodeEffect, CompositorEffect)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;

protected:
	static void _bind_methods();

public:
	DisplayDecodeEffect();
	~DisplayDecodeEffect() override;

	// Process-exit boundary: stop render callbacks and release device
	// resources while RenderingServer and RenderingDevice are still live.
	void release_device_resources();
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

// Installs the display decode on a 3D view: appended last on the nearest
// WorldEnvironment's compositor, or directly on the viewport's World3D when
// the view has none. Every 3D view needs exactly one — the game world, ONED
// workspace previews, the menu avatar preview, probes — or the viewport shows
// the gamma-domain scene encoded twice.
class DisplayDecode : public Node3D {
	GDCLASS(DisplayDecode, Node3D)

private:
	Ref<DisplayDecodeEffect> effect_;
	// The owning WorldEnvironment by identity: an embedder may free it before
	// this node leaves the tree (preview teardown), so never a raw pointer.
	ObjectID world_environment_id_;
	Ref<World3D> world_;
	Ref<Compositor> previous_compositor_;
	Ref<Compositor> installed_compositor_;
	bool shutdown_ = false;

	void install();
	void uninstall();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// Explicit teardown ahead of tree exit (the game world's ordered
	// teardown); re-entering the tree re-installs through the READY leg.
	void shutdown();
	Dictionary get_backend_report() const;
};

} // namespace godot
