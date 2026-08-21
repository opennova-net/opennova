#include "world/nova_scar_presenter.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <vector>

#include "object/nova_object_model.h"

namespace godot {

namespace {

constexpr const char *kScarShaderPath = "res://shaders/scar_quad.gdshader";
constexpr const char *kWorldMeshName = "ScarWorld";

// One batch row of the draw-list dictionary (Simulation::get_scar_draw_list).
struct BatchRow {
	int owner = 0xFFFF;
	int texture = 0;
	int section = 0;
	bool entity_local = false;
	bool building = false;
	int first = 0;
	int count = 0;
};

// Append one batch's vertex run as a triangle-list surface (the list already
// carries six vertices per quad in the witnessed order, so no index array).
bool append_surface(const Ref<ArrayMesh> &p_mesh, const PackedVector3Array &p_vertices,
		const PackedVector2Array &p_uvs, const PackedColorArray &p_colors,
		const BatchRow &p_row, const Ref<ShaderMaterial> &p_material) {
	if (p_row.count < 3 || p_row.first < 0 ||
			p_row.first + p_row.count > p_vertices.size() ||
			p_row.first + p_row.count > p_uvs.size() ||
			p_row.first + p_row.count > p_colors.size()) {
		return false;
	}
	PackedVector3Array vertices = p_vertices.slice(p_row.first, p_row.first + p_row.count);
	PackedVector2Array uvs = p_uvs.slice(p_row.first, p_row.first + p_row.count);
	PackedColorArray colors = p_colors.slice(p_row.first, p_row.first + p_row.count);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_COLOR] = colors;
	p_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	const int surface = p_mesh->get_surface_count() - 1;
	if (p_material.is_valid()) {
		p_mesh->surface_set_material(surface, p_material);
	}
	return true;
}

} // namespace

ScarPresenter::ScarPresenter() = default;

ScarPresenter::~ScarPresenter() = default;

void ScarPresenter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&ScarPresenter::set_resource_root);
	ClassDB::bind_method(D_METHOD("get_resource_root"), &ScarPresenter::get_resource_root);
	ClassDB::bind_method(D_METHOD("present", "draw_list", "owner_nodes"),
			&ScarPresenter::present);
	ClassDB::bind_method(D_METHOD("clear"), &ScarPresenter::clear);
	ClassDB::bind_method(D_METHOD("get_stats"), &ScarPresenter::get_stats);
}

void ScarPresenter::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		// Entity-ring meshes live under OTHER nodes (the owner models); free
		// them with the presenter so a torn-down pass leaves no scar behind.
		free_entity_meshes_();
	}
}

void ScarPresenter::set_resource_root(const Ref<ResourceRoot> &p_root) {
	if (resource_root_ == p_root) {
		return;
	}
	resource_root_ = p_root;
	// A new root resolves different bytes for the same strip name.
	textures_.clear();
	materials_.clear();
}

Ref<Texture2D> ScarPresenter::texture_(const String &p_name) {
	if (p_name.is_empty()) {
		return Ref<Texture2D>();
	}
	const Ref<Texture2D> *cached = textures_.getptr(p_name);
	if (cached != nullptr) {
		return *cached;
	}
	Ref<Texture2D> texture;
	if (resource_root_.is_valid()) {
		texture = resource_root_->load_texture(p_name);
	}
	textures_[p_name] = texture;
	return texture;
}

Ref<ShaderMaterial> ScarPresenter::material_for_strip_(int p_strip,
		const String &p_texture_name) {
	const Ref<ShaderMaterial> *cached = materials_.getptr(p_strip);
	if (cached != nullptr) {
		return *cached;
	}
	if (shader_.is_null()) {
		shader_ = ResourceLoader::get_singleton()->load(kScarShaderPath, "Shader");
	}
	Ref<ShaderMaterial> material;
	material.instantiate();
	if (shader_.is_valid()) {
		material->set_shader(shader_);
	}
	const Ref<Texture2D> texture = texture_(p_texture_name);
	if (texture.is_valid()) {
		material->set_shader_parameter("albedo_tex", texture);
	} else {
		++stat_textures_missing_;
	}
	materials_[p_strip] = material;
	return material;
}

MeshInstance3D *ScarPresenter::ensure_world_mesh_() {
	if (world_mesh_id_.is_valid()) {
		MeshInstance3D *existing = Object::cast_to<MeshInstance3D>(
				ObjectDB::get_instance(world_mesh_id_));
		if (existing != nullptr) {
			return existing;
		}
	}
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_name(kWorldMeshName);
	// Shared-ring vertices are world space whatever this node's parent does.
	instance->set_as_top_level(true);
	add_child(instance);
	world_mesh_id_ = instance->get_instance_id();
	return instance;
}

void ScarPresenter::free_entity_meshes_() {
	for (const KeyValue<uint32_t, ObjectID> &kv : entity_meshes_) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(kv.value));
		if (node != nullptr) {
			node->queue_free();
		}
	}
	entity_meshes_.clear();
	stat_entity_meshes_ = 0;
}

void ScarPresenter::clear() {
	free_entity_meshes_();
	if (world_mesh_id_.is_valid()) {
		MeshInstance3D *world_mesh = Object::cast_to<MeshInstance3D>(
				ObjectDB::get_instance(world_mesh_id_));
		if (world_mesh != nullptr) {
			world_mesh->set_mesh(Ref<Mesh>());
			world_mesh->set_visible(false);
		}
	}
	stat_world_surfaces_ = 0;
	stat_batches_ = 0;
	stat_vertices_ = 0;
}

void ScarPresenter::present(const Dictionary &p_draw_list, const Dictionary &p_owner_nodes) {
	stat_textures_missing_ = 0;
	const PackedVector3Array vertices = p_draw_list.get("vertices", PackedVector3Array());
	const PackedVector2Array uvs = p_draw_list.get("uvs", PackedVector2Array());
	const PackedColorArray colors = p_draw_list.get("colors", PackedColorArray());
	const PackedInt32Array owners = p_draw_list.get("batch_owner", PackedInt32Array());
	const PackedInt32Array textures = p_draw_list.get("batch_texture", PackedInt32Array());
	const PackedInt32Array sections = p_draw_list.get("batch_section", PackedInt32Array());
	const PackedInt32Array flags = p_draw_list.get("batch_flags", PackedInt32Array());
	const PackedInt32Array firsts = p_draw_list.get("batch_first", PackedInt32Array());
	const PackedInt32Array counts = p_draw_list.get("batch_count", PackedInt32Array());
	const PackedStringArray strip_names =
			p_draw_list.get("strip_names", PackedStringArray());
	const int64_t batch_count = owners.size();
	if (batch_count == 0 || textures.size() != batch_count ||
			sections.size() != batch_count || flags.size() != batch_count ||
			firsts.size() != batch_count || counts.size() != batch_count) {
		clear();
		return;
	}
	stat_batches_ = static_cast<int>(batch_count);
	stat_vertices_ = static_cast<int>(vertices.size());

	// --- the shared ring: one top-level mesh, one surface per batch ---------
	Ref<ArrayMesh> world_mesh;
	world_mesh.instantiate();
	// --- the entity rings: (owner << 8 | section) -> surfaces --------------
	struct EntityGroup {
		int owner = 0;
		int section = 0;
		Ref<ArrayMesh> mesh;
	};
	std::vector<EntityGroup> groups;
	HashMap<uint32_t, size_t> group_index;

	for (int64_t i = 0; i < batch_count; ++i) {
		BatchRow row;
		row.owner = owners[i];
		row.texture = textures[i];
		row.section = sections[i];
		row.entity_local = (flags[i] & 1) != 0;
		row.building = (flags[i] & 2) != 0;
		row.first = firsts[i];
		row.count = counts[i];
		const String texture_name = row.texture >= 0 && row.texture < strip_names.size()
				? strip_names[row.texture]
				: String();
		const Ref<ShaderMaterial> material = material_for_strip_(row.texture, texture_name);
		if (!row.entity_local) {
			append_surface(world_mesh, vertices, uvs, colors, row, material);
			continue;
		}
		const uint32_t key = (static_cast<uint32_t>(row.owner & 0xFFFF) << 8) |
				static_cast<uint32_t>(row.section & 0xFF);
		size_t *found = group_index.getptr(key);
		if (found == nullptr) {
			EntityGroup group;
			group.owner = row.owner;
			group.section = row.section;
			group.mesh.instantiate();
			groups.push_back(group);
			group_index[key] = groups.size() - 1;
			found = group_index.getptr(key);
		}
		append_surface(groups[*found].mesh, vertices, uvs, colors, row, material);
	}

	MeshInstance3D *world_instance = ensure_world_mesh_();
	stat_world_surfaces_ = world_mesh->get_surface_count();
	if (stat_world_surfaces_ > 0) {
		world_instance->set_mesh(world_mesh);
		world_instance->set_visible(true);
	} else {
		world_instance->set_mesh(Ref<Mesh>());
		world_instance->set_visible(false);
	}

	// Reconcile the entity meshes: rebuild the live groups under their owner's
	// section node, drop the ones that produced no batch this frame.
	HashMap<uint32_t, ObjectID> kept;
	for (const EntityGroup &group : groups) {
		if (group.mesh->get_surface_count() == 0) {
			continue;
		}
		const Variant owner_v = p_owner_nodes.get(group.owner, Variant());
		Node3D *owner = Object::cast_to<Node3D>(static_cast<Object *>(owner_v));
		if (owner == nullptr) {
			continue;
		}
		// The struck section's render-part node hosts the section-local quads;
		// a model without that part (or a plain Node3D owner) hosts them at its
		// own origin.
		Node3D *host = owner;
		if (ObjectModel *model = Object::cast_to<ObjectModel>(owner)) {
			const Dictionary parts = model->get_render_part_nodes();
			const Variant part_v = parts.get(group.section, Variant());
			if (Node3D *part = Object::cast_to<Node3D>(static_cast<Object *>(part_v))) {
				host = part;
			}
		}
		const uint32_t key = (static_cast<uint32_t>(group.owner & 0xFFFF) << 8) |
				static_cast<uint32_t>(group.section & 0xFF);
		MeshInstance3D *instance = nullptr;
		if (const ObjectID *existing_id = entity_meshes_.getptr(key)) {
			instance = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(*existing_id));
			if (instance != nullptr && instance->get_parent() != host) {
				instance->queue_free();
				instance = nullptr;
			}
		}
		if (instance == nullptr) {
			instance = memnew(MeshInstance3D);
			instance->set_name(String("Scars_") + String::num_int64(group.owner) +
					"_s" + String::num_int64(group.section));
			host->add_child(instance);
		}
		instance->set_mesh(group.mesh);
		kept[key] = instance->get_instance_id();
	}
	for (const KeyValue<uint32_t, ObjectID> &kv : entity_meshes_) {
		if (kept.has(kv.key)) {
			continue;
		}
		Node *stale = Object::cast_to<Node>(ObjectDB::get_instance(kv.value));
		if (stale != nullptr) {
			stale->queue_free();
		}
	}
	entity_meshes_ = kept;
	stat_entity_meshes_ = static_cast<int>(entity_meshes_.size());
}

Dictionary ScarPresenter::get_stats() const {
	Dictionary out;
	out["world_surfaces"] = stat_world_surfaces_;
	out["entity_meshes"] = stat_entity_meshes_;
	out["batches"] = stat_batches_;
	out["vertices"] = stat_vertices_;
	out["textures_missing"] = stat_textures_missing_;
	return out;
}

} // namespace godot
