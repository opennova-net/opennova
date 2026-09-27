// Sim-side model derivations from a parsed .3di (ADR 0028). Bodies moved
// verbatim from the shell binding's simulation_internal.h; the [orig]
// witnesses ride with them.
#include <runtime/world/model_geometry.h>
#include <base/io/fixed.h>
#include <runtime/world/entity.h>

#include <algorithm>
#include <cmath>

using namespace opennova::threedi;

namespace opennova::world {

// Build the runtime collision model from a parsed .3di CDTA block — the exact
// inverse of the parse scaling (BPLN normals int16 Q14 / 16384, distances + AABBs 16.16;
// engine/formats/threedi/threedi_3di3_read.cpp parse_bpln/parse_bvol). Sections mirror the COBJ
// grouping: CVRT/CNRM/CFAC/BVOL arrays are sequential per object, and each face's local
// CNRM index resolves against its object's run at build time, exactly the load-time
// fixup retail performs [orig: the per-COBJ normal-run fixup in the collision builder
// @ 0x5b3bf0]. Face-only Poly Collision LOD models remain valid without semantic
// volumes, and organic callers may explicitly retain COBJ sphere-only models for posed
// person collision.
bool collision_model_from_3di(const ThreediCollisionModel *col,
	                             opennova::world::CollisionModel &out,
	                             bool allow_sphere_only) {
	if (col == nullptr || !threedi_3di3_collision_is_runtime_safe(col)) return false;
	const bool has_face_mesh =
			col->face_count > 0 && col->faces != nullptr && col->vertex_count > 0 &&
			col->vertices != nullptr && col->object_count > 0 && col->objects != nullptr;
	bool has_person_spheres = false;
	if (allow_sphere_only && col->objects != nullptr) {
		for (size_t i = 0; i < col->object_count; ++i) {
			if (col->objects[i].radius > 0) {
				has_person_spheres = true;
				break;
			}
		}
	}
	if (col->volume_count == 0 && !has_face_mesh && !has_person_spheres)
		return false;
	auto fx = [](float v) { return static_cast<int32_t>(std::lround(v * io::kFp16OneD)); };

	out.vertices.reserve(col->vertex_count);
	for (size_t i = 0; i < col->vertex_count; ++i) {
		opennova::world::CollisionVertex v;
		for (int k = 0; k < 3; ++k) v.p[k] = fx(col->vertices[i].position[k]);
		out.vertices.push_back(v);
	}
	// The parser divided the authored signed Q14 CNRM words by 16384, so
	// multiplying by that power of two is an exact recovery.
	out.normals.reserve(col->normal_count);
	for (size_t i = 0; i < col->normal_count; ++i) {
		opennova::world::CollisionNormal n;
		for (int k = 0; k < 3; ++k)
			n.n[k] = static_cast<int16_t>(std::lround(col->normals[i].normal[k] * io::kFp14One));
		n.dominant_axis = col->normals[i].dominate_axis;
		out.normals.push_back(n);
	}
	// The legacy projectile path consumes the same CVRT run requantized to its
	// authored Q8 words. Parsed positions originated as Q8/256, so this
	// round-trip is exact while the indexed path above retains its Q16 view.
	out.face_vertices.reserve(col->vertex_count);
	for (size_t i = 0; i < col->vertex_count; ++i) {
		opennova::world::CollisionFaceVertex v;
		v.x = static_cast<int16_t>(std::lround(col->vertices[i].position[0] * 256.0f));
		v.y = static_cast<int16_t>(std::lround(col->vertices[i].position[1] * 256.0f));
		v.z = static_cast<int16_t>(std::lround(col->vertices[i].position[2] * 256.0f));
		out.face_vertices.push_back(v);
	}
	// One shared face vector carries both query representations: exact indexed
	// CVRT/CNRM fields and the embedded Q8/Q14 fields used by the older walker.
	out.faces.reserve(col->face_count);
	for (size_t i = 0; i < col->face_count; ++i) {
		const ThreediCollisionFace &sf = col->faces[i];
		opennova::world::CollisionFace f;
		for (int k = 0; k < 3; ++k) {
			f.vertex_index[k] = sf.vert_index[k];
			f.v[k] = sf.vert_index[k];
			f.normal[k] = 0;
			f.min[k] = 0;
			f.max[k] = 0;
		}
		f.min[0] = sf.min_x_fp16;
		f.min[1] = sf.min_y_fp16;
		f.min[2] = sf.min_z_fp16;
		f.max[0] = sf.max_x_fp16;
		f.max[1] = sf.max_y_fp16;
		f.max[2] = sf.max_z_fp16;
		f.normal_index = sf.normal_index;
		f.axis = 0;
		f.plane_dist = sf.plane_dist_fp16;
		f.material_flags = sf.material_flags;
		f.poly_type = sf.poly_type;
		f.flags = sf.material_flags;
		f.material = sf.poly_type;
		out.faces.push_back(f);
	}
	// Resolve each face's embedded Q14 normal from its object-local CNRM run
	// [orig: the per-COBJ normal-run fixup in the collision builder @ 0x5b3bf0;
	// walked by Physics_RaycastAgainstBoneCollision @ 0x4e4cb0].
	{
		size_t face_cursor = 0;
		size_t normal_base = 0;
		for (size_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
			const ThreediCollisionObject &object = col->objects[obj_idx];
			for (int32_t fi = 0; fi < object.num_faces && face_cursor < out.faces.size();
			     ++fi, ++face_cursor) {
				opennova::world::CollisionFace &f = out.faces[face_cursor];
				const int32_t ni = f.normal_index;
				const size_t resolved = normal_base + static_cast<size_t>(ni);
				if (ni >= 0 && resolved < out.normals.size()) {
					const opennova::world::CollisionNormal &n = out.normals[resolved];
					f.normal[0] = n.n[0];
					f.normal[1] = n.n[1];
					f.normal[2] = n.n[2];
					f.axis = n.dominant_axis;
				}
			}
			normal_base += static_cast<size_t>(object.num_normals);
		}
	}

	out.planes.reserve(col->plane_count);
	for (size_t i = 0; i < col->plane_count; ++i) {
		const ThreediBoundingPlane &sp = col->planes[i];
		opennova::world::CollisionPlane p;
		p.flags = sp.flags;
		p.nx = static_cast<int16_t>(std::lround(sp.normal[0] * io::kFp14One));
		p.ny = static_cast<int16_t>(std::lround(sp.normal[1] * io::kFp14One));
		p.nz = static_cast<int16_t>(std::lround(sp.normal[2] * io::kFp14One));
		p.dist = fx(sp.radius);
		out.planes.push_back(p);
	}

	// BPLN windows are consecutive across the BVOL pool; the running prefix is
	// each volume's plane_start. Authored 16.16 bounds carry over verbatim.
	out.volumes.reserve(col->volume_count);
	int32_t plane_cursor = 0;
	for (size_t i = 0; i < col->volume_count; ++i) {
		const ThreediBoundingVolume &sv = col->volumes[i];
		opennova::world::CollisionVolume v;
		v.type = sv.collidable_type;
		v.flags = static_cast<uint32_t>(sv.flags);
		v.min_x = sv.min_x_fp16;
		v.max_x = sv.max_x_fp16;
		v.min_y = sv.min_y_fp16;
		v.max_y = sv.max_y_fp16;
		v.min_z = sv.min_z_fp16;
		v.max_z = sv.max_z_fp16;
		v.plane_start = plane_cursor;
		v.plane_count = sv.plane_count;
		plane_cursor += sv.plane_count;
		out.volumes.push_back(v);
	}

	if (col->object_count == 0) {
		// Legacy ungrouped convex-only block.
		out.sections.assign(1, {});
		out.sections[0].volume_count = static_cast<int32_t>(col->volume_count);
		return true;
	}

	out.sections.assign(col->object_count, {});
	int32_t vertex_cursor = 0, normal_cursor = 0, face_cursor = 0, volume_cursor = 0;
	for (size_t s = 0; s < col->object_count; ++s) {
		const ThreediCollisionObject &object = col->objects[s];
		opennova::world::CollisionSection &sec = out.sections[s];
        sec.flags = static_cast<uint32_t>(object.unk0); // [orig: COBJ copy @0x5B3BF0]
		sec.vertex_start = vertex_cursor;
		sec.vertex_count = object.num_vertices;
		sec.normal_start = normal_cursor;
		sec.normal_count = object.num_normals;
		sec.face_start = face_cursor;
		sec.face_count = object.num_faces;
		sec.face_vertex_start = vertex_cursor;
		sec.face_vertex_count = object.num_vertices;
		sec.volume_start = volume_cursor;
		sec.volume_count = object.num_bounding_volumes;
		for (int k = 0; k < 3; ++k) {
			sec.offset[k] = object.offset[k];
			sec.center[k] = object.med[k];
		}
		sec.min_x = object.min[0]; sec.max_x = object.max[0];
		sec.min_y = object.min[1]; sec.max_y = object.max[1];
		sec.min_z = object.min[2]; sec.max_z = object.max[2];
		sec.radius = object.radius;
		// Empty COBJ records can carry inverted/sentinel bounds. Preserve valid
		// authored bounds exactly; otherwise let finalize_sections derive them
		// from that object's volume or vertex run.
		sec.authored_bounds =
				object.radius >= 0 &&
				object.min[0] <= object.max[0] &&
				object.min[1] <= object.max[1] &&
				object.min[2] <= object.max[2];
		vertex_cursor += sec.vertex_count;
		normal_cursor += sec.normal_count;
		face_cursor += sec.face_count;
		volume_cursor += sec.volume_count;
	}
	return true;
}

opennova::renderer::ObjectProjectionSphere collision_projection_sphere_from_3di(
    const Threedi3di3 &model, int32_t runtime_scale_q16,
    int32_t definition_scale_q16, bool zero_center) {
  // No collision block: both producers skip the stamp and the entity keeps
  // its zero spawn words, which the collector then projects as radius zero.
  // [orig: Entity_InitFromModel @0x40de97; Entity_ComputeBoundingSphere @0x5c69be]
  if (model.collision == nullptr) {
    opennova::renderer::ObjectProjectionSphere unstamped;
    unstamped.valid = true;
    return unstamped;
  }
  const auto &bounds = model.collision->model_data;
  std::array<int32_t, 3> minimum{}, maximum{};
  for (int axis = 0; axis < 3; ++axis) {
    minimum[axis] = bounds.has_bbox_fp16 ? bounds.bbox_fp16[axis]
        : io::float_to_fp16_16_round_sat(bounds.bbox[axis]);
    maximum[axis] = bounds.has_bbox_fp16 ? bounds.bbox_fp16[axis + 3]
        : io::float_to_fp16_16_round_sat(bounds.bbox[axis + 3]);
  }
  return opennova::renderer::object_projection_sphere_from_bounds_q16(
      minimum, maximum, runtime_scale_q16, definition_scale_q16, zero_center);
}

// The model bound-sphere radius. Production files use GHDR's exact Q16 value;
// only headerless in-memory fixtures derive it from LOD-0 part spheres (and,
// for degenerate fixture models, strip boxes).
int32_t model_bound_radius_q16_from_3di(const Threedi3di3 &model) {
	// Production files retain GHDR's exact signed carrier (the on-disk field
	// is authored as unsigned Q16.16). Retail stores this as MODEL gpm[5] and
	// Entity_InitFromModel reads it behind the collision-block gate.
	// [orig: ModSuperOed WriteGHDR @0x452B40; Entity_InitFromModel @0x40dcd7/@0x40de8f]
	if (model.header.has_header) return model.header.max_radius_fp16;
	if (model.lod_count == 0 || model.lods == nullptr) return 0;
	const ThreediLod &lod = model.lods[0];
	float r = 0.0f;
	for (size_t i = 0; lod.render_objects != nullptr && i < lod.render_object_count; ++i) {
		const ThreediRenderObject &p = lod.render_objects[i];
		const float cx = p.abs[0] + p.bounding_center[0];
		const float cy = p.abs[1] + p.bounding_center[1];
		const float cz = p.abs[2] + p.bounding_center[2];
		const float c = std::sqrt(cx * cx + cy * cy + cz * cz);
		if (c + p.bounding_radius > r) r = c + p.bounding_radius;
	}
	if (r <= 0.0f) {
		for (size_t i = 0; lod.strips != nullptr && i < lod.strip_count; ++i) {
			const ThreediTriangleStrip &pr = lod.strips[i];
			for (int a = 0; a < 3; ++a) {
				r = std::max(r, std::abs(pr.min[a]));
				r = std::max(r, std::abs(pr.max[a]));
			}
		}
	}
	return static_cast<int32_t>(std::lround(static_cast<double>(r) * io::kFp16OneD));
}

float model_bound_radius_from_3di(const Threedi3di3 &model) {
	return static_cast<float>(model_bound_radius_q16_from_3di(model)) / io::kFp16One;
}

int32_t model_bound_floor_q16(const opennova::threedi::Threedi3di3 &model) {
	if (model.collision == nullptr) return 0;
	return static_cast<int32_t>(
			std::lround(static_cast<double>(model.collision->model_data.bbox[2]) * 65536.0));
}

int32_t entity_bound_radius_q16(const EntityBoundRadiusInputs &inputs) {
	// [orig: Entity_InitFromModel @0x40dc30: CDTA gate @0x40de8f,
	// scaled base @0x40e052, signed first-husk max @0x40e062..0x40e06f,
	// and unconditional +0x1000 padding @0x40e076]
	if (!inputs.has_collision_block) return 0;
	int32_t radius = inputs.model_radius_q16;
	if (inputs.uniform_scale_q16 != 0)
		radius = world::retail_q16_mul_rhu(radius, inputs.uniform_scale_q16);
	if (inputs.has_first_husk)
		radius = std::max(radius, inputs.first_husk_radius_q16);
	return static_cast<int32_t>(static_cast<uint32_t>(radius) + 0x1000u);
}

// Build the runtime occlusion model from the parsed OCCL tables — the 60 B
// portal-face records with their sequential slices (the per-record starts are
// running prefixes over the OOBJ counts, mirroring the arena assignment of
// [orig: ThreediGp_LoadOcclusionModelData @ 0x5b4a00]). The OFAC dwords decode as the
// 12 B record: bytes 0-2 = vertex indices, byte 3 = plane index, then the 3
// edge words (bit 15 = winding).
bool occlusion_model_from_3di(const Threedi3di3 &model,
                             opennova::world::OcclusionModel &out) {
	if (model.occlusion_object_count == 0) return false;
	out.vertices.reserve(model.occlusion_vertex_count);
	for (size_t i = 0; i < model.occlusion_vertex_count; ++i) {
		opennova::world::OcclusionVertex v;
		v.p[0] = model.occlusion_vertices[i].position[0];
		v.p[1] = model.occlusion_vertices[i].position[1];
		v.p[2] = model.occlusion_vertices[i].position[2];
		out.vertices.push_back(v);
	}
	out.planes.reserve(model.occlusion_plane_count);
	for (size_t i = 0; i < model.occlusion_plane_count; ++i) {
		opennova::world::OcclusionPlane p;
		p.normal[0] = model.occlusion_planes[i].normal[0];
		p.normal[1] = model.occlusion_planes[i].normal[1];
		p.normal[2] = model.occlusion_planes[i].normal[2];
		p.d = model.occlusion_planes[i].radius;
		out.planes.push_back(p);
	}
	out.faces.reserve(model.occlusion_face_count);
	for (size_t i = 0; i < model.occlusion_face_count; ++i) {
		const ThreediOcclusionFace &sf = model.occlusion_faces[i];
		opennova::world::OcclusionFaceRec f;
		f.v[0] = static_cast<uint8_t>(sf.raw_indices & 0xFF);
		f.v[1] = static_cast<uint8_t>((sf.raw_indices >> 8) & 0xFF);
		f.v[2] = static_cast<uint8_t>((sf.raw_indices >> 16) & 0xFF);
		f.plane = static_cast<uint8_t>((sf.raw_indices >> 24) & 0xFF);
		f.edge[0] = static_cast<uint16_t>(sf.edge_data & 0xFFFF);
		f.edge[1] = static_cast<uint16_t>(sf.edge_data >> 16);
		f.edge[2] = static_cast<uint16_t>(sf.other_edge_data & 0xFFFF);
		out.faces.push_back(f);
	}
	out.records.reserve(model.occlusion_object_count);
	int32_t vert_cursor = 0;
	int32_t plane_cursor = 0;
	int32_t face_cursor = 0;
	for (size_t i = 0; i < model.occlusion_object_count; ++i) {
		const ThreediOcclusionObject &so = model.occlusion_objects[i];
		opennova::world::OcclusionPortalFace rec;
		rec.type = so.type;
		rec.section_a = so.parent_subobject_index;
		rec.section_b = so.connecting_subobject;
		rec.pos[0] = so.position[0];
		rec.pos[1] = so.position[1];
		rec.pos[2] = so.position[2];
		rec.radius = so.radius;
		rec.vert_start = vert_cursor;
		rec.vert_count = so.num_vertices;
		rec.plane_start = plane_cursor;
		rec.plane_count = so.num_planes;
		rec.face_start = face_cursor;
		rec.face_count = so.face_count;
		rec.slot_priority_scale = so.slot_priority_scale;
		if (so.num_vertices > 0) vert_cursor += so.num_vertices;
		if (so.num_planes > 0) plane_cursor += so.num_planes;
		if (so.face_count > 0) face_cursor += so.face_count;
		out.records.push_back(rec);
	}
	// Slice sanity: reject models whose records point past their arrays, and
	// whose OFAC bytes index outside their record's slice — the engine's hot
	// loops (traverse/build_occluder_planes) read face vertex/plane/edge
	// indices unchecked, so malformed or modded data is rejected here once.
	for (const opennova::world::OcclusionPortalFace &rec : out.records) {
		if (rec.vert_start < 0 || rec.vert_count < 0 ||
		    rec.vert_start + rec.vert_count > static_cast<int32_t>(out.vertices.size()) ||
		    rec.plane_start < 0 || rec.plane_count < 0 ||
		    rec.plane_start + rec.plane_count > static_cast<int32_t>(out.planes.size()) ||
		    rec.face_start < 0 || rec.face_count < 0 ||
		    rec.face_start + rec.face_count > static_cast<int32_t>(out.faces.size()))
			return false;
		for (int32_t f = 0; f < rec.face_count; ++f) {
			const opennova::world::OcclusionFaceRec &face = out.faces[rec.face_start + f];
			if (face.v[0] >= rec.vert_count || face.v[1] >= rec.vert_count ||
			    face.v[2] >= rec.vert_count || face.plane >= rec.plane_count)
				return false;
			for (int k = 0; k < 3; ++k) {
				if ((face.edge[k] & 0xFF) >= rec.vert_count ||
				    ((face.edge[k] >> 8) & 0x7F) >= rec.vert_count)
					return false;
			}
		}
	}
	return true;
}

// LOD skinning stamp: the header mesh type, or any strip carrying a bone
// table. Mirrors retail's per-LOD skinned check the render path applies; the
// binding's ObjectData::is_skinned delegates here (ADR 0016).
bool model_is_skinned(const Threedi3di3 &model, int lod_index) {
	if (lod_index < 0 || static_cast<size_t>(lod_index) >= model.lod_count ||
			model.lods == nullptr)
		return false;
	if (model.header.mesh_type == THREEDI_MESH_SKINNED) return true;
	const ThreediLod &lod = model.lods[lod_index];
	if (lod.strips == nullptr) return false;
	for (size_t i = 0; i < lod.strip_count; ++i) {
		if (lod.strips[i].bone_table_length > 0) return true;
	}
	return false;
}

// A real collision presence: authored volumes always count; a face mesh or
// person spheres count only on skinned (organic) models. This is the
// allow_sphere_only gate the collision build consumes.
bool model_has_collision(const Threedi3di3 &model) {
	const ThreediCollisionModel *collision = model.collision;
	if (collision == nullptr) return false;
	if (collision->volume_count > 0) return true;
	if (!model_is_skinned(model, 0)) return false;
	if (collision->face_count > 0 && collision->faces != nullptr &&
			collision->vertex_count > 0 && collision->vertices != nullptr &&
			collision->object_count > 0 && collision->objects != nullptr)
		return true;
	if (collision->objects != nullptr) {
		for (size_t i = 0; i < collision->object_count; ++i) {
			if (collision->objects[i].radius > 0) return true;
		}
	}
	return false;
}

} // namespace opennova::world
