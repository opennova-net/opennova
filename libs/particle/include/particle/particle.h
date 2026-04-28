#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::particle {

// Engine: Joint Operations particle effect format (.ptl).
// IDB: retail Jointops.exe.
// Section dispatcher: CEffectWorld_ParseSectionCallback @ 0x5ecb40.
// Definition hydrator: CParticleDef_ParseFromConfigMap @ 0x5ed210 (size 0x1da5).
// See notes/ida_particle_witness.md for the full witness matrix and field-offset table.

struct Color3 {
	std::uint8_t r = 0;
	std::uint8_t g = 0;
	std::uint8_t b = 0;
};

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 — full set decoded.
enum class BlendMode : std::uint8_t {
	Blend = 0,     // default; matched first by string fallback
	Additive = 1,
	Premult = 2,
	Bump = 3,
	Mod = 4,       // matches "mod" substring (off_7DCBA8); also matches "mod2x" before that branch
	Mod2x = 5,
	Bumpadd = 6,
	Distort = 7,
};

const char *blend_mode_name(BlendMode mode) noexcept;
BlendMode parse_blend_mode(std::string_view raw) noexcept;

// FlagTable @ 0x848800, 5 entries (flagCount @ 0x848d28). Stored as bitfield
// because FlagTable_ParseFromString @ 0x5df970 ORs matched bits — typical use
// is one bit set, but the engine never enforces.
namespace move_flag {
constexpr std::uint32_t Normal    = 1u <<  0;
constexpr std::uint32_t Gravitate = 1u <<  1;
constexpr std::uint32_t Wander    = 1u <<  2;
constexpr std::uint32_t Bubble    = 1u <<  3;
constexpr std::uint32_t Orbit     = 1u <<  4;
} // namespace move_flag

std::uint32_t parse_move_bits(std::string_view raw) noexcept;
std::string format_move_bits(std::uint32_t bits);

// FlagTable @ 0x846A18, 26 named entries (`dword_846A14` reports 29 slots, the
// last 3 are zeroed). Bits are powers of two in the FlagTable order observed
// at 0x846A20+. Engine semantics: FlagTable_ParseFromString matches each token
// in the input string and ORs the bit; output writer emits space-separated
// names in table order via sub_5DF9C0 @ 0x5df9c0.
namespace particle_flag {
constexpr std::uint32_t NoVisNoUpdate         = 1u <<  0;
constexpr std::uint32_t InitialClip           = 1u <<  1;  // "INITIALYCLIP" (sic)
constexpr std::uint32_t NeverAge              = 1u <<  2;
constexpr std::uint32_t TopAlign              = 1u <<  3;
constexpr std::uint32_t OnMyDeath             = 1u <<  4;
constexpr std::uint32_t UseParentScale        = 1u <<  5;
constexpr std::uint32_t UseParentColor        = 1u <<  6;
constexpr std::uint32_t UseParentAlpha        = 1u <<  7;
constexpr std::uint32_t YawAndPitch           = 1u <<  8;
constexpr std::uint32_t GlobalWind            = 1u <<  9;
constexpr std::uint32_t FocalWind             = 1u << 10;
constexpr std::uint32_t FocalWindForceAging   = 1u << 11;
constexpr std::uint32_t CollideBounce         = 1u << 12;
constexpr std::uint32_t CollideSlide          = 1u << 13;
constexpr std::uint32_t CollideKill           = 1u << 14;
constexpr std::uint32_t EmitVector            = 1u << 15;
constexpr std::uint32_t PositionInterpolate   = 1u << 16;
constexpr std::uint32_t ForeverEmit           = 1u << 17;
constexpr std::uint32_t PositionRelative      = 1u << 18;
constexpr std::uint32_t UseParentRotations    = 1u << 19;
constexpr std::uint32_t GfxFlipRand           = 1u << 20;
constexpr std::uint32_t ControledAlignment    = 1u << 21;  // "CONTROLEDALLIGNMENT" (sic)
constexpr std::uint32_t SignedRotations       = 1u << 22;
constexpr std::uint32_t OneFrame              = 1u << 23;
constexpr std::uint32_t BurstDistribute       = 1u << 24;
constexpr std::uint32_t AmbientColor          = 1u << 25;
} // namespace particle_flag

std::uint32_t parse_particle_flags(std::string_view raw) noexcept;
std::string format_particle_flags(std::uint32_t bits);

// "table12", "table12 reverse", "table12 inverse", or both modifiers in any
// order. Engine: per-func dispatch in CParticleDef_ParseProperties (e.g.
// scale_func @ 0x5eafdd) sets bit 0x02 on "reverse" and bit 0x01 on "inverse".
struct CurveRef {
	std::string name;
	bool reverse = false;
	bool inverse = false;
	bool present = false; // true once any *_func key has been seen
};

// One of up to 4 graphic layers per particle. Engine layout: 788 bytes each
// (CParticleDefEntry_ParseGraphicProperty @ 0x5e3550). Field offsets in
// comments are within the layer block.
struct GraphicLayer {
	int index = 0;                 // 1..4 once present (header line "graphicN = ...")
	bool present = false;
	std::string texture;           // +0..259, e.g. "dirtpuf.tga"; may be empty ("blank" layer)
	std::string blend_mode_raw;    // +260..323, lowercased input to ParseBlendMode
	BlendMode blend_mode = BlendMode::Blend;  // +324, result of CParticleDefEntry_ParseBlendMode @ 0x5e29f0
	int flip_frames = 1;           // +716, default 1
	int flip_rate = 8;             // +720
	Color3 color1, color2, color3, color4;   // +700/+704/+708/+712
	bool color_overrides_set = false;
	float alpha = 1.0f;            // +408
	float scale = 0.0f;            // +328
	float scale_adj = 0.0f;        // +332
	CurveRef scale_func;           // +336..407 (LUT)
	CurveRef alpha_func;           // +412..483
	CurveRef red_func;             // +484..555
	CurveRef green_func;           // +556..627
	CurveRef blue_func;            // +628..699
};

// Engine: CParticleEffectDef, ~5204 B. Field comments cite offsets in the
// engine heap layout (see CParticleDef_ParseFromConfigMap decompile,
// notes/ida_particle_witness.md "CParticleEffectDef layout" table).
struct ParticleDef {
	std::string id;             // +0
	std::string child_id;       // +132
	std::string flags_raw;      // +136 — preserved verbatim from source for round-trip
	std::uint32_t flags = 0;    // +136 — FlagTable_ParseFromString @ 0x846A18 (see particle_flag::*)
	std::string move_raw;       // +140 — preserved verbatim
	std::uint32_t move = 0;     // +140 — FlagTable_ParseFromString @ 0x848800 (see move_flag::*)
	float lod = 0.0f;           // observed in corpus; CParticleDef_ParseProperties has no `lod` case — value is silently ignored on parse but written by CParticleDef_SaveToFile @ 0x5e4d70

	// Emission
	float emit_dur = 0.0f;
	float emit_dur_adj = 0.0f;
	float emit_rate = 0.0f;
	float emit_rate_adj = 0.0f;
	CurveRef emit_rate_func;
	float emit_delay = 0.0f;
	int emit_burst = 1;         // engine clamps `<1` to 1 post-parse
	int emit_maxoverride = 0;
	int emit_shape = 0;
	Vec3 emit_shape_size{};
	Vec3 emit_shape_size_skip{};

	// Position / lifetime
	float y_offset = 0.0f;
	float z_offset = 0.0f;
	float age = 0.0f;
	float age_adj = 0.0f;
	float scale = 0.0f;
	float scale_adj = 0.0f;
	CurveRef scale_func;

	// Visual
	float alpha = 1.0f;
	CurveRef alpha_func;
	CurveRef red_func;
	CurveRef green_func;
	CurveRef blue_func;
	Color3 color1, color2, color3, color4;
	float bump_scale = 0.0f;

	// Motion
	Vec3 orientation{};
	Vec3 orientationadj{};
	float yaw_rot = 0.0f;
	float yaw_rot_adj = 0.0f;
	float pitch_rot = 0.0f;
	float pitch_rot_adj = 0.0f;
	float roll_rot = 0.0f;
	float roll_rot_adj = 0.0f;
	float speed = 0.0f;
	float speed_adj = 0.0f;
	float elastic = 0.0f;
	float gravity = 0.0f;
	Vec3 gravity_mask = {1.0f, 1.0f, 1.0f};
	float drag = 0.0f;
	float spread = 0.0f;
	float spread_skip = 0.0f;
	float orbitalspeed = 0.0f;
	float orbitalspeed_adj = 0.0f;
	Vec3 orbital_axis = {0.0f, 1.0f, 0.0f};

	// Sound — 20 collide_sound{0..19} string slots @ +3952, stride 32
	std::array<std::string, 20> collide_sounds{};

	// Graphics — 4 layers @ +148/+936/+1724/+2512
	std::array<GraphicLayer, 4> graphics{};

	// Forward-compatibility: keys not in CParticleDef_ParseFromConfigMap's
	// dispatch switch are silently dropped by the engine. We preserve them so
	// new editor keys round-trip without surprises.
	std::vector<std::pair<std::string, std::string>> unknown_keys;
};

struct EffectDef {
	std::string id;
	std::vector<std::string> pdefs;   // ordered list of ParticleDef.id references
};

// 32 rows × 8 uint8_t observed in every corpus file. The smoke test asserts.
struct TableDef {
	std::string id;
	std::vector<std::array<std::uint8_t, 8>> rows;
};

// Editor-only metadata for a [tabledef]. Runtime ignores it.
struct TableEditHandles {
	std::string table_id;
	int handlecount = 0;
	int tightness = 0;
};

struct ParticleFile {
	std::vector<EffectDef> effects;
	std::vector<ParticleDef> particles;
	std::vector<TableDef> tables;
	std::vector<TableEditHandles> table_handles;

	const ParticleDef *find_particle(std::string_view id) const noexcept;
	const TableDef *find_table(std::string_view id) const noexcept;
};

} // namespace opennova::particle
