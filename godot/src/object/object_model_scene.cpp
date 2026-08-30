// ObjectModel: retained-scene construction — child teardown, cache
// reset, Skeleton3D/Skin construction, mesh/material assembly, the
// submission notifier, and transformed-mesh bounds. Ported during the
// 2026-08-09 de-scripting.

#include "object/object_model.h"
#include "render/frame_fx.h"

#include <runtime/renderer/authored_occluder.h>
#include <runtime/simassets/model_builders.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>

namespace godot {

void ObjectModel::rebuild_scene() {
	// Every retained child is recreated below: per-instance stamps other
	// devices hold on this subtree (slot-capture layers) are stale from here.
	++scene_build_serial_;
	while (get_child_count() > 0) {
		Node *child = get_child(0);
		remove_child(child);
		child->queue_free();
	}
	retire_geometry_instances();
	// The submission notifier died in the sweep above; set_model_bounds
	// recreates it against the rebuilt bounds (even equal ones).
	screen_notifier_ = nullptr;
	robj_nodes_.clear();
	robj_rest_transforms_.clear();
	authored_occluders_.clear();
	authored_lod_thresholds_q16_.clear();
	authored_lod_available_.clear();
	authored_lod_models_.erase(this);
	skeleton_ = nullptr;
	skeleton_skin_.unref();
	muzzle_bone_ = -1;
	surface_material_indices_.clear();
	surface_materials_.clear();
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
	const Threedi3di3 &native_model = object_data_->native_model();
	authored_lod_thresholds_q16_.reserve(native_model.lod_count);
	authored_lod_available_.resize(native_model.lod_count, false);
	const std::size_t first_retained_lod = authored_lod_enabled_
			? 0
			: static_cast<std::size_t>(active_lod_);
	const std::size_t retained_lod_end = authored_lod_enabled_
			? native_model.lod_count
			: std::min(native_model.lod_count, first_retained_lod + 1);
	for (std::size_t lod_index = first_retained_lod;
			lod_index < retained_lod_end;
			++lod_index) {
		authored_lod_thresholds_q16_.push_back(
				native_model.lods[lod_index].lod_threshold);
	}
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
	for (std::size_t lod_index = first_retained_lod;
			lod_index < retained_lod_end;
			++lod_index) {
		const Array submeshes = object_data_->build_lod_submeshes(
				static_cast<int>(lod_index), skeletal_mode, bone_count, false);
		authored_lod_available_[lod_index] = !submeshes.is_empty();
		for (int64_t entry = 0; entry < submeshes.size(); ++entry) {
			const Dictionary submesh = submeshes[entry];
			const Ref<ArrayMesh> mesh = submesh.get("mesh", Variant());
			if (mesh.is_null()) {
				continue;
			}
			const int robj_index =
					int(submesh.get("robj_index", submesh.get("part_index", 0)));
			const int material_index = int(submesh.get("material_index", 0));
			if (static_cast<int>(lod_index) == active_lod_ &&
					!robj_rest_transforms_.has(robj_index)) {
				robj_rest_transforms_[robj_index] =
						Transform3D(Basis(), submesh.get("abs", Vector3()));
			}
			MeshInstance3D *instance = memnew(MeshInstance3D);
			++geometry_instance_count_;
			instance->set_mesh(mesh);
			instance->set_meta("_opennova_lod_index",
					static_cast<int64_t>(lod_index));
			instance->set_visible(static_cast<int>(lod_index) == active_lod_);
			// The stored presentation policy: fresh instances take the layer/cast
			// decision the owner already made, so a rebuild never resets it.
			instance->set_cast_shadows_setting(presentation_cast_setting(false));
			instance->set_layer_mask(presentation_layer_mask(false));
			Ref<ShaderMaterial> material = material_for_index(material_index);
			// One source submesh is one retail strip. Transparent strips must own
			// their material instance because Godot stores render_priority on the
			// material, while retail chooses Q1/Q2 independently for every strip on
			// every frame (renderer/render_order owns the cited queue contract).
			// Opaque strips may keep sharing their retained
			// material cache entry.
			if (bool(submesh.get("is_alpha", false)) && material.is_valid()) {
				const Ref<ShaderMaterial> shared_material = material;
				material = material->duplicate();
				FrameFx::clone_q3_object_material(shared_material, material);
			}
			instance->set_material_override(material);
			// A per-vertex skinned model rides retail's bone path, which never
			// collects a Q3 copy (renderer::q3_object_source_admitted).
			if (opennova::renderer::q3_object_source_admitted(
						object_data_->is_skinned(static_cast<int>(lod_index)))) {
				FrameFx::register_q3_object_source(instance, material);
			}
			const bool blended_draw = bool(submesh.get("is_alpha", false));
			if (blended_draw) {
				// Static harvesting keeps retail's independently ordered alpha strips
				// in one global population instead of assigning them to spatial
				// MultiMeshes whose changing centers would perturb Q1/Q2 ordering.
				instance->set_meta("_opennova_blended_draw", true);
			}
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
				draw.bone_path =
						skeletal_mode && bool(submesh.get("is_skinned", false));
				alpha_strip_draws_.push_back(draw);
			}
			// Retail multi-pass effects retain one logical material but submit the
			// same strip geometry again. Pair those auxiliary materials through
			// metadata and duplicate geometry only; never duplicate logical rows.
			auto add_auxiliary_draw = [&](const StringName &p_material_meta,
											  const String &p_name,
											  const StringName &p_kind_meta) {
				if (material.is_null() || !material->has_meta(p_material_meta)) {
					return;
				}
				const Ref<ShaderMaterial> auxiliary_material =
						material->get_meta(p_material_meta, Variant());
				if (auxiliary_material.is_null()) {
					return;
				}
				MeshInstance3D *auxiliary_instance = memnew(MeshInstance3D);
				++geometry_instance_count_;
				auxiliary_instance->set_name(p_name);
				auxiliary_instance->set_mesh(mesh);
				auxiliary_instance->set_material_override(auxiliary_material);
				auxiliary_instance->set_cast_shadows_setting(
						presentation_cast_setting(true));
				auxiliary_instance->set_layer_mask(presentation_layer_mask(true));
				auxiliary_instance->set_meta("_opennova_auxiliary_draw", true);
				auxiliary_instance->set_meta("_opennova_lod_index",
						static_cast<int64_t>(lod_index));
				auxiliary_instance->set_visible(static_cast<int>(lod_index) ==
						active_lod_);
				if (blended_draw) {
					auxiliary_instance->set_meta("_opennova_blended_draw", true);
				}
				auxiliary_instance->set_meta(p_kind_meta, true);
				Node *surface_parent = instance->get_parent();
				if (surface_parent == nullptr) {
					memdelete(auxiliary_instance);
					--geometry_instance_count_;
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
			collect_anim_frames(material_index);
		}
	}

	refresh_active_lod_rest_transforms();
	if (authored_lod_enabled_ && authored_lod_thresholds_q16_.size() > 1) {
		authored_lod_models_.insert(this);
	}

	if (authored_occluders_enabled_) {
		opennova::world::OcclusionModel occlusion_model;
		if (opennova::simassets::occlusion_model_from_3di(native_model,
					occlusion_model)) {
			const std::vector<opennova::renderer::AuthoredOccluderSection> sections =
					opennova::renderer::build_authored_occluder_sections(occlusion_model);
			for (const opennova::renderer::AuthoredOccluderSection &section :
					sections) {
				PackedVector3Array vertices;
				vertices.resize(static_cast<int64_t>(section.vertices.size()));
				for (std::size_t i = 0; i < section.vertices.size(); ++i) {
					const opennova::renderer::OccluderVertex &vertex =
							section.vertices[i];
					vertices.set(static_cast<int64_t>(i),
							Vector3(-vertex.x, vertex.y, vertex.z));
				}
				PackedInt32Array indices;
				indices.resize(static_cast<int64_t>(section.indices.size()));
				for (std::size_t i = 0; i < section.indices.size(); ++i) {
					indices.set(static_cast<int64_t>(i), section.indices[i]);
				}
				Ref<ArrayOccluder3D> occluder;
				occluder.instantiate();
				occluder->set_arrays(vertices, indices);
				OccluderInstance3D *instance = memnew(OccluderInstance3D);
				instance->set_name(String("AuthoredOccluder_Section") +
						String::num_int64(section.section));
				instance->set_occluder(occluder);
				instance->set_visible(
						section_visibility_mask_ == -1 ||
						(section.section < 63 &&
								((static_cast<uint64_t>(section_visibility_mask_) >>
										 section.section) &
										1u) != 0));
				add_child(instance);
				authored_occluders_[section.section] = instance;
			}
		}
	}
	// Whether Godot's occlusion consumer runs is the world's decision
	// (GameWorld reads get_authored_occluder_count after placement); a model
	// never flips its viewport's state.

	live_geometry_instance_count_ += geometry_instance_count_;

	classify_materials();
	apply_runtime_state(0.0);
	// Newly rebuilt GeometryInstance3Ds have default instance uniforms. Keep
	// the stance gate and the per-entity lighting factors immediately correct;
	// the terrain-frame leg supplies the resident page after Terrain has
	// serviced its cache requests.
	stamp_match_terrain_instances(false, 0.0f, Vector4());
	stamp_entity_lighting_instances();
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
		// Route through the public seam so the on-screen edge wakes the
		// runtime frame like a real notifier would.
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
			MeshInstance3D *instance =
					Object::cast_to<MeshInstance3D>(node->get_child(i));
			if (instance == nullptr || !instance->is_visible() ||
					instance->get_mesh().is_null()) {
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
