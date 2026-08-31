// ObjectModel: retained-scene construction — child teardown, cache
// reset, Skeleton3D/Skin construction, the per-level mesh/material rows and
// the surface slots they swap onto, the submission notifier, and
// transformed-mesh bounds. Ported during the 2026-08-09 de-scripting; the
// one-instance-per-surface-slot scene (every authored RLOD cached as rows,
// never as hidden nodes) dates from 2026-08-30.

#include "object/object_model.h"
#include "render/frame_fx.h"

#include <runtime/renderer/authored_occluder.h>
#include <runtime/simassets/model_builders.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>
#include <cstddef>

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
	level_surfaces_.clear();
	surface_slots_.clear();
	level_bound_visuals_.clear();
	applied_lod_ = -1;
	skeletal_scene_ = false;
	skeleton_ = nullptr;
	skeleton_skin_.unref();
	muzzle_bone_ = -1;
	surface_material_indices_.clear();
	surface_materials_.clear();
	alpha_strip_draws_.clear();
	alpha_strip_models_.erase(this);
	render_order_dirty_ = true;
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
	level_surfaces_.resize(native_model.lod_count);
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
	skeletal_scene_ = skeletal_.is_valid() && skeletal_->is_loaded();
	if (skeletal_scene_) {
		build_skeleton();
		resolve_muzzle_userpoint();
	}
	const int bone_count =
			skeletal_scene_ && skeleton_ != nullptr ? skeleton_->get_bone_count() : 0;
	// Every retained level's submeshes become mesh/material rows (no nodes):
	// the shared meshes come from ObjectData's memo, the materials from this
	// model's per-index cache, and an alpha strip owns its priority-bearing
	// duplicate at every level it appears in.
	std::size_t slot_count = 0;
	for (std::size_t lod_index = first_retained_lod;
			lod_index < retained_lod_end;
			++lod_index) {
		const Array submeshes = object_data_->build_lod_submeshes(
				static_cast<int>(lod_index), skeletal_scene_, bone_count, false);
		authored_lod_available_[lod_index] = !submeshes.is_empty();
		std::vector<LevelSurface> &level = level_surfaces_[lod_index];
		level.reserve(static_cast<std::size_t>(submeshes.size()));
		// A per-vertex skinned model rides retail's bone path, which never
		// collects a Q3 copy (renderer::q3_object_source_admitted).
		const bool q3_admitted = opennova::renderer::q3_object_source_admitted(
				object_data_->is_skinned(static_cast<int>(lod_index)));
		for (int64_t entry = 0; entry < submeshes.size(); ++entry) {
			const Dictionary submesh = submeshes[entry];
			const Ref<ArrayMesh> mesh = submesh.get("mesh", Variant());
			if (mesh.is_null()) {
				continue;
			}
			LevelSurface surface;
			surface.mesh = mesh;
			surface.robj_index =
					int(submesh.get("robj_index", submesh.get("part_index", 0)));
			surface.material_index = int(submesh.get("material_index", 0));
			surface.is_alpha = bool(submesh.get("is_alpha", false));
			// Skinned + rigid-fake-skinned submeshes bind to the shared
			// Skeleton3D; everything else hangs under its Robj part node so
			// PANM part transforms keep working.
			surface.is_skinned = skeletal_scene_ && skeleton_ != nullptr &&
					bool(submesh.get("is_skinned", false));
			surface.q3_admitted = q3_admitted;
			surface.local_center = mesh->get_aabb().get_center();
			Ref<ShaderMaterial> material = material_for_index(surface.material_index);
			// One source submesh is one retail strip. Transparent strips must own
			// their material instance because Godot stores render_priority on the
			// material, while retail chooses Q1/Q2 independently for every strip on
			// every frame (renderer/render_order owns the cited queue contract).
			// Opaque strips may keep sharing their retained
			// material cache entry.
			if (surface.is_alpha && material.is_valid()) {
				const Ref<ShaderMaterial> shared_material = material;
				material = material->duplicate();
				FrameFx::clone_q3_object_material(shared_material, material);
			}
			surface.material = material;
			// Retail multi-pass effects retain one logical material but submit the
			// same strip geometry again. Pair those auxiliary materials through
			// metadata and duplicate geometry only; never duplicate logical rows.
			if (material.is_valid() &&
					material->has_meta("_opennova_postmultiply_material")) {
				surface.auxiliary_material =
						material->get_meta("_opennova_postmultiply_material", Variant());
			}
			// The ROBJ part nodes are the union over every retained level so the
			// dense PANM apply array and the section mask cover a part the
			// moment its level becomes active.
			if (!surface.is_skinned) {
				get_or_create_robj_node(surface.robj_index);
			}
			collect_anim_frames(surface.material_index);
			level.push_back(surface);
		}
		slot_count = std::max(slot_count, level.size());
	}
	// One retained MeshInstance3D per surface slot, parented from the start
	// under the part the finest retained level puts it on (so every stamp walk
	// over the part/skeleton children reaches it); apply_level_surfaces moves
	// it where the active level needs it.
	surface_slots_.resize(slot_count);
	for (std::size_t slot = 0; slot < slot_count; ++slot) {
		MeshInstance3D *instance = memnew(MeshInstance3D);
		++geometry_instance_count_;
		++live_geometry_instance_count_;
		// The stored presentation policy: fresh instances take the layer/cast
		// decision the owner already made, so a rebuild never resets it.
		instance->set_cast_shadows_setting(presentation_cast_setting(false));
		instance->set_layer_mask(presentation_layer_mask(false));
		instance->set_visible(false);
		Node3D *parent = nullptr;
		for (std::size_t lod_index = first_retained_lod;
				lod_index < retained_lod_end && parent == nullptr;
				++lod_index) {
			if (slot < level_surfaces_[lod_index].size()) {
				parent = surface_parent_for(level_surfaces_[lod_index][slot]);
			}
		}
		if (parent == nullptr) {
			parent = this;
		}
		parent->add_child(instance);
		surface_slots_[slot].instance = instance;
	}
	apply_level_surfaces();

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

	apply_runtime_state(0.0);
	// Newly rebuilt GeometryInstance3Ds have default instance uniforms. Keep
	// the stance gate immediately correct (apply_level_surfaces stamped the
	// per-entity lighting factors); the terrain-frame leg supplies the
	// resident page after Terrain has serviced its cache requests.
	stamp_match_terrain_instances(false, 0.0f, Vector4());
	bool any_alpha_strip = false;
	for (const std::vector<LevelSurface> &level : level_surfaces_) {
		for (const LevelSurface &surface : level) {
			any_alpha_strip = any_alpha_strip || surface.is_alpha;
		}
	}
	if (any_alpha_strip) {
		alpha_strip_models_.insert(this);
		set_notify_transform(true);
	}
	refresh_render_order();
}

Node3D *ObjectModel::surface_parent_for(const LevelSurface &p_surface) {
	if (p_surface.is_skinned && skeleton_ != nullptr) {
		return skeleton_;
	}
	return get_or_create_robj_node(p_surface.robj_index);
}

int ObjectModel::get_level_surface_count(int p_lod_index) const {
	if (p_lod_index < 0 ||
			static_cast<std::size_t>(p_lod_index) >= level_surfaces_.size()) {
		return 0;
	}
	return static_cast<int>(level_surfaces_[p_lod_index].size());
}

// Swap the active level onto the retained slots. Each slot keeps its node
// (and with it the per-instance uniforms, the layer/cast policy and the
// capture stamps other devices hold); only what the level decides moves:
// the mesh, the material, the part/skeleton parent, the skin binding, the
// Q3 source registration and the auxiliary pair. Nothing is created here
// except a first-seen auxiliary instance, and nothing is freed.
void ObjectModel::apply_level_surfaces() {
	if (applied_lod_ == active_lod_) {
		return;
	}
	const std::vector<LevelSurface> *level =
			active_lod_ >= 0 &&
					static_cast<std::size_t>(active_lod_) < level_surfaces_.size()
			? &level_surfaces_[active_lod_]
			: nullptr;
	const std::size_t live_count = level != nullptr ? level->size() : 0;
	surface_material_indices_.clear();
	surface_materials_.clear();
	alpha_strip_draws_.clear();
	const auto reparent = [](MeshInstance3D *p_instance, Node3D *p_parent) {
		Node *current = p_instance->get_parent();
		if (current == p_parent) {
			return;
		}
		if (current != nullptr) {
			current->remove_child(p_instance);
		}
		p_parent->add_child(p_instance);
	};
	const auto bind_skin = [&](MeshInstance3D *p_instance, bool p_skinned) {
		if (p_skinned && skeleton_ != nullptr) {
			p_instance->set_skin(skeleton_skin_);
			p_instance->set_skeleton_path(p_instance->get_path_to(skeleton_));
		} else {
			p_instance->set_skin(Ref<Skin>());
			p_instance->set_skeleton_path(NodePath());
		}
	};
	for (std::size_t slot_index = 0; slot_index < surface_slots_.size();
			++slot_index) {
		SurfaceSlot &slot = surface_slots_[slot_index];
		MeshInstance3D *instance = slot.instance;
		if (instance == nullptr) {
			continue;
		}
		if (slot_index >= live_count) {
			// The active level has no submesh for this slot: parked, still
			// stamped, never drawn.
			instance->set_visible(false);
			FrameFx::unregister_q3_source(instance);
			if (slot.auxiliary != nullptr) {
				slot.auxiliary->set_visible(false);
			}
			continue;
		}
		const LevelSurface &surface = (*level)[slot_index];
		Node3D *parent = surface_parent_for(surface);
		reparent(instance, parent);
		if (instance->get_mesh() != surface.mesh) {
			instance->set_mesh(surface.mesh);
		}
		if (instance->get_material_override() != surface.material) {
			instance->set_material_override(surface.material);
		}
		bind_skin(instance, surface.is_skinned);
		instance->set_visible(true);
		// The level's collector decides the Q3 copy (never a per-vertex
		// skinned level); the registration follows the material's glow
		// capability and re-reads the swapped mesh once.
		if (surface.q3_admitted) {
			FrameFx::register_q3_object_source(instance, surface.material);
		} else {
			FrameFx::unregister_q3_source(instance);
		}
		if (surface.auxiliary_material.is_valid()) {
			MeshInstance3D *auxiliary = slot.auxiliary;
			if (auxiliary == nullptr) {
				auxiliary = memnew(MeshInstance3D);
				++geometry_instance_count_;
				++live_geometry_instance_count_;
				auxiliary->set_name("PostMultiply");
				auxiliary->set_cast_shadows_setting(presentation_cast_setting(true));
				auxiliary->set_layer_mask(presentation_layer_mask(true));
				auxiliary->set_meta("_opennova_auxiliary_draw", true);
				parent->add_child(auxiliary);
				slot.auxiliary = auxiliary;
				// Minted between the terrain-frame and viewmodel legs: it
				// takes the retained page binding and pass flag now rather
				// than drawing with default uniforms until the next leg.
				stamp_instance_uniforms(auxiliary);
			}
			reparent(auxiliary, parent);
			if (auxiliary->get_mesh() != surface.mesh) {
				auxiliary->set_mesh(surface.mesh);
			}
			if (auxiliary->get_material_override() != surface.auxiliary_material) {
				auxiliary->set_material_override(surface.auxiliary_material);
			}
			bind_skin(auxiliary, surface.is_skinned);
			auxiliary->set_visible(true);
		} else if (slot.auxiliary != nullptr) {
			slot.auxiliary->set_visible(false);
		}
		surface_material_indices_.append(surface.material_index);
		surface_materials_.push_back(surface.material);
		if (surface.is_alpha && surface.material.is_valid()) {
			AlphaStripDraw draw;
			draw.instance = instance;
			draw.material = surface.material;
			draw.local_center = surface.local_center;
			draw.bone_path = surface.is_skinned;
			alpha_strip_draws_.push_back(draw);
		}
	}
	applied_lod_ = active_lod_;
	for (std::size_t i = 0; i < level_bound_visuals_.size();) {
		VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(
				ObjectDB::get_instance(level_bound_visuals_[i].id));
		if (visual == nullptr) {
			level_bound_visuals_.erase(level_bound_visuals_.begin() +
					static_cast<std::ptrdiff_t>(i));
			continue;
		}
		visual->set_visible(level_bound_visuals_[i].lod_index == active_lod_);
		++i;
	}
	classify_materials();
	// A slot that moved between parts carries the last selection written for
	// its previous part: the lighting factors are re-stamped now and the
	// point-light hash gate reopened so the next selection lands.
	stamp_entity_lighting_instances();
}

void ObjectModel::add_level_bound_visual(int p_lod_index,
		VisualInstance3D *p_visual) {
	if (p_visual == nullptr) {
		return;
	}
	LevelBoundVisual bound;
	bound.id = p_visual->get_instance_id();
	bound.lod_index = p_lod_index;
	level_bound_visuals_.push_back(bound);
	p_visual->set_visible(p_lod_index == active_lod_);
}

void ObjectModel::harvest_level_surfaces(Vector<HarvestedSurface> &r_rows) {
	r_rows.clear();
	if (object_data_.is_null() || !object_data_->has_document()) {
		return;
	}
	const int original_lod = active_lod_;
	for (std::size_t lod_index = 0; lod_index < level_surfaces_.size();
			++lod_index) {
		const int lod = static_cast<int>(lod_index);
		if (level_surfaces_[lod_index].empty()) {
			continue;
		}
		if (lod != active_lod_) {
			if (!authored_lod_enabled_) {
				continue;
			}
			// Posing the level writes that level's ROBJ base pose (the same
			// pose an individual model shows at the level).
			set_active_lod(lod);
			if (active_lod_ != lod) {
				continue;
			}
		}
		for (const LevelSurface &surface : level_surfaces_[lod_index]) {
			if (surface.is_skinned || surface.mesh.is_null()) {
				continue;
			}
			Node3D *const *part = robj_nodes_.getptr(surface.robj_index);
			HarvestedSurface row;
			row.mesh = surface.mesh;
			row.material = surface.material;
			row.offset = part != nullptr && *part != nullptr
					? (*part)->get_transform()
					: Transform3D();
			row.robj_index = surface.robj_index;
			row.lod_index = lod;
			row.blended_draw = surface.is_alpha;
			r_rows.push_back(row);
			if (surface.auxiliary_material.is_valid()) {
				HarvestedSurface auxiliary = row;
				auxiliary.material = surface.auxiliary_material;
				auxiliary.auxiliary_draw = true;
				r_rows.push_back(auxiliary);
			}
		}
	}
	if (active_lod_ != original_lod) {
		set_active_lod(original_lod);
	}
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
