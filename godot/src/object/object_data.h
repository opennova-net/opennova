#pragma once

#include <atomic>

namespace opennova::env {
struct WeatherOscillator;
}

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

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include <runtime/renderer/material_eval.h>

#include "resource_index/resource_root.h"

namespace godot {

class ObjectData : public Resource {
	GDCLASS(ObjectData, Resource)

private:
	// Immutable parsed runtime content. Loading another .3di replaces the whole
	// model and invalidates every derived cache; there is no Godot authoring
	// session or editable intermediate representation.
	Threedi3di3 source_model = {};
	bool has_source_model = false;
	uint64_t change_revision_ = 0;
	// Process-wide content counter bumped alongside every per-document revision.
	// Per-frame consumers compare it once instead of walking every tracked model.
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
	// Consumers must never mutate the meshes; materials apply via
	// MeshInstance3D.material_override. Cleared by _clear() and
	// _notify_object_changed(), the two whole-content replacement funnels.
	// Main-thread only, like the rest of this class.
	mutable std::unordered_map<uint64_t, Array> submesh_cache;
	static uint64_t _submesh_cache_key(int p_lod_index, bool p_skeletal, int p_bone_count, bool p_native_frame);

	// Per-frame PANM evaluation cache behind apply_panm_to_nodes: one data
	// instance is SHARED across every placed model of the same graphic (the
	// placer's per-graphic cache). Deterministic tracks at the same clock/bus
	// reuse an evaluation; noise tracks deliberately re-evaluate per instance
	// because retail consumes one CRT sample per submitted model. `changed`
	// marks parts whose transform moved since
	// the PREVIOUS evaluation; `revision` bumps when any did, letting a caller
	// that already applied this revision skip every node write. One cache per
	// authored RLOD (PANM is written per RLOD), so instances of one graphic
	// drawn at mixed levels never evict each other's evaluation. Invalidated by
	// _notify_object_changed()/_clear() like the submesh cache. Main-thread
	// only.
	struct PanmEvalCache {
		int lod = -1;
		int64_t time_ms = -1;
		uint64_t ctrl_hash = 0;
		bool valid = false;
		bool has_noise = false;
		uint64_t revision = 0;
		std::vector<ThreediPartAnimation> anims;         // effective set for `lod`
		std::vector<ThreediMatrix4x4> base_transforms;   // rebuilt on invalidation
		std::vector<ThreediVec3> pivots;
		std::vector<ThreediMatrix4x4> node_matrices;     // scratch, per anim node
		std::vector<int> part_to_node;
		std::vector<Transform3D> part_transforms;        // per part, godot frame
		std::vector<uint64_t> part_revision;             // revision at last change
	};
	mutable std::vector<PanmEvalCache> panm_caches_; // indexed by LOD
	// Diagnostic serial: increments whenever node matrices are actually
	// evaluated for any level, even if a random sample happens to reproduce
	// the prior transform and therefore does not mint a changed-pose revision.
	mutable uint64_t panm_evaluation_serial_ = 0;
	void _invalidate_panm_cache() { panm_caches_.clear(); }
	PanmEvalCache *_panm_cache_prepare(int p_lod_index) const;
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
	void _clear_source_model();
	void _notify_object_changed();
	Error _open_3di(const String &p_path);
	Error _open_3di_bytes(const String &p_name, const PackedByteArray &p_bytes);
	bool _effective_panm_for_lod(int p_lod_index,
			std::vector<ThreediPartAnimation> &r_nodes) const;

protected:
	static void _bind_methods();

public:
	// 3DI3 flag/slot re-exports (engine threedi/threedi_3di3.h is the value
	// authority; defined FROM it so they can never drift). Bound as class
	// constants so runtime consumers do not re-declare the bytes.
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

	bool has_document() const;
	String get_source_path() const;
	String get_last_error() const;
	uint64_t get_change_revision() const { return change_revision_; }
	static uint64_t get_global_change_counter() {
		return global_change_counter_.load(std::memory_order_relaxed);
	}
	Dictionary get_summary() const;

	int get_material_count() const;
	Array get_lod_surfaces(int p_lod_index) const;
	bool is_skinned(int p_lod_index) const;
	Array get_materials() const;
	Dictionary get_material_info(int p_index) const;
	PackedStringArray get_material_anim_frames(int p_index, int p_slot) const;
	static String canonical_control_register_name(const String &p_name);
	// Drops the register-name memo; the module terminator calls it so no
	// godot::String outlives the extension.
	static void clear_static_caches();
	Array get_control_registers() const;
	String resolve_material_texture_path(int p_material_index, int p_texture_index) const;
	Ref<Texture2D> load_material_texture(int p_material_index, int p_texture_index) const;
	Ref<Texture2D> load_texture_name(const String &p_texture_name) const;
	int get_light_count() const;
	Dictionary get_light_info(int p_index) const;
	int get_user_point_count() const;
	Dictionary get_user_point_info(int p_index) const;
	// The item-effect attach scan: name -> 16-bit mask over the FIRST 16
	// userpoints (case-insensitive; duplicate names all match) — one impl in
	// engine/formats/threedi. [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]
	int get_user_point_bone_mask(const String &p_name) const;
	// PLAYPARTANIM's engine math (one impl in engine/runtime/world ai.h): the
	// witnessed rate from ANIMTIME seconds
	// [orig: Entity_ApplyCommand @0x43B1A9..0x43B1F9] and one 16 ms sweep step
	// (returns {"phase": int, "finished": bool})
	// [orig: Entity_UpdateSuspensionBounce @0x456740..0x4567A9]. The
	// authoritative AI path integrates in AiSystem and presents through
	// set_part_phase; local runtime controllers use the same helpers.
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
	Dictionary get_part_anim_info(int p_lod_index, int p_anim_index) const;
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
	// runtime mirror) with triangle winding reversed to stay front-facing under Godot's CCW cull.
	// For the first-person viewmodel rigs, whose skeletal runtime (SkeletalAnim model_bind)
	// poses in the native frame; the owner maps the whole rig to the camera in one container
	// transform. World models keep the default flipped frame.
	Array build_lod_submeshes(int p_lod_index, bool p_skeletal = false, int p_bone_count = 0,
			bool p_native_frame = false) const;
	Dictionary eval_material_runtime(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const;
	// Typed render hot path. ObjectModel converts its CTRL dictionary once per frame, then
	// evaluates every dynamic material without Dictionary/Variant round trips.
	static opennova::renderer::ControlRegisterValues runtime_control_values(
			const Dictionary &p_ctrl_values);
	// The weather's CTRL registers, FLICKER (3) and SWING (4): the wave rings
	// hashed on a position. Retail writes the global slots 0x83FD00 / 0x83FD08
	// from HUD_CacheEntityDisplayInfo — on the LOCAL PLAYER from the HUD and
	// viewmodel legs (@ 0x5a8341, @ 0x4dee8b) and on the RENDERED ENTITY from
	// the gnrc/Sway world bone callbacks (@ 0x4e286c, @ 0x4e2b22) right before
	// its batch snapshots the registers. The Weather node publishes the local
	// player's pair (the frame default) and the ring copy every display
	// frame; a model whose 3DI declares either register hashes its own
	// position through weather_ctrl_registers_at. A model's own dictionary
	// entry wins over the default.
	static void set_weather_ctrl_registers(int32_t p_flicker, int32_t p_swing);
	static void set_weather_rings(const opennova::env::WeatherOscillator &p_oscillator);
	static void clear_weather_rings();
	static bool weather_ctrl_registers_at(int32_t p_x_q16, int32_t p_y_q16, int32_t p_z_q16,
			int32_t &r_flicker, int32_t &r_swing);
	bool uses_weather_ctrl_registers() const;
	bool eval_material_runtime_native(int p_index, int64_t p_time_ms,
			const opennova::renderer::ControlRegisterValues &p_ctrl_values,
			opennova::renderer::MaterialRuntime &r_runtime) const;
	int compute_anim_frame_native(int p_index, int64_t p_time_ms,
			const opennova::renderer::ControlRegisterValues &p_ctrl_values) const;
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
	// The hot variant (distinct name — an overload would ambiguate the
	// Dictionary form's ClassDB bind): a retained caller passes its cached
	// converted table so the per-call dict iteration disappears.
	int64_t apply_panm_to_nodes_table(int p_lod_index, int64_t p_time_ms,
			const opennova::renderer::ControlRegisterValues &p_ctrl_table,
			const Array &p_nodes, int64_t p_applied_revision) const;
	// The dict conversion split for retained callers: the dict-only half
	// caches per change; FLICKER/SWING ride the live weather globals at use
	// time unless the dict pins them (the same override order the one-shot
	// runtime_control_values applies).
	static opennova::renderer::ControlRegisterValues
	runtime_control_values_dict_only(const Dictionary &p_ctrl_values,
			bool &r_has_flicker, bool &r_has_swing);
	static void stamp_weather_ctrl_registers(
			opennova::renderer::ControlRegisterValues &r_values,
			bool p_dict_has_flicker, bool p_dict_has_swing);
	int64_t get_panm_evaluation_serial() const;
	Array evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const;
};

} // namespace godot
