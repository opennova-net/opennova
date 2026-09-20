#pragma once

// Portable Q3 frame compilation. This is the seam between device presenters
// and the focused beauty-depth Q3 adapter: producers publish generation-bound
// resource leases plus plain values; the compiler validates, orders, and owns
// one immutable draw list until the next compile. No Godot or RenderingDevice
// facts cross this interface.

#include <runtime/renderer/material_classify.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// These are the five witnessed draws in FrameFX's bloom-source bracket:
// glow-capable object duplicates first, followed by the NV water redraw,
// celestial discs, and the occlusion-independent sun glow. They draw into
// FrameFX's altbuffer, a backbuffer-sized render-target texture in the
// display format (an X8R8G8B8 display gets an A8R8G8B8 altbuffer) with the
// beauty depth-stencil still bound; the compiled list is that altbuffer's
// content.
// [orig: CRenderBatchQueue_SortAndFlush(4) @ 0x582a54;
// render_water_surface(view, 1) @ 0x582a62;
// render_celestial_bodies(1) / render_skybox_sun_glow(0, 0) @ 0x582a77;
// FrameFX_CreateAltBufferTexture @ 0x582120 (format 22 -> 21 @ 0x582141,
// CreateTexture @ 0x58217e)].
enum class Q3Technique : std::uint8_t {
	NormalCopy = 0,
	RotatedSpecularGlass = 1,
	WaterNightVision = 2,
	CelestialBody = 3,
	SunGlow = 4,
	Count = 5,
};

// LightScene coronas are deliberately named here only so a producer cannot
// silently submit them as sun glow. They are ordinary world additive draws
// and the current retail-backed shader explicitly discards them from Q3.
enum class Q3Source : std::uint8_t {
	Object = 0,
	Water = 1,
	CelestialBody = 2,
	SunGlow = 3,
	LightCorona = 4,
};

enum class Q3GeometryKind : std::uint8_t {
	Rigid = 0,
	StaticInstances = 1,
};

// Whether a model's strips can publish Q3 copies at all. Only the rigid
// object path collects the glow-capable duplicate; a per-vertex skinned
// (bone-path) model never does, whatever its materials' capability words
// say, so the bone path is never a Q3 producer. A Q3 source is therefore
// always rigid geometry under one transform (or a MultiMesh population);
// no producer carries a bone palette.
// [orig: Render_SubmitEntity @ 0x5dade0 (modelData+16 & 1 selects the bone
//  path); collect_render_batches_for_entity @ 0x5d94b0 appends only the
//  opaque list and the Q1/Q2 alpha queues (@ 0x5d97ca, @ 0x5d983b); the Q3
//  copy @ 0x5d93b5..0x5d9447 lives only in collect_render_objects_for_batch
//  @ 0x5d8f20]
inline constexpr bool q3_object_source_admitted(bool skinned_mesh) {
	return !skinned_mesh;
}

// Glass.fx's GLOW technique samples the static CubeRotSpecular cube through
// the sun-aligned MatRotSpecular. Render_FillStaticCubemaps builds that cube
// from two coincident -Z lobes, a white pow-800 lobe at intensity 1.4 and a
// warm (1, 248/255, 240/255) pow-40 lobe at intensity 1.0, summed and
// clamped; the adapter evaluates that generator analytically against the
// live sun direction. [orig: Glass.fx TGlassFFP TECHNIQUE_GLOW;
// Render_FillStaticCubemaps @ 0x58f290; generate_cubemap_lighting
// @ 0x685bb0; apply_shader_parameters @ 0x58e14b (the MatRotSpecular
// upload)].
inline constexpr float kQ3GlassWhiteLobeGain = 1.4f;
inline constexpr float kQ3GlassWhiteLobePower = 800.0f;
inline constexpr std::array<float, 3> kQ3GlassWarmLobeColor{
	1.0f, 248.0f / 255.0f, 240.0f / 255.0f};
inline constexpr float kQ3GlassWarmLobePower = 40.0f;

// The NV water redraw's bright pass: Water_PSBumpReflectNV keeps
// saturate(luma(0.25, 0.60, 0.15)^2 - 0.15) of the reflected colour, with
// the device fog colour forced black. [orig: render_water_surface(view, 1)
// @ 0x5c3311..0x5c3320 (detail gate), @ 0x5c3442..0x5c3458
// (SetFogAndBlendMode(2) + Water_ShaderBlendNV); Water_PSBumpReflectNV
// source @ 0x7dbd28, assembled in Water_InitSurfaceShaders @ 0x5c1bc4; NV
// descriptors @ 0x5c1c27..0x5c1c81 (blend 1/2/5 and opaque 0x20000);
// CD3DDevice_SetFogAndBlendMode @ 0x677740 case 2 @ 0x6778ed].
inline constexpr std::array<float, 3> kQ3WaterNvLumaWeights{
	0.25f, 0.60f, 0.15f};
inline constexpr float kQ3WaterNvBrightBias = 0.15f;

// FrameFX's bloom pass draws the celestial discs and the sun glow through
// Render_SetViewportFarDepth: a D3DVIEWPORT9 with MinZ 0.98 / MaxZ
// 0.99996948 remaps their clip depth into that far band before the ordinary
// z-tested flush, so both survive only where the beauty depth is at (or
// within the band of) the far plane, i.e. cleared sky. Every nearer surface
// occludes them in the bloom source. In Godot's reverse-Z [0, 1] clip depth
// the band is [1 - MaxZ, 1 - MinZ]: z' = (1 - MaxZ) + z * (MaxZ - MinZ).
// [orig: FrameFX_RenderBloomPass @ 0x582940 (Render_SetViewportFarDepth
// @ 0x582a70 -> render_celestial_bodies(1) @ 0x582a77 ->
// render_skybox_sun_glow(0, 0) @ 0x582a80); Render_SetViewportFarDepth
// @ 0x58a840 (MinZ 0.98000002 @ 0x58a859, MaxZ 0.99996948 @ 0x58a86b); the
// z-tested flushes CRenderBatchQueue_SortAndFlush(0) @ 0x5acce9 / 0x5ad118].
inline constexpr float kQ3FarBandMinZ = 0.98000002f;
inline constexpr float kQ3FarBandMaxZ = 0.99996948f;

// An opaque portable identity, not a GPU handle. The adapter resolves the
// resource by its id. A geometry lease names the adapter's cache entry and
// the packed generation the producer saw; the snapshot publishes the current
// generation of every leased geometry resource (`resource_generations`) and
// the compiler rejects a submission whose lease no longer matches it
// (StaleResourceLease), so a frame never draws a stream its owner has since
// re-packed or evicted. Material and texture leases are identity-only and
// minted at generation 1.
struct Q3ResourceLease {
	std::uint64_t resource_id = 0;
	std::uint64_t generation = 0;

	bool valid() const { return resource_id != 0 && generation != 0; }
};

// The current generation of one leased resource, as published by its owner
// for this frame. A resource absent from the table is not generation-checked.
struct Q3ResourceGeneration {
	std::uint64_t resource_id = 0;
	std::uint64_t generation = 0;
};

struct Q3Vec2 {
	float x = 0.0f;
	float y = 0.0f;
};

struct Q3Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Q3Vec4 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 0.0f;
};

// Column-major transform matrix, directly convertible at the adapter.
struct Q3Matrix4 {
	std::array<float, 16> values{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
	};
};

// Only values needed by the focused object Q3 techniques live here. The LUM
// GLOW slot is a copy of the NORMAL pass block, so NormalCopy re-shades the
// SELFLUM specialization from the leased Diffuse1/Detail textures and
// `self_lum_color` (the material's RGB modulator) with alpha 0; Glass's Q3
// technique uses ReflectColor and the global sun-aligned analytic
// CubeRotSpecular lobe. Neither consumes `alpha_mod` (the SELFLUM and glass
// wrappers ignore it); it is snapshotted for the coverage contract only.
// [orig: _FFP.fx LUM GLOW copy @ 0x5afc7f; Glass.fx TECHNIQUE_GLOW].
struct Q3ObjectMaterialParameters {
	ObjectMaterialClassification classification{};
	Q3ResourceLease base_texture{};
	Q3ResourceLease detail_texture{};
	Q3Vec4 self_lum_color{1.0f, 1.0f, 1.0f, 1.0f};
	Q3Vec4 reflect_color{0.7f, 0.8f, 0.9f, 0.35f};
	float alpha_mod = 1.0f;
	std::array<float, 9> uv_transform{
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};
};

// Dynamic water inputs consumed by Water_PSBumpReflectNV. Per-vertex diffuse,
// specular, fog, and projective rows remain in the leased geometry.
struct Q3WaterMaterialParameters {
	Q3ResourceLease reflection_texture{};
	Q3ResourceLease noise_color_texture{};
	Q3ResourceLease noise_normal_texture{};
	Q3Vec3 water_color{0.408f, 0.314f, 0.224f};
	Q3Vec4 water_uv{1.0f, 0.2f, 0.0f, 0.0f};
	Q3Vec2 reflection_uv_scale{1.0f, 1.0f};
	bool has_reflection = false;
	bool underwater_view = false;
};

// Shared parameter block for the body and sun-glow techniques. `opacity` is
// already the producer's pass-specific value: body opacity for CelestialBody,
// or glare_q3_peak_opacity for SunGlow.
struct Q3CelestialMaterialParameters {
	Q3ResourceLease diffuse_texture{};
	Q3Vec3 tint{1.0f, 1.0f, 1.0f};
	float opacity = 1.0f;
	Q3Vec3 glare_direction{0.0f, 1.0f, 0.0f};
	bool additive = false;
	bool glare_view_fade = false;
};

// One producer row. Input order is retail submission order. Ranges address
// Q3FrameSnapshot's flat arrays and are copied/remapped into the draw list.
struct Q3SubmissionSnapshot {
	std::uint64_t submission_id = 0;
	Q3Source source = Q3Source::Object;
	Q3GeometryKind geometry_kind = Q3GeometryKind::Rigid;
	Q3ResourceLease geometry{};
	Q3ResourceLease material{};
	std::uint32_t surface_index = 0;
	float view_depth = 0.0f;
	std::uint32_t submit_flags = 0;
	std::size_t first_transform = 0;
	std::size_t transform_count = 0;
	Q3ObjectMaterialParameters object{};
	Q3WaterMaterialParameters water{};
	Q3CelestialMaterialParameters celestial{};
	bool visible = true;
};

struct Q3FrameSnapshot {
	std::uint64_t frame_id = 0;
	std::uint64_t scene_generation = 0;
	std::vector<Q3SubmissionSnapshot> submissions;
	std::vector<Q3Matrix4> transforms;
	std::vector<Q3ResourceGeneration> resource_generations;
};

enum class Q3RejectReason : std::uint8_t {
	GlowCopySuppressed = 0,
	UnsupportedObjectMaterial = 1,
	UnsupportedSource = 2,
	InvalidResourceLease = 3,
	InvalidTransformRange = 4,
	NonFiniteInput = 5,
	StaleResourceLease = 6,
};

struct Q3RejectedSubmission {
	std::uint64_t submission_id = 0;
	std::size_t input_index = 0;
	Q3RejectReason reason = Q3RejectReason::UnsupportedSource;
};

struct Q3DrawCommand {
	std::uint64_t submission_id = 0;
	std::size_t input_index = 0;
	Q3Technique technique = Q3Technique::NormalCopy;
	Q3GeometryKind geometry_kind = Q3GeometryKind::Rigid;
	Q3ResourceLease geometry{};
	Q3ResourceLease material{};
	std::uint32_t surface_index = 0;
	std::uint32_t sort_key = 0;
	std::uint32_t first_transform = 0;
	std::uint32_t transform_count = 0;
	Q3ObjectMaterialParameters object{};
	Q3WaterMaterialParameters water{};
	Q3CelestialMaterialParameters celestial{};
};

struct Q3FrameDebugCounters {
	std::size_t invisible_submissions = 0;
	std::size_t emitted_submissions = 0;
	std::size_t rejected_submissions = 0;
	std::size_t invalid_resource_submissions = 0;
	std::size_t stale_lease_submissions = 0;
	std::size_t unsupported_light_coronas = 0;
	std::array<std::size_t, static_cast<std::size_t>(Q3Technique::Count)>
			emitted_by_technique{};
};

struct Q3DrawList {
	std::uint64_t frame_id = 0;
	std::uint64_t scene_generation = 0;
	std::vector<Q3DrawCommand> commands;
	std::vector<Q3Matrix4> transforms;
	std::vector<Q3RejectedSubmission> rejected;
	Q3FrameDebugCounters debug{};
};

// Deep in-process module: one call validates every resource/range, derives
// object techniques from the authoritative material classification, applies
// the witnessed FrameFX bracket and Q3 back-to-front key, and snapshots every
// value needed by the device adapter.
//
// The result remains valid until the next compile call. Unsupported inputs are
// omitted and reported; the compiler never guesses a technique.
class Q3FrameCompiler {
public:
	const Q3DrawList &compile(const Q3FrameSnapshot &snapshot);
	const Q3DrawList &draw_list() const { return draw_list_; }

private:
	struct PreparedSubmission {
		std::size_t input_index = 0;
		Q3Technique technique = Q3Technique::NormalCopy;
		std::uint32_t sort_key = 0;
	};

	Q3DrawList draw_list_{};
	std::vector<PreparedSubmission> prepared_;
};

} // namespace opennova::renderer
