#pragma once

#include <atomic>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <oed/oed.h>
#include <tdp/tdp.h>
#include <threedi/threedi_3di3.h>
#include <threedi/threedi_panm.h>

#include <renderer/material_eval.h>

#include "resource_index/nova_resource_root.h"

namespace godot {

class ObjectData : public Resource {
	GDCLASS(ObjectData, Resource)

private:
	enum class SourceKind {
		Empty,
		Threedi,
		Project,
		Ase,
	};

	// THE document: the parsed (or OED-built) 3DI3 model, edited in place.
	// 3DI sources parse straight into it; project/ASE sources keep the model
	// the OED session builds. There is no intermediate representation.
	Threedi3di3 source_model = {};
	TdpProject source_project = {};
	OedSession *oed_session = nullptr;

	SourceKind source_kind = SourceKind::Empty;
	bool has_source_model = false;
	bool has_source_project = false;
	uint8_t oed_dirty_mask = 0;
	uint8_t last_oed_update_mask = 0;
	uint64_t change_revision_ = 0;
	// Process-wide edit counter bumped alongside every per-document
	// change_revision_ bump. Per-frame consumers (the static-shadow planner)
	// compare it once instead of walking every tracked document.
	static std::atomic<uint64_t> global_change_counter_;

	String source_path;
	String source_dir;
	String object_name = "untitled";
	String last_error;
	Ref<ResourceRoot> resource_root;

	// Memoized build_lod_submeshes results, keyed (lod | skeletal | bone_count).
	// Entries hold SHARED Ref<ArrayMesh> refs: every model instance built from
	// one ObjectData renders the same meshes (the mission placer shares one
	// data per graphic, so N animated entities stop paying N mesh builds).
	// Consumers must never mutate the meshes — materials apply via
	// MeshInstance3D.material_override. Cleared by _clear() and
	// _notify_object_changed(), the two funnels every document mutation passes
	// through. Main-thread only, like the rest of this class.
	mutable std::unordered_map<uint64_t, Array> submesh_cache;
	static uint64_t _submesh_cache_key(int p_lod_index, bool p_skeletal, int p_bone_count, bool p_native_frame);

	// Per-frame PANM evaluation cache behind apply_panm_to_nodes: one data
	// instance is SHARED across every placed model of the same graphic (the
	// placer's per-graphic cache). Deterministic tracks at the same clock/bus
	// reuse an evaluation; noise tracks deliberately re-evaluate per instance
	// because retail consumes one CRT sample per submitted model. `changed`
	// marks parts whose transform moved since
	// the PREVIOUS evaluation; `revision` bumps when any did, letting a caller
	// that already applied this revision skip every node write. Invalidated by
	// _notify_object_changed()/_clear() like the submesh cache. Main-thread
	// only.
	struct PanmEvalCache {
		int lod = -1;
		int64_t time_ms = -1;
		uint64_t ctrl_hash = 0;
		bool valid = false;
		bool has_noise = false;
		// Diagnostic serial: increments whenever node matrices are actually
		// evaluated, even if a random sample happens to reproduce the prior
		// transform and therefore does not mint a changed-pose revision.
		uint64_t evaluation_serial = 0;
		uint64_t revision = 0;
		std::vector<ThreediPartAnimation> anims;         // effective set for `lod`
		std::vector<ThreediMatrix4x4> base_transforms;   // rebuilt on invalidation
		std::vector<ThreediVec3> pivots;
		std::vector<ThreediMatrix4x4> node_matrices;     // scratch, per anim node
		std::vector<int> part_to_node;
		std::vector<Transform3D> part_transforms;        // per part, godot frame
		std::vector<uint64_t> part_revision;             // revision at last change
	};
	mutable PanmEvalCache panm_cache_;
	void _invalidate_panm_cache() { panm_cache_.valid = false; panm_cache_.lod = -1; }
	bool _panm_cache_prepare(int p_lod_index) const;
	// Material generator fixups depend only on the loaded document's local CTRL
	// table. Cache their native names once instead of rebuilding a
	// vector<string> for every material of every model on every render frame.
	mutable std::vector<std::string> runtime_control_names_cache_;
	mutable bool runtime_control_names_valid_ = false;
	const std::vector<std::string> &_runtime_control_names() const;
	void _invalidate_runtime_control_names() {
		runtime_control_names_valid_ = false;
		runtime_control_names_cache_.clear();
	}

	void _clear();
	void _clear_oed_session();
	void _clear_source_model();
	void _clear_source_project();
	void _mark_oed_dirty(uint8_t p_update_mask);
	void _clear_oed_dirty(uint8_t p_update_mask);
	uint8_t _normalize_oed_update_mask(int p_update_mask) const;
	void _notify_object_changed(uint8_t p_update_mask = 0);
	Error _open_3di(const String &p_path);
	Error _open_3di_bytes(const String &p_name, const PackedByteArray &p_bytes);
	Error _open_3dp(const String &p_path);
	Error _open_ase(const String &p_path);
	void _seed_project_materials_from_ase_session();
	Error _build_model_from_project_session(const char *p_model_name, uint8_t p_dirty_mask = 0);
	Error _rebuild_oed_session_from_project(uint8_t p_dirty_mask = 0);
	Error _export_project_backed_3di(const String &p_path, uint8_t p_update_mask);
	Error _export_patched_3di(const String &p_path);
	TdpProject _build_project_from_model() const;
	String _export_basename() const;
	String _source_kind_name() const;
	bool _effective_panm_for_lod(int p_lod_index,
			std::vector<ThreediPartAnimation> &r_nodes) const;

protected:
	static void _bind_methods();

public:
	enum {
		UPDATE_NONE = 0,
		UPDATE_MTRL = OED_UPDATE_MTRL,
		UPDATE_LGHT = OED_UPDATE_LGHT,
		UPDATE_PANM = OED_UPDATE_PANM,
		UPDATE_ALL = OED_UPDATE_ALL,
	};

	// 3DI3 flag/slot re-exports (engine threedi/threedi_3di3.h is the value
	// authority; defined FROM it so they can never drift). Bound as class
	// constants so the ONED inspectors stop re-declaring the bytes.
	enum {
		MATERIAL_FLAG_ALPHA_TEST = THREEDI_MATERIAL_FLAG_ALPHA_TEST,
		MATERIAL_FLAG_ALPHA_INVERT = THREEDI_MATERIAL_FLAG_ALPHA_INVERT,
		MATERIAL_FLAG_TWO_SIDED = THREEDI_MATERIAL_FLAG_TWO_SIDED,
		TEX_FLAG_ANIMATED = THREEDI_TEX_FLAG_ANIMATED,
		TEX_FLAG_CLAMPED = THREEDI_TEX_FLAG_CLAMPED,
		TEX_SLOT_DIFFUSE = THREEDI_TEX_SLOT_DIFFUSE,
		TEX_SLOT_DETAIL = THREEDI_TEX_SLOT_DETAIL,
		TEX_SLOT_NORMAL = THREEDI_TEX_SLOT_NORMAL,
		TEX_SLOT_NORMAL_B = THREEDI_TEX_SLOT_NORMAL_B,
		LIGHT_FLAG_DISABLE_CORONA = THREEDI_LIGHT_FLAG_DISABLE_CORONA,
		LIGHT_FLAG_DISABLE_TERRAIN = THREEDI_LIGHT_FLAG_DISABLE_TERRAIN,
		LIGHT_FLAG_DISABLE_OBJECTS = THREEDI_LIGHT_FLAG_DISABLE_OBJECTS,
		LIGHT_FLAG_TYPE_TARGET = THREEDI_LIGHT_FLAG_TYPE_TARGET,
	};

	// Generator-style consumers (threedi/threedi_panm.h
	// ThreediGeneratorConsumer): the 0x71..0x75 control-register range
	// dispatches differently per retail consumer.
	enum {
		GENERATOR_CONSUMER_UV = THREEDI_GENERATOR_CONSUMER_UV,
		GENERATOR_CONSUMER_RGB = THREEDI_GENERATOR_CONSUMER_RGB,
		GENERATOR_CONSUMER_ALPHA = THREEDI_GENERATOR_CONSUMER_ALPHA,
		GENERATOR_CONSUMER_LIGHT = THREEDI_GENERATOR_CONSUMER_LIGHT,
		GENERATOR_CONSUMER_PANM = THREEDI_GENERATOR_CONSUMER_PANM,
	};

	// --- Witnessed threedi catalog/unit re-exports (statics; the engine
	// headers carry the witnesses) ---
	// PANM track units (threedi_panm.h): rotations in 360/16384-degree
	// counts, values/speeds in signed 8.8 (1/256 per count).
	static double panm_rotation_unit_deg();
	static double panm_value_unit();
	// Authorable per-track ranges in human units — tracks are int16, so this
	// is the int16 span through the units above:
	// { rotation_min/max: deg, value_min/max: 8.8, speed_min/max: 8.8 }.
	static Dictionary panm_track_limits();
	// The semantic PANM mode catalog the part-animation inspector offers:
	// [{ mode:String, label:String, control:int, uses_control_register:bool }].
	static Array get_panm_mode_options();
	// The canonical generator-style byte list (engine kControlEntries) + the
	// per-consumer dispatch matrix and the loader's ctrl-reference rule.
	static PackedInt32Array get_generator_style_ids();
	static String generator_style_code_name(int p_style_id);
	static bool generator_style_reads_control_value(int p_consumer, int p_style_id);
	static bool generator_style_parameter_is_ctrl_reference(int p_style_id);

	ObjectData();
	~ObjectData();

	// Native-side read access to the parsed model. The collision sweep
	// (Simulation::resolve_collision_instances) builds the runtime collision
	// model from the CDTA block; GDScript keeps the curated getters only.
	const Threedi3di3 &native_model() const { return source_model; }

	Error open_file(const String &p_path);
	// Mounted .3DI loads also feed the retail-compatible network challenge registry.
	// Foliage definitions pass false: retail marks their shared model-def node and
	// excludes it from the snapshot built for C2S 0x3D.
	Error open_from_resource_root(const Ref<ResourceRoot> &p_resource_root,
			const String &p_name, bool p_include_in_network_challenge = true);
	// A renderer mesh-cache hit is still a logical model-definition load in the
	// new mission generation. Recreate retail's sticky foliage mark without
	// reparsing/rebuilding the cached .3DI.
	static void mark_cached_network_challenge_foliage_model(const String &p_name);
	static void reset_network_challenge_model_registry();
	static int64_t network_challenge_model_count();
	Error save_project_to_dir(const String &p_dir_path);
	Error export_3di_to_dir(const String &p_dir_path, int p_update_mask = 0);
	void reset_empty(const String &p_name = "untitled");
	Error set_lod_scene(int p_lod_index, const String &p_path);

	// Whole-document edit-state snapshot for the editor's undo (B3): the
	// OED-editable state (object name, materials, lights, per-LOD part
	// animations, project LOD scalars, dirty mask) as one opaque in-process
	// byte blob. NOT a persistence format: raw POD bytes + a version tag,
	// valid only against the same geometry (apply validates the fixed
	// material/light/LOD counts and rejects cross-geometry restores).
	PackedByteArray snapshot_edit_state() const;
	Error apply_edit_state(const PackedByteArray &p_bytes);

	bool has_document() const;
	bool can_save_project() const;
	bool can_export_3di() const;
	String get_source_path() const;
	String get_source_dir() const;
	String get_object_name() const;
	String get_source_kind() const;
	String get_last_error() const;
	int get_oed_dirty_mask() const;
	int get_last_oed_update_mask() const;
	uint64_t get_change_revision() const { return change_revision_; }
	static uint64_t get_global_change_counter() {
		return global_change_counter_.load(std::memory_order_relaxed);
	}
	Dictionary get_summary() const;
	Array get_project_lods() const;
	bool set_lod_field(int p_lod_index, const String &p_key, const Variant &p_value);
	bool set_project_field(const String &p_key, const Variant &p_value);

	int get_material_count() const;
	Array get_lod_surfaces(int p_lod_index) const;
	bool is_skinned(int p_lod_index) const;
	Array get_materials() const;
	Dictionary get_material_info(int p_index) const;
	bool set_material_field(int p_index, const String &p_key, const Variant &p_value);
	int get_material_shader_flags(int p_index) const;
	PackedStringArray get_material_anim_frames(int p_index, int p_slot) const;
	bool set_material_anim_frame(int p_index, int p_slot, int p_frame_idx, const String &p_path);
	Array get_shader_catalog() const;
	static Array get_global_control_register_catalog();
	static String canonical_control_register_name(const String &p_name);
	Array get_control_registers() const;
	bool set_control_register_name(int p_index, const String &p_name);
	String resolve_material_texture_path(int p_material_index, int p_texture_index) const;
	Ref<Texture2D> load_material_texture(int p_material_index, int p_texture_index) const;
	String resolve_texture_name(const String &p_texture_name) const;
	Ref<Texture2D> load_texture_name(const String &p_texture_name) const;
	int get_light_count() const;
	Array get_lights() const;
	Dictionary get_light_info(int p_index) const;
	bool set_light_field(int p_index, const String &p_key, const Variant &p_value);
	int get_user_point_count() const;
	Dictionary get_user_point_info(int p_index) const;
	// The item-effect attach scan: name -> 16-bit mask over the FIRST 16
	// userpoints (case-insensitive; duplicate names all match) — one impl in
	// engine/formats/threedi. [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]
	int get_user_point_bone_mask(const String &p_name) const;
	// The PLAYPARTANIM editor-preview integrator's engine math (one impl in
	// engine/runtime/world ai.h): the witnessed rate from ANIMTIME seconds
	// [orig: Entity_ApplyCommand @0x43B1A9..0x43B1F9] and one 16 ms sweep step
	// (returns {"phase": int, "finished": bool})
	// [orig: Entity_UpdateSuspensionBounce @0x456740..0x4567A9]. The
	// authoritative runtime integrates in AiSystem and presents through
	// set_part_phase; the preview drives the same math through these.
	static int part_anim_rate_for_seconds(double p_seconds);
	static Dictionary part_anim_step(int p_phase, int p_dir, int p_rate);
	Vector3 get_ground_anchor(int p_lod_index = 0) const;
	bool has_collision() const;
	// The model carries GPM-family occlusion/portal records (OVRT/OPLN/OFAC/OOBJ)
	// — the placer de-batches such buildings so their sections can be masked
	// per frame [orig: the model +0xDC/+0xE0 record gate all consumers use].
	bool has_occlusion() const;
	Array get_collision_volumes() const;
	// Effective PANM source selection mirrors retail: a LOD-local block wins,
	// otherwise the model-level PANM block is inherited. Collision asks only
	// for LOD0; the any-LOD query is for visual de-batching.
	bool has_live_panm() const;
	bool has_live_panm_for_lod(int p_lod_index) const;
	int get_live_panm_lod() const;
	PackedInt32Array get_effective_panm_targets(int p_lod_index) const;
	int get_part_anim_count(int p_lod_index) const;
	Array get_part_animations(int p_lod_index) const;
	Array get_part_anim_editor_entries(int p_lod_index) const;
	int add_part_anim(int p_lod_index, int p_part_index);
	int duplicate_part_anim(int p_lod_index, int p_anim_index);
	bool delete_part_anim(int p_lod_index, int p_anim_index);
	bool set_part_anim_target(int p_lod_index, int p_anim_index, int p_part_index, int p_parent_part);
	bool set_part_anim_channel_enabled(int p_lod_index, int p_anim_index, const String &p_channel, bool p_enabled);
	bool set_part_anim_channel_mode(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, const String &p_mode, int p_control_register);
	bool set_part_anim_channel_values(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, double p_from_value, double p_to_value, double p_speed);
	bool set_part_anim_rotation_reversed(int p_lod_index, int p_anim_index, bool p_reversed);
	Dictionary get_part_anim_info(int p_lod_index, int p_anim_index) const;
	bool set_part_anim_field(int p_lod_index, int p_anim_index, const String &p_key, const Variant &p_value);
	bool set_part_anim_track_field(int p_lod_index, int p_anim_index, const String &p_track, const String &p_key, const Variant &p_value);
	Dictionary get_render_lod_info(int p_lod_index) const;
	// Per-part parent-relative bone pivot (native model space, raw ThreediRenderObject.rel),
	// indexed by part index, for the given LOD -- the model's authoritative bone rest positions.
	// The skeletal runtime feeds these to SkeletalAnim in place of the .bad's lossy
	// BadBone.position (roughly half the .bad corpus triplicates X into all 3 slots). Matches the
	// original engine, which sources bone pivots from the model bone-def table, not the .bad.
	// [orig: the modelDef+56 pivot table read by BoneAnim_BuildWorldMatrices @0x40c400.]
	PackedVector3Array get_bone_origins(int p_lod_index = 0) const;
	// Per-part parent index (raw ThreediRenderObject.parent_index), indexed by part index, for
	// the given LOD -- the model's authoritative bone hierarchy, paired with get_bone_origins as
	// the model bone table. The root part's parent is itself in the file; SkeletalAnim/
	// sample_clip normalize that to -1. [orig: the modelDef+56 row's +20 parent index read by
	// BoneAnim_BuildWorldMatrices @0x40c400 -- the FK hierarchy comes from the MODEL, never
	// the .bad.]
	PackedInt32Array get_bone_parents(int p_lod_index = 0) const;
	// p_native_frame: emit vertices/normals/tangents in the NATIVE model frame (no (-x,y,z)
	// import flip) with triangle winding reversed to stay front-facing under Godot's CCW cull.
	// For the first-person viewmodel rigs, whose skeletal runtime (SkeletalAnim model_bind)
	// poses in the native frame; the owner maps the whole rig to the camera in one container
	// transform. World models keep the default flipped frame.
	Array build_lod_submeshes(int p_lod_index, bool p_skeletal = false, int p_bone_count = 0,
			bool p_native_frame = false) const;
	Dictionary eval_material_runtime(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const;
	int compute_anim_frame(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const;
	// Typed render hot path. The script-facing methods above remain the tooling
	// boundary; ObjectModel converts its CTRL dictionary once per frame, then
	// evaluates every dynamic material without Dictionary/Variant round trips.
	static renderer::ControlRegisterValues runtime_control_values(
			const Dictionary &p_ctrl_values);
	bool eval_material_runtime_native(int p_index, int64_t p_time_ms,
			const renderer::ControlRegisterValues &p_ctrl_values,
			renderer::MaterialRuntime &r_runtime) const;
	int compute_anim_frame_native(int p_index, int64_t p_time_ms,
			const renderer::ControlRegisterValues &p_ctrl_values) const;
	Dictionary evaluate_panm(int p_lod_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const;
	// The hot-path form of evaluate_panm: evaluates through the shared
	// per-graphic frame cache and writes ONLY changed part transforms onto the
	// caller's node array (index = part index; null/absent entries skipped).
	// p_applied_revision is what the caller last applied: the returned
	// revision equal to it means nothing was written; 0 forces a full apply
	// (fresh nodes). The common empty-control path performs no per-call
	// allocation, and the result never boxes transforms into a Dictionary.
	int64_t apply_panm_to_nodes(int p_lod_index, int64_t p_time_ms,
			const Dictionary &p_ctrl_values, const Array &p_nodes,
			int64_t p_applied_revision) const;
	int64_t get_panm_evaluation_serial() const;
	Array evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const;

	Error set_material_shader(int p_material_index, const String &p_shader_name);
	Error set_material_texture(int p_material_index, int p_texture_index, const String &p_texture_name);
	Error set_material_texture_slot(int p_material_index, int p_slot, const String &p_texture_name);
	Error set_material_texture_slot_options(int p_material_index, int p_slot, int p_flags, int p_frame, int p_type);
	Error set_material_alpha_threshold(int p_material_index, float p_alpha_threshold);
	Error set_material_uv_generator(int p_material_index, const String &p_axis, const Dictionary &p_params);
	Error set_material_rgb_generator(int p_material_index, const Dictionary &p_params);
	Error set_material_alpha_generator(int p_material_index, const Dictionary &p_params);
	Error set_material_texture_animation(int p_material_index, const Dictionary &p_params);
	Error set_light_colors(int p_light_index, const Color &p_start, const Color &p_end);
	Error set_part_animation_flags(int p_lod_index, int p_anim_index, int p_flags);
};

} // namespace godot
