// ObjectModel: retained-scene construction — child teardown, cache
// reset, Skeleton3D/Skin construction, mesh/material assembly, the
// submission notifier, and transformed-mesh bounds. Ported verbatim from
// nova_object_scene_builder.gd (2026-08-09 de-scripting).

#include "object/nova_object_model.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>

namespace godot {

void ObjectModel::rebuild_scene() {
	while (get_child_count() > 0) {
		Node *child = get_child(0);
		remove_child(child);
		child->queue_free();
	}
	// The submission notifier died in the sweep above; set_model_bounds
	// recreates it against the rebuilt bounds (even equal ones).
	screen_notifier_ = nullptr;
	robj_nodes_.clear();
	robj_rest_transforms_.clear();
	skeleton_ = nullptr;
	skeleton_skin_.unref();
	muzzle_bone_ = -1;
	surface_material_indices_.clear();
	surface_materials_.clear();
	surface_lighting_contexts_.clear();
	alpha_strip_draws_.clear();
	alpha_strip_models_.erase(this);
	render_order_dirty_ = true;
	point_light_draw_parts_dirty_ = true;
	set_notify_transform(false);
	anim_frames_by_mat_.clear();
	material_cache_.clear();
	material_defs_.clear();
	body_pose_dirty_ = true;
	bounds_dirty_ = true;
	has_live_panm_ = false;
	material_needs_eval_.clear();
	dynamic_material_slots_ = PackedInt32Array();
	material_runtime_stamps_.clear();
	last_env_gen_ = -1;
	last_env_values_.unref();
	last_section_env_values_.unref();
	point_light_selection_hashes_.clear();
	robj_dense_ = Array();
	panm_applied_revision_ = 0;
	if (object_data_.is_null() || !object_data_->has_document()) {
		od_has_doc_ = false;
		set_model_bounds(AABB());
		return;
	}
	od_has_doc_ = true;

	build_material_defs();
	active_lod_ = clamp_lod_index(active_lod_);
	refresh_live_panm_classification();
	// A loaded .adm drives the model: build a Skeleton3D from its .bad
	// skeleton — per-vertex skinned models AND rigid first-person weapons
	// (fake skinning). Without a .adm the model renders static.
	const bool skeletal_mode = skeletal_.is_valid() && skeletal_->is_loaded();
	if (skeletal_mode) {
		build_skeleton();
		resolve_muzzle_userpoint();
	}
	const int bone_count =
			skeletal_mode && skeleton_ != nullptr ? skeleton_->get_bone_count() : 0;
	const Array submeshes = object_data_->build_lod_submeshes(active_lod_,
			skeletal_mode, bone_count, native_frame_);
	for (int64_t entry = 0; entry < submeshes.size(); ++entry) {
		const Dictionary submesh = submeshes[entry];
		const Ref<ArrayMesh> mesh = submesh.get("mesh", Variant());
		if (mesh.is_null()) {
			continue;
		}
		const int robj_index =
				int(submesh.get("robj_index", submesh.get("part_index", 0)));
		const int material_index = int(submesh.get("material_index", 0));
		if (!robj_rest_transforms_.has(robj_index)) {
			robj_rest_transforms_[robj_index] =
					Transform3D(Basis(), submesh.get("abs", Vector3()));
		}
		const int lighting_context = lighting_context_for_robj(robj_index);
		MeshInstance3D *instance = memnew(MeshInstance3D);
		instance->set_mesh(mesh);
		instance->set_cast_shadows_setting(shadow_caster_layers_ != 0
						? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
						: GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		// Mission placement has already resolved the engine's two-part
		// building/vehicle reflection policy; this device leg only maps that
		// typed decision to Godot visibility layers.
		instance->set_layer_mask(
				(mirror_reflected_ ? LAYER_WORLD : LAYER_WORLD_NO_MIRROR) |
				shadow_caster_layers_);
		Ref<ShaderMaterial> material =
				material_for_index(material_index, lighting_context);
		// One imported submesh is one retail strip. Transparent strips must own
		// their material instance because Godot stores render_priority on the
		// material, while retail chooses Q1/Q2 independently for every strip on
		// every frame (renderer/render_order owns the cited queue contract).
		// Opaque strips may keep sharing their retained
		// material cache entry.
		if (bool(submesh.get("is_alpha", false)) && material.is_valid()) {
			material = material->duplicate();
		}
		instance->set_material_override(material);
		// Skinned + rigid-fake-skinned submeshes bind to the shared
		// Skeleton3D; everything else stays under its Robj part node so PANM
		// part transforms keep working.
		if (skeletal_mode && skeleton_ != nullptr &&
				bool(submesh.get("is_skinned", false))) {
			skeleton_->add_child(instance);
			instance->set_skin(skeleton_skin_);
			instance->set_skeleton_path(instance->get_path_to(skeleton_));
		} else {
			Node3D *node = get_or_create_robj_node(robj_index);
			node->add_child(instance);
		}
		if (bool(submesh.get("is_alpha", false)) && material.is_valid()) {
			AlphaStripDraw draw;
			draw.instance = instance;
			draw.material = material;
			draw.local_center = mesh->get_aabb().get_center();
			draw.bone_path = skeletal_mode &&
					bool(submesh.get("is_skinned", false));
			alpha_strip_draws_.push_back(draw);
		}
		// Retail multi-pass effects retain one logical material but submit the
		// same strip geometry again. Pair those auxiliary materials through
		// metadata and duplicate geometry only; never duplicate logical rows.
		auto add_auxiliary_draw = [&](const StringName &p_material_meta,
				const String &p_name, const StringName &p_kind_meta) {
			if (!material->has_meta(p_material_meta)) {
				return;
			}
			const Ref<ShaderMaterial> auxiliary_material = material->get_meta(
					p_material_meta, Variant());
			if (auxiliary_material.is_null()) {
				return;
			}
			MeshInstance3D *auxiliary_instance = memnew(MeshInstance3D);
			auxiliary_instance->set_name(p_name);
			auxiliary_instance->set_mesh(mesh);
			auxiliary_instance->set_material_override(auxiliary_material);
			auxiliary_instance->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			auxiliary_instance->set_layer_mask(
					mirror_reflected_ ? LAYER_WORLD : LAYER_WORLD_NO_MIRROR);
			auxiliary_instance->set_meta("_opennova_auxiliary_draw", true);
			auxiliary_instance->set_meta(p_kind_meta, true);
			Node *surface_parent = instance->get_parent();
			if (surface_parent == nullptr) {
				memdelete(auxiliary_instance);
				return;
			}
			surface_parent->add_child(auxiliary_instance);
			if (skeletal_mode && skeleton_ != nullptr &&
					bool(submesh.get("is_skinned", false))) {
				auxiliary_instance->set_skin(skeleton_skin_);
				auxiliary_instance->set_skeleton_path(
						auxiliary_instance->get_path_to(skeleton_));
			}
		};
		add_auxiliary_draw("_opennova_postmultiply_material", "PostMultiply",
				"_opennova_postmultiply_proxy");
		surface_material_indices_.append(material_index);
		surface_materials_.push_back(material);
		surface_lighting_contexts_.append(static_cast<uint8_t>(lighting_context));
		collect_anim_frames(material_index);
	}

	classify_materials();
	apply_runtime_state(0.0);
	// Newly rebuilt GeometryInstance3Ds have default instance uniforms. Keep
	// the stance gate immediately correct; the terrain-frame leg supplies the
	// resident page after Terrain has serviced its cache requests.
	stamp_match_terrain_instances(false, 0.0f, Vector4());
	if (!alpha_strip_draws_.is_empty()) {
		alpha_strip_models_.insert(this);
		set_notify_transform(true);
	}
	refresh_render_order();
}

// Build the Skeleton3D + rest-derived Skin from the loaded SkeletalAnim.
// [orig: BoneFile_Load @0x40fff0 builds the runtime skeleton.]
void ObjectModel::build_skeleton() {
	skeleton_ = memnew(Skeleton3D);
	skeleton_->set_name("Skeleton3D");
	// This model writes final bone poses directly; MANUAL removes Godot's
	// empty per-frame modifier pass.
	skeleton_->set_modifier_callback_mode_process(
			Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	add_child(skeleton_);
	const Array bones = skeletal_->get_skeleton_bones();
	// The rig is index-driven (row i is bone i; net-re §5.40). Godot refuses
	// empty/duplicate/':'/'/' names; unique placeholders preserve row order.
	HashMap<String, bool> used;
	for (int64_t i = 0; i < bones.size(); ++i) {
		const Dictionary bd = bones[i];
		String n = String(bd.get("name", "")).strip_edges();
		n = n.replace(":", "_").replace("/", "_");
		if (n.is_empty()) {
			n = String("bone_") + String::num_int64(i);
		}
		if (used.has(n)) {
			n = n + "_" + String::num_int64(i);
		}
		used[n] = true;
		skeleton_->add_bone(n);
	}
	for (int64_t i = 0; i < bones.size(); ++i) {
		const Dictionary bd = bones[i];
		const int parent = int(bd.get("parent_index", -1));
		if (parent >= 0 && parent < skeleton_->get_bone_count() && parent != i) {
			skeleton_->set_bone_parent(static_cast<int>(i), parent);
		}
		skeleton_->set_bone_rest(static_cast<int>(i), bd.get("rest", Transform3D()));
	}
	for (int i = 0; i < skeleton_->get_bone_count(); ++i) {
		skeleton_->reset_bone_pose(i);
	}
	skeleton_skin_ = skeleton_->create_skin_from_rest_transforms();
}

// Keep the submission notifier matching the model's current mesh bounds. An
// empty-bounds model draws nothing: it carries no notifier and stays flagged
// on-screen so a later real rebuild starts from the safe default.
void ObjectModel::sync_screen_notifier(const AABB &p_bounds) {
	if (p_bounds.size == Vector3()) {
		if (screen_notifier_ != nullptr) {
			screen_notifier_->queue_free();
			screen_notifier_ = nullptr;
		}
		// Route through the public seam: the safe default must also clear any
		// off-screen claim left in the shared submission registry.
		set_on_screen(true);
		return;
	}
	if (screen_notifier_ == nullptr) {
		screen_notifier_ = memnew(VisibleOnScreenNotifier3D);
		screen_notifier_->set_name("ScreenNotifier");
		screen_notifier_->connect("screen_entered",
				callable_mp(this, &ObjectModel::set_on_screen).bind(true));
		screen_notifier_->connect("screen_exited",
				callable_mp(this, &ObjectModel::set_on_screen).bind(false));
		add_child(screen_notifier_);
	}
	// Grow past the rest bounds: a playing pose can sweep limbs slightly
	// outside the mesh-rest AABB; culling stays conservative.
	screen_notifier_->set_aabb(p_bounds.grow(1.0f));
}

AABB ObjectModel::compute_transformed_mesh_bounds() const {
	AABB bounds;
	bool has_bounds = false;
	const Transform3D model_inverse = get_global_transform().affine_inverse();
	// Static/rigid submeshes hang under their Robj part nodes; skinned (and
	// rigid-fake-skinned) submeshes hang under the shared Skeleton3D. Walk
	// both so a fully-skinned model still reports real bounds.
	Vector<Node3D *> roots;
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		roots.push_back(kv.value);
	}
	if (skeleton_ != nullptr) {
		roots.push_back(skeleton_);
	}
	for (Node3D *node : roots) {
		if (node == nullptr) {
			continue;
		}
		for (int i = 0; i < node->get_child_count(); ++i) {
			MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(node->get_child(i));
			if (instance == nullptr || instance->get_mesh().is_null()) {
				continue;
			}
			const AABB mesh_aabb = instance->get_mesh()->get_aabb();
			if (mesh_aabb.size == Vector3()) {
				continue;
			}
			const AABB local_aabb =
					model_inverse.xform(instance->get_global_transform().xform(mesh_aabb));
			bounds = has_bounds ? bounds.merge(local_aabb) : local_aabb;
			has_bounds = true;
		}
	}
	return bounds;
}

} // namespace godot
