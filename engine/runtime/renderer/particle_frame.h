#pragma once

// Portable particle frame compilation. This is the seam between an immutable
// simulation snapshot and an embedding renderer. It owns ordering and draw list
// formation; there are no Godot or particle-simulator dependencies.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace opennova::renderer {

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

// The effect world's particle passes [orig: CParticleGroup_RenderChildren
// @ 0x5E5890 over its renderFlags]. The two scene passes classify the emitter
// origin against the water split plane, not each generated quad: Below (flag 1)
// is strict, equality belongs to Above (flag 2). A child whose def leads with
// a Distort graphic (def class 7) draws in neither; it draws only in the
// post-scene distortion pass (flag 4) [orig: the `== 7` test @ 0x5E58D2 and the
// `& 4` arm @ 0x5E58DB..0x5E58EE]. All is reserved for domains/passes that do
// not use the water partition.
enum class ParticleWaterSubset : std::uint8_t {
	All = 0,
	Below = 1,
	Above = 2,
	Distortion = 3,
};

// Converts the frame-level far/camera-side bracket into the manager's raw
// below/above selector. Underwater views reverse the two submissions.
ParticleWaterSubset particle_water_subset_for_side(bool camera_above_water,
		bool camera_side);

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

// The material a draw binds in a thermal-view frame. The batch flush binds
// each texture's secondary material (sample+8) instead of its primary
// (sample+4) while the effect world's thermal word is set; that word is the
// main scene's thermal byte, zero in every other scene call
// [orig: CParticleBatch_FlushAndBindMaterial @ 0x5E42BF..0x5E42DB; the store
// EffectWorld_RenderParticlePass @ 0x5F7274]. Blend's secondary inverts
// colour: MODULATE(1 - TEXTURE, 1 - DIFFUSE) under SRCALPHA/INVSRCALPHA;
// Additive's and Premult's darken: MODULATE(TEXTURE, DIFFUSE) under
// ZERO/INVSRCCOLOR; every other type's secondary is its primary
// [orig: CParticleTexture_InitTextureAndChannels — case 0 @ 0x5E833D with the
// shared tail @ 0x5E8584..0x5E85DD, cases 1/2 @ 0x5E8376..0x5E83AC, the
// primary copies @ 0x5E8422 / @ 0x5E847C / @ 0x5E8508, Distort's one
// channel @ 0x5E860B].
enum class ParticleThermalMaterial : std::uint8_t {
	Primary = 0,
	InvertedBlend = 1,
	DarkeningModulate = 2,
};
ParticleThermalMaterial particle_thermal_material(ParticlePipeline pipeline, bool thermal);

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

// One quad's inputs. Positive camera_pull moves the center camera-ward.
//
// Input order contract: the compiler never reorders a producer's particle run
// before the witnessed per-leaf quicksort, so the run must already be the
// emitter's dense particle array order. In retail that array is spawn order
// perturbed by expiry: SpawnParticle appends at slot `count`
// [orig: CParticleEmitter_SpawnParticle @ 0x5e80c7], AdvanceFrame reclaims an
// expired slot by copying the LAST live particle into it and shrinking the
// count [orig: CParticleEmitter_AdvanceFrame @ 0x5e687e, 0x5e68d9], and
// ComputeViewDepths then walks slots 0..count-1 into the batch buffer
// [orig: CParticleEmitter_ComputeViewDepths @ 0x5e75cf]. The manager's in-place
// quicksort consumes exactly that order (including its equal-key swaps).
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
	// The particle's serial byte (+0), which the density stride reads
	// (ParticleViewInput::lod_divisor).
	std::uint8_t serial = 0;
};

struct ParticleEmitterSnapshot {
	std::uint64_t emitter_id = 0;
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	// The emitter's live world origin is the water-pass selector. Bounds and
	// individual particle positions deliberately do not participate.
	ParticleVec3 position{};
	// The def's first graphic is Distort (the child's class id +0x1E0 == 7):
	// the emitter draws only in the distortion pass.
	bool distortion_class = false;
	// The owning group's building-section gate (EffectSectionGate, the group's
	// +0x6C): a hidden group draws in no pass.
	bool group_visible = true;
	ParticleAabb bounds{};
	// This emitter's run inside ParticleFrameSnapshot::particles. A run that
	// overruns the flat array is clamped by the compiler.
	std::size_t first_particle = 0;
	std::size_t particle_count = 0;
};

// Flat frame input. Emitters are in retail registration order and every
// emitter's quads form one contiguous run, so a producer refills both vectors
// with clear() each frame and keeps their capacity instead of rebuilding one
// inner vector per emitter.
struct ParticleFrameSnapshot {
	std::uint64_t frame_id = 0;
	std::vector<ParticleEmitterSnapshot> emitters;
	std::vector<ParticleQuadSnapshot> particles;
};

// Orthonormal camera basis in world space. forward points in the viewing
// direction; the compiler deliberately does not normalize producer input.
struct ParticleViewInput {
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	ParticleWaterSubset water_subset = ParticleWaterSubset::All;
	float water_height = 0.0f;
	ParticleVec3 position{};
	ParticleVec3 right{1.0f, 0.0f, 0.0f};
	ParticleVec3 up{0.0f, 1.0f, 0.0f};
	ParticleVec3 forward{0.0f, 0.0f, 1.0f};
	// Column-major camera projection. Retail projects each emitter AABB's
	// bounding sphere into viewport space before its z/x/y recursive interval
	// partition [orig: CEffectWorld_RegisterRenderObject @ 0x5e44c0;
	// CParticleManager_TransformToViewSpace @ 0x5ecc50]. Godot's reverse-Z
	// projection already has near=1/far=0, matching retail after its `1-z`.
	float projection[16]{};
	bool projection_valid = false;
	bool projection_near_is_one = true;
	// The scene pass's density stride (renderer/particle_density.h
	// particle_lod_divisor): a quad is built only for a particle whose serial
	// byte modulo the stride is 0, after the batch's depth sort; 1 builds every
	// particle.
	// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5E6DA2..0x5E6E06;
	//  CParticleEmitter_RenderStaticBillboards @ 0x5F4E82..0x5F4EC2;
	//  CParticleEmitter_RenderTopAlignedBillboards @ 0x5F5681..0x5F56E6]
	std::uint32_t lod_divisor = 1;
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
	// emitter's first occurrence in the draw list; quad_count is its total.
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
	// Emitters of groups the building-section gate hides this frame.
	std::size_t section_hidden_emitters = 0;
	std::size_t water_filtered_emitters = 0;
	std::size_t water_filtered_particles = 0;
	std::size_t invisible_particles = 0;
	// Particles the view's density stride left unbuilt.
	std::size_t lod_skipped_particles = 0;
	std::size_t truncated_particles = 0;
	std::size_t emitted_quads = 0;
	std::size_t draw_commands = 0;
	std::size_t adjacent_state_merges = 0;
	std::size_t recursive_partition_calls = 0;
	std::size_t render_batch_leaves = 0;
	std::size_t capacity_growths_this_compile = 0;
	std::uint64_t lifetime_capacity_growths = 0;
	std::size_t vertex_capacity = 0;
	std::size_t command_capacity = 0;
	std::size_t emitter_bounds_capacity = 0;
	// Retained explicit-recursion stack shared by every quicksort of a compile.
	std::size_t sort_stack_capacity = 0;
};

struct ParticleDrawList {
	std::uint64_t frame_id = 0;
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	std::vector<ParticleVertex> vertices;
	std::vector<ParticleDrawCommand> commands;
	std::vector<ParticleEmitterDrawBounds> emitter_bounds;
	ParticleFrameDebugCounters debug{};
};

// Deep in-process module: one call filters a domain, reproduces retail's
// projected z/x/y recursive emitter partition and per-leaf particle quicksort,
// builds quads, forms adjacent state runs, calculates bounds, and accounts for
// retained allocations.
//
// The returned draw list remains valid until the next compile call. Reusing one
// compiler per render domain makes no-allocation-after-warmup observable.
class ParticleFrameCompiler {
public:
	ParticleFrameCompiler();
	~ParticleFrameCompiler();
	ParticleFrameCompiler(ParticleFrameCompiler &&) noexcept;
	ParticleFrameCompiler &operator=(ParticleFrameCompiler &&) noexcept;

	ParticleFrameCompiler(const ParticleFrameCompiler &) = delete;
	ParticleFrameCompiler &operator=(const ParticleFrameCompiler &) = delete;

	const ParticleDrawList &compile(const ParticleFrameSnapshot &snapshot,
			const ParticleViewInput &view);

	// The retained result of the most recent compile (default-empty before the
	// first). Diagnostics read it in place instead of copying it per frame.
	const ParticleDrawList &draw_list() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace opennova::renderer