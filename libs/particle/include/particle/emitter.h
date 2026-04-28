#pragma once

#include <cstdint>
#include <vector>

#include "particle/particle.h"

namespace opennova::particle {

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

struct Particle {
	Vec3 position{};
	Vec3 velocity{};
	float age = 0.0f;          // remaining seconds; <=0 means expired
	float lifetime = 0.0f;     // initial age (for normalized t = 1 - age/lifetime)
	float scale = 0.0f;        // grows from 0..def.scale over lifetime (engine: scale_velocity = 1/age)
	float scale_velocity = 0.0f;
	float rotation = 0.0f;     // accumulated radians
	float rotation_rate = 0.0f;
	Color3 color{};            // sampled from one of color1..4 at spawn (per engine, DWORD-randomized)
	std::uint8_t alpha = 255;
	std::uint8_t graphic_layer = 0;  // which of def.graphics[0..3] this particle uses
	std::uint8_t serial = 0;         // monotonic serial within emitter (matches engine's per-particle byte at +0)
};

struct Emitter {
	const ParticleDef *def = nullptr;
	Vec3 position{};
	Vec3 prev_position{};
	Vec3 forward = {0.0f, 0.0f, 1.0f}; // emission direction for shape=3 (cone)

	std::vector<Particle> particles;
	std::size_t max_particles = 256;   // soft cap; engine uses emit_maxoverride

	// Emission scheduling
	float emit_accumulator = 0.0f;     // tracks time since last burst
	float emit_dur_remaining = 0.0f;   // counts down from def.emit_dur
	float age = 0.0f;                  // emitter wall-clock
	float emit_delay_remaining = 0.0f; // counts down from def.emit_delay before any spawn
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

// rand10 normalized to [0, 1).
float emitter_rand_unit(Emitter &e) noexcept;

// rand10 normalized to [-1, 1).
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

// Number of currently-alive particles.
inline std::size_t emitter_alive_count(const Emitter &e) noexcept {
	return e.particles.size();
}

} // namespace opennova::particle
