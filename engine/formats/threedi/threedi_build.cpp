// The 3DI3 construction API's assembly half: see threedi_build.h.
#include <formats/threedi/threedi_build.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace opennova::threedi {

void threedi_build_assemble(const ThreediBuildModel &m, ThreediAssembled &out) {
	out = ThreediAssembled{};
	Threedi3di3 &model = out.model;
	model.version = 0;
	model.header.has_header = 1;
	std::snprintf(model.header.name, sizeof(model.header.name), "%s", m.name.c_str());
	model.header.mesh_type = m.skinned ? THREEDI_MESH_SKINNED : THREEDI_MESH_BASIC;
	model.header.lod_count_decl = static_cast<int32_t>(m.lods.size());

	// --- render LODs ---
	const size_t lod_count = m.lods.size();
	out.lods.resize(lod_count);
	out.lod_vertices.resize(lod_count);
	out.lod_indices.resize(lod_count);
	out.lod_strips.resize(lod_count);
	out.lod_parts.resize(lod_count);
	out.lod_panm.resize(lod_count);
	double max_radius = 0.0;
	for (size_t li = 0; li < lod_count; ++li) {
		const ThreediBuildLod &src = m.lods[li];
		ThreediLod &lod = out.lods[li];
		std::memset(&lod, 0, sizeof(lod));
		std::snprintf(lod.model_type, sizeof(lod.model_type), "%s", src.type.c_str());
		lod.lod_threshold = src.threshold;
		lod.rmdl_render_object_count = static_cast<int32_t>(src.parts.size());
		std::vector<ThreediVertex> &verts = out.lod_vertices[li];
		std::vector<uint16_t> &indices = out.lod_indices[li];
		std::vector<ThreediTriangleStrip> &strips = out.lod_strips[li];
		std::vector<ThreediRenderObject> &parts = out.lod_parts[li];
		for (size_t pi = 0; pi < src.parts.size(); ++pi) {
			const ThreediBuildPart &part = src.parts[pi];
			ThreediRenderObject ro{};
			ro.parent_index = static_cast<int32_t>(part.parent);
			const ThreediBuildVec3 abs = threedi_build_to_model(part.pivot);
			const ThreediBuildVec3 parent_pivot = part.parent == static_cast<int>(pi) ? ThreediBuildVec3{} : threedi_build_to_model(src.parts[part.parent].pivot);
			ro.abs[0] = static_cast<float>(abs.x);
			ro.abs[1] = static_cast<float>(abs.y);
			ro.abs[2] = static_cast<float>(abs.z);
			// The derived form keeps the writer's signed-zero convention; the
			// caller's own value (a scene node's exact local origin) wins only
			// where the two differ as floats.
			ThreediBuildVec3 rel{abs.x - parent_pivot.x, abs.y - parent_pivot.y, abs.z - parent_pivot.z};
			if (part.has_rel) {
				const ThreediBuildVec3 given = threedi_build_to_model(part.rel);
				if (static_cast<float>(given.x) != static_cast<float>(rel.x)) rel.x = given.x;
				if (static_cast<float>(given.y) != static_cast<float>(rel.y)) rel.y = given.y;
				if (static_cast<float>(given.z) != static_cast<float>(rel.z)) rel.z = given.z;
			}
			ro.rel[0] = static_cast<float>(rel.x);
			ro.rel[1] = static_cast<float>(rel.y);
			ro.rel[2] = static_cast<float>(rel.z);
			double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
			bool any = false;
			// Opaque strips first, then alpha strips (the renderer's walk).
			for (int pass = 0; pass < 2; ++pass) {
				for (const ThreediBuildStrip &strip : part.strips) {
					if (strip.alpha != (pass == 1)) continue;
					ThreediTriangleStrip rec{};
					rec.material_index = strip.material;
					rec.index_offset = static_cast<int32_t>(indices.size());
					rec.num_indices = static_cast<uint16_t>(strip.indices.size());
					rec.num_triangles = static_cast<uint16_t>(strip.indices.size() / 3);
					rec.is_strip = 0;
					rec.start_vertex = static_cast<int32_t>(verts.size());
					rec.num_vertices = static_cast<int32_t>(strip.vertices.size());
					double smn[3] = {1e9, 1e9, 1e9}, smx[3] = {-1e9, -1e9, -1e9};
					for (const ThreediVertex &v : strip.vertices) {
						for (int k = 0; k < 3; ++k) {
							smn[k] = std::min<double>(smn[k], v.position[k]);
							smx[k] = std::max<double>(smx[k], v.position[k]);
						}
						max_radius = std::max(max_radius, std::sqrt(static_cast<double>(v.position[0]) * v.position[0] +
								static_cast<double>(v.position[1]) * v.position[1] + static_cast<double>(v.position[2]) * v.position[2]));
					}
					if (!m.skinned) {
						for (int k = 0; k < 3; ++k) {
							rec.min[k] = static_cast<float>(smn[k]);
							rec.max[k] = static_cast<float>(smx[k]);
						}
					}
					if (m.skinned) {
						if (!strip.bone_table.empty()) {
							const size_t n = std::min<size_t>(strip.bone_table.size(), sizeof(rec.bone_table));
							for (size_t b = 0; b < n; ++b) rec.bone_table[b] = strip.bone_table[b];
							rec.bone_table_length = static_cast<int32_t>(n);
						} else {
							rec.bone_table[0] = static_cast<uint8_t>(strip.bone < 0 ? static_cast<int>(pi) : strip.bone);
							rec.bone_table_length = 1;
						}
					}
					for (int k = 0; k < 3; ++k) {
						mn[k] = std::min(mn[k], smn[k]);
						mx[k] = std::max(mx[k], smx[k]);
					}
					any = any || !strip.vertices.empty();
					verts.insert(verts.end(), strip.vertices.begin(), strip.vertices.end());
					indices.insert(indices.end(), strip.indices.begin(), strip.indices.end());
					strips.push_back(rec);
					if (strip.alpha) ++ro.num_alpha_strips;
					else ++ro.num_strips;
				}
			}
			if (any) {
				for (int k = 0; k < 3; ++k) ro.bounding_center[k] = static_cast<float>((mn[k] + mx[k]) * 0.5);
				double r = 0.0;
				for (int k = 0; k < 3; ++k) r += (mx[k] - mn[k]) * (mx[k] - mn[k]) * 0.25;
				ro.bounding_radius = static_cast<float>(std::sqrt(r));
			}
			parts.push_back(ro);
		}
		const uint32_t vertex_flags = 1u | (m.skinned ? THREEDI_VERTEX_FLAG_SKINNED : 0u) |
				(m.tangents ? THREEDI_VERTEX_FLAG_TANGENTS : 0u);
		for (ThreediVertex &v : verts) {
			v.flags = vertex_flags;
			v.is_skinned = m.skinned ? 1 : 0;
			v.has_tangents = m.tangents ? 1 : 0;
		}
		lod.vertices.count = static_cast<uint32_t>(verts.size());
		lod.vertices.stride = 40u + (m.skinned ? 16u : 0u) + (m.tangents ? 24u : 0u);
		lod.vertices.flags = vertex_flags;
		lod.vertices.items = verts.data();
		lod.indices.count = static_cast<uint32_t>(indices.size());
		lod.indices.indices = indices.data();
		lod.strips = strips.data();
		lod.strip_count = strips.size();
		lod.strip_record_size = m.skinned ? 68u : 48u;
		lod.render_objects = parts.data();
		lod.render_object_count = parts.size();
		out.lod_panm[li] = src.panm;
		lod.part_animations = out.lod_panm[li].data();
		lod.part_animation_count = out.lod_panm[li].size();
		lod.part_animation_record_size = 68u;
	}
	model.lods = out.lods.data();
	model.lod_count = lod_count;
	model.header.max_radius_fp16 = threedi_q16(max_radius);

	// --- materials, lights, points, registers, matrices ---
	out.materials = m.materials;
	model.materials = out.materials.data();
	model.material_count = static_cast<uint32_t>(out.materials.size());
	model.material_record_size = 584u;
	out.lights = m.lights;
	model.lights = out.lights.data();
	model.light_count = out.lights.size();
	out.user_points = m.user_points;
	model.user_points = out.user_points.data();
	model.user_point_count = out.user_points.size();
	for (const std::string &reg : m.control_registers) {
		ThreediControlRegister r{};
		std::snprintf(r.name, sizeof(r.name), "%s", reg.c_str());
		out.registers.push_back(r);
	}
	model.ctrl.count = static_cast<uint32_t>(out.registers.size());
	model.ctrl.record_size = 24u;
	model.ctrl.registers = out.registers.data();
	for (int i = 0; i < m.matrix_count; ++i) {
		ThreediMatrix4x4 identity;
		threedi_mat4_identity(&identity);
		out.matrices.push_back(identity);
	}
	model.mtrx.count = static_cast<uint32_t>(out.matrices.size());
	model.mtrx.record_size = 64u;
	model.mtrx.matrices = out.matrices.data();

	// --- collision ---
	if (!m.collision.empty()) {
		double bmn[3] = {1e9, 1e9, 1e9}, bmx[3] = {-1e9, -1e9, -1e9};
		bool bounded = false;
		auto expand = [&](double x, double y, double z) {
			bmn[0] = std::min(bmn[0], x);
			bmn[1] = std::min(bmn[1], y);
			bmn[2] = std::min(bmn[2], z);
			bmx[0] = std::max(bmx[0], x);
			bmx[1] = std::max(bmx[1], y);
			bmx[2] = std::max(bmx[2], z);
			bounded = true;
		};
		for (size_t oi = 0; oi < m.collision.size(); ++oi) {
			const ThreediBuildCollisionObject &src = m.collision[oi];
			ThreediCollisionObject o{};
			o.num_vertices = static_cast<int32_t>(src.vertices.size());
			o.num_faces = static_cast<int32_t>(src.faces.size());
			o.num_normals = static_cast<int32_t>(src.normals.size());
			o.num_bounding_volumes = static_cast<int32_t>(src.volumes.size());
			o.parent_subobject_index = src.parent_part;
			o.offset[0] = threedi_q16(src.offset.x);
			o.offset[1] = threedi_q16(src.offset.y);
			o.offset[2] = threedi_q16(src.offset.z);
			double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
			bool any = false;
			for (const ThreediCollisionVertex &v : src.vertices) {
				for (int k = 0; k < 3; ++k) {
					mn[k] = std::min<double>(mn[k], v.position[k]);
					mx[k] = std::max<double>(mx[k], v.position[k]);
				}
				expand(v.position[0], v.position[1], v.position[2]);
				any = true;
			}
			for (const ThreediBoundingVolume &v : src.volumes) {
				const double vmn[3] = {v.min_x_fp16 / 65536.0, v.min_y_fp16 / 65536.0, v.min_z_fp16 / 65536.0};
				const double vmx[3] = {v.max_x_fp16 / 65536.0, v.max_y_fp16 / 65536.0, v.max_z_fp16 / 65536.0};
				for (int k = 0; k < 3; ++k) {
					mn[k] = std::min(mn[k], vmn[k]);
					mx[k] = std::max(mx[k], vmx[k]);
				}
				expand(vmn[0], vmn[1], vmn[2]);
				expand(vmx[0], vmx[1], vmx[2]);
				any = true;
			}
			if (src.sphere) {
				const ThreediBuildVec3 c = src.sphere_center;
				const double r = src.sphere_radius;
				o.min[0] = threedi_q16(c.x - r);
				o.min[1] = threedi_q16(c.y - r);
				o.min[2] = threedi_q16(c.z - r);
				o.max[0] = threedi_q16(c.x + r);
				o.max[1] = threedi_q16(c.y + r);
				o.max[2] = threedi_q16(c.z + r);
				o.med[0] = threedi_q16(c.x);
				o.med[1] = threedi_q16(c.y);
				o.med[2] = threedi_q16(c.z);
				o.radius = threedi_q16(r);
				expand(c.x - r, c.y - r, c.z - r);
				expand(c.x + r, c.y + r, c.z + r);
			} else if (any) {
				double r = 0.0;
				for (int k = 0; k < 3; ++k) {
					o.min[k] = threedi_q16(mn[k]);
					o.max[k] = threedi_q16(mx[k]);
					o.med[k] = threedi_q16((mn[k] + mx[k]) * 0.5);
					r += (mx[k] - mn[k]) * (mx[k] - mn[k]) * 0.25;
				}
				o.radius = threedi_q16(std::sqrt(r));
			} else {
				// The retail sentinel for an empty section.
				for (int k = 0; k < 3; ++k) {
					o.min[k] = 10000 << 16;
					o.max[k] = -(10000 << 16);
				}
			}
			out.objects.push_back(o);
			out.vertices.insert(out.vertices.end(), src.vertices.begin(), src.vertices.end());
			out.normals.insert(out.normals.end(), src.normals.begin(), src.normals.end());
			out.faces.insert(out.faces.end(), src.faces.begin(), src.faces.end());
			out.volumes.insert(out.volumes.end(), src.volumes.begin(), src.volumes.end());
			out.planes.insert(out.planes.end(), src.planes.begin(), src.planes.end());
			// CXLT: the retail corpus carries one translation per non-root
			// section on rigid models and one per section on skinned ones.
			if (oi > 0 || m.skinned) {
				ThreediCollisionTranslation t{};
				t.translation[0] = o.offset[0];
				t.translation[1] = o.offset[1];
				t.translation[2] = o.offset[2];
				out.translations.push_back(t);
			}
		}
		ThreediCollisionModel &col = out.collision;
		if (bounded) {
			for (int k = 0; k < 3; ++k) {
				col.model_data.bbox[k] = threedi_q16f(bmn[k]);
				col.model_data.bbox[k + 3] = threedi_q16f(bmx[k]);
			}
			double r = 0.0, rxy = 0.0, rz = 0.0;
			for (int i = 0; i < 8; ++i) {
				const double x = (i & 1) ? bmx[0] : bmn[0], y = (i & 2) ? bmx[1] : bmn[1], z = (i & 4) ? bmx[2] : bmn[2];
				r = std::max(r, std::sqrt(x * x + y * y + z * z));
				rxy = std::max(rxy, std::sqrt(x * x + y * y));
				rz = std::max(rz, std::fabs(z));
			}
			col.model_data.radii[0] = threedi_q16f(r);
			col.model_data.radii[1] = threedi_q16f(rxy);
			col.model_data.radii[2] = threedi_q16f(rz);
		}
		col.model_data.num_vertices = static_cast<int32_t>(out.vertices.size());
		col.model_data.num_normals = static_cast<int32_t>(out.normals.size());
		col.model_data.num_faces = static_cast<int32_t>(out.faces.size());
		col.model_data.num_objects = static_cast<int32_t>(out.objects.size());
		col.model_data.num_transforms = static_cast<int32_t>(out.translations.size());
		col.model_data.num_bounding_planes = static_cast<int32_t>(out.planes.size());
		col.model_data.num_bounding_volumes = static_cast<int32_t>(out.volumes.size());
		col.planes = out.planes.data();
		col.plane_count = out.planes.size();
		col.volumes = out.volumes.data();
		col.volume_count = out.volumes.size();
		col.vertices = out.vertices.data();
		col.vertex_count = out.vertices.size();
		col.normals = out.normals.data();
		col.normal_count = out.normals.size();
		col.faces = out.faces.data();
		col.face_count = out.faces.size();
		col.objects = out.objects.data();
		col.object_count = out.objects.size();
		col.translations = out.translations.data();
		col.translation_count = out.translations.size();
		model.collision = &out.collision;
	}

	// --- occlusion ---
	for (const ThreediBuildOcclusionRecord &rec : m.occlusion) {
		out.occ_objects.push_back(rec.object);
		out.occ_vertices.insert(out.occ_vertices.end(), rec.vertices.begin(), rec.vertices.end());
		out.occ_planes.insert(out.occ_planes.end(), rec.planes.begin(), rec.planes.end());
		out.occ_faces.insert(out.occ_faces.end(), rec.faces.begin(), rec.faces.end());
	}
	model.occlusion_vertices = out.occ_vertices.data();
	model.occlusion_vertex_count = out.occ_vertices.size();
	model.occlusion_vertex_record_size = 12u;
	model.occlusion_planes = out.occ_planes.data();
	model.occlusion_plane_count = out.occ_planes.size();
	model.occlusion_plane_record_size = 16u;
	model.occlusion_faces = out.occ_faces.data();
	model.occlusion_face_count = out.occ_faces.size();
	model.occlusion_face_record_size = 12u;
	model.occlusion_objects = out.occ_objects.data();
	model.occlusion_object_count = out.occ_objects.size();
	model.occlusion_object_record_size = 36u;
}

bool threedi_build_mint(const ThreediBuildModel &m, std::vector<uint8_t> &out) {
	ThreediAssembled assembled;
	threedi_build_assemble(m, assembled);
	return threedi_3di3_write_memory(&assembled.model, out) == 0;
}

} // namespace opennova::threedi
