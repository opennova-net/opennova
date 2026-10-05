// The 3DI3 construction API: authors a Threedi3di3 in memory from small,
// integer-friendly data and hands it to the parity writer
// (threedi_3di3_write_memory), so every model we ship is produced by our own
// writer from scratch (ADR 0003) and re-minted byte-for-byte on every
// platform. This is a construction seam, not an intermediate representation:
// nothing at runtime walks a ThreediBuildModel, Threedi3di3 stays the one
// model every consumer reads (ADR 0027). Consumers: apps/3di
// (opennova-3di, the Blender exporter's CLI; ADR 0047) and the synthetic
// fixture generator (tests/fixtures/minimal_3di_builder.h).
//
// Frames (docs/threedi/3di-gp-format-re.md):
//   - MISSION axes (x forward, y left, z up) are the authoring frame here and
//     the on-disk frame of the collision block (CVRT/CNRM/CFAC/BPLN/BVOL/COBJ/
//     CXLT/CMDL) and of the user points (USRP 16.16 ints).
//   - MODEL axes = (-y, z, x) of mission: the on-disk frame of VERT/STRP/ROBJ,
//     the LGHT offsets, the MTRX frames and the OCCL tables. Render vertices,
//     strip bounds and part bound centers are ABSOLUTE model coordinates (the
//     PANM node matrices pivot about ROBJ abs themselves; the retail corpus
//     stores wheels and rockers that way), strip indices are strip-relative.
//   - PRESENTATION axes (right-handed, y up: the frame a scene editor shows)
//     = (y, z, x) of mission. The helpers below are the one owner of every
//     map and its inverse; a projector and an exporter that both use them
//     cannot drift apart.
// Every float that the writer quantizes (16.16, Q14, Q8, byte colors) is
// stored pre-quantized so a read -> write round trip reproduces the bytes:
// int/65536.0f, int/16384.0f and int/256.0f multiply back exactly in binary
// float, so int -> float -> int is the identity on every platform.
#pragma once

#include <base/io/fixed.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include <array>
#include <cmath>
#include <cstdint>
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
// mission <-> presentation
inline ThreediBuildVec3 threedi_mission_to_presentation(const ThreediBuildVec3 &m) { return ThreediBuildVec3{m.y, m.z, m.x}; }
inline ThreediBuildVec3 threedi_presentation_to_mission(const ThreediBuildVec3 &p) { return ThreediBuildVec3{p.z, p.x, p.y}; }
// A PANM rotation frame between mission axes (3x3, row-major, p' = p R) and
// the model-axes MTRX row: M = C^T R C, C the mission -> model map, and back
// R = C M C^T.
ThreediMatrix4x4 threedi_build_frame_to_model(const double mission[9]);
void threedi_build_frame_to_mission(const ThreediMatrix4x4 &frame, double mission[9]);

// The PANM flags word a row's tracks imply (the form `build` writes when no
// word is given): rotation type 2 when any rotation track animates, scale
// type 2 for any scale track, and `trans_axis` when the translation track does.
uint32_t threedi_build_panm_flags(const ThreediPartAnimation &row, uint8_t trans_axis);

// A LGHT colour generator's rate (per second) and phase as WriteLGHT packs
// them: times 256 in float, truncated, the phase wrapped to its byte (styles
// up to 0x70; above, the phase byte is a CTRL register index)
// [orig: WriteLGHT @ 0x456DF0 (ModSuperOed.exe)] (the retired port's
// pack_rate and pack_phase, 5fc5b4f6a^:engine/formats/oed/export_3di.cpp);
// and the values those words hold.
inline uint16_t threedi_build_light_rate(double rate) {
	return static_cast<uint16_t>(static_cast<int32_t>(static_cast<float>(rate) * 256.0f));
}
inline uint8_t threedi_build_light_phase(double phase) {
	return static_cast<uint8_t>(static_cast<int32_t>(static_cast<float>(phase) * 256.0f) & 0xFF);
}
inline double threedi_build_light_rate_value(uint16_t rate) { return rate / 256.0; }
inline double threedi_build_light_phase_value(uint8_t phase) { return phase / 256.0; }

inline int32_t threedi_q16(double v) { return static_cast<int32_t>(std::lround(v * io::kFp16OneD)); }
inline float threedi_q16f(double v) { return static_cast<float>(threedi_q16(v)) / io::kFp16One; }
inline float threedi_q14f(double v) { return static_cast<float>(std::lround(v * io::kFp14One)) / io::kFp14One; }
inline float threedi_q8f(double v) { return static_cast<float>(std::lround(v * 256.0)) / 256.0f; }
inline float threedi_byte_unit(int c) { return static_cast<float>(c) / 255.0f; }
// A colour channel (0..1) as the byte it was authored as: threedi_byte_unit's
// inverse.
inline int threedi_build_byte_of(float unit) { return static_cast<int>(std::lround(unit * 255.0f)); }
// The quantizers of the collision fields the retired OED writer derives: it
// truncates toward zero where the helpers above round (CVRT 8.8, BPLN Q14,
// and the 16.16 CFAC, BPLN, BVOL, COBJ and CMDL values) [orig: WriteCVRT @
// 0x454450 (ModSuperOed.exe), WriteCFAC @ 0x454830 (ModSuperOed.exe),
// WriteBPLN @ 0x455A80 (ModSuperOed.exe), WriteBVOL @ 0x455CE0
// (ModSuperOed.exe), WriteCOBJ @ 0x454E70 (ModSuperOed.exe), WriteCDTA @
// 0x456050 (ModSuperOed.exe)] (the retired port:
// 5fc5b4f6a^:engine/formats/oed/export_3di.cpp). A value already on the grid
// (a scene of a retail model) quantizes to itself either way.
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
	ThreediBuildVec3 pivot;     // mission axes (ROBJ abs; rel is derived from it)
	// A part without strips: the point its sphere sits on, radius 0. The
	// exporter seeds such a part with one placeholder vertex, its `_## center`
	// helper's first mesh vertex in the retail corpus (near the pivot; 1,779
	// of 2,411 such JO parts also carry it, on the 8.8 grid, as their
	// section's only collision vertex) [5fc5b4f6a^:engine/formats/oed/
	// convert_internal.cpp, the placeholder injection]. Without it the
	// sphere is (0, 0, 0), as the retail parts no helper mesh seeded (658).
	bool has_center = false;
	ThreediBuildVec3 center;
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
	// The authored positions (floats, before the 8.8 grid) face normals are
	// taken from, so a face the grid collapses keeps its normal.
	std::vector<ThreediBuildVec3> exact;
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

// An occlusion record's sphere given as stored (mission axes), for a record
// whose sphere is not the one threedi_build_occ_sphere derives: 206 JOTAC
// models store each record's centre mirrored across y from its vertices'
// centre, the radius still the farthest vertex from the true one (Crdrblk2,
// DRGVLA; 182 others store the centre itself).
struct ThreediBuildOccSphere {
	ThreediBuildVec3 centre;
	double radius = 0.0;
};

// The sphere the builder gives an occlusion record over its vertices (model
// axes, as OOBJ stores them): the vertex mean and the farthest vertex from
// it, as OED took them. It is the sphere the runtime needs: the portal-slot
// collector carries the stored centre through the entity pose into render
// space exactly as the occluder build carries the record's vertices, and
// tests that sphere against the view and its angular size [orig:
// Terrain_CollectVisibleSectorUserpoints @ 0x5c6b60 (@0x5c6df8), the centre through
// Math_TransformPointByMatrix4x4; the clip at radius x 65536 @ 0x5c6e42; the
// radius^2 / distance^2 > 0.01 gate @ 0x5c6e48..0x5c6e88;
// Terrain_BuildClipPlanesFromCollision @ 0x5b34e0 (@0x5b3595), the vertices through the same
// pose]. No vertex: a NaN centre and radius 0.
void threedi_build_occ_sphere(const ThreediOcclusionVertex *vertices, size_t count, float center[3], float &radius);

ThreediPartAnimation threedi_build_inert_panm(int part, int parent);

// A part's ROBJ sphere over the render vertices authored on it: the vertex
// box's centre and the farthest vertex from it, the distance taken wide and
// stored as a float (the retired port's WriteRDTA takes the farthest vertex
// too; 5fc5b4f6a^:engine/formats/oed/rdta.cpp). Over the vertices each part's
// triangles use (retail pools one vertex window across strips in 70 models),
// that reproduces 5,239 of the 5,933 rigid JO parts and 244 of the 256
// skinned mesh parts the corpus can attribute; the box's half-diagonal
// reproduces 704 and none. No vertex: a zero sphere at the origin.
void threedi_build_part_sphere(const std::vector<const ThreediVertex *> &vertices, float center[3], float &radius);

// A LGHT record's view_proj from its offset, rotation (the light's Z axis in
// model axes), atten_end and the cone half-angle `falloff` in degrees: a view
// looking along the axis times a perspective of fov 2 * falloff, near 0.1, far
// atten_end. An omni light (falloff 0) yields the NaN columns retail ships
// (Armry01's LGHT); the JO runtime never reads it.
void threedi_build_light_view_proj(ThreediLight &light, float falloff);

// The cosine a LGHT record stores for the cone half-angle `falloff` (degrees),
// through the D3DX degree constant the exporter converts with (the retired
// port's deg_to_rad).
float threedi_build_light_cone_cos(float falloff);

// A spot light's cone half-angle (degrees) as the float build re-derives the
// record from: its byte (wrapped), the cosine and the view_proj all follow from
// it. The float nearest the angle the cosine holds rarely gives back the same
// cosine, and a small cone leaves thousands of floats with one cosine, so the
// floats around it are tried for one that reproduces the byte, the cosine and
// the view_proj, then the byte and the cosine (a retail record whose view_proj
// another tool built), else the angle itself.
float threedi_build_light_cone_half_angle(const ThreediLight &light);

// A material's glass, reflection and emissive words from its shader's
// capabilities, by the OED rule WriteMTRL applies (it holds for every material
// of the 958 JO models; 5fc5b4f6a^:engine/formats/oed/export_3di.cpp): a GLASS
// shader reflects 0x80 grey unless another colour is set, and is glass while it
// reflects; an EMISSIVE one is emissive type 2; any other is neither. The
// shader table is the renderer's, so the caller says which the shader is.
void threedi_build_material_surface(ThreediMaterial &material, bool glass_shader, bool emissive_shader);

ThreediTransform threedi_build_track(uint8_t control, uint8_t param, int16_t rate, int16_t start, int16_t end);

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
	// CXLT (mission axes): the rows WriteCXLT writes, the collision LOD's
	// attach points in order, truncated to 16.16 [orig: WriteCXLT @ 0x455920
	// (ModSuperOed.exe)] (the retired port:
	// 5fc5b4f6a^:engine/formats/oed/export_3di.cpp). The runtime reads them
	// by row (a palm item's broken pieces pivot on rows 0 and 1). When
	// `translations_given` is false the builder derives the table by our own
	// rule, not retail's: one row per non-root section on a rigid model and one
	// per section on a skinned one, at the section's offset (the retail count
	// in 917 of 958 JO models; the rows themselves are the author's helpers).
	bool translations_given = false;
	std::vector<ThreediBuildVec3> translations;
	std::vector<ThreediBuildOcclusionRecord> occlusion;
	// MTRX rows after the identity row 0: the rotation frames a PANM row
	// selects with matrix_index > 0 (model axes, row-major, p' = p * M; a
	// tilted tail rotor spins about its frame's axes).
	std::vector<ThreediMatrix4x4> frames;

	// --- render ------------------------------------------------------------
	int add_lod(int32_t threshold = 0, const char *type = "gnrc");
	int add_part(int lod, int parent, ThreediBuildVec3 pivot);
	int add_material(const char *shader, const char *texture, uint8_t slot = THREEDI_TEX_SLOT_DIFFUSE);
	// The RGB generator on a material (styles > 112 read CTRL register `reg`).
	void set_rgb_gen(int material, uint8_t style, int reg, double rate, const int start_rgb[3], const int end_rgb[3]);
	ThreediPartAnimation &add_panm(int lod, int part, int parent, uint32_t flags = 0);

	// --- user points, lights, registers -------------------------------------
	int add_user_point(const char *point_name, ThreediBuildVec3 pos, ThreediBuildVec3 dir, int subobject, int32_t type);
	// A LGHT record. `dir` is the light's local Z axis in mission axes (an
	// omni light keeps the retail default, straight down: Armry01's lights)
	// and `falloff` the cone half-angle in degrees (0 for an omni light).
	int add_light(ThreediBuildVec3 pos, double atten_start, double atten_end, uint8_t style, int subobject,
			const int rgb_start[3], const int rgb_end[3], uint8_t flags = 0, uint8_t phase = 0,
			uint16_t rate = 0, ThreediBuildVec3 dir = ThreediBuildVec3{0.0, 0.0, -1.0}, double falloff = 0.0);
	int add_control_register(const char *register_name);

	// --- collision (mission axes) --------------------------------------------
	// Section i pairs with render part i by ordinal; `parent_part` is retail's
	// COBJ +20 word, the PARENT of that part (the root names itself).
	int add_cobj(int parent_part, ThreediBuildVec3 offset = ThreediBuildVec3{});
	// A bounding volume carved by six axis planes (normal . p + radius == 0).
	void add_volume(int cobj, int32_t type, int32_t flags, const ThreediBuildBox &box);
	// A bounding volume over an explicit plane list (mission axes; the caller
	// quantizes normals through threedi_q14f and radii through threedi_q16f).
	void add_volume_planes(int cobj, int32_t type, int32_t flags, const ThreediBuildBox &box,
			const std::vector<ThreediBoundingPlane> &volume_planes);
	// A bounding volume from an authored triangle mesh (mission axes; each
	// triangle wound counter-clockwise about its outward normal) by the OED
	// rule [orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe), its -colonly
	// branch] (the retired port:
	// 5fc5b4f6a^:engine/formats/oed/convert_internal.cpp): the vertex box's
	// six planes (+x -x +y -y +z -z), then each triangle's own plane unless one
	// already matches it (normal within 0.005 per axis, distance within 0.03;
	// the last match wins). A triangle whose edge cross product is at most
	// 0.0001 long takes plane 0. A ladder (CL, type 4) then swaps plane 0 with
	// the plane the last triangle took: the runtime reads plane 0 as the
	// ladder's facing. The planes and box are stored as WriteBPLN and
	// WriteBVOL truncate them; seam flags come from the assembly's seam pass.
	// ModSuperOed stops at 32 planes, but the retail corpus ships volumes of
	// up to 61, so every plane is kept. The arithmetic is float, as OED's.
	// Returns how far the authored vertices reach outside the solid the
	// planes bound (a non-convex mesh loses the rest).
	double add_volume_mesh(int cobj, int32_t type, int32_t flags, const std::vector<ThreediBuildVec3> &verts,
			const std::vector<std::array<int, 3>> &tris);
	// One collision face over three of the object's local vertices, wound
	// counter-clockwise about its normal in mission axes: the retail corpus
	// order (Dtruck2 905 of 906 faces, Armry01 250 of 250). The normal is
	// taken from the unquantized positions, so a face the 8.8 grid collapses
	// keeps the normal it was authored with, as retail's do (Mp5b_1st carries
	// 276 such faces); `given` (mission axes) overrides it. False (and no
	// face) only when the authored corners are collinear too.
	bool add_face(int cobj, uint16_t a, uint16_t b, uint16_t c, uint8_t poly_type = 1, uint32_t material_flags = 0,
			const ThreediBuildVec3 *given = nullptr);
	// A collision vertex on the 8.8 grid, truncated as WriteCVRT stores it
	// [orig: WriteCVRT @ 0x454450 (ModSuperOed.exe)].
	uint16_t add_collision_vertex(int cobj, ThreediBuildVec3 p);

	// --- occlusion (authored in mission axes, stored in model axes) ---------
	// An occlusion record over an authored mesh (mission axes): `type` is the
	// OCCL record type (0 occluder, 1 open, 2 window, 3 portal, 4 OH),
	// `section_a` the parent section, `section_b` the connecting one. Faces
	// keep their corner order (counter-clockwise about the outward normal in
	// mission axes, as retail stores them) and name a plane, or -1 to let the
	// OED rule pick one: the six bounding-box planes first (+x -x +y -y +z -z),
	// then each face's own plane unless one already matches it (normal within
	// 0.005 per axis and distance within 0.03; the LAST match wins), at most 32
	// planes [orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe), the
	// collision/occlusion plane table; witnessed on Armry01's OCCL]. `planes`
	// given explicitly (mission axes, n . p + d == 0) replace the rule. The
	// record's sphere is threedi_build_occ_sphere's unless `sphere` gives the
	// one it stores. False when the rule overflows 32.
	bool add_occ_record(uint8_t type, int section_a, int section_b, const std::vector<ThreediBuildVec3> &verts,
			const std::vector<std::array<int, 4>> &faces, const std::vector<std::array<double, 4>> &explicit_planes = {},
			const ThreediBuildOccSphere *sphere = nullptr);
};

// Assemble the contiguous Threedi3di3 and serialize it through the parity
// writer into `out`. Returns false when the writer refused the model;
// `overflow` then names a chunk too large for its length field, when that
// is why (threedi_3di3_write_memory).
bool threedi_build_mint(const ThreediBuildModel &m, std::vector<uint8_t> &out,
		ThreediChunkOverflow *overflow = nullptr);

} // namespace opennova::threedi
