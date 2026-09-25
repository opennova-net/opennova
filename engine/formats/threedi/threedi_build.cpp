// The 3DI3 construction API: see threedi_build.h.
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

// Rows of C: mission x -> model z, mission y -> -model x, mission z -> model y.
static const double kMissionToModel[3][3] = {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}};

ThreediMatrix4x4 threedi_build_frame_to_model(const double mission[9]) {
	ThreediMatrix4x4 m;
	threedi_mat4_identity(&m);
	for (int a = 0; a < 3; ++a)
		for (int b = 0; b < 3; ++b) {
			double sum = 0.0;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j) sum += kMissionToModel[i][a] * mission[i * 3 + j] * kMissionToModel[j][b];
			m.m[a * 4 + b] = static_cast<float>(sum);
		}
	return m;
}

void threedi_build_frame_to_mission(const ThreediMatrix4x4 &frame, double mission[9]) {
	const float *r = frame.m;
	for (int a = 0; a < 3; ++a)
		for (int b = 0; b < 3; ++b) {
			double sum = 0.0;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j) sum += kMissionToModel[a][i] * r[i * 4 + j] * kMissionToModel[b][j];
			mission[a * 3 + b] = sum;
		}
}

uint32_t threedi_build_panm_flags(const ThreediPartAnimation &row, uint8_t trans_axis) {
	const auto live = [](const ThreediTransform &t) { return t.control != 0; };
	const bool rot = live(row.rotation_x) || live(row.rotation_y) || live(row.rotation_z);
	const bool scale = live(row.scale_x) || live(row.scale_y) || live(row.scale_z);
	return threedi_panm_pack_flags(scale ? 2 : 0, rot ? 2 : 0, 0, live(row.translation) ? trans_axis : 0);
}

ThreediPartAnimation threedi_build_inert_panm(int part, int parent) {
	ThreediPartAnimation row{};
	row.parent_subobject = static_cast<uint8_t>(parent);
	row.subobject_index = static_cast<uint8_t>(part);
	return row;
}

ThreediTransform threedi_build_track(uint8_t control, uint8_t param, int16_t rate, int16_t start, int16_t end) {
	ThreediTransform t{};
	t.control = control;
	t.control_param = param;
	t.rate = rate;
	t.start = start;
	t.end = end;
	return t;
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

uint16_t occ_edge(int a, int b) {
	const int lo = a < b ? a : b;
	const int hi = a < b ? b : a;
	return static_cast<uint16_t>((lo & 0xFF) | ((hi & 0x7F) << 8) | (a > b ? 0x8000 : 0));
}

ThreediOcclusionFace occ_face(int v0, int v1, int v2, int plane) {
	ThreediOcclusionFace f{};
	f.raw_indices = static_cast<uint32_t>(v0 & 0xFF) | (static_cast<uint32_t>(v1 & 0xFF) << 8) |
			(static_cast<uint32_t>(v2 & 0xFF) << 16) | (static_cast<uint32_t>(plane & 0xFF) << 24);
	f.edge_data = static_cast<uint32_t>(occ_edge(v0, v1)) | (static_cast<uint32_t>(occ_edge(v1, v2)) << 16);
	f.other_edge_data = occ_edge(v2, v0);
	return f;
}

ThreediOcclusionVertex occ_vertex(ThreediBuildVec3 mission) {
	const ThreediBuildVec3 m = threedi_build_to_model(mission);
	ThreediOcclusionVertex v{};
	v.position[0] = static_cast<float>(m.x);
	v.position[1] = static_cast<float>(m.y);
	v.position[2] = static_cast<float>(m.z);
	return v;
}

ThreediBuildVolumeSource volume_source(const ThreediBuildBox &box) {
	ThreediBuildVolumeSource s;
	const double b[6] = {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
	for (int k = 0; k < 6; ++k) s.box[k] = static_cast<float>(b[k]);
	return s;
}

void finish_occlusion_record(std::vector<ThreediBuildOcclusionRecord> &occlusion, ThreediBuildOcclusionRecord &rec,
		uint8_t type, int section_a, int section_b) {
	rec.object.type = type;
	rec.object.parent_subobject_index = static_cast<uint8_t>(section_a);
	rec.object.connecting_subobject = static_cast<uint8_t>(section_b);
	float center[3] = {0, 0, 0};
	for (const ThreediOcclusionVertex &v : rec.vertices)
		for (int k = 0; k < 3; ++k) center[k] += v.position[k] / static_cast<float>(rec.vertices.size());
	float radius = 0.0f;
	for (const ThreediOcclusionVertex &v : rec.vertices) {
		const float dx = v.position[0] - center[0], dy = v.position[1] - center[1], dz = v.position[2] - center[2];
		radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
	}
	rec.object.position[0] = center[0];
	rec.object.position[1] = center[1];
	rec.object.position[2] = center[2];
	rec.object.radius = radius;
	rec.object.num_vertices = static_cast<int32_t>(rec.vertices.size());
	rec.object.num_planes = static_cast<int32_t>(rec.planes.size());
	rec.object.face_count = static_cast<int32_t>(rec.faces.size());
	occlusion.push_back(rec);
}

// Assembly: the contiguous Threedi3di3 the writer serializes. Owns every
// array the struct points at (`model` points into the vectors, so it never
// copies or moves).
struct ThreediAssembled {
	ThreediAssembled() = default;
	ThreediAssembled(const ThreediAssembled &) = delete;
	ThreediAssembled &operator=(const ThreediAssembled &) = delete;

	Threedi3di3 model{};
	std::vector<ThreediLod> lods;
	std::vector<std::vector<ThreediVertex>> lod_vertices;
	std::vector<std::vector<uint16_t>> lod_indices;
	std::vector<std::vector<ThreediTriangleStrip>> lod_strips;
	std::vector<std::vector<ThreediRenderObject>> lod_parts;
	std::vector<std::vector<ThreediPartAnimation>> lod_panm;
	std::vector<ThreediMaterial> materials;
	std::vector<ThreediLight> lights;
	std::vector<ThreediUserPoint> user_points;
	std::vector<ThreediControlRegister> registers;
	std::vector<ThreediMatrix4x4> matrices;
	ThreediCollisionModel collision{};
	std::vector<ThreediBoundingPlane> planes;
	std::vector<ThreediBoundingVolume> volumes;
	std::vector<ThreediCollisionVertex> vertices;
	std::vector<ThreediCollisionNormal> normals;
	std::vector<ThreediCollisionFace> faces;
	std::vector<ThreediCollisionObject> objects;
	std::vector<ThreediCollisionTranslation> translations;
	std::vector<ThreediOcclusionVertex> occ_vertices;
	std::vector<ThreediOcclusionPlane> occ_planes;
	std::vector<ThreediOcclusionFace> occ_faces;
	std::vector<ThreediOcclusionObject> occ_objects;
};

void assemble(const ThreediBuildModel &m, ThreediAssembled &out) {
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
					for (int k = 0; k < 3; ++k) {
						rec.min[k] = static_cast<float>(smn[k]);
						rec.max[k] = static_cast<float>(smx[k]);
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
			// CXLT without a given table (threedi_build.h): one row per
			// non-root section on a rigid model, one per section on a skinned
			// one, at the section offset.
			if (!m.translations_given && (oi > 0 || m.skinned)) {
				ThreediCollisionTranslation t{};
				t.translation[0] = o.offset[0];
				t.translation[1] = o.offset[1];
				t.translation[2] = o.offset[2];
				out.translations.push_back(t);
			}
		}
		if (m.translations_given)
			for (const ThreediBuildVec3 &p : m.translations) {
				ThreediCollisionTranslation t{};
				t.translation[0] = threedi_q16_trunc(p.x);
				t.translation[1] = threedi_q16_trunc(p.y);
				t.translation[2] = threedi_q16_trunc(p.z);
				out.translations.push_back(t);
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

} // namespace

int ThreediBuildModel::add_lod(int32_t threshold, const char *type) {
	ThreediBuildLod lod;
	lod.threshold = threshold;
	lod.type = type;
	lods.push_back(lod);
	return static_cast<int>(lods.size()) - 1;
}

int ThreediBuildModel::add_part(int lod, int parent, ThreediBuildVec3 pivot, const ThreediBuildVec3 *rel) {
	ThreediBuildPart part;
	part.parent = parent;
	part.pivot = pivot;
	if (rel != nullptr) {
		part.has_rel = true;
		part.rel = *rel;
	}
	lods[lod].parts.push_back(part);
	return static_cast<int>(lods[lod].parts.size()) - 1;
}

int ThreediBuildModel::add_material(const char *shader, const char *texture, uint8_t slot) {
	ThreediMaterial m{};
	m.index = static_cast<int32_t>(materials.size());
	std::snprintf(m.shader_name, sizeof(m.shader_name), "%s", shader);
	if (texture != nullptr && texture[0] != '\0') {
		m.texture_count = 1;
		std::snprintf(m.textures[0].name, sizeof(m.textures[0].name), "%s", texture);
		m.textures[0].slot = slot;
		m.textures[0].type = THREEDI_TEX_TYPE_DIFFUSE;
	}
	m.alpha_gen.reg = -1;
	m.rgb_gen.reg = -1;
	m.rgb_gen2.reg = -1;
	m.u_params.reg = -1;
	m.v_params.reg = -1;
	materials.push_back(m);
	return m.index;
}

void ThreediBuildModel::set_rgb_gen(int material, uint8_t style, int reg, double rate, const int start_rgb[3],
		const int end_rgb[3]) {
	ThreediRgbGen &g = materials[material].rgb_gen;
	g.style = style;
	g.reg = style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD ? reg : -1;
	g.phase = 0.0f;
	g.rate = threedi_q8f(rate);
	for (int k = 0; k < 3; ++k) {
		g.start_color[k] = threedi_byte_unit(start_rgb[k]);
		g.end_color[k] = threedi_byte_unit(end_rgb[k]);
	}
	g.start_color[3] = 0.0f;
	g.end_color[3] = 0.0f;
}

ThreediPartAnimation &ThreediBuildModel::add_panm(int lod, int part, int parent, uint32_t flags) {
	ThreediPartAnimation row = threedi_build_inert_panm(part, parent);
	row.flags = flags;
	lods[lod].panm.push_back(row);
	return lods[lod].panm.back();
}

int ThreediBuildModel::add_user_point(const char *point_name, ThreediBuildVec3 pos, ThreediBuildVec3 dir, int subobject,
		int32_t type) {
	ThreediUserPoint p{};
	p.x = threedi_q16(pos.x);
	p.y = threedi_q16(pos.y);
	p.z = threedi_q16(pos.z);
	p.rot_x = threedi_q16(dir.x);
	p.rot_y = threedi_q16(dir.y);
	p.rot_z = threedi_q16(dir.z);
	p.subobject_index = subobject;
	p.userpoint_type = type;
	std::snprintf(p.name, sizeof(p.name), "%s", point_name);
	user_points.push_back(p);
	return static_cast<int>(user_points.size()) - 1;
}

int ThreediBuildModel::add_light(ThreediBuildVec3 pos, double atten_start, double atten_end, uint8_t style, int subobject,
		const int rgb_start[3], const int rgb_end[3], uint8_t flags, uint8_t phase, uint16_t rate, ThreediBuildVec3 dir,
		double falloff) {
	ThreediLight l{};
	const ThreediBuildVec3 m = threedi_build_to_model(pos);
	l.offset[0] = static_cast<float>(m.x);
	l.offset[1] = static_cast<float>(m.y);
	l.offset[2] = static_cast<float>(m.z);
	l.atten_start = static_cast<float>(atten_start);
	l.atten_end = static_cast<float>(atten_end);
	l.style = style;
	l.phase = phase;
	l.rate = rate;
	l.color_start[0] = static_cast<uint8_t>(rgb_start[2]);
	l.color_start[1] = static_cast<uint8_t>(rgb_start[1]);
	l.color_start[2] = static_cast<uint8_t>(rgb_start[0]);
	l.color_end[0] = static_cast<uint8_t>(rgb_end[2]);
	l.color_end[1] = static_cast<uint8_t>(rgb_end[1]);
	l.color_end[2] = static_cast<uint8_t>(rgb_end[0]);
	l.subobj_index = static_cast<uint8_t>(subobject);
	l.flags = flags;
	l.falloff_byte = static_cast<uint8_t>(static_cast<int32_t>(falloff) & 0xFF);
	const ThreediBuildVec3 d = threedi_build_to_model(dir);
	// + 0.0f folds the axis map's negative zeros: retail stores +0.0.
	l.rotation[0] = static_cast<float>(d.x) + 0.0f;
	l.rotation[1] = static_cast<float>(d.y) + 0.0f;
	l.rotation[2] = static_cast<float>(d.z) + 0.0f;
	l.rotation[3] = std::cos(static_cast<float>(falloff) * 0.017453292f);
	threedi_build_light_view_proj(l, static_cast<float>(falloff));
	lights.push_back(l);
	return static_cast<int>(lights.size()) - 1;
}

int ThreediBuildModel::add_control_register(const char *register_name) {
	control_registers.emplace_back(register_name);
	return static_cast<int>(control_registers.size()) - 1;
}

int ThreediBuildModel::add_cobj(int parent_part, ThreediBuildVec3 offset) {
	ThreediBuildCollisionObject o;
	o.parent_part = parent_part;
	o.offset = offset;
	collision.push_back(o);
	return static_cast<int>(collision.size()) - 1;
}

void ThreediBuildModel::add_volume(int cobj, int32_t type, int32_t flags, const ThreediBuildBox &box) {
	ThreediBuildCollisionObject &o = collision[cobj];
	ThreediBoundingVolume v{};
	v.collidable_type = type;
	v.flags = flags;
	v.min_x_fp16 = threedi_q16(box.min.x);
	v.min_y_fp16 = threedi_q16(box.min.y);
	v.min_z_fp16 = threedi_q16(box.min.z);
	v.max_x_fp16 = threedi_q16(box.max.x);
	v.max_y_fp16 = threedi_q16(box.max.y);
	v.max_z_fp16 = threedi_q16(box.max.z);
	v.plane_count = 6;
	o.volumes.push_back(v);
	o.volume_sources.push_back(volume_source(box));
	const double n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const double d[6] = {-box.max.x, box.min.x, -box.max.y, box.min.y, -box.max.z, box.min.z};
	for (int p = 0; p < 6; ++p) {
		ThreediBoundingPlane plane{};
		plane.normal[0] = threedi_q14f(n[p][0]);
		plane.normal[1] = threedi_q14f(n[p][1]);
		plane.normal[2] = threedi_q14f(n[p][2]);
		plane.radius = threedi_q16f(d[p]);
		o.planes.push_back(plane);
	}
}

void ThreediBuildModel::add_volume_planes(int cobj, int32_t type, int32_t flags, const ThreediBuildBox &box,
		const std::vector<ThreediBoundingPlane> &volume_planes) {
	ThreediBuildCollisionObject &o = collision[cobj];
	ThreediBoundingVolume v{};
	v.collidable_type = type;
	v.flags = flags;
	v.min_x_fp16 = threedi_q16(box.min.x);
	v.min_y_fp16 = threedi_q16(box.min.y);
	v.min_z_fp16 = threedi_q16(box.min.z);
	v.max_x_fp16 = threedi_q16(box.max.x);
	v.max_y_fp16 = threedi_q16(box.max.y);
	v.max_z_fp16 = threedi_q16(box.max.z);
	v.plane_count = static_cast<int32_t>(volume_planes.size());
	o.volumes.push_back(v);
	o.volume_sources.push_back(volume_source(box));
	o.planes.insert(o.planes.end(), volume_planes.begin(), volume_planes.end());
}

double ThreediBuildModel::add_volume_mesh(int cobj, int32_t type, int32_t flags,
		const std::vector<ThreediBuildVec3> &verts, const std::vector<std::array<int, 3>> &tris) {
	ThreediBuildCollisionObject &o = collision[cobj];
	ThreediBuildVolumeSource src;
	src.meshed = true;
	float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
	std::vector<std::array<float, 3>> p;
	for (const ThreediBuildVec3 &v : verts) p.push_back({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
	for (size_t i = 0; i < p.size(); ++i)
		for (int k = 0; k < 3; ++k) {
			mn[k] = i == 0 ? p[i][k] : std::min(mn[k], p[i][k]);
			mx[k] = i == 0 ? p[i][k] : std::max(mx[k], p[i][k]);
		}
	struct Plane {
		float n[3];
		float d;
	};
	std::vector<Plane> planes = {{{1.f, 0.f, 0.f}, -mx[0]}, {{-1.f, 0.f, 0.f}, mn[0]}, {{0.f, 1.f, 0.f}, -mx[1]},
			{{0.f, -1.f, 0.f}, mn[1]}, {{0.f, 0.f, 1.f}, -mx[2]}, {{0.f, 0.f, -1.f}, mn[2]}};
	size_t last = 0;
	for (const std::array<int, 3> &t : tris) {
		const std::array<float, 3> &a = p[t[0]], &b = p[t[1]], &c = p[t[2]];
		const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
		const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
		const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		int index = 0;
		if (len > 0.0001f) {
			const float inv = 1.0f / len;
			for (float &x : n) x *= inv;
			const float d = -(n[0] * a[0] + n[1] * a[1] + n[2] * a[2]);
			bool matched = false;
			for (size_t k = 0; k < planes.size(); ++k)
				if (std::fabs(planes[k].n[0] - n[0]) <= 0.005f && std::fabs(planes[k].n[1] - n[1]) <= 0.005f &&
						std::fabs(planes[k].n[2] - n[2]) <= 0.005f && std::fabs(planes[k].d - d) <= 0.03f) {
					index = static_cast<int>(k);
					last = k;
					matched = true;
				}
			if (!matched) {
				index = static_cast<int>(planes.size());
				last = planes.size();
				planes.push_back({{n[0], n[1], n[2]}, d});
			}
		}
		src.face_planes.push_back(index);
		src.face_boxes.push_back({std::min({a[0], b[0], c[0]}), std::min({a[1], b[1], c[1]}), std::min({a[2], b[2], c[2]}),
				std::max({a[0], b[0], c[0]}), std::max({a[1], b[1], c[1]}), std::max({a[2], b[2], c[2]})});
	}
	if (type == 4) std::swap(planes[0], planes[last]);
	ThreediBoundingVolume v{};
	v.collidable_type = type;
	v.flags = flags;
	v.min_x_fp16 = threedi_q16_trunc(mn[0]);
	v.min_y_fp16 = threedi_q16_trunc(mn[1]);
	v.min_z_fp16 = threedi_q16_trunc(mn[2]);
	v.max_x_fp16 = threedi_q16_trunc(mx[0]);
	v.max_y_fp16 = threedi_q16_trunc(mx[1]);
	v.max_z_fp16 = threedi_q16_trunc(mx[2]);
	v.plane_count = static_cast<int32_t>(planes.size());
	o.volumes.push_back(v);
	for (int k = 0; k < 3; ++k) {
		src.box[k] = mn[k];
		src.box[k + 3] = mx[k];
	}
	o.volume_sources.push_back(std::move(src));
	for (const Plane &q : planes) {
		ThreediBoundingPlane plane{};
		for (int k = 0; k < 3; ++k) plane.normal[k] = threedi_q14f_trunc(q.n[k]);
		plane.radius = static_cast<float>(threedi_q16_trunc(q.d)) / io::kFp16One;
		o.planes.push_back(plane);
	}
	// How far the authored vertices reach outside the solid the planes
	// bound: the volume is that solid, so a non-convex mesh loses the rest.
	double outside = 0.0;
	for (const std::array<float, 3> &v : p)
		for (const Plane &q : planes)
			outside = std::max(outside, static_cast<double>(q.n[0]) * v[0] + static_cast<double>(q.n[1]) * v[1] +
					static_cast<double>(q.n[2]) * v[2] + q.d);
	return outside;
}

bool ThreediBuildModel::add_face(int cobj, uint16_t a, uint16_t b, uint16_t c, uint8_t poly_type,
		uint32_t material_flags, const ThreediBuildVec3 *given) {
	ThreediBuildCollisionObject &o = collision[cobj];
	const ThreediCollisionVertex &va = o.vertices[a];
	const ThreediCollisionVertex &vb = o.vertices[b];
	const ThreediCollisionVertex &vc = o.vertices[c];
	const ThreediBuildVec3 &pa = o.exact[a], &pb = o.exact[b], &pc = o.exact[c];
	const double ex = pb.x - pa.x, ey = pb.y - pa.y, ez = pb.z - pa.z;
	const double fx = pc.x - pa.x, fy = pc.y - pa.y, fz = pc.z - pa.z;
	double nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
	if (given != nullptr) {
		// Stored as given: retail's Q14 normals are not all unit length.
		nx = given->x;
		ny = given->y;
		nz = given->z;
	} else {
		const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
		if (!(len > 0.0)) return false;
		nx /= len;
		ny /= len;
		nz /= len;
	}
	// Q14, truncated, and the dominant axis compared on those integers (z
	// wins only strictly, then y; a tie goes to x) [orig: WriteCNRM @ 0x454600].
	ThreediCollisionNormal normal{};
	normal.normal[0] = threedi_q14f_trunc(static_cast<float>(nx));
	normal.normal[1] = threedi_q14f_trunc(static_cast<float>(ny));
	normal.normal[2] = threedi_q14f_trunc(static_cast<float>(nz));
	const int ax = std::abs(static_cast<int>(normal.normal[0] * io::kFp14One));
	const int ay = std::abs(static_cast<int>(normal.normal[1] * io::kFp14One));
	const int az = std::abs(static_cast<int>(normal.normal[2] * io::kFp14One));
	normal.dominate_axis = (az > ax && az > ay) ? 1 : (ay > ax && ay > az) ? 2 : 4;
	int16_t normal_index = -1;
	for (size_t i = 0; i < o.normals.size(); ++i) {
		if (std::memcmp(&o.normals[i], &normal, sizeof(normal)) == 0) {
			normal_index = static_cast<int16_t>(i);
			break;
		}
	}
	if (normal_index < 0) {
		normal_index = static_cast<int16_t>(o.normals.size());
		o.normals.push_back(normal);
	}
	ThreediCollisionFace face{};
	face.vert_index[0] = static_cast<int16_t>(a);
	face.vert_index[1] = static_cast<int16_t>(b);
	face.vert_index[2] = static_cast<int16_t>(c);
	face.normal_index = normal_index;
	// The face's plane as the runtime tests it (n . p + plane_dist, zero on
	// the plane; collision_query.cpp): -(n . v0), and the corners' box,
	// both truncated as WriteCFAC stores them [orig: WriteCFAC @ 0x454830;
	// 5fc5b4f6a^:engine/formats/oed/export_3di.cpp]. OED took them from the
	// unquantized corner and normal; these come from the stored (CVRT,
	// CNRM) ones, under 1/256 unit away, so build(scene(x)) re-mints x
	// byte for byte.
	face.plane_dist_fp16 = threedi_q16_trunc(-(static_cast<double>(normal.normal[0]) * va.position[0] +
			static_cast<double>(normal.normal[1]) * va.position[1] + static_cast<double>(normal.normal[2]) * va.position[2]));
	double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
	for (const ThreediCollisionVertex *v : {&va, &vb, &vc}) {
		for (int k = 0; k < 3; ++k) {
			mn[k] = std::min<double>(mn[k], v->position[k]);
			mx[k] = std::max<double>(mx[k], v->position[k]);
		}
	}
	face.min_x_fp16 = threedi_q16_trunc(mn[0]);
	face.min_y_fp16 = threedi_q16_trunc(mn[1]);
	face.min_z_fp16 = threedi_q16_trunc(mn[2]);
	face.max_x_fp16 = threedi_q16_trunc(mx[0]);
	face.max_y_fp16 = threedi_q16_trunc(mx[1]);
	face.max_z_fp16 = threedi_q16_trunc(mx[2]);
	face.material_flags = material_flags;
	face.poly_type = poly_type;
	o.faces.push_back(face);
	return true;
}

uint16_t ThreediBuildModel::add_collision_vertex(int cobj, ThreediBuildVec3 p) {
	ThreediCollisionVertex v{};
	v.position[0] = threedi_q8f_trunc(p.x);
	v.position[1] = threedi_q8f_trunc(p.y);
	v.position[2] = threedi_q8f_trunc(p.z);
	collision[cobj].vertices.push_back(v);
	collision[cobj].exact.push_back(p);
	return static_cast<uint16_t>(collision[cobj].vertices.size() - 1);
}

bool ThreediBuildModel::add_occ_record(uint8_t type, int section_a, int section_b,
		const std::vector<ThreediBuildVec3> &verts, const std::vector<std::array<int, 4>> &faces,
		const std::vector<std::array<double, 4>> &explicit_planes) {
	ThreediBuildOcclusionRecord rec;
	for (const ThreediBuildVec3 &v : verts) rec.vertices.push_back(occ_vertex(v));
	std::vector<std::array<double, 4>> planes = explicit_planes;
	std::vector<int> face_plane(faces.size(), 0);
	for (size_t f = 0; f < faces.size(); ++f) face_plane[f] = faces[f][3];
	if (explicit_planes.empty() && !verts.empty()) {
		double mn[3] = {verts[0].x, verts[0].y, verts[0].z}, mx[3] = {verts[0].x, verts[0].y, verts[0].z};
		for (const ThreediBuildVec3 &v : verts) {
			const double p[3] = {v.x, v.y, v.z};
			for (int k = 0; k < 3; ++k) {
				mn[k] = std::min(mn[k], p[k]);
				mx[k] = std::max(mx[k], p[k]);
			}
		}
		planes = {{1, 0, 0, -mx[0]}, {-1, 0, 0, mn[0]}, {0, 1, 0, -mx[1]}, {0, -1, 0, mn[1]}, {0, 0, 1, -mx[2]},
				{0, 0, -1, mn[2]}};
		for (size_t f = 0; f < faces.size(); ++f) {
			if (face_plane[f] >= 0) continue;
			const ThreediBuildVec3 &p0 = verts[faces[f][0]], &p1 = verts[faces[f][1]], &p2 = verts[faces[f][2]];
			const float e1[3] = {static_cast<float>(p1.x - p0.x), static_cast<float>(p1.y - p0.y),
					static_cast<float>(p1.z - p0.z)};
			const float e2[3] = {static_cast<float>(p2.x - p0.x), static_cast<float>(p2.y - p0.y),
					static_cast<float>(p2.z - p0.z)};
			float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
			const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			int index = 0;
			if (len > 0.0001f) {
				for (float &c : n) c /= len;
				const double d = -(n[0] * p0.x + n[1] * p0.y + n[2] * p0.z);
				index = -1;
				for (size_t k = 0; k < planes.size(); ++k)
					if (std::fabs(planes[k][0] - n[0]) <= 0.005 && std::fabs(planes[k][1] - n[1]) <= 0.005 &&
							std::fabs(planes[k][2] - n[2]) <= 0.005 && std::fabs(planes[k][3] - d) <= 0.03)
						index = static_cast<int>(k);
				if (index < 0) {
					if (planes.size() >= 32) return false;
					index = static_cast<int>(planes.size());
					planes.push_back({n[0], n[1], n[2], d});
				}
			}
			face_plane[f] = index;
		}
	}
	for (const std::array<double, 4> &p : planes) {
		const ThreediBuildVec3 n = threedi_build_to_model(ThreediBuildVec3{p[0], p[1], p[2]});
		ThreediOcclusionPlane plane{};
		plane.normal[0] = static_cast<float>(n.x);
		plane.normal[1] = static_cast<float>(n.y);
		plane.normal[2] = static_cast<float>(n.z);
		plane.radius = static_cast<float>(p[3]);
		rec.planes.push_back(plane);
	}
	for (size_t f = 0; f < faces.size(); ++f)
		rec.faces.push_back(occ_face(faces[f][0], faces[f][1], faces[f][2], face_plane[f]));
	finish_occlusion_record(occlusion, rec, type, section_a, section_b);
	return true;
}

bool threedi_build_mint(const ThreediBuildModel &m, std::vector<uint8_t> &out) {
	ThreediAssembled assembled;
	assemble(m, assembled);
	return threedi_3di3_write_memory(&assembled.model, out) == 0;
}

} // namespace opennova::threedi
