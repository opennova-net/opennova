#include "model/model_scene_projector.h"

#include "model/model_bounding_volume_3d.h"
#include "model/model_collision_section_3d.h"
#include "model/model_frames.h"
#include "model/model_light_3d.h"
#include "model/model_user_point_3d.h"
#include "util/string_convert.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <formats/threedi/threedi_scene_names.h>

using namespace godot;
using namespace opennova::threedi;

namespace {

constexpr uint32_t kExpectedMaterialRecordSize = 584u;
constexpr uint32_t kExpectedPanmRecordSize = 68u;
constexpr uint32_t kExpectedCtrlRecordSize = 24u;
constexpr int kMaxLods = 4;

void own_tree(Node *p_node, Node *p_owner) {
	for (int i = 0; i < p_node->get_child_count(); ++i) {
		Node *child = p_node->get_child(i);
		child->set_owner(p_owner);
		own_tree(child, p_owner);
	}
}

String hex_word(uint32_t value) {
	return String::num_uint64(value, 16);
}

bool matrix_is_identity(const ThreediMatrix4x4 &m) {
	ThreediMatrix4x4 identity;
	threedi_mat4_identity(&identity);
	return memcmp(m.m, identity.m, sizeof(m.m)) == 0;
}

} // namespace

void ModelSceneProjector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("project", "document", "manifest", "textures_dir"), &ModelSceneProjector::project,
			DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_last_error"), &ModelSceneProjector::get_last_error);
	ClassDB::bind_method(D_METHOD("get_refusals"), &ModelSceneProjector::get_refusals);
}

void ModelSceneProjector::refuse(const String &p_what) {
	refusals_.push_back(p_what);
}

bool ModelSceneProjector::check_scope(const Threedi3di3 *m) {
	if (m->occlusion_object_count > 0) {
		refuse("occlusion records (OCCL) have no scene form yet");
	}
	if (m->info.data_len > 0) {
		refuse("a non-empty INFO chunk");
	}
	if (m->mtrx.count != 1 || m->mtrx.matrices == nullptr || !matrix_is_identity(m->mtrx.matrices[0])) {
		refuse("a MTRX table other than one identity matrix");
	}
	if (m->ovrt.count > 0) {
		refuse("an OVRT table");
	}
	// The reader also lands the first PANM chunk it finds in the file-level
	// table (LOD0's rows again); the per-LOD tables are the scene form and
	// the writer emits PANM per RLOD, so that alias is not a construct.
	if (m->material_count > 0 && m->material_record_size != kExpectedMaterialRecordSize) {
		refuse("a MTRL record size other than 584");
	}
	for (uint32_t i = 0; i < m->material_count; ++i) {
		if (m->materials[i].index != static_cast<int32_t>(i)) {
			refuse("a MTRL index that is not its table position");
			break;
		}
	}
	if (m->ctrl.count > 0 && m->ctrl.record_size != kExpectedCtrlRecordSize) {
		refuse("a CTRL record size other than 24");
	}
	if (m->lod_count > static_cast<size_t>(kMaxLods)) {
		refuse("more than four LODs");
	}
	const bool skinned = m->header.mesh_type == THREEDI_MESH_SKINNED;
	for (size_t li = 0; li < m->lod_count; ++li) {
		const ThreediLod &lod = m->lods[li];
		String where = "LOD" + String::num_int64(static_cast<int64_t>(li)) + ": ";
		if (lod.rmdl_render_object_count != static_cast<int32_t>(lod.render_object_count)) {
			refuse(where + "the RMDL part count differs from the ROBJ table");
		}
		if (lod.strip_count > 0 && lod.strip_record_size != (skinned ? 68u : 48u)) {
			refuse(where + "a STRP record size other than the mesh type's");
		}
		if (lod.part_animation_count > 0 && lod.part_animation_record_size != kExpectedPanmRecordSize) {
			refuse(where + "a PANM record size other than 68");
		}
		const uint32_t allowed = 1u | THREEDI_VERTEX_FLAG_SKINNED | THREEDI_VERTEX_FLAG_TANGENTS;
		if (lod.vertices.count > 0 && (lod.vertices.flags & ~allowed) != 0) {
			refuse(where + "VERT flags 0x" + hex_word(lod.vertices.flags) + " outside the known set");
		}
		if (lod.vertices.count > 0 && (((lod.vertices.flags & THREEDI_VERTEX_FLAG_SKINNED) != 0) != skinned)) {
			refuse(where + "VERT skinned flag disagrees with the GHDR mesh type");
		}
		size_t part_strips = 0;
		for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
			const ThreediRenderObject &ro = lod.render_objects[pi];
			part_strips += static_cast<size_t>(ro.num_strips + ro.num_alpha_strips);
			if (ro.parent_index < 0 || static_cast<size_t>(ro.parent_index) >= lod.render_object_count) {
				refuse(where + "a part whose parent is outside the table");
			}
		}
		if (part_strips != lod.strip_count) {
			refuse(where + "the per-part strip counts do not sum to the STRP table");
		}
		for (size_t si = 0; si < lod.strip_count; ++si) {
			const ThreediTriangleStrip &strip = lod.strips[si];
			if (strip.is_strip != 0) {
				refuse(where + "an indexed triangle strip (only triangle lists have a scene form)");
				break;
			}
			if (skinned && strip.bone_table_length <= 0) {
				refuse(where + "a skinned strip without a bone table");
				break;
			}
			if (strip.start_vertex < 0 || strip.num_vertices < 0 ||
					static_cast<uint32_t>(strip.start_vertex + strip.num_vertices) > lod.vertices.count) {
				refuse(where + "a strip whose vertex window leaves the VERT table");
				break;
			}
			if (strip.index_offset < 0 ||
					static_cast<uint32_t>(strip.index_offset) + strip.num_indices > lod.indices.count) {
				refuse(where + "a strip whose index window leaves the INDX table");
				break;
			}
			if (strip.material_index < 0 || static_cast<uint32_t>(strip.material_index) >= m->material_count) {
				refuse(where + "a strip naming a material outside the MTRL table");
				break;
			}
		}
	}
	if (m->collision != nullptr) {
		const ThreediCollisionModel *col = m->collision;
		if (!threedi_3di3_collision_is_runtime_safe(col)) {
			refuse("a collision block the runtime would reject");
		} else {
			size_t owned_volumes = 0;
			for (size_t oi = 0; oi < col->object_count; ++oi) {
				owned_volumes += static_cast<size_t>(col->objects[oi].num_bounding_volumes);
			}
			if (owned_volumes != col->volume_count) {
				refuse("bounding volumes owned by no collision section");
			}
			for (size_t oi = 0; oi < col->object_count; ++oi) {
				const ThreediCollisionObject &o = col->objects[oi];
				if (o.unk0 != 0 || o.unk3 != 0 || o.unk4 != 0 || o.unk5 != 0) {
					refuse("a collision section with non-zero unnamed words");
					break;
				}
			}
			// CXLT rows must be the offsets the assembler derives (one per
			// non-root section on rigid models, one per section on skinned).
			const size_t expected_translations = skinned ? col->object_count : (col->object_count > 0 ? col->object_count - 1 : 0);
			if (col->translation_count != expected_translations) {
				refuse("a CXLT table that is not one row per translated section");
			} else {
				for (size_t oi = 0; oi < col->object_count; ++oi) {
					if (oi == 0 && !skinned) continue;
					const size_t ti = skinned ? oi : oi - 1;
					const ThreediCollisionObject &o = col->objects[oi];
					const ThreediCollisionTranslation &t = col->translations[ti];
					if (t.translation[0] != o.offset[0] || t.translation[1] != o.offset[1] ||
							t.translation[2] != o.offset[2]) {
						refuse("a CXLT row that differs from its section's offset");
						break;
					}
				}
			}
		}
	}
	return refusals_.is_empty();
}

Node3D *ModelSceneProjector::project(const Ref<ModelDocument> &p_document, const Ref<ModelAuthoringManifest> &p_manifest,
		const String &p_textures_dir) {
	refusals_.clear();
	last_error_ = String();
	if (p_document.is_null() || p_document->model() == nullptr) {
		last_error_ = "No model to project";
		return nullptr;
	}
	if (p_manifest.is_null()) {
		last_error_ = "No manifest to fill";
		return nullptr;
	}
	const Threedi3di3 *m = p_document->model();
	if (!check_scope(m)) {
		last_error_ = "The model carries constructs outside the scene form: " + String(", ").join(refusals_);
		return nullptr;
	}
	const bool skinned = m->header.mesh_type == THREEDI_MESH_SKINNED;
	const String textures_dir = p_textures_dir.is_empty() ? p_document->get_source_path().get_base_dir() : p_textures_dir;
	const String model_name = opennova::to_gd(std::string(m->header.name));

	// --- manifest: header words, registers, materials, LOD words ---
	p_manifest->set_model_name(model_name);
	p_manifest->set_version(static_cast<int>(m->version));
	p_manifest->set_skinned(skinned);
	p_manifest->set_matrix_count(static_cast<int>(m->mtrx.count));
	PackedStringArray registers;
	for (uint32_t i = 0; i < m->ctrl.count; ++i) {
		registers.push_back(opennova::to_gd(std::string(m->ctrl.registers[i].name)));
	}
	p_manifest->set_control_registers(registers);
	TypedArray<ModelMaterialSpec> materials;
	std::vector<Ref<StandardMaterial3D>> preview_materials;
	std::vector<int> material_bucket(m->material_count, -1); // 0 opaque, 1 alpha, -1 unused
	for (uint32_t i = 0; i < m->material_count; ++i) {
		Ref<ModelMaterialSpec> spec;
		spec.instantiate();
		spec->assign(m->materials[i]);
		String stem = spec->get_diffuse_texture().get_basename();
		if (stem.is_empty()) {
			stem = spec->get_shader_tag().to_lower();
		}
		spec->set_material_name("mtrl" + String::num_int64(i).pad_zeros(2) + "_" + stem);
		materials.push_back(spec);
		Ref<StandardMaterial3D> preview;
		preview.instantiate();
		preview->set_name(spec->get_material_name());
		const String diffuse = spec->get_diffuse_texture();
		Ref<Texture2D> texture = diffuse.is_empty() ? Ref<Texture2D>() : opennova::load_texture_from_dir(textures_dir, diffuse);
		if (texture.is_valid()) {
			preview->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture);
		} else {
			preview->set_albedo(Color(0.6f, 0.6f, 0.65f, 1.0f));
		}
		preview_materials.push_back(preview);
	}
	TypedArray<ModelLodSpec> lods;
	for (size_t li = 0; li < m->lod_count; ++li) {
		const ThreediLod &lod = m->lods[li];
		Ref<ModelLodSpec> spec;
		spec.instantiate();
		spec->set_threshold(lod.lod_threshold);
		spec->set_model_type(opennova::to_gd(std::string(lod.model_type)));
		TypedArray<ModelPartAnimationRow> rows;
		for (size_t ri = 0; ri < lod.part_animation_count; ++ri) {
			Ref<ModelPartAnimationRow> row;
			row.instantiate();
			row->assign(lod.part_animations[ri]);
			rows.push_back(row);
		}
		spec->set_part_animations(rows);
		lods.push_back(spec);
	}
	p_manifest->set_lods(lods);

	// --- the tree ---
	Node3D *root = memnew(Node3D);
	root->set_name(model_name.is_empty() ? String("Model") : model_name);

	for (size_t li = 0; li < m->lod_count; ++li) {
		const ThreediLod &lod = m->lods[li];
		Node3D *lod_node = memnew(Node3D);
		lod_node->set_name(opennova::to_gd(threedi_scene_lod_name(static_cast<int>(li))));
		root->add_child(lod_node);

		// Part pivots: abs from the file, rel checked against the parent chain.
		std::vector<Vector3> part_abs(lod.render_object_count);
		std::vector<Vector3> part_rel(lod.render_object_count);
		for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
			const ThreediRenderObject &ro = lod.render_objects[pi];
			part_abs[pi] = presentation_from_model(ro.abs[0], ro.abs[1], ro.abs[2]);
			part_rel[pi] = presentation_from_model(ro.rel[0], ro.rel[1], ro.rel[2]);
			const bool is_root = ro.parent_index == static_cast<int32_t>(pi);
			const Vector3 parent_abs = is_root ? Vector3() : part_abs[static_cast<size_t>(ro.parent_index)];
			if (!(parent_abs + part_rel[pi] == part_abs[pi])) {
				refuse("LOD" + String::num_int64(static_cast<int64_t>(li)) + ": part " +
						String::num_int64(static_cast<int64_t>(pi)) + " pivots do not chain (abs != parent abs + rel)");
			}
		}
		if (!refusals_.is_empty()) {
			break;
		}

		// Mesh host per part: the PN## node (static) or the Skeleton3D (skinned).
		std::vector<Node3D *> part_nodes(lod.render_object_count, nullptr);
		Skeleton3D *skeleton = nullptr;
		Ref<Skin> skin;
		if (skinned) {
			skeleton = memnew(Skeleton3D);
			skeleton->set_name("Skeleton3D");
			lod_node->add_child(skeleton);
			for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
				skeleton->add_bone(opennova::to_gd(threedi_scene_bone_name(static_cast<int>(pi))));
			}
			for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
				const ThreediRenderObject &ro = lod.render_objects[pi];
				const bool is_root = ro.parent_index == static_cast<int32_t>(pi);
				if (!is_root) {
					skeleton->set_bone_parent(static_cast<int>(pi), ro.parent_index);
				}
				skeleton->set_bone_rest(static_cast<int>(pi), Transform3D(Basis(), part_rel[pi]));
			}
			skeleton->reset_bone_poses();
			skin.instantiate();
			for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
				skin->add_bind(static_cast<int>(pi), Transform3D(Basis(), -part_abs[pi]));
				skin->set_bind_name(static_cast<int>(pi),
						opennova::to_gd(threedi_scene_bone_name(static_cast<int>(pi))));
			}
		} else {
			// Nested by parent: the file lists parents before children in the
			// retail corpus, but resolve in passes so any order works.
			size_t placed = 0;
			while (placed < lod.render_object_count) {
				size_t progressed = 0;
				for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
					if (part_nodes[pi] != nullptr) continue;
					const ThreediRenderObject &ro = lod.render_objects[pi];
					const bool is_root = ro.parent_index == static_cast<int32_t>(pi);
					Node3D *parent = is_root ? lod_node : part_nodes[static_cast<size_t>(ro.parent_index)];
					if (parent == nullptr) continue;
					Node3D *part = memnew(Node3D);
					part->set_name(opennova::to_gd(threedi_scene_part_name(static_cast<int>(pi))));
					part->set_position(part_rel[pi]);
					parent->add_child(part);
					part_nodes[pi] = part;
					++placed;
					++progressed;
				}
				if (progressed == 0) {
					refuse("LOD" + String::num_int64(static_cast<int64_t>(li)) + ": a part parent cycle");
					break;
				}
			}
			if (!refusals_.is_empty()) break;
		}

		// Strips -> one MeshInstance3D per part, one surface per strip, file order.
		const bool has_tangents = (lod.vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0;
		size_t strip_cursor = 0;
		for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
			const ThreediRenderObject &ro = lod.render_objects[pi];
			const size_t strip_total = static_cast<size_t>(ro.num_strips + ro.num_alpha_strips);
			if (strip_total == 0) continue;
			Ref<ArrayMesh> mesh;
			mesh.instantiate();
			for (size_t s = 0; s < strip_total; ++s, ++strip_cursor) {
				const ThreediTriangleStrip &strip = lod.strips[strip_cursor];
				const bool alpha = s >= static_cast<size_t>(ro.num_strips);
				int &bucket = material_bucket[static_cast<size_t>(strip.material_index)];
				if (bucket >= 0 && bucket != (alpha ? 1 : 0)) {
					refuse("material " + String::num_int64(strip.material_index) +
							" is used by both opaque and alpha strips");
				}
				bucket = alpha ? 1 : 0;
				PackedVector3Array vertices;
				PackedVector3Array normals;
				PackedVector2Array uvs;
				PackedVector2Array uvs2;
				PackedFloat32Array tangents;
				PackedInt32Array bones;
				PackedFloat32Array weights;
				// ArrayMesh octahedral-packs normals and tangents, so the file's
				// exact floats ride uncompressed RGB float custom channels
				// (CUSTOM0 normal, CUSTOM1 tangent, CUSTOM2 bitangent) that the
				// exporter prefers when present.
				PackedFloat32Array exact_normals;
				PackedFloat32Array exact_tangents;
				PackedFloat32Array exact_bitangents;
				for (int32_t vi = 0; vi < strip.num_vertices; ++vi) {
					const ThreediVertex &v = lod.vertices.items[static_cast<size_t>(strip.start_vertex + vi)];
					Vector3 position = presentation_from_model(v.position[0], v.position[1], v.position[2]);
					if (!skinned) {
						position -= part_abs[pi];
					}
					vertices.push_back(position);
					const Vector3 exact_normal = presentation_from_model(v.normal[0], v.normal[1], v.normal[2]);
					normals.push_back(exact_normal);
					exact_normals.push_back(exact_normal.x);
					exact_normals.push_back(exact_normal.y);
					exact_normals.push_back(exact_normal.z);
					uvs.push_back(Vector2(v.uv0[0], v.uv0[1]));
					uvs2.push_back(Vector2(v.uv1[0], v.uv1[1]));
					if (has_tangents) {
						const Vector3 n = presentation_from_model(v.normal[0], v.normal[1], v.normal[2]);
						const Vector3 t = presentation_from_model(v.tangent[0], v.tangent[1], v.tangent[2]);
						const Vector3 b = presentation_from_model(v.bitangent[0], v.bitangent[1], v.bitangent[2]);
						const float w = n.cross(t).dot(b) < 0.0f ? -1.0f : 1.0f;
						tangents.push_back(t.x);
						tangents.push_back(t.y);
						tangents.push_back(t.z);
						tangents.push_back(w);
						exact_tangents.push_back(t.x);
						exact_tangents.push_back(t.y);
						exact_tangents.push_back(t.z);
						exact_bitangents.push_back(b.x);
						exact_bitangents.push_back(b.y);
						exact_bitangents.push_back(b.z);
					}
					if (skinned) {
						for (int k = 0; k < 4; ++k) {
							const int local = v.bone_indices[k];
							bones.push_back(local < strip.bone_table_length ? strip.bone_table[local] : 0);
						}
						weights.push_back(v.bone_weights[0]);
						weights.push_back(v.bone_weights[1]);
						weights.push_back(v.bone_weights[2]);
						weights.push_back(0.0f);
					}
				}
				PackedInt32Array indices;
				for (uint16_t k = 0; k < strip.num_indices; ++k) {
					indices.push_back(lod.indices.indices[static_cast<size_t>(strip.index_offset) + k]);
				}
				Array arrays;
				arrays.resize(Mesh::ARRAY_MAX);
				arrays[Mesh::ARRAY_VERTEX] = vertices;
				arrays[Mesh::ARRAY_NORMAL] = normals;
				arrays[Mesh::ARRAY_TEX_UV] = uvs;
				arrays[Mesh::ARRAY_TEX_UV2] = uvs2;
				if (has_tangents) arrays[Mesh::ARRAY_TANGENT] = tangents;
				if (skinned) {
					arrays[Mesh::ARRAY_BONES] = bones;
					arrays[Mesh::ARRAY_WEIGHTS] = weights;
				}
				arrays[Mesh::ARRAY_INDEX] = indices;
				arrays[Mesh::ARRAY_CUSTOM0] = exact_normals;
				int64_t format = static_cast<int64_t>(Mesh::ARRAY_CUSTOM_RGB_FLOAT) << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT;
				if (has_tangents) {
					arrays[Mesh::ARRAY_CUSTOM1] = exact_tangents;
					arrays[Mesh::ARRAY_CUSTOM2] = exact_bitangents;
					format |= static_cast<int64_t>(Mesh::ARRAY_CUSTOM_RGB_FLOAT) << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT;
					format |= static_cast<int64_t>(Mesh::ARRAY_CUSTOM_RGB_FLOAT) << Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT;
				}
				mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), format);
				const int surface = mesh->get_surface_count() - 1;
				const Ref<ModelMaterialSpec> spec = materials[strip.material_index];
				mesh->surface_set_name(surface, spec->get_material_name());
				mesh->surface_set_material(surface, preview_materials[static_cast<size_t>(strip.material_index)]);
			}
			MeshInstance3D *instance = memnew(MeshInstance3D);
			instance->set_name(opennova::to_gd(threedi_scene_mesh_name(static_cast<int>(pi), 0)));
			instance->set_mesh(mesh);
			if (skinned) {
				skeleton->add_child(instance);
				instance->set_skin(skin);
				instance->set_skeleton_path(NodePath(".."));
			} else {
				part_nodes[pi]->add_child(instance);
			}
		}
	}
	for (uint32_t i = 0; i < m->material_count; ++i) {
		const Ref<ModelMaterialSpec> spec = materials[i];
		spec->set_alpha_strips(material_bucket[i] == 1);
	}
	p_manifest->set_materials(materials);
	p_manifest->set_expected_bone_rows(skinned && m->lod_count > 0 ? static_cast<int>(m->lods[0].render_object_count) : 0);

	// --- collision ---
	if (refusals_.is_empty() && m->collision != nullptr && m->collision->object_count > 0) {
		const ThreediCollisionModel *col = m->collision;
		std::vector<ThreediCollisionObjectRun> runs(col->object_count);
		threedi_collision_object_runs(col, runs.data());
		Node3D *collision = memnew(Node3D);
		collision->set_name("Collision");
		root->add_child(collision);
		size_t plane_cursor = 0;
		for (size_t oi = 0; oi < col->object_count; ++oi) {
			const ThreediCollisionObject &o = col->objects[oi];
			const ThreediCollisionObjectRun &run = runs[oi];
			ModelCollisionSection3D *section = memnew(ModelCollisionSection3D);
			section->set_name(opennova::to_gd(threedi_scene_collision_section_name(static_cast<int>(oi))));
			section->set_position(presentation_from_q16(o.offset[0], o.offset[1], o.offset[2]));
			section->set_parent_part(o.parent_subobject_index);
			collision->add_child(section);
			const bool sphere = o.num_vertices == 0 && o.num_faces == 0 && o.num_bounding_volumes == 0 && o.radius > 0;
			if (sphere) {
				section->set_sphere(true);
				section->set_sphere_center(presentation_from_q16(o.med[0], o.med[1], o.med[2]));
				section->set_sphere_radius(static_cast<float>(o.radius / 65536.0));
			}
			if (o.num_faces > 0) {
				PackedVector3Array vertices;
				for (int32_t vi = 0; vi < o.num_vertices; ++vi) {
					const ThreediCollisionVertex &v = col->vertices[static_cast<size_t>(run.vertex_start + vi)];
					vertices.push_back(presentation_from_mission(v.position[0], v.position[1], v.position[2]));
				}
				// Faces group by (poly_type, material_flags), contiguously.
				Ref<ArrayMesh> mesh;
				mesh.instantiate();
				std::vector<std::pair<uint8_t, uint32_t>> seen;
				int32_t fi = 0;
				while (fi < o.num_faces) {
					const ThreediCollisionFace &first = col->faces[static_cast<size_t>(run.face_start + fi)];
					const std::pair<uint8_t, uint32_t> key(first.poly_type, first.material_flags);
					for (const auto &k : seen) {
						if (k == key) {
							refuse(String(section->get_name()) + String(": collision faces interleave surface kinds"));
							break;
						}
					}
					if (!refusals_.is_empty()) break;
					seen.push_back(key);
					PackedInt32Array indices;
					while (fi < o.num_faces) {
						const ThreediCollisionFace &f = col->faces[static_cast<size_t>(run.face_start + fi)];
						if (f.poly_type != key.first || f.material_flags != key.second) break;
						indices.push_back(f.vert_index[0]);
						indices.push_back(f.vert_index[1]);
						indices.push_back(f.vert_index[2]);
						++fi;
					}
					Array arrays;
					arrays.resize(Mesh::ARRAY_MAX);
					arrays[Mesh::ARRAY_VERTEX] = vertices;
					arrays[Mesh::ARRAY_INDEX] = indices;
					mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
					mesh->surface_set_name(mesh->get_surface_count() - 1,
							"pt" + String::num_int64(key.first) + "_mf" + hex_word(key.second));
				}
				MeshInstance3D *faces = memnew(MeshInstance3D);
				faces->set_name(String(section->get_name()) + String(" faces"));
				faces->set_mesh(mesh);
				section->add_child(faces);
			}
			for (int32_t bi = 0; bi < o.num_bounding_volumes; ++bi) {
				const ThreediBoundingVolume &v = col->volumes[static_cast<size_t>(run.volume_start + bi)];
				ModelBoundingVolume3D *volume = memnew(ModelBoundingVolume3D);
				volume->set_name(opennova::to_gd(
						threedi_scene_collision_volume_name(v.collidable_type, v.flags, bi, 0)));
				volume->set_collidable_type(v.collidable_type);
				volume->set_flags(v.flags);
				const Vector3 lo = presentation_from_q16(v.min_x_fp16, v.min_y_fp16, v.min_z_fp16);
				const Vector3 hi = presentation_from_q16(v.max_x_fp16, v.max_y_fp16, v.max_z_fp16);
				volume->set_bounds(AABB(lo, hi - lo));
				Array planes;
				for (int32_t pk = 0; pk < v.plane_count; ++pk, ++plane_cursor) {
					const ThreediBoundingPlane &pl = col->planes[plane_cursor];
					planes.push_back(Plane(presentation_from_mission(pl.normal[0], pl.normal[1], pl.normal[2]),
							-pl.radius));
				}
				volume->set_planes(planes);
				section->add_child(volume);
			}
		}
	}

	// --- user points and lights ---
	if (refusals_.is_empty() && m->user_point_count > 0) {
		Node3D *points = memnew(Node3D);
		points->set_name("UserPoints");
		root->add_child(points);
		for (size_t i = 0; i < m->user_point_count; ++i) {
			const ThreediUserPoint &up = m->user_points[i];
			std::string name;
			if (!threedi_scene_user_point_name(up.userpoint_type, up.subobject_index, up.name, name)) {
				refuse("user point '" + opennova::to_gd(std::string(up.name)) + "' has a type outside 'A'..'Z'");
				break;
			}
			ModelUserPoint3D *point = memnew(ModelUserPoint3D);
			point->set_name(opennova::to_gd(name));
			point->set_position(presentation_from_q16(up.x, up.y, up.z));
			point->set_direction(presentation_from_q16(up.rot_x, up.rot_y, up.rot_z));
			point->set_point_type(up.userpoint_type);
			point->set_part_index(up.subobject_index);
			point->set_label(opennova::to_gd(std::string(up.name)));
			points->add_child(point);
		}
	}
	if (refusals_.is_empty() && m->light_count > 0) {
		Node3D *lights = memnew(Node3D);
		lights->set_name("Lights");
		root->add_child(lights);
		for (size_t i = 0; i < m->light_count; ++i) {
			ModelLight3D *light = memnew(ModelLight3D);
			light->set_name(opennova::to_gd(threedi_scene_light_name(static_cast<int>(i))));
			light->assign(m->lights[i]);
			lights->add_child(light);
		}
	}

	if (!refusals_.is_empty()) {
		last_error_ = "The model carries constructs outside the scene form: " + String(", ").join(refusals_);
		memdelete(root);
		return nullptr;
	}
	own_tree(root, root);
	return root;
}
