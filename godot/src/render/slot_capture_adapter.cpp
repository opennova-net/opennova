#include "render/slot_capture_adapter.h"
#include "render/q3_frame_adapter.h"
#include "render/q3_geometry_cache.h"

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
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
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
#include <runtime/renderer/render_slot_shadow.h>

using namespace godot;
using namespace opennova::renderer;

namespace {

// mat4 + vec4: the per-draw block below.
constexpr std::uint32_t kPushConstantBytes = 80u;
// Push flag bits (params.z) the shaders decode.
constexpr std::uint32_t kFlagAlphaTest = 1u;
constexpr std::uint32_t kFlagAlphaInvert = 2u;
constexpr std::uint32_t kFlagAlphaMod = 4u;
constexpr std::uint32_t kFlagDetailAlpha = 8u;
constexpr std::uint32_t kFlagSkinned = 16u;
constexpr std::uint32_t kBoneMatrixBytes = 64u;

// The black PROJSHAD pass over the retail 0x00FFFFFF clear: the geometry
// arrives in the Q3GeometryCache layout (q3_geometry_cache.h, 104-byte
// vertices) with the bind-space positions and, for skinned surfaces, the bone
// indices in CUSTOM0 and weights in CUSTOM1 (Q3PackParameters::skin_channels),
// skinned here from the frame's bone palette (a storage buffer of
// skeleton-space bone x bind matrices, exactly the CPU palette Q3 blends).
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
		vec3 skinned = vec3(0.0);
		float total = 0.0;
		for (int influence = 0; influence < 4; influence++) {
			float weight = in_custom1[influence];
			if (weight <= 0.0) {
				continue;
			}
			uint bone = base + uint(in_custom0[influence] + 0.5);
			skinned += (palette.bones[bone] * bind).xyz * weight;
			total += weight;
		}
		if (total > 0.0) {
			position = skinned / total;
		}
	}
	gl_Position = pc.mvp * vec4(position, 1.0);
	uv = in_uv;
	uv2 = in_uv2;
}
)GLSL";

// The four PROJSHAD coverage policies of the object wrappers
// (godot/shaders/object/output_opaque.gdshaderinc's capture branch over
// sampling/single|detail.gdshaderinc obj_proj_shadow_coverage + the coverage
// include): coverage = Diffuse1.a, x Detail.a over UV2 for the _MT FFP
// blocks, x u_alpha_mod for the FFP families; alpha-test materials keep
// coverage > ref (invert: <= ref) and discard the rest; every kept fragment
// writes black at alpha 1 (output_alpha's capture branch writes ALPHA = 1.0).
// The no-pass techniques never reach the device (skipped at compile).
// The engine's object_shader_template.h carries the witness for both the
// per-technique table (object_projected_shadow_coverage) and the alpha-test
// compare; this shader only evaluates them.
const char *kSlotCaptureFragmentShader = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D diffuse_texture;
layout(set = 0, binding = 1) uniform sampler2D detail_texture;

layout(push_constant, std430) uniform SlotPush {
	mat4 mvp;
	vec4 params;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec2 uv2;
layout(location = 0) out vec4 frag_color;

void main() {
	uint flags = uint(pc.params.z + 0.5);
	float coverage = texture(diffuse_texture, uv).a;
	if ((flags & 8u) != 0u) {
		coverage *= texture(detail_texture, uv2).a;
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
	frag_color = vec4(0.0, 0.0, 0.0, 1.0);
}
)GLSL";

struct SlotPush {
	std::array<float, 16> mvp{};
	std::array<float, 4> params{};
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
	std::uint32_t flags = 0;
	bool two_sided = false;
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

Ref<RDUniform> sampled_texture_uniform(int p_binding, const RID &p_sampler,
		const RID &p_texture) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	uniform->set_binding(p_binding);
	uniform->add_id(p_sampler);
	uniform->add_id(p_texture);
	return uniform;
}

Ref<Texture2D> texture_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name) {
	if (p_material.is_null())
		return Ref<Texture2D>();
	const Variant value = p_material->get_shader_parameter(p_name);
	if (value.get_type() != Variant::OBJECT)
		return Ref<Texture2D>();
	return value;
}

float float_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, float p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT
			? static_cast<float>(value) : p_default;
}

Vector3 vector3_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, const Vector3 &p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::VECTOR3 ? static_cast<Vector3>(value) : p_default;
}

RID server_rid(const Ref<Texture2D> &p_texture) {
	return p_texture.is_valid() ? p_texture->get_rid() : RID();
}

Ref<Material> active_material(GeometryInstance3D *p_source,
		const Ref<Mesh> &p_mesh, int p_surface) {
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_source))
		return mesh_instance->get_active_material(p_surface);
	Ref<Material> material = p_source->get_material_override();
	if (material.is_null() && p_mesh.is_valid())
		material = p_mesh->surface_get_material(p_surface);
	return material;
}

// The skeleton-space bone x bind palette of a skinned instance (the same
// palette the Q3 adapter CPU-skins with); empty for an unskinned instance.
std::vector<Transform3D> skin_palette(MeshInstance3D *p_instance) {
	std::vector<Transform3D> result;
	if (p_instance == nullptr || p_instance->get_skeleton_path().is_empty())
		return result;
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
// effect is the registered P0/P1 instance.
void collect_visible_geometry(Node *p_node,
		std::vector<GeometryInstance3D *> &r_instances) {
	if (p_node == nullptr)
		return;
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		if (geometry->is_visible_in_tree() &&
				!bool(geometry->get_meta("_opennova_auxiliary_draw", false)))
			r_instances.push_back(geometry);
	}
	for (int index = 0; index < p_node->get_child_count(); ++index)
		collect_visible_geometry(p_node->get_child(index), r_instances);
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

} // namespace

class SlotCaptureAdapter::Impl {
public:
	struct PipelineKey {
		int64_t framebuffer_format = -1;
		bool two_sided = false;

		bool operator<(const PipelineKey &p_other) const {
			return std::tie(framebuffer_format, two_sided) <
					std::tie(p_other.framebuffer_format, p_other.two_sided);
		}
	};

	// The per-order MSAA colour/depth pair and its framebuffer; the capture
	// resolves into the request's target. Retail's slot RT carries its own z
	// (render_shadow_pass clears the slot RT per capture), so the pass depth
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
	std::vector<RID> transient_uniforms;
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
		transient_uniforms.clear();
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
		for (RID &uniform : transient_uniforms)
			release_uniform_rid(uniform);
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
	RID pipeline_for(int64_t p_framebuffer_format, bool p_two_sided);
	bool upload_stream(const Q3PackedStream &p_stream);
	bool upload_bones(const std::vector<Transform3D> &p_bones);
	RID make_uniform(const DeviceCommand &p_command);
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
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kSlotCaptureVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kSlotCaptureFragmentShader));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null() || !spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			!spirv->get_stage_compile_error(
					RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
		set_failure("Slot capture shader compilation failed");
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

	// The Q3GeometryCache stream layout (q3_frame_adapter.cpp declares the
	// same eight attributes).
	TypedArray<Ref<RDVertexAttribute>> attributes;
	auto add_attribute = [&](std::uint32_t p_location,
			RenderingDevice::DataFormat p_format, std::uint32_t p_offset) {
		Ref<RDVertexAttribute> attribute;
		attribute.instantiate();
		attribute->set_location(p_location);
		attribute->set_binding(0);
		attribute->set_format(p_format);
		attribute->set_offset(p_offset);
		attribute->set_stride(kQ3VertexStride);
		attribute->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);
		attributes.push_back(attribute);
	};
	add_attribute(0, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0);
	add_attribute(1, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 12);
	add_attribute(2, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 24);
	add_attribute(3, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 32);
	add_attribute(4, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 48);
	add_attribute(5, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 64);
	add_attribute(6, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 80);
	add_attribute(7, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 96);
	vertex_format = rd->vertex_format_create(attributes);
	vertex_buffers.resize(1);
	vertex_offsets.resize(1);
	if (vertex_format == RenderingDevice::INVALID_FORMAT_ID) {
		set_failure("RenderingDevice rejected the slot capture vertex format");
		return false;
	}
	if (clear_white.is_empty()) {
		// The retail slot RT clear (renderer::kSlotCaptureClearArgb, white RGB
		// and alpha 0 — 0x00FFFFFF as Color(1.0f, 1.0f, 1.0f, 0.0f)); the
		// drape reads the RGB.
		const uint32_t argb = kSlotCaptureClearArgb;
		clear_white.push_back(Color(float((argb >> 16) & 0xFF) / 255.0f,
				float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f,
				float((argb >> 24) & 0xFF) / 255.0f));
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
	// MSAA on the capture: the drape keeps the resolved partial-coverage RGB
	// edge, and a hard-aliased 1-2 px silhouette line scintillates against
	// the breathing first-person camera (the eye rides the posed head bone).
	// The 4x resolve supplies the same coverage ramp retail's multisampled
	// black-on-white RT resolves, like FrameFX's capture stretch supplies the
	// bloom-source box filter.
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
		bool p_two_sided) {
	const PipelineKey key{p_framebuffer_format, p_two_sided};
	const auto found = pipelines.find(key);
	if (found != pipelines.end())
		return found->second;
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	// The wrappers' cull_back / cull_disabled per material two-sidedness; the
	// stream keeps Godot's clockwise front faces like the Q3 adapter.
	raster->set_cull_mode(p_two_sided ? RenderingDevice::POLYGON_CULL_DISABLED :
			RenderingDevice::POLYGON_CULL_BACK);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_4);
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	// Reverse-Z like Godot's own scene depth (the projection carries
	// Projection::create_depth_correction): nearer = greater.
	depth->set_enable_depth_test(true);
	depth->set_enable_depth_write(true);
	depth->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	// Every kept fragment writes black at alpha 1, so the MaterialBlend
	// alpha-blend variants replace exactly like the beauty wrappers' capture
	// branch (output_alpha writes ALPHA = 1.0); additive variants never reach
	// the device (black adds nothing).
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(false);
	Ref<RDPipelineColorBlendState> color_blend;
	color_blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	color_blend->set_attachments(attachments);
	RID pipeline = rd->render_pipeline_create(shader, p_framebuffer_format,
			vertex_format, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster,
			multisample, depth, color_blend);
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

RID SlotCaptureAdapter::Impl::make_uniform(const DeviceCommand &p_command) {
	RenderingServer *server = RenderingServer::get_singleton();
	auto resolve = [&](const RID &p_server_rid) {
		const RID result = server != nullptr && p_server_rid.is_valid() ?
				server->texture_get_rd_texture(p_server_rid, false) : RID();
		return result.is_valid() ? result : fallback_texture;
	};
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, sampler, resolve(p_command.diffuse)));
	uniforms.push_back(sampled_texture_uniform(1, sampler, resolve(p_command.detail)));
	return rd->uniform_set_create(uniforms, shader, 0);
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
	for (RID &uniform : transient_uniforms)
		release_uniform_rid(uniform);
	transient_uniforms.clear();
	int captures_drawn = 0;
	int draw_calls = 0;
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
			if (!pipeline_for(framebuffer_format, command.two_sided).is_valid())
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
			RID uniform = make_uniform(command);
			if (!uniform.is_valid()) {
				rd->draw_list_end();
				set_failure("RenderingDevice could not bind slot capture textures");
				return false;
			}
			transient_uniforms.push_back(uniform);
			const RID pipeline = pipeline_for(framebuffer_format, command.two_sided);
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
				PackedByteArray push_bytes;
				push_bytes.resize(kPushConstantBytes);
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
	result["slot_capture_clear"] = Color(1.0f, 1.0f, 1.0f, 0.0f);
	result["slot_depth_compare"] = "greater_or_equal";
	result["slot_depth_write"] = true;
	result["slot_skinning"] = "gpu_bone_palette";
	result["slot_status"] = String::utf8(status.c_str());
	result["slot_failure"] = String::utf8(failure.c_str());
	result["slot_submitted_frame_id"] = static_cast<int64_t>(submitted_frame_id);
	result["slot_drawn_frame_id"] = static_cast<int64_t>(drawn_frame_id);
	result["slot_captures_compiled"] = counters.captures_compiled;
	result["slot_surfaces_compiled"] = counters.surfaces_compiled;
	result["slot_skinned_commands"] = counters.skinned_commands;
	result["slot_packed_vertices"] = counters.packed_vertices;
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
		for (const std::uint64_t caster : request.casters) {
			collect_visible_geometry(Object::cast_to<Node>(
					ObjectDB::get_instance(caster)), instances);
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
				const Ref<Material> material = active_material(source, mesh, surface);
				const Ref<ShaderMaterial> shader_material = material;
				ObjectMaterialClassification classification;
				if (shader_material.is_null() ||
						!Q3FrameAdapter::object_material_classification(material,
								classification)) {
					// Not an object effect (no registered classification): retail
					// submits no PROJSHAD pass for an effect without one, and no
					// coverage source exists to invent one from.
					++counters.unclassified_surfaces;
					continue;
				}
				const Ref<Texture2D> diffuse = texture_parameter(shader_material, "u_diffuse");
				const Ref<Texture2D> detail = texture_parameter(shader_material, "u_detail");
				ObjectShaderKey key = build_object_shader_key(classification);
				if (detail.is_null()) {
					// The runtime's missing-detail downgrade: an unresolved
					// secondary composes the no-detail technique
					// (object_model_materials.cpp).
					key &= ~OSCAP_DETAIL;
				}
				const ObjectShaderPipelineDescriptor pipeline =
						describe_object_shader_pipeline(key);
				const ObjectProjectedShadowPolicy policy =
						object_projected_shadow_policy(pipeline.technique);
				const ObjectProjectedShadowCoverage coverage =
						object_projected_shadow_coverage(pipeline.technique);
				if (policy == ObjectProjectedShadowPolicy::NoPass ||
						coverage == ObjectProjectedShadowCoverage::NoPass ||
						(policy == ObjectProjectedShadowPolicy::MaterialBlend &&
								classification.blend == ObjectBlendMode::Additive)) {
					// No PROJSHAD declaration, or the _FFP material-blend pass
					// of an additive material: black added to the RT is a no-op.
					++counters.no_pass_surfaces;
					continue;
				}
				Q3GeometryCache::Request cache_request;
				cache_request.key = {source_id, surface};
				cache_request.geometry_generation = 1;
				cache_request.pack.source = Q3Source::Object;
				cache_request.pack.uv_u = vector3_parameter(shader_material,
						"u_uv_transform_u", Vector3(1, 0, 0));
				cache_request.pack.uv_v = vector3_parameter(shader_material,
						"u_uv_transform_v", Vector3(0, 1, 0));
				cache_request.pack.skin_channels = !palette.empty();
				std::shared_ptr<const Q3PackedStream> stream = cache.acquire(
						cache_request, [&]() {
							return array_mesh->surface_get_arrays(surface);
						});
				if (!stream)
					continue;
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
					command.flags |= kFlagSkinned;
					++counters.skinned_commands;
				}
				command.diffuse_resource = diffuse;
				command.detail_resource = detail;
				command.diffuse = server_rid(diffuse);
				command.detail = server_rid(detail);
				command.alpha_mod = float_parameter(shader_material, "u_alpha_mod", 1.0f);
				command.alpha_test_value = classification.alpha_test_value;
				if (classification.alpha_test)
					command.flags |= kFlagAlphaTest;
				if (classification.alpha_test_invert)
					command.flags |= kFlagAlphaInvert;
				if (coverage == ObjectProjectedShadowCoverage::DiffuseAlphaFfp ||
						coverage == ObjectProjectedShadowCoverage::DiffuseDetailAlphaFfp)
					command.flags |= kFlagAlphaMod;
				if (coverage == ObjectProjectedShadowCoverage::DiffuseDetailAlphaFfp)
					command.flags |= kFlagDetailAlpha;
				command.two_sided = classification.is_two_sided;
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

int SlotCaptureAdapter::compiled_surface_count(int p_order) const {
	if (!impl_ || p_order < 0 || p_order >= kSlotCaptureCount)
		return 0;
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	return impl_->compiled_surfaces[static_cast<std::size_t>(p_order)];
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
	adapter_->draw(rd);
}
