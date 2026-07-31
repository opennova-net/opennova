// NovaObjectData — geometry views: LOD surfaces and memoized submesh builds,
// bones/skinning, collision volumes, lights and user points.
#include "object/nova_object_data_internal.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <vector>

using namespace novaobj;

namespace {

int material_array_index_for_id(const ThreediModelIR &ir, int32_t material_index) {
	for (size_t i = 0; i < ir.material_count; ++i) {
		if (ir.materials[i].index == material_index) {
			return static_cast<int>(i);
		}
	}
	if (material_index >= 0 && static_cast<size_t>(material_index) < ir.material_count) {
		return material_index;
	}
	return -1;
}

Vector3 godot_position(const ThreediIRVertex &v) {
	return Vector3(-v.position[0], v.position[1], v.position[2]);
}

Vector3 godot_normal(const ThreediIRVertex &v) {
	return Vector3(-v.normal[0], v.normal[1], v.normal[2]);
}

bool vertex_has_tangents(const ThreediIRVertex &v) {
	if ((v.flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0) {
		return true;
	}
	const float tangent_len =
			v.tangent[0] * v.tangent[0] + v.tangent[1] * v.tangent[1] + v.tangent[2] * v.tangent[2];
	const float bitangent_len =
			v.bitangent[0] * v.bitangent[0] + v.bitangent[1] * v.bitangent[1] + v.bitangent[2] * v.bitangent[2];
	return tangent_len > 0.000001f && bitangent_len > 0.000001f;
}

bool decode_primitive_indices(const ThreediIRLod &lod, const ThreediIRPrimitive &prim, std::vector<uint16_t> &out) {
	out.clear();
	if (lod.indices == nullptr || lod.vertices == nullptr || prim.index_count == 0 || prim.vertex_count == 0) {
		return false;
	}
	if (prim.index_offset + prim.index_count > lod.index_count ||
			prim.vertex_offset + prim.vertex_count > lod.vertex_count) {
		return false;
	}

	const uint16_t *raw = lod.indices + prim.index_offset;
	uint16_t min_idx = 0xffffu;
	uint16_t max_idx = 0;
	for (uint32_t i = 0; i < prim.index_count; ++i) {
		const uint16_t idx = raw[i];
		min_idx = std::min(min_idx, idx);
		max_idx = std::max(max_idx, idx);
	}
	const bool relative_valid = max_idx < prim.vertex_count;
	const bool absolute_valid = min_idx >= prim.vertex_offset &&
			static_cast<uint32_t>(max_idx) - prim.vertex_offset < prim.vertex_count;
	const bool use_absolute = absolute_valid && !relative_valid;

	auto to_local = [&](uint16_t idx, bool &ok) -> uint16_t {
		if (!use_absolute) {
			if (idx >= prim.vertex_count) {
				ok = false;
				return 0;
			}
			return idx;
		}
		if (idx < prim.vertex_offset) {
			ok = false;
			return 0;
		}
		const uint32_t local = static_cast<uint32_t>(idx) - prim.vertex_offset;
		if (local >= prim.vertex_count) {
			ok = false;
			return 0;
		}
		return static_cast<uint16_t>(local);
	};

	bool ok = true;
	if (prim.topology == THREEDI_IR_TOPOLOGY_TRIANGLES) {
		out.reserve(prim.index_count);
		for (uint32_t i = 0; i + 2 < prim.index_count; i += 3) {
			const uint16_t a = to_local(raw[i], ok);
			const uint16_t b = to_local(raw[i + 1], ok);
			const uint16_t c = to_local(raw[i + 2], ok);
			if (!ok) {
				return false;
			}
			if (a == b || b == c || a == c) {
				continue;
			}
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
	} else {
		out.reserve(static_cast<size_t>(prim.index_count) * 3);
		for (uint32_t i = 0; i + 2 < prim.index_count; ++i) {
			const bool odd = (i & 1u) != 0u;
			const uint16_t a = to_local(raw[i], ok);
			const uint16_t b = to_local(raw[i + (odd ? 2 : 1)], ok);
			const uint16_t c = to_local(raw[i + (odd ? 1 : 2)], ok);
			if (!ok) {
				return false;
			}
			if (a == b || b == c || a == c) {
				continue;
			}
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
	}
	return true;
}

bool primitive_is_alpha(const ThreediIRLod &lod, size_t prim_index) {
	const ThreediIRPrimitive &prim = lod.primitives[prim_index];
	if (prim.part_index < 0 || static_cast<size_t>(prim.part_index) >= lod.part_count) {
		return false;
	}
	const ThreediIRPart &part = lod.parts[prim.part_index];
	const int alpha_start = part.primitive_start + part.opaque_count;
	const int alpha_end = alpha_start + part.alpha_count;
	return static_cast<int>(prim_index) >= alpha_start && static_cast<int>(prim_index) < alpha_end;
}

} // namespace

int NovaObjectData::get_light_count() const {
	return has_ir ? static_cast<int>(ir.light_count) : 0;
}

Array NovaObjectData::get_lights() const {
	Array result;
	if (!has_ir) {
		return result;
	}
	for (size_t i = 0; i < ir.light_count; ++i) {
		const ThreediIRLight &light = ir.lights[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["part_index"] = light.part_index;
		item["offset"] = godot_vec3(light.offset);
		item["attenuation_start"] = light.attenuation_start;
		item["attenuation_end"] = light.attenuation_end;
		item["color_start"] = Color(light.color_start[0], light.color_start[1], light.color_start[2]);
		item["color_end"] = Color(light.color_end[0], light.color_end[1], light.color_end[2]);
		item["style"] = light.style;
		item["phase"] = light.phase;
		item["rate"] = light.rate;
		item["flags"] = light.flags;
		item["falloff"] = light.falloff;
		item["type"] = light.light_type;
		result.push_back(item);
	}
	return result;
}

Dictionary NovaObjectData::get_light_info(int p_index) const {
	Dictionary info;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.light_count) {
		return info;
	}
	const ThreediIRLight &light = ir.lights[p_index];
	info["name"] = vformat("Light %d", p_index);
	info["position"] = godot_vec3(light.offset);
	info["atten_start"] = light.attenuation_start;
	info["atten_end"] = light.attenuation_end;
	info["color_start"] = Color(light.color_start[0], light.color_start[1], light.color_start[2], 1.0f);
	info["color_end"] = Color(light.color_end[0], light.color_end[1], light.color_end[2], 1.0f);
	info["falloff_deg"] = static_cast<int>(light.falloff);
	info["subobject"] = light.part_index;
	info["disable_corona"] = (light.flags & THREEDI_IR_LIGHT_FLAG_DISABLE_CORONA) != 0;
	info["disable_lightterrain"] = (light.flags & THREEDI_IR_LIGHT_FLAG_DISABLE_TERRAIN) != 0;
	info["disable_lightobjects"] = (light.flags & THREEDI_IR_LIGHT_FLAG_DISABLE_OBJECTS) != 0;
	info["colorgen_style"] = static_cast<int>(light.style);
	info["colorgen_phase"] = static_cast<int>(light.phase);
	info["colorgen_rate"] = static_cast<int>(light.rate);
	info["light_type"] = static_cast<int>(light.light_type);
	return info;
}

bool NovaObjectData::set_light_field(int p_index, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.light_count) {
		return false;
	}
	ThreediIRLight &light = ir.lights[p_index];
	const String key = p_key;
	auto set_flag = [&](uint8_t bit) {
		if (static_cast<bool>(p_value)) {
			light.flags |= bit;
		} else {
			light.flags &= ~bit;
		}
	};
	if (key == "position") {
		const Vector3 v = p_value;
		light.offset[0] = -v.x;
		light.offset[1] = v.y;
		light.offset[2] = v.z;
		_notify_object_changed(UPDATE_LGHT);
		return true;
	}
	if (key == "atten_start") { light.attenuation_start = static_cast<float>(p_value); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "atten_end") { light.attenuation_end = static_cast<float>(p_value); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "color_start") {
		const Color c = p_value;
		light.color_start[0] = c.r;
		light.color_start[1] = c.g;
		light.color_start[2] = c.b;
		_notify_object_changed(UPDATE_LGHT);
		return true;
	}
	if (key == "color_end") {
		const Color c = p_value;
		light.color_end[0] = c.r;
		light.color_end[1] = c.g;
		light.color_end[2] = c.b;
		_notify_object_changed(UPDATE_LGHT);
		return true;
	}
	if (key == "falloff_deg") { light.falloff = static_cast<float>(p_value); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "subobject") { light.part_index = static_cast<int32_t>(static_cast<int>(p_value)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "disable_corona") { set_flag(0x01); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "disable_lightterrain") { set_flag(0x02); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "disable_lightobjects") { set_flag(0x04); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "colorgen_style") { light.style = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "colorgen_phase") { light.phase = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "colorgen_rate") { light.rate = static_cast<uint16_t>(std::clamp(static_cast<int>(p_value), 0, 65535)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "light_type") { light.light_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_LGHT); return true; }
	return false;
}

int NovaObjectData::get_user_point_count() const {
	return has_ir ? static_cast<int>(ir.userpoint_count) : 0;
}

Dictionary NovaObjectData::get_user_point_info(int p_index) const {
	Dictionary info;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.userpoint_count) {
		return info;
	}
	const ThreediIRUserPoint &point = ir.userpoints[p_index];
	info["name"] = from_native(point.name);
	info["position"] = godot_vec3(point.position);
	info["rotation"] = godot_vec3(point.direction);
	info["subobject"] = point.part_index;
	info["point_type"] = point.type_code;
	return info;
}

Vector3 NovaObjectData::get_ground_anchor(int p_lod_index) const {
	// The model-space point that should sit at a placed object's stored position:
	// the "ground" userpoint if present, else part 0's bounding center (see
	// threedi_ir_ground_anchor). The helper returns IR axis order; godot_vec3
	// applies the single negate-x that maps it into render/model space, exactly as
	// get_user_point_info / build_lod_submeshes do for userpoints and part origins.
	if (!has_ir) {
		return Vector3();
	}
	float anchor[3];
	if (!threedi_ir_ground_anchor(&ir, p_lod_index, anchor)) {
		return Vector3();
	}
	return godot_vec3(anchor);
}

bool NovaObjectData::has_collision() const {
	if (!has_ir || ir.collision == nullptr) return false;
	const ThreediIRCollision *collision = ir.collision;
	if (collision->volume_count > 0) return true;
	if (!is_skinned(0)) return false;
	if (collision->face_count > 0 && collision->faces != nullptr &&
			collision->vertex_count > 0 && collision->vertices != nullptr &&
			collision->object_count > 0 && collision->objects != nullptr)
		return true;
	if (collision->objects != nullptr) {
		for (size_t i = 0; i < collision->object_count; ++i) {
			if (collision->objects[i].radius_fp16 > 0) return true;
		}
	}
	return false;
}

bool NovaObjectData::has_occlusion() const {
	return has_ir && ir.occlusion != nullptr && ir.occlusion->object_count > 0;
}

Array NovaObjectData::get_collision_volumes() const {
	// Expose the parsed collision bounding volumes (the engine's CB/CC collidable
	// primitives) in Godot model-local space. Each volume carries its AABB plus the
	// bounding planes that carve the convex region; callers build ConvexPolygonShape3D
	// hulls from them (editor picking now, runtime collision later).
	//
	// Coordinate frame: unlike render geometry (RDTA, which the importer stores already
	// converted to engine space, so the Godot boundary only needs godot_position's
	// negate-x), collision geometry (CVRT) is stored in *workspace* space with no
	// conversion -- confirmed in the OED exporter and validated here against the visual
	// mesh AABB. RDTA reaches engine space via (-y, z, x); composing that with
	// godot_position's negate-x gives the net workspace->Godot map (x, y, z) -> (y, z, x),
	// a pure cyclic axis rotation. Applying it makes a hull placed at the same transform
	// as the visual model coincide with it (empirically the best of the candidates: see
	// the Object Editor "Collision" overlay).
	Array out;
	if (!has_ir || ir.collision == nullptr) {
		return out;
	}
	const ThreediIRCollision *col = ir.collision;
	for (size_t i = 0; i < col->volume_count; ++i) {
		const ThreediIRCollisionVolume &v = col->volumes[i];
		// (x, y, z) -> (y, z, x); the cyclic rotation has no sign flips, so min stays min.
		const Vector3 gmin(v.min[1], v.min[2], v.min[0]);
		const Vector3 gmax(v.max[1], v.max[2], v.max[0]);
		Array planes;
		// plane_start / plane_count come straight from the on-disk model with no clamp
		// (threedi_ir_from_3di3), so a malformed file can make plane_count huge or plane_start out of
		// range. Planes are contiguous, so clamp the window to [0, col->plane_count) and iterate that
		// instead of spinning over billions of out-of-range indices; the arithmetic is 64-bit so
		// plane_start + plane_count cannot signed-overflow.
		const int64_t start = v.plane_start;
		const int64_t plane_total = static_cast<int64_t>(col->plane_count);
		const int64_t begin = start > 0 ? start : 0;
		int64_t end = start + static_cast<int64_t>(v.plane_count);
		if (end > plane_total) {
			end = plane_total;
		}
		for (int64_t idx = begin; idx < end; ++idx) {
			const ThreediIRCollisionPlane &pl = col->planes[static_cast<size_t>(idx)];
			// The map is orthonormal, so the normal rotates the same way and the plane's
			// perpendicular offset is preserved in magnitude. The stored convention is
			// `normal.dot(p) + distance == 0` (offset is the *negated* signed distance,
			// verified against the volume AABBs), whereas Godot's Plane(normal, d) means
			// `normal.dot(p) == d`; hence the negation.
			const Vector3 n(pl.normal[1], pl.normal[2], pl.normal[0]);
			planes.push_back(Plane(n, -pl.distance));
		}
		Dictionary d;
		d["type"] = v.type;
		d["flags"] = v.flags;
		d["min"] = gmin;
		d["max"] = gmax;
		d["planes"] = planes;
		d["part_index"] = v.part_index;
		d["object_index"] = v.object_index;
		out.push_back(d);
	}
	return out;
}

Dictionary NovaObjectData::get_render_lod_info(int p_lod_index) const {
	Dictionary info;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return info;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	info["render_function"] = from_native(ir.render_function);
	info["threshold"] = lod.threshold;
	info["part_count"] = static_cast<int>(lod.part_count);
	info["render_object_count"] = static_cast<int>(lod.part_count);
	info["strip_count"] = static_cast<int>(lod.primitive_count);
	info["vertex_count"] = static_cast<int>(lod.vertex_count);
	info["index_count"] = static_cast<int>(lod.index_count);
	return info;
}

PackedVector3Array NovaObjectData::get_bone_origins(int p_lod_index) const {
	PackedVector3Array out;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return out;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	out.resize(static_cast<int64_t>(lod.part_count));
	for (size_t i = 0; i < lod.part_count; ++i) {
		const ThreediIRPart &part = lod.parts[i];
		// Raw native rel_position (parent-relative -- the parent-local FK offset the sampler wants),
		// NOT godot_vec3-flipped: bones stay engine-native (ADR 0007 conv #1 -- the mesh carries the
		// (-x,y,z) flip, the bones do not), matching how BadBone.position is consumed as-is by
		// sample_clip. Verified: this reproduces retail's modelDef+56 pivot (the rigid gun renders
		// correctly). [orig: BoneAnim_BuildWorldMatrices @0x40c400 reads the model pivot raw.]
		out[static_cast<int64_t>(i)] = Vector3(part.rel_position[0], part.rel_position[1], part.rel_position[2]);
	}
	return out;
}

PackedInt32Array NovaObjectData::get_bone_parents(int p_lod_index) const {
	PackedInt32Array out;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return out;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	out.resize(static_cast<int64_t>(lod.part_count));
	for (size_t i = 0; i < lod.part_count; ++i) {
		// Raw parent index (the root references itself in the file; the sampler normalizes).
		out[static_cast<int64_t>(i)] = lod.parts[i].parent_index;
	}
	return out;
}

bool NovaObjectData::is_skinned(int p_lod_index) const {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	if (ir.mesh_type == THREEDI_IR_MESH_SKINNED) {
		return true;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.primitives == nullptr) {
		return false;
	}
	for (size_t i = 0; i < lod.primitive_count; ++i) {
		if (lod.primitives[i].bone_table_length > 0) {
			return true;
		}
	}
	return false;
}

Array NovaObjectData::get_lod_surfaces(int p_lod_index) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.vertices == nullptr || lod.indices == nullptr || lod.primitives == nullptr) {
		return result;
	}

	for (size_t prim_index = 0; prim_index < lod.primitive_count; ++prim_index) {
		const ThreediIRPrimitive &prim = lod.primitives[prim_index];
		std::vector<uint16_t> decoded_indices;
		if (!decode_primitive_indices(lod, prim, decoded_indices)) {
			continue;
		}

		bool has_tangents = true;
		for (uint32_t i = 0; i < prim.vertex_count; ++i) {
			if (!vertex_has_tangents(lod.vertices[prim.vertex_offset + i])) {
				has_tangents = false;
				break;
			}
		}

		PackedVector3Array vertices;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		PackedVector2Array uvs2;
		PackedFloat32Array tangents;
		PackedInt32Array indices;
		// Per-vertex skinning, emitted only for skinned primitives. ARRAY_BONES carries 4
		// *skeleton* bone indices (the per-vertex bone_indices are local indices into this
		// primitive's bone_table, which maps local -> skeleton; we remap here so the owner
		// can bind one whole-skeleton Skin). ARRAY_WEIGHTS carries the 4 matching weights.
		// [orig: the runtime skins via the .bad skeleton; bone_table is the per-strip remap.]
		const bool skinned = prim.bone_table_length > 0;
		PackedInt32Array bones;
		PackedFloat32Array weights;
		auto get_vertex = [&](uint16_t local_index) -> const ThreediIRVertex * {
			const uint32_t src_index = prim.vertex_offset + static_cast<uint32_t>(local_index);
			if (src_index >= lod.vertex_count) {
				return nullptr;
			}
			return &lod.vertices[src_index];
		};
		auto push_vertex = [&](const ThreediIRVertex &v) {
			const Vector3 normal = godot_normal(v);
			const Vector3 tangent(-v.tangent[0], v.tangent[1], v.tangent[2]);
			const Vector3 bitangent(-v.bitangent[0], v.bitangent[1], v.bitangent[2]);
			vertices.push_back(godot_position(v));
			normals.push_back(normal);
			uvs.push_back(Vector2(v.uv0[0], v.uv0[1]));
			uvs2.push_back(Vector2(v.uv1[0], v.uv1[1]));
			if (has_tangents) {
				const float w = normal.cross(tangent).dot(bitangent) < 0.0f ? -1.0f : 1.0f;
				tangents.push_back(tangent.x);
				tangents.push_back(tangent.y);
				tangents.push_back(tangent.z);
				tangents.push_back(w);
			}
			if (skinned) {
				for (int k = 0; k < 4; ++k) {
					const int local = static_cast<int>(v.bone_indices[k]);
					const int bone = (local >= 0 && local < prim.bone_table_length)
							? static_cast<int>(prim.bone_table[local])
							: 0;
					bones.push_back(bone);
				}
				float w0 = v.bone_weights[0];
				float w1 = v.bone_weights[1];
				float w2 = v.bone_weights[2];
				float w3 = v.bone_weights[3];
				float sum = w0 + w1 + w2 + w3;
				if (sum <= 1e-6f) {  // degenerate: pin fully to the first influence
					w0 = 1.0f;
					w1 = w2 = w3 = 0.0f;
					sum = 1.0f;
				}
				weights.push_back(w0 / sum);
				weights.push_back(w1 / sum);
				weights.push_back(w2 / sum);
				weights.push_back(w3 / sum);
			}
			indices.push_back(vertices.size() - 1);
		};
		auto push_triangle = [&](uint16_t a, uint16_t b, uint16_t c) {
			const ThreediIRVertex *va = get_vertex(a);
			const ThreediIRVertex *vb = get_vertex(b);
			const ThreediIRVertex *vc = get_vertex(c);
			if (va == nullptr || vb == nullptr || vc == nullptr) {
				return;
			}

			push_vertex(*va);
			push_vertex(*vb);
			push_vertex(*vc);
		};

		for (size_t i = 0; i + 2 < decoded_indices.size(); i += 3) {
			push_triangle(decoded_indices[i], decoded_indices[i + 1], decoded_indices[i + 2]);
		}

		if (vertices.is_empty()) {
			continue;
		}
		Dictionary surface;
		surface["primitive_index"] = static_cast<int64_t>(prim_index);
		surface["material_index"] = prim.material_index;
		surface["material_array_index"] = material_array_index_for_id(ir, prim.material_index);
		surface["part_index"] = prim.part_index;
		surface["vertex_offset"] = static_cast<int64_t>(prim.vertex_offset);
		surface["vertices"] = vertices;
		surface["normals"] = normals;
		surface["uvs"] = uvs;
		surface["uvs2"] = uvs2;
		if (!tangents.is_empty() && tangents.size() == vertices.size() * 4) {
			surface["tangents"] = tangents;
		}
		if (skinned && bones.size() == vertices.size() * 4 && weights.size() == vertices.size() * 4) {
			surface["bones"] = bones;
			surface["weights"] = weights;
			surface["is_skinned"] = true;
		}
		surface["indices"] = indices;
		result.push_back(surface);
	}
	return result;
}

uint64_t NovaObjectData::_submesh_cache_key(int p_lod_index, bool p_skeletal, int p_bone_count, bool p_native_frame) {
	return static_cast<uint64_t>(p_lod_index) |
			(static_cast<uint64_t>(p_skeletal ? 1 : 0) << 16) |
			(static_cast<uint64_t>(p_native_frame ? 1 : 0) << 17) |
			(static_cast<uint64_t>(p_bone_count) << 24);
}

Array NovaObjectData::build_lod_submeshes(int p_lod_index, bool p_skeletal, int p_bone_count,
		bool p_native_frame) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	// Memo hit: hand back a deep copy of the ENTRY dictionaries (so a caller's
	// edits never taint the cache) whose ArrayMesh refs stay SHARED —
	// Array::duplicate(true) does not duplicate Resources, and that sharing is
	// the point: N models from one data render one set of meshes.
	const uint64_t cache_key = _submesh_cache_key(p_lod_index, p_skeletal, p_bone_count, p_native_frame);
	const auto cached = submesh_cache.find(cache_key);
	if (cached != submesh_cache.end()) {
		return cached->second.duplicate(true);
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	const Array surfaces = get_lod_surfaces(p_lod_index);
	for (int i = 0; i < surfaces.size(); ++i) {
		const Dictionary surface = surfaces[i];
		const int part_index = static_cast<int>(surface.get("part_index", 0));
		const int material_array_index = static_cast<int>(surface.get("material_array_index", surface.get("material_index", 0)));

		PackedVector3Array vertices = surface.get("vertices", PackedVector3Array());
		if (vertices.is_empty()) {
			continue;
		}
		PackedVector3Array normals = surface.get("normals", PackedVector3Array());
		PackedFloat32Array tangents = surface.get("tangents", PackedFloat32Array());
		PackedInt32Array mesh_indices = surface.get("indices", PackedInt32Array());
		if (p_native_frame) {
			// Undo the baked (-x,y,z) import flip: native positions/normals/tangents, and
			// reverse each triangle's winding — the source D3D clockwise-front order is only
			// CCW-correct for Godot BECAUSE of that mirror; unmirrored it must be re-reversed.
			// (See header: the FP viewmodel path, paired with NovaSkeletalAnim model_bind.)
			for (int v = 0; v < vertices.size(); ++v) {
				const Vector3 p = vertices[v];
				vertices.set(v, Vector3(-p.x, p.y, p.z));
			}
			for (int v = 0; v < normals.size(); ++v) {
				const Vector3 n = normals[v];
				normals.set(v, Vector3(-n.x, n.y, n.z));
			}
			for (int t = 0; t + 3 < tangents.size(); t += 4) {
				tangents.set(t, -tangents[t]);          // tangent x back to native
				tangents.set(t + 3, -tangents[t + 3]);  // bitangent handedness follows the mirror
			}
			for (int t = 0; t + 2 < mesh_indices.size(); t += 3) {
				const int32_t tmp = mesh_indices[t + 1];
				mesh_indices.set(t + 1, mesh_indices[t + 2]);
				mesh_indices.set(t + 2, tmp);
			}
		}

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TEX_UV] = surface.get("uvs", PackedVector2Array());
		arrays[Mesh::ARRAY_TEX_UV2] = surface.get("uvs2", PackedVector2Array());
		if (!tangents.is_empty() && tangents.size() == vertices.size() * 4) {
			arrays[Mesh::ARRAY_TANGENT] = tangents;
		}
		// Skinning arrays (4 bones + 4 weights per vertex). ArrayMesh requires both present
		// together; only attach when both are valid for this surface's vertex count.
		PackedInt32Array bones = surface.get("bones", PackedInt32Array());
		PackedFloat32Array weights = surface.get("weights", PackedFloat32Array());
		bool surface_skinned = bones.size() == vertices.size() * 4 && weights.size() == vertices.size() * 4;
		// Rigid "fake skinning": when a skeleton will be applied (p_skeletal) but this surface
		// has no per-vertex skin, fully weight every vertex (1.0) to a single bone = the part's
		// subobject index. The .bad skeleton is authored so subobject i <-> bone i, so the rigid
		// part follows that bone. [orig: rigid weapon parts ride a bone via fake skinning.]
		if (!surface_skinned && p_skeletal) {
			const int bone = p_bone_count > 0 ? CLAMP(part_index, 0, p_bone_count - 1) : MAX(part_index, 0);
			const int vcount = static_cast<int>(vertices.size());
			bones.resize(vcount * 4);
			weights.resize(vcount * 4);
			for (int v = 0; v < vcount; ++v) {
				bones.set(v * 4 + 0, bone);
				bones.set(v * 4 + 1, 0);
				bones.set(v * 4 + 2, 0);
				bones.set(v * 4 + 3, 0);
				weights.set(v * 4 + 0, 1.0f);
				weights.set(v * 4 + 1, 0.0f);
				weights.set(v * 4 + 2, 0.0f);
				weights.set(v * 4 + 3, 0.0f);
			}
			surface_skinned = true;
		}
		if (surface_skinned) {
			arrays[Mesh::ARRAY_BONES] = bones;
			arrays[Mesh::ARRAY_WEIGHTS] = weights;
		}
		arrays[Mesh::ARRAY_INDEX] = mesh_indices;

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh->surface_set_name(0, vformat("material_%d", material_array_index));

		Vector3 abs = Vector3();
		int parent_index = -1;
		if (part_index >= 0 && static_cast<size_t>(part_index) < lod.part_count) {
			const ThreediIRPart &part = lod.parts[part_index];
			abs = godot_vec3(part.abs_position);
			parent_index = part.parent_index;
		}

		const size_t prim_index = static_cast<size_t>(static_cast<int64_t>(surface.get("primitive_index", 0)));
		Dictionary entry;
		entry["robj_index"] = part_index;
		entry["part_index"] = part_index;
		entry["material_index"] = material_array_index;
		entry["source_material_index"] = surface.get("material_index", material_array_index);
		entry["is_alpha"] = prim_index < lod.primitive_count ? primitive_is_alpha(lod, prim_index) : false;
		entry["mesh"] = mesh;
		entry["abs"] = abs;
		entry["parent_index"] = parent_index;
		entry["primitive_index"] = surface.get("primitive_index", i);
		entry["is_skinned"] = surface_skinned;
		result.push_back(entry);
	}
	// Keep the pristine copy; the caller gets its own entry dictionaries. The
	// empty early-outs above are deliberately NOT cached.
	submesh_cache.emplace(cache_key, result);
	return result.duplicate(true);
}

Error NovaObjectData::set_light_colors(int p_light_index, const Color &p_start, const Color &p_end) {
	if (!has_ir || p_light_index < 0 || static_cast<size_t>(p_light_index) >= ir.light_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediIRLight &light = ir.lights[p_light_index];
	light.color_start[0] = p_start.r;
	light.color_start[1] = p_start.g;
	light.color_start[2] = p_start.b;
	light.color_end[0] = p_end.r;
	light.color_end[1] = p_end.g;
	light.color_end[2] = p_end.b;
	_notify_object_changed(UPDATE_LGHT);
	return OK;
}
