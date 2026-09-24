// The 3DI3 construction API's assembly half: see threedi_build.h.
#include <formats/threedi/threedi_build.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace opennova::threedi {

void threedi_build_light_view_proj(ThreediLight &light, float falloff) {
	// The retired OED exporter's build_light_view_proj
	// (5fc5b4f6a^:engine/formats/oed/export_3di.cpp): it reproduces
	// Armry01's LGHT records to within one ulp in two entries.
	const auto normalize = [](float v[3]) {
		const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (len > 0.0f)
			for (int k = 0; k < 3; ++k) v[k] /= len;
	};
	float dir[3] = {light.rotation[0], light.rotation[1], light.rotation[2]};
	normalize(dir);
	float right[3] = {dir[2], 0.0f, -dir[0]};
	normalize(right);
	float up[3] = {dir[1] * right[2] - dir[2] * right[1], dir[2] * right[0] - dir[0] * right[2],
			dir[0] * right[1] - dir[1] * right[0]};
	normalize(up);
	float view[16] = {};
	view[0] = right[0];
	view[1] = up[0];
	view[2] = dir[0];
	view[4] = right[1];
	view[5] = up[1];
	view[6] = dir[1];
	view[8] = right[2];
	view[9] = up[2];
	view[10] = dir[2];
	view[15] = 1.0f;
	const float *pos = light.offset;
	view[12] = -(pos[0] * view[0] + pos[1] * view[4] + pos[2] * view[8]);
	view[13] = -(pos[0] * view[1] + pos[1] * view[5] + pos[2] * view[9]);
	view[14] = -(pos[0] * view[2] + pos[1] * view[6] + pos[2] * view[10]);
	const float fov = (falloff + falloff) * 0.017453289f;
	const float zn = 0.1f;
	const float zf = light.atten_end;
	const float y_scale = 1.0f / std::tan(fov * 0.5f);
	float proj[16] = {};
	proj[0] = y_scale;
	proj[5] = y_scale;
	proj[10] = zf / (zf - zn);
	proj[11] = 1.0f;
	proj[14] = (-zn * zf) / (zf - zn);
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) sum += view[i * 4 + k] * proj[k * 4 + j];
			light.view_proj[i * 4 + j] = sum;
		}
}

namespace {

// NormalizeVec3's x87 shape: the sum of squares in double, the length and its
// reciprocal stored as float [orig: NormalizeVec3 @ 0x4215E0 (ModSuperOed)].
void normalize_x87(float v[3]) {
	const double x = v[0], y = v[1], z = v[2];
	const float len = static_cast<float>(std::sqrt(x * x + y * y + z * z));
	if (len == 0.0f) {
		v[0] = v[1] = v[2] = 0.0f;
		return;
	}
	const float inv = static_cast<float>(1.0 / static_cast<double>(len));
	v[0] = static_cast<float>(static_cast<double>(inv) * x);
	v[1] = static_cast<float>(static_cast<double>(inv) * y);
	v[2] = static_cast<float>(static_cast<double>(inv) * z);
}

// A LOD's tangents and bitangents by the OED rule: each triangle's dP/du and
// dP/dv from its D3D UVs (a triangle whose UVs are degenerate reuses the last
// good one's, as the static carry in BuildTransformMatrix does), summed over
// the triangles that share a vertex of the same part, position and normal
// (OED's shared ASE vertex within one smoothing group), then normalized
// [orig: BuildTransformMatrix @ 0x421B80, the smoothed-vector walk sub_457360
// (ModSuperOed); 5fc5b4f6a^:engine/formats/oed/convert_internal.cpp
// compute_face_plane and rdta.cpp smooth_vertex_basis]. The map is linear, so
// working in model axes yields the model-axis vectors OED writes.
void derive_tangents(std::vector<ThreediVertex> &verts, const std::vector<uint16_t> &indices,
		const std::vector<ThreediTriangleStrip> &strips, const std::vector<int> &strip_part, float carry[6]) {
	struct Key {
		int part;
		float p[3], n[3];
		bool operator<(const Key &o) const {
			if (part != o.part) return part < o.part;
			for (int k = 0; k < 3; ++k) {
				if (p[k] != o.p[k]) return p[k] < o.p[k];
				if (n[k] != o.n[k]) return n[k] < o.n[k];
			}
			return false;
		}
	};
	const auto key_of = [&](size_t s, const ThreediVertex &v) {
		Key k{strip_part[s], {v.position[0], v.position[1], v.position[2]}, {v.normal[0], v.normal[1], v.normal[2]}};
		return k;
	};
	std::map<Key, std::array<float, 6>> sums;
	const auto accumulate = [](float acc[3], const float add[3]) {
		for (int k = 0; k < 3; ++k) acc[k] = static_cast<float>(static_cast<double>(acc[k]) + static_cast<double>(add[k]));
	};
	for (size_t s = 0; s < strips.size(); ++s) {
		const ThreediTriangleStrip &rec = strips[s];
		for (int t = 0; t + 2 < rec.num_indices; t += 3) {
			const ThreediVertex *c[3];
			for (int k = 0; k < 3; ++k)
				c[k] = &verts[static_cast<size_t>(rec.start_vertex) + indices[static_cast<size_t>(rec.index_offset + t + k)]];
			const float du1 = c[1]->uv0[0] - c[0]->uv0[0], dv1 = c[1]->uv0[1] - c[0]->uv0[1];
			const float du2 = c[2]->uv0[0] - c[0]->uv0[0], dv2 = c[2]->uv0[1] - c[0]->uv0[1];
			const float det = static_cast<float>(static_cast<double>(du1) * dv2 - static_cast<double>(dv1) * du2);
			if (std::fabs(det) > 1e-9f) {
				for (int a = 0; a < 3; ++a) {
					const float d1 = c[1]->position[a] - c[0]->position[a];
					const float d2 = c[2]->position[a] - c[0]->position[a];
					const float cross1 = static_cast<float>(static_cast<double>(dv1) * d2 - static_cast<double>(d1) * dv2);
					const float cross2 = static_cast<float>(static_cast<double>(d1) * du2 - static_cast<double>(du1) * d2);
					carry[a] = static_cast<float>(-(static_cast<double>(cross1) / det));
					carry[a + 3] = static_cast<float>(-(static_cast<double>(cross2) / det));
				}
			}
			for (int k = 0; k < 3; ++k) {
				std::array<float, 6> &sum = sums[key_of(s, *c[k])];
				accumulate(sum.data(), carry);
				accumulate(sum.data() + 3, carry + 3);
			}
		}
	}
	for (size_t s = 0; s < strips.size(); ++s) {
		const ThreediTriangleStrip &rec = strips[s];
		for (int i = 0; i < rec.num_vertices; ++i) {
			ThreediVertex &v = verts[static_cast<size_t>(rec.start_vertex + i)];
			const auto it = sums.find(key_of(s, v));
			float t[3] = {0, 0, 0}, b[3] = {0, 0, 0};
			if (it != sums.end()) {
				for (int k = 0; k < 3; ++k) {
					t[k] = it->second[k];
					b[k] = it->second[k + 3];
				}
			}
			normalize_x87(t);
			normalize_x87(b);
			for (int k = 0; k < 3; ++k) {
				v.tangent[k] = t[k];
				v.bitangent[k] = b[k];
			}
		}
	}
}

} // namespace

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
	float tangent_carry[6] = {};
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
		std::vector<int> strip_part; // the part each strip record was authored on
		for (size_t pi = 0; pi < src.parts.size(); ++pi) {
			const ThreediBuildPart &part = src.parts[pi];
			ThreediRenderObject ro{};
			ro.parent_index = static_cast<int32_t>(part.parent);
			const ThreediBuildVec3 abs = threedi_build_to_model(part.pivot);
			// The root names itself; retail also ships parts whose parent is -1.
			const bool rooted = part.parent == static_cast<int>(pi) || part.parent < 0 ||
					part.parent >= static_cast<int>(src.parts.size());
			const ThreediBuildVec3 parent_pivot = rooted ? ThreediBuildVec3{} : threedi_build_to_model(src.parts[part.parent].pivot);
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
			// A skinned model's strips are all owned by the root ROBJ while
			// each part keeps the bounds of the geometry authored on it: the
			// retail skinned layout (every JO mesh_type-2 model, e.g.
			// FSldr03: ROBJ 0 counts all six strips, ROBJ 19 carries the
			// mesh bounds); they are emitted after the part walk below.
			for (int pass = 0; pass < 2; ++pass) {
				for (const ThreediBuildStrip &strip : part.strips) {
					if (strip.alpha != (pass == 1)) continue;
					if (m.skinned) {
						for (const ThreediVertex &v : strip.vertices) {
							for (int k = 0; k < 3; ++k) {
								mn[k] = std::min<double>(mn[k], v.position[k]);
								mx[k] = std::max<double>(mx[k], v.position[k]);
							}
							max_radius = std::max(max_radius, std::sqrt(static_cast<double>(v.position[0]) * v.position[0] +
									static_cast<double>(v.position[1]) * v.position[1] + static_cast<double>(v.position[2]) * v.position[2]));
						}
						any = any || !strip.vertices.empty();
						continue;
					}
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
					strip_part.push_back(static_cast<int>(pi));
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
		if (m.skinned && !parts.empty()) {
			for (int pass = 0; pass < 2; ++pass) {
				for (size_t pi = 0; pi < src.parts.size(); ++pi) {
					for (const ThreediBuildStrip &strip : src.parts[pi].strips) {
						if (strip.alpha != (pass == 1)) continue;
						ThreediTriangleStrip rec{};
						rec.material_index = strip.material;
						rec.index_offset = static_cast<int32_t>(indices.size());
						rec.num_indices = static_cast<uint16_t>(strip.indices.size());
						rec.num_triangles = static_cast<uint16_t>(strip.indices.size() / 3);
						rec.is_strip = 0;
						rec.start_vertex = static_cast<int32_t>(verts.size());
						rec.num_vertices = static_cast<int32_t>(strip.vertices.size());
						if (!strip.bone_table.empty()) {
							const size_t n = std::min<size_t>(strip.bone_table.size(), sizeof(rec.bone_table));
							for (size_t b = 0; b < n; ++b) rec.bone_table[b] = strip.bone_table[b];
							rec.bone_table_length = static_cast<int32_t>(n);
						} else {
							rec.bone_table[0] = static_cast<uint8_t>(strip.bone < 0 ? static_cast<int>(pi) : strip.bone);
							rec.bone_table_length = 1;
						}
						verts.insert(verts.end(), strip.vertices.begin(), strip.vertices.end());
						indices.insert(indices.end(), strip.indices.begin(), strip.indices.end());
						strips.push_back(rec);
						strip_part.push_back(static_cast<int>(pi));
						if (strip.alpha) ++parts[0].num_alpha_strips;
						else ++parts[0].num_strips;
					}
				}
			}
		}
		if (m.tangents) derive_tangents(verts, indices, strips, strip_part, tangent_carry);
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
	ThreediMatrix4x4 identity;
	threedi_mat4_identity(&identity);
	out.matrices.push_back(identity);
	out.matrices.insert(out.matrices.end(), m.frames.begin(), m.frames.end());
	model.mtrx.count = static_cast<uint32_t>(out.matrices.size());
	model.mtrx.record_size = 64u;
	model.mtrx.matrices = out.matrices.data();

	// --- collision ---
	// Every model carries a CDTA; one with no section still bounds LOD 0 in
	// its CMDL (CNet01: no COBJ, a CMDL box over its render geometry).
	{
		// CMDL: the box and radii of the collision LOD's faces and LOD 0's
		// render triangles, radii[2] the height (WriteCDTA runs ComputeLodBounds
		// over both [orig: WriteCDTA @ 0x456050; 5fc5b4f6a^:engine/formats/oed/
		// export_3di.cpp]). The bullet faces stand for the collision LOD, and
		// every value here comes from the stored (quantized) positions so
		// build(scene(x)) re-mints x byte for byte.
		// The box envelops both LODs; the radii and height are the collision
		// LOD's alone (retail: CNet01, no face, stores radii 0 and a height of
		// -20000, the empty sentinels' span; Dblkhwk1's radii leave LOD 0 out).
		double bmn[3] = {10000.0, 10000.0, 10000.0}, bmx[3] = {-10000.0, -10000.0, -10000.0};
		double cmn_z = 10000.0, cmx_z = -10000.0, max_r = 0.0, max_rxy = 0.0;
		auto expand = [&](double x, double y, double z) {
			const double p[3] = {x, y, z};
			for (int k = 0; k < 3; ++k) {
				bmn[k] = std::min(bmn[k], p[k]);
				bmx[k] = std::max(bmx[k], p[k]);
			}
		};
		for (const ThreediBuildCollisionObject &src : m.collision)
			for (const ThreediCollisionFace &f : src.faces)
				for (int k = 0; k < 3; ++k) {
					const float *p = src.vertices[static_cast<size_t>(f.vert_index[k])].position;
					const double x = p[0], y = p[1], z = p[2];
					expand(x, y, z);
					cmn_z = std::min(cmn_z, z);
					cmx_z = std::max(cmx_z, z);
					max_r = std::max(max_r, std::sqrt(x * x + y * y + z * z));
					max_rxy = std::max(max_rxy, std::sqrt(x * x + y * y));
				}
		if (!m.lods.empty())
			for (const ThreediBuildPart &part : m.lods[0].parts)
				for (const ThreediBuildStrip &strip : part.strips)
					for (uint16_t i : strip.indices) {
						const ThreediVertex &v = strip.vertices[i];
						const ThreediBuildVec3 p = threedi_build_to_mission(ThreediBuildVec3{v.position[0], v.position[1], v.position[2]});
						expand(p.x, p.y, p.z);
					}
		const double kSentinel = 10000.0;
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
			// The section's bounds cover its vertices and its volumes' boxes;
			// its radius is the farthest VERTEX from their midpoint, so a
			// volume-only section's is 0 (retail ships 52), and the stored
			// midpoint is that of the truncated bounds [orig: WriteCOBJ @
			// 0x454E70; 5fc5b4f6a^:engine/formats/oed/export_3di.cpp]. An empty
			// section keeps the +-10000 sentinels. A bone section is bounded
			// by the vertices the bone moves when the scene gives them.
			double mn[3] = {kSentinel, kSentinel, kSentinel}, mx[3] = {-kSentinel, -kSentinel, -kSentinel};
			double radius = 0.0;
			if (src.sphere) {
				const ThreediBuildVec3 c = src.sphere_center;
				const double r = src.sphere_radius;
				const ThreediBuildBox b = src.sphere_bounded ? src.sphere_bounds
						: ThreediBuildBox{{c.x - r, c.y - r, c.z - r}, {c.x + r, c.y + r, c.z + r}};
				const double bmin[3] = {b.min.x, b.min.y, b.min.z}, bmax[3] = {b.max.x, b.max.y, b.max.z};
				for (int k = 0; k < 3; ++k) {
					mn[k] = bmin[k];
					mx[k] = bmax[k];
				}
				radius = r;
			} else {
				for (const ThreediCollisionVertex &v : src.vertices)
					for (int k = 0; k < 3; ++k) {
						mn[k] = std::min<double>(mn[k], v.position[k]);
						mx[k] = std::max<double>(mx[k], v.position[k]);
					}
				for (const ThreediBoundingVolume &v : src.volumes) {
					const int32_t vmn[3] = {v.min_x_fp16, v.min_y_fp16, v.min_z_fp16};
					const int32_t vmx[3] = {v.max_x_fp16, v.max_y_fp16, v.max_z_fp16};
					for (int k = 0; k < 3; ++k) {
						mn[k] = std::min(mn[k], vmn[k] / io::kFp16OneD);
						mx[k] = std::max(mx[k], vmx[k] / io::kFp16OneD);
					}
				}
				const double mid[3] = {(mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5, (mn[2] + mx[2]) * 0.5};
				for (const ThreediCollisionVertex &v : src.vertices) {
					const double d[3] = {v.position[0] - mid[0], v.position[1] - mid[1], v.position[2] - mid[2]};
					radius = std::max(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
				}
			}
			for (int k = 0; k < 3; ++k) {
				o.min[k] = threedi_q16_trunc(mn[k]);
				o.max[k] = threedi_q16_trunc(mx[k]);
				o.med[k] = (o.min[k] + o.max[k]) / 2;
			}
			if (src.sphere) {
				// A bone section's stored midpoint is the sphere centre given.
				o.med[0] = threedi_q16(src.sphere_center.x);
				o.med[1] = threedi_q16(src.sphere_center.y);
				o.med[2] = threedi_q16(src.sphere_center.z);
			}
			o.radius = threedi_q16_trunc(radius);
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
		// Seam flags on the planes of volumes built from triangles: for each
		// triangle, in section then volume order, its plane's flag is cleared,
		// then set when the triangle's box shrunk by 0.01 lies inside the box
		// of another solid (CB, type 1) volume in any section; the last
		// triangle on a plane decides. A ladder's triangles keep the plane
		// indices they took before the plane 0 swap, as OED's do [orig:
		// ConvertToInternal @ 0x4268B3, the collision-overlap pass;
		// 5fc5b4f6a^:engine/formats/oed/convert_internal.cpp].
		struct VolumeRef {
			const ThreediBuildVolumeSource *src;
			int32_t type;
			size_t plane_start;
			int32_t plane_count;
		};
		std::vector<VolumeRef> refs;
		size_t plane_cursor = 0;
		for (const ThreediBuildCollisionObject &src : m.collision)
			for (size_t vi = 0; vi < src.volumes.size(); ++vi) {
				refs.push_back({&src.volume_sources[vi], src.volumes[vi].collidable_type, plane_cursor, src.volumes[vi].plane_count});
				plane_cursor += static_cast<size_t>(std::max(0, src.volumes[vi].plane_count));
			}
		for (size_t n = 0; n < refs.size(); ++n) {
			const ThreediBuildVolumeSource &vol = *refs[n].src;
			if (!vol.meshed) continue;
			for (size_t fi = 0; fi < vol.face_boxes.size(); ++fi) {
				const std::array<float, 6> &b = vol.face_boxes[fi];
				const float pad = 0.0099999998f;
				const float inner[6] = {b[0] + pad, b[1] + pad, b[2] + pad, b[3] - pad, b[4] - pad, b[5] - pad};
				bool overlap = false;
				for (size_t j = 0; j < refs.size(); ++j) {
					if (j == n || refs[j].type != 1) continue;
					const float *o = refs[j].src->box;
					if (inner[0] >= o[0] && inner[3] <= o[3] && inner[1] >= o[1] && inner[4] <= o[4] && inner[2] >= o[2] &&
							inner[5] <= o[5])
						overlap = true;
				}
				const int p = vol.face_planes[fi];
				if (p >= 0 && p < refs[n].plane_count) out.planes[refs[n].plane_start + static_cast<size_t>(p)].flags = overlap ? 1 : 0;
			}
		}
		ThreediCollisionModel &col = out.collision;
		const auto fp = [](double v) { return static_cast<float>(threedi_q16_trunc(v) / io::kFp16OneD); };
		for (int k = 0; k < 3; ++k) {
			col.model_data.bbox[k] = fp(bmn[k]);
			col.model_data.bbox[k + 3] = fp(bmx[k]);
		}
		col.model_data.radii[0] = fp(max_r);
		col.model_data.radii[1] = fp(max_rxy);
		col.model_data.radii[2] = fp(cmx_z - cmn_z);
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
