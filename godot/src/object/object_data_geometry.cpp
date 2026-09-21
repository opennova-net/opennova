// ObjectData — geometry views: LOD surfaces and memoized submesh builds,
// bones/skinning, collision volumes, lights and user points.
#include "object/object_data_internal.h"
#include "object/model_light.h"
#include "object/model_user_point.h"

#include <runtime/world/model_geometry.h> // model_has_collision / model_is_skinned (ADR 0016: one impl)
#include <runtime/renderer/model_mesh_prepare.h>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <vector>

using namespace novaobj;
using namespace opennova::threedi;

namespace {

Array pack_mesh_arrays(const opennova::renderer::PreparedMeshSurface &surface) {
	PackedVector3Array vertices, normals;
	PackedVector2Array uvs, uvs2;
	PackedFloat32Array tangents, weights;
	PackedInt32Array bones, indices;
	for (const auto &v : surface.vertices) vertices.push_back(Vector3(v[0], v[1], v[2]));
	for (const auto &v : surface.normals) normals.push_back(Vector3(v[0], v[1], v[2]));
	for (const auto &v : surface.uvs) uvs.push_back(Vector2(v[0], v[1]));
	for (const auto &v : surface.uvs2) uvs2.push_back(Vector2(v[0], v[1]));
	for (const auto &v : surface.tangents)
		for (const auto value : v) tangents.push_back(value);
	for (const auto &v : surface.bones)
		for (const auto value : v) bones.push_back(value);
	for (const auto &v : surface.weights)
		for (const auto value : v) weights.push_back(value);
	for (const auto value : surface.indices) indices.push_back(value);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uvs2;
	if (!tangents.is_empty()) arrays[Mesh::ARRAY_TANGENT] = tangents;
	if (!bones.is_empty()) {
		arrays[Mesh::ARRAY_BONES] = bones;
		arrays[Mesh::ARRAY_WEIGHTS] = weights;
	}
	arrays[Mesh::ARRAY_INDEX] = indices;
	return arrays;
}

} // namespace

int ObjectData::get_light_count() const {
	return source_model_ ? static_cast<int>(native_model().light_count) : 0;
}

Ref<ModelLight> ObjectData::get_light_info(int p_index) const {
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().light_count) {
		return Ref<ModelLight>();
	}
	const ThreediLight &light = native_model().lights[p_index];
	Ref<ModelLight> info;
	info.instantiate();
	info->set_name(vformat("Light %d", p_index));
	info->set_position(godot_vec3(light.offset));
	info->set_atten_start(light.atten_start);
	info->set_atten_end(light.atten_end);
	info->set_color_start(Color(light.color_start[2] / 255.0f, light.color_start[1] / 255.0f, light.color_start[0] / 255.0f, 1.0f));
	info->set_color_end(Color(light.color_end[2] / 255.0f, light.color_end[1] / 255.0f, light.color_end[0] / 255.0f, 1.0f));
	info->set_falloff_deg(static_cast<int>(light.falloff_byte));
	info->set_subobject(static_cast<int>(light.subobj_index));
	info->set_disable_corona((light.flags & THREEDI_LIGHT_FLAG_DISABLE_CORONA) != 0);
	info->set_disable_lightterrain((light.flags & THREEDI_LIGHT_FLAG_DISABLE_TERRAIN) != 0);
	info->set_disable_lightobjects((light.flags & THREEDI_LIGHT_FLAG_DISABLE_OBJECTS) != 0);
	info->set_colorgen_style(static_cast<int>(light.style));
	info->set_colorgen_phase(static_cast<int>(light.phase));
	info->set_colorgen_rate(static_cast<int>(light.rate));
	info->set_light_type((light.flags & THREEDI_LIGHT_FLAG_TYPE_TARGET) != 0 ? 1 : 0);
	return info;
}

int ObjectData::get_user_point_count() const {
	return source_model_ ? static_cast<int>(native_model().user_point_count) : 0;
}

Ref<ModelUserPoint> ObjectData::get_user_point_info(int p_index) const {
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().user_point_count) {
		return Ref<ModelUserPoint>();
	}
	const ThreediUserPoint &point = native_model().user_points[p_index];
	float position[3];
	float direction[3];
	threedi_user_point_position(&point, position);
	threedi_user_point_direction(&point, direction);
	Ref<ModelUserPoint> info;
	info.instantiate();
	info->assign(from_native(point.name), godot_vec3(position), godot_vec3(direction),
			point.subobject_index, point.userpoint_type);
	return info;
}

int ObjectData::get_user_point_bone_mask(const String &p_name) const {
	if (!source_model_) return 0;
	return threedi_3di3_user_point_mask(&native_model(),
			p_name.utf8().get_data());
}

bool ObjectData::has_collision() const {
	// One implementation per engine fact (ADR 0016): the predicate lives in
	// engine/runtime/world beside the collision model build it gates.
	return source_model_ &&
			opennova::world::model_has_collision(native_model());
}

bool ObjectData::has_occlusion() const {
	return source_model_ && native_model().occlusion_object_count > 0;
}

Array ObjectData::get_collision_volumes() const {
	// Expose the parsed collision bounding volumes (the engine's CB/CC collidable
	// primitives) in Godot model-local space. Each volume carries its AABB plus the
	// bounding planes that carve the convex region; runtime callers build
	// ConvexPolygonShape3D hulls from them.
	//
	// Coordinate frame: unlike render geometry (RDTA, which the 3DI reader stores already
	// converted to engine space, so the Godot boundary only needs the render frame's
	// negate-x), collision geometry (CVRT) is stored in *workspace* space with no
	// conversion, as validated against the visual mesh AABB. RDTA reaches engine
	// space via (-y, z, x); composing that with
	// the render frame's negate-x gives the net workspace->Godot map (x, y, z) -> (y, z, x),
	// a pure cyclic axis rotation. Applying it makes a hull placed at the same transform
	// as the visual model coincide with it.
	Array out;
	if (!source_model_ || native_model().collision == nullptr) {
		return out;
	}
	const ThreediCollisionModel *col = native_model().collision;

	// Per-volume owning object/part: BVOL runs are sequential per COBJ; volumes
	// beyond the owned runs are retail's dead trailing data (kept, unowned).
	std::vector<int32_t> volume_object(col->volume_count, -1);
	std::vector<int32_t> volume_part(col->volume_count, 0);
	{
		size_t vol_cursor = 0;
		for (size_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
			const ThreediCollisionObject &obj = col->objects[obj_idx];
			for (int32_t v = 0; v < obj.num_bounding_volumes &&
					vol_cursor < col->volume_count; ++v, ++vol_cursor) {
				volume_object[vol_cursor] = static_cast<int32_t>(obj_idx);
				volume_part[vol_cursor] = obj.parent_subobject_index;
			}
		}
	}

	int64_t plane_cursor = 0;
	for (size_t i = 0; i < col->volume_count; ++i) {
		const ThreediBoundingVolume &v = col->volumes[i];
		// (x, y, z) -> (y, z, x); the cyclic rotation has no sign flips, so min stays min.
		const Vector3 gmin(v.min_y_fp16 / 65536.0f, v.min_z_fp16 / 65536.0f, v.min_x_fp16 / 65536.0f);
		const Vector3 gmax(v.max_y_fp16 / 65536.0f, v.max_z_fp16 / 65536.0f, v.max_x_fp16 / 65536.0f);
		Array planes;
		// Plane windows are consecutive across the BVOL pool. plane_count comes
		// straight from the on-disk model with no clamp, so a malformed file can
		// make it huge; clamp the window to [0, col->plane_count) and iterate
		// that instead of spinning over billions of out-of-range indices; the
		// arithmetic is 64-bit so the cursor cannot signed-overflow.
		const int64_t start = plane_cursor;
		plane_cursor += static_cast<int64_t>(v.plane_count);
		const int64_t plane_total = static_cast<int64_t>(col->plane_count);
		const int64_t begin = std::clamp<int64_t>(start, 0, plane_total);
		const int64_t end = std::clamp<int64_t>(plane_cursor, begin, plane_total);
		for (int64_t idx = begin; idx < end; ++idx) {
			const ThreediBoundingPlane &pl = col->planes[static_cast<size_t>(idx)];
			// The map is orthonormal, so the normal rotates the same way and the plane's
			// perpendicular offset is preserved in magnitude. The stored convention is
			// `normal.dot(p) + distance == 0` (offset is the *negated* signed distance,
			// verified against the volume AABBs), whereas Godot's Plane(normal, d) means
			// `normal.dot(p) == d`; hence the negation.
			const Vector3 n(pl.normal[1], pl.normal[2], pl.normal[0]);
			planes.push_back(Plane(n, -pl.radius));
		}
		Dictionary d;
		d["type"] = v.collidable_type;
		d["flags"] = v.flags;
		d["min"] = gmin;
		d["max"] = gmax;
		d["planes"] = planes;
		d["part_index"] = volume_part[i];
		d["object_index"] = volume_object[i];
		out.push_back(d);
	}
	return out;
}

int ObjectData::get_part_anim_count(int p_lod_index) const {
	if (!source_model_ || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= native_model().lod_count) {
		return 0;
	}
	return static_cast<int>(
			native_model().lods[p_lod_index].part_animation_count);
}

PackedVector3Array ObjectData::get_bone_origins(int p_lod_index) const {
	PackedVector3Array out;
	if (!source_model_ || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= native_model().lod_count) {
		return out;
	}
	const ThreediLod &lod = native_model().lods[p_lod_index];
	out.resize(static_cast<int64_t>(lod.render_object_count));
	for (size_t i = 0; i < lod.render_object_count; ++i) {
		const ThreediRenderObject &part = lod.render_objects[i];
		// Raw native rel (parent-relative -- the parent-local FK offset the sampler wants),
		// NOT godot_vec3-flipped: bones stay engine-native (ADR 0007 conv #1 -- the mesh carries the
		// (-x,y,z) flip, the bones do not), matching how BadBone.position is consumed as-is by
		// sample_clip. Verified: this reproduces retail's modelDef+56 pivot (the rigid gun renders
		// correctly). [orig: BoneAnim_BuildWorldMatrices @0x40c400 reads the model pivot raw.]
		out[static_cast<int64_t>(i)] = Vector3(part.rel[0], part.rel[1], part.rel[2]);
	}
	return out;
}

PackedInt32Array ObjectData::get_bone_parents(int p_lod_index) const {
	PackedInt32Array out;
	if (!source_model_ || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= native_model().lod_count) {
		return out;
	}
	const ThreediLod &lod = native_model().lods[p_lod_index];
	out.resize(static_cast<int64_t>(lod.render_object_count));
	for (size_t i = 0; i < lod.render_object_count; ++i) {
		// Raw parent index (the root references itself in the file; the sampler normalizes).
		out[static_cast<int64_t>(i)] = lod.render_objects[i].parent_index;
	}
	return out;
}

bool ObjectData::is_skinned(int p_lod_index) const {
	// One implementation per engine fact (ADR 0016): delegates to
	// engine/runtime/world, beside the collision gate that consumes it.
	return source_model_ &&
			opennova::world::model_is_skinned(native_model(), p_lod_index);
}

uint64_t ObjectData::_submesh_cache_key(int p_lod_index, bool p_skeletal, int p_bone_count, bool p_native_frame) {
	return static_cast<uint64_t>(p_lod_index) |
			(static_cast<uint64_t>(p_skeletal ? 1 : 0) << 16) |
			(static_cast<uint64_t>(p_native_frame ? 1 : 0) << 17) |
			(static_cast<uint64_t>(p_bone_count) << 24);
}

Array ObjectData::build_lod_submeshes(int p_lod_index, bool p_skeletal, int p_bone_count,
		bool p_native_frame) const {
	Array result;
	if (!source_model_ || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= native_model().lod_count) {
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
	const auto surfaces = opennova::renderer::prepare_model_mesh(
			native_model(), p_lod_index, {p_skeletal, p_bone_count, p_native_frame});
	for (const auto &surface : surfaces) {
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, pack_mesh_arrays(surface));
		mesh->surface_set_name(0, vformat("material_%d", surface.material_array_index));

		Dictionary entry;
		entry["robj_index"] = surface.part_index;
		entry["part_index"] = surface.part_index;
		entry["material_index"] = surface.material_array_index;
		entry["source_material_index"] = surface.material_index;
		entry["is_alpha"] = surface.is_alpha;
		entry["mesh"] = mesh;
		entry["abs"] = Vector3(surface.abs[0], surface.abs[1], surface.abs[2]);
		entry["parent_index"] = surface.parent_index;
		entry["primitive_index"] = static_cast<int64_t>(surface.primitive_index);
		entry["is_skinned"] = !surface.bones.empty();
		result.push_back(entry);
	}
	// Keep the pristine copy; the caller gets its own entry dictionaries. The
	// empty early-outs above are deliberately NOT cached.
	submesh_cache.emplace(cache_key, result);
	return result.duplicate(true);
}
