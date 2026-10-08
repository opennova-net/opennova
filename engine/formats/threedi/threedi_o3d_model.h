// The `.o3d` scene text as read (threedi_o3d_read.h): every record the text
// holds, in the text's own terms (mission axes, the author's winding, doubles
// as written), each with the line it came from so the lowering can name it.
// Nothing here is a 3DI3 limit: a name holds any number of characters, a mesh
// any number of vertices, triangles and influences, a model any number of
// parts, texture rows and registers. What the game holds is a target's to
// say, and the lowering (threedi_o3d_lower.h) is this struct's one consumer:
// it turns it into the construction API's ThreediBuildModel for a target.
//
// The text's parse, not a model representation: ADR 0027 stands, the parsed
// Threedi3di3 is the one model every runtime consumer walks, and nothing past
// the build ever sees this struct (ADR 0052). Authoring text, not a port.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/threedi/threedi_build.h>

namespace opennova::threedi {

// One influence of a skinned vertex: a part of its LOD (the skeleton is the
// LOD's part list) and its share of the vertex.
struct ThreediO3dInfluence {
	long long part = 0;
	double weight = 0.0;
};

struct ThreediO3dVertex {
	int line = 0;
	double position[3] = {}, normal[3] = {};
	double uv0[2] = {}, uv1[2] = {}; // uv1 is uv0 without `uv1 1`
	// The vertex's influences in the mesh's list (skinned only): the PRIMARY
	// first (the bone the lit skinned shaders light it by), then the rest in
	// the author's order.
	uint32_t first_influence = 0, influence_count = 0;
};

// A tangent frame as `vt` gives it: tangent then bitangent, mission axes, on
// D3D UVs (dP/du and dP/dv, v running down).
using ThreediO3dFrame = std::array<double, 6>;

// A triangle list under one material, any size: the lowering splits it into
// strips the target holds.
struct ThreediO3dMesh {
	int line = 0;
	long long material = 0;
	bool alpha = false;
	std::vector<ThreediO3dVertex> vertices;
	std::vector<ThreediO3dInfluence> influences;
	// One per vertex when the mesh gives them (every vertex or none).
	std::vector<ThreediO3dFrame> frames;
	int frames_line = 0; // the first `vt`
	// Counter-clockwise about the outward normal in mission axes.
	std::vector<std::array<uint32_t, 3>> triangles;
};

struct ThreediO3dPart {
	int line = 0;
	long long parent = 0;
	ThreediBuildVec3 pivot;
	bool has_center = false;
	ThreediBuildVec3 center;
	std::vector<ThreediO3dMesh> meshes;
};

struct ThreediO3dTrack {
	int line = 0;
	int target = 0; // threedi_panm_track_index
	uint8_t style = 0;
	long long param = 0; // a phase byte, or the index of the register it names
	bool names_register = false;
	int16_t rate = 0, start = 0, end = 0;
	int axis = 0; // a trans track's axis as given (0: none given)
};

struct ThreediO3dPanm {
	int line = 0;
	long long part = 0, parent = 0;
	bool flags_given = false;
	uint32_t flags = 0;
	long long matrix = 0;
	std::vector<ThreediO3dTrack> tracks; // in the text's order (a later one replaces its target)
};

struct ThreediO3dLod {
	int line = 0;
	int32_t threshold = 0;
	std::string type = "gnrc";
	std::vector<ThreediO3dPart> parts;
	std::vector<ThreediO3dPanm> panm;
};

struct ThreediO3dTexture {
	int line = 0;
	std::string name;
	uint8_t slot = THREEDI_TEX_SLOT_DIFFUSE, type = THREEDI_TEX_TYPE_DIFFUSE, flags = 0, frame = 0;
};

// A generator record (`rgbgen`, `alphagen`, `ugen`, `vgen`) as given.
struct ThreediO3dGenerator {
	int line = 0; // 0: not given
	uint8_t style = 0;
	long long reg = -1;
	double rate = 0.0, start = 0.0, end = 0.0, phase = 0.0;
	int start_rgb[3] = {}, end_rgb[3] = {};
};

struct ThreediO3dMaterial {
	int line = 0;
	std::string shader;
	std::vector<ThreediO3dTexture> textures;
	int texanim_line = 0; // 0: no flipbook
	long long frames = 0;
	uint8_t animation_type = 0;
	int16_t time = 0;
	uint8_t reflect[4] = {};
	uint8_t flags = 0, alpha_test = 0, glass = 0, emissive = 0;
	ThreediO3dGenerator rgbgen, alphagen, ugen, vgen;
};

struct ThreediO3dUserPoint {
	int line = 0;
	std::string name;
	double position[3] = {}, direction[3] = {};
	long long part = 0;
	int32_t type = THREEDI_USER_POINT_GAMEPLAY;
};

struct ThreediO3dLight {
	int line = 0;
	long long part = 0;
	double position[3] = {}, atten_start = 0.0, atten_end = 0.0;
	uint8_t style = 0;
	double rate = 0.0;
	double phase = 0.0; // a register index for a register-driven style
	int start_rgb[3] = {}, end_rgb[3] = {};
	uint8_t flags = 0;
	double direction[3] = {0.0, 0.0, -1.0};
	double falloff = 0.0;
};

struct ThreediO3dOcclusion {
	int line = 0;
	uint8_t type = 0;
	long long section = 0, connecting = 0;
	bool sphere_given = false;
	ThreediBuildOccSphere sphere;
	std::vector<ThreediBuildVec3> vertices;
	std::vector<std::array<double, 4>> planes;
	std::vector<std::array<int, 4>> faces; // a b c plane (-1: the OED rule picks it)
};

struct ThreediO3dPlane {
	int line = 0;
	double normal[3] = {}, distance = 0.0;
	int16_t flags = 0;
};

struct ThreediO3dVolume {
	enum class Kind { box, planes, mesh };
	int line = 0;
	Kind kind = Kind::box;
	int32_t type = 0, flags = 0;
	ThreediBuildBox box; // box, planes
	std::vector<ThreediO3dPlane> planes;      // planes
	std::string label;                       // mesh
	std::vector<ThreediBuildVec3> vertices;   // mesh
	std::vector<std::array<int, 3>> triangles; // mesh
};

struct ThreediO3dCollisionVertex {
	int line = 0;
	ThreediBuildVec3 position;
};

struct ThreediO3dCollisionFace {
	int line = 0;
	uint32_t corner[3] = {};
	uint8_t poly_type = 1;
	uint32_t flags = 0;
	bool normal_given = false;
	ThreediBuildVec3 normal;
};

struct ThreediO3dSection {
	int line = 0;
	long long parent = 0;
	ThreediBuildVec3 offset;
	int sphere_line = 0; // `csphere`; 0: none
	ThreediBuildVec3 sphere_center;
	double sphere_radius = 0.0;
	bool sphere_bounded = false;
	ThreediBuildBox sphere_bounds;
	std::vector<ThreediO3dCollisionVertex> vertices;
	std::vector<ThreediO3dCollisionFace> faces;
	std::vector<ThreediO3dVolume> volumes;
};

struct ThreediO3dNamed {
	int line = 0;
	std::string name;
};

struct ThreediO3dFrameRecord {
	int line = 0;
	double rotation[9] = {};
};

struct ThreediO3dTranslation {
	int line = 0;
	ThreediBuildVec3 position;
};

struct ThreediO3dModel {
	ThreediO3dNamed name; // line 0: no `model` record
	bool skinned = false, uv1 = false;
	int tangents_line = 0; // `tangents 1`; 0: the layout follows the shaders
	std::vector<ThreediO3dNamed> registers;
	std::vector<ThreediO3dFrameRecord> frames; // MTRX rows 1, 2, ...
	std::vector<ThreediO3dMaterial> materials;
	std::vector<ThreediO3dLod> lods;
	std::vector<ThreediO3dUserPoint> user_points;
	std::vector<ThreediO3dLight> lights;
	std::vector<ThreediO3dOcclusion> occlusion;
	std::vector<ThreediO3dSection> sections;
	bool translations_given = false;
	std::vector<ThreediO3dTranslation> translations;
};

} // namespace opennova::threedi
