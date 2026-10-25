#pragma once

// GLSL text the RenderingDevice render legs share: engine constants spliced
// into a shader template's @TOKEN@ slots, so the GLSL never restates them,
// the shared GLSL snippets, and the one vertex + fragment SPIR-V compile.

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include "util/string_convert.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace godot {

// The process's RenderingDevice GLSL compiles so far, their count and their microseconds (an
// instrument: the verbose PipelineCompiles line names a frame that compiled one, beside Godot's
// own pipeline compilations; FrameStats::rd_glsl_compile_totals reads them).
struct RdGlslCompileTotals {
	std::atomic<int64_t> count{0};
	std::atomic<int64_t> usec{0};
};
inline RdGlslCompileTotals &rd_glsl_compile_totals() {
	static RdGlslCompileTotals totals;
	return totals;
}

// One GLSL compile on p_rd, counted and timed into rd_glsl_compile_totals.
inline Ref<RDShaderSPIRV> compile_rd_glsl_source(RenderingDevice *p_rd,
		const Ref<RDShaderSource> &p_source) {
	const auto start = std::chrono::steady_clock::now();
	Ref<RDShaderSPIRV> spirv = p_rd->shader_compile_spirv_from_source(p_source);
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start);
	RdGlslCompileTotals &totals = rd_glsl_compile_totals();
	totals.count.fetch_add(1, std::memory_order_relaxed);
	totals.usec.fetch_add(static_cast<int64_t>(elapsed.count()), std::memory_order_relaxed);
	return spirv;
}

// The process's RenderingDevice pipeline creates so far, render and compute, their count and
// their microseconds (an instrument beside the GLSL compiles: the device passes make their
// pipelines at their first use, which Godot's own pipeline counters do not see;
// FrameStats::rd_pipeline_create_totals reads them).
inline RdGlslCompileTotals &rd_pipeline_create_totals() {
	static RdGlslCompileTotals totals;
	return totals;
}

inline void count_rd_pipeline_create(std::chrono::steady_clock::time_point p_start) {
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - p_start);
	RdGlslCompileTotals &totals = rd_pipeline_create_totals();
	totals.count.fetch_add(1, std::memory_order_relaxed);
	totals.usec.fetch_add(static_cast<int64_t>(elapsed.count()), std::memory_order_relaxed);
}

// RenderingDevice::render_pipeline_create on p_rd, counted and timed into
// rd_pipeline_create_totals.
template <typename... Args>
inline RID create_rd_render_pipeline(RenderingDevice *p_rd, Args &&...p_args) {
	const auto start = std::chrono::steady_clock::now();
	const RID pipeline = p_rd->render_pipeline_create(std::forward<Args>(p_args)...);
	count_rd_pipeline_create(start);
	return pipeline;
}

// RenderingDevice::compute_pipeline_create on p_rd, counted and timed the same way.
template <typename... Args>
inline RID create_rd_compute_pipeline(RenderingDevice *p_rd, Args &&...p_args) {
	const auto start = std::chrono::steady_clock::now();
	const RID pipeline = p_rd->compute_pipeline_create(std::forward<Args>(p_args)...);
	count_rd_pipeline_create(start);
	return pipeline;
}

// A GLSL float literal for an engine constant: %.9g round-trips every float,
// and a trailing ".0" keeps integral values typed as floats.
inline std::string glsl_float(float p_value) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(p_value));
	std::string text(buffer);
	if (text.find_first_of(".eE") == std::string::npos)
		text += ".0";
	return text;
}

inline std::string glsl_vec3(const std::array<float, 3> &p_value) {
	return "vec3(" + glsl_float(p_value[0]) + ", " + glsl_float(p_value[1]) + ", " +
			glsl_float(p_value[2]) + ")";
}

inline void splice_token(std::string &p_text, const char *p_token, const std::string &p_value) {
	const std::string token(p_token);
	for (std::size_t at = p_text.find(token); at != std::string::npos;
			at = p_text.find(token, at + p_value.size()))
		p_text.replace(at, token.size(), p_value);
}

// The object stages' sampLinearWrap2D under the effects' filter mode, as the
// beauty wrappers sample it (shared.gdshaderinc obj_sample_effect_stage),
// clamped at the stage texture's last retail mip level. filter_code is the
// effect stage's shader filter code (engine renderer/texture_filter.h
// shader_filter_code; render/texture_filter_device.h effect_filter_code): 0
// bilinear on the nearest level, 1 trilinear, 2 the 2x anisotropic footprint.
// A fragment template splices it into its @SAMPLE_EFFECT_STAGE@ slot and calls
// rd_sample_effect_stage(texture, uv, max_lod, filter_code).
inline const char *const kGlslSampleEffectStage = R"GLSL(vec4 rd_sample_effect_stage(sampler2D tex, vec2 tex_uv, float max_lod,
		uint filter_code) {
	vec2 size = vec2(textureSize(tex, 0));
	vec2 du = dFdx(tex_uv);
	vec2 dv = dFdy(tex_uv);
	float length_x = length(du * size);
	float length_y = length(dv * size);
	float major = max(length_x, length_y);
	float isotropic_lod = log2(max(major, 1.0e-8));
	if (filter_code == 0u) {
		return textureLod(tex, tex_uv,
				clamp(floor(isotropic_lod + 0.5), 0.0, max(max_lod, 0.0)));
	}
	if (filter_code == 1u) {
		return textureLod(tex, tex_uv, min(isotropic_lod, max_lod));
	}
	float minor = min(length_x, length_y);
	float ratio = clamp(major / max(minor, 1.0e-8), 1.0, 2.0);
	float lod = min(log2(max(major / ratio, 1.0e-8)), max_lod);
	vec2 offset = (length_x >= length_y ? du : dv) * (0.5 - 0.5 / ratio);
	return 0.5 * (textureLod(tex, tex_uv - offset, lod) +
			textureLod(tex, tex_uv + offset, lod));
})GLSL";

// Runs p_create on the render thread when the server has a RenderingDevice: a compositor's
// device objects (its shaders, samplers and fixed targets) created where the renderer creates its
// matching effect, the renderer's start or a mission load's stage, ahead of the compositor's first
// render callback (its callback's own create then finds them). A renderer without one (headless,
// Compatibility) creates nothing here, as its callbacks create nothing.
inline void call_device_create(const Callable &p_create) {
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr && server->get_rendering_device() != nullptr)
		server->call_on_render_thread(p_create);
}

// The SPIR-V of each vertex + fragment pair compile_rd_spirv compiled, by the pair's text, held
// as plain bytes (no Godot object in static storage outlives the engine): the compositors that
// draw one pair (a particle pass per view, an overlay per view) compile it once per process and
// each creates its own shader from the bytes.
struct RdSpirvPairCache {
	std::mutex mutex;
	std::map<std::pair<std::string, std::string>,
			std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>>
			entries;
};
inline RdSpirvPairCache &rd_spirv_pair_cache() {
	static RdSpirvPairCache cache;
	return cache;
}

inline PackedByteArray rd_spirv_packed(const std::vector<std::uint8_t> &p_bytes) {
	PackedByteArray packed;
	packed.resize(static_cast<int64_t>(p_bytes.size()));
	if (!p_bytes.empty())
		std::memcpy(packed.ptrw(), p_bytes.data(), p_bytes.size());
	return packed;
}

inline std::vector<std::uint8_t> rd_spirv_bytes(const PackedByteArray &p_packed) {
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(p_packed.size()));
	if (!bytes.empty())
		std::memcpy(bytes.data(), p_packed.ptr(), bytes.size());
	return bytes;
}

// Compiles one vertex + fragment GLSL pair to SPIR-V on p_rd, once per process
// for each pair's text (rd_spirv_pair_cache). Returns an empty string when
// both stages produced bytecode (r_spirv holds it); otherwise the failure
// text: "no SPIR-V" when the compiler returned nothing, else
// "vertex=<error>; fragment=<error>". The caller creates the shader from
// r_spirv (shader_create_from_spirv) and reports a rejection itself.
inline std::string compile_rd_spirv(RenderingDevice *p_rd, const std::string &p_vertex_glsl,
		const std::string &p_fragment_glsl, Ref<RDShaderSPIRV> &r_spirv) {
	RdSpirvPairCache &cache = rd_spirv_pair_cache();
	const std::pair<std::string, std::string> key(p_vertex_glsl, p_fragment_glsl);
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		const auto found = cache.entries.find(key);
		if (found != cache.entries.end()) {
			r_spirv.instantiate();
			r_spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX,
					rd_spirv_packed(found->second.first));
			r_spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT,
					rd_spirv_packed(found->second.second));
			return std::string();
		}
	}
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			opennova::to_gd(p_vertex_glsl));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			opennova::to_gd(p_fragment_glsl));
	r_spirv = compile_rd_glsl_source(p_rd, source);
	if (r_spirv.is_null())
		return "no SPIR-V";
	const String vertex_error =
			r_spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX);
	const String fragment_error =
			r_spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	const PackedByteArray vertex_bytes =
			r_spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX);
	const PackedByteArray fragment_bytes =
			r_spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (vertex_error.is_empty() && fragment_error.is_empty() && !vertex_bytes.is_empty() &&
			!fragment_bytes.is_empty()) {
		std::lock_guard<std::mutex> lock(cache.mutex);
		cache.entries.emplace(key,
				std::make_pair(rd_spirv_bytes(vertex_bytes), rd_spirv_bytes(fragment_bytes)));
		return std::string();
	}
	return "vertex=" + opennova::to_std(vertex_error) + "; fragment=" +
			opennova::to_std(fragment_error);
}

} // namespace godot
