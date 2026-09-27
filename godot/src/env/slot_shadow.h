#pragma once

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <vector>

#include <runtime/renderer/render_slot_shadow.h>

#include "render/slot_capture_adapter.h"

namespace godot {

class EffectLightDirector;
class LightScene;
class MissionEnvironment;
class ObjectModel;
class TerrainData;
class Weather;

// The render-slot entity ground-shadow device (the Godot half of
// engine/runtime/renderer/render_slot_shadow.h — the planner carries the
// witness map). Owns the 12 silhouette capture targets (the retail RT
// chain) as RenderingDevice textures the drape samples through Texture2DRDs,
// publishes one typed capture request per armed slot to the
// SlotCaptureCompositorEffect it installs on the beauty view's compositor
// (PRE_OPAQUE: the captures finish before the terrain drape samples them),
// steers each capture along the planner's slot direction (the clamped sun,
// or the dominant nearby point light), and publishes the drape projection
// matrices + per-channel shadow terms the terrain drape next-pass multiplies
// in (godot/shaders/slot_shadow_drape.gdshader). A bound slot past the
// capture budget drapes nothing: retail's authored-blob leg for it is dead in
// JO (render_slot_shadow.h).
//
// Device folds (documented on docs/render/render-lighting-re.md): the drape
// projects per-pixel over the terrain surface, bounded by the retail lod x
// lod patch placed by the anchor march (opennova::renderer::slot_patch_bounds over the
// TerrainData height query) and clipped by the shadowztex depth stage
// (opennova::renderer::slot_depth_clip) — both published per slot to the shader;
// capture-with linked children (held weapons, mounted riders) render into
// their parent's slot through the per-frame claim pass — the
// RenderSlot_RenderEntityAndChildren child walk — while tree-parented riders
// fold into the ancestor exclusion and ride the parent's subtree walk; the
// attached-light drape darkening folds the light's attenuation at the entity
// into the per-slot term; the capture eye backs off the caster along the
// slot direction (the RenderingDevice depth band of advance_frame, D-RLIT-10)
// where retail renders the entity at the origin of a rotation-only view.
//
// Driven once per display frame by the GameWorld leg table through
// GameWorld.render_slot_shadow_frame(), after the light select has pushed
// this frame's LightScene and context into it — never self-clocked.
class SlotShadow : public Node3D {
	GDCLASS(SlotShadow, Node3D)

public:
	// Models flagged as dynamic shadow casters join this group
	// (ObjectModel::set_shadow_caster_enabled).
	static const StringName &caster_group();
	// The shared drape material every patch instance draws with, on
	// renderer::kRungSlotDrape and reading the terrain/sky stencil mark.
	static Ref<ShaderMaterial> get_drape_material();
	// The capture texture bound to the drape's u_slot_tex_<order> (a
	// Texture2DRD over the live device's resolve target; empty of a device
	// texture while no RenderingDevice exists).
	static Ref<Texture2D> get_capture_texture(int p_order);
	static int get_capture_count();
	// Module shutdown hook (register_types uninitialize).
	static void cleanup_statics();

	SlotShadow();

	void set_environment_node(MissionEnvironment *p_environment);
	// The terrain the anchor march probes [orig: Terrain_GetHeightAtPosition
	// @0x606720 inside RenderSlot_UpdateEntityLight]; without it the anchor
	// stays at the entity (the march's step-0 exit on a ground-standing
	// caster).
	void set_terrain_data(const Ref<TerrainData> &p_terrain);
	// The EffectWorld pool the per-slot dominant-light pick reads, and the
	// owner ids its interior room lights resolve to.
	void set_light_director(const Ref<EffectLightDirector> &p_director);
	void set_light_context(const Vector3 &p_gain, int p_time_ms,
			Weather *p_weather);
	// The retail shadow-detail option (0..4) driving the RT chain base and
	// the refresh cadence. The packaged runtime serves the top setting.
	void set_shadow_detail(int p_detail);
	// The local player's model: halved slot priority, the every-frame
	// refresh exception, and the first-person drape gates (detail >= 2, not
	// prone) [orig: RenderSlot_DrawAllDrapes @0x5d6e70..0x5d6e90, see
	// docs/render/render-lighting-re.md].
	void set_local_player_model(ObjectModel *p_model);
	// The model of the vehicle the local player rides (null on foot): it
	// shares the local player's halved priority and every-frame refresh
	// (retail: RenderSlot_SortAndAssign @0x5d669c, local->parent).
	void set_local_player_parent_model(ObjectModel *p_model);
	void set_local_player_first_person(bool p_first_person);
	void set_local_player_prone(bool p_prone);
	// The terrain sector pass's gate: retail draws every drape inside that
	// pass, which the indoors letter skips with its terrain, while the slot
	// captures run earlier in the frame and keep going
	// (renderer::kRungSlotDrape carries the witness). OcclusionFrame closes it
	// with the terrain's; a closed pass hides every drape patch at once.
	void set_terrain_pass_drawn(bool p_drawn);
	bool is_terrain_pass_drawn() const { return terrain_pass_drawn_; }

	// One display frame: plan, publish the armed capture requests, publish
	// the drape terms.
	void advance_frame();
	// F3 Stats capture toggle for the capture pass's RD GPU span (see
	// SlotCaptureCompositorEffect::set_gpu_timing_enabled).
	void set_gpu_timing_enabled(bool p_enabled);
	// Membership/derived-fact revision for the caster registry: ObjectModel
	// bumps it from every site that can change the group population or a
	// cached per-caster fact (group add/remove incl. husk swap, capture-with,
	// person, radius stamps, reparenting). The frame rebuilds its
	// records only when this moved; a freed node self-heals through its null
	// ObjectDB resolve. Main-thread only.
	static uint64_t caster_group_revision();
	static void bump_caster_group_revision();
	Dictionary get_report() const;

	// Inspection seams (the F3 sampler and the GUT pins): the bitmask of
	// slots advance_frame armed for a capture this frame, the slot order a
	// caster renders into this frame (its own, or the parent's it is claimed
	// into; -1 for none), the caster count of an armed order's request, the
	// device target side of an order, whether the capture effect sits on the
	// beauty compositor, and a readback of an order's resolved capture (empty
	// without a RenderingDevice; call after RenderingServer.force_sync()).
	int get_armed_capture_mask() const { return static_cast<int>(armed_capture_mask_); }
	int get_capture_order_of(ObjectModel *p_model) const;
	int get_capture_caster_count(int p_order) const;
	int get_capture_target_size(int p_order) const;
	bool is_capture_effect_installed() const;
	Ref<Image> get_capture_image(int p_order) const;
	// The drape patch mesh an order draws this frame (world positions in
	// vertex order; empty when the order draws no drape).
	PackedVector3Array get_patch_vertices(int p_order) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct CasterInfo {
		ObjectModel *model = nullptr;
		opennova::renderer::SlotCandidateState state;  // bound_radius = entity+0 (lod, light query)
		// The model sphere (gpm[5]) the silhouette capture extent and the
		// depth clip size from [orig: RenderSlot_RenderEntityAndChildren
		// @0x5d7835 reads the model's +0x14, not entity+0].
		float capture_radius = 1.0f;
	};
	// The registry row behind CasterInfo: identity plus the derived values a
	// frame used to re-read from the node every frame. An unstamped model
	// (radius_fallback) keeps reading its live render bounds per frame — the
	// preview/test path — so only stamped facts cache.
	struct CasterRecord {
		ObjectID id;
		float capture_radius = 0.0f;
		float slot_radius = 0.0f;
		bool radius_fallback = false;
		bool is_person = false;
		bool seat_parented_ancestor = false;
	};
	void _rebuild_caster_records();

	void _ensure_captures();
	void _release_captures();
	void _install_effect();
	void _uninstall_effect();
	bool _ensure_capture_target(int p_order, int p_size);
	void _flush_deferred_frees(bool p_all);
	void _clear_all_terms();
	void _invalidate_uniform_stamps();
	Projection _drape_projection(const Transform3D &p_pose, float p_half_u,
			float p_half_v) const;

	opennova::renderer::RenderSlotPlan plan_;
	Ref<SlotCaptureCompositorEffect> effect_;
	// The compositor the effect was added to (the scope WorldEnvironment's).
	ObjectID world_environment_id_;
	Ref<Compositor> installed_into_;
	// The resolve targets by order (RD textures this node owns; the static
	// Texture2DRDs wrap them for the drape) and their sides.
	RID capture_targets_[opennova::renderer::kSlotCaptureCount];
	int capture_target_sizes_[opennova::renderer::kSlotCaptureCount] = {};
	// A replaced target stays alive until the render side has finished the
	// frames that named it (two main-thread frames later).
	struct DeferredFree {
		RID rid;
		uint32_t frame = 0;
	};
	std::vector<DeferredFree> deferred_frees_;
	// This frame's published requests and the caster -> order map behind
	// get_capture_order_of.
	std::vector<SlotCaptureRequest> requests_;
	HashMap<uint64_t, int> capture_orders_;
	// Slots armed for a capture by the latest advance_frame (bit = order).
	uint32_t armed_capture_mask_ = 0;
	// The casters registered with the plan (released on churn).
	HashSet<uint64_t> registered_ids_;
	std::vector<CasterRecord> caster_records_;
	uint64_t caster_records_revision_ = 0;
	bool caster_records_valid_ = false;
	// Reused frame scratch (cleared, capacity retained).
	std::vector<CasterInfo> casters_scratch_;
	HashMap<uint64_t, size_t> caster_index_scratch_;
	// Last-pushed uniform payloads: identical-value pushes are elided (the
	// materials retain them), the T1 material-gating precedent. The drape
	// material is process-shared, so a stamp is only trustworthy while this
	// is the ONE live SlotShadow writing it (a second writer's push would
	// leave a stale stamp here and an elided frame would keep ITS value) and
	// while the material object the stamp was taken against still exists:
	// advance_frame invalidates every stamp when either condition fails
	// (live_instances_ above one, or the drape material's RID changed).
	RID stamped_drape_rid_;
	static int live_instances_;
	PackedVector4Array last_silhouette_terms_;
	PackedVector4Array last_clip_u_;
	PackedVector4Array last_clip_v_;
	PackedVector4Array last_light_pos_;
	PackedVector4Array last_light_diffuse_;
	struct SlotParamStamp {
		bool valid = false;
		Projection mat;
		uint64_t tex_id = 0;
	};
	std::array<SlotParamStamp, opennova::renderer::kSlotCaptureCount>
			drape_mat_stamps_{};
	// The per-slot dominant-light query buffer (reused across frames).
	std::vector<opennova::renderer::SlotPointLight> slot_lights_;
	// One drape patch mesh per capture order (retail's per-slot patch vertex
	// range of the shared static buffer, RenderSlot_RebuildPatchVertexBuffer
	// @0x5d5130): the mesh and its scenario instance, the origin/lod the
	// vertices were built for (the rebuild gate), and those vertices.
	struct PatchMesh {
		RID mesh;
		RID instance;
		bool built = false;
		float origin_x = 0.0f;
		float origin_north = 0.0f;
		int lod = 0;
		bool visible = false;
		std::vector<std::array<float, 3>> vertices;
	};
	std::array<PatchMesh, opennova::renderer::kSlotCaptureCount> patch_meshes_{};
	std::vector<uint16_t> patch_indices_scratch_;
	void _draw_patch(int p_order, const opennova::renderer::SlotPatch &p_patch, int p_lod);
	void _hide_patches_from(uint32_t p_drawn_mask);
	void _release_patches();
	ObjectID environment_node_id_;
	Ref<TerrainData> terrain_data_;
	Ref<EffectLightDirector> light_director_;
	Vector3 light_gain_ = Vector3(1, 1, 1);
	int light_time_ms_ = 0;
	ObjectID weather_id_;
	ObjectID local_player_id_;
	ObjectID local_player_parent_id_;
	bool local_first_person_ = true;
	bool local_prone_ = false;
	bool terrain_pass_drawn_ = true;
	// Retail's highest selectable SHADOWQUALITY is 3 (Settings_ClampGraphicsOptions
	// @0x54d546, copied into RenderSlot_DetailLevel @0x5d6159); 4 is the
	// unreachable 1024-base oversample tier.
	int shadow_detail_ = 3;
	uint32_t frame_ = 0;
	int report_captures_ = 0;
	int report_bound_ = 0;
	bool shutdown_ = false;
	// Latched so a lazily (re)instantiated effect re-applies the F3 timing flag.
	bool gpu_timing_enabled_ = false;

	static Ref<ShaderMaterial> drape_material_;
	// The depth-clip stage's 32x4 "shadowztex" (opennova::renderer::shadowztex_pixels),
	// bound once on the shared drape material.
	static Ref<ImageTexture> shadowztex_;
	// The twelve capture textures bound once to the drape's u_slot_tex_i; the
	// live SlotShadow points them at its device targets.
	static Ref<Texture2DRD> capture_textures_[opennova::renderer::kSlotCaptureCount];
};

}  // namespace godot
