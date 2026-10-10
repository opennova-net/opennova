// The GP model format (GPM, GPS, GPP): the runtime `.3di` of Delta Force: Black Hawk
// Down, the format JO's 3DI3 replaced. OpenNova reads it only to migrate it to 3DI3
// (threedi_gp_migrate.h; ADR 0027 as amended for the BHD-era import): nothing at runtime
// walks a GP model and nothing writes one.
//
// The layout is the BHD loader's [orig: GP_LoadModel @ 0x510E10 (dfbhd)], one section after
// another with no chunk headers: the 0xEC header, the user points, the texture table, the
// collision block [orig: GP_LoadCollisionModel @ 0x514D70 (dfbhd)], the render vertices, one
// render model per LOD [orig: GP_LoadRenderModel @ 0x5157F0 (dfbhd)], the CTRL registers, the
// MTRX frames, then the sections the header's flags name: lights (bit 0), the vertex stream
// (bit 1) and the occlusion records (bit 2) [orig: GP_LoadOcclusionData @ 0x514FB0 (dfbhd)].
// docs/threedi/3di-gp-format-re.md section 2 is the record. A record keeps the words a reader
// or the migration uses; the runtime words the loader overwrites (pointers, the "Addr" tag)
// and the words no reader was found for are skipped.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::threedi_gp {

// The third magic byte after "GP": 'M', 'S' or 'P' [orig: @ 0x510ED7..0x510EF9 (dfbhd)].
enum class Kind : uint8_t {
	None = 0,
	Gpm = 1, // rigid: 44-byte vertices
	Gps = 2, // rigid with a stored normal w: 48-byte vertices
	Gpp = 3, // skinned: 60-byte vertices (three weights, four palette slots)
};

inline constexpr size_t kHeaderSize = 0xEC;
inline constexpr uint8_t kFormatVersion = 2;     // byte 3 [orig: @ 0x510EA2 (dfbhd)]
inline constexpr uint32_t kMaxRevision = 0x103;  // the u32 at 4, at most 259 [orig: @ 0x510EA2 (dfbhd)]
inline constexpr int kMaxLods = 4;               // the loader's four render-model slots
inline constexpr uint32_t kFlagLights = 0x1;     // [orig: @ 0x5113F6 (dfbhd)]
inline constexpr uint32_t kFlagVertexStream = 0x2; // [orig: @ 0x511488 (dfbhd)]
inline constexpr uint32_t kFlagOcclusion = 0x4;  // [orig: @ 0x511524 (dfbhd)]

// The 0xEC header (byte offsets). The model the BHD runtime walks is the loader's +4, so a
// consumer's model+h IS header offset h.
struct Header {
	Kind kind = Kind::None;
	uint32_t revision = 0;        // +0x04: 0x100, 0x102 or 0x103 in BHD's corpus; no consumer
	std::string name;             // +0x08, 16 bytes
	uint32_t flags = 0;           // +0x18: kFlag*
	int32_t lod_count = 0;        // +0x1C: 1..4
	// +0x20: each LOD's threshold, a 16.16 projected-radius pixel count
	// [orig: Model_SelectRlodLevelAndRender @ 0x5218EB..0x521959 (dfbhd)].
	std::array<int32_t, kMaxLods> thresholds{};
	// +0x40: each LOD's render tag, the raw dword the bone-callback table is searched for
	// (a multi-character constant: the bytes "crng" are 'gnrc')
	// [orig: EntityDef_LoadModelsAndCallbacks @ 0x43D74F..0x43D78E (dfbhd)].
	std::array<uint32_t, kMaxLods> tags{};
	int32_t radius = 0;           // +0x60: 16.16 [orig: Entity_InitFromModel @ 0x415788 (dfbhd)]
	int32_t radius_xy = 0;        // +0x64: no reader found
	int32_t height = 0;           // +0x68: no reader found
	int32_t vertex_count = 0;     // +0x88
	int32_t user_point_count = 0; // +0xB0
	int32_t control_register_count = 0; // +0xBC
	int32_t matrix_count = 0;     // +0xC4
	int32_t occlusion_count = 0;  // +0xD0
};

// A user point, 48 bytes, the 3DI3 USRP record's words [orig: @ 0x510F5B..0x510F74 (dfbhd)].
struct UserPoint {
	std::array<int32_t, 3> position{};  // 16.16, mission axes
	std::array<int32_t, 3> direction{}; // the local Z axis, 16.16
	int32_t part = 0;
	uint8_t type = 0;                   // +28 ('S', 'G', 'L', 'F', ...)
	std::string name;                   // +32, 16 bytes
};

// A texture table row, 60 bytes, read after the user points behind a u32 count
// [orig: GP_LoadModel @ 0x510F8D..0x510FB6 (dfbhd)]. A render material finds its row by
// the id at +0x24 [orig: GP_LoadRenderModel @ 0x515D71..0x515D84 (dfbhd)]; the name is the
// texture file both engines resolve (a .dds sibling first) [orig:
// Texture_LoadByNameWithChannel @ 0x4F9FB8..0x4FA117 (dfbhd)].
struct TextureRow {
	std::string name;   // +0x00, 16 bytes
	std::string alpha;  // +0x10, 16 bytes: the alpha source a flag 0x100 row loads
	int16_t id = 0;     // +0x24
	uint16_t flags = 0; // +0x26: 0x8 alpha only, 0x100 colour with +0x10's alpha
};

// --- collision (mission axes; 8.8 vertices, Q14 normals, 16.16 the rest) ---------

struct CollisionVertex {
	int16_t x = 0, y = 0, z = 0;
	int16_t section = 0; // the owning section's ordinal in most files; no BHD reader
};

struct CollisionNormal {
	int16_t x = 0, y = 0, z = 0;
	int16_t axis = 0; // 1 XY, 2 XZ, 4 YZ: the point-in-triangle projection
};

// 44 bytes, the JO runtime face word for word but for the box's order.
struct CollisionFace {
	std::array<int16_t, 3> corners{}; // local to the section's vertex run
	int16_t normal = 0;               // local to the section's normal run
	int32_t plane = 0;                // 16.16
	std::array<int32_t, 6> box{};     // INTERLEAVED: min x, max x, min y, max y, min z, max z
	uint32_t flags = 0;
	uint8_t surface = 0;              // the poly type (surface id)
};

// A section, 128 bytes.
struct CollisionSection {
	int32_t flags = 0;          // +0
	int32_t vertex_count = 0;   // +4
	int32_t face_count = 0;     // +12
	int32_t normal_count = 0;   // +20: the section's own normal run
	int32_t volume_count = 0;   // +28
	int32_t parent = 0;         // +36: the render part
	std::array<int32_t, 3> offset{};  // +52
	std::array<int32_t, 6> box{};     // +64, INTERLEAVED, as stored (the loader widens it)
	std::array<int32_t, 3> centre{};  // +88
	int32_t radius = 0;               // +100
};

// A bounding plane, 16 bytes: a 16.16 unit normal and its distance (inside: n.p + d < 0).
struct CollisionPlane {
	std::array<int32_t, 3> normal{};
	int32_t d = 0;
};

// A bounding volume, 96 bytes.
struct CollisionVolume {
	int32_t type = 0;                 // +0, the collidable type
	int32_t flags = 0;                // +4
	std::array<int32_t, 6> box{};     // +48, INTERLEAVED: the box every reader tests
	int32_t plane_count = 0;          // +72
};

struct Collision {
	std::array<int32_t, 3> radii{};   // +12: from the origin, in xy, the height
	std::array<int32_t, 6> box{};     // +24, INTERLEAVED
	std::vector<CollisionVertex> vertices;
	std::vector<CollisionNormal> normals;
	std::vector<CollisionFace> faces;
	std::vector<CollisionSection> sections;
	std::vector<std::array<int32_t, 3>> translations; // 16.16
	std::vector<CollisionPlane> planes;
	std::vector<CollisionVolume> volumes;
};

// --- render (model axes) ----------------------------------------------------------

// A render vertex as the file stores it; the kind decides which words are present. The
// loader drops the packed colour (GPM dword 6, GPS 7, GPP 10) and copies the rest
// [orig: GP_LoadModel @ 0x511055..0x511096, 0x51111E..0x51115F, 0x5111E6..0x51124B (dfbhd)].
struct Vertex {
	std::array<float, 3> position{};
	std::array<float, 3> weights{};      // GPP
	std::array<uint8_t, 4> palette{};    // GPP: palette slots, at most 15 [orig: @ 0x51127E (dfbhd)]
	float blend = 0.0f;                  // GPS: the weight of bone A (bone B takes the rest)
	std::array<float, 3> normal{};
	std::array<float, 2> uv0{};
	std::array<float, 2> uv1{};
};

// A part (subobject), 72 bytes.
struct Part {
	int32_t parent = 0;               // +12 (the root names itself)
	std::array<float, 3> rel{};       // +16: no BHD reader
	std::array<float, 3> abs{};       // +28: the pivot
	std::array<float, 3> centre{};    // +40: the bounding sphere's
	int32_t radius = 0;               // +52: 16.16
};

// A batch, 32 bytes: the opaque and alpha primitive counts it groups. A part's batch
// (local mode) or the one table of a skinned model (global mode).
struct Batch {
	int part = 0;                  // the part it belongs to (0 in a global table)
	int32_t opaque_count = 0;      // +4
	int32_t alpha_count = 0;       // +12
};

// A primitive: an index run over the model's vertices. A local one (a part's batch) is 40
// bytes before its indices, a global one 48 [orig: GP_LoadRenderModel @ 0x515A25, @ 0x5158CE
// (dfbhd)].
struct Primitive {
	int part = 0;                   // the part whose batch holds it (0 in a global table)
	bool alpha = false;             // in its batch's alpha run
	int32_t material = 0;           // +0: this LOD's material
	int32_t topology = 0;           // +12: 0 list, 1 strip
	int32_t first_vertex = 0;       // +16: the base vertex the indices count from
	int32_t vertex_word = 0;        // +20: GPM, GPS the last index; GPP the vertex count
	// +24: a GPP palette (16 part indices) and, global, its length at +47; a GPS primitive's
	// two bones are the low bytes of the words at +24 and +28.
	std::array<uint8_t, 16> palette{};
	uint8_t palette_length = 0;     // +47 (global)
	std::vector<uint16_t> indices;
};

// An 8-byte generator: style, its phase or (above style 0x70) local CTRL index, rate, start
// and end; the U, V and alpha generators of both engines share it byte for byte.
struct Generator {
	uint8_t style = 0;
	uint8_t param = 0;
	int16_t rate = 0;
	int16_t start = 0;
	int16_t end = 0;
};

// The 12-byte RGB generator: the colours are D3DCOLOR words (B, G, R, x)
// [orig: RgbGen_EvaluateColor @ 0x4FC179..0x4FC285 (dfbhd)].
struct RgbGenerator {
	uint8_t style = 0;
	uint8_t param = 0;
	int16_t rate = 0;
	std::array<uint8_t, 4> start{};
	std::array<uint8_t, 4> end{};
};

// A render material, 152 bytes. GP names no shader: the render state comes from the
// bytes below [orig: GPMaterial_BuildModeWord @ 0x5163F0; GPMaterial_CompileGPM @ 0x516E70
// (dfbhd)]; the record's own name (+0x00) is never read.
inline constexpr uint32_t kMaterialTwoSided = 0x1;    // cull none
inline constexpr uint32_t kMaterialAlphaTest = 0x2;   // GREATER than +0x3B
inline constexpr uint32_t kMaterialAlphaTestInv = 0x4;
inline constexpr uint32_t kMaterialClamp = 0x8;
inline constexpr uint32_t kMaterialFlipbook = 0x10;
inline constexpr uint32_t kMaterialUnlit = 0x100;
inline constexpr uint32_t kMaterialBump = 0x200;
struct Material {
	uint32_t attributes = 0;      // +0x10: kMaterial*
	int8_t frame_count = 0;       // +0x16: a flipbook's frames
	uint8_t frame_counter = 0;    // +0x17: the counter that picks the frame
	std::array<int8_t, 3> region_textures{}; // +0x28: the texture row id per mission region (-1 none)
	int8_t alpha_texture = 0;     // +0x2B: the row id when only the alpha source is the texture
	uint8_t env_pass = 0;         // +0x30: the environment pass
	uint8_t shader = 0;           // +0x32: 0 fixed function, else a bump technique
	uint8_t colour_source = 0;    // +0x38: 1 the region colour, 2 the texture
	uint8_t alpha_source = 0;     // +0x39: 0/1 the material's, 2 the texture's
	uint8_t alpha = 0;            // +0x3A: the constant alpha an alpha source 1 takes
	uint8_t alpha_ref = 0;        // +0x3B
	uint8_t blend = 0;            // +0x3C: 0/1 opaque, 2 alpha, 3 one/srcalpha, 4 additive, 5 multiply
	uint8_t paired = 0;           // +0x3D: the next record is its second stage
	Generator u, v;               // +0x50, +0x58
	RgbGenerator rgb;             // +0x60
	Generator alpha_gen;          // +0x6C
};

// A part's animation, 92 bytes (present when the render model's flags carry 2), as the
// bone builder reads it [orig: Model_TransformBoneMatrices @ 0x4FD850 (dfbhd)].
inline constexpr uint32_t kPanmAnimated = 0x1;
inline constexpr uint32_t kPanmSpinner = 0x2;
inline constexpr uint32_t kPanmEuler = 0x4;
inline constexpr uint32_t kPanmEulerZxy = 0x8;
inline constexpr uint32_t kPanmScaleUniform = 0x10;
inline constexpr uint32_t kPanmScale3 = 0x20;
inline constexpr uint32_t kPanmViewA = 0x100;
inline constexpr uint32_t kPanmViewB = 0x200;
struct PartAnimation {
	uint32_t flags = 0;
	uint8_t parent = 0;   // +4: the output slot carrying the pivot
	uint8_t part = 0;     // +5: the input row and pivot part
	int32_t frame = 0;    // +8: the MTRX selector (> 0 selects a frame)
	// +12: rotation Y, X, Z then scale X, Y, Z (8 bytes each); a spinner reads the first
	// 12 bytes as three float rates (Y, X, Z, in turns a second) instead.
	std::array<Generator, 6> tracks{};
	std::array<float, 3> spin{};
};

// One LOD's render model: the 0x88 header and the blob it sizes. The blob opens with one
// 12-byte point a non-root part (mission axes: the attach points the collision block's
// translations hold too), which nothing reads; they are skipped.
inline constexpr uint32_t kRenderGlobalBatch = 0x1;
inline constexpr uint32_t kRenderPartAnimations = 0x2;
struct RenderModel {
	uint32_t flags = 0;                  // +0x50: kRender*
	uint32_t extra_count = 0;            // +0x60: 88-byte primitives (none in BHD's corpus)
	uint32_t record_count = 0;           // +0x68: 232-byte records (none in BHD's corpus)
	std::vector<Part> parts;
	std::vector<Batch> batches;          // one a part (local), or the global table's
	std::vector<Primitive> primitives;   // in file order: batch by batch, opaque then alpha
	std::vector<Material> materials;
	std::vector<PartAnimation> part_animations;
};

struct Light {
	uint8_t style = 0, phase = 0;      // phase: a local CTRL index above style 0x70
	uint16_t rate = 0;
	uint32_t colour_start = 0;         // B, G, R, 0
	uint32_t colour_end = 0;
	std::array<float, 3> position{};   // model axes
	float atten_start = 0.0f, atten_end = 0.0f;
	int32_t part = 0;
};

struct OcclusionObject {
	uint8_t type = 0, parent = 0, connecting = 0;
	std::vector<std::array<float, 3>> vertices;
	std::vector<std::array<float, 4>> planes;
	std::vector<std::array<uint32_t, 3>> faces;
};

struct File {
	Header header;
	std::vector<UserPoint> user_points;
	std::vector<TextureRow> textures;
	Collision collision;
	std::vector<Vertex> vertices;
	std::vector<RenderModel> lods;
	std::vector<std::string> control_registers; // the names, in order
	std::vector<std::array<float, 16>> matrices;
	std::vector<Light> lights;
	std::vector<uint8_t> vertex_stream;           // 24 bytes a vertex
	std::vector<OcclusionObject> occlusion;
};

// The kind the first bytes name; None when they are not a GP model's.
Kind detect(const uint8_t *data, size_t size);

// Parse a whole GP file. False, with `error` saying where, when the header's gates fail
// (the loader's), a count runs past the bytes, or a section does not add up.
bool parse(const uint8_t *data, size_t size, File &out, std::string &error);

} // namespace opennova::threedi_gp
