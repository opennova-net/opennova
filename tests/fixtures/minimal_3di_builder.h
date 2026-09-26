// The synthetic 3DI3 model builder behind tests/fixtures/minimal_3di_gen.cpp:
// the engine's construction API (formats/threedi/threedi_build.h) with the
// recipe shapes the generator authors (boxes, collision face boxes and quads,
// octagonal prisms, occlusion boxes and quads). Every committed
// fixtures/threedi/synth model is minted by the same builder and parity writer
// opennova-3di uses (ADR 0003, ADR 0047), so the fixtures follow the rules a
// shipped model does: retail's windings, the exporter's quantization and its
// derived bounds.
//
// Frames (docs/threedi/3di-gp-format-re.md): the recipes author MISSION axes
// (x forward, y left, z up); the builder stores render data in MODEL axes
// (-y, z, x) and the collision block and user points in mission axes.
#pragma once

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace synth3di {

using Vec3 = opennova::threedi::ThreediBuildVec3;
using Box = opennova::threedi::ThreediBuildBox;

inline float byte_unit(int c) { return opennova::threedi::threedi_byte_unit(c); }

// User point kinds as the retail corpus spells them: 71 ('G') for gameplay
// points (seats, ground, cameras), 83 ('S') for effect/particle points.
constexpr int32_t kUserPointGameplay = 71;
constexpr int32_t kUserPointEffect = 83;

inline opennova::threedi::ThreediPartAnimation inert_panm(int part, int parent) {
	return opennova::threedi::threedi_build_inert_panm(part, parent);
}

inline opennova::threedi::ThreediTransform track(uint8_t control, uint8_t param, int16_t rate, int16_t start, int16_t end) {
	return opennova::threedi::threedi_build_track(control, param, rate, start, end);
}

struct Model : opennova::threedi::ThreediBuildModel {
	// A second texture on a material, the detail stage (UV1) of FF_MT shaders.
	void add_detail_texture(int material, const char *texture) {
		opennova::threedi::ThreediMaterial &m = materials[material];
		opennova::threedi::ThreediMaterialTexture &t = m.textures[m.texture_count++];
		std::snprintf(t.name, sizeof(t.name), "%s", texture);
		t.slot = opennova::threedi::THREEDI_TEX_SLOT_DETAIL;
		t.type = opennova::threedi::THREEDI_TEX_TYPE_DIFFUSE;
	}

	// An axis box (mission axes) as one triangle-list strip of 24 vertices
	// in ABSOLUTE model coordinates, wound counter-clockwise about each face
	// normal in model space (the retail winding); `bone` makes it a skinned
	// strip riding that bone.
	void add_box(int lod, int part, int material, const Box &box, bool alpha = false, int bone = -1) {
		opennova::threedi::ThreediBuildStrip strip;
		strip.material = material;
		strip.alpha = alpha;
		strip.bone = bone;
		static const int kFaceNormal[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		for (int f = 0; f < 6; ++f) {
			const int axis = f / 2;
			const bool positive = (f % 2) == 0;
			const int u_axis = (axis + 1) % 3;
			const int v_axis = (axis + 2) % 3;
			Vec3 corners[4];
			const double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (int c = 0; c < 4; ++c) {
				double p[3] = {box.min.x, box.min.y, box.min.z};
				const double hi[3] = {box.max.x, box.max.y, box.max.z};
				p[axis] = positive ? hi[axis] : p[axis];
				p[u_axis] = uv[c][0] > 0.5 ? hi[u_axis] : p[u_axis];
				p[v_axis] = uv[c][1] > 0.5 ? hi[v_axis] : p[v_axis];
				corners[c] = Vec3{p[0], p[1], p[2]};
			}
			const Vec3 n = opennova::threedi::threedi_build_to_model(Vec3{static_cast<double>(kFaceNormal[f][0]),
					static_cast<double>(kFaceNormal[f][1]), static_cast<double>(kFaceNormal[f][2])});
			const uint16_t base = static_cast<uint16_t>(strip.vertices.size());
			for (int c = 0; c < 4; ++c) {
				const Vec3 m = opennova::threedi::threedi_build_to_model(corners[c]);
				opennova::threedi::ThreediVertex v{};
				v.position[0] = static_cast<float>(m.x);
				v.position[1] = static_cast<float>(m.y);
				v.position[2] = static_cast<float>(m.z);
				v.normal[0] = static_cast<float>(n.x);
				v.normal[1] = static_cast<float>(n.y);
				v.normal[2] = static_cast<float>(n.z);
				v.uv0[0] = static_cast<float>(uv[c][0]);
				v.uv0[1] = static_cast<float>(uv[c][1]);
				v.uv1[0] = v.uv0[0];
				v.uv1[1] = v.uv0[1];
				if (bone >= 0) {
					v.bone_weights[0] = 1.0f;
					v.is_skinned = 1;
				}
				strip.vertices.push_back(v);
			}
			// Two triangles per face, wound so cross(e1, e2) points along n.
			const Vec3 a = opennova::threedi::threedi_build_to_model(corners[0]);
			const Vec3 b = opennova::threedi::threedi_build_to_model(corners[1]);
			const Vec3 c = opennova::threedi::threedi_build_to_model(corners[2]);
			const double ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
			const double fx = c.x - a.x, fy = c.y - a.y, fz = c.z - a.z;
			const double cx = ey * fz - ez * fy, cy = ez * fx - ex * fz, cz = ex * fy - ey * fx;
			const bool ccw = cx * n.x + cy * n.y + cz * n.z > 0.0;
			const uint16_t tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
			for (int t = 0; t < 2; ++t) {
				strip.indices.push_back(static_cast<uint16_t>(base + tri[t][0]));
				strip.indices.push_back(static_cast<uint16_t>(base + (ccw ? tri[t][1] : tri[t][2])));
				strip.indices.push_back(static_cast<uint16_t>(base + (ccw ? tri[t][2] : tri[t][1])));
			}
		}
		lods[lod].parts[part].strips.push_back(strip);
	}

	// A person bone's section: a hit sphere and no geometry.
	int add_sphere_cobj(int parent_part, Vec3 offset, Vec3 center, double radius) {
		const int index = add_cobj(parent_part, offset);
		collision[index].sphere = true;
		collision[index].sphere_center = center;
		collision[index].sphere_radius = radius;
		return index;
	}

	// A regular octagonal prism (eight side planes + two caps) around (cx, cy).
	void add_prism8(int cobj, int32_t type, int32_t flags, double cx, double cy, double radius, double z0, double z1) {
		std::vector<opennova::threedi::ThreediBoundingPlane> planes;
		for (int s = 0; s < 8; ++s) {
			const double angle = s * 3.14159265358979323846 / 4.0;
			const double nx = opennova::threedi::threedi_q14f(std::cos(angle));
			const double ny = opennova::threedi::threedi_q14f(std::sin(angle));
			opennova::threedi::ThreediBoundingPlane plane{};
			plane.normal[0] = static_cast<float>(nx);
			plane.normal[1] = static_cast<float>(ny);
			plane.radius = opennova::threedi::threedi_q16f(-(nx * cx + ny * cy + radius));
			planes.push_back(plane);
		}
		opennova::threedi::ThreediBoundingPlane top{};
		top.normal[2] = 1.0f;
		top.radius = opennova::threedi::threedi_q16f(-z1);
		planes.push_back(top);
		opennova::threedi::ThreediBoundingPlane bottom{};
		bottom.normal[2] = -1.0f;
		bottom.radius = opennova::threedi::threedi_q16f(z0);
		planes.push_back(bottom);
		add_volume_planes(cobj, type, flags, Box{{cx - radius, cy - radius, z0}, {cx + radius, cy + radius, z1}}, planes);
	}

	// A closed axis box as twelve collision faces over eight shared vertices,
	// each side counter-clockwise from outside (mission axes), the winding
	// retail stores.
	void add_face_box(int cobj, const Box &box, uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[8];
		for (int i = 0; i < 8; ++i)
			c[i] = add_collision_vertex(cobj, Vec3{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
					(i & 4) ? box.max.z : box.min.z});
		static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		for (const int *q : kSides) {
			add_face(cobj, c[q[0]], c[q[1]], c[q[2]], poly_type, material_flags);
			add_face(cobj, c[q[0]], c[q[2]], c[q[3]], poly_type, material_flags);
		}
	}

	// A single quad (two faces) over four mission-axes corners, listed
	// counter-clockwise from the side the faces face.
	void add_face_quad(int cobj, const Vec3 corners[4], uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[4];
		for (int i = 0; i < 4; ++i) c[i] = add_collision_vertex(cobj, corners[i]);
		add_face(cobj, c[0], c[1], c[2], poly_type, material_flags);
		add_face(cobj, c[0], c[2], c[3], poly_type, material_flags);
	}

	// A quad portal or window: four corners wound consistently, one plane
	// whose normal points from section_a toward section_b.
	void add_occ_quad(uint8_t type, int section_a, int section_b, const Vec3 corners[4], Vec3 normal) {
		const std::vector<Vec3> verts(corners, corners + 4);
		const double d = -(normal.x * corners[0].x + normal.y * corners[0].y + normal.z * corners[0].z);
		add_occ_record(type, section_a, section_b, verts, {{0, 1, 2, 0}, {0, 2, 3, 0}}, {{normal.x, normal.y, normal.z, d}});
	}

	// A closed axis-box occluder or open record: 8 vertices, 6 outward planes,
	// 12 faces (each side counter-clockwise from outside, mission axes).
	void add_occ_box(uint8_t type, int section_a, int section_b, const Box &box) {
		std::vector<Vec3> verts;
		for (int i = 0; i < 8; ++i)
			verts.push_back(Vec3{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
					(i & 4) ? box.max.z : box.min.z});
		const double n[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
		static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		std::vector<std::array<double, 4>> planes;
		std::vector<std::array<int, 4>> faces;
		for (int s = 0; s < 6; ++s) {
			const Vec3 &on = verts[kSides[s][0]];
			planes.push_back({n[s][0], n[s][1], n[s][2], -(n[s][0] * on.x + n[s][1] * on.y + n[s][2] * on.z)});
			faces.push_back({kSides[s][0], kSides[s][1], kSides[s][2], s});
			faces.push_back({kSides[s][0], kSides[s][2], kSides[s][3], s});
		}
		add_occ_record(type, section_a, section_b, verts, faces, planes);
	}
};

// Mint `m` through the construction API and the parity writer. False when
// the writer refused the model.
inline bool mint(const Model &m, std::vector<uint8_t> &out) { return opennova::threedi::threedi_build_mint(m, out); }

// A minted model read back, for a caller that wants the Threedi3di3 itself
// (tests/world/npc_weapons_test.cpp writes it where a loader finds it).
struct Assembled {
	Assembled() = default;
	Assembled(const Assembled &) = delete;
	Assembled &operator=(const Assembled &) = delete;
	~Assembled() { opennova::threedi::threedi_3di3_free(&model); }
	opennova::threedi::Threedi3di3 model{};
};

inline bool assemble(const Model &m, Assembled &out) {
	std::vector<uint8_t> bytes;
	return mint(m, bytes) && opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &out.model) == 0;
}

} // namespace synth3di
