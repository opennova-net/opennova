#pragma once

// Portable particle frame compilation. This is the seam between an immutable
// simulation snapshot and an embedding renderer. It owns ordering and packet
// formation; there are no Godot or particle-simulator dependencies.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace renderer {

struct ParticleVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// Snapshot bounds are authoritative for cross-emitter sorting. The compiler
// separately reports exact bounds of the emitted quad vertices.
struct ParticleAabb {
	ParticleVec3 min{};
	ParticleVec3 max{};
	bool valid = false;
};

enum class ParticleRenderDomain : std::uint8_t {
	World = 0,
	FirstPerson = 1,
};

// Exact PTL graphic type values. Retail atlas pages are partitioned by type.
enum class ParticlePipeline : std::uint8_t {
	Blend = 0,
	Additive = 1,
	Premult = 2,
	Bump = 3,
	Mod = 4,
	Mod2x = 5,
	Bumpadd = 6,
	Distort = 7,
};

enum class ParticleRenderPass : std::uint8_t {
	Color = 0,
	Distortion = 1,
};

enum class ParticleAlignment : std::uint8_t {
	CameraFacing = 0,
	WorldOriented = 1,
};

struct ParticleUvRect {
	float u_min = 0.0f;
	float v_min = 0.0f;
	float u_max = 1.0f;
	float v_max = 1.0f;
};

// Exact metadata for one resolved flip frame. Type remains separate from
// pipeline because retail atlas pages are partitioned by exact type.
struct ParticleAtlasRegion {
	std::uint32_t page = 0;
	std::uint8_t type = 0;
	ParticleUvRect rect{};
	float inset_u = 0.0f;
	float inset_v = 0.0f;
};

struct ParticleDrawState {
	ParticleRenderPass pass = ParticleRenderPass::Color;
	ParticlePipeline pipeline = ParticlePipeline::Blend;
	ParticleAtlasRegion atlas{};
	// Catalog-defined specialization; part of run identity.
	std::uint16_t variant = 0;
};

// D3DFVF_XYZ | DIFFUSE | SPECULAR | TEX1. Colors stay as raw DWORDs.
struct ParticleVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::uint32_t primary_color = 0xffffffffu;
	std::uint32_t secondary_color = 0xffffffffu;
	float u = 0.0f;
	float v = 0.0f;
};

static_assert(sizeof(ParticleVertex) == 28, "particle vertex must retain retail stride");
static_assert(std::is_standard_layout<ParticleVertex>::value,
		"particle vertex must be directly uploadable");
static_assert(std::is_trivially_copyable<ParticleVertex>::value,
		"particle vertex must be directly uploadable");

// Input vector order is the stable registration/emission order and is retained
// when sort depths tie. Positive camera_pull moves the center camera-ward.
struct ParticleQuadSnapshot {
	ParticleVec3 center{};
	float half_width = 0.0f;
	float half_height = 0.0f;
	float roll = 0.0f;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float camera_pull = 0.0f;
	std::uint32_t primary_color = 0xffffffffu;
	std::uint32_t secondary_color = 0xffffffffu;
	ParticleAlignment alignment = ParticleAlignment::CameraFacing;
	ParticleDrawState state{};
	bool visible = true;
};

struct ParticleEmitterSnapshot {
	std::uint64_t emitter_id = 0;
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	ParticleAabb bounds{};
	std::vector<ParticleQuadSnapshot> particles;
};

struct ParticleFrameSnapshot {
	std::uint64_t frame_id = 0;
	std::vector<ParticleEmitterSnapshot> emitters;
};

// Orthonormal camera basis in world space. forward points in the viewing
// direction; the compiler deliberately does not normalize producer input.
struct ParticleViewInput {
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	ParticleVec3 position{};
	ParticleVec3 right{1.0f, 0.0f, 0.0f};
	ParticleVec3 up{0.0f, 1.0f, 0.0f};
	ParticleVec3 forward{0.0f, 0.0f, 1.0f};
};

struct ParticleDrawCommand {
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	ParticleRenderPass pass = ParticleRenderPass::Color;
	ParticlePipeline pipeline = ParticlePipeline::Blend;
	std::uint32_t atlas_page = 0;
	std::uint8_t atlas_type = 0;
	std::uint16_t variant = 0;
	std::uint32_t first_quad = 0;
	std::uint32_t quad_count = 0;
};

struct ParticleEmitterDrawBounds {
	std::uint64_t emitter_id = 0;
	ParticleAabb bounds{};
	// Quads from overlapping emitters can interleave. first_quad is this
	// emitter's first occurrence in the packet; quad_count is its total.
	// An emitter with no emitted quads leaves both fields at zero.
	std::uint32_t first_quad = 0;
	std::uint32_t quad_count = 0;
};

// Value-based diagnostics let F3 and structural CI tests stay outside renderer
// internals. A capacity growth is one explicit reserve of a retained vector.
struct ParticleFrameDebugCounters {
	std::uint64_t compile_index = 0;
	std::size_t input_emitters = 0;
	std::size_t selected_emitters = 0;
	std::size_t input_particles = 0;
	std::size_t domain_filtered_particles = 0;
	std::size_t invisible_particles = 0;
	std::size_t truncated_particles = 0;
	std::size_t emitted_quads = 0;
	std::size_t draw_commands = 0;
	std::size_t adjacent_state_merges = 0;
	std::size_t capacity_growths_this_compile = 0;
	std::uint64_t lifetime_capacity_growths = 0;
	std::size_t vertex_capacity = 0;
	std::size_t command_capacity = 0;
	std::size_t emitter_bounds_capacity = 0;
	std::size_t emitter_sort_capacity = 0;
	std::size_t particle_sort_capacity = 0;
};

struct ParticleDrawPacket {
	std::uint64_t frame_id = 0;
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	std::vector<ParticleVertex> vertices;
	std::vector<ParticleDrawCommand> commands;
	std::vector<ParticleEmitterDrawBounds> emitter_bounds;
	ParticleFrameDebugCounters debug{};
};

// Deep in-process module: one call filters a domain, globally orders visible
// particles by depth, builds quads, forms adjacent state runs, calculates
// bounds, and accounts for retained allocations.
//
// The returned packet remains valid until the next compile call. Reusing one
// compiler per render domain makes no-allocation-after-warmup observable.
class ParticleFrameCompiler {
public:
	ParticleFrameCompiler();
	~ParticleFrameCompiler();
	ParticleFrameCompiler(ParticleFrameCompiler &&) noexcept;
	ParticleFrameCompiler &operator=(ParticleFrameCompiler &&) noexcept;

	ParticleFrameCompiler(const ParticleFrameCompiler &) = delete;
	ParticleFrameCompiler &operator=(const ParticleFrameCompiler &) = delete;

	const ParticleDrawPacket &compile(const ParticleFrameSnapshot &snapshot,
			const ParticleViewInput &view);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace renderer
