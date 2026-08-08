#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "particle/particle.h"

namespace opennova::particle {

// Reimpl safety ceiling for a single emitter. The retail JO PTL corpus tops out
// at emit_maxoverride=400 (17 non-zero values across 1,402 field records); 4096
// preserves more than 10x that authored headroom while bounding hostile mods.
inline constexpr std::size_t kEmitterHardParticleLimit = 4096;

// Portable, Godot-agnostic particle simulator. Captures the engine's
// CParticleEmitter behavior (per-particle physics, emission shapes, lifetime,
// child particles) but does NOT claim byte-exact parity with the retail
// renderer — the engine has DirectX-bound state (draw order, atlas baking,
// view-space transforms) that lives outside the simulation core.
//
// Engine references (Jointops.exe IDB):
//   CParticleEmitter_AdvanceFrame    @ 0x5e6570  — emission + expiration loop
//   CParticleEmitter_UpdateParticles @ 0x5e6980  — per-frame physics integration
//   CParticleEmitter_SpawnParticle   @ 0x5e7640  — per-particle init + RNG
//   CParticleEmitter_TranslatePosition @ 0x5efe90 — parent transform follow
//
// RNG: engine uses `rand() & 0x3FF` (10-bit, 0..1023) throughout SpawnParticle.
// We expose a deterministic LCG with the same resolution so tests can pin
// exact spawn positions across platforms.

// Per-particle modulator presence flags. Engine writes these at particle+4 in
// CParticleEmitter_SpawnParticle @ 0x5e7640 based on which per-graphic LUT
// pointer is non-null (i.e. the curve resolved to a tabledef during
// CEffectDef_ResolveAllReferences @ 0x5e9d70). The renderer
// (CParticleEmitter_BuildBillboardQuads @ 0x5e6d60) only modulates the
// matching channel when its bit is set — particles that lack a curve LUT
// pointer skip that channel's modulation entirely.
namespace particle_runtime_flag {
constexpr std::uint32_t AlphaCurve   = 0x01;  // graphic+480: alpha LUT present
constexpr std::uint32_t RedCurve     = 0x02;  // graphic+552: red LUT present
constexpr std::uint32_t GreenCurve   = 0x04;  // graphic+624: green LUT present
constexpr std::uint32_t BlueCurve    = 0x08;  // graphic+696: blue LUT present
constexpr std::uint32_t ScaleCurve   = 0x10;  // graphic+404: scale LUT present (interp scale modulator)
constexpr std::uint32_t Flipbook     = 0x20;  // graphic+716: flip frame_count > 1
constexpr std::uint32_t LitColor     = 0x80;  // graphic+472 ∈ {Bump=3, Bumpadd=6} → vertex-lit color path
constexpr std::uint32_t Distort      = 0x100; // graphic+472 == Distort=7
} // namespace particle_runtime_flag

struct Particle {
	Vec3 position{};
	Vec3 velocity{};
	float age = 0.0f;          // remaining seconds; <=0 means expired
	float lifetime = 0.0f;     // initial age (for normalized t = 1 - age/lifetime)
	// Base draw size in world units, randomized once at spawn from the chosen
	// graphic layer: `graphic.scale + graphic.scale_adj * rand_signed`
	// [orig: CParticleEmitter_SpawnParticle @ 0x5e7862 — graphic+328/+332 into
	// particle+0x38]. The renderer's quad half-extent is
	// `0.5 * size * (ScaleCurve ? scale_lut_lerp(phase) / 128 : 1)`
	// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 — the
	// flt_7C3DD4 (1/128) * flt_7C3B94 (0.5) chain].
	float size = 0.0f;
	// Curve phase: 0 -> 256 across the particle's lifetime. This is the LUT
	// index source for every per-particle curve (color/alpha/scale) — the
	// engine stores it at particle+0x30 and advances it by `phase_rate * dt`
	// per frame; it is NOT a draw-size ramp (the earlier "spawn-pop softener"
	// reading was wrong). Color LUTs read `lut[(int)phase % 256]` (no lerp);
	// the scale LUT lerps between bytes with the fractional part
	// [orig: SpawnParticle @ 0x5e7898 seeds 0; UpdateParticles @ 0x5e6980
	// advances; BuildBillboardQuads @ 0x5e6d60 indexes].
	float curve_phase = 0.0f;
	// 256 / lifetime — the phase advance per second
	// [orig: SpawnParticle @ 0x5e788c — flt_7D1D70 (256.0) / age into +0x34].
	float phase_rate = 0.0f;
	float rotation = 0.0f;     // roll, accumulated radians (engine stores degrees at +0x3C, deg->rad at render)
	float rotation_rate = 0.0f;
	// YAWANDPITCH channel (def.flags & 0x100): world-oriented quads carry
	// per-particle yaw/pitch Euler state in a parallel array at emitter+0x150
	// [orig: CParticleEmitter_SpawnNewParticle @ 0x5f3663 seeds yaw =
	// def.orientation.x + adj.x*rand01, yaw_rate = yaw_rot family @ 0x5f36a5;
	// pitch pair follows; RenderStaticBillboards @ 0x5f5068 feeds
	// (yaw, pitch, roll) * pi/180 into the Euler matrix]. Radians here.
	float yaw = 0.0f;
	float yaw_rate = 0.0f;
	float pitch = 0.0f;
	float pitch_rate = 0.0f;
	Color3 color{};            // sampled from one of color1..4 at spawn (per engine, DWORD-randomized)
	std::uint8_t alpha = 255;
	std::uint8_t color_slot = 0;     // which of color1..4 this particle sampled at spawn
	std::uint8_t graphic_layer = 0;  // which of def.graphics[0..3] this particle uses
	std::uint8_t serial = 0;         // monotonic serial within emitter (matches engine's per-particle byte at +0)
	std::uint32_t flags = 0;         // particle_runtime_flag::* — engine particle+4
};

struct Emitter {
	const ParticleDef *def = nullptr;
	Vec3 position{};
	Vec3 prev_position{};
	Vec3 forward = {0.0f, 0.0f, 1.0f}; // emission direction for shape=3 (cone)

	// Manager-level RGB tint applied to every particle's modulated color in
	// the renderer (CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 reads
	// emitter+200..202 as 3 bytes and computes `(byte * channel) >> 7` per
	// channel — so engine-byte 128 = 1.0 neutral). Used for screen-flash /
	// explosion tints. Default {1, 1, 1} matches the engine neutral state.
	Vec3 color_tint = {1.0f, 1.0f, 1.0f};

	// Optional override for the shared NORMAL-gravity / GRAVITATE-force slot.
	// Retail seeds that slot in CEffectEmitter_Initialize @ 0x5e6020 as
	// `def.gravity * -0.09803897`; `gravity_accel` stores that converted value.
	// A non-zero spring_const lets a manager override only the GRAVITATE path.
	float spring_const = 0.0f;
	float gravity_accel = 0.0f;     // retail authored gravity × -0.09803897
	float drag_coefficient = 0.0f;  // retail authored drag × 0.01
	// Runtime radians/sec, converted after signed-randomizing authored
	// orbitalspeed +/- orbitalspeed_adj (which are degrees/sec). The reimpl ORBIT
	// integrator remains an approximation of retail's basis/age chain.
	float orbit_speed = 0.0f;

	// CParticleEmitter_TranslatePosition @ 0x5efe90 mirror.
	// `last_translation_delta` holds (new_pos - old_pos) from the most
	// recent `emitter_translate` call; `cumulative_translation` is the
	// running sum of those deltas since `emitter_init`. The engine seeds
	// equivalents at emitter+212/+224 to the spawn position and uses them
	// as AABB min/max accumulators in UpdateAllParticles; we expose just
	// the deltas because we don't yet track a runtime AABB.
	Vec3 last_translation_delta = {0.0f, 0.0f, 0.0f};
	Vec3 cumulative_translation = {0.0f, 0.0f, 0.0f};

	// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 LOD decimation:
	// engine computes `divisor = round(1.0 / *(emitter+8 + 0x3F4))` each
	// frame from a manager-set perf budget, then skips particles where
	// `(particle.serial % divisor) != 0`. Default 1 = render every
	// particle (engine behaviour at full perf budget). Higher values
	// uniformly skip particles for low-perf scenes — physics remains
	// untouched because the simulator never reads this field; only the
	// renderer does. Exposed for callers that want to dial down render
	// cost without changing simulation state.
	std::uint32_t lod_divisor = 1;

	// CParticleEmitter_UpdateParticles @ 0x5e6980 kill-plane:
	//   def.flags & 0x08000000 (bit 27 = BELOWH20): kill if particle.y > threshold
	//   def.flags & 0x10000000 (bit 28 = ABOVEH20): kill if particle.y <= threshold
	// Engine threshold lives at `*(emitter+332)` (a manager-supplied
	// `float*` populated per spawn site — terrain y for ground kill,
	// ceiling y for upward kill). We expose the value + mode directly.
	// Default mode 0 = disabled (no kill plane). Bits 27/28 are the named
	// flags BELOWH20 / ABOVEH20 (`particle_flag::BelowH2O` / `AboveH2O`,
	// engine flag-table idx 27/28 — corrected from the earlier "engine-
	// internal" note per the ParticleEdit grill D5). `def.flags` carries them
	// via the parser and the retail corpus authors both bits. A
	// caller still has to map the bit to a mode and provide the site-specific
	// threshold. Like `color_tint` / `spring_const` / `lod_divisor`, these are
	// runtime scalars NOT reset by `emitter_init`.
	std::uint32_t kill_plane_mode = 0;  // 0=disabled, 1=kill above, 2=kill at/below
	float kill_plane_y = 0.0f;

	std::vector<Particle> particles;
	// Caller/authored soft cap, always bounded by kEmitterHardParticleLimit in
	// the simulator even when a caller assigns this field directly.
	std::size_t max_particles = 256;

	// Emission scheduling
	float emit_accumulator = 0.0f;     // tracks time since last burst
	float emit_dur_remaining = 0.0f;   // counts down from def.emit_dur (by AGE, not per burst)
	float emit_dur_total = 0.0f;
	float emit_rate = 0.0f;
	float age = 0.0f;                  // emitter wall-clock
	float emit_delay_remaining = 0.0f; // counts down from def.emit_delay before any spawn
	bool emit_started = false;         // first burst primed? (it lands at t≈0, not one interval in)
	bool active = false;
	bool finite = true;                // false if FOREVEREMIT flag set

	// Per-particle serial counter (matches engine's `LOBYTE(emitter[1].prevPosZ)` increment).
	std::uint8_t next_serial = 0;

	// Deterministic RNG state — caller supplies seed via emitter_init.
	std::uint32_t rng_state = 1;
};

// Emit-shape values consumed by spawn_particle. Matches engine field
// `emit_shape` (offset +3924) and the switch in CParticleEmitter_SpawnParticle.
enum class EmitShape : std::uint32_t {
	Point = 0,
	Box = 1,         // axis-aligned box ±emit_shape_size
	Sphere = 2,      // random direction normalized, biased by emit_shape_size
	Cone = 3,        // cone around `forward` with emit_shape_size half-angles
};

// 10-bit deterministic RNG. Engine uses `rand() & 0x3FF`; this returns the
// same range so tests can pin behavior without depending on libc rand().
std::uint32_t emitter_rand10(Emitter &e) noexcept;

// rand10 normalized to [0, 1], inclusive, using retail's /1023 scale.
float emitter_rand_unit(Emitter &e) noexcept;

// rand10 normalized to [-1, 1], inclusive.
float emitter_rand_signed(Emitter &e) noexcept;

// Initialise an emitter against a particle def. Resets all state, including
// emission delay/duration counters. Particle storage is cleared but the
// underlying vector capacity is retained.
void emitter_init(Emitter &e, const ParticleDef *def, Vec3 pos, std::uint32_t seed);

// Advance the simulation by `dt` seconds. Spawns new particles per emission
// schedule, integrates physics, ages particles, removes expired ones.
void emitter_advance(Emitter &e, float dt);

// Spawn one particle immediately (bypasses the emission schedule). Returns
// false if the particle pool is at max_particles and no slot can be reclaimed.
// Test helper; production code should use emitter_advance.
bool emitter_spawn_one(Emitter &e);

// Move the emitter to `new_pos`, recording the delta and updating the
// cumulative drift since spawn. Engine ref:
// CParticleEmitter_TranslatePosition @ 0x5efe90 — same pattern (compute
// delta, update position, accumulate). Use this in preference to
// `e.position = new_value` when callers need the delta tracking
// (e.g. moving emitters following a parent transform).
void emitter_translate(Emitter &e, Vec3 new_pos) noexcept;


} // namespace opennova::particle
