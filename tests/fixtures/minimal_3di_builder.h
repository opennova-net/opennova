// The synthetic 3DI3 model builder behind tests/fixtures/minimal_3di_gen.cpp:
// authors a Threedi3di3 in memory from small integer-friendly data and hands
// it to the parity writer (threedi_3di3_write), so every committed
// fixtures/threedi/synth model is minted by our own writer from scratch
// (ADR 0003) and re-minted byte-for-byte on every platform.
//
// Frames (docs/threedi/3di-gp-format-re.md; the runtime conversions in
// godot/src/object/object_data_geometry.cpp and engine/runtime/simassets):
//   - MISSION axes (x forward, y left, z up) are the authoring frame here and
//     the on-disk frame of the collision block (CVRT/CNRM/CFAC/BPLN/BVOL/COBJ/
//     CXLT/CMDL) and of the user points (USRP 16.16 ints).
//   - MODEL axes = (-y, z, x) of mission: the on-disk frame of VERT/STRP/ROBJ,
//     the LGHT offsets and the OCCL tables. Render vertices, strip bounds and
//     part bound centers are ABSOLUTE model coordinates (the PANM node
//     matrices pivot about ROBJ abs themselves; the retail corpus stores
//     wheels and rockers that way), strip indices are strip-relative.
// Every float that the writer quantizes (16.16, Q14, Q8, byte colors) is
// stored pre-quantized so a read -> write round trip reproduces the bytes.
#ifndef OPENNOVA_TESTS_MINIMAL_3DI_BUILDER_H
#define OPENNOVA_TESTS_MINIMAL_3DI_BUILDER_H

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace synth3di {

struct Vec3 {
	double x = 0.0, y = 0.0, z = 0.0;
};

struct Box {
	Vec3 min, max;
};

inline Vec3 to_model(const Vec3 &m) { return Vec3{-m.y, m.z, m.x}; }
inline int32_t q16(double v) { return static_cast<int32_t>(std::lround(v * 65536.0)); }
inline float q16f(double v) { return static_cast<float>(q16(v)) / 65536.0f; }
inline float q14f(double v) { return static_cast<float>(std::lround(v * 16384.0)) / 16384.0f; }
inline float q8f(double v) { return static_cast<float>(std::lround(v * 256.0)) / 256.0f; }
inline float byte_unit(int c) { return static_cast<float>(c) / 255.0f; }

// User point kinds as the retail corpus spells them: 71 ('G') for gameplay
// points (seats, ground, cameras), 83 ('S') for effect/particle points.
constexpr int32_t kUserPointGameplay = 71;
constexpr int32_t kUserPointEffect = 83;

struct Strip {
	int material = 0;
	bool alpha = false;
	int bone = -1; // skinned strips: the skeleton bone every vertex rides
	std::vector<opennova::threedi::ThreediVertex> vertices;
	std::vector<uint16_t> indices;
};

struct Part {
	int parent = 0; // the root references itself
	Vec3 pivot;     // mission axes
	std::vector<Strip> strips;
};

struct Lod {
	std::string type = "gnrc";
	int32_t threshold = 0;
	std::vector<Part> parts;
	std::vector<opennova::threedi::ThreediPartAnimation> panm;
};

struct CollisionObject {
	int parent_part = 0;
	Vec3 offset; // mission axes
	std::vector<opennova::threedi::ThreediCollisionVertex> vertices;
	std::vector<opennova::threedi::ThreediCollisionNormal> normals;
	std::vector<opennova::threedi::ThreediCollisionFace> faces;
	std::vector<opennova::threedi::ThreediBoundingVolume> volumes;
	std::vector<opennova::threedi::ThreediBoundingPlane> planes;
	bool sphere = false; // sphere-only skeletal section (person bones)
	Vec3 sphere_center;
	double sphere_radius = 0.0;
};

struct OcclusionRecord {
	opennova::threedi::ThreediOcclusionObject object{};
	std::vector<opennova::threedi::ThreediOcclusionVertex> vertices;
	std::vector<opennova::threedi::ThreediOcclusionPlane> planes;
	std::vector<opennova::threedi::ThreediOcclusionFace> faces;
};

inline opennova::threedi::ThreediPartAnimation inert_panm(int part, int parent) {
	opennova::threedi::ThreediPartAnimation row{};
	row.parent_subobject = static_cast<uint8_t>(parent);
	row.subobject_index = static_cast<uint8_t>(part);
	return row;
}

inline opennova::threedi::ThreediTransform track(uint8_t control, uint8_t param, int16_t rate, int16_t start, int16_t end) {
	opennova::threedi::ThreediTransform t{};
	t.control = control;
	t.control_param = param;
	t.rate = rate;
	t.start = start;
	t.end = end;
	return t;
}

struct Model {
	std::string name;
	bool skinned = false;
	std::vector<Lod> lods;
	std::vector<opennova::threedi::ThreediMaterial> materials;
	std::vector<opennova::threedi::ThreediLight> lights;
	std::vector<opennova::threedi::ThreediUserPoint> user_points;
	std::vector<std::string> control_registers;
	std::vector<CollisionObject> collision;
	std::vector<OcclusionRecord> occlusion;
	int matrix_count = 1;

	// --- render ------------------------------------------------------------
	int add_lod(int32_t threshold = 0, const char *type = "gnrc") {
		Lod lod;
		lod.threshold = threshold;
		lod.type = type;
		lods.push_back(lod);
		return static_cast<int>(lods.size()) - 1;
	}

	int add_part(int lod, int parent, Vec3 pivot) {
		Part part;
		part.parent = parent;
		part.pivot = pivot;
		lods[lod].parts.push_back(part);
		return static_cast<int>(lods[lod].parts.size()) - 1;
	}

	int add_material(const char *shader, const char *texture, uint8_t slot = opennova::threedi::THREEDI_TEX_SLOT_DIFFUSE) {
		opennova::threedi::ThreediMaterial m{};
		m.index = static_cast<int32_t>(materials.size());
		std::snprintf(m.shader_name, sizeof(m.shader_name), "%s", shader);
		if (texture != nullptr && texture[0] != '\0') {
			m.texture_count = 1;
			std::snprintf(m.textures[0].name, sizeof(m.textures[0].name), "%s", texture);
			m.textures[0].slot = slot;
			m.textures[0].type = opennova::threedi::THREEDI_TEX_TYPE_DIFFUSE;
		}
		m.alpha_gen.reg = -1;
		m.rgb_gen.reg = -1;
		m.rgb_gen2.reg = -1;
		m.u_params.reg = -1;
		m.v_params.reg = -1;
		materials.push_back(m);
		return m.index;
	}

	void add_detail_texture(int material, const char *texture) {
		opennova::threedi::ThreediMaterial &m = materials[material];
		opennova::threedi::ThreediMaterialTexture &t = m.textures[m.texture_count++];
		std::snprintf(t.name, sizeof(t.name), "%s", texture);
		t.slot = opennova::threedi::THREEDI_TEX_SLOT_DETAIL;
		t.type = opennova::threedi::THREEDI_TEX_TYPE_DIFFUSE;
	}

	// The RGB generator on a material (styles > 112 read CTRL register `reg`).
	void set_rgb_gen(int material, uint8_t style, int reg, double rate, const int start_rgb[3], const int end_rgb[3]) {
		opennova::threedi::ThreediRgbGen &g = materials[material].rgb_gen;
		g.style = style;
		g.reg = style > 112 ? reg : -1;
		g.phase = 0.0f;
		g.rate = q8f(rate);
		for (int k = 0; k < 3; ++k) {
			g.start_color[k] = byte_unit(start_rgb[k]);
			g.end_color[k] = byte_unit(end_rgb[k]);
		}
		g.start_color[3] = 0.0f;
		g.end_color[3] = 0.0f;
	}

	// An axis box (mission axes) as one triangle-list strip of 24 vertices;
	// vertices land in ABSOLUTE model coordinates, wound counter-clockwise
	// about each face normal in model space (the retail winding).
	void add_box(int lod, int part, int material, const Box &box, bool alpha = false, int bone = -1) {
		Strip strip;
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
			const Vec3 n = to_model(Vec3{static_cast<double>(kFaceNormal[f][0]),
					static_cast<double>(kFaceNormal[f][1]), static_cast<double>(kFaceNormal[f][2])});
			const uint16_t base = static_cast<uint16_t>(strip.vertices.size());
			for (int c = 0; c < 4; ++c) {
				const Vec3 m = to_model(corners[c]);
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
			const Vec3 a = to_model(corners[0]), b = to_model(corners[1]), c = to_model(corners[2]);
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

	opennova::threedi::ThreediPartAnimation &add_panm(int lod, int part, int parent, uint32_t flags = 0) {
		opennova::threedi::ThreediPartAnimation row = inert_panm(part, parent);
		row.flags = flags;
		lods[lod].panm.push_back(row);
		return lods[lod].panm.back();
	}

	// --- user points, lights, registers -------------------------------------
	int add_user_point(const char *point_name, Vec3 pos, Vec3 dir, int subobject, int32_t type) {
		opennova::threedi::ThreediUserPoint p{};
		p.x = q16(pos.x);
		p.y = q16(pos.y);
		p.z = q16(pos.z);
		p.rot_x = q16(dir.x);
		p.rot_y = q16(dir.y);
		p.rot_z = q16(dir.z);
		p.subobject_index = subobject;
		p.userpoint_type = type;
		std::snprintf(p.name, sizeof(p.name), "%s", point_name);
		user_points.push_back(p);
		return static_cast<int>(user_points.size()) - 1;
	}

	int add_light(Vec3 pos, double atten_start, double atten_end, uint8_t style, int subobject,
			const int rgb_start[3], const int rgb_end[3], uint8_t flags = 0, uint8_t phase = 0,
			uint16_t rate = 0) {
		opennova::threedi::ThreediLight l{};
		const Vec3 m = to_model(pos);
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
		l.rotation[0] = 0.0f;
		l.rotation[1] = -1.0f;
		l.rotation[2] = 0.0f;
		l.rotation[3] = 1.0f;
		lights.push_back(l);
		return static_cast<int>(lights.size()) - 1;
	}

	int add_control_register(const char *register_name) {
		control_registers.emplace_back(register_name);
		return static_cast<int>(control_registers.size()) - 1;
	}

	// --- collision (mission axes) --------------------------------------------
	// Section i pairs with render part i by ordinal; `parent_part` is retail's
	// COBJ +20 word, the PARENT of that part (the root names itself).
	int add_cobj(int parent_part, Vec3 offset = Vec3{}) {
		CollisionObject o;
		o.parent_part = parent_part;
		o.offset = offset;
		collision.push_back(o);
		return static_cast<int>(collision.size()) - 1;
	}

	int add_sphere_cobj(int parent_part, Vec3 offset, Vec3 center, double radius) {
		const int index = add_cobj(parent_part, offset);
		collision[index].sphere = true;
		collision[index].sphere_center = center;
		collision[index].sphere_radius = radius;
		return index;
	}

	// A bounding volume carved by six axis planes (normal . p + radius == 0).
	void add_volume(int cobj, int32_t type, int32_t flags, const Box &box) {
		CollisionObject &o = collision[cobj];
		opennova::threedi::ThreediBoundingVolume v{};
		v.collidable_type = type;
		v.flags = flags;
		v.min_x_fp16 = q16(box.min.x);
		v.min_y_fp16 = q16(box.min.y);
		v.min_z_fp16 = q16(box.min.z);
		v.max_x_fp16 = q16(box.max.x);
		v.max_y_fp16 = q16(box.max.y);
		v.max_z_fp16 = q16(box.max.z);
		v.plane_count = 6;
		o.volumes.push_back(v);
		const double n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		const double d[6] = {-box.max.x, box.min.x, -box.max.y, box.min.y, -box.max.z, box.min.z};
		for (int p = 0; p < 6; ++p) {
			opennova::threedi::ThreediBoundingPlane plane{};
			plane.normal[0] = q14f(n[p][0]);
			plane.normal[1] = q14f(n[p][1]);
			plane.normal[2] = q14f(n[p][2]);
			plane.radius = q16f(d[p]);
			o.planes.push_back(plane);
		}
	}

	// A regular octagonal prism (eight side planes + two caps) around (cx, cy).
	void add_prism8(int cobj, int32_t type, int32_t flags, double cx, double cy, double radius, double z0, double z1) {
		CollisionObject &o = collision[cobj];
		opennova::threedi::ThreediBoundingVolume v{};
		v.collidable_type = type;
		v.flags = flags;
		v.min_x_fp16 = q16(cx - radius);
		v.min_y_fp16 = q16(cy - radius);
		v.min_z_fp16 = q16(z0);
		v.max_x_fp16 = q16(cx + radius);
		v.max_y_fp16 = q16(cy + radius);
		v.max_z_fp16 = q16(z1);
		v.plane_count = 10;
		o.volumes.push_back(v);
		for (int s = 0; s < 8; ++s) {
			const double angle = s * 3.14159265358979323846 / 4.0;
			const double nx = q14f(std::cos(angle)), ny = q14f(std::sin(angle));
			opennova::threedi::ThreediBoundingPlane plane{};
			plane.normal[0] = static_cast<float>(nx);
			plane.normal[1] = static_cast<float>(ny);
			plane.normal[2] = 0.0f;
			plane.radius = q16f(-(nx * cx + ny * cy + radius));
			o.planes.push_back(plane);
		}
		opennova::threedi::ThreediBoundingPlane top{};
		top.normal[2] = 1.0f;
		top.radius = q16f(-z1);
		o.planes.push_back(top);
		opennova::threedi::ThreediBoundingPlane bottom{};
		bottom.normal[2] = -1.0f;
		bottom.radius = q16f(z0);
		o.planes.push_back(bottom);
	}

	// One collision face over three of the object's local vertices.
	void add_face(int cobj, uint16_t a, uint16_t b, uint16_t c, uint8_t poly_type = 1, uint32_t material_flags = 0) {
		CollisionObject &o = collision[cobj];
		const opennova::threedi::ThreediCollisionVertex &va = o.vertices[a];
		const opennova::threedi::ThreediCollisionVertex &vb = o.vertices[b];
		const opennova::threedi::ThreediCollisionVertex &vc = o.vertices[c];
		const double ex = vb.position[0] - va.position[0], ey = vb.position[1] - va.position[1], ez = vb.position[2] - va.position[2];
		const double fx = vc.position[0] - va.position[0], fy = vc.position[1] - va.position[1], fz = vc.position[2] - va.position[2];
		// The retail corpus winds collision faces clockwise about their normal
		// (mission axes); the face normal is the reversed cross product.
		double nx = -(ey * fz - ez * fy), ny = -(ez * fx - ex * fz), nz = -(ex * fy - ey * fx);
		const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
		nx /= len;
		ny /= len;
		nz /= len;
		opennova::threedi::ThreediCollisionNormal normal{};
		normal.normal[0] = q14f(nx);
		normal.normal[1] = q14f(ny);
		normal.normal[2] = q14f(nz);
		const double ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
		normal.dominate_axis = (az >= ax && az >= ay) ? 1 : (ay >= ax ? 2 : 4);
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
		opennova::threedi::ThreediCollisionFace face{};
		face.vert_index[0] = static_cast<int16_t>(a);
		face.vert_index[1] = static_cast<int16_t>(b);
		face.vert_index[2] = static_cast<int16_t>(c);
		face.normal_index = normal_index;
		face.plane_dist_fp16 = q16(normal.normal[0] * va.position[0] + normal.normal[1] * va.position[1] + normal.normal[2] * va.position[2]);
		double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
		for (const opennova::threedi::ThreediCollisionVertex *v : {&va, &vb, &vc}) {
			for (int k = 0; k < 3; ++k) {
				mn[k] = std::min<double>(mn[k], v->position[k]);
				mx[k] = std::max<double>(mx[k], v->position[k]);
			}
		}
		face.min_x_fp16 = q16(mn[0]);
		face.min_y_fp16 = q16(mn[1]);
		face.min_z_fp16 = q16(mn[2]);
		face.max_x_fp16 = q16(mx[0]);
		face.max_y_fp16 = q16(mx[1]);
		face.max_z_fp16 = q16(mx[2]);
		face.material_flags = material_flags;
		face.poly_type = poly_type;
		o.faces.push_back(face);
	}

	uint16_t add_collision_vertex(int cobj, Vec3 p) {
		opennova::threedi::ThreediCollisionVertex v{};
		v.position[0] = q8f(p.x);
		v.position[1] = q8f(p.y);
		v.position[2] = q8f(p.z);
		collision[cobj].vertices.push_back(v);
		return static_cast<uint16_t>(collision[cobj].vertices.size() - 1);
	}

	// A closed axis box as twelve collision faces (eight shared vertices).
	void add_face_box(int cobj, const Box &box, uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[8];
		for (int i = 0; i < 8; ++i) {
			c[i] = add_collision_vertex(cobj, Vec3{(i & 1) ? box.max.x : box.min.x,
					(i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z});
		}
		// Each side listed counter-clockwise from outside (mission axes); the
		// face helper reverses that into the retail clockwise winding.
		static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		for (const int *q : kSides) {
			add_face(cobj, c[q[0]], c[q[2]], c[q[1]], poly_type, material_flags);
			add_face(cobj, c[q[0]], c[q[3]], c[q[2]], poly_type, material_flags);
		}
	}

	// A single quad (two faces) over four mission-axes corners.
	void add_face_quad(int cobj, const Vec3 corners[4], uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[4];
		for (int i = 0; i < 4; ++i) c[i] = add_collision_vertex(cobj, corners[i]);
		add_face(cobj, c[0], c[2], c[1], poly_type, material_flags);
		add_face(cobj, c[0], c[3], c[2], poly_type, material_flags);
	}

	// --- occlusion (authored in mission axes, stored in model axes) ---------
	static uint16_t occ_edge(int a, int b) {
		const int lo = a < b ? a : b;
		const int hi = a < b ? b : a;
		return static_cast<uint16_t>((lo & 0xFF) | ((hi & 0x7F) << 8) | (a > b ? 0x8000 : 0));
	}

	static opennova::threedi::ThreediOcclusionFace occ_face(int v0, int v1, int v2, int plane) {
		opennova::threedi::ThreediOcclusionFace f{};
		f.raw_indices = static_cast<uint32_t>(v0 & 0xFF) | (static_cast<uint32_t>(v1 & 0xFF) << 8) |
				(static_cast<uint32_t>(v2 & 0xFF) << 16) | (static_cast<uint32_t>(plane & 0xFF) << 24);
		f.edge_data = static_cast<uint32_t>(occ_edge(v0, v1)) | (static_cast<uint32_t>(occ_edge(v1, v2)) << 16);
		f.other_edge_data = occ_edge(v2, v0);
		return f;
	}

	static opennova::threedi::ThreediOcclusionVertex occ_vertex(Vec3 mission) {
		const Vec3 m = to_model(mission);
		opennova::threedi::ThreediOcclusionVertex v{};
		v.position[0] = static_cast<float>(m.x);
		v.position[1] = static_cast<float>(m.y);
		v.position[2] = static_cast<float>(m.z);
		return v;
	}

	static opennova::threedi::ThreediOcclusionPlane occ_plane(Vec3 mission_normal, const opennova::threedi::ThreediOcclusionVertex &on_plane) {
		const Vec3 n = to_model(mission_normal);
		opennova::threedi::ThreediOcclusionPlane p{};
		p.normal[0] = static_cast<float>(n.x);
		p.normal[1] = static_cast<float>(n.y);
		p.normal[2] = static_cast<float>(n.z);
		p.radius = -static_cast<float>(n.x * on_plane.position[0] + n.y * on_plane.position[1] + n.z * on_plane.position[2]);
		return p;
	}

	void finish_occlusion_record(OcclusionRecord &rec, uint8_t type, int section_a, int section_b) {
		rec.object.type = type;
		rec.object.parent_subobject_index = static_cast<uint8_t>(section_a);
		rec.object.connecting_subobject = static_cast<uint8_t>(section_b);
		float center[3] = {0, 0, 0};
		for (const opennova::threedi::ThreediOcclusionVertex &v : rec.vertices)
			for (int k = 0; k < 3; ++k) center[k] += v.position[k] / static_cast<float>(rec.vertices.size());
		float radius = 0.0f;
		for (const opennova::threedi::ThreediOcclusionVertex &v : rec.vertices) {
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

	// A quad portal/window record: four corners wound consistently, one plane
	// whose normal points from section_a toward section_b.
	void add_occ_quad(uint8_t type, int section_a, int section_b, const Vec3 corners[4], Vec3 normal) {
		OcclusionRecord rec;
		for (int i = 0; i < 4; ++i) rec.vertices.push_back(occ_vertex(corners[i]));
		rec.planes.push_back(occ_plane(normal, rec.vertices[0]));
		rec.faces.push_back(occ_face(0, 1, 2, 0));
		rec.faces.push_back(occ_face(0, 2, 3, 0));
		finish_occlusion_record(rec, type, section_a, section_b);
	}

	// A closed axis-box occluder/open record: 8 vertices, 6 outward planes,
	// 12 faces (each side counter-clockwise from outside, mission axes).
	void add_occ_box(uint8_t type, int section_a, int section_b, const Box &box) {
		OcclusionRecord rec;
		for (int i = 0; i < 8; ++i) {
			rec.vertices.push_back(occ_vertex(Vec3{(i & 1) ? box.max.x : box.min.x,
					(i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z}));
		}
		const double n[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
		static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		for (int p = 0; p < 6; ++p)
			rec.planes.push_back(occ_plane(Vec3{n[p][0], n[p][1], n[p][2]}, rec.vertices[kSides[p][0]]));
		for (int s = 0; s < 6; ++s) {
			const int *q = kSides[s];
			rec.faces.push_back(occ_face(q[0], q[1], q[2], s));
			rec.faces.push_back(occ_face(q[0], q[2], q[3], s));
		}
		finish_occlusion_record(rec, type, section_a, section_b);
	}
};

// ---------------------------------------------------------------------------
// Assembly: the contiguous Threedi3di3 the writer serializes. Owns every
// array the struct points at.
struct Assembled {
	opennova::threedi::Threedi3di3 model{};
	std::vector<opennova::threedi::ThreediLod> lods;
	std::vector<std::vector<opennova::threedi::ThreediVertex>> lod_vertices;
	std::vector<std::vector<uint16_t>> lod_indices;
	std::vector<std::vector<opennova::threedi::ThreediTriangleStrip>> lod_strips;
	std::vector<std::vector<opennova::threedi::ThreediRenderObject>> lod_parts;
	std::vector<std::vector<opennova::threedi::ThreediPartAnimation>> lod_panm;
	std::vector<opennova::threedi::ThreediMaterial> materials;
	std::vector<opennova::threedi::ThreediLight> lights;
	std::vector<opennova::threedi::ThreediUserPoint> user_points;
	std::vector<opennova::threedi::ThreediControlRegister> registers;
	std::vector<opennova::threedi::ThreediMatrix4x4> matrices;
	opennova::threedi::ThreediCollisionModel collision{};
	std::vector<opennova::threedi::ThreediBoundingPlane> planes;
	std::vector<opennova::threedi::ThreediBoundingVolume> volumes;
	std::vector<opennova::threedi::ThreediCollisionVertex> vertices;
	std::vector<opennova::threedi::ThreediCollisionNormal> normals;
	std::vector<opennova::threedi::ThreediCollisionFace> faces;
	std::vector<opennova::threedi::ThreediCollisionObject> objects;
	std::vector<opennova::threedi::ThreediCollisionTranslation> translations;
	std::vector<opennova::threedi::ThreediOcclusionVertex> occ_vertices;
	std::vector<opennova::threedi::ThreediOcclusionPlane> occ_planes;
	std::vector<opennova::threedi::ThreediOcclusionFace> occ_faces;
	std::vector<opennova::threedi::ThreediOcclusionObject> occ_objects;
};

inline void assemble(const Model &m, Assembled &out) {
	out = Assembled{};
	opennova::threedi::Threedi3di3 &model = out.model;
	model.version = 0;
	model.header.has_header = 1;
	std::snprintf(model.header.name, sizeof(model.header.name), "%s", m.name.c_str());
	model.header.mesh_type = m.skinned ? opennova::threedi::THREEDI_MESH_SKINNED : opennova::threedi::THREEDI_MESH_BASIC;
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
		const Lod &src = m.lods[li];
		opennova::threedi::ThreediLod &lod = out.lods[li];
		std::memset(&lod, 0, sizeof(lod));
		std::snprintf(lod.model_type, sizeof(lod.model_type), "%s", src.type.c_str());
		lod.lod_threshold = src.threshold;
		lod.rmdl_render_object_count = static_cast<int32_t>(src.parts.size());
		std::vector<opennova::threedi::ThreediVertex> &verts = out.lod_vertices[li];
		std::vector<uint16_t> &indices = out.lod_indices[li];
		std::vector<opennova::threedi::ThreediTriangleStrip> &strips = out.lod_strips[li];
		std::vector<opennova::threedi::ThreediRenderObject> &parts = out.lod_parts[li];
		for (size_t pi = 0; pi < src.parts.size(); ++pi) {
			const Part &part = src.parts[pi];
			opennova::threedi::ThreediRenderObject ro{};
			ro.parent_index = static_cast<int32_t>(part.parent);
			const Vec3 abs = to_model(part.pivot);
			const Vec3 parent_pivot = part.parent == static_cast<int>(pi) ? Vec3{} : to_model(src.parts[part.parent].pivot);
			ro.abs[0] = static_cast<float>(abs.x);
			ro.abs[1] = static_cast<float>(abs.y);
			ro.abs[2] = static_cast<float>(abs.z);
			ro.rel[0] = static_cast<float>(abs.x - parent_pivot.x);
			ro.rel[1] = static_cast<float>(abs.y - parent_pivot.y);
			ro.rel[2] = static_cast<float>(abs.z - parent_pivot.z);
			double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
			bool any = false;
			// Opaque strips first, then alpha strips (the renderer's walk).
			for (int pass = 0; pass < 2; ++pass) {
				for (const Strip &strip : part.strips) {
					if (strip.alpha != (pass == 1)) continue;
					opennova::threedi::ThreediTriangleStrip rec{};
					rec.material_index = strip.material;
					rec.index_offset = static_cast<int32_t>(indices.size());
					rec.num_indices = static_cast<uint16_t>(strip.indices.size());
					rec.num_triangles = static_cast<uint16_t>(strip.indices.size() / 3);
					rec.is_strip = 0;
					rec.start_vertex = static_cast<int32_t>(verts.size());
					rec.num_vertices = static_cast<int32_t>(strip.vertices.size());
					double smn[3] = {1e9, 1e9, 1e9}, smx[3] = {-1e9, -1e9, -1e9};
					for (const opennova::threedi::ThreediVertex &v : strip.vertices) {
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
						rec.bone_table[0] = static_cast<uint8_t>(strip.bone < 0 ? static_cast<int>(pi) : strip.bone);
						rec.bone_table_length = 1;
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
		for (opennova::threedi::ThreediVertex &v : verts) {
			v.flags = m.skinned ? (opennova::threedi::THREEDI_VERTEX_FLAG_SKINNED | 1u) : 1u;
			v.is_skinned = m.skinned ? 1 : 0;
		}
		lod.vertices.count = static_cast<uint32_t>(verts.size());
		lod.vertices.stride = m.skinned ? 56u : 40u;
		lod.vertices.flags = m.skinned ? (opennova::threedi::THREEDI_VERTEX_FLAG_SKINNED | 1u) : 1u;
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
	model.header.max_radius_fp16 = q16(max_radius);

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
		opennova::threedi::ThreediControlRegister r{};
		std::snprintf(r.name, sizeof(r.name), "%s", reg.c_str());
		out.registers.push_back(r);
	}
	model.ctrl.count = static_cast<uint32_t>(out.registers.size());
	model.ctrl.record_size = 24u;
	model.ctrl.registers = out.registers.data();
	for (int i = 0; i < m.matrix_count; ++i) {
		opennova::threedi::ThreediMatrix4x4 identity;
		opennova::threedi::threedi_mat4_identity(&identity);
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
			const CollisionObject &src = m.collision[oi];
			opennova::threedi::ThreediCollisionObject o{};
			o.num_vertices = static_cast<int32_t>(src.vertices.size());
			o.num_faces = static_cast<int32_t>(src.faces.size());
			o.num_normals = static_cast<int32_t>(src.normals.size());
			o.num_bounding_volumes = static_cast<int32_t>(src.volumes.size());
			o.parent_subobject_index = src.parent_part;
			o.offset[0] = q16(src.offset.x);
			o.offset[1] = q16(src.offset.y);
			o.offset[2] = q16(src.offset.z);
			double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
			bool any = false;
			for (const opennova::threedi::ThreediCollisionVertex &v : src.vertices) {
				for (int k = 0; k < 3; ++k) {
					mn[k] = std::min<double>(mn[k], v.position[k]);
					mx[k] = std::max<double>(mx[k], v.position[k]);
				}
				expand(v.position[0], v.position[1], v.position[2]);
				any = true;
			}
			for (const opennova::threedi::ThreediBoundingVolume &v : src.volumes) {
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
				const Vec3 c = src.sphere_center;
				const double r = src.sphere_radius;
				o.min[0] = q16(c.x - r);
				o.min[1] = q16(c.y - r);
				o.min[2] = q16(c.z - r);
				o.max[0] = q16(c.x + r);
				o.max[1] = q16(c.y + r);
				o.max[2] = q16(c.z + r);
				o.med[0] = q16(c.x);
				o.med[1] = q16(c.y);
				o.med[2] = q16(c.z);
				o.radius = q16(r);
				expand(c.x - r, c.y - r, c.z - r);
				expand(c.x + r, c.y + r, c.z + r);
			} else if (any) {
				double r = 0.0;
				for (int k = 0; k < 3; ++k) {
					o.min[k] = q16(mn[k]);
					o.max[k] = q16(mx[k]);
					o.med[k] = q16((mn[k] + mx[k]) * 0.5);
					r += (mx[k] - mn[k]) * (mx[k] - mn[k]) * 0.25;
				}
				o.radius = q16(std::sqrt(r));
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
				opennova::threedi::ThreediCollisionTranslation t{};
				t.translation[0] = o.offset[0];
				t.translation[1] = o.offset[1];
				t.translation[2] = o.offset[2];
				out.translations.push_back(t);
			}
		}
		opennova::threedi::ThreediCollisionModel &col = out.collision;
		if (bounded) {
			for (int k = 0; k < 3; ++k) {
				col.model_data.bbox[k] = q16f(bmn[k]);
				col.model_data.bbox[k + 3] = q16f(bmx[k]);
			}
			double r = 0.0, rxy = 0.0, rz = 0.0;
			for (int i = 0; i < 8; ++i) {
				const double x = (i & 1) ? bmx[0] : bmn[0], y = (i & 2) ? bmx[1] : bmn[1], z = (i & 4) ? bmx[2] : bmn[2];
				r = std::max(r, std::sqrt(x * x + y * y + z * z));
				rxy = std::max(rxy, std::sqrt(x * x + y * y));
				rz = std::max(rz, std::fabs(z));
			}
			col.model_data.radii[0] = q16f(r);
			col.model_data.radii[1] = q16f(rxy);
			col.model_data.radii[2] = q16f(rz);
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
	for (const OcclusionRecord &rec : m.occlusion) {
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

// Serialize through the parity writer (it only writes files) and hand back
// the bytes. Returns false when the writer refused the model.
inline bool mint(const Model &m, const std::string &scratch_path, std::vector<uint8_t> &out) {
	Assembled assembled;
	assemble(m, assembled);
	if (opennova::threedi::threedi_3di3_write(scratch_path.c_str(), &assembled.model) != 0) return false;
	std::ifstream f(scratch_path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff size = f.tellg();
	f.seekg(0);
	out.resize(static_cast<size_t>(size));
	if (!out.empty()) f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	f.close();
	std::remove(scratch_path.c_str());
	return true;
}

} // namespace synth3di

#endif // OPENNOVA_TESTS_MINIMAL_3DI_BUILDER_H
