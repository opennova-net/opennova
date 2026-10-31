#include "render/slot_capture_adapter.h"
#include "object/object_model.h"
#include "object/post_multiply_draw.h"
#include "render/q3_geometry_cache.h"
#include "render/retained_array_mesh.h"
#include "render/material_params.h"
#include "render/q3_source_registry.h"
#include "render/q3_vertex_format.h"
#include "render/rd_glsl.h"
#include "render/rd_pipeline_state.h"
#include "render/rd_timestamp_span.h"
#include "render/rd_uniforms.h"
#include "render/texture_filter_device.h"
#include "util/string_convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/renderer/q3_frame.h>
#include <runtime/renderer/render_slot_shadow.h>

using namespace godot;
using namespace opennova::renderer;

namespace {

// mat4 + vec4: the per-draw block below.
constexpr std::uint32_t kPushConstantBytes = 96u;
// The push flag bits (params.z) the shaders decode are the engine's
// renderer::SlotCaptureCommandFlag (render_slot_shadow.h); the shaders below
// spell the same values.
static_assert(kSlotCaptureAlphaTest == 1u && kSlotCaptureAlphaInvert == 2u &&
				kSlotCaptureAlphaMod == 4u && kSlotCaptureDetailAlpha == 8u &&
				kSlotCaptureSkinned == 16u && kSlotCaptureBlendCoverage == 32u,
		"the capture shaders decode the engine's command flag bits");
constexpr std::uint32_t kBoneMatrixBytes = 64u;

// The black PROJSHAD pass over the retail 0x00FFFFFF clear: the geometry
// arrives in the Q3GeometryCache layout (q3_geometry_cache.h, 104-byte
// vertices) with the bind-space positions and, for skinned surfaces, the bone
// indices in CUSTOM0 and weights in CUSTOM1 (Q3PackParameters::skin_channels),
// skinned here from the frame's bone palette (a storage buffer of
// skeleton-space bone x bind matrices, exactly the CPU palette Q3 blends) by
// retail's blend as the mesh carries it: the four weights as they are, the
// fourth being 1 - (w0 + w1 + w2), never renormalized (the witness is cited
// where the engine lays the weights out, renderer::prepare_model_mesh).
// The witness (vscPostBlackT1 / vscSkinPostBlackT1's black diffuse inside
// RenderSlot_RenderEntityAndChildren) is cited on the engine's
// render_slot_shadow.h; this shader only carries the device layout.
const char *kSlotCaptureVertexShader = R"GLSL(#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec4 in_custom0;
layout(location = 5) in vec4 in_custom1;
layout(location = 6) in vec4 in_custom2;
layout(location = 7) in vec2 in_uv2;

layout(push_constant, std430) uniform SlotPush {
	mat4 mvp;
	vec4 params;
	vec4 max_lods;
} pc;

layout(set = 1, binding = 0, std430) readonly buffer BonePalette {
	mat4 bones[];
} palette;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec2 uv2;

void main() {
	vec3 position = in_position;
	uint flags = uint(pc.params.z + 0.5);
	if ((flags & 16u) != 0u) {
		uint base = uint(pc.params.w + 0.5);
		vec4 bind = vec4(in_position, 1.0);
		// The four weights exactly as the mesh carries them (the fourth,
		// possibly a hair negative, is 1 - (w0 + w1 + w2)), never renormalized.
		// A zero weight adds nothing, so its bone is not read.
		vec3 skinned = vec3(0.0);
		for (int influence = 0; influence < 4; influence++) {
			float weight = in_custom1[influence];
			if (weight == 0.0) {
				continue;
			}
			uint bone = base + uint(in_custom0[influence] + 0.5);
			skinned += (palette.bones[bone] * bind).xyz * weight;
		}
		position = skinned;
	}
	gl_Position = pc.mvp * vec4(position, 1.0);
	uv = in_uv;
	uv2 = in_uv2;
}
)GLSL";

// The PROJSHAD coverage per technique (the engine's
// object_projected_shadow_coverage table): coverage = Diffuse1.a, x Detail.a
// over the transformed UV2 for the _MT FFP blocks, x u_alpha_mod
// (AlphaGenValue) for the FFP families; alpha-test materials keep coverage
// > ref (invert: <= ref) and discard the rest. Every kept fragment writes
// black; its alpha is 1 for the opaque state (ONE/ZERO replaces) and the
// coverage itself for the _FFP alpha-blend variant, whose SRCALPHA/
// INVSRCALPHA pipeline blends black by Diffuse1.a x AlphaGenValue (x
// Diffuse2.a) over the white clear (the _FFP.fx TBoringFFPProjShad blend
// law the engine's object_shader_template.h cites beside its
// ObjectProjectedShadowPolicy::MaterialBlend). The no-pass techniques never
// reach the device (skipped at compile). The engine header carries the
// witness for the per-technique table, the blend state and the alpha-test
// compare; this shader only evaluates them.
const char *kSlotCaptureFragmentShaderTemplate = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D diffuse_texture;
layout(set = 0, binding = 1) uniform sampler2D detail_texture;

layout(push_constant, std430) uniform SlotPush {
	mat4 mvp;
	vec4 params;
	vec4 max_lods;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec2 uv2;
layout(location = 0) out vec4 frag_color;

// TBoringFFPProjShad samples with sampLinearWrap2D (rd_glsl.h
// kGlslSampleEffectStage) under the effects' filter code (max_lods.z),
// clamped at the stage texture's last retail mip level (max_lods.x Diffuse1,
// max_lods.y Diffuse2).
@SAMPLE_EFFECT_STAGE@

void main() {
	uint flags = uint(pc.params.z + 0.5);
	uint filter_code = uint(pc.max_lods.z + 0.5);
	float coverage = rd_sample_effect_stage(diffuse_texture, uv, pc.max_lods.x,
			filter_code).a;
	if ((flags & 8u) != 0u) {
		coverage *= rd_sample_effect_stage(detail_texture, uv2, pc.max_lods.y,
				filter_code).a;
	}
	if ((flags & 4u) != 0u) {
		coverage *= pc.params.y;
	}
	if ((flags & 1u) != 0u) {
		bool passes = (flags & 2u) != 0u ? coverage <= pc.params.x :
				coverage > pc.params.x;
		if (!passes) {
			discard;
		}
	}
	float alpha = (flags & 32u) != 0u ? clamp(coverage, 0.0, 1.0) : 1.0;
	frag_color = vec4(0.0, 0.0, 0.0, alpha);
}
)GLSL";

std::string slot_capture_fragment_shader_source() {
	std::string source(kSlotCaptureFragmentShaderTemplate);
	splice_token(source, "@SAMPLE_EFFECT_STAGE@", kGlslSampleEffectStage);
	return source;
}

struct SlotPush {
	std::array<float, 16> mvp{};
	std::array<float, 4> params{};
	std::array<float, 4> max_lods{};
};

static_assert(sizeof(SlotPush) == kPushConstantBytes);

struct DeviceCommand {
	std::shared_ptr<const Q3PackedStream> stream;
	std::uint32_t first_transform = 0;
	std::uint32_t transform_count = 0;
	std::uint32_t first_bone = 0;
	std::uint32_t bone_count = 0;
	// Keep the sampled server resources alive across the compile -> draw
	// handoff; the RID is only the device lookup key.
	Ref<Texture2D> diffuse_resource;
	Ref<Texture2D> detail_resource;
	RID diffuse;
	RID detail;
	float alpha_test_value = 0.0f;
	float alpha_mod = 1.0f;
	// The stages' last retail mip levels (texture_path_resolver
	// texture_max_lod; kQ3NoMipCeiling = unbounded).
	float diffuse_max_lod = kQ3NoMipCeiling;
	float detail_max_lod = kQ3NoMipCeiling;
	std::uint32_t flags = 0;
	// The PROJSHAD pass's device state (renderer::object_projected_shadow_pipeline_state):
	// the cull per two-sidedness, SRCALPHA/INVSRCALPHA over the clear for the _FFP
	// alpha-blend variant, else the opaque replace.
	FixedFunctionState state{};
};

struct DeviceCapture {
	int order = 0;
	int size = 0;
	RID target;
	Projection view_projection;
	std::vector<DeviceCommand> commands;
};

struct DeviceFrame {
	std::uint64_t frame_id = 0;
	std::vector<DeviceCapture> captures;
	std::vector<Transform3D> transforms;
	std::vector<Transform3D> bones;
	std::vector<std::uint64_t> evicted_entries;
};

// The skeleton-space bone x bind palette of a skinned instance (the same
// palette the Q3 adapter CPU-skins with, and the one a skinned effect's strip
// is posed with in the object shaders); empty for an unskinned instance.
std::vector<Transform3D> skin_palette(MeshInstance3D *p_instance) {
	std::vector<Transform3D> result;
	if (p_instance == nullptr)
		return result;
	if (p_instance->get_skeleton_path().is_empty()) {
		// A skinned effect's strip rides its model's bone palette, not a skin
		// binding (object_model_skin_palette.cpp).
		ObjectModel::skin_palette_of(p_instance, result);
		return result;
	}
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(
			p_instance->get_node_or_null(p_instance->get_skeleton_path()));
	const Ref<Skin> skin = p_instance->get_skin();
	if (skeleton == nullptr || skin.is_null() || skin->get_bind_count() <= 0)
		return result;
	result.reserve(skin->get_bind_count());
	for (int bind = 0; bind < skin->get_bind_count(); ++bind) {
		const int bone = skin->get_bind_bone(bind);
		if (bone < 0 || bone >= skeleton->get_bone_count())
			return {};
		result.push_back(skeleton->get_bone_global_pose(bone) *
				skin->get_bind_pose(bind));
	}
	return result;
}

// Every visible drawn instance under a caster: the ObjectModel's LOD/part
// instances, the shared-skeleton skinned strips, and any tree-parented rider
// (retail's seat children render inside the parent's slot). The auxiliary
// duplicates (the BmTxMirrT P3 postmultiply instance) re-submit the same
// strip and carry no classified material; retail's single PROJSHAD pass per
// effect is the registered P0/P1 instance. An ObjectModel counts by the
// engine's rule (renderer::slot_capture_draws: the render claims lifted, its
// present intent kept, a local-view swap drawing its stand-in instead),
// except under a carrier's child walk (`p_walk`), which draws its riders with
// no visibility test (renderer::SlotFrameCapture). Every other node keeps its
// own visible flag (the inactive RLOD levels, hidden parts).
void collect_visible_geometry(Node *p_node,
		std::vector<GeometryInstance3D *> &r_instances, bool p_walk = false) {
	if (p_node == nullptr)
		return;
	if (const ObjectModel *model = Object::cast_to<ObjectModel>(p_node)) {
		if (!p_walk && !slot_capture_draws(slot_draw_visibility(*model)))
			return;
	} else if (const Node3D *spatial = Object::cast_to<Node3D>(p_node)) {
		if (!spatial->is_visible())
			return;
	}
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		if (Object::cast_to<PostMultiplyDraw>(geometry) == nullptr)
			r_instances.push_back(geometry);
	}
	for (int index = 0; index < p_node->get_child_count(); ++index)
		collect_visible_geometry(p_node->get_child(index), r_instances, p_walk);
}

void write_matrix(PackedByteArray &r_bytes, int64_t p_offset,
		const Transform3D &p_transform) {
	std::array<float, 16> values{};
	for (int column = 0; column < 3; ++column) {
		for (int row = 0; row < 3; ++row)
			values[column * 4 + row] = p_transform.basis[row][column];
	}
	values[12] = p_transform.origin.x;
	values[13] = p_transform.origin.y;
	values[14] = p_transform.origin.z;
	values[15] = 1.0f;
	std::memcpy(r_bytes.ptrw() + p_offset, values.data(), sizeof(values));
}

// One caster surface's PROJSHAD admission and its packed stream (the frame's
// compile and the load's warm share it, so a stream the warm packed is the
// one the first capture finds).
enum class SurfaceAdmission { Unclassified, NoPass, NoStream, Admitted };

struct AdmittedSurface {
	Ref<ShaderMaterial> material;
	ObjectMaterialClassification classification;
	// The material's published draw values (Q3SourceRegistry::object_material).
	Q3ObjectMaterialValues values;
	Ref<Texture2D> diffuse;
	Ref<Texture2D> detail;
	// The engine's PROJSHAD draw for the surface (renderer::slot_capture_surface).
	SlotCaptureSurface surface;
	std::shared_ptr<const Q3PackedStream> stream;
};

SurfaceAdmission admit_surface(Q3GeometryCache &r_cache, GeometryInstance3D *p_source,
		const Ref<Mesh> &p_mesh, ArrayMesh *p_array_mesh, int p_surface, bool p_skinned,
		AdmittedSurface &r_surface) {
	const Ref<Material> material = active_material(p_source, p_mesh, p_surface);
	r_surface.material = material;
	Q3ObjectMaterialValues &values = r_surface.values;
	if (r_surface.material.is_null() ||
			!Q3SourceRegistry::object_material(material, r_surface.classification, values)) {
		// An ArrayMesh surface whose material is not a registered
		// object material (no ObjectMaterialClassification): retail
		// submits no PROJSHAD pass for an effect without one, and no
		// coverage source exists to invent one from. Counted as
		// unclassified; a non-ArrayMesh instance (a PrimitiveMesh)
		// never reaches this test and is not counted.
		return SurfaceAdmission::Unclassified;
	}
	r_surface.diffuse = values.diffuse;
	r_surface.detail = values.detail;
	// The engine decides the surface's PROJSHAD draw and its command state
	// (renderer::slot_capture_surface); an unresolved secondary composes the
	// no-detail technique.
	r_surface.surface = slot_capture_surface(r_surface.classification,
			r_surface.detail.is_valid(), p_skinned);
	if (!r_surface.surface.draws) {
		return SurfaceAdmission::NoPass;
	}
	// Keyed on the mesh as well as the node: an ObjectModel surface
	// slot swaps a different ArrayMesh onto the same MeshInstance3D
	// per authored RLOD level (apply_level_surfaces), and the slot
	// pass follows no per-source geometry generation, so each
	// level's arrays are their own retained entry and a crossing
	// selects the level's packed stream instead of drawing the
	// first-seen level's silhouette forever.
	Q3GeometryCache::Request cache_request;
	cache_request.key = {p_source->get_instance_id(), p_surface,
		static_cast<std::uint64_t>(p_array_mesh->get_rid().get_id())};
	cache_request.geometry_generation = 1;
	cache_request.pack.source = Q3Source::Object;
	cache_request.pack.uv_u =
			Vector3(values.runtime.uv.m00, values.runtime.uv.m10, values.runtime.uv.m20);
	cache_request.pack.uv_v =
			Vector3(values.runtime.uv.m01, values.runtime.uv.m11, values.runtime.uv.m21);
	cache_request.pack.skin_channels = p_skinned;
	r_surface.stream = r_cache.acquire(cache_request, [&](bool &r_read_back) {
		return surface_arrays(p_array_mesh, p_surface, r_read_back);
	});
	return r_surface.stream ? SurfaceAdmission::Admitted : SurfaceAdmission::NoStream;
}

} // namespace

namespace godot {

SlotCasterVisibility slot_draw_visibility(const ObjectModel &p_model) {
	SlotCasterVisibility visibility;
	visibility.present_visible = p_model.is_present_visible();
	visibility.draw_visible = p_model.is_visible();
	visibility.render_claimed = p_model.is_occlusion_hidden() || p_model.is_subpixel_hidden();
	visibility.local_view_suppressed = p_model.is_slot_local_view_suppressed();
	return visibility;
}

} // namespace godot

class SlotCaptureAdapter::Impl {
public:
	struct PipelineKey {
		int64_t framebuffer_format = -1;
		// renderer::fixed_function_state_key of the command's state.
		std::uint32_t state = 0;

		bool operator<(const PipelineKey &p_other) const {
			return std::tie(framebuffer_format, state) <
					std::tie(p_other.framebuffer_format, p_other.state);
		}
	};

	// The per-order MSAA colour/depth pair and its framebuffer; the capture
	// resolves into the request's target. Retail's slot RT carries its own z
	// (Render_ShadowPass clears the slot RT per capture), so the pass depth
	// tests and writes inside the target alone.
	struct Target {
		int size = 0;
		RID msaa_color;
		RID msaa_depth;
		RID framebuffer;
	};

	struct DeviceGeometry {
		RID buffer;
		std::uint32_t capacity = 0;
		std::uint64_t uploaded_generation = 0;
	};

	mutable std::mutex frame_mutex;
	std::shared_ptr<const DeviceFrame> published_frame;
	mutable std::mutex diagnostics_mutex;
	std::string status = "waiting_for_frame";
	std::string failure;
	SlotCaptureFrameCounters counters{};
	std::uint64_t submitted_frame_id = 0;
	std::uint64_t drawn_frame_id = 0;
	std::size_t cached_entries = 0;
	std::size_t device_buffers = 0;
	std::array<int, kSlotCaptureCount> compiled_surfaces{};

	Q3GeometryCache geometry_cache;
	std::uint64_t next_frame_id = 1;
	std::map<std::uint64_t, DeviceGeometry> device_geometry;
	std::atomic<std::uint64_t> consumed_frame_id{0};

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	RID fallback_texture;
	RID bone_buffer;
	std::uint32_t bone_buffer_capacity = 0;
	RID bone_uniform;
	int64_t vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	TypedArray<RID> vertex_buffers;
	PackedInt64Array vertex_offsets;
	std::map<PipelineKey, RID> pipelines;
	// Each texture pair's uniform set, kept across frames by the two RD textures it binds:
	// a draw binds its caster's textures as the game's device draw sets them, with no set
	// made and freed per draw per frame (the device lets a set go with a texture it binds,
	// so a kept set is checked before it is bound; D-NET-1353 LF-s18).
	std::map<std::pair<std::uint64_t, std::uint64_t>, RID> texture_uniforms;
	// The draws' push block, one buffer reused for every draw.
	PackedByteArray push_bytes;
	std::array<Target, kSlotCaptureCount> targets{};
	PackedColorArray clear_white;

	~Impl() { discard_device_state(); }

	void set_failure(const std::string &p_failure) {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = "failed";
		failure = p_failure;
	}

	std::shared_ptr<const DeviceFrame> frame_snapshot() const {
		std::lock_guard<std::mutex> lock(frame_mutex);
		return published_frame;
	}

	void publish(std::shared_ptr<const DeviceFrame> p_frame) {
		std::lock_guard<std::mutex> lock(frame_mutex);
		published_frame = std::move(p_frame);
	}

	void release_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid())
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_uniform_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() && rd->uniform_set_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_pipeline_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() && rd->render_pipeline_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_texture_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() && rd->texture_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_framebuffer_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() && rd->framebuffer_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_target(Target &p_target) {
		release_framebuffer_rid(p_target.framebuffer);
		release_texture_rid(p_target.msaa_depth);
		release_texture_rid(p_target.msaa_color);
		p_target = Target();
	}

	void discard_device_state() {
		texture_uniforms.clear();
		push_bytes = PackedByteArray();
		pipelines.clear();
		device_geometry.clear();
		for (Target &target : targets)
			target = Target();
		bone_uniform = RID();
		bone_buffer = RID();
		bone_buffer_capacity = 0;
		fallback_texture = RID();
		sampler = RID();
		shader = RID();
		vertex_buffers.clear();
		vertex_offsets.clear();
		vertex_format = RenderingDevice::INVALID_FORMAT_ID;
		rd = nullptr;
	}

	void release_device_geometry(std::uint64_t p_entry_id) {
		const auto found = device_geometry.find(p_entry_id);
		if (found == device_geometry.end())
			return;
		release_rid(found->second.buffer);
		device_geometry.erase(found);
	}

	void publish_device_buffer_count() {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		device_buffers = device_geometry.size();
	}

	void consume_frame(RenderingDevice *p_rd) {
		const std::shared_ptr<const DeviceFrame> frame = frame_snapshot();
		if (!frame)
			return;
		if (rd != nullptr && rd != p_rd)
			return;
		for (const std::uint64_t entry_id : frame->evicted_entries)
			release_device_geometry(entry_id);
		consumed_frame_id.store(frame->frame_id, std::memory_order_release);
		publish_device_buffer_count();
	}

	void release_device(RenderingDevice *p_rd) {
		if (p_rd == nullptr || (rd != nullptr && rd != p_rd)) {
			discard_device_state();
			publish_device_buffer_count();
			return;
		}
		rd = p_rd;
		for (auto &entry : texture_uniforms)
			release_uniform_rid(entry.second);
		release_uniform_rid(bone_uniform);
		for (auto &entry : pipelines)
			release_pipeline_rid(entry.second);
		for (auto &entry : device_geometry)
			release_rid(entry.second.buffer);
		for (Target &target : targets)
			release_target(target);
		release_rid(bone_buffer);
		release_texture_rid(fallback_texture);
		release_rid(sampler);
		release_rid(shader);
		discard_device_state();
		publish_device_buffer_count();
	}

	bool initialize(RenderingDevice *p_rd);
	RID make_texture(int p_size, RenderingDevice::DataFormat p_format,
			int64_t p_usage);
	bool ensure_target(Target &r_target, int p_size);
	RID pipeline_for(int64_t p_framebuffer_format, const FixedFunctionState &p_state);
	bool upload_stream(const Q3PackedStream &p_stream);
	bool upload_bones(const std::vector<Transform3D> &p_bones);
	RID uniform_for(const DeviceCommand &p_command);
	bool draw(const DeviceFrame &p_frame);
	Dictionary report() const;
};

bool SlotCaptureAdapter::Impl::initialize(RenderingDevice *p_rd) {
	if (rd == p_rd && shader.is_valid() && sampler.is_valid() &&
			vertex_format != RenderingDevice::INVALID_FORMAT_ID)
		return true;
	release_device(p_rd);
	rd = p_rd;
	if (rd == nullptr) {
		set_failure("RenderingDevice is unavailable for the slot captures");
		return false;
	}
	if (rd->limit_get(RenderingDevice::LIMIT_MAX_PUSH_CONSTANT_SIZE) <
			kPushConstantBytes) {
		set_failure("RenderingDevice does not support the slot capture push block");
		return false;
	}
	Ref<RDShaderSPIRV> spirv;
	const std::string compile_errors = compile_rd_spirv(rd, kSlotCaptureVertexShader,
			slot_capture_fragment_shader_source(), spirv);
	if (!compile_errors.empty()) {
		set_failure("Slot capture shader compilation failed: " + compile_errors);
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova slot capture");
	if (!shader.is_valid()) {
		set_failure("RenderingDevice rejected the slot capture shader");
		return false;
	}
	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler = rd->sampler_create(sampler_state);

	// An opaque white texel: a missing diffuse/detail samples coverage 1, the
	// object wrappers' no-texture default.
	Ref<RDTextureFormat> fallback_format;
	fallback_format.instantiate();
	fallback_format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fallback_format->set_width(1);
	fallback_format->set_height(1);
	fallback_format->set_depth(1);
	fallback_format->set_array_layers(1);
	fallback_format->set_mipmaps(1);
	fallback_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	fallback_format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	fallback_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> fallback_view;
	fallback_view.instantiate();
	PackedByteArray fallback_pixel;
	fallback_pixel.resize(4);
	fallback_pixel.fill(255);
	TypedArray<PackedByteArray> fallback_data;
	fallback_data.push_back(fallback_pixel);
	fallback_texture = rd->texture_create(fallback_format, fallback_view,
			fallback_data);
	if (!sampler.is_valid() || !fallback_texture.is_valid()) {
		set_failure("RenderingDevice could not create the slot capture sampled resources");
		return false;
	}

	// The Q3GeometryCache stream layout, shared with the focused Q3 adapter.
	vertex_format = rd->vertex_format_create(q3_vertex_attributes());
	vertex_buffers.resize(1);
	vertex_offsets.resize(1);
	if (vertex_format == RenderingDevice::INVALID_FORMAT_ID) {
		set_failure("RenderingDevice rejected the slot capture vertex format");
		return false;
	}
	if (clear_white.is_empty()) {
		// The retail slot RT clear (renderer::kSlotCaptureClearArgb, white RGB
		// and alpha 0); the drape reads the RGB.
		clear_white.push_back(slot_capture_clear_color());
	}
	return true;
}

RID SlotCaptureAdapter::Impl::make_texture(int p_size,
		RenderingDevice::DataFormat p_format, int64_t p_usage) {
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(p_format);
	format->set_width(p_size);
	format->set_height(p_size);
	format->set_depth(1);
	format->set_array_layers(1);
	format->set_mipmaps(1);
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	// MSAA on the capture is a device fold, not a retail property: retail's
	// slot RT chain is single-sampled (RenderSlot_InitTextureChain creates a
	// plain D3DUSAGE_RENDERTARGET texture and a D3DMULTISAMPLE_NONE depth
	// surface, GRenderTarget_CreateSurfaces @ 0x67f7b0) and its drape reads
	// that RT bilinearly. The 4x resolve keeps a partial-coverage RGB edge in
	// the resolve target so a hard-aliased 1-2 px silhouette line does not
	// scintillate against the breathing first-person camera (the eye rides
	// the posed head bone); the record lists it with the other slot folds.
	format->set_samples(RenderingDevice::TEXTURE_SAMPLES_4);
	format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(p_usage));
	Ref<RDTextureView> view;
	view.instantiate();
	return rd->texture_create(format, view);
}

bool SlotCaptureAdapter::Impl::ensure_target(Target &r_target, int p_size) {
	if (r_target.size == p_size && r_target.framebuffer.is_valid() &&
			rd->framebuffer_is_valid(r_target.framebuffer))
		return true;
	release_target(r_target);
	r_target.size = p_size;
	r_target.msaa_color = make_texture(p_size,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	r_target.msaa_depth = make_texture(p_size,
			RenderingDevice::DATA_FORMAT_D32_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	if (r_target.msaa_color.is_valid() && r_target.msaa_depth.is_valid()) {
		TypedArray<RID> attachments;
		attachments.push_back(r_target.msaa_color);
		attachments.push_back(r_target.msaa_depth);
		r_target.framebuffer = rd->framebuffer_create(attachments);
	}
	if (!r_target.framebuffer.is_valid()) {
		set_failure("RenderingDevice could not allocate a slot capture target");
		release_target(r_target);
		return false;
	}
	return true;
}

RID SlotCaptureAdapter::Impl::pipeline_for(int64_t p_framebuffer_format,
		const FixedFunctionState &p_state) {
	const PipelineKey key{p_framebuffer_format, fixed_function_state_key(p_state)};
	const auto found = pipelines.find(key);
	if (found != pipelines.end())
		return found->second;
	// The engine's PROJSHAD state; the 4x MSAA is the capture's device fold
	// (make_texture). Reverse-Z like Godot's own scene depth (the projection
	// carries Projection::create_depth_correction): nearer = greater.
	const RdPipelineState state = rd_pipeline_state(p_state);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_4);
	RID pipeline = create_rd_render_pipeline(rd, shader, p_framebuffer_format,
			vertex_format, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, state.rasterization,
			multisample, state.depth_stencil, state.color_blend);
	if (!pipeline.is_valid())
		set_failure("RenderingDevice rejected a slot capture pipeline");
	else
		pipelines.emplace(key, pipeline);
	return pipeline;
}

bool SlotCaptureAdapter::Impl::upload_stream(const Q3PackedStream &p_stream) {
	if (p_stream.bytes.is_empty())
		return false;
	DeviceGeometry &geometry = device_geometry[p_stream.entry_id];
	const std::uint32_t required = static_cast<std::uint32_t>(p_stream.bytes.size());
	if (!geometry.buffer.is_valid() || required > geometry.capacity) {
		release_rid(geometry.buffer);
		geometry.capacity = required;
		geometry.uploaded_generation = 0;
		geometry.buffer = rd->vertex_buffer_create(geometry.capacity);
		if (!geometry.buffer.is_valid()) {
			set_failure("RenderingDevice could not allocate a slot capture vertex buffer");
			return false;
		}
	}
	if (geometry.uploaded_generation == p_stream.generation)
		return true;
	if (rd->buffer_update(geometry.buffer, 0, required, p_stream.bytes) != OK) {
		set_failure("RenderingDevice rejected the slot capture vertex upload");
		return false;
	}
	geometry.uploaded_generation = p_stream.generation;
	return true;
}

// The frame's bone palette (every skinned command's matrices, concatenated)
// in one storage buffer bound as set 1; grown when a frame needs more.
bool SlotCaptureAdapter::Impl::upload_bones(const std::vector<Transform3D> &p_bones) {
	const std::uint32_t required = static_cast<std::uint32_t>(
			std::max<std::size_t>(p_bones.size(), 1) * kBoneMatrixBytes);
	if (!bone_buffer.is_valid() || required > bone_buffer_capacity) {
		release_uniform_rid(bone_uniform);
		release_rid(bone_buffer);
		bone_buffer_capacity = required;
		bone_buffer = rd->storage_buffer_create(bone_buffer_capacity);
		if (!bone_buffer.is_valid()) {
			set_failure("RenderingDevice could not allocate the slot capture bone palette");
			return false;
		}
	}
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(p_bones.size()) * kBoneMatrixBytes);
	for (std::size_t bone = 0; bone < p_bones.size(); ++bone)
		write_matrix(bytes, static_cast<int64_t>(bone) * kBoneMatrixBytes, p_bones[bone]);
	if (!bytes.is_empty() &&
			rd->buffer_update(bone_buffer, 0, static_cast<std::uint32_t>(bytes.size()),
					bytes) != OK) {
		set_failure("RenderingDevice rejected the slot capture bone upload");
		return false;
	}
	if (!bone_uniform.is_valid() || !rd->uniform_set_is_valid(bone_uniform)) {
		Ref<RDUniform> uniform;
		uniform.instantiate();
		uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		uniform->set_binding(0);
		uniform->add_id(bone_buffer);
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(uniform);
		bone_uniform = rd->uniform_set_create(uniforms, shader, 1);
		if (!bone_uniform.is_valid()) {
			set_failure("RenderingDevice could not bind the slot capture bone palette");
			return false;
		}
	}
	return true;
}

RID SlotCaptureAdapter::Impl::uniform_for(const DeviceCommand &p_command) {
	RenderingServer *server = RenderingServer::get_singleton();
	auto resolve = [&](const RID &p_server_rid) {
		const RID result = server != nullptr && p_server_rid.is_valid() ?
				server->texture_get_rd_texture(p_server_rid, false) : RID();
		return result.is_valid() ? result : fallback_texture;
	};
	const RID diffuse = resolve(p_command.diffuse);
	const RID detail = resolve(p_command.detail);
	const std::pair<std::uint64_t, std::uint64_t> key{diffuse.get_id(), detail.get_id()};
	const auto found = texture_uniforms.find(key);
	if (found != texture_uniforms.end() && found->second.is_valid() && rd->uniform_set_is_valid(found->second))
		return found->second;
	// A miss: the pairs whose set the device let go with a texture it bound are dropped, their
	// keys never met again (RD ids are not reused), so the map holds the live pairs alone.
	for (auto entry = texture_uniforms.begin(); entry != texture_uniforms.end();) {
		if (!entry->second.is_valid() || !rd->uniform_set_is_valid(entry->second))
			entry = texture_uniforms.erase(entry);
		else
			++entry;
	}
	RID &kept = texture_uniforms[key];
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, sampler, diffuse));
	uniforms.push_back(sampled_texture_uniform(1, sampler, detail));
	kept = rd->uniform_set_create(uniforms, shader, 0);
	return kept;
}

bool SlotCaptureAdapter::Impl::draw(const DeviceFrame &p_frame) {
	// Uploads precede the draw lists: a buffer update inside one is rejected.
	for (const DeviceCapture &capture : p_frame.captures) {
		for (const DeviceCommand &command : capture.commands) {
			if (!command.stream || !upload_stream(*command.stream)) {
				publish_device_buffer_count();
				return false;
			}
		}
	}
	publish_device_buffer_count();
	if (!upload_bones(p_frame.bones))
		return false;
	if (push_bytes.size() != static_cast<int64_t>(kPushConstantBytes))
		push_bytes.resize(kPushConstantBytes);
	int captures_drawn = 0;
	int draw_calls = 0;
	// The effects' filter code the frame draws under (the device leg's).
	const int effect_filter_code = TextureFilterDevice::effect_filter_code();
	for (const DeviceCapture &capture : p_frame.captures) {
		if (capture.order < 0 || capture.order >= kSlotCaptureCount ||
				capture.size <= 0 || !capture.target.is_valid() ||
				!rd->texture_is_valid(capture.target))
			continue;
		Target &target = targets[static_cast<std::size_t>(capture.order)];
		if (!ensure_target(target, capture.size))
			return false;
		const int64_t framebuffer_format = rd->framebuffer_get_format(target.framebuffer);
		for (const DeviceCommand &command : capture.commands) {
			if (!pipeline_for(framebuffer_format, command.state).is_valid())
				return false;
		}
		// Clear to the retail 0x00FFFFFF and the reverse-Z far depth, draw the
		// caster set, resolve the multisampled silhouette into the target.
		const int64_t draw_list = rd->draw_list_begin(target.framebuffer,
				RenderingDevice::DRAW_CLEAR_ALL, clear_white, 0.0f);
		if (draw_list == RenderingDevice::INVALID_ID) {
			set_failure("RenderingDevice could not begin a slot capture draw list");
			return false;
		}
		for (const DeviceCommand &command : capture.commands) {
			const RID uniform = uniform_for(command);
			if (!uniform.is_valid()) {
				rd->draw_list_end();
				set_failure("RenderingDevice could not bind slot capture textures");
				return false;
			}
			const RID pipeline = pipeline_for(framebuffer_format, command.state);
			rd->draw_list_bind_render_pipeline(draw_list, pipeline);
			rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
			rd->draw_list_bind_uniform_set(draw_list, bone_uniform, 1);
			vertex_buffers[0] = device_geometry[command.stream->entry_id].buffer;
			vertex_offsets[0] = 0;
			rd->draw_list_bind_vertex_buffers_format(draw_list, vertex_format,
					command.stream->vertex_count, vertex_buffers, vertex_offsets);
			for (std::uint32_t instance = 0; instance < command.transform_count;
					++instance) {
				const Transform3D &model = p_frame.transforms[
						command.first_transform + instance];
				const Projection mvp = capture.view_projection * Projection(model);
				SlotPush push{};
				for (std::uint32_t column = 0; column < 4; ++column) {
					for (std::uint32_t row = 0; row < 4; ++row)
						push.mvp[column * 4 + row] = mvp[column][row];
				}
				push.params = {command.alpha_test_value, command.alpha_mod,
						static_cast<float>(command.flags),
						static_cast<float>(command.first_bone)};
				push.max_lods = {command.diffuse_max_lod, command.detail_max_lod,
						static_cast<float>(effect_filter_code), 0.0f};
				std::memcpy(push_bytes.ptrw(), &push, sizeof(push));
				rd->draw_list_set_push_constant(draw_list, push_bytes,
						kPushConstantBytes);
				rd->draw_list_draw(draw_list, false, 1);
				++draw_calls;
			}
		}
		rd->draw_list_end();
		if (rd->texture_resolve_multisample(target.msaa_color, capture.target) != OK) {
			set_failure("RenderingDevice could not resolve a slot capture");
			return false;
		}
		++captures_drawn;
	}
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = captures_drawn > 0 ? "drawn" : "drawn_empty";
		failure.clear();
		drawn_frame_id = p_frame.frame_id;
		counters.captures_drawn = captures_drawn;
		counters.draw_calls = draw_calls;
	}
	return true;
}

Dictionary SlotCaptureAdapter::Impl::report() const {
	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	Dictionary result;
	result["slot_backend"] = "typed_rendering_device_pre_opaque_pass";
	result["slot_capture_msaa"] = 4;
	result["slot_capture_clear"] = slot_capture_clear_color();
	result["slot_depth_compare"] = "greater_or_equal";
	result["slot_depth_write"] = true;
	result["slot_skinning"] = "gpu_bone_palette";
	result["slot_status"] = opennova::to_gd(status);
	result["slot_failure"] = opennova::to_gd(failure);
	result["slot_submitted_frame_id"] = static_cast<int64_t>(submitted_frame_id);
	result["slot_drawn_frame_id"] = static_cast<int64_t>(drawn_frame_id);
	result["slot_captures_compiled"] = counters.captures_compiled;
	result["slot_surfaces_compiled"] = counters.surfaces_compiled;
	result["slot_skinned_commands"] = counters.skinned_commands;
	result["slot_blended_commands"] = counters.blended_commands;
	result["slot_packed_vertices"] = counters.packed_vertices;
	result["slot_readbacks"] = counters.readbacks;
	result["slot_unclassified_surfaces"] = counters.unclassified_surfaces;
	result["slot_no_pass_surfaces"] = counters.no_pass_surfaces;
	result["slot_captures_drawn"] = counters.captures_drawn;
	result["slot_draw_calls"] = counters.draw_calls;
	result["slot_cached_entries"] = static_cast<int64_t>(cached_entries);
	result["slot_device_buffers"] = static_cast<int64_t>(device_buffers);
	return result;
}

SlotCaptureAdapter::SlotCaptureAdapter() : impl_(std::make_unique<Impl>()) {}

SlotCaptureAdapter::~SlotCaptureAdapter() = default;

void SlotCaptureAdapter::compile_frame(
		const std::vector<SlotCaptureRequest> &p_requests) {
	if (!impl_)
		return;
	auto frame = std::make_shared<DeviceFrame>();
	frame->frame_id = impl_->next_frame_id++;
	Q3GeometryCache &cache = impl_->geometry_cache;
	cache.begin_frame(frame->frame_id);
	cache.prune([](std::uint64_t p_source_id) {
		return ObjectDB::get_instance(p_source_id) != nullptr;
	});
	SlotCaptureFrameCounters counters{};
	std::array<int, kSlotCaptureCount> compiled_surfaces{};
	std::vector<GeometryInstance3D *> instances;
	for (const SlotCaptureRequest &request : p_requests) {
		if (request.order < 0 || request.order >= kSlotCaptureCount)
			continue;
		DeviceCapture capture;
		capture.order = request.order;
		capture.size = request.size;
		capture.target = request.target;
		// Godot's RD clip space: the depth correction flips y and maps the
		// GL clip depth onto the reverse-Z [0, 1] band, exactly what the
		// scene's own view projection carries.
		capture.view_projection = Projection::create_depth_correction(true) *
				request.projection * Projection(request.view.affine_inverse());
		instances.clear();
		for (std::size_t index = 0; index < request.casters.size(); ++index) {
			collect_visible_geometry(Object::cast_to<Node>(
					ObjectDB::get_instance(request.casters[index])), instances,
					index >= request.walk_begin);
		}
		for (GeometryInstance3D *source : instances) {
			Ref<Mesh> mesh;
			Ref<MultiMesh> multimesh;
			MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(source);
			if (mesh_instance != nullptr) {
				mesh = mesh_instance->get_mesh();
			} else if (MultiMeshInstance3D *multi_instance =
					Object::cast_to<MultiMeshInstance3D>(source)) {
				multimesh = multi_instance->get_multimesh();
				if (multimesh.is_valid())
					mesh = multimesh->get_mesh();
			}
			Ref<ArrayMesh> array_mesh = mesh;
			if (array_mesh.is_null())
				continue;
			const std::uint64_t source_id = source->get_instance_id();
			std::vector<Transform3D> emitted_transforms;
			if (multimesh.is_valid()) {
				const std::vector<Transform3D> &local_transforms =
						cache.instance_transforms(source_id, 1, multimesh.ptr());
				const Transform3D source_transform = source->get_global_transform();
				emitted_transforms.reserve(local_transforms.size());
				for (const Transform3D &local_transform : local_transforms) {
					const Transform3D transform = source_transform * local_transform;
					if (std::abs(transform.basis.determinant()) > 1.0e-8f)
						emitted_transforms.push_back(transform);
				}
			} else {
				emitted_transforms.push_back(source->get_global_transform());
			}
			if (emitted_transforms.empty())
				continue;
			const std::vector<Transform3D> palette = skin_palette(mesh_instance);
			std::uint32_t first_bone = 0;
			bool bones_appended = false;
			for (int surface = 0; surface < array_mesh->get_surface_count(); ++surface) {
				AdmittedSurface admitted;
				switch (admit_surface(cache, source, mesh, array_mesh.ptr(), surface,
						!palette.empty(), admitted)) {
					case SurfaceAdmission::Unclassified:
						++counters.unclassified_surfaces;
						continue;
					case SurfaceAdmission::NoPass:
						++counters.no_pass_surfaces;
						continue;
					case SurfaceAdmission::NoStream:
						continue;
					case SurfaceAdmission::Admitted:
						break;
				}
				const SlotCaptureSurface &plan = admitted.surface;
				const Ref<Texture2D> &diffuse = admitted.diffuse;
				const Ref<Texture2D> &detail = admitted.detail;
				const std::shared_ptr<const Q3PackedStream> &stream = admitted.stream;
				DeviceCommand command;
				command.stream = stream;
				command.first_transform = static_cast<std::uint32_t>(frame->transforms.size());
				for (const Transform3D &transform : emitted_transforms)
					frame->transforms.push_back(transform);
				command.transform_count = static_cast<std::uint32_t>(
						frame->transforms.size()) - command.first_transform;
				if (!palette.empty()) {
					if (!bones_appended) {
						first_bone = static_cast<std::uint32_t>(frame->bones.size());
						for (const Transform3D &bone : palette)
							frame->bones.push_back(bone);
						bones_appended = true;
					}
					command.first_bone = first_bone;
					command.bone_count = static_cast<std::uint32_t>(palette.size());
					++counters.skinned_commands;
				}
				command.diffuse_resource = diffuse;
				command.detail_resource = detail;
				command.diffuse = server_rid(diffuse);
				command.detail = server_rid(detail);
				command.alpha_mod = admitted.values.runtime.alpha;
				command.diffuse_max_lod = admitted.values.diffuse_max_lod;
				command.detail_max_lod = admitted.values.detail_max_lod;
				command.alpha_test_value = plan.alpha_test_value;
				command.flags = plan.flags;
				command.state = plan.state;
				if (command.state.blend.enabled)
					++counters.blended_commands;
				capture.commands.push_back(std::move(command));
			}
		}
		compiled_surfaces[static_cast<std::size_t>(request.order)] =
				static_cast<int>(capture.commands.size());
		counters.surfaces_compiled += static_cast<int>(capture.commands.size());
		++counters.captures_compiled;
		frame->captures.push_back(std::move(capture));
	}
	frame->evicted_entries = cache.pending_evictions(
			impl_->consumed_frame_id.load(std::memory_order_acquire));
	const Q3GeometryCache::FrameCounters &cache_counters = cache.frame_counters();
	counters.packed_vertices = static_cast<int>(cache_counters.packed_vertices);
	counters.readbacks = static_cast<int>(cache_counters.readbacks);
	impl_->publish(frame);
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	impl_->status = frame->captures.empty() ? "compiled_empty" : "compiled";
	impl_->submitted_frame_id = frame->frame_id;
	counters.captures_drawn = impl_->counters.captures_drawn;
	counters.draw_calls = impl_->counters.draw_calls;
	impl_->counters = counters;
	impl_->compiled_surfaces = compiled_surfaces;
	impl_->cached_entries = cache.entry_count();
}

SlotCaptureWarmCounters SlotCaptureAdapter::warm_streams(
		const std::vector<std::uint64_t> &p_casters) {
	SlotCaptureWarmCounters warmed;
	if (!impl_)
		return warmed;
	Q3GeometryCache &cache = impl_->geometry_cache;
	const Q3GeometryCache::FrameCounters before = cache.frame_counters();
	std::vector<GeometryInstance3D *> instances;
	for (const std::uint64_t caster : p_casters)
		collect_visible_geometry(Object::cast_to<Node>(ObjectDB::get_instance(caster)), instances);
	for (GeometryInstance3D *source : instances) {
		Ref<Mesh> mesh;
		MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(source);
		if (mesh_instance != nullptr) {
			mesh = mesh_instance->get_mesh();
		} else if (MultiMeshInstance3D *multi_instance =
				Object::cast_to<MultiMeshInstance3D>(source)) {
			const Ref<MultiMesh> multimesh = multi_instance->get_multimesh();
			if (multimesh.is_valid())
				mesh = multimesh->get_mesh();
		}
		Ref<ArrayMesh> array_mesh = mesh;
		if (array_mesh.is_null())
			continue;
		const bool skinned = !skin_palette(mesh_instance).empty();
		for (int surface = 0; surface < array_mesh->get_surface_count(); ++surface) {
			AdmittedSurface admitted;
			if (admit_surface(cache, source, mesh, array_mesh.ptr(), surface, skinned, admitted) ==
					SurfaceAdmission::Admitted)
				++warmed.surfaces;
		}
	}
	const Q3GeometryCache::FrameCounters &after = cache.frame_counters();
	warmed.packed_vertices = static_cast<int>(after.packed_vertices - before.packed_vertices);
	warmed.readbacks = static_cast<int>(after.readbacks - before.readbacks);
	return warmed;
}

void SlotCaptureAdapter::clear_frame() {
	if (!impl_)
		return;
	impl_->publish(nullptr);
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	impl_->counters = SlotCaptureFrameCounters{};
	impl_->compiled_surfaces = {};
}

bool SlotCaptureAdapter::draw(RenderingDevice *p_rd) {
	if (!impl_)
		return false;
	const std::shared_ptr<const DeviceFrame> frame = impl_->frame_snapshot();
	if (!frame)
		return true;
	if (!impl_->initialize(p_rd))
		return false;
	// Consumed every render, drawn or not: a frame with no captures still
	// names the cache entries evicted since the last consumed one.
	impl_->consume_frame(p_rd);
	if (frame->captures.empty()) {
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->counters.captures_drawn = 0;
		impl_->counters.draw_calls = 0;
		return true;
	}
	return impl_->draw(*frame);
}

bool SlotCaptureAdapter::create_device(RenderingDevice *p_rd) {
	return impl_ && impl_->initialize(p_rd);
}

void SlotCaptureAdapter::release_device(RenderingDevice *p_rd) {
	if (impl_)
		impl_->release_device(p_rd);
}

SlotCaptureFrameCounters SlotCaptureAdapter::frame_counters() const {
	if (!impl_)
		return SlotCaptureFrameCounters{};
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	return impl_->counters;
}

Dictionary SlotCaptureAdapter::get_report() const {
	return impl_ ? impl_->report() : Dictionary();
}

SlotCaptureCompositorEffect::SlotCaptureCompositorEffect() :
		adapter_(std::make_unique<SlotCaptureAdapter>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	set_enabled(true);
}

SlotCaptureCompositorEffect::~SlotCaptureCompositorEffect() = default;

void SlotCaptureCompositorEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&SlotCaptureCompositorEffect::get_backend_report);
}

void SlotCaptureCompositorEffect::create_device() {
	if (!is_shutdown())
		call_device_create(callable_mp(this,
				&SlotCaptureCompositorEffect::create_device_on_render_thread));
}

void SlotCaptureCompositorEffect::create_device_on_render_thread() {
	RenderingServer *server = RenderingServer::get_singleton();
	if (!is_shutdown() && server != nullptr)
		adapter_->create_device(server->get_rendering_device());
}

void SlotCaptureCompositorEffect::release_device_resources() {
	set_enabled(false);
	if (shutdown_requested_.exchange(true, std::memory_order_acq_rel))
		return;
	adapter_->clear_frame();
	RenderingServer *server = RenderingServer::get_singleton();
	adapter_->release_device(server != nullptr ? server->get_rendering_device() : nullptr);
}

Dictionary SlotCaptureCompositorEffect::get_backend_report() const {
	Dictionary result = adapter_->get_report();
	result["slot_callback_type"] = static_cast<int>(EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	result["slot_shutdown"] = is_shutdown();
	result["slot_gpu_us"] = static_cast<int64_t>(
			gpu_span_us_.load(std::memory_order_relaxed));
	result["slot_gpu_valid"] = gpu_span_valid_.load(std::memory_order_relaxed);
	return result;
}

void SlotCaptureCompositorEffect::_render_callback(int32_t p_effect_callback_type,
		RenderData *p_render_data) {
	if (is_shutdown() || p_effect_callback_type != EFFECT_CALLBACK_TYPE_PRE_OPAQUE)
		return;
	RenderingServer *server = RenderingServer::get_singleton();
	RenderingDevice *rd = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rd == nullptr)
		return;
	const bool gpu_timing = gpu_timing_enabled_.load(std::memory_order_relaxed);
	if (gpu_timing) {
		std::uint64_t span_us = 0;
		gpu_span_valid_.store(rd_timestamp_span_us(rd,
				"opennova_slot_capture_begin", "opennova_slot_capture_end",
				span_us), std::memory_order_relaxed);
		gpu_span_us_.store(span_us, std::memory_order_relaxed);
		rd->capture_timestamp("opennova_slot_capture_begin");
	}
	adapter_->draw(rd);
	if (gpu_timing)
		rd->capture_timestamp("opennova_slot_capture_end");
}
