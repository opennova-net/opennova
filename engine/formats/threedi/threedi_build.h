// The 3DI3 construction API: authors a Threedi3di3 in memory from small,
// integer-friendly data and hands it to the parity writer
// (threedi_3di3_write / threedi_3di3_write_memory), so every model we ship
// is produced by our own writer from scratch (ADR 0003) and re-minted
// byte-for-byte on every platform. This is a construction seam, not an
// intermediate representation: nothing at runtime walks a ThreediBuildModel,
// Threedi3di3 stays the one model every consumer reads (ADR 0027). Consumer:
// apps/threedi_cli (opennova-3di, the Blender exporter's CLI; ADR 0047).
//
// Frames (docs/threedi/3di-gp-format-re.md):
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

#include <base/io/fixed.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include <algorithm>
#include <array>
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
inline int32_t threedi_q16(double v) { return static_cast<int32_t>(std::lround(v * io::kFp16OneD)); }
inline float threedi_q16f(double v) { return static_cast<float>(threedi_q16(v)) / io::kFp16One; }
inline float threedi_q14f(double v) { return static_cast<float>(std::lround(v * io::kFp14One)) / io::kFp14One; }
inline float threedi_q8f(double v) { return static_cast<float>(std::lround(v * 256.0)) / 256.0f; }
inline float threedi_byte_unit(int c) { return static_cast<float>(c) / 255.0f; }
// The quantizers of the collision fields the retired OED writer derives: it
// truncates toward zero where the helpers above round (CVRT 8.8, BPLN Q14,
// and the 16.16 CFAC, BPLN, BVOL, COBJ and CMDL values) [orig: WriteCVRT @
// 0x454450, WriteCFAC @ 0x454830, WriteBPLN @ 0x455A80, WriteBVOL @ 0x455CE0,
// WriteCOBJ @ 0x454E70, WriteCDTA @ 0x456050; 5fc5b4f6a^:engine/formats/oed/
// export_3di.cpp]. A value already on the grid (a scene of a retail model)
// quantizes to itself either way.
inline int32_t threedi_q16_trunc(double v) { return static_cast<int32_t>(v * io::kFp16OneD); }
inline float threedi_q8f_trunc(double v) {
	return static_cast<float>(static_cast<int16_t>(static_cast<float>(v) * 256.0f)) / 256.0f;
}
inline float threedi_q14f_trunc(float v) {
	return static_cast<float>(static_cast<int16_t>(static_cast<int32_t>(v * io::kFp14One))) / io::kFp14One;
}

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

// What the seam pass needs of one volume (parallel to a section's volumes):
// its unquantized box, and for a volume built from authored triangles
// (add_volume_mesh) each triangle's box and the plane it took.
struct ThreediBuildVolumeSource {
	float box[6] = {}; // min x y z, max x y z
	bool meshed = false;
	std::vector<std::array<float, 6>> face_boxes;
	std::vector<int> face_planes;
};

struct ThreediBuildCollisionObject {
	int parent_part = 0;
	ThreediBuildVec3 offset; // mission axes
	std::vector<ThreediCollisionVertex> vertices;
	std::vector<ThreediBuildVec3> exact; // the unquantized positions face normals are taken from
	std::vector<ThreediCollisionNormal> normals;
	std::vector<ThreediCollisionFace> faces;
	std::vector<ThreediBoundingVolume> volumes;
	std::vector<ThreediBuildVolumeSource> volume_sources; // parallel to volumes
	std::vector<ThreediBoundingPlane> planes;
	bool sphere = false; // sphere-only skeletal section (person bones)
	ThreediBuildVec3 sphere_center;
	double sphere_radius = 0.0;
	// The bounds of the vertices the bone moves, when given: WriteCOBJ stores
	// them (and their midpoint) rather than the sphere's cube.
	bool sphere_bounded = false;
	ThreediBuildBox sphere_bounds;
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

// A LGHT record's view_proj from its offset, rotation (the light's Z axis in
// model axes), atten_end and the cone half-angle `falloff` in degrees: a view
// looking along the axis times a perspective of fov 2 * falloff, near 0.1, far
// atten_end. An omni light (falloff 0) yields the NaN columns retail ships
// (Armry01's LGHT); the JO runtime never reads it.
void threedi_build_light_view_proj(ThreediLight &light, float falloff);

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
	// MTRX rows after the identity row 0: the rotation frames a PANM row
	// selects with matrix_index > 0 (model axes, row-major, p' = p * M; a
	// tilted tail rotor spins about its frame's axes).
	std::vector<ThreediMatrix4x4> frames;

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

	// A LGHT record. `dir` is the light's local Z axis in mission axes (an
	// omni light keeps the retail default, straight down: Armry01's lights)
	// and `falloff` the cone half-angle in degrees (0 for an omni light).
	int add_light(ThreediBuildVec3 pos, double atten_start, double atten_end, uint8_t style, int subobject,
			const int rgb_start[3], const int rgb_end[3], uint8_t flags = 0, uint8_t phase = 0,
			uint16_t rate = 0, ThreediBuildVec3 dir = ThreediBuildVec3{0.0, 0.0, -1.0}, double falloff = 0.0) {
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
		o.volume_sources.push_back(volume_source(box));
		o.planes.insert(o.planes.end(), volume_planes.begin(), volume_planes.end());
	}

	// A bounding volume from an authored triangle mesh (mission axes; each
	// triangle wound counter-clockwise about its outward normal) by the OED
	// rule [orig: ConvertToInternal @ 0x4268B3, its -colonly branch;
	// 5fc5b4f6a^:engine/formats/oed/convert_internal.cpp]: the vertex box's six
	// planes (+x -x +y -y +z -z), then each triangle's own plane unless one
	// already matches it (normal within 0.005 per axis, distance within 0.03;
	// the last match wins). A triangle whose edge cross product is at most
	// 0.0001 long takes plane 0. A ladder (CL, type 4) then swaps plane 0 with
	// the plane the last triangle took: the runtime reads plane 0 as the
	// ladder's facing. The planes and box are stored as WriteBPLN and
	// WriteBVOL truncate them; seam flags come from the assembly's seam pass.
	// ModSuperOed stops at 32 planes, but the retail corpus ships volumes of
	// up to 61, so every plane is kept. The arithmetic is float, as OED's.
	double add_volume_mesh(int cobj, int32_t type, int32_t flags, const std::vector<ThreediBuildVec3> &verts,
			const std::vector<std::array<int, 3>> &tris) {
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

	static ThreediBuildVolumeSource volume_source(const ThreediBuildBox &box) {
		ThreediBuildVolumeSource s;
		const double b[6] = {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
		for (int k = 0; k < 6; ++k) s.box[k] = static_cast<float>(b[k]);
		return s;
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
		o.volume_sources.push_back(volume_source(ThreediBuildBox{{cx - radius, cy - radius, z0}, {cx + radius, cy + radius, z1}}));
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

	// One collision face over three of the object's local vertices, wound
	// counter-clockwise about its normal in mission axes: the retail corpus
	// order (Dtruck2 905 of 906 faces, Armry01 250 of 250). The normal is
	// taken from the unquantized positions, so a face the 8.8 grid collapses
	// keeps the normal it was authored with, as retail's do (Mp5b_1st carries
	// 276 such faces); `given` (mission axes) overrides it. False (and no
	// face) only when the authored corners are collinear too.
	bool add_face(int cobj, uint16_t a, uint16_t b, uint16_t c, uint8_t poly_type = 1, uint32_t material_flags = 0,
			const ThreediBuildVec3 *given = nullptr) {
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

	// A collision vertex on the 8.8 grid, truncated as WriteCVRT stores it
	// [orig: WriteCVRT @ 0x454450].
	uint16_t add_collision_vertex(int cobj, ThreediBuildVec3 p) {
		ThreediCollisionVertex v{};
		v.position[0] = threedi_q8f_trunc(p.x);
		v.position[1] = threedi_q8f_trunc(p.y);
		v.position[2] = threedi_q8f_trunc(p.z);
		collision[cobj].vertices.push_back(v);
		collision[cobj].exact.push_back(p);
		return static_cast<uint16_t>(collision[cobj].vertices.size() - 1);
	}

	// A closed axis box as twelve collision faces (eight shared vertices).
	void add_face_box(int cobj, const ThreediBuildBox &box, uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[8];
		for (int i = 0; i < 8; ++i) {
			c[i] = add_collision_vertex(cobj, ThreediBuildVec3{(i & 1) ? box.max.x : box.min.x,
					(i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z});
		}
		// Each side listed counter-clockwise from outside (mission axes), the
		// winding retail stores.
		static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		for (const int *q : kSides) {
			add_face(cobj, c[q[0]], c[q[1]], c[q[2]], poly_type, material_flags);
			add_face(cobj, c[q[0]], c[q[2]], c[q[3]], poly_type, material_flags);
		}
	}

	// A single quad (two faces) over four mission-axes corners.
	void add_face_quad(int cobj, const ThreediBuildVec3 corners[4], uint8_t poly_type = 1, uint32_t material_flags = 0) {
		uint16_t c[4];
		for (int i = 0; i < 4; ++i) c[i] = add_collision_vertex(cobj, corners[i]);
		add_face(cobj, c[0], c[1], c[2], poly_type, material_flags);
		add_face(cobj, c[0], c[2], c[3], poly_type, material_flags);
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

	// An occlusion record over an authored mesh (mission axes): `type` is the
	// OCCL record type (0 occluder, 1 open, 2 window, 3 portal, 4 OH),
	// `section_a` the parent section, `section_b` the connecting one. Faces
	// keep their corner order (counter-clockwise about the outward normal in
	// mission axes, as retail stores them) and name a plane, or -1 to let the
	// OED rule pick one: the six bounding-box planes first (+x -x +y -y +z -z),
	// then each face's own plane unless one already matches it (normal within
	// 0.005 per axis and distance within 0.03; the LAST match wins), at most 32
	// planes [orig: ConvertToInternal @ 0x4268B3, the collision/occlusion plane
	// table; witnessed on Armry01's OCCL]. `planes` given explicitly (mission
	// axes, n . p + d == 0) replace the rule. False when the rule overflows 32.
	bool add_occ_record(uint8_t type, int section_a, int section_b, const std::vector<ThreediBuildVec3> &verts,
			const std::vector<std::array<int, 4>> &faces, const std::vector<std::array<double, 4>> &explicit_planes = {}) {
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
		finish_occlusion_record(rec, type, section_a, section_b);
		return true;
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
