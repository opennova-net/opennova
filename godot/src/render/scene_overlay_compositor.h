#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

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

// The SELFLUM surfaces of one model for the overlay tail: the sun glare and
// the water glint, whose placement, UPL_INTENSITY submit value and Q3 copy
// env/Celestial keeps while this stage draws them (retail submits both with
// flags 0x110 at the scene tail: render_skybox_sun_glow @ 0x5ad0f7,
// update_sun_glare @ 0x5ad470). Each mesh's triangle list is read once (the
// authored geometry never changes) and placed by its instance's global
// transform every frame; the SelfLumColor (u_rgb_mod) and the diffuse
// texture (u_diffuse) come from the surface's live material.
class SceneOverlayModelSurfaces {
public:
	// How one model's surfaces enter the tail: the world offset every placed
	// vertex takes (a sky body placed at the main camera, redrawn at another
	// view's camera), the depth state, and per-instance SelfLumColor
	// overrides (a redraw at another submit value); an instance without one
	// takes its material's u_rgb_mod.
	struct AppendOptions {
		Vector3 offset;
		opennova::renderer::SceneOverlayDepth depth =
				opennova::renderer::SceneOverlayDepth::Always;
		const std::unordered_map<const MeshInstance3D *, std::array<float, 3>>
				*self_lum = nullptr;
	};
	// Append every mesh surface under `p_model` as one SELFLUM batch of
	// `p_slot`; returns the batches appended.
	int append(opennova::renderer::SceneOverlaySlot p_slot, Node *p_model,
			const float p_light_scale_rgb[3], float p_fog_visibility,
			SceneOverlaySubmission &r_submission,
			const AppendOptions &p_options = AppendOptions());
	// Take the model's meshes out of every camera (layer mask 0) so only the
	// stage draws them; re-applied every frame because a model rebuild
	// re-stamps its presentation layers.
	static void take_over(Node *p_model);
	void clear();

private:
	struct Geometry {
		std::vector<float> positions; // local, 3 per vertex, triangle list
		std::vector<float> uvs;       // 2 per vertex
	};
	const std::vector<Geometry> &geometry_for(const Ref<Mesh> &p_mesh);
	int append_instance(opennova::renderer::SceneOverlaySlot p_slot,
			MeshInstance3D *p_instance, const float p_light_scale_rgb[3],
			float p_fog_visibility, SceneOverlaySubmission &r_submission,
			const AppendOptions &p_options);

	std::map<std::uint64_t, std::vector<Geometry>> geometry_;
	std::vector<float> placed_;
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
		VIEW_MIRROR = 1, // the water mirror: the coronas, the dim and the sky redraw
		// The weapon Inset pass: the tail without the glare, its own coronas
		// (runtime/renderer/scene_overlay.h kInsetOverlayOrder).
		VIEW_INSET = 2,
	};

	SceneOverlayCompositorEffect();
	~SceneOverlayCompositorEffect() override;

	void set_view_kind(ViewKind p_kind);
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
