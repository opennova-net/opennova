#include "render/display_decode.h"
#include "render/rd_fullscreen.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/classes/compositor.hpp>
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
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

constexpr std::uint32_t kPushConstantBytes = 16u;

enum class DecodePass : std::uint32_t {
	Snapshot = 0,
	GammaDecode = 1,
};

const char *kDecodeFragmentShader = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D source_color;

layout(push_constant, std430) uniform DecodePush {
	uint mode;
	uint padding0;
	uint padding1;
	uint padding2;
} pc;

layout(location = 0) out vec4 frag_color;

vec3 gamma_to_linear(vec3 gamma_rgb) {
	vec3 c = clamp(gamma_rgb, vec3(0.0), vec3(1.0));
	return mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			c * (1.0 / 12.92), lessThan(c, vec3(0.04045)));
}

void main() {
	vec4 source = texelFetch(source_color, ivec2(gl_FragCoord.xy), 0);
	if (pc.mode == 1u) {
		frag_color = vec4(gamma_to_linear(source.rgb), source.a);
	} else {
		frag_color = source;
	}
}
)GLSL";

Ref<RDUniform> sampled_texture_uniform(int binding, const RID &sampler,
		const RID &texture) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	uniform->set_binding(binding);
	uniform->add_id(sampler);
	uniform->add_id(texture);
	return uniform;
}

WorldEnvironment *find_world_environment(Node *root) {
	if (root == nullptr)
		return nullptr;
	if (WorldEnvironment *environment =
				Object::cast_to<WorldEnvironment>(root))
		return environment;
	for (int i = 0; i < root->get_child_count(); ++i) {
		if (WorldEnvironment *environment =
					find_world_environment(root->get_child(i)))
			return environment;
	}
	return nullptr;
}

WorldEnvironment *world_environment_from_id(const ObjectID &id) {
	if (!id.is_valid())
		return nullptr;
	return Object::cast_to<WorldEnvironment>(
			ObjectDB::get_instance(static_cast<std::uint64_t>(id)));
}

// A compositor carrying every effect of `previous` (minus any earlier copy of
// `terminal`) with `terminal` appended last: the display decode must run after
// every other post-transparent effect.
Ref<Compositor> compositor_with_terminal(const Ref<Compositor> &previous,
		const Ref<CompositorEffect> &terminal) {
	TypedArray<Ref<CompositorEffect>> effects;
	if (previous.is_valid()) {
		const TypedArray<Ref<CompositorEffect>> previous_effects =
				previous->get_compositor_effects();
		for (int64_t i = 0; i < previous_effects.size(); ++i) {
			Ref<CompositorEffect> existing = previous_effects[i];
			if (existing.is_null() ||
					existing->get_instance_id() == terminal->get_instance_id())
				continue;
			effects.push_back(existing);
		}
	}
	effects.push_back(terminal);
	Ref<Compositor> compositor;
	compositor.instantiate();
	compositor->set_compositor_effects(effects);
	return compositor;
}

} // namespace

class DisplayDecodeEffect::Impl {
public:
	struct ViewTarget {
		RID color;
		RID color_framebuffer;
		RID scene_scratch;
		RID scene_scratch_framebuffer;
		RID color_uniform;
		RID scratch_uniform;
		Vector2i size;
	};

	mutable std::mutex diagnostics_mutex;
	std::string status = "waiting_for_frame";
	std::string failure;
	bool callback_seen = false;
	bool rd_available = false;
	std::uint64_t rendered_frames = 0;
	std::size_t gpu_draw_calls = 0;
	std::size_t view_count = 0;
	Vector2i last_size;
	std::atomic<bool> shutdown_requested{false};

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	std::map<int64_t, RID> pipelines;
	std::vector<ViewTarget> targets;
	std::uint64_t target_buffers_id = 0;
	PackedByteArray push_constants;

	// RenderingServer owns the RenderingDevice. release_device_resources()
	// frees live RIDs explicitly; destruction can occur after server teardown
	// and must not query or call through that process-owned singleton.
	~Impl() = default;

	void set_failure(const std::string &reason,
			const std::string &next_status = "failed") {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = next_status;
		failure = reason;
	}

	void release_rid(RID &rid) {
		if (rd != nullptr && rid.is_valid())
			rd->free_rid(rid);
		rid = RID();
	}

	void release_uniform(RID &rid) {
		if (rd != nullptr && rid.is_valid() && rd->uniform_set_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_texture(RID &rid) {
		if (rd != nullptr && rid.is_valid() && rd->texture_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_framebuffer(RID &rid) {
		if (rd != nullptr && rid.is_valid() && rd->framebuffer_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_pipeline(RID &rid) {
		if (rd != nullptr && rid.is_valid() &&
				rd->render_pipeline_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_target(ViewTarget &target) {
		release_uniform(target.scratch_uniform);
		release_uniform(target.color_uniform);
		release_framebuffer(target.scene_scratch_framebuffer);
		release_framebuffer(target.color_framebuffer);
		release_texture(target.scene_scratch);
		target = ViewTarget();
	}

	void release_targets() {
		for (ViewTarget &target : targets)
			release_target(target);
		targets.clear();
		target_buffers_id = 0;
	}

	void release_all() {
		RenderingServer *server = RenderingServer::get_singleton();
		rd = server != nullptr ? server->get_rendering_device() : nullptr;
		release_targets();
		for (auto &entry : pipelines)
			release_pipeline(entry.second);
		pipelines.clear();
		release_rid(sampler);
		release_rid(shader);
	}

	bool initialize_rd();
	bool ensure_targets(RenderSceneBuffersRD *buffers,
			std::uint32_t count, const Vector2i &size);
	RID pipeline_for(int64_t framebuffer_format);
	bool draw_one(const RID &framebuffer, const RID &uniform,
			DecodePass pass);
	bool render(RenderData *render_data);
	Dictionary report() const;
};

bool DisplayDecodeEffect::Impl::initialize_rd() {
	if (shutdown_requested.load(std::memory_order_acquire))
		return false;
	if (rd != nullptr && shader.is_valid() && sampler.is_valid())
		return true;
	release_all();
	RenderingServer *server = RenderingServer::get_singleton();
	rd = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		set_failure("RenderingDevice is unavailable; the display decode "
				"requires Forward+ or Mobile", "compatibility_renderer_unsupported");
		return false;
	}

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kRdFullscreenVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kDecodeFragmentShader));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		set_failure("RenderingDevice returned no SPIR-V for the display decode",
				"shader_compile_failed");
		return false;
	}
	const String vertex_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_VERTEX);
	const String fragment_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!vertex_error.is_empty() || !fragment_error.is_empty() ||
			spirv->get_stage_bytecode(
					RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			spirv->get_stage_bytecode(
					RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
		set_failure("Display-decode shader compilation failed: vertex=" +
				std::string(vertex_error.utf8().get_data()) + "; fragment=" +
				std::string(fragment_error.utf8().get_data()),
				"shader_compile_failed");
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova DisplayDecode");
	if (!shader.is_valid()) {
		set_failure("RenderingDevice rejected the display-decode shader",
				"shader_create_failed");
		return false;
	}

	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler = rd->sampler_create(sampler_state);
	if (!sampler.is_valid()) {
		set_failure("RenderingDevice could not create the display-decode sampler",
				"sampler_create_failed");
		release_all();
		return false;
	}
	push_constants.resize(kPushConstantBytes);
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		rd_available = true;
		status = "ready";
		failure.clear();
	}
	return true;
}

bool DisplayDecodeEffect::Impl::ensure_targets(
		RenderSceneBuffersRD *buffers, std::uint32_t count,
		const Vector2i &size) {
	if (buffers == nullptr || count == 0 || size.x <= 0 || size.y <= 0) {
		set_failure("Compositor callback has invalid scene render buffers",
				"render_targets_invalid");
		return false;
	}
	const std::uint64_t buffers_id = buffers->get_instance_id();
	bool matches = target_buffers_id == buffers_id && targets.size() == count;
	if (matches) {
		for (std::uint32_t view = 0; view < count; ++view) {
			const ViewTarget &target = targets[view];
			if (target.size != size || target.color != buffers->get_color_layer(view) ||
					!target.color_framebuffer.is_valid() ||
					!rd->framebuffer_is_valid(target.color_framebuffer)) {
				matches = false;
				break;
			}
		}
	}
	if (matches)
		return true;

	release_targets();
	targets.reserve(count);
	for (std::uint32_t view = 0; view < count; ++view) {
		ViewTarget target;
		target.color = buffers->get_color_layer(view);
		target.size = size;
		const Ref<RDTextureFormat> color_format =
				rd->texture_get_format(target.color);
		if (!target.color.is_valid() || color_format.is_null()) {
			set_failure("Resolved scene color is unavailable for the display "
					"decode view " + std::to_string(view), "render_targets_invalid");
			release_target(target);
			release_targets();
			return false;
		}
		TypedArray<RID> color_attachments;
		color_attachments.push_back(target.color);
		target.color_framebuffer = rd->framebuffer_create(color_attachments);
		Ref<RDTextureFormat> scratch_format;
		scratch_format.instantiate();
		scratch_format->set_format(color_format->get_format());
		scratch_format->set_width(size.x);
		scratch_format->set_height(size.y);
		scratch_format->set_depth(1);
		scratch_format->set_array_layers(1);
		scratch_format->set_mipmaps(1);
		scratch_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		scratch_format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
		scratch_format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT));
		Ref<RDTextureView> scratch_view;
		scratch_view.instantiate();
		target.scene_scratch = rd->texture_create(scratch_format, scratch_view);
		TypedArray<RID> scratch_attachments;
		scratch_attachments.push_back(target.scene_scratch);
		target.scene_scratch_framebuffer =
				rd->framebuffer_create(scratch_attachments);
		{
			TypedArray<Ref<RDUniform>> uniforms;
			uniforms.push_back(sampled_texture_uniform(0, sampler, target.color));
			target.color_uniform = rd->uniform_set_create(uniforms, shader, 0);
		}
		{
			TypedArray<Ref<RDUniform>> uniforms;
			uniforms.push_back(
					sampled_texture_uniform(0, sampler, target.scene_scratch));
			target.scratch_uniform = rd->uniform_set_create(uniforms, shader, 0);
		}
		const bool valid = target.color_framebuffer.is_valid() &&
				target.scene_scratch.is_valid() &&
				target.scene_scratch_framebuffer.is_valid() &&
				target.color_uniform.is_valid() && target.scratch_uniform.is_valid();
		if (!valid) {
			set_failure("RenderingDevice could not allocate the display-decode "
					"target chain for view " + std::to_string(view),
					"render_targets_failed");
			release_target(target);
			release_targets();
			return false;
		}
		targets.push_back(target);
	}
	target_buffers_id = buffers_id;
	return true;
}

RID DisplayDecodeEffect::Impl::pipeline_for(int64_t framebuffer_format) {
	const auto found = pipelines.find(framebuffer_format);
	if (found != pipelines.end())
		return found->second;

	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_1);
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test(false);
	depth->set_enable_depth_write(false);
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(false);
	Ref<RDPipelineColorBlendState> color_blend;
	color_blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	color_blend->set_attachments(attachments);
	RID pipeline = rd->render_pipeline_create(shader, framebuffer_format,
			RenderingDevice::INVALID_FORMAT_ID,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample,
			depth, color_blend);
	if (!pipeline.is_valid()) {
		set_failure("RenderingDevice rejected a display-decode pipeline",
				"pipeline_create_failed");
		return RID();
	}
	pipelines.emplace(framebuffer_format, pipeline);
	return pipeline;
}

bool DisplayDecodeEffect::Impl::draw_one(const RID &framebuffer,
		const RID &uniform, DecodePass pass) {
	const int64_t format = rd->framebuffer_get_format(framebuffer);
	const RID pipeline = pipeline_for(format);
	if (!pipeline.is_valid() || !uniform.is_valid())
		return false;
	std::uint32_t words[4] = {static_cast<std::uint32_t>(pass), 0, 0, 0};
	std::memcpy(push_constants.ptrw(), words, sizeof(words));
	const int64_t draw_list = rd->draw_list_begin(framebuffer,
			RenderingDevice::DRAW_IGNORE_COLOR_ALL, PackedColorArray());
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin a display-decode draw list",
				"draw_list_failed");
		return false;
	}
	rd->draw_list_bind_render_pipeline(draw_list, pipeline);
	rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
	rd->draw_list_set_push_constant(draw_list, push_constants,
			kPushConstantBytes);
	rd->draw_list_draw(draw_list, false, 1, 3);
	rd->draw_list_end();
	return true;
}

bool DisplayDecodeEffect::Impl::render(RenderData *render_data) {
	if (!initialize_rd() || render_data == nullptr) {
		if (render_data == nullptr)
			set_failure("Display-decode callback received no RenderData",
					"render_data_missing");
		return false;
	}
	Ref<RenderSceneBuffers> generic_buffers =
			render_data->get_render_scene_buffers();
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(
			generic_buffers.ptr());
	if (buffers == nullptr) {
		set_failure("Display decode requires RenderSceneBuffersRD",
				"render_data_unsupported");
		return false;
	}
	const std::uint32_t count = buffers->get_view_count();
	const Vector2i size = buffers->get_internal_size();
	if (!ensure_targets(buffers, count, size))
		return false;
	std::size_t draws = 0;
	for (std::uint32_t view = 0; view < count; ++view) {
		ViewTarget &target = targets[view];
		// All 3D retail draws have blended as gamma-domain numeric values. Copy
		// once, then apply the display-backend transfer immediately before
		// Godot's sRGB output encoding. Canvas/viewmodel/HUD passes run
		// afterward.
		if (!draw_one(target.scene_scratch_framebuffer, target.color_uniform,
				DecodePass::Snapshot))
			return false;
		++draws;
		if (!draw_one(target.color_framebuffer, target.scratch_uniform,
				DecodePass::GammaDecode))
			return false;
		++draws;
	}
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = "drawn";
		failure.clear();
		++rendered_frames;
		gpu_draw_calls = draws;
		view_count = count;
		last_size = size;
	}
	return true;
}

Dictionary DisplayDecodeEffect::Impl::report() const {
	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	Dictionary result;
	result["backend"] = "rendering_device_display_decode";
	result["callback"] = "post_transparent_terminal";
	result["callback_type"] = static_cast<int>(
			CompositorEffect::EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	result["framebuffer_blend_domain"] = "gamma";
	result["terminal_transfer"] = "srgb_inverse_then_display_encode";
	result["callback_seen"] = callback_seen;
	result["rd_available"] = rd_available;
	result["status"] = String::utf8(status.c_str());
	result["failure"] = String::utf8(failure.c_str());
	result["rendered_frames"] = static_cast<int64_t>(rendered_frames);
	result["gpu_draw_calls"] = static_cast<int64_t>(gpu_draw_calls);
	result["view_count"] = static_cast<int64_t>(view_count);
	result["frame_size"] = last_size;
	result["shutdown"] = shutdown_requested.load(std::memory_order_acquire);
	return result;
}

DisplayDecodeEffect::DisplayDecodeEffect() :
		impl_(std::make_unique<Impl>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_enabled(true);
}

DisplayDecodeEffect::~DisplayDecodeEffect() = default;

void DisplayDecodeEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&DisplayDecodeEffect::get_backend_report);
}

void DisplayDecodeEffect::release_device_resources() {
	set_enabled(false);
	if (!impl_ || impl_->shutdown_requested.exchange(true,
			std::memory_order_acq_rel))
		return;
	impl_->release_all();
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	impl_->rd_available = false;
	impl_->status = "shutdown";
	impl_->failure.clear();
}

Dictionary DisplayDecodeEffect::get_backend_report() const {
	return impl_ ? impl_->report() : Dictionary();
}

void DisplayDecodeEffect::_render_callback(
		int32_t p_effect_callback_type, RenderData *p_render_data) {
	if (!impl_ || impl_->shutdown_requested.load(std::memory_order_acquire))
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->callback_seen = true;
	}
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT) {
		impl_->set_failure("Display decode invoked at the wrong callback",
				"callback_mismatch");
		return;
	}
	impl_->render(p_render_data);
}

void DisplayDecode::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&DisplayDecode::get_backend_report);
	ClassDB::bind_method(D_METHOD("shutdown"), &DisplayDecode::shutdown);
}

void DisplayDecode::install() {
	if (shutdown_)
		return;
	if (effect_.is_null())
		effect_.instantiate();
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	WorldEnvironment *world_environment = find_world_environment(scope);
	if (world_environment != nullptr) {
		world_environment_id_ = ObjectID(world_environment->get_instance_id());
		previous_compositor_ = world_environment->get_compositor();
		installed_compositor_ = compositor_with_terminal(previous_compositor_,
				effect_);
		world_environment->set_compositor(installed_compositor_);
		return;
	}
	// No WorldEnvironment in scope: attach the compositor to the viewport's
	// scenario directly. A view without a WorldEnvironment has no compositor
	// of its own, so there is nothing to merge or restore beyond clearing it.
	Viewport *viewport = get_viewport();
	world_ = viewport != nullptr ? viewport->find_world_3d() : Ref<World3D>();
	RenderingServer *server = RenderingServer::get_singleton();
	if (world_.is_null() || server == nullptr)
		return;
	installed_compositor_ = compositor_with_terminal(Ref<Compositor>(), effect_);
	server->scenario_set_compositor(world_->get_scenario(),
			installed_compositor_->get_rid());
}

void DisplayDecode::uninstall() {
	Ref<DisplayDecodeEffect> effect = effect_;
	if (effect.is_valid())
		effect->set_enabled(false);
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	if (world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_)
		world_environment->set_compositor(previous_compositor_);
	RenderingServer *server = RenderingServer::get_singleton();
	if (world_.is_valid() && server != nullptr)
		server->scenario_set_compositor(world_->get_scenario(), RID());
	world_environment_id_ = ObjectID();
	world_.unref();
	installed_compositor_.unref();
	previous_compositor_.unref();
	// Detaching only changes the next render setup. Drain any callback already
	// submitted before freeing the RenderingDevice objects it may still read.
	if (effect.is_valid() && server != nullptr &&
			server->get_rendering_device() != nullptr)
		server->force_sync();
	if (effect.is_valid())
		effect->release_device_resources();
	effect_.unref();
	effect.unref();
}

void DisplayDecode::shutdown() {
	if (shutdown_)
		return;
	shutdown_ = true;
	uninstall();
}

Dictionary DisplayDecode::get_backend_report() const {
	Dictionary result = effect_.is_valid() ? effect_->get_backend_report() :
			Dictionary();
	result["shutdown"] = shutdown_;
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	result["terminal_compositor_installed"] =
			(world_environment != nullptr &&
					world_environment->get_compositor() == installed_compositor_) ||
			(world_.is_valid() && installed_compositor_.is_valid());
	return result;
}

void DisplayDecode::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Re-entry after an exit-tree (or explicit) shutdown: the released
		// effect is rebuilt by the READY leg below, so clear the latch and ask
		// for that leg again (READY fires only once on its own).
		if (shutdown_) {
			shutdown_ = false;
			request_ready();
		}
	} else if (p_what == NOTIFICATION_READY) {
		install();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		// Latch so a re-enter re-installs through the ENTER_TREE leg above.
		shutdown_ = true;
		uninstall();
	}
}
