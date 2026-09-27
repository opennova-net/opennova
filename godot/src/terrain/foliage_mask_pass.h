#pragma once

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <formats/foliage/foliage.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::renderer {
struct FoliageDrawList;
class FoliageFrameCompiler;
} // namespace opennova::renderer

namespace godot {

class Camera3D;
class Node;

// One slot's normalized MODEL mesh as device bytes: (x, y, z, u, v) floats
// per vertex and u32 indices (opennova::renderer::FoliageSlotModelMesh).
struct FoliageMaskMesh {
	uint64_t generation = 0;
	PackedByteArray vertices;
	PackedByteArray indices;
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;
};

// One MODEL depth-mask draw: the slot mesh drawn once per instance block.
struct FoliageMaskDraw {
	int slot = 0;
	bool far_side = false;
	uint32_t first_instance = 0;
	uint32_t instance_count = 0;
	float alpha_reference = 0.0f;
	float wind_offset = 0.0f;
};

// One immutable frame for the render thread.
struct FoliageMaskFrame {
	uint64_t frame_id = 0;
	std::array<std::shared_ptr<const FoliageMaskMesh>, opennova::FOLIAGE_MAX_DEFS> meshes;
	// RenderingServer texture RIDs of the slots' :fd textures, resolved to
	// RenderingDevice textures on the render thread.
	std::array<RID, opennova::FOLIAGE_MAX_DEFS> fd_textures;
	// 16 floats per instance: the FoliageModelInstance rows.
	PackedByteArray instance_rows;
	std::vector<FoliageMaskDraw> draws;
	// The RG32F mask target (owned by FoliageMaskPass) and its size.
	RID target;
	Vector2i target_size;
	// An Inset frame's camera transform: the render of that camera draws it.
	Transform3D view_camera;
};

// The pass's diagnostics (FoliageDispatcher::get_backend_report "mask").
struct FoliageMaskReport {
	bool callback_seen = false;
	std::string status;
	std::string failure;
	uint64_t drawn_frame_id = 0;
	int64_t drawn_draws = 0;
	int64_t views = 0;
	bool installed = false;
	bool active = false;
	Vector2i target_size;
	int64_t draws = 0;
	int64_t instances = 0;
	// The weapon Inset view's published flag and eye.
	bool view_active = false;
	Vector3 view_eye;
};

// The MODEL-tier depth masks as a texture for the opaque pass. Retail draws
// each mask immediately inside the BySide entity wave, after that wave's
// MATCHTERRAIN pre-pass and before the wave's queued entities flush, so the
// person entities of the wave (and everything drawn after them) depth-test
// against the masks while the terrain, sector models and first entity wave
// drawn before do not (retail Terrain_RenderWorldScene
// @ 0x5c953e..0x5c9567 far wave, @ 0x5c9600..0x5c9647 camera wave;
// Terrain_RenderSectorEntitiesBySide @ 0x5c7ddf..0x5c7f11 masks, before
// Render_SectorEntity @ 0x5c7ffc). Godot draws those persons in its opaque
// pass, before any transparent-pass mask instance, so this PRE_OPAQUE pass
// rasterizes the same masks for each view that runs it into a texture the
// view's opaque pass then samples: R = the nearest far-wave mask depth, G =
// the nearest camera-wave one (Godot reverse-Z, 0 = none); the person
// shaders compare against it (shaders/object/match_terrain.gdshaderinc).
class FoliageMaskCompositorEffect : public CompositorEffect {
	GDCLASS(FoliageMaskCompositorEffect, CompositorEffect)

public:
	FoliageMaskCompositorEffect();
	~FoliageMaskCompositorEffect() override;

	void publish(const std::shared_ptr<const FoliageMaskFrame> &p_frame);
	// The weapon Inset pass's own frame (null clears it): the render whose
	// camera transform equals the frame's view_camera draws it instead.
	void publish_view(const std::shared_ptr<const FoliageMaskFrame> &p_frame);
	// Stop callbacks and free device objects while the RenderingDevice lives.
	void release_device_resources();
	// The render side's half of the report.
	void fill_report(FoliageMaskReport &r_report) const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;

protected:
	static void _bind_methods();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
	std::atomic<bool> shutdown_requested_{false};
};

// Main-thread owner of the mask pass: installs the effect on the scope's
// WorldEnvironment compositor, owns the RG32F target the opaque-pass
// consumers sample through the opennova_foliage_mask_depth global, publishes
// the compiling camera's eye (opennova_foliage_mask_eye: only that view's
// persons take the masks) and one frame per foliage compile; the weapon Inset
// pass publishes its own frame and eye (opennova_foliage_mask_inset_eye /
// _inset_active) the same way.
class FoliageMaskPass {
public:
	FoliageMaskPass();
	~FoliageMaskPass();

	// p_scope anchors the WorldEnvironment lookup and the viewport the target
	// is sized for.
	void publish(Node *p_scope,
			const opennova::renderer::FoliageDrawList &p_draw_list,
			const opennova::renderer::FoliageFrameCompiler &p_compiler,
			const std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> &p_fd_textures);
	// The weapon Inset pass's own mask frame, compiled for `p_camera` after
	// the main one (retail's Inset scene core draws its own BySide masks):
	// drawn by the Inset camera's render, into the same target, before that
	// render's opaque pass. Needs the main publish's installed effect.
	void publish_view(Camera3D *p_camera,
			const opennova::renderer::FoliageDrawList &p_draw_list,
			const opennova::renderer::FoliageFrameCompiler &p_compiler,
			const std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> &p_fd_textures);
	void clear_view();
	// Publishes an empty frame (both views') and clears the active global.
	void clear();
	void release();
	FoliageMaskReport get_report() const;

private:
	std::shared_ptr<FoliageMaskFrame> _build_frame(
			const opennova::renderer::FoliageDrawList &p_draw_list,
			const opennova::renderer::FoliageFrameCompiler &p_compiler,
			const std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> &p_fd_textures,
			int64_t &r_instances);
	void _install(Node *p_scope);
	void _uninstall();
	bool _ensure_target(const Vector2i &p_size);
	void _flush_deferred_frees(bool p_all);
	void _set_active(bool p_active);
	void _set_eye(const Vector3 &p_eye);
	void _set_view_active(bool p_active);
	void _set_view_eye(const Vector3 &p_eye);

	Ref<FoliageMaskCompositorEffect> effect_;
	Ref<Compositor> installed_into_;
	Ref<Texture2DRD> target_texture_;
	RID target_;
	Vector2i target_size_;
	struct DeferredFree {
		RID rid;
		uint64_t frame = 0;
	};
	std::vector<DeferredFree> deferred_frees_;
	std::array<std::shared_ptr<const FoliageMaskMesh>, opennova::FOLIAGE_MAX_DEFS> meshes_;
	uint64_t mesh_generation_ = 0;
	uint64_t frame_ = 0;
	bool active_ = false;
	bool active_written_ = false;
	Vector3 eye_;
	bool eye_written_ = false;
	bool texture_bound_ = false;
	int64_t last_draws_ = 0;
	int64_t last_instances_ = 0;
	// Whether the main / Inset frame carries masks (each view's active global).
	bool main_draws_ = false;
	bool view_draws_ = false;
	// The Inset view's globals (opennova_foliage_mask_inset_active / _eye).
	bool view_active_ = false;
	bool view_active_written_ = false;
	Vector3 view_eye_;
	bool view_eye_written_ = false;
};

} // namespace godot
