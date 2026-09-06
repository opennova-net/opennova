#include "model/model_scene_exporter.h"

#include "model/model_bounding_volume_3d.h"
#include "model/model_collision_section_3d.h"
#include "model/model_frames.h"
#include "model/model_light_3d.h"
#include "model/model_user_point_3d.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/plane.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <formats/threedi/threedi_scene_names.h>
#include <runtime/renderer/material_descriptor.h>

using namespace godot;
using namespace opennova::threedi;

namespace {

constexpr int kMaxStripBones = 16;

std::string std_name(const Node *p_node) {
	return opennova::to_std(String(p_node->get_name()));
}

// The transform of `p_node` relative to `p_ancestor` (both in one tree),
// composed from the ancestor DOWN so a chain of pivots sums the way the
// file's ROBJ abs values do (abs = parent abs + rel, left to right).
Transform3D relative_transform(const Node3D *p_node, const Node *p_ancestor) {
	std::vector<const Node3D *> chain;
	const Node *cursor = p_node;
	while (cursor != nullptr && cursor != p_ancestor) {
		const Node3D *spatial = Object::cast_to<Node3D>(cursor);
		if (spatial != nullptr) {
			chain.push_back(spatial);
		}
		cursor = cursor->get_parent();
	}
	Transform3D out;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		out = out * (*it)->get_transform();
	}
	return out;
}

// A RGB float custom channel of `count` triples, or empty.
PackedFloat32Array exact_channel(const Array &p_arrays, int p_index, int64_t p_count) {
	const Variant value = p_arrays[p_index];
	if (value.get_type() != Variant::PACKED_FLOAT32_ARRAY) {
		return PackedFloat32Array();
	}
	const PackedFloat32Array channel = value;
	return channel.size() == p_count * 3 ? channel : PackedFloat32Array();
}

bool parse_face_surface_name(const String &p_name, int &r_poly_type, uint32_t &r_material_flags) {
	// "pt<n>_mf<hex>"
	if (!p_name.begins_with("pt")) return false;
	const int underscore = p_name.find("_mf");
	if (underscore < 0) return false;
	const String poly = p_name.substr(2, underscore - 2);
	const String flags = p_name.substr(underscore + 3);
	if (!poly.is_valid_int() || flags.is_empty()) return false;
	r_poly_type = poly.to_int();
	r_material_flags = static_cast<uint32_t>(flags.hex_to_int());
	return true;
}

String material_key(MeshInstance3D *p_mesh, const Ref<Mesh> &p_resource, int p_surface) {
	Ref<Material> override_material = p_mesh->get_surface_override_material(p_surface);
	if (override_material.is_valid() && !override_material->get_name().is_empty()) {
		return override_material->get_name();
	}
	Ref<Material> material = p_resource->surface_get_material(p_surface);
	if (material.is_valid() && !material->get_name().is_empty()) {
		return material->get_name();
	}
	const Ref<ArrayMesh> array_mesh = p_resource;
	if (array_mesh.is_valid()) {
		return array_mesh->surface_get_name(p_surface);
	}
	return String();
}

} // namespace

void ModelSceneExporter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("export_scene", "root", "manifest"), &ModelSceneExporter::export_scene);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ModelSceneExporter::get_last_error);
	ClassDB::bind_method(D_METHOD("get_errors"), &ModelSceneExporter::get_errors);
}

void ModelSceneExporter::fail(const String &p_what) {
	errors_.push_back(p_what);
}

Ref<ModelDocument> ModelSceneExporter::export_scene(Node3D *p_root, const Ref<ModelAuthoringManifest> &p_manifest) {
	errors_.clear();
	last_error_ = String();
	if (p_root == nullptr) {
		fail("no scene root");
	}
	if (p_manifest.is_null()) {
		fail("no manifest");
	}
	if (!errors_.is_empty()) {
		last_error_ = String(", ").join(errors_);
		return Ref<ModelDocument>();
	}
	ThreediBuildModel model;
	model.name = opennova::to_std(p_manifest->get_model_name());
	if (model.name.empty() || model.name.size() > 16) {
		fail("the manifest's model_name is empty or longer than 16 characters");
	}
	model.skinned = p_manifest->get_skinned();
	model.matrix_count = p_manifest->get_matrix_count();

	// Materials: the manifest rows in order; the tangent vertex format follows
	// the shader tags (the TANGENT bit of any slot's descriptor row selects the
	// Extended variant: the witnessed rule beside the engine's descriptor table,
	// docs/threedi/3di-gp-format-re.md).
	const TypedArray<ModelMaterialSpec> specs = p_manifest->get_materials();
	for (int i = 0; i < specs.size(); ++i) {
		const Ref<ModelMaterialSpec> spec = specs[i];
		if (spec.is_null()) {
			fail("material row " + String::num_int64(i) + " is empty");
			continue;
		}
		ThreediMaterial material;
		String error;
		if (!spec->write(material, error)) {
			fail(error);
			continue;
		}
		material.index = i;
		const std::string tag = opennova::to_std(spec->get_shader_tag());
		const opennova::renderer::MaterialDescriptorRecord *descriptor =
				opennova::renderer::find_material_descriptor(tag);
		if (descriptor == nullptr) {
			fail("material '" + spec->get_material_name() + "': unknown shader tag " + spec->get_shader_tag());
			continue;
		}
		if ((descriptor->shader_flags & opennova::renderer::MATERIAL_FLAG_TANGENT) != 0) {
			model.tangents = true;
		}
		model.materials.push_back(material);
	}
	const PackedStringArray registers = p_manifest->get_control_registers();
	for (int i = 0; i < registers.size(); ++i) {
		model.add_control_register(opennova::to_std(registers[i]).c_str());
	}

	// LODs: the "LOD<n>" children, contiguous from 0.
	std::map<int, Node3D *> lod_nodes;
	Node3D *collision = nullptr;
	Node3D *points = nullptr;
	Node3D *lights = nullptr;
	for (int i = 0; i < p_root->get_child_count(); ++i) {
		Node3D *child = Object::cast_to<Node3D>(p_root->get_child(i));
		if (child == nullptr) continue;
		const std::string name = std_name(child);
		int lod_index = 0;
		if (threedi_scene_parse_lod_name(name, lod_index)) {
			if (lod_nodes.count(lod_index) != 0) {
				fail("two nodes name LOD" + String::num_int64(lod_index));
			}
			lod_nodes[lod_index] = child;
		} else if (name == "Collision") {
			collision = child;
		} else if (name == "UserPoints") {
			points = child;
		} else if (name == "Lights") {
			lights = child;
		}
	}
	if (lod_nodes.empty()) {
		fail("no LOD0 node under the root");
	}
	int expected = 0;
	for (const auto &entry : lod_nodes) {
		if (entry.first != expected) {
			fail("LOD nodes are not contiguous from LOD0");
			break;
		}
		++expected;
	}
	if (!errors_.is_empty()) {
		last_error_ = String(", ").join(errors_);
		return Ref<ModelDocument>();
	}
	int lod0_part_count = 0;
	std::vector<int> lod0_part_parents;
	for (const auto &entry : lod_nodes) {
		int part_count = 0;
		if (!export_lod(entry.second, entry.first, p_manifest, model, part_count)) {
			break;
		}
		if (entry.first == 0) {
			lod0_part_count = part_count;
			for (const ThreediBuildPart &part : model.lods[0].parts) {
				lod0_part_parents.push_back(part.parent);
			}
		}
	}
	if (errors_.is_empty() && collision != nullptr) {
		export_collision(collision, lod0_part_count, lod0_part_parents, model);
	}
	if (errors_.is_empty() && points != nullptr) {
		export_user_points(points, model);
	}
	if (errors_.is_empty() && lights != nullptr) {
		export_lights(lights, model);
	}
	if (errors_.is_empty() && p_manifest->get_expected_bone_rows() > 0 &&
			lod0_part_count != p_manifest->get_expected_bone_rows()) {
		fail("the rig carries " + String::num_int64(lod0_part_count) + " rows; the manifest expects " +
				String::num_int64(p_manifest->get_expected_bone_rows()));
	}
	if (!errors_.is_empty()) {
		last_error_ = String(", ").join(errors_);
		return Ref<ModelDocument>();
	}
	std::unique_ptr<ThreediAssembled> assembled(new ThreediAssembled());
	threedi_build_assemble(model, *assembled);
	assembled->model.version = static_cast<uint32_t>(p_manifest->get_version());
	Ref<ModelDocument> document;
	document.instantiate();
	document->adopt_assembled(std::move(assembled));
	return document;
}

bool ModelSceneExporter::collect_static_parts(Node3D *p_lod_node, std::vector<PartRef> &r_parts) {
	// Every PN## descendant; the parent part is the nearest PN## ancestor.
	std::map<int, PartRef> found;
	std::vector<Node *> stack;
	stack.push_back(p_lod_node);
	while (!stack.empty()) {
		Node *node = stack.back();
		stack.pop_back();
		for (int i = 0; i < node->get_child_count(); ++i) {
			Node *child = node->get_child(i);
			stack.push_back(child);
			Node3D *spatial = Object::cast_to<Node3D>(child);
			if (spatial == nullptr) continue;
			const std::string name = std_name(spatial);
			if (threedi_scene_has_dedup_suffix(name)) {
				fail("node '" + String(spatial->get_name()) + "' carries a DCC dedup suffix");
				return false;
			}
			int index = 0;
			if (!threedi_scene_parse_part_name(name, index)) continue;
			if (found.count(index) != 0) {
				fail("two nodes name " + String(spatial->get_name()));
				return false;
			}
			PartRef ref;
			ref.node = spatial;
			ref.global = relative_transform(spatial, p_lod_node);
			ref.parent = index; // the root names itself; refined below
			found[index] = ref;
		}
	}
	int expected = 0;
	for (const auto &entry : found) {
		if (entry.first != expected) {
			fail("part nodes are not contiguous from PN01 (missing PN" + String::num_int64(expected + 1).pad_zeros(2) + ")");
			return false;
		}
		++expected;
	}
	r_parts.clear();
	for (const auto &entry : found) {
		PartRef ref = entry.second;
		const Node *cursor = ref.node->get_parent();
		const Node *parent_node = p_lod_node;
		while (cursor != nullptr && cursor != p_lod_node) {
			int parent_index = 0;
			if (threedi_scene_parse_part_name(std_name(cursor), parent_index)) {
				ref.parent = parent_index;
				parent_node = cursor;
				break;
			}
			cursor = cursor->get_parent();
		}
		ref.rel = relative_transform(ref.node, parent_node).origin;
		r_parts.push_back(ref);
	}
	return true;
}

bool ModelSceneExporter::collect_skinned_parts(Node3D *p_lod_node, Skeleton3D *&r_skeleton,
		std::vector<PartRef> &r_parts) {
	r_skeleton = nullptr;
	for (int i = 0; i < p_lod_node->get_child_count(); ++i) {
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_lod_node->get_child(i));
		if (skeleton != nullptr) {
			if (r_skeleton != nullptr) {
				fail("more than one Skeleton3D under " + String(p_lod_node->get_name()));
				return false;
			}
			r_skeleton = skeleton;
		}
	}
	if (r_skeleton == nullptr) {
		fail("a skinned model needs a Skeleton3D under " + String(p_lod_node->get_name()));
		return false;
	}
	const Transform3D skeleton_global = relative_transform(r_skeleton, p_lod_node);
	std::map<int, int> part_to_bone;
	for (int b = 0; b < r_skeleton->get_bone_count(); ++b) {
		const std::string name = opennova::to_std(r_skeleton->get_bone_name(b));
		int index = 0;
		if (!threedi_scene_parse_bone_name(name, index)) {
			fail("bone '" + r_skeleton->get_bone_name(b) + "' is not named BN##");
			return false;
		}
		if (part_to_bone.count(index) != 0) {
			fail("two bones name BN" + String::num_int64(index + 1).pad_zeros(2));
			return false;
		}
		part_to_bone[index] = b;
	}
	int expected = 0;
	for (const auto &entry : part_to_bone) {
		if (entry.first != expected) {
			fail("bones are not contiguous from BN01 (missing BN" + String::num_int64(expected + 1).pad_zeros(2) + ")");
			return false;
		}
		++expected;
	}
	r_parts.clear();
	for (const auto &entry : part_to_bone) {
		PartRef ref;
		ref.node = r_skeleton;
		ref.global = skeleton_global * r_skeleton->get_bone_global_rest(entry.second);
		const int parent_bone = r_skeleton->get_bone_parent(entry.second);
		ref.parent = entry.first;
		ref.rel = ref.global.origin;
		if (parent_bone >= 0) {
			int parent_index = 0;
			threedi_scene_parse_bone_name(opennova::to_std(r_skeleton->get_bone_name(parent_bone)), parent_index);
			ref.parent = parent_index;
			ref.rel = skeleton_global.basis.xform(r_skeleton->get_bone_rest(entry.second).origin);
		}
		r_parts.push_back(ref);
	}
	return true;
}

bool ModelSceneExporter::export_lod(Node3D *p_lod_node, int p_lod_index, const Ref<ModelAuthoringManifest> &p_manifest,
		ThreediBuildModel &r_model, int &r_part_count) {
	int threshold = 0;
	std::string type = "gnrc";
	Ref<ModelLodSpec> lod_spec;
	const TypedArray<ModelLodSpec> lods = p_manifest->get_lods();
	if (p_lod_index < lods.size()) {
		lod_spec = lods[p_lod_index];
	}
	if (lod_spec.is_valid()) {
		threshold = lod_spec->get_threshold();
		type = opennova::to_std(lod_spec->get_model_type());
	}
	const int lod = r_model.add_lod(threshold, type.c_str());
	std::vector<PartRef> parts;
	Skeleton3D *skeleton = nullptr;
	if (r_model.skinned) {
		if (!collect_skinned_parts(p_lod_node, skeleton, parts)) return false;
	} else {
		if (!collect_static_parts(p_lod_node, parts)) return false;
	}
	if (parts.empty()) {
		fail(String(p_lod_node->get_name()) + ": no parts (PN## nodes or BN## bones)");
		return false;
	}
	for (size_t pi = 0; pi < parts.size(); ++pi) {
		const PartRef &ref = parts[pi];
		const int parent = ref.parent < 0 ? static_cast<int>(pi) : ref.parent;
		const ThreediBuildVec3 rel = mission_from_presentation(ref.rel);
		r_model.add_part(lod, parent, mission_from_presentation(ref.global.origin), &rel);
	}
	r_part_count = static_cast<int>(parts.size());
	if (lod_spec.is_valid()) {
		const TypedArray<ModelPartAnimationRow> rows = lod_spec->get_part_animations();
		for (int i = 0; i < rows.size(); ++i) {
			const Ref<ModelPartAnimationRow> row = rows[i];
			ThreediPartAnimation record;
			if (row.is_null() || !row->write(record)) {
				fail(String(p_lod_node->get_name()) + ": PANM row " + String::num_int64(i) +
						" is empty or a track is not five words");
				return false;
			}
			r_model.lods[lod].panm.push_back(record);
		}
	}

	// Meshes: "## Mesh<n>" MeshInstance3D nodes; static ones under their PN##
	// node, skinned ones under the Skeleton3D. Walk the LOD subtree once and
	// sort by (part, mesh ordinal) so the strip order is the file's.
	struct MeshRef {
		MeshInstance3D *node;
		int part;
		int ordinal;
	};
	std::vector<MeshRef> meshes;
	std::vector<Node *> stack;
	stack.push_back(p_lod_node);
	while (!stack.empty()) {
		Node *node = stack.back();
		stack.pop_back();
		for (int i = 0; i < node->get_child_count(); ++i) {
			Node *child = node->get_child(i);
			stack.push_back(child);
			MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(child);
			if (mesh == nullptr) continue;
			const std::string name = std_name(mesh);
			int part = 0, ordinal = 0;
			if (!threedi_scene_parse_mesh_name(name, part, ordinal)) {
				fail("mesh node '" + String(mesh->get_name()) + "' is not named \"## Mesh<n>\"");
				return false;
			}
			if (part < 0 || part >= static_cast<int>(parts.size())) {
				fail("mesh node '" + String(mesh->get_name()) + "' names a part the LOD does not have");
				return false;
			}
			meshes.push_back(MeshRef{mesh, part, ordinal});
		}
	}
	std::stable_sort(meshes.begin(), meshes.end(), [](const MeshRef &a, const MeshRef &b) {
		return a.part != b.part ? a.part < b.part : a.ordinal < b.ordinal;
	});
	for (const MeshRef &ref : meshes) {
		const Transform3D mesh_global = relative_transform(ref.node, p_lod_node);
		if (!export_mesh(ref.node, ref.part, mesh_global, skeleton, parts, lod, p_manifest, r_model)) {
			return false;
		}
	}
	return true;
}

bool ModelSceneExporter::export_mesh(MeshInstance3D *p_mesh, int p_part, const Transform3D &p_mesh_global,
		Skeleton3D *p_skeleton, const std::vector<PartRef> &p_parts, int p_lod,
		const Ref<ModelAuthoringManifest> &p_manifest, ThreediBuildModel &r_model) {
	const Ref<Mesh> resource = p_mesh->get_mesh();
	if (resource.is_null()) {
		fail("mesh node '" + String(p_mesh->get_name()) + "' has no mesh");
		return false;
	}
	// Skin binds -> part indices (skinned meshes only).
	std::vector<int> bind_to_part;
	if (r_model.skinned) {
		const Ref<Skin> skin = p_mesh->get_skin();
		if (skin.is_null() || p_skeleton == nullptr) {
			fail("skinned mesh '" + String(p_mesh->get_name()) + "' has no Skin");
			return false;
		}
		for (int b = 0; b < skin->get_bind_count(); ++b) {
			int bone = skin->get_bind_bone(b);
			if (bone < 0) {
				bone = p_skeleton->find_bone(skin->get_bind_name(b));
			}
			int part = -1;
			if (bone >= 0) {
				threedi_scene_parse_bone_name(opennova::to_std(p_skeleton->get_bone_name(bone)), part);
			}
			if (part < 0) {
				fail("skinned mesh '" + String(p_mesh->get_name()) + "': bind " + String::num_int64(b) +
						" resolves to no BN## bone");
				return false;
			}
			bind_to_part.push_back(part);
		}
	}
	const Basis normal_basis = p_mesh_global.basis;
	for (int s = 0; s < resource->get_surface_count(); ++s) {
		const Ref<ArrayMesh> array_mesh = resource;
		if (array_mesh.is_valid() && array_mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
			fail("mesh '" + String(p_mesh->get_name()) + "' surface " + String::num_int64(s) + " is not a triangle list");
			return false;
		}
		const String key = material_key(p_mesh, resource, s);
		const int material_index = p_manifest->find_material_index(key);
		if (material_index < 0) {
			fail("mesh '" + String(p_mesh->get_name()) + "' surface " + String::num_int64(s) +
					": no manifest material named '" + key + "'");
			return false;
		}
		const Ref<ModelMaterialSpec> spec = p_manifest->get_materials()[material_index];
		const Array arrays = resource->surface_get_arrays(s);
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
		const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array uvs2 = arrays[Mesh::ARRAY_TEX_UV2];
		const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
		const PackedInt32Array bones = arrays[Mesh::ARRAY_BONES];
		const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
		PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const int64_t count = positions.size();
		if (count == 0) continue;
		if (normals.size() != count) {
			fail("mesh '" + String(p_mesh->get_name()) + "' surface " + String::num_int64(s) + " has no normals");
			return false;
		}
		if (r_model.tangents && tangents.size() != count * 4) {
			fail("mesh '" + String(p_mesh->get_name()) + "' surface " + String::num_int64(s) +
					" has no tangents but a material asks for the tangent vertex format");
			return false;
		}
		if (r_model.skinned && (bones.size() != count * 4 || weights.size() != count * 4)) {
			fail("mesh '" + String(p_mesh->get_name()) + "' surface " + String::num_int64(s) +
					" carries no 4-influence skin arrays");
			return false;
		}
		if (indices.is_empty()) {
			indices.resize(count);
			for (int64_t i = 0; i < count; ++i) indices[i] = static_cast<int32_t>(i);
		}
		// The projector's exact channels (ArrayMesh packs normals and tangents
		// octahedrally); an authored mesh has none and its decoded values rule.
		const PackedFloat32Array exact_normals = exact_channel(arrays, Mesh::ARRAY_CUSTOM0, count);
		const PackedFloat32Array exact_tangents = exact_channel(arrays, Mesh::ARRAY_CUSTOM1, count);
		const PackedFloat32Array exact_bitangents = exact_channel(arrays, Mesh::ARRAY_CUSTOM2, count);
		if (indices.size() % 3 != 0) {
			fail("mesh '" + String(p_mesh->get_name()) + "' surface " + String::num_int64(s) + ": index count not a multiple of 3");
			return false;
		}
		// Vertices in model axes; skinned ones carry the part index of each of
		// their (up to three) influences in `part_of[k]` for the strip cut.
		std::vector<ThreediVertex> verts(static_cast<size_t>(count));
		std::vector<std::array<int, 3>> part_of(static_cast<size_t>(count));
		for (int64_t i = 0; i < count; ++i) {
			ThreediVertex &v = verts[static_cast<size_t>(i)];
			v = ThreediVertex{};
			const Vector3 position = p_mesh_global.xform(positions[i]);
			const ThreediBuildVec3 pm = model_from_presentation(position);
			v.position[0] = static_cast<float>(pm.x);
			v.position[1] = static_cast<float>(pm.y);
			v.position[2] = static_cast<float>(pm.z);
			const Vector3 normal = normal_basis.xform(exact_normals.is_empty()
							? normals[i]
							: Vector3(exact_normals[i * 3], exact_normals[i * 3 + 1], exact_normals[i * 3 + 2]));
			const ThreediBuildVec3 nm = model_from_presentation(normal);
			v.normal[0] = static_cast<float>(nm.x);
			v.normal[1] = static_cast<float>(nm.y);
			v.normal[2] = static_cast<float>(nm.z);
			if (uvs.size() == count) {
				v.uv0[0] = uvs[i].x;
				v.uv0[1] = uvs[i].y;
			}
			if (uvs2.size() == count) {
				v.uv1[0] = uvs2[i].x;
				v.uv1[1] = uvs2[i].y;
			} else {
				v.uv1[0] = v.uv0[0];
				v.uv1[1] = v.uv0[1];
			}
			if (r_model.tangents) {
				const bool exact = !exact_tangents.is_empty() && !exact_bitangents.is_empty();
				const Vector3 tangent = normal_basis.xform(exact
								? Vector3(exact_tangents[i * 3], exact_tangents[i * 3 + 1], exact_tangents[i * 3 + 2])
								: Vector3(tangents[i * 4], tangents[i * 4 + 1], tangents[i * 4 + 2]));
				const float w = tangents[i * 4 + 3];
				const Vector3 bitangent = exact
						? normal_basis.xform(Vector3(exact_bitangents[i * 3], exact_bitangents[i * 3 + 1], exact_bitangents[i * 3 + 2]))
						: normal.cross(tangent) * w;
				const ThreediBuildVec3 tm = model_from_presentation(tangent);
				const ThreediBuildVec3 bm = model_from_presentation(bitangent);
				v.tangent[0] = static_cast<float>(tm.x);
				v.tangent[1] = static_cast<float>(tm.y);
				v.tangent[2] = static_cast<float>(tm.z);
				v.bitangent[0] = static_cast<float>(bm.x);
				v.bitangent[1] = static_cast<float>(bm.y);
				v.bitangent[2] = static_cast<float>(bm.z);
			}
			part_of[static_cast<size_t>(i)] = {p_part, p_part, p_part};
			if (r_model.skinned) {
				// The three heaviest influences, renormalised; the file carries
				// three weights and four local indices (the fourth unused).
				struct Influence {
					int part;
					float weight;
				};
				std::vector<Influence> influences;
				for (int k = 0; k < 4; ++k) {
					const int bind = bones[i * 4 + k];
					const float weight = weights[i * 4 + k];
					if (weight <= 0.0f) continue;
					if (bind < 0 || bind >= static_cast<int>(bind_to_part.size())) {
						fail("mesh '" + String(p_mesh->get_name()) + "': a vertex names skin bind " +
								String::num_int64(bind) + " outside the Skin");
						return false;
					}
					influences.push_back(Influence{bind_to_part[static_cast<size_t>(bind)], weight});
				}
				if (influences.empty()) {
					influences.push_back(Influence{p_part, 1.0f});
				}
				std::stable_sort(influences.begin(), influences.end(),
						[](const Influence &a, const Influence &b) { return a.weight > b.weight; });
				if (influences.size() > 3) influences.resize(3);
				float sum = 0.0f;
				for (const Influence &inf : influences) sum += inf.weight;
				for (size_t k = 0; k < 3; ++k) {
					if (k < influences.size()) {
						v.bone_weights[k] = influences[k].weight / sum;
						part_of[static_cast<size_t>(i)][k] = influences[k].part;
					} else {
						v.bone_weights[k] = 0.0f;
						part_of[static_cast<size_t>(i)][k] = influences[0].part;
					}
				}
				v.is_skinned = 1;
			}
		}
		// One strip when the surface's parts fit a 16-entry table: the vertex
		// list and indices go verbatim (the file's own order survives a round
		// trip). Otherwise cut by triangles, duplicating vertices per strip.
		const int64_t tri_count = indices.size() / 3;
		{
			std::vector<uint8_t> bone_table;
			bool fits = true;
			if (r_model.skinned) {
				for (int64_t i = 0; i < count && fits; ++i) {
					for (int k = 0; k < 3; ++k) {
						const int part = part_of[static_cast<size_t>(i)][k];
						if (std::find(bone_table.begin(), bone_table.end(), part) != bone_table.end()) continue;
						if (bone_table.size() >= static_cast<size_t>(kMaxStripBones)) {
							fits = false;
							break;
						}
						bone_table.push_back(static_cast<uint8_t>(part));
					}
				}
			}
			if (fits) {
				std::vector<ThreediVertex> strip_vertices = verts;
				std::vector<uint16_t> strip_indices;
				strip_indices.reserve(static_cast<size_t>(indices.size()));
				for (int64_t k = 0; k < indices.size(); ++k) {
					if (indices[k] < 0 || indices[k] >= count) {
						fail("mesh '" + String(p_mesh->get_name()) + "': an index leaves the vertex array");
						return false;
					}
					strip_indices.push_back(static_cast<uint16_t>(indices[k]));
				}
				if (count > 0x10000) {
					fail("mesh '" + String(p_mesh->get_name()) + "': a strip needs more than 65536 vertices");
					return false;
				}
				if (r_model.skinned) {
					for (int64_t i = 0; i < count; ++i) {
						ThreediVertex &v = strip_vertices[static_cast<size_t>(i)];
						for (int k = 0; k < 3; ++k) {
							const int part = part_of[static_cast<size_t>(i)][k];
							v.bone_indices[k] = static_cast<uint8_t>(
									std::find(bone_table.begin(), bone_table.end(), part) - bone_table.begin());
						}
						v.bone_indices[3] = 0;
					}
				}
				r_model.add_strip(p_lod, p_part, material_index, spec->get_alpha_strips(), std::move(strip_vertices),
						std::move(strip_indices), std::move(bone_table));
				continue;
			}
		}
		int64_t tri = 0;
		while (tri < tri_count) {
			std::vector<uint8_t> bone_table;
			std::vector<int> global_to_local(static_cast<size_t>(count), -1);
			std::vector<ThreediVertex> strip_vertices;
			std::vector<uint16_t> strip_indices;
			auto table_slot = [&](int part) -> int {
				for (size_t k = 0; k < bone_table.size(); ++k) {
					if (bone_table[k] == part) return static_cast<int>(k);
				}
				return -1;
			};
			for (; tri < tri_count; ++tri) {
				// Would this triangle's parts overflow the table?
				if (r_model.skinned) {
					std::vector<int> fresh;
					for (int c = 0; c < 3; ++c) {
						const int gi = indices[tri * 3 + c];
						for (int k = 0; k < 3; ++k) {
							const int part = part_of[static_cast<size_t>(gi)][k];
							if (table_slot(part) < 0 && std::find(fresh.begin(), fresh.end(), part) == fresh.end()) {
								fresh.push_back(part);
							}
						}
					}
					if (bone_table.size() + fresh.size() > static_cast<size_t>(kMaxStripBones)) {
						if (bone_table.empty()) {
							fail("mesh '" + String(p_mesh->get_name()) + "': one triangle spans more than 16 parts");
							return false;
						}
						break;
					}
					for (const int part : fresh) bone_table.push_back(static_cast<uint8_t>(part));
				}
				for (int c = 0; c < 3; ++c) {
					const int gi = indices[tri * 3 + c];
					if (gi < 0 || gi >= count) {
						fail("mesh '" + String(p_mesh->get_name()) + "': an index leaves the vertex array");
						return false;
					}
					int local = global_to_local[static_cast<size_t>(gi)];
					if (local < 0) {
						local = static_cast<int>(strip_vertices.size());
						if (local > 0xFFFF) {
							fail("mesh '" + String(p_mesh->get_name()) + "': a strip needs more than 65536 vertices");
							return false;
						}
						global_to_local[static_cast<size_t>(gi)] = local;
						ThreediVertex v = verts[static_cast<size_t>(gi)];
						if (r_model.skinned) {
							for (int k = 0; k < 3; ++k) {
								v.bone_indices[k] = static_cast<uint8_t>(table_slot(part_of[static_cast<size_t>(gi)][k]));
							}
							v.bone_indices[3] = 0;
						}
						strip_vertices.push_back(v);
					}
					strip_indices.push_back(static_cast<uint16_t>(local));
				}
			}
			r_model.add_strip(p_lod, p_part, material_index, spec->get_alpha_strips(), std::move(strip_vertices),
					std::move(strip_indices), std::move(bone_table));
		}
	}
	return true;
}

bool ModelSceneExporter::export_collision(Node3D *p_collision, int p_part_count, const std::vector<int> &p_part_parents,
		ThreediBuildModel &r_model) {
	std::map<int, Node3D *> sections;
	for (int i = 0; i < p_collision->get_child_count(); ++i) {
		Node3D *child = Object::cast_to<Node3D>(p_collision->get_child(i));
		if (child == nullptr) continue;
		int index = 0;
		if (!threedi_scene_parse_collision_section_name(std_name(child), index)) {
			fail("collision child '" + String(child->get_name()) + "' is not named CO##");
			return false;
		}
		if (sections.count(index) != 0) {
			fail("two nodes name " + String(child->get_name()));
			return false;
		}
		sections[index] = child;
	}
	int expected = 0;
	for (const auto &entry : sections) {
		if (entry.first != expected) {
			fail("collision sections are not contiguous from CO01");
			return false;
		}
		++expected;
	}
	for (const auto &entry : sections) {
		const int index = entry.first;
		Node3D *node = entry.second;
		const ModelCollisionSection3D *typed = Object::cast_to<ModelCollisionSection3D>(node);
		int parent_part = index;
		if (typed != nullptr) {
			parent_part = typed->get_parent_part();
		} else if (index < static_cast<int>(p_part_parents.size())) {
			parent_part = p_part_parents[static_cast<size_t>(index)];
		}
		const ThreediBuildVec3 offset = mission_from_presentation(node->get_position());
		int cobj = -1;
		if (typed != nullptr && typed->get_sphere()) {
			cobj = r_model.add_sphere_cobj(parent_part, offset, mission_from_presentation(typed->get_sphere_center()),
					typed->get_sphere_radius());
		} else {
			cobj = r_model.add_cobj(parent_part, offset);
		}
		String faces_name = String(node->get_name()) + String(" faces");
		for (int i = 0; i < node->get_child_count(); ++i) {
			Node3D *child = Object::cast_to<Node3D>(node->get_child(i));
			if (child == nullptr) continue;
			MeshInstance3D *mesh_node = Object::cast_to<MeshInstance3D>(child);
			if (mesh_node != nullptr && String(child->get_name()) == faces_name) {
				const Ref<Mesh> resource = mesh_node->get_mesh();
				if (resource.is_null()) continue;
				const Transform3D local = relative_transform(mesh_node, node);
				bool vertices_added = false;
				for (int s = 0; s < resource->get_surface_count(); ++s) {
					const Ref<ArrayMesh> array_mesh = resource;
					int poly_type = 1;
					uint32_t material_flags = 0;
					const String surface_name = array_mesh.is_valid() ? array_mesh->surface_get_name(s) : String();
					if (!surface_name.is_empty() && !parse_face_surface_name(surface_name, poly_type, material_flags)) {
						fail(faces_name + ": surface '" + surface_name + "' is not named pt<n>_mf<hex>");
						return false;
					}
					const Array arrays = resource->surface_get_arrays(s);
					const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
					PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
					if (!vertices_added) {
						for (int64_t vi = 0; vi < positions.size(); ++vi) {
							r_model.add_collision_vertex(cobj, mission_from_presentation(local.xform(positions[vi])));
						}
						vertices_added = true;
					} else if (static_cast<size_t>(positions.size()) != r_model.collision[cobj].vertices.size()) {
						fail(faces_name + ": every surface must carry the section's whole vertex list");
						return false;
					}
					if (indices.is_empty()) {
						indices.resize(positions.size());
						for (int64_t k = 0; k < positions.size(); ++k) indices[k] = static_cast<int32_t>(k);
					}
					for (int64_t t = 0; t + 2 < indices.size(); t += 3) {
						r_model.add_face(cobj, static_cast<uint16_t>(indices[t]), static_cast<uint16_t>(indices[t + 1]),
								static_cast<uint16_t>(indices[t + 2]), static_cast<uint8_t>(poly_type), material_flags);
					}
				}
				continue;
			}
			const ModelBoundingVolume3D *volume = Object::cast_to<ModelBoundingVolume3D>(child);
			if (volume != nullptr) {
				const AABB bounds = volume->get_bounds();
				const ThreediBuildVec3 lo = mission_from_presentation(bounds.position);
				const ThreediBuildVec3 hi = mission_from_presentation(bounds.position + bounds.size);
				ThreediBuildBox box;
				box.min = lo;
				box.max = hi;
				std::vector<ThreediBoundingPlane> planes;
				const Array plane_values = volume->get_planes();
				for (int k = 0; k < plane_values.size(); ++k) {
					const Plane plane = plane_values[k];
					const ThreediBuildVec3 n = mission_from_presentation(plane.normal);
					ThreediBoundingPlane record{};
					record.normal[0] = threedi_q14f(n.x);
					record.normal[1] = threedi_q14f(n.y);
					record.normal[2] = threedi_q14f(n.z);
					record.radius = threedi_q16f(-plane.d);
					planes.push_back(record);
				}
				if (planes.empty()) {
					fail(String(child->get_name()) + ": a bounding volume needs at least one plane");
					return false;
				}
				r_model.add_volume_planes(cobj, volume->get_collidable_type(), volume->get_flags(), box, planes);
				continue;
			}
			// A plain node named per the contract: an axis box from its mesh.
			int32_t type = 0, flags = 0;
			int ordinal = 0;
			if (threedi_scene_parse_collision_volume_name(std_name(child), type, flags, ordinal)) {
				if (mesh_node == nullptr || mesh_node->get_mesh().is_null()) {
					fail(String(child->get_name()) + ": a plain volume node needs a mesh to take its box from");
					return false;
				}
				const AABB aabb = relative_transform(mesh_node, node).xform(mesh_node->get_mesh()->get_aabb());
				ThreediBuildBox box;
				box.min = mission_from_presentation(aabb.position);
				box.max = mission_from_presentation(aabb.position + aabb.size);
				r_model.add_volume(cobj, type < 0 ? 1 : type, flags, box);
				continue;
			}
			if (threedi_scene_is_occlusion_name(std_name(child))) {
				fail(String(child->get_name()) + ": occlusion records have no scene form yet");
				return false;
			}
		}
	}
	return true;
}

bool ModelSceneExporter::export_user_points(Node3D *p_points, ThreediBuildModel &r_model) {
	for (int i = 0; i < p_points->get_child_count(); ++i) {
		Node3D *child = Object::cast_to<Node3D>(p_points->get_child(i));
		if (child == nullptr) continue;
		const ModelUserPoint3D *typed = Object::cast_to<ModelUserPoint3D>(child);
		int32_t point_type = 0;
		int part = 0;
		std::string label;
		Vector3 direction;
		if (typed != nullptr) {
			point_type = typed->get_point_type();
			part = typed->get_part_index();
			label = opennova::to_std(typed->get_label());
			direction = typed->get_direction();
		} else {
			if (!threedi_scene_parse_user_point_name(std_name(child), point_type, part, label)) {
				fail("user point '" + String(child->get_name()) + "' is not named UPcNN <label>");
				return false;
			}
			direction = -child->get_transform().basis.get_column(2);
		}
		if (label.size() > 16) {
			fail("user point '" + String(child->get_name()) + "': label longer than 16 characters");
			return false;
		}
		const Transform3D local = relative_transform(child, p_points->get_parent());
		r_model.add_user_point(label.c_str(), mission_from_presentation(local.origin),
				mission_from_presentation(direction), part, point_type);
	}
	return true;
}

bool ModelSceneExporter::export_lights(Node3D *p_lights, ThreediBuildModel &r_model) {
	for (int i = 0; i < p_lights->get_child_count(); ++i) {
		const ModelLight3D *light = Object::cast_to<ModelLight3D>(p_lights->get_child(i));
		if (light == nullptr) {
			fail("light '" + String(p_lights->get_child(i)->get_name()) + "' is not a ModelLight3D");
			return false;
		}
		ThreediLight record;
		if (!light->write(record)) {
			fail("light '" + String(light->get_name()) + "': rotation needs 4 words and view_proj 16");
			return false;
		}
		r_model.lights.push_back(record);
	}
	return true;
}
