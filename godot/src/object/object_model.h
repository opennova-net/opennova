#pragma once

#include <runtime/anim/remote_body_state.h>
#include <runtime/renderer/model_controls.h>

// ObjectModel — the retained visual for one NovaLogic object graphic,
// NATIVE (the 2026-08-09 de-scripting of the former GDScript implementation).
// One Node3D owns the
// retained scene (Robj part nodes / Skeleton3D + Skin / materials), the
// main-body skeletal channels, the PLAYPARTANIM part channels, the CTRL
// register store, environment lighting application, and the event-driven
// runtime frame. Presenters drive it through direct typed calls — there is
// no script bridge, no virtual dispatch layer, and no capability probing.
//
// Behavioral witnesses live at each method ([orig:] blocks carried from the
// GDScript origin); docs/adr/0007 + docs/world/world-wac-ai-re.md §14 for
// the skeletal semantics, docs/render/render-lighting-re.md for lighting.

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/visible_on_screen_notifier3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include <array>
#include <runtime/renderer/object_lod.h>
#include <cstdint>
#include <vector>

#include "object/entity_ref.h"
#include "object/object_data.h"
#include "object/post_multiply_draw.h"
#include "object/skeletal_anim.h"

namespace godot {


class Terrain;
class MeshInstance3D;
class OccluderInstance3D;
class VisualInstance3D;

// The env-derived world lighting/fog values (ADR 0017's typed record,
// native). Computed once per env change; the object shader family reads the
// same block as the opennova_light_block_* / opennova_fog_* global shader
// parameters MissionEnvironment writes, and the typed record serves the
// light director, the terrain light rows, and the diagnostics.
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 ->
//  RenderBatchCtx_StoreLightingConstants @ 0x5d89e0]
class EnvLightValues : public RefCounted {
	GDCLASS(EnvLightValues, RefCounted)

protected:
	static void _bind_methods();

public:
	Vector3 hemi_sky;
	Vector3 dir;
	Vector3 dir_color;
	Vector3 hemi_ground;
	Vector3 ceiling;
	Vector3 floor_color;
	Vector3 gain;
	bool fog_enabled = false;
	bool thermal_view = false;
	Vector3 fog_color;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	int fog_type = 0;

	// True when `other` carries the same lighting/fog the shaders consume.
	// A null other (first push after rebuild) is never equal.
	bool equals(const Ref<EnvLightValues> &p_other) const;

	// The un-enved default: the RETAIL NOON register (shipped full_00.env tod
	// 1200 block bytes /255), so a preview lights like a JO noon world.
	static Ref<EnvLightValues> retail_noon_defaults();

	// Property surface so the environment system (GDScript) fills the record.
	void set_hemi_sky(const Vector3 &v) { hemi_sky = v; }
	Vector3 get_hemi_sky() const { return hemi_sky; }
	void set_dir(const Vector3 &v) { dir = v; }
	Vector3 get_dir() const { return dir; }
	void set_dir_color(const Vector3 &v) { dir_color = v; }
	Vector3 get_dir_color() const { return dir_color; }
	void set_hemi_ground(const Vector3 &v) { hemi_ground = v; }
	Vector3 get_hemi_ground() const { return hemi_ground; }
	void set_ceiling(const Vector3 &v) { ceiling = v; }
	Vector3 get_ceiling() const { return ceiling; }
	void set_floor_color(const Vector3 &v) { floor_color = v; }
	Vector3 get_floor_color() const { return floor_color; }
	void set_gain(const Vector3 &v) { gain = v; }
	Vector3 get_gain() const { return gain; }
	void set_fog_enabled(bool v) { fog_enabled = v; }
	bool get_fog_enabled() const { return fog_enabled; }
	void set_fog_color(const Vector3 &v) { fog_color = v; }
	Vector3 get_fog_color() const { return fog_color; }
	void set_fog_start(float v) { fog_start = v; }
	float get_fog_start() const { return fog_start; }
	void set_fog_end(float v) { fog_end = v; }
	float get_fog_end() const { return fog_end; }
	void set_fog_type(int v) { fog_type = v; }
	int get_fog_type() const { return fog_type; }
};

// The typed channel between the environment system and its non-shader
// consumers: the env PUBLISHES its current world light/fog values here and
// consumers hold this ref, reading values + generation through typed calls —
// no consumer ever holds the environment object itself.
class EnvLightState : public RefCounted {
	GDCLASS(EnvLightState, RefCounted)

	Ref<EnvLightValues> values_;
	int64_t generation_ = 0;

protected:
	static void _bind_methods();

public:
	void publish(const Ref<EnvLightValues> &p_values,
			bool p_pass_changed = false);
	Ref<EnvLightValues> get_values() const { return values_; }
	int64_t get_generation() const { return generation_; }
};

// Retail samples GetTickCount once into one global DWORD for a rendered
// frame; models, material animation, and Generic collision consume that same
// value. (The former panm_clock.gd, native.)
class PanmClock : public RefCounted {
	GDCLASS(PanmClock, RefCounted)

	int64_t time_ms_ = 0;
	int64_t sampled_frame_ = -1;

protected:
	static void _bind_methods();

public:
	bool sample(int64_t p_value_ms, int64_t p_frame);
	int64_t get_time_ms() const { return time_ms_; }
	void set_time_ms_for_test(int64_t p_value_ms);
};

class ObjectModel : public Node3D {
	GDCLASS(ObjectModel, Node3D)

public:
	// The nine aim-overlay classes (mirrors anim::kOverlayClassCount; pinned
	// by static_assert in object_model_anim.cpp).
	static constexpr int kAimOverlayClasses = 9;
	// The witnessed lighting uniform surface defaults — the RETAIL NOON
	// register (shipped full_00.env tod 1200 bytes /255), so an un-enved
	// preview lights like a JO noon world. Must stay equal to the checked-in
	// shader defaults (res://shaders/object/shared.gdshaderinc).
	static Vector3 default_hemi_sky_color() { return Vector3(84.0f / 255.0f, 88.0f / 255.0f, 89.0f / 255.0f); }
	static Vector3 default_dir_light_dir() { return Vector3(-0.4082f, -0.8165f, -0.4082f); }
	static Vector3 default_dir_light_color() { return Vector3(170.0f / 255.0f, 170.0f / 255.0f, 167.0f / 255.0f); }
	static Vector3 default_hemi_ground_color() { return Vector3(49.0f / 255.0f, 55.0f / 255.0f, 46.0f / 255.0f); }


	// Visual-layer bits, mirrored from the renderer's one layer allocation
	// (render/visual_layers.h owns the scheme; keep the two in lockstep).
	enum {
		LAYER_WORLD = 1 << 0,
		LAYER_VIEWMODEL = 1 << 11,
		LAYER_FP_BODY_SHADOW_ONLY = 1 << 12,
		LAYER_STATIC_SHADOW_CASTER = 1 << 13,
		LAYER_DYNAMIC_SHADOW_CASTER = 1 << 14,
		LAYER_WORLD_NO_MIRROR = 1 << 16,
		LAYER_SHADOW_CASTER_MASK =
				LAYER_STATIC_SHADOW_CASTER | LAYER_DYNAMIC_SHADOW_CASTER,
	};

	// Which camera population this model's surface instances belong to. Retail
	// decides "drawn by this camera" with one branch per submit (the FP body is
	// a suppressed submit, the FP gun a viewmodel-first draw); Godot keeps that
	// decision as per-instance layer/cast state, so the decision is STORED here
	// and written on its edges + inside rebuild_scene, never per frame
	// [orig: BoneCallback_org0_World @0x4e3940 the body submit gate;
	// Player_RenderFirstPersonViewModel @0x4ded60, see
	// docs/world/world-wac-ai-re.md section 13.1].
	// Which part of a composed player avatar this model is, when it is one:
	// the placer's body/head submits (the head follows the body's every
	// presentation call) and the FP rig's arms (which carry the per-submit
	// camo triplet). Every other model is AVATAR_PART_NONE.
	enum AvatarPart {
		AVATAR_PART_NONE = 0,
		AVATAR_PART_BODY = 1,
		AVATAR_PART_HEAD = 2,
		AVATAR_PART_ARMS = 3,
	};
	enum PresentationLayer {
		// The ordinary entity: the mirror-policy world layer plus this model's
		// shadow-caster markers; casts when it carries a marker.
		PRESENTATION_LAYER_WORLD = 0,
		// The local player's body/held gun as a drawn entity (third person,
		// the debug body): the world layer, markers kept, always casting so
		// the render-slot capture cameras photograph it.
		PRESENTATION_LAYER_LOCAL_BODY = 1,
		// The same models while first person hides them from every camera by
		// LAYER (the hidden FP layer), still casting for the slot capture.
		PRESENTATION_LAYER_LOCAL_BODY_HIDDEN = 2,
		// The FP arms/gun: the viewmodel layer alone (every caster marker
		// stripped so the gun never leaks into world shadows), never casting.
		PRESENTATION_LAYER_VIEWMODEL = 3,
	};

	// Fixed layout returned by profile_awake_frame(delta). Keeping this a
	// packed numeric record lets the F3 feed cross the script boundary once per
	// frame without allocating Dictionaries or Strings on the render hot path.
	enum AwakeFrameProfileSlot {
		AWAKE_PROFILE_CLOCK_ANIMATION_US = 0,
		AWAKE_PROFILE_PANM_US,
		AWAKE_PROFILE_MATERIAL_US,
		AWAKE_PROFILE_ORDER_BOUNDS_US,
		AWAKE_PROFILE_AWAKE_MODELS,
		AWAKE_PROFILE_RENDERABLE_MODELS,
		AWAKE_PROFILE_SLOT_COUNT,
	};

private:
	struct AwakeFrameProfile {
		int64_t clock_animation_us = 0;
		int64_t panm_us = 0;
		int64_t material_us = 0;
		int64_t order_bounds_us = 0;
		int64_t awake_models = 0;
		int64_t renderable_models = 0;
	};
	static void advance_awake_frame_impl(double p_delta,
			AwakeFrameProfile *p_profile);
	struct AlphaStripDraw {
		MeshInstance3D *instance = nullptr;
		Ref<ShaderMaterial> material;
		Vector3 local_center;
		bool bone_path = false;
		// The rung last pushed to the material; Godot re-sorts on every
		// render_priority write, so equal rungs are never re-pushed.
		int32_t rung = INT32_MIN;
	};

	// One authored submesh of one RLOD level: the mesh/material pair (an
	// alpha strip owns its priority-bearing duplicate, an opaque strip shares
	// the per-material cache entry) and the postmultiply auxiliary material,
	// built once per rebuild and swapped onto the retained surface slot when
	// the level becomes active. Nothing here is a node.
	struct LevelSurface {
		Ref<ArrayMesh> mesh;
		Ref<ShaderMaterial> material;
		Ref<ShaderMaterial> auxiliary_material;
		int robj_index = 0;
		int material_index = 0;
		Vector3 local_center;
		bool is_alpha = false;
		bool is_skinned = false;
		// The level's collector admits a Q3 copy (never a per-vertex skinned
		// level: renderer::q3_object_source_admitted).
		bool q3_admitted = false;
	};
	// One retained surface instance: slot k draws submesh k of the active
	// level (mesh, material, part/skeleton parent and skin binding swapped
	// in place on a level switch), hidden while the active level has fewer
	// submeshes. The auxiliary postmultiply instance is created the first
	// time a level's submesh k carries the pair and hidden otherwise.
	struct SurfaceSlot {
		MeshInstance3D *instance = nullptr;
		PostMultiplyDraw *auxiliary = nullptr;
	};
	// A visual another owner parented under this model and bound to one
	// authored level (the placer's static shadow siblings): shown only while
	// that level is active, exactly like the model's own surfaces.
	struct LevelBoundVisual {
		ObjectID id;
		int lod_index = 0;
	};

	Ref<ObjectData> object_data_;
	HashMap<int64_t, Ref<ShaderMaterial>> material_cache_;
	// The postmultiply proxy paired with a cached material (retail's
	// multi-pass effects: one logical material, the strip submitted again),
	// keyed like material_cache_; absent for materials without the pass.
	HashMap<int64_t, Ref<ShaderMaterial>> postmultiply_cache_;
	Vector<AlphaStripDraw> alpha_strip_draws_;
	// level_surfaces_[lod] = that level's submeshes in authored order (empty
	// for a level the build did not retain); surface_slots_ holds one
	// instance per slot, sized to the largest retained level.
	std::vector<std::vector<LevelSurface>> level_surfaces_;
	std::vector<SurfaceSlot> surface_slots_;
	std::vector<LevelBoundVisual> level_bound_visuals_;
	// The level the slots currently carry (-1 = none applied since the build).
	int applied_lod_ = -1;
	bool skeletal_scene_ = false;
	HashMap<int, Node3D *> robj_nodes_;
	HashMap<int, Transform3D> robj_rest_transforms_;
	// Last applied point-light selections (FNV over count + packed vectors).
	// Per-render-object selection hashes. Retail re-scopes a building's owner
	// group for every ROBJ draw; a single model-wide hash cannot represent that
	// state and also incorrectly survives a retained-scene rebuild.
	HashMap<int32_t, uint64_t> point_light_selection_hashes_;
	// Dense part-index -> Node3D array + the PANM revision this model last
	// applied (stays a Godot Array: ObjectData::apply_panm_to_nodes takes
	// it directly).
	Array robj_dense_;
	bool viewmodel_pass_ = false;
	uint32_t viewmodel_pass_stamped_serial_ = 0;
	int64_t panm_applied_revision_ = 0;
	int64_t section_visibility_mask_ = -1;
	HashMap<int, OccluderInstance3D *> authored_occluders_;
	PackedInt32Array surface_material_indices_;
	Vector<Ref<ShaderMaterial>> surface_materials_;
	HashMap<int64_t, Array> anim_frames_by_mat_;
	// Native CTRL values, writer ownership and local part-channel playback.
	opennova::renderer::ModelControls controls_;
	opennova::renderer::ControlRegisterValues runtime_ctrl_values();
	// Optional visual parts (a player body's selected head) driven by this
	// model's presentation calls: every animation/body/part call and every CTRL
	// register store is forwarded, EXCEPT the registers the composer declared
	// part-local for that link (retail rewrites the head/body/arms TEX_CAMO
	// triplet on the shared CTRL bus immediately before each part's own
	// submit, so a retained composition keeps those per part). ObjectIDs make
	// teardown safe when a child is queued.
	struct PresentationLink {
		ObjectID id;
		HashSet<String> part_local_registers; // canonical register names
	};
	Vector<PresentationLink> presentation_links_;
	int ctrl_batch_depth_ = 0;
	bool ctrl_batch_dirty_ = false;
	int64_t anim_time_ms_ = 0;
	Ref<PanmClock> panm_clock_;
	int active_lod_ = 0;
	bool authored_lod_enabled_ = false;
    bool exact_owner_lod_ = false;
    bool geometry_visible_ = true;
	bool focal_sway_active_ = false;
	Basis focal_sway_basis_;
	Vector3 focal_sway_offset_;
	void apply_focal_sway();
	bool rigid_parts_ = false;
	ObjectID authored_lod_owner_;
	ObjectID authored_lod_projection_owner_;
	opennova::renderer::ObjectProjectionSphere entity_projection_sphere_;
	bool entity_projection_person_ = false;
	bool entity_projection_override_ = false;
	int32_t entity_projection_scale_q16_ = 0;
	bool parachute_deployed_ = false;
	int32_t parachute_projection_radius_q16_ = 0;
	void refresh_entity_projection_sphere();
	uint64_t lod_projection_frame_ = 0;
	int32_t lod_projected_radius_q16_ = 0;
	bool lod_projection_visible_ = false;
	bool authored_occluders_enabled_ = false;
	std::vector<int32_t> authored_lod_thresholds_q16_;
	std::vector<bool> authored_lod_available_;
	bool is_playing_ = true;
	AABB model_bounds_;
	float lighting_effect_scale_ = 1.0f;
	bool thermal_entity_wave_ = false;
	bool interior_lerp_ = false;
	float interior_daylight_ = 0.0f;
	bool interior_section_lighting_ = false;
	float interior_section_daylight_ = 0.0f;
	uint32_t shadow_caster_layers_ = 0;
	bool slot_shadow_person_ = false;
	// Effective entity/model scale in signed Q16.16. Zero is retail's sentinel
	// for an ordinary 1.0 matrix; kept on the model so every present writer
	// composes the same scale instead of overwriting it with a pose transform.
	int32_t entity_uniform_scale_q16_ = 0;
	float model_sphere_radius_ = 0.0f;  // gpm[5]; 0 = unstamped
	int32_t entity_bound_radius_q16_ = 0;  // entity+0; 0 = none (no collision block)
	// The eweap-powerup projection form: the entity init stores a zero bbox
	// center before measuring the sphere (world::item_def_zero_bbox_center).
	bool entity_projection_zero_center_ = false;
	ObjectID slot_shadow_capture_with_;
	ObjectID entity_light_owner_;
	String slot_shadow_decal_texture_;
	Vector4 slot_shadow_decal_dims_;
	bool mirror_reflected_ = false;
	AvatarPart avatar_part_ = AVATAR_PART_NONE;
	int character_id_ = 0; // the composed avatar's character id (0xffff-masked)
	Vector3i avatar_camo_; // the arms' raw camo triplet for the per-submit FP writer
	String graphic_name_; // the object graphic the placer built this model from
	Ref<EntityRef> entity_ref_;
	PresentationLayer presentation_layer_ = PRESENTATION_LAYER_WORLD;
	bool on_screen_ = true;
	// Two-bit visibility ownership (ADR 0043 d9): the entity presenter's
	// placed walk owns the sim's intent, the render-occlusion frame owns its
	// claim, and Node3D::visible is their product (each setter recomputes
	// it, so the visibility-changed notification fires on every edge).
	bool present_visible_ = true;
	bool occlusion_hidden_ = false;
	VisibleOnScreenNotifier3D *screen_notifier_ = nullptr;
	bool match_terrain_enabled_ = false;
	// The last MATCHTERRAIN page state the terrain-frame leg stamped
	// (refresh_match_terrain_frame), kept for instances minted between legs.
	bool match_terrain_page_ready_ = false;
	float match_terrain_page_layer_ = 0.0f;
	Vector4 match_terrain_page_projection_;
	bool awake_ = false; // in the shared awake set below

	// The one runtime-frame set: every model holding live per-frame work (PANM,
	// dynamic materials, part/body anim, an env restamp due). The single frame
	// driver — GameWorld's render_material_frame leg, the menu
	// process loop — advances this set once per render frame; models add
	// themselves on wake and drop out on park. Replaces the per-node _process
	// clock so nothing self-clocks outside that one driver.
	static HashSet<ObjectModel *> awake_models_;
	// Monotonic invalidation stamp for native row plans that retain typed model
	// pointers. Godot destroys Nodes on the main thread; PREDELETE advances this
	// before any cached pointer can be observed by a later presentation walk.
	static uint64_t lifetime_generation_;
	// Every built model that owns at least one blended strip: the water-plane
	// owner marks them all dirty when the ladder's inputs change.
	static HashSet<ObjectModel *> alpha_strip_models_;
	// Strip classification runs only when something the ladder reads moved:
	// the model transform, a part/robj transform, a rebuild, or the water
	// plane generation (retail recomputes every strip every frame because its
	// batch walk already visits them; the result is identical).
	bool render_order_dirty_ = true;
	uint64_t render_order_generation_ = 0;
	// Models currently submitted through retail's crouch/prone MATCHTERRAIN
	// leg. GameWorld refreshes their resident terrain-page binding after the
	// terrain cache has processed this frame's requests.
	static HashSet<ObjectModel *> match_terrain_models_;
	static HashSet<ObjectModel *> authored_lod_models_;
	// Every GeometryInstance3D the scene builds carries instance uniforms
	// (u_entity_light, the stance and viewmodel flags), so each one holds 16
	// vec4 slots of Godot's global shader buffer for as long as it exists,
	// visible or not (one per surface slot plus the auxiliary pairs, never
	// one per retained RLOD). The process-wide sum is
	// the shell's estimate of that allocation (Godot does not expose it):
	// GameWorld.get_runtime_perf_counters reads it for the F3/perf path.
	int geometry_instance_count_ = 0;
	static int64_t live_geometry_instance_count_;
	void retire_geometry_instances();

	// Main-body skeletal animation (.bad/.adm via SkeletalAnim).
	Ref<SkeletalAnim> skeletal_;
	Skeleton3D *skeleton_ = nullptr;
	Ref<Skin> skeleton_skin_;
	int muzzle_bone_ = -1;
	// The def-AUTHORED launch userpoint name (items.def launchups_closeattack,
	// pushed by the placer); empty = this model has no AI muzzle.
	String muzzle_point_name_;
	String anim_key_;
	int anim_variant_ = 0;
	double anim_time_ = 0.0;
	bool anim_playing_ = false;
	bool anim_external_phase_ = false;
	// Native arbitration/playback numbers; Godot retains only clip resources.
	opennova::anim::RemoteBodyState remote_body_;
	String remote_pending_key_;
	String remote_blend_source_key_;
	bool body_pose_dirty_ = true;
	bool bounds_dirty_ = true;
	bool body_phase_stamp_valid_ = false;
	int body_phase_ticks_applied_ = 0;
	String body_blend_source_key_;
	double body_blend_source_time_ = 0.0;
	float body_blend_weight_ = 1.0f;
	int last_slot_resolved_ = -1;
	String last_slot_key_;

	// Third-person aim overlay + the upper-body weapon channel. The nine
	// per-class deltas live in a fixed array (no Variant container on the
	// present hot path); valid_ = an overlay is applied.
	std::array<Basis, kAimOverlayClasses> aim_overlay_deltas_{};
	bool aim_overlay_valid_ = false;
	PackedInt32Array aim_overlay_classes_;
	bool collapse_right_hand_ = false;
	String wpn_key_;
	int wpn_phase_ticks_ = 0;
	// The secondary channel's outgoing clip + cross-fade weight (the sim's
	// wpn_prev / wpn_prev_clip_phase / wpn_blend_weight). Empty prev key = the
	// channel is not blending [orig: the AnimMap_UpdateEntity @0x40b5f0
	// re-init, see docs/world/world-wac-ai-re.md §14.8.7].
	String wpn_prev_key_;
	int wpn_prev_phase_ticks_ = 0;
	float wpn_blend_weight_ = 1.0f;
	// The served variant-ring entries for the target and outgoing weapon clips
	// (the sim's wpn_variant / wpn_prev_variant — the +68 play latch).
	int wpn_variant_ = 0;
	int wpn_prev_variant_ = 0;

	// Per-frame work skips.
	bool has_live_panm_ = false;
	Vector<bool> material_needs_eval_;
	PackedInt32Array dynamic_material_slots_;
	struct MaterialRuntimeStamp {
		bool runtime_valid = false;
		opennova::renderer::MaterialRuntime runtime;
		int anim_frame = -1;
	};
	std::vector<MaterialRuntimeStamp> material_runtime_stamps_;
	// Set by an EntityPresenter row plan that retains this model by pointer and
	// cleared when that plan drops the row; only planned models advance
	// lifetime_generation_ when they die.
	bool present_planned_ = false;

	// --- core (object_model.cpp) ---
	void set_shadow_caster_layer_enabled(uint32_t p_layer, bool p_enabled);
	// The stored layer/cast decision for one surface instance (auxiliary
	// postmultiply draws never cast under the world policy) and the walk that
	// re-applies it to every instance below `p_root`, preserving the
	// render-slot capture bits SlotShadow stamps beside it.
	uint32_t presentation_layer_mask(bool p_auxiliary) const;
	GeometryInstance3D::ShadowCastingSetting presentation_cast_setting(
			bool p_auxiliary) const;
	void apply_presentation_layer_below(Node *p_root);
	// The per-entry lighting factors (effectScale, interior flag, daylight t)
	// as instance state on every surface instance.
	void stamp_entity_lighting_instances();
	void finish_ctrl_change(bool p_apply_now);
	Node3D *get_or_create_robj_node(int p_robj_index);
	void apply_runtime_state(double p_delta, bool p_renderable = true,
			AwakeFrameProfile *p_profile = nullptr);
	void advance_runtime_frame_profiled(double p_delta,
			AwakeFrameProfile *p_profile);
	bool apply_robj_transforms();
	void wake_runtime_frame();
	void sleep_runtime_frame_if_idle();
	bool needs_runtime_frame_work() const;
	void refresh_live_panm_classification();
	int clamp_lod_index(int p_lod_index) const;
	// Swap the active level's submeshes onto the retained surface slots:
	// mesh, material, part/skeleton parent, skin binding, the Q3 source
	// registration, the auxiliary pair, the alpha-strip ladder rows, the
	// dynamic-material slot tables, the level-bound visuals and the
	// per-instance lighting stamps. Never creates or frees a slot instance.
	void apply_level_surfaces();
	// The Node3D a level surface hangs under: the shared Skeleton3D for a
	// skinned strip, else its ROBJ part node.
	Node3D *surface_parent_for(const LevelSurface &p_surface);
	void refresh_active_lod_rest_transforms();
	void stamp_match_terrain_instances(bool p_page_ready, float p_layer,
			const Vector4 &p_projection);
	// The per-instance uniforms every retained instance carries (the
	// MATCHTERRAIN page binding, the viewmodel pass flag and cull margin),
	// written from the model's retained state onto one instance: the
	// terrain-frame and viewmodel legs stamp the built set, this stamps an
	// instance minted later (a first-seen auxiliary of a level switch) so it
	// never draws with default uniforms until the next leg.
	void stamp_instance_uniforms(GeometryInstance3D *p_instance) const;
	void set_model_bounds(const AABB &p_bounds);
	static bool aabb_equal_approx(const AABB &p_a, const AABB &p_b);
	Vector<ObjectModel *> live_presentation_links() const;
	Vector<ObjectModel *> live_presentation_links_sharing(
			const String &p_register) const;

	// --- body/part animation (object_model_anim.cpp) ---
	void resolve_muzzle_userpoint();
	bool select_body_clip_seeded(const String &p_key, int p_phase_ticks);
	void accept_remote_body_clip(const String &p_key,
			int p_flags, int p_phase_ticks, bool p_had_current);
	void queue_remote_body_clip(const String &p_key);
	void clear_remote_body_blend();
	int body_phase_ticks(const String &p_key, double p_seconds) const;
	void start_remote_body_blend(const String &p_target_key, int p_target_flags,
			int p_target_phase_ticks);
	bool promote_remote_body_pending_if_due();
	void pose_body_blend_at_times(const String &p_source_key, double p_source_time,
			const String &p_target_key, double p_target_time, float p_weight);
	void set_body_playhead(double p_seconds);
	String resolve_body_clip_key(const String &p_key) const;
	double clip_phase_seconds(const String &p_key, int p_phase_ticks) const;
	void clear_body_blend();
	void reset_body_pose();
	String resolve_anim_channel_register(int p_slot) const;
	String resolve_anim_channel_owner(int p_slot) const;
	bool advance_part_anims(double p_delta);

	// --- materials/environment (object_model_materials.cpp) ---
	Ref<ShaderMaterial> material_for_index(int p_material_array_index);
	Ref<ShaderMaterial> postmultiply_material_for_index(int p_material_array_index) const;
	// Builds the surface material for the MTRL row at `p_array_index` (-1 = no
	// row: the FF_ST_OP defaults).
	Ref<ShaderMaterial> create_material(int p_array_index,
			Ref<ShaderMaterial> &r_postmultiply);
	void collect_anim_frames(int p_material_index);
	Ref<Texture2D> load_texture_name(const String &p_texture_name);
	static Ref<ImageTexture> solid_colour_texture(const Color &p_color);
	// One shader parameter written to a material and, when the material
	// carries the postmultiply pass, to its proxy as well.
	static void set_material_and_auxiliary_parameter(
			const Ref<ShaderMaterial> &p_material,
			const Ref<ShaderMaterial> &p_auxiliary, const StringName &p_name,
			const Variant &p_value);
	bool material_runtime_is_dynamic(int p_material_index) const;
	void classify_materials();

	// --- retained-scene construction (object_model_scene.cpp) ---
	void rebuild_scene();
	void build_skeleton();
	// Bumped by every rebuild_scene(): a device that stamps this subtree's
	// instances (SlotShadow's capture layers) re-stamps when it moves.
	uint32_t scene_build_serial_ = 0;

public:
	uint32_t get_scene_build_serial() const { return scene_build_serial_; }

private:
	void sync_screen_notifier(const AABB &p_bounds);
	AABB compute_transformed_mesh_bounds() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	ObjectModel();
	~ObjectModel();

	// The one runtime-frame clock: every context's single driver advances the
	// AWAKE set once per render frame — the game from GameWorld's
	// render_material_frame leg, the menu shell from their one process
	// loop. Models self-park out of the set the first frame they hold no live
	// work; there is no per-node _process.
	static void advance_awake_frame(double p_delta);
	static PackedInt64Array profile_awake_frame(double p_delta);
	static uint64_t lifetime_generation() { return lifetime_generation_; }
	void set_present_planned(bool p_planned) { present_planned_ = p_planned; }
	// True while this model is in the shared awake set (the park/re-arm gate's
	// observable — replaces the ex-per-node is_processing() the tests read).
	bool is_runtime_frame_awake() const { return awake_; }

	// --- data / configuration ---
	void set_object_data(const Ref<ObjectData> &p_data);
	Ref<ObjectData> get_object_data() const { return object_data_; }
	// Compose `p_model` as a linked visual part; `p_part_local_registers` names
	// the CTRL registers that stay per part (not forwarded).
	void add_presentation_link(ObjectModel *p_model,
			const PackedStringArray &p_part_local_registers = PackedStringArray());
	void set_mirror_reflected(bool p_reflected) { mirror_reflected_ = p_reflected; }
	bool get_mirror_reflected() const { return mirror_reflected_; }
	void set_avatar_part(AvatarPart p_part) { avatar_part_ = p_part; }
	AvatarPart get_avatar_part() const { return avatar_part_; }
	void set_character_id(int p_id) { character_id_ = p_id; }
	int get_character_id() const { return character_id_; }
	void set_avatar_camo(const Vector3i &p_camo) { avatar_camo_ = p_camo; }
	Vector3i get_avatar_camo() const { return avatar_camo_; }
	void set_graphic_name(const String &p_name) { graphic_name_ = p_name; }
	String get_graphic_name() const { return graphic_name_; }
	// The entity identity this model presents; null for models that are no
	// placed or wire-spawned entity (viewmodels, husk grafts, helpers).
	void set_entity_ref(const Ref<EntityRef> &p_ref) { entity_ref_ = p_ref; }
	void set_thermal_entity_wave(bool p_enabled);
	bool get_thermal_entity_wave() const { return thermal_entity_wave_; }
	Ref<EntityRef> get_entity_ref() const { return entity_ref_; }
	void set_presentation_layer(PresentationLayer p_layer);
	void set_shadow_caster_enabled(bool p_enabled);
	bool is_shadow_caster_enabled() const;
	void set_static_shadow_caster_enabled(bool p_enabled);
	bool is_static_shadow_caster_enabled() const;
	// Render-slot ground-shadow profile (SlotShadow consumes): person-type
	// casters are the depth-clip stage's steepened class (that stage owns the
	// 4x, not the drape — render_slot_shadow.h); vehicles may author an
	// items.def `shadow` blob decal fallback [orig: itemdef type 3 / the
	// +0xA0 decal, see docs/render/render-lighting-re.md]. dims = (w, l, ox, oy).
	void set_slot_shadow_person(bool p_person);
	bool is_slot_shadow_person() const;
	void set_entity_uniform_scale_q16(int64_t p_scale_q16);
	int64_t get_entity_uniform_scale_q16() const;
	// Compose a renderer-owned entity pose with the effective authored scale.
	// All native and GDScript presentation owners use this one operation.
	Transform3D compose_entity_transform(const Basis &p_basis,
			const Vector3 &p_origin) const;
	// The two radii retail's shadow slot reads, world units, stamped by the
	// placer from the .3di: the MODEL SPHERE (the header's origin sphere,
	// gpm[5] — world model_bound_radius_from_3di) sizes the silhouette
	// capture extent and the depth clip; the ENTITY BOUND (entity+0: that
	// sphere raised to the husk model's, + the 0x1000 pad, written only for a
	// model with a collision block) sizes the slot lod/patch and the light
	// query [orig: Entity_InitFromModel @0x40dc30; RenderSlot_AllocSlot
	// @0x5d5773 reads entity+0; RenderSlot_RenderEntityAndChildren
	// @0x5d7835 reads gpm[5]; see docs/render/render-lighting-re.md]. A model
	// sphere of 0 = unstamped (SlotShadow falls back to the render bounds).
	void set_shadow_bound_radii(float p_model_sphere, float p_entity_bound);
	void set_bound_radii_q16(int32_t p_model_sphere, int32_t p_entity_bound);
	int32_t get_entity_bound_radius_q16() const { return entity_bound_radius_q16_; }
	float get_model_sphere_radius() const;
	float get_entity_bound_radius() const;
	// Capture-with link: this model renders into ANOTHER caster's slot
	// (retail renders held weapons and mounted/standing children inside the
	// parent entity's slot RT — the RenderSlot_RenderEntityAndChildren
	// child walk); it never takes a slot of its own.
	// Attached render models share their entity lighting query and groups,
	// independently of their posed model origins (native EntityLightQuery).
	void set_entity_light_owner(ObjectModel *p_owner);
	ObjectModel *get_entity_light_owner() const;
	void set_slot_shadow_capture_with(ObjectModel *p_owner);
	ObjectModel *get_slot_shadow_capture_with() const;
	void set_slot_shadow_decal(const String &p_texture, const Vector4 &p_dims);
	String get_slot_shadow_decal_texture() const;
	Vector4 get_slot_shadow_decal_dims() const;
	void update_slot_shadow_group();
	void set_entity_lighting_context(float p_effect_scale, bool p_interior_lerp,
			float p_interior_daylight);
	// The stamped context, for a second model drawn as the SAME entity
	// submission (the local view's virtual display takes its carrier's).
	float get_lighting_effect_scale() const { return lighting_effect_scale_; }
	bool is_interior_lerp() const { return interior_lerp_; }
	float get_interior_daylight() const { return interior_daylight_; }
	void set_interior_section_light_transfer(float p_daylight);
	AABB get_model_bounds() const { return model_bounds_; }
	// The rendered model bounds in world space (geometry diagnostics/culling).
	// Lighting uses the entity origin and get_entity_bound_radius_q16 instead.
	AABB get_world_bounds() const;
	struct PointLightDrawPart {
		int32_t robj_index = 0;
		AABB world_bounds;
	};
	// Visible rigid ROBJ draws and their exact world bounds. The EffectWorld
	// device leg uses these only for a building's per-ROBJ owner-section scope
	// [orig: collect_render_objects_for_batch @0x5d8ff7, see
	// docs/render/render-lighting-re.md].
	void collect_point_light_draw_parts(
			std::vector<PointLightDrawPart> &r_parts) const;
	// The per-ROBJ world bounds are rebuilt only when a part/robj transform,
	// the section mask, a rebuild, or the model transform changed; the
	// EffectWorld device asks for them every frame per visible building.
	mutable std::vector<PointLightDrawPart> point_light_draw_parts_cache_;
	mutable Transform3D point_light_draw_parts_transform_;
	mutable bool point_light_draw_parts_dirty_ = true;
	// Write one frame's selected point lights (packed posr = xyz world +
	// atten2, color = premultiplied rgb + range) as per-instance shader
	// parameters on every surface instance. A selection hash gates redundant
	// RenderingServer writes; count 0 clears.
	void apply_point_light_selection(int p_count, const Vector4 *p_posr,
			const Vector4 *p_color);
	void apply_point_light_selection_to_robj(int p_robj_index, int p_count,
			const Vector4 *p_posr, const Vector4 *p_color);
	// One authored LGHT record's live world position. Record offsets are model
	// space; a nonzero attach subobject follows the same rest-to-live transform
	// as user points [orig: Entity_SpawnGlowEffects @0x56c836 plus the
	// per-frame attachment mover, see docs/render/render-lighting-re.md].
	Vector3 get_model_light_world_position(int p_index) const;
	// Retail submits MATCHTERRAIN only for a skinned entity whose MoveOrder
	// stance bits are crouch/prone. The live presentation row owns that gate.
	void set_match_terrain_enabled(bool p_enabled);
	// The first-person viewmodel drawn inside the beauty pass: every mesh
	// instance takes the shader-side renderfov projection + depth band
	// (u_viewmodel_pass, shaders/viewmodel_pass.gdshaderinc), a cull
	// margin that keeps the eye inside its AABB (a narrowed ADS beauty frustum
	// must not cull gun parts the wider renderfov shows), and its alpha strips
	// the viewmodel rung. Re-stamps after a scene rebuild; idempotent per frame.
	void set_viewmodel_pass(bool p_enabled);
	static void refresh_match_terrain_frame(Terrain *p_terrain);
	static int update_authored_lods(const Transform3D &p_camera_transform,
			float p_vertical_fov_degrees,
			float p_viewport_width,
			float p_viewport_height);
	Dictionary get_render_part_nodes() const;
	// A model-space attachment through the rendered subobject's live pose.
	// Skeletal bones need their inverse rest pose; rigid PANM parts already
	// map model space directly. Missing parts use the model root.
	Transform3D subobject_model_to_world(int p_subobject) const;
	void set_focal_sway(bool active, const Basis &basis, const Vector3 &world_offset);
	void set_section_visibility_mask(int64_t p_mask);
	// The occlusion pass's last-applied mask (-1 = no verdict yet, all
	// sections visible). Read by the corona owner-section gate.
	int64_t get_section_visibility_mask() const {
		return section_visibility_mask_;
	}
	PackedInt32Array get_surface_material_indices() const { return surface_material_indices_; }
	Array get_surface_materials() const;
	bool is_playing() const { return is_playing_; }
	void set_playing(bool p_value);
	void set_panm_clock(const Ref<PanmClock> &p_clock);
	void set_active_lod(int p_lod_index);
	int get_active_lod() const { return active_lod_; }
	void set_authored_lod_enabled(bool p_enabled);
	// Attachment RLOD: an attached model (the third-person held weapon, the
	// NVG/binocular items, a mounted child) never runs its own threshold
	// walk; it draws at its owner's selected level clamped to its own LOD
	// count (renderer::attachment_lod_index). update_authored_lods applies
	// the owner's level after the frame's selections; a freed owner reads as
	// level 0.
	void set_authored_lod_owner(ObjectModel *p_owner, bool p_exact = false);
    void set_geometry_visible(bool p_visible);
    void set_rigid_parts(bool p_rigid);
	ObjectModel *get_authored_lod_owner() const;
	// A composed avatar shares its entity projection while each part retains
	// its own threshold table. This differs from an attachment's level owner.
	void set_authored_lod_projection_owner(ObjectModel *p_owner);
	ObjectModel *get_authored_lod_projection_owner() const;
	void configure_entity_projection(bool p_person, int32_t p_parachute_radius_q16,
			bool p_zero_center);
	void set_parachute_deployed(bool p_deployed);
	// A carved static becomes a live husk visual while retaining the primary
	// entity's already-derived local sphere and the scale of its pose matrix.
	void set_entity_projection_override(
			const opennova::renderer::ObjectProjectionSphere &p_sphere,
			int32_t p_entity_scale_q16);
	// The retained surface slots (one MeshInstance3D each, sized to the
	// largest retained level) and the submesh count of one level (0 for a
	// level the build did not retain): the typed read-back the tests pin the
	// one-instance-per-slot contract against.
	int get_surface_slot_count() const {
		return static_cast<int>(surface_slots_.size());
	}
	int get_level_surface_count(int p_lod_index) const;
	// Every surface instance this scene retains (the slots plus their
	// auxiliary postmultiply pairs): the model's term of the shell's
	// instance-uniform estimate.
	int get_retained_surface_instance_count() const {
		return geometry_instance_count_;
	}
	// One harvested row of a level's submeshes for the placer's static
	// template: the shared mesh and the model's own material (an auxiliary
	// postmultiply pair is its own row), the submesh's model-local transform
	// at that level, and the facts the population emitter needs.
	struct HarvestedSurface {
		Ref<Mesh> mesh;
		Ref<Material> material;
		Transform3D offset;
		int robj_index = 0;
		int lod_index = 0;
		bool auxiliary_draw = false;
		bool blended_draw = false;
	};
	// Every retained level's submeshes in level order, each level posed in
	// turn (its ROBJ part nodes carry that level's PANM base pose, so the
	// offsets are exactly what an individual model draws at the level); the
	// model is left at the level it had. Rows under the shared skeleton
	// (skinned strips) are never harvested.
	void harvest_level_surfaces(Vector<HarvestedSurface> &r_rows);
	// Bind a visual another owner parented under this model to one authored
	// level: it is shown only while that level is active. The binding lives
	// until the next rebuild (which frees every child).
	void add_level_bound_visual(int p_lod_index, VisualInstance3D *p_visual);
	void set_authored_occluders_enabled(bool p_enabled);
	// The retained OccluderInstance3D children the last build created (0 when
	// authored occluders are off or the model carries no eligible records).
	// The world decides from this whether to switch Godot's occlusion culling
	// on for its viewport; a model never flips viewport state itself.
	int get_authored_occluder_count() const {
		return static_cast<int>(authored_occluders_.size());
	}
	// The surface instances every live ObjectModel scene currently retains
	// (the slots and auxiliary draws): the ObjectModel term of the shell's
	// instance-uniform geometry estimate.
	static int64_t get_live_geometry_instance_count() {
		return live_geometry_instance_count_;
	}
	void rebuild();
	void refresh_render_order();
	static void mark_render_order_dirty_all();
	void advance_runtime_frame(double p_delta);
	void set_on_screen(bool p_value);
	bool is_on_screen() const { return on_screen_; }
	// The two visibility owners' bits: the sim's present intent (default
	// visible) and the render-occlusion frame's claim (default released).
	void set_present_visible(bool p_visible);
	bool is_present_visible() const { return present_visible_; }
	void set_occlusion_hidden(bool p_hidden);
	bool is_occlusion_hidden() const { return occlusion_hidden_; }

	// --- CTRL registers ---
	void begin_ctrl_update();
	void end_ctrl_update();
	void set_ctrl_value(const String &p_name, int64_t p_value);
	void clear_ctrl_value(const String &p_name);
	void set_ctrl_override(const String &p_owner, const String &p_name, int64_t p_value);
	void clear_ctrl_override(const String &p_owner, const String &p_name);
	// Release every register `p_owner` holds here (and on the linked parts that
	// share them): the cold/teardown release of a writer whose register set is
	// not enumerable up front (the ordinal DOOR_xx bus), at O(owned) cost.
	void clear_ctrl_overrides_owned(const String &p_owner);
	Dictionary get_ctrl_values() const;

	// --- main-body skeletal + part channels ---
	void set_skeletal_anim(const Ref<SkeletalAnim> &p_skeletal);
	Ref<SkeletalAnim> get_skeletal_anim() const { return skeletal_; }
	Skeleton3D *get_skeleton() const { return skeleton_; }
	bool has_skeleton() const { return skeleton_ != nullptr; }
	bool has_muzzle() const;
	// The def-authored launch userpoint name; rebuild resolves it against the
	// model's userpoint table (case-insensitive, retail's by-name lookup).
	void set_muzzle_point_name(const String &p_name);
	void play_body_clip(const String &p_key);
	void play_body_clip_variant(const String &p_key, int p_variant);
	void play_body_clip_variant_at_tick(const String &p_key, int p_variant, int p_ticks);
	void play_body_clip_variant_at_time(const String &p_key, int p_variant,
			double p_seconds);
	void play_body_clip_at(const String &p_key, int p_phase_ticks);
	void play_body_blend_at(const String &p_source_key, int p_source_phase_ticks,
			const String &p_target_key, int p_target_phase_ticks, double p_weight);
	void play_body_clip_seeded(const String &p_key, int p_phase_ticks);
	bool apply_remote_body_state(int p_state_id, const String &p_key, int p_flags,
			int p_phase_ticks = -1);
	void reset_remote_body_state();
	bool advance_remote_body_blend_tick(int p_state_id);
	bool remote_body_needs_fixed_tick() const;
	void stop_body_clip();
	String get_active_body_clip() const { return anim_key_; }
	void play_body_anim(int p_slot);
	void play_body_anim_at(int p_slot, int p_phase_ticks);
	int64_t get_animation_time_ms() const { return anim_time_ms_; }
	void set_animation_time(double p_seconds);
	double get_animation_time() const;
	void play_part_anim(int p_channel, int p_play_type, double p_time_s);
	void restart_part_anim(int p_channel, int p_play_type, double p_time_s);
	void set_part_phase(int p_channel, int64_t p_phase);
	void clear_part_phase(int p_channel);
	void clear_part_anims();
	// The registers carrying a running PLAYPARTANIM sweep (the sweep's live
	// phase is the register's value, get_ctrl_values); empty = none running.
	PackedStringArray get_active_part_anim_registers() const;
	void set_weapon_channel(const String &p_key, int p_phase_ticks,
			const String &p_prev_key = String(), int p_prev_phase_ticks = 0,
			float p_blend_weight = 1.0f, int p_variant = 0, int p_prev_variant = 0);
	// The applied weapon-channel pose — presentation-state read-back: whether
	// a channel is held, its clip key and its phase (-1 = not replicated).
	bool has_weapon_channel() const;
	String get_weapon_channel_key() const { return wpn_key_; }
	int get_weapon_channel_phase_ticks() const { return wpn_phase_ticks_; }
	// The typed present path: p_deltas is kAimOverlayClasses body-relative
	// per-class rotations; clear drops the overlay.
	void set_aim_overlay_deltas(const Basis *p_deltas);
	void clear_aim_overlay();
	// The script-facing form (an Array of kAimOverlayClasses Basis, or empty
	// to clear) converts into the typed path.
	void set_aim_overlay(const Array &p_deltas);
	Array get_aim_overlay() const;

	void set_right_hand_collapsed(bool p_collapsed);
	bool is_right_hand_collapsed() const { return collapse_right_hand_; }
	// The active two-channel blend — presentation-state read-back: whether a
	// second channel is blending (a single channel poses the body otherwise),
	// its source clip key, its playhead in seconds and the blend weight.
	bool has_body_blend() const;
	String get_body_blend_source_key() const { return body_blend_source_key_; }
	float get_body_blend_source_time() const { return static_cast<float>(body_blend_source_time_); }
	float get_body_blend_weight() const { return body_blend_weight_; }
	void advance_body_animation(double p_delta, bool p_write_pose = true);
	// Diagnostics: whether a body-pose input changed since the last pose write
	// (the aim-overlay/weapon-channel dedup fast path pins against this).
	bool is_body_pose_dirty() const { return body_pose_dirty_; }
};

} // namespace godot
VARIANT_ENUM_CAST(godot::ObjectModel::AwakeFrameProfileSlot);
VARIANT_ENUM_CAST(godot::ObjectModel::AvatarPart);
VARIANT_ENUM_CAST(godot::ObjectModel::PresentationLayer);
