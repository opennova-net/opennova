#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <runtime/renderer/scene_overlay.h>

namespace godot {

// One immutable frame of the post-particle overlay tail: the engine frame
// (renderer::SceneOverlayFrame, every slot) plus the RenderingServer texture
// RIDs its batches index. Each view's effect compiles its own slot order out
// of it on the render thread.
struct SceneOverlaySubmission {
	std::uint64_t frame_id = 0;
	opennova::renderer::SceneOverlayFrame frame;
	std::vector<RID> textures;

	// The table index of a texture (each texture enters the table once);
	// kSceneOverlayNoTexture for a null one.
	std::uint32_t texture_index(const Ref<Texture2D> &p_texture);
};

// The post-particle overlay stage of one view: a POST_TRANSPARENT compositor
// pass installed right after the view's particle pass B and before the
// terminal frame effects (retail draws the tail after particle pass B and
// before the FrameFX bloom; runtime/renderer/scene_overlay.h carries the
// order and the witnesses). It draws the typed draw list into the view's
// resolved colour against its resolved depth, in the retail order, with each
// batch's fixed-function combine, blend and depth state.
class SceneOverlayCompositorEffect : public CompositorEffect {
	GDCLASS(SceneOverlayCompositorEffect, CompositorEffect)

public:
	enum ViewKind {
		VIEW_SCENE = 0,  // the main view and the scope aperture: the full tail
		VIEW_MIRROR = 1, // the water mirror: the coronas only
	};

	SceneOverlayCompositorEffect();
	~SceneOverlayCompositorEffect() override;

	void set_view_kind(ViewKind p_kind);
	ViewKind get_view_kind() const;
	void publish(const std::shared_ptr<const SceneOverlaySubmission> &p_submission);
	void clear_submission();
	// Release RenderingDevice objects only while the owner knows the server
	// is live; destruction afterwards discards handles.
	void release_device_resources();
	// The pass diagnostics, written into the owning renderer's draw-list
	// report (the one Dictionary transport edge those reports ride).
	void write_backend_report(Dictionary &r_report) const;

	void _render_callback(int32_t p_effect_callback_type, RenderData *p_render_data) override;

protected:
	static void _bind_methods();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace godot
