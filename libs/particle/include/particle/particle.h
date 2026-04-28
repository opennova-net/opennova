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

// Per-frame UV rectangle for flipbook animations. Engine writes runtime-
// resolved rects at `graphic+724` as a contiguous array of pointers — each
// pointer references a 5-float struct read by
// `CParticleEmitter_BuildBillboardQuads @ 0x5e6d60` as
// (u_min, v_max, u_max, v_min, inset). The renderer applies `inset` as a
// symmetric texel padding to avoid bleeding from neighbouring atlas tiles.
//
// Our portable form embeds the rects directly in `GraphicLayer::baked_uv_rects`
// (as derived state alongside the curve LUTs). Default fill is a horizontal
// strip — `u_min = frame/N, u_max = (frame+1)/N, v_min = 0, v_max = 1,
// inset = 0` — which matches the previous on-the-fly UV math. Future atlas-
// bake support replaces the default with explicit per-frame rects.
struct UvRect {
	float u_min = 0.0f;
	float v_min = 0.0f;
	float u_max = 1.0f;
	float v_max = 1.0f;
	float inset = 0.0f;
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
//
// **Engine quirk (verified 2026-04-28 from raw table bytes)**: the `bitmask`
// fields in the engine struct are NOT sequential. Memory order at 0x848800
// is `[NORMAL, GRAVITATE, WANDER, BUBBLE, ORBIT]` but the bitmask field of
// each entry is reordered: ORBIT lives at bit 2 (=0x04), WANDER at bit 3
// (=0x08), BUBBLE at bit 4 (=0x10). The renderer code path
// `(def.move & 4)` in `UpdateAllParticles @ 0x5f3be0` is therefore the
// ORBIT integrator (rotation around `def.orbital_axis`), not WANDER.
namespace move_flag {
constexpr std::uint32_t Normal    = 1u <<  0;  // bit 0 = 0x01
constexpr std::uint32_t Gravitate = 1u <<  1;  // bit 1 = 0x02
constexpr std::uint32_t Orbit     = 1u <<  2;  // bit 2 = 0x04 — engine table entry @ 0x848C20
constexpr std::uint32_t Wander    = 1u <<  3;  // bit 3 = 0x08 — engine table entry @ 0x848A10
constexpr std::uint32_t Bubble    = 1u <<  4;  // bit 4 = 0x10 — engine table entry @ 0x848B18
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
//
// `baked_lut` mirrors the runtime LUT pointer the engine writes at
// CurveRef-equivalent offset +68 (e.g. graphic+480 for alpha_func, +552 for
// red_func, ...). Engine: CEffectDef_ResolveTblDefReference @ 0x5e9630 stores
// `TableDefByName + 328` — i.e. the tabledef's 32 × 8 byte buffer read row-
// major as a flat 256-byte array. The renderer indexes
// `lut[(int)(t * 256) & 0xFF]` (no interpolation) per
// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60.
struct CurveRef {
	std::string name;
	bool reverse = false;
	bool inverse = false;
	bool present = false;                          // true once any *_func key has been seen
	bool baked = false;                            // true after bake_particle_def_curves resolves a TableDef
	std::array<std::uint8_t, 256> baked_lut{};
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

	// Per-frame UV rect array (runtime-baked alongside CurveRef LUTs). Engine
	// stores equivalents at graphic+724 as a pointer array populated by
	// `CParticleManager_BuildTextureAtlases @ 0x5e8db0`. Filled by
	// `bake_graphic_uv_rects` with horizontal-strip defaults; atlas-bake
	// support would replace the defaults with explicit per-frame rects.
	std::vector<UvRect> baked_uv_rects;
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

// Engine: CEffectDef_ResolveTblDefReference @ 0x5e9630. Flatten `table.rows`
// (32 × 8 bytes) row-major into a 256-byte LUT. `reverse` reads source bytes
// in reverse index order; `inverse` writes `255 - src` per byte. Both modifiers
// are independent and may combine. Output array is overwritten in full.
void bake_curve_lut(const TableDef &table, bool reverse, bool inverse,
		std::array<std::uint8_t, 256> &out) noexcept;

// Walks every `*_func` CurveRef on `def` (5 particle-level + 5 per graphic),
// looks up each `CurveRef::name` in `tables`, and bakes the LUT into
// `CurveRef::baked_lut` (sets `baked = true` on success). Curves with
// `present == false` are skipped. Curves whose name does not resolve leave
// `baked = false` and the LUT untouched. Also bakes per-frame UV rects via
// `bake_graphic_uv_rects` for every present graphic layer. Mirrors the
// engine's resolve-after-parse pass
// (CEffectDef_ResolveAllReferences @ 0x5e9d70).
void bake_particle_def_curves(ParticleDef &def,
		const std::vector<TableDef> &tables) noexcept;

// Fill `layer.baked_uv_rects` with per-frame UV rectangles. Default fill is
// horizontal strip (frame N spans u in [N/count, (N+1)/count], v in [0, 1])
// matching the renderer's prior on-the-fly math. Engine equivalent:
// `CParticleManager_BuildTextureAtlases @ 0x5e8db0` populates the
// `graphic+724` rect-pointer array from the atlas placement of each frame.
// Idempotent — calling it multiple times overwrites the same entries.
void bake_graphic_uv_rects(GraphicLayer &layer) noexcept;

} // namespace opennova::particle
