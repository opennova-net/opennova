// The 3DI3 construction API: authors a Threedi3di3 in memory from small,
// integer-friendly data and hands it to the parity writer
// (threedi_3di3_write / threedi_3di3_write_memory), so every model we ship
// and every fixtures/threedi/synth file is produced by our own writer from
// scratch (ADR 0003) and re-minted byte-for-byte on every platform. This is a
// construction seam, not an intermediate representation: nothing at runtime
// walks a ThreediBuildModel, Threedi3di3 stays the one model every consumer
// reads (ADR 0027). Consumers: tests/fixtures/minimal_3di_gen.cpp (the
// synthetic fixture set and assets/house.3di) and the Godot model exporter.
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
//   - PRESENTATION axes (right-handed, y up: the frame a scene editor shows)
//     = (-x, y, z) of model = (y, z, x) of mission. The helpers below are the
//     one owner of every map and its inverse; a projector and an exporter
//     that both use them cannot drift apart.
// Every float that the writer quantizes (16.16, Q14, Q8, byte colors) is
// stored pre-quantized so a read -> write round trip reproduces the bytes:
// int/65536.0f, int/16384.0f and int/256.0f multiply back exactly in binary
// float, so int -> float -> int is the identity on every platform.
#pragma once

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace opennova::threedi {

struct ThreediBuildVec3 {
	double x = 0.0, y = 0.0, z = 0.0;
};

struct ThreediBuildBox {
	ThreediBuildVec3 min, max;
};

// mission <-> model
inline ThreediBuildVec3 threedi_build_to_model(const ThreediBuildVec3 &m) { return ThreediBuildVec3{-m.y, m.z, m.x}; }
inline ThreediBuildVec3 threedi_build_to_mission(const ThreediBuildVec3 &d) { return ThreediBuildVec3{d.z, -d.x, d.y}; }
// model <-> presentation
inline ThreediBuildVec3 threedi_model_to_presentation(const ThreediBuildVec3 &d) { return ThreediBuildVec3{-d.x, d.y, d.z}; }
inline ThreediBuildVec3 threedi_presentation_to_model(const ThreediBuildVec3 &p) { return ThreediBuildVec3{-p.x, p.y, p.z}; }
// mission <-> presentation
inline ThreediBuildVec3 threedi_mission_to_presentation(const ThreediBuildVec3 &m) { return ThreediBuildVec3{m.y, m.z, m.x}; }
inline ThreediBuildVec3 threedi_presentation_to_mission(const ThreediBuildVec3 &p) { return ThreediBuildVec3{p.z, p.x, p.y}; }
inline int32_t threedi_q16(double v) { return static_cast<int32_t>(std::lround(v * 65536.0)); }
inline float threedi_q16f(double v) { return static_cast<float>(threedi_q16(v)) / 65536.0f; }
inline float threedi_q14f(double v) { return static_cast<float>(std::lround(v * 16384.0)) / 16384.0f; }
inline float threedi_q8f(double v) { return static_cast<float>(std::lround(v * 256.0)) / 256.0f; }
inline float threedi_byte_unit(int c) { return static_cast<float>(c) / 255.0f; }

struct ThreediBuildStrip {
	int material = 0;
	bool alpha = false;
	int bone = -1; // skinned strips: the skeleton bone every vertex rides
	std::vector<ThreediVertex> vertices;
	std::vector<uint16_t> indices;
	// Skinned strips with several bones: the STRP bone table (at most 16
	// parts) the vertices' local bone_indices address; empty means the
	// single-bone form above.
	std::vector<uint8_t> bone_table;
};

struct ThreediBuildPart {
	int parent = 0; // the root references itself
	ThreediBuildVec3 pivot;     // mission axes (ROBJ abs)
	// The parent-relative pivot (ROBJ rel) when the caller carries it as an
	// exact value of its own (a scene node's local origin). Assembly derives
	// abs - parent abs and takes this value only where the two differ as
	// floats (a recipe's double pivots leave a rel the float abs cannot
	// re-derive), so the derived signed-zero convention survives.
	bool has_rel = false;
	ThreediBuildVec3 rel;
	std::vector<ThreediBuildStrip> strips;
};

struct ThreediBuildLod {
	std::string type = "gnrc";
	int32_t threshold = 0;
	std::vector<ThreediBuildPart> parts;
	std::vector<ThreediPartAnimation> panm;
};

struct ThreediBuildCollisionObject {
	int parent_part = 0;
	ThreediBuildVec3 offset; // mission axes
	std::vector<ThreediCollisionVertex> vertices;
	std::vector<ThreediCollisionNormal> normals;
	std::vector<ThreediCollisionFace> faces;
	std::vector<ThreediBoundingVolume> volumes;
	std::vector<ThreediBoundingPlane> planes;
	bool sphere = false; // sphere-only skeletal section (person bones)
	ThreediBuildVec3 sphere_center;
	double sphere_radius = 0.0;
};

struct ThreediBuildOcclusionRecord {
	ThreediOcclusionObject object{};
	std::vector<ThreediOcclusionVertex> vertices;
	std::vector<ThreediOcclusionPlane> planes;
	std::vector<ThreediOcclusionFace> faces;
};

inline ThreediPartAnimation threedi_build_inert_panm(int part, int parent) {
	ThreediPartAnimation row{};
	row.parent_subobject = static_cast<uint8_t>(parent);
	row.subobject_index = static_cast<uint8_t>(part);
	return row;
}

inline ThreediTransform threedi_build_track(uint8_t control, uint8_t param, int16_t rate, int16_t start, int16_t end) {
	ThreediTransform t{};
	t.control = control;
	t.control_param = param;
	t.rate = rate;
	t.start = start;
	t.end = end;
	return t;
}

struct ThreediBuildModel {
	std::string name;
	bool skinned = false;
	bool tangents = false; // VERT carries tangent/bitangent (any material tag with the TANGENT bit)
	std::vector<ThreediBuildLod> lods;
	std::vector<ThreediMaterial> materials;
	std::vector<ThreediLight> lights;
	std::vector<ThreediUserPoint> user_points;
	std::vector<std::string> control_registers;
	std::vector<ThreediBuildCollisionObject> collision;
	std::vector<ThreediBuildOcclusionRecord> occlusion;
	int matrix_count = 1;

	// --- render ------------------------------------------------------------
	int add_lod(int32_t threshold = 0, const char *type = "gnrc") {
		ThreediBuildLod lod;
		lod.threshold = threshold;
		lod.type = type;
		lods.push_back(lod);
		return static_cast<int>(lods.size()) - 1;
	}

	int add_part(int lod, int parent, ThreediBuildVec3 pivot, const ThreediBuildVec3 *rel = nullptr) {
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

	int add_material(const char *shader, const char *texture, uint8_t slot = THREEDI_TEX_SLOT_DIFFUSE) {
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

	void add_detail_texture(int material, const char *texture) {
		ThreediMaterial &m = materials[material];
		ThreediMaterialTexture &t = m.textures[m.texture_count++];
		std::snprintf(t.name, sizeof(t.name), "%s", texture);
		t.slot = THREEDI_TEX_SLOT_DETAIL;
		t.type = THREEDI_TEX_TYPE_DIFFUSE;
	}

	// The RGB generator on a material (styles > 112 read CTRL register `reg`).
	void set_rgb_gen(int material, uint8_t style, int reg, double rate, const int start_rgb[3], const int end_rgb[3]) {
		ThreediRgbGen &g = materials[material].rgb_gen;
		g.style = style;
		g.reg = style > 112 ? reg : -1;
		g.phase = 0.0f;
		g.rate = threedi_q8f(rate);
		for (int k = 0; k < 3; ++k) {
			g.start_color[k] = threedi_byte_unit(start_rgb[k]);
			g.end_color[k] = threedi_byte_unit(end_rgb[k]);
		}
		g.start_color[3] = 0.0f;
		g.end_color[3] = 0.0f;
	}

	// An axis box (mission axes) as one triangle-list strip of 24 vertices;
	// vertices land in ABSOLUTE model coordinates, wound counter-clockwise
	// about each face normal in model space (the retail winding).
	void add_box(int lod, int part, int material, const ThreediBuildBox &box, bool alpha = false, int bone = -1) {
		ThreediBuildStrip strip;
		strip.material = material;
		strip.alpha = alpha;
		strip.bone = bone;
		static const int kFaceNormal[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		for (int f = 0; f < 6; ++f) {
			const int axis = f / 2;
			const bool positive = (f % 2) == 0;
			const int u_axis = (axis + 1) % 3;
			const int v_axis = (axis + 2) % 3;
			ThreediBuildVec3 corners[4];
			const double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (int c = 0; c < 4; ++c) {
				double p[3] = {box.min.x, box.min.y, box.min.z};
				const double hi[3] = {box.max.x, box.max.y, box.max.z};
				p[axis] = positive ? hi[axis] : p[axis];
				p[u_axis] = uv[c][0] > 0.5 ? hi[u_axis] : p[u_axis];
				p[v_axis] = uv[c][1] > 0.5 ? hi[v_axis] : p[v_axis];
				corners[c] = ThreediBuildVec3{p[0], p[1], p[2]};
			}
			const ThreediBuildVec3 n = threedi_build_to_model(ThreediBuildVec3{static_cast<double>(kFaceNormal[f][0]),
					static_cast<double>(kFaceNormal[f][1]), static_cast<double>(kFaceNormal[f][2])});
			const uint16_t base = static_cast<uint16_t>(strip.vertices.size());
			for (int c = 0; c < 4; ++c) {
				const ThreediBuildVec3 m = threedi_build_to_model(corners[c]);
				ThreediVertex v{};
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
			const ThreediBuildVec3 a = threedi_build_to_model(corners[0]), b = threedi_build_to_model(corners[1]), c = threedi_build_to_model(corners[2]);
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

	// An arbitrary triangle list already in MODEL axes: `vertices` carry
	// positions, normals and tangents in the on-disk render frame plus
	// strip-local bone indices and weights; `indices` are strip-relative.
	// `bone_table` (at most 16 parts) resolves those local indices; empty
	// means the single-bone form (`bone`, else the owning part).
	void add_strip(int lod, int part, int material, bool alpha, std::vector<ThreediVertex> vertices,
			std::vector<uint16_t> indices, std::vector<uint8_t> bone_table = {}, int bone = -1) {
		ThreediBuildStrip strip;
		strip.material = material;
		strip.alpha = alpha;
		strip.bone = bone;
		strip.vertices = std::move(vertices);
		strip.indices = std::move(indices);
		strip.bone_table = std::move(bone_table);
		lods[lod].parts[part].strips.push_back(std::move(strip));
	}

	ThreediPartAnimation &add_panm(int lod, int part, int parent, uint32_t flags = 0) {
		ThreediPartAnimation row = threedi_build_inert_panm(part, parent);
		row.flags = flags;
		lods[lod].panm.push_back(row);
		return lods[lod].panm.back();
	}

	// --- user points, lights, registers -------------------------------------
	int add_user_point(const char *point_name, ThreediBuildVec3 pos, ThreediBuildVec3 dir, int subobject, int32_t type) {
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

	int add_light(ThreediBuildVec3 pos, double atten_start, double atten_end, uint8_t style, int subobject,
			const int rgb_start[3], const int rgb_end[3], uint8_t flags = 0, uint8_t phase = 0,
			uint16_t rate = 0) {
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
	int add_cobj(int parent_part, ThreediBuildVec3 offset = ThreediBuildVec3{}) {
		ThreediBuildCollisionObject o;
		o.parent_part = parent_part;
		o.offset = offset;
		collision.push_back(o);
		return static_cast<int>(collision.size()) - 1;
	}

	int add_sphere_cobj(int parent_part, ThreediBuildVec3 offset, ThreediBuildVec3 center, double radius) {
		const int index = add_cobj(parent_part, offset);
		collision[index].sphere = true;
		collision[index].sphere_center = center;
		collision[index].sphere_radius = radius;
		return index;
	}

	// A bounding volume carved by six axis planes (normal . p + radius == 0).
	void add_volume(int cobj, int32_t type, int32_t flags, const ThreediBuildBox &box) {
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

	// A bounding volume over an explicit plane list (mission axes; the caller
	// quantizes normals through threedi_q14f and radii through threedi_q16f).
	void add_volume_planes(int cobj, int32_t type, int32_t flags, const ThreediBuildBox &box,
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
		o.planes.insert(o.planes.end(), volume_planes.begin(), volume_planes.end());
	}

	// A regular octagonal prism (eight side planes + two caps) around (cx, cy).
	void add_prism8(int cobj, int32_t type, int32_t flags, double cx, double cy, double radius, double z0, double z1) {
		ThreediBuildCollisionObject &o = collision[cobj];
		ThreediBoundingVolume v{};
		v.collidable_type = type;
		v.flags = flags;
		v.min_x_fp16 = threedi_q16(cx - radius);
		v.min_y_fp16 = threedi_q16(cy - radius);
		v.min_z_fp16 = threedi_q16(z0);
		v.max_x_fp16 = threedi_q16(cx + radius);
		v.max_y_fp16 = threedi_q16(cy + radius);
		v.max_z_fp16 = threedi_q16(z1);
		v.plane_count = 10;
		o.volumes.push_back(v);
		for (int s = 0; s < 8; ++s) {
			const double angle = s * 3.14159265358979323846 / 4.0;
			const double nx = threedi_q14f(std::cos(angle)), ny = threedi_q14f(std::sin(angle));
			ThreediBoundingPlane plane{};
			plane.normal[0] = static_cast<float>(nx);
			plane.normal[1] = static_cast<float>(ny);
			plane.normal[2] = 0.0f;
			plane.radius = threedi_q16f(-(nx * cx + ny * cy + radius));
			o.planes.push_back(plane);
		}
		ThreediBoundingPlane top{};
		top.normal[2] = 1.0f;
		top.radius = threedi_q16f(-z1);
		o.planes.push_back(top);
		ThreediBoundingPlane bottom{};
		bottom.normal[2] = -1.0f;
		bottom.radius = threedi_q16f(z0);
		o.planes.push_back(bottom);
	}

	// One collision face over three of the object's local vertices.
	void add_face(int cobj, uint16_t a, uint16_t b, uint16_t c, uint8_t poly_type = 1, uint32_t material_flags = 0) {
		ThreediBuildCollisionObject &o = collision[cobj];
		const ThreediCollisionVertex &va = o.vertices[a];
		const ThreediCollisionVertex &vb = o.vertices[b];
		const ThreediCollisionVertex &vc = o.vertices[c];
		const double ex = vb.position[0] - va.position[0], ey = vb.position[1] - va.position[1], ez = vb.position[2] - va.position[2];
		const double fx = vc.position[0] - va.position[0], fy = vc.position[1] - va.position[1], fz = vc.position[2] - va.position[2];
		// The retail corpus winds collision faces clockwise about their normal
		// (mission axes); the face normal is the reversed cross product.
		double nx = -(ey * fz - ez * fy), ny = -(ez * fx - ex * fz), nz = -(ex * fy - ey * fx);
		const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
		nx /= len;
		ny /= len;
		nz /= len;
		ThreediCollisionNormal normal{};
		normal.normal[0] = threedi_q14f(nx);
		normal.normal[1] = threedi_q14f(ny);
		normal.normal[2] = threedi_q14f(nz);
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
		ThreediCollisionFace face{};
		face.vert_index[0] = static_cast<int16_t>(a);
		face.vert_index[1] = static_cast<int16_t>(b);
		face.vert_index[2] = static_cast<int16_t>(c);
		face.normal_index = normal_index;
		face.plane_dist_fp16 = threedi_q16(normal.normal[0] * va.position[0] + normal.normal[1] * va.position[1] + normal.normal[2] * va.position[2]);
		double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
		for (const ThreediCollisionVertex *v : {&va, &vb, &vc}) {
			for (int k = 0; k < 3; ++k) {
				mn[k] = std::min<double>(mn[k], v->position[k]);
				mx[k] = std::max<double>(mx[k], v->position[k]);
			}
		}
		face.min_x_fp16 = threedi_q16(mn[0]);
		face.min_y_fp16 = threedi_q16(mn[1]);
		face.min_z_fp16 = threedi_q16(mn[2]);
		face.max_x_fp16 = threedi_q16(mx[0]);
		face.max_y_fp16 = threedi_q16(mx[1]);
		face.max_z_fp16 = threedi_q16(mx[2]);
		face.material_flags = material_flags;
		face.poly_type = poly_type;
		o.faces.push_back(face);
	}

	uint16_t add_collision_vertex(int cobj, ThreediBuildVec3 p) {
		ThreediCollisionVertex v{};
		v.position[0] = threedi_q8f(p.x);
		v.position[1] = threedi_q8f(p.y);
		v.position[2] = threedi_q8f(p.z);
		collision[cobj].vertices.push_back(v);
		return static_cast<uint16_t>(collision[cobj].vertices.size() - 1);
	}

	// A closed axis box as twelve collision faces (eight shared vertices).
	void add_face_box(int cobj, const ThreediBuildBox &box, uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[8];
		for (int i = 0; i < 8; ++i) {
			c[i] = add_collision_vertex(cobj, ThreediBuildVec3{(i & 1) ? box.max.x : box.min.x,
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
	void add_face_quad(int cobj, const ThreediBuildVec3 corners[4], uint8_t poly_type = 1, uint32_t material_flags = 0) {
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

	static ThreediOcclusionFace occ_face(int v0, int v1, int v2, int plane) {
		ThreediOcclusionFace f{};
		f.raw_indices = static_cast<uint32_t>(v0 & 0xFF) | (static_cast<uint32_t>(v1 & 0xFF) << 8) |
				(static_cast<uint32_t>(v2 & 0xFF) << 16) | (static_cast<uint32_t>(plane & 0xFF) << 24);
		f.edge_data = static_cast<uint32_t>(occ_edge(v0, v1)) | (static_cast<uint32_t>(occ_edge(v1, v2)) << 16);
		f.other_edge_data = occ_edge(v2, v0);
		return f;
	}

	static ThreediOcclusionVertex occ_vertex(ThreediBuildVec3 mission) {
		const ThreediBuildVec3 m = threedi_build_to_model(mission);
		ThreediOcclusionVertex v{};
		v.position[0] = static_cast<float>(m.x);
		v.position[1] = static_cast<float>(m.y);
		v.position[2] = static_cast<float>(m.z);
		return v;
	}

	static ThreediOcclusionPlane occ_plane(ThreediBuildVec3 mission_normal, const ThreediOcclusionVertex &on_plane) {
		const ThreediBuildVec3 n = threedi_build_to_model(mission_normal);
		ThreediOcclusionPlane p{};
		p.normal[0] = static_cast<float>(n.x);
		p.normal[1] = static_cast<float>(n.y);
		p.normal[2] = static_cast<float>(n.z);
		p.radius = -static_cast<float>(n.x * on_plane.position[0] + n.y * on_plane.position[1] + n.z * on_plane.position[2]);
		return p;
	}

	void finish_occlusion_record(ThreediBuildOcclusionRecord &rec, uint8_t type, int section_a, int section_b) {
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

	// A quad portal/window record: four corners wound consistently, one plane
	// whose normal points from section_a toward section_b.
	void add_occ_quad(uint8_t type, int section_a, int section_b, const ThreediBuildVec3 corners[4], ThreediBuildVec3 normal) {
		ThreediBuildOcclusionRecord rec;
		for (int i = 0; i < 4; ++i) rec.vertices.push_back(occ_vertex(corners[i]));
		rec.planes.push_back(occ_plane(normal, rec.vertices[0]));
		rec.faces.push_back(occ_face(0, 1, 2, 0));
		rec.faces.push_back(occ_face(0, 2, 3, 0));
		finish_occlusion_record(rec, type, section_a, section_b);
	}

	// A closed axis-box occluder/open record: 8 vertices, 6 outward planes,
	// 12 faces (each side counter-clockwise from outside, mission axes).
	void add_occ_box(uint8_t type, int section_a, int section_b, const ThreediBuildBox &box) {
		ThreediBuildOcclusionRecord rec;
		for (int i = 0; i < 8; ++i) {
			rec.vertices.push_back(occ_vertex(ThreediBuildVec3{(i & 1) ? box.max.x : box.min.x,
					(i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z}));
		}
		const double n[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
		static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		for (int p = 0; p < 6; ++p)
			rec.planes.push_back(occ_plane(ThreediBuildVec3{n[p][0], n[p][1], n[p][2]}, rec.vertices[kSides[p][0]]));
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
struct ThreediAssembled {
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

// Assembly: fill `out` with the contiguous Threedi3di3 the writer serializes.
// `out` owns every array the struct points at; `out.model` is the view.
void threedi_build_assemble(const ThreediBuildModel &m, ThreediAssembled &out);

// Assemble and serialize through the parity writer into `out`. Returns false
// when the writer refused the model.
bool threedi_build_mint(const ThreediBuildModel &m, std::vector<uint8_t> &out);

} // namespace opennova::threedi
