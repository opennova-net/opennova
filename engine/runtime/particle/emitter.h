#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <formats/particle/particle.h>

namespace opennova::particle {

// Reimpl safety ceiling for a single emitter. The retail JO PTL corpus tops out
// at emit_maxoverride=400 (17 non-zero values across 1,402 field records); 4096
// preserves more than 10x that authored headroom while bounding hostile mods.
inline constexpr std::size_t kEmitterHardParticleLimit = 4096;

// Retail's "unlimited" particle budget word: FOREVEREMIT emitters and every
// child emitter (no self-emission) hold this value in emitter+0x11C
// [orig: CEffectEmitter_Initialize @ 0x5e62e1; CEffectEmitter_CalcEmissionRate @ 0x5e1c8f].
inline constexpr std::int32_t kEmitterForeverBudget = 0x7FFFFFF;

// Portable, Godot-agnostic particle simulator. Captures the engine's
// CParticleEmitter behavior (per-particle physics, emission shapes, lifetime,
// child emitters) but does NOT claim byte-exact parity with the retail
// renderer — the engine has DirectX-bound state (draw order, atlas baking,
// view-space transforms) that lives outside the simulation core.
//
// Engine references (Jointops.exe IDB):
//   CParticleEmitter_AdvanceFrame    @ 0x5e6570  — child spawns, expiry, update, emission, clock
//   CParticleEmitter_UpdateParticles @ 0x5e6980  — base-system physics integration
//   CParticleEmitter_UpdateAllParticles @ 0x5f3be0 — super-system integration (wind, orbit, aux angles)
//   CParticleEmitter_SpawnParticle   @ 0x5e7640  — per-particle init + RNG + parent inheritance
//   CParticleEmitter_SpawnNewParticle @ 0x5f35b0 — super-system spawn (aux angles, orbit rate)
//   CEffectEmitter_AdvanceEmission   @ 0x5e1d30  — the self-emission schedule
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

// The emitter's class id is its def's first graphic blend id (def+0x1E0, in
// the doc frame graphic[0]+324): a Distort lead graphic (class 7) keeps the
// emitter out of both scene particle passes and draws it only in the
// post-scene distortion pass [orig: CParticleGroup_RenderChildren
// @ 0x5E58D2; the same test in EffectWorld_HasDistortionParticles
// @ 0x5E986B].
inline bool particle_def_is_distortion_class(const ParticleDef &definition) noexcept {
	return definition.graphics[0].blend_mode == BlendMode::Distort;
}

struct Particle;
// Borrowed for one simulation advance; an absent field leaves ordinary effects unchanged.
class ParticleForceField {
public:
	virtual ~ParticleForceField() = default;
	virtual void apply(Particle &particle, std::size_t index, bool repulsion) const = 0;
	// The zone a particle spawned at `position` (render frame) binds to when no
	// spawn window is open: the nearest containing focal-wind zone, 0 for none.
	// [orig: CParticleEmitter_SpawnNewParticle @0x5F35B0 stores
	//  Terrain_FindNearestAmbientSoundZone(pos) @0x5F37C6..0x5F37D8 into
	//  particle+12 for EVERY spawn; Terrain_FindNearestAmbientSoundZone @0x5CBCD0
	//  answers dword_29D6BB0 while a rotor-wash trigger window is open]
	virtual std::uint16_t zone_at(const Vec3 &position) const = 0;
};

// The camera's six clip planes in the effect frame, `a*x + b*y + c*z + d >= 0`
// inside. Retail keeps the equivalent viewport/clip state on the particle
// manager (+108) and tests each NOVISNOUPDATE emitter's bounding sphere
// against it before updating [orig: CParticleEmitter_AdvanceFrame @ 0x5e65dc ->
// BoundingBox_IsVisibleInFrustum @ 0x5e45b0 -> Viewport_TransformAndClipPoint @ 0x4115e0].
struct ParticleViewFrustum {
	float planes[6][4]{};
	bool valid = false;
};

// Per-advance environment. Everything here is read-only for the simulator.
struct EmitterEnvironment {
	const ParticleForceField *forces = nullptr;
	// GLOBALWIND drift, effect-frame units per second — retail multiplies the
	// mission wind's per-tick fixed-point vector by 62 into flt_848D40..48 every
	// tick [orig: render_emitter_effect @ 0x5f70c0 (0x5f7112..0x5f7143)].
	Vec3 global_wind{};
	// NOVISNOUPDATE gate; null (or !valid) means no visibility state — retail
	// skips the test while the manager's clip state is unset (+108 == 0).
	const ParticleViewFrustum *frustum = nullptr;
};

struct Particle {
	std::uint16_t force_zone = 0;
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
	// per frame; it is NOT a draw-size ramp. Color LUTs read
	// `lut[(int)phase % 256]` (no lerp); the scale LUT lerps between bytes with
	// the fractional part. A mid-frame spawn starts at `time_offset * rate`
	// [orig: SpawnParticle @ 0x5e7898 seeds 0, @ 0x5e7d60 adds the offset;
	// UpdateParticles @ 0x5e6980 advances; BuildBillboardQuads @ 0x5e6d60 indexes].
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
	// pitch pair at aux+8/+12 @ 0x5f36fe/0x5f3757; RenderStaticBillboards
	// @ 0x5f5068 feeds (yaw, pitch, roll) * pi/180 into the Euler matrix]. Radians here.
	float yaw = 0.0f;
	float yaw_rate = 0.0f;
	float pitch = 0.0f;
	float pitch_rate = 0.0f;
	// ORBIT rate, radians/second, randomized PER PARTICLE at aux+16:
	// `((rand01*2-1) * orbitalspeed_adj + orbitalspeed * sign) * pi/180`, the
	// sign random unless SIGNEDROTATIONS [orig: CParticleEmitter_SpawnNewParticle
	// @ 0x5f3764..0x5f37c3 — the randomize-then-x-pi/180 tail @ 0x5f37a2..0x5f37c3]. The update rotates the particle's emitter-relative
	// position and velocity around def.orbital_axis by `orbit_rate * dt`
	// [orig: CParticleEmitter_UpdateAllParticles @ 0x5f3be0, the (move & 4) leg].
	float orbit_rate = 0.0f;
	// Child-emission schedule, one record per parent particle (retail keeps
	// them in a parallel array at emitter+0xF0, seeded in the SpawnParticle
	// tail @ 0x5e7fe2..0x5e80a6 and walked at the top of the parent's
	// AdvanceFrame @ 0x5e6633..0x5e6807). `child_clock` counts down to the next
	// child burst, `child_interval` is the reload value, `child_budget` the
	// particles this parent may still spawn.
	float child_clock = 0.0f;
	float child_interval = 0.0f;
	std::int32_t child_budget = 0;
	Color3 color{};            // sampled from one of color1..4 at spawn (per engine, DWORD-randomized)
	std::uint8_t alpha = 255;
	std::uint8_t color_slot = 0;     // which of color1..4 this particle sampled at spawn
	std::uint8_t graphic_layer = 0;  // which of def.graphics[0..3] this particle uses
	std::uint8_t serial = 0;         // monotonic serial within emitter (matches engine's per-particle byte at +0)
	std::uint32_t flags = 0;         // particle_runtime_flag::* — engine particle+4
};

struct Emitter {
	std::uint16_t force_zone = 0;
	const ParticleDef *def = nullptr;
	// The resolved `child_id` definition. Retail instantiates one child emitter
	// per parent emitter at Initialize (emitter+0xC) and seeds a per-particle
	// schedule for it on every parent spawn; the embedder binds the child
	// Emitter it advances alongside [orig: CEffectEmitter_Initialize @ 0x5e6417..0x5e64a4].
	const ParticleDef *child_def = nullptr;
	Vec3 position{};
	Vec3 prev_position{};
	// Emission axis handed to the direction helper. EMITVECTOR defs take the
	// spawn direction; every other def leaves it zero, and the helper's
	// `|v|^2 < 0.99` fallback then emits around world +Y
	// [orig: CEffectEmitter_Initialize @ 0x5e60f9..0x5e612e (flag 0x10000);
	//  compute_cone_direction_vector @ 0x5e203a / generate_random_direction_basis @ 0x5e23d6].
	Vec3 forward = {0.0f, 0.0f, 0.0f};
	// The direction the group was spawned/re-oriented with, kept for every def
	// (retail's group orientation input); reports read this, the simulator
	// only reads `forward` [orig: CEffectEmitter_SetOrientationFromDirection
	// @ 0x5e5b00 — the +48/+60/+124 writes @ 0x5e5d51..0x5e5d82 are EMITVECTOR-only].
	Vec3 spawn_direction = {0.0f, 0.0f, 1.0f};

	// Optional override for the shared NORMAL-gravity / GRAVITATE-force slot.
	// Retail seeds that slot in CEffectEmitter_Initialize @ 0x5e6020 as
	// `def.gravity * -0.09803897`; `gravity_accel` stores that converted value.
	// A non-zero spring_const lets a manager override only the GRAVITATE path.
	float spring_const = 0.0f;
	float gravity_accel = 0.0f;     // retail authored gravity × -0.09803897
	float drag_coefficient = 0.0f;  // retail authored drag × 0.01

	// CParticleEmitter_TranslatePosition @ 0x5efe90 mirror.
	// `last_translation_delta` holds (new_pos - old_pos) from the most
	// recent `emitter_translate` call; `cumulative_translation` is the
	// running sum of those deltas since `emitter_init`.
	Vec3 last_translation_delta = {0.0f, 0.0f, 0.0f};
	Vec3 cumulative_translation = {0.0f, 0.0f, 0.0f};

	// The live-particle AABB retail accumulates in UpdateParticles
	// (emitter+0xD4..0xE8, reset to ±1e15 each update) and collapses to the
	// emitter position when no particle is alive [orig: UpdateAllParticles
	// @ 0x5f3be0 head; AdvanceFrame @ 0x5e658a]. Read by the NOVISNOUPDATE gate.
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	bool bounds_valid = false;

	// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 LOD decimation:
	// engine computes `divisor = round(1.0 / *(emitter+8 + 0x3F4))` each
	// frame from a manager-set perf budget, then skips particles where
	// `(particle.serial % divisor) != 0`. Default 1 = render every
	// particle (engine behaviour at full perf budget). Physics remains
	// untouched because the simulator never reads this field.
	std::uint32_t lod_divisor = 1;

	// CParticleEmitter_UpdateParticles @ 0x5e6980 kill-plane:
	//   def.flags & 0x08000000 (bit 27 = BELOWH20): kill if particle.y > threshold
	//   def.flags & 0x10000000 (bit 28 = ABOVEH20): kill if particle.y <= threshold
	// Engine threshold lives at `*(emitter+332)` (a manager-supplied
	// `float*` populated per spawn site). We expose the value + mode directly.
	// Default mode 0 = disabled (no kill plane). Like `spring_const` /
	// `lod_divisor`, these are runtime scalars NOT reset by `emitter_init`.
	std::uint32_t kill_plane_mode = 0;  // 0=disabled, 1=kill above, 2=kill at/below
	float kill_plane_y = 0.0f;

	std::vector<Particle> particles;
	// Caller/authored soft cap, always bounded by kEmitterHardParticleLimit in
	// the simulator even when a caller assigns this field directly.
	std::size_t max_particles = 256;

	// Runtime copies of the definition's two positional values. Ordinary
	// emitters retain the authored y-offset/camera-pull behavior; controlled
	// effect groups reinterpret the pair through retail's blend parameter.
	float spawn_y_offset = 0.0f;
	float camera_pull = 0.0f;
	float age = 0.0f;                  // emitter wall-clock

	// The self-emission schedule [orig: CEffectEmitter_Initialize @ 0x5e6020;
	// CEffectEmitter_AdvanceEmission @ 0x5e1d30]:
	//   emit_rate      randomized `emit_rate ± emit_rate_adj` (particles/second)
	//   emit_dur_total randomized `emit_dur ± emit_dur_adj`
	//   emit_interval  the current inter-burst interval slot (emitter+0x114). It
	//                  STARTS AT emit_delay, which is how the delay is realized:
	//                  the first burst waits until the carried time reaches it.
	//   emit_carry     time carried between frames (emitter+0x144)
	//   emit_budget    particles this emitter may still spawn (emitter+0x11C) =
	//                  emit_burst * (int)(rate * dur), kEmitterForeverBudget for
	//                  FOREVEREMIT and for child emitters
	//   emit_clock     the emit-rate curve phase (emitter+0x110): starts at
	//                  `-delay*(delay+dur)/256`, advances `256/(delay+dur)` per
	//                  second, indexes `lut[(int)clock % 256]`; the emitter's
	//                  emission window closes when it reaches 256
	//   emit_heading   the sequential azimuth accumulator (emitter+208) the
	//                  BURSTDISTRIBUTE direction helper advances by 2*pi/emit_burst
	//   self_emitting  false for child emitters and for trigger-only groups
	//                  (retail emitter+0x104 == 0): AdvanceEmission is skipped
	float emit_rate = 0.0f;
	float emit_dur_total = 0.0f;
	float emit_interval = 0.0f;
	float emit_carry = 0.0f;
	std::int32_t emit_budget = 0;
	float emit_clock = 0.0f;
	float emit_clock_rate = 0.0f;
	float emit_heading = 0.0f;
	bool self_emitting = true;
	bool active = false;

	// Per-particle serial counter (matches engine's `LOBYTE(emitter[1].prevPosZ)` increment).
	std::uint8_t next_serial = 0;

	// Deterministic RNG state — caller supplies seed via emitter_init.
	std::uint32_t rng_state = 1;
};

// Emit-shape values consumed by spawn_particle. Matches engine field
// `emit_shape` (offset +3924) and the switch in CParticleEmitter_SpawnParticle.
enum class EmitShape : std::uint32_t {
	Point = 0,
	Box = 1,         // hollow box shell ±emit_shape_size
	Sphere = 2,      // direction helper with polar range [0, 360], annular per-axis magnitude
	Cone = 3,        // direction helper pinned to polar 90 (a ring around the axis), annular magnitude
};

// 10-bit deterministic RNG. Engine uses `rand() & 0x3FF`; this returns the
// same range so tests can pin behavior without depending on libc rand().
std::uint32_t emitter_rand10(Emitter &e) noexcept;

// rand10 normalized to [0, 1], inclusive, using retail's /1023 scale.
float emitter_rand_unit(Emitter &e) noexcept;

// rand10 normalized to [-1, 1], inclusive.
float emitter_rand_signed(Emitter &e) noexcept;

// Initialise an emitter against a particle def at `pos`. `direction` is the
// spawn direction; only EMITVECTOR defs adopt it as their emission axis.
// Resets all schedule state. Particle storage is cleared but the underlying
// vector capacity is retained.
void emitter_init(Emitter &e, const ParticleDef *def, Vec3 pos, std::uint32_t seed,
		Vec3 direction = {0.0f, 0.0f, 1.0f});

// Retail's per-particle budget for a freshly initialized emitter:
// `emit_burst * (int)(rate * dur)`, or kEmitterForeverBudget for FOREVEREMIT /
// non-self-emitting emitters [orig: CEffectEmitter_Initialize @ 0x5e62bf..0x5e62e1].
std::int32_t emitter_initial_budget(const Emitter &e) noexcept;

// Advance the simulation by `dt` seconds in retail's AdvanceFrame order: child
// spawns into `child` (when the def names a child and one is bound), expiry,
// integration, self-emission, clock. `child` may be null. `group_visible` is
// the owning group's section gate (EffectSectionGate): a NOVISNOUPDATE emitter
// of a hidden group freezes exactly as an off-screen one does.
void emitter_advance(Emitter &e, float dt, const EmitterEnvironment &env = {},
		Emitter *child = nullptr, bool group_visible = true);

// The retail aliveness leaf: emitting (budget left and the window open, or a
// non-self-emitting child) or still carrying live particles
// [orig: CParticleEmitter_IsAliveOrEmitting @ 0x5e2640].
bool emitter_alive(const Emitter &e) noexcept;

// Spawn one particle immediately (bypasses the emission schedule). Returns
// false if the particle pool is at max_particles and no slot can be reclaimed.
// Test helper; production code should use emitter_advance. `forces` answers
// the spawn-time zone search when the emitter has no spawn window.
bool emitter_spawn_one(Emitter &e, const ParticleForceField *forces = nullptr);

// The rotor-wash re-trigger: one particle spawned exactly like a scheduled
// one but at `position` along `forward`, with `force_zone_window` standing in
// for the open dword_29D6BB0 window (0 falls back to the `forces` search).
// [orig: CEffectWorld_SpawnAllActiveChildren @0x5E5E70 calls vtable slot 6 =
//  CParticleEmitter_SpawnNewParticle @0x5F35B0 (position, direction, 0.0, 0,
//  parent -1, 0) on each top-level child; the emission schedule
//  (CEffectEmitter_AdvanceEmission @0x5E1D30 @0x5E1E3E) calls the same slot
//  with the emitter's own +24 position and +48 forward]
bool emitter_spawn_one_at(Emitter &e, Vec3 position, Vec3 forward,
		std::uint16_t force_zone_window, const ParticleForceField *forces = nullptr);

// Move the emitter to `new_pos`, recording the delta and updating the
// cumulative drift since spawn. Engine ref:
// CParticleEmitter_TranslatePosition @ 0x5efe90 — same pattern (compute
// delta, update position, accumulate). Use this in preference to
// `e.position = new_value` when callers need the delta tracking
// (e.g. moving emitters following a parent transform).
void emitter_translate(Emitter &e, Vec3 new_pos) noexcept;

// The mission wind as the particle world sees it: `wind_speed` and
// `wind_direction` (degrees) from the mission header become a per-tick
// fixed-point vector, which the effect world converts to its float frame and
// scales by 62 into units per second [orig: sub_5DE970 @ 0x5de970 (Game_StartMission
// @ 0x524aff); render_emitter_effect @ 0x5f70c0 @ 0x5f7112..0x5f7143].
Vec3 mission_wind_vector(int wind_speed, int wind_direction_degrees) noexcept;

} // namespace opennova::particle
