#include "render/tracer_ribbon_surfaces.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "util/color_convert.h"

namespace godot {

namespace {

using opennova::renderer::TracerShader;

// The normal-pass materials (godot/shaders/tracer_ribbon_*.gdshader).
const char *ribbon_shader_path(TracerShader p_shader) {
	switch (p_shader) {
		case TracerShader::Smoke:
			return "res://shaders/tracer_ribbon_smoke.gdshader";
		case TracerShader::NvgLaser:
			return "res://shaders/tracer_ribbon_nvg.gdshader";
		default:
			return "res://shaders/tracer_ribbon_stock.gdshader";
	}
}

} // namespace

void TracerRibbonSurfaces::set_resource_root(const Ref<ResourceRoot> &p_root) {
	if (resource_root_ == p_root) {
		return;
	}
	resource_root_ = p_root;
	smoke_texture_.unref();
	smoke_texture_loaded_ = false;
	for (Ref<ShaderMaterial> &material : materials_) {
		material.unref();
	}
}

Ref<Texture2D> TracerRibbonSurfaces::smoke_texture() {
	if (smoke_texture_loaded_ || resource_root_.is_null()) {
		return smoke_texture_;
	}
	smoke_texture_loaded_ = true;
	smoke_texture_ = resource_root_->load_texture(opennova::renderer::kEmitterPoolTexture,
			ResourceRoot::TEXTURE_LOADER_ARCHIVE_SELF_ALPHA);
	return smoke_texture_;
}

Ref<ShaderMaterial> TracerRibbonSurfaces::material(TracerShader p_shader, bool p_fog_black) {
	const std::size_t slot = static_cast<std::size_t>(p_shader) * 2u + (p_fog_black ? 1u : 0u);
	if (slot >= materials_.size()) {
		return Ref<ShaderMaterial>();
	}
	Ref<ShaderMaterial> &material = materials_[slot];
	if (material.is_null()) {
		Ref<Shader> shader = ResourceLoader::get_singleton()->load(ribbon_shader_path(p_shader));
		if (shader.is_null()) {
			return Ref<ShaderMaterial>();
		}
		material.instantiate();
		material->set_shader(shader);
		material->set_shader_parameter("fog_black", p_fog_black);
		if (p_shader != TracerShader::Stock) {
			material->set_shader_parameter("smoke_tex", smoke_texture());
		}
	}
	return material;
}

void TracerRibbonSurfaces::emit(const opennova::renderer::TracerRibbonFrame &p_frame, const Ref<ArrayMesh> &p_mesh,
		int p_rung) {
	if (p_mesh.is_null()) {
		return;
	}
	std::size_t run_start = 0;
	for (std::size_t d = 1; d <= p_frame.draws.size(); ++d) {
		if (d < p_frame.draws.size() && p_frame.draws[d].shader == p_frame.draws[run_start].shader &&
				p_frame.draws[d].fog_black == p_frame.draws[run_start].fog_black) {
			continue;
		}
		emit_run_(p_frame, run_start, d, p_mesh, p_rung);
		run_start = d;
	}
}

// One run of consecutive same-material draws as one indexed surface. The draws' vertices are contiguous, so the
// run re-bases its indices on its first vertex.
void TracerRibbonSurfaces::emit_run_(const opennova::renderer::TracerRibbonFrame &p_frame, std::size_t p_first_draw,
		std::size_t p_end_draw, const Ref<ArrayMesh> &p_mesh, int p_rung) {
	if (p_first_draw >= p_end_draw) {
		return;
	}
	const opennova::renderer::TracerDraw &first = p_frame.draws[p_first_draw];
	const opennova::renderer::TracerDraw &last = p_frame.draws[p_end_draw - 1];
	const Ref<ShaderMaterial> ribbon = material(first.shader, first.fog_black);
	if (ribbon.is_null()) {
		return;
	}
	ribbon->set_render_priority(p_rung);
	const std::uint32_t base = first.first_vertex;
	const std::uint32_t vertex_end = last.first_vertex + last.vertex_count;
	const int64_t vertex_count = static_cast<int64_t>(vertex_end - base);
	const std::uint32_t index_end = last.first_index + last.index_count;
	PackedVector3Array positions;
	PackedColorArray colors;
	PackedVector2Array uv0;
	PackedVector2Array uv1;
	PackedInt32Array indices;
	positions.resize(vertex_count);
	colors.resize(vertex_count);
	uv0.resize(vertex_count);
	uv1.resize(vertex_count);
	indices.resize(static_cast<int64_t>(index_end - first.first_index));
	for (int64_t v = 0; v < vertex_count; ++v) {
		const opennova::renderer::TracerVertex &src = p_frame.vertices[base + v];
		positions.set(v, Vector3(src.x, src.y, src.z));
		colors.set(v, opennova::color_from_argb(src.argb));
		uv0.set(v, Vector2(src.u0, src.v0));
		uv1.set(v, Vector2(src.u1, src.v1));
	}
	for (std::uint32_t k = first.first_index; k < index_end; ++k) {
		indices.set(static_cast<int64_t>(k - first.first_index), static_cast<int32_t>(p_frame.indices[k] - base));
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_TEX_UV] = uv0;
	arrays[Mesh::ARRAY_TEX_UV2] = uv1;
	arrays[Mesh::ARRAY_INDEX] = indices;
	p_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	p_mesh->surface_set_material(p_mesh->get_surface_count() - 1, ribbon);
}

} // namespace godot
