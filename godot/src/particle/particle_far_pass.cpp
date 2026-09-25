#include "particle/particle_far_pass.h"

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "render/visual_layers.h"

namespace godot {

namespace {

Color unpack_argb_bytes(std::uint32_t p_value) {
	return Color(static_cast<float>((p_value >> 16) & 0xffu) / 255.0f,
			static_cast<float>((p_value >> 8) & 0xffu) / 255.0f,
			static_cast<float>(p_value & 0xffu) / 255.0f,
			static_cast<float>((p_value >> 24) & 0xffu) / 255.0f);
}

} // namespace

ParticleFarPass::Run &ParticleFarPass::run_at(Node *p_owner, std::size_t p_index) {
	while (runs_.size() <= p_index) {
		Run run;
		run.mesh.instantiate();
		run.instance = memnew(MeshInstance3D);
		run.instance->set_name(String("ParticleFarRun") + String::num_int64(
				static_cast<int64_t>(runs_.size())));
		run.instance->set_mesh(run.mesh);
		run.instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		run.instance->set_as_top_level(true);
		run.instance->set_transform(Transform3D());
		run.instance->set_layer_mask(visual_layers::WORLD);
		// One shared sort origin (the world origin) and an offset per run: the
		// transparent sort then keeps the compiler's order inside the rung.
		run.instance->set_sorting_use_aabb_center(false);
		run.instance->set_sorting_offset(static_cast<float>(runs_.size()));
		run.instance->set_visible(false);
		p_owner->add_child(run.instance);
		runs_.push_back(run);
	}
	return runs_[p_index];
}

void ParticleFarPass::upload(Node *p_owner,
		const opennova::renderer::ParticleDrawList &p_draw_list,
		View p_view, const MaterialFor &p_material_for) {
	std::size_t used = 0;
	if (p_owner != nullptr) {
		if (arrays_.size() != Mesh::ARRAY_MAX)
			arrays_.resize(Mesh::ARRAY_MAX);
		for (const opennova::renderer::ParticleDrawCommand &command : p_draw_list.commands) {
			if (command.pipeline == opennova::renderer::ParticlePipeline::Distort ||
					command.quad_count == 0)
				continue;
			const std::size_t first_vertex = static_cast<std::size_t>(command.first_quad) * 4u;
			const std::size_t vertex_count = static_cast<std::size_t>(command.quad_count) * 4u;
			if (first_vertex > p_draw_list.vertices.size() ||
					vertex_count > p_draw_list.vertices.size() - first_vertex)
				continue;
			const Ref<ShaderMaterial> material = p_material_for(command);
			if (material.is_null())
				continue;

			vertices_.resize(static_cast<int64_t>(vertex_count));
			uvs_.resize(static_cast<int64_t>(vertex_count));
			colors_.resize(static_cast<int64_t>(vertex_count));
			custom0_.resize(static_cast<int64_t>(vertex_count * 4u));
			indices_.resize(static_cast<int64_t>(command.quad_count) * 6);
			Vector3 *vertices = vertices_.ptrw();
			Vector2 *uvs = uvs_.ptrw();
			Color *colors = colors_.ptrw();
			std::uint8_t *custom = custom0_.ptrw();
			std::int32_t *indices = indices_.ptrw();
			for (std::size_t index = 0; index < vertex_count; ++index) {
				const opennova::renderer::ParticleVertex &source =
						p_draw_list.vertices[first_vertex + index];
				vertices[index] = Vector3(source.x, source.y, source.z);
				uvs[index] = Vector2(source.u, source.v);
				colors[index] = unpack_argb_bytes(source.primary_color);
				// The SPECULAR colour as RGBA8 in CUSTOM0.
				custom[index * 4u + 0] = static_cast<std::uint8_t>((source.secondary_color >> 16) & 0xffu);
				custom[index * 4u + 1] = static_cast<std::uint8_t>((source.secondary_color >> 8) & 0xffu);
				custom[index * 4u + 2] = static_cast<std::uint8_t>(source.secondary_color & 0xffu);
				custom[index * 4u + 3] = static_cast<std::uint8_t>((source.secondary_color >> 24) & 0xffu);
			}
			// The compositor's quad triangulation (0 1 2, 1 3 2).
			for (std::uint32_t quad = 0; quad < command.quad_count; ++quad) {
				const std::int32_t vertex = static_cast<std::int32_t>(quad * 4u);
				std::int32_t *out = indices + static_cast<std::size_t>(quad) * 6u;
				out[0] = vertex + 0;
				out[1] = vertex + 1;
				out[2] = vertex + 2;
				out[3] = vertex + 1;
				out[4] = vertex + 3;
				out[5] = vertex + 2;
			}
			arrays_[Mesh::ARRAY_VERTEX] = vertices_;
			arrays_[Mesh::ARRAY_TEX_UV] = uvs_;
			arrays_[Mesh::ARRAY_COLOR] = colors_;
			arrays_[Mesh::ARRAY_CUSTOM0] = custom0_;
			arrays_[Mesh::ARRAY_INDEX] = indices_;

			Run &run = run_at(p_owner, used);
			run.mesh->clear_surfaces();
			run.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays_,
					TypedArray<Array>(), Dictionary(),
					static_cast<Mesh::ArrayFormat>(Mesh::ARRAY_FORMAT_CUSTOM0));
			// The surface owns a converted copy; drop the shares so the next
			// run's ptrw() writes in place.
			arrays_.fill(Variant());
			run.mesh->surface_set_material(0, material);
			run.instance->set_instance_shader_parameter("u_far_view",
					static_cast<int64_t>(p_view));
			run.instance->set_visible(true);
			++used;
		}
	}
	for (std::size_t index = used; index < runs_.size(); ++index) {
		if (runs_[index].instance->is_visible()) {
			runs_[index].mesh->clear_surfaces();
			runs_[index].instance->set_visible(false);
		}
	}
	live_runs_ = used;
}

void ParticleFarPass::clear() {
	for (Run &run : runs_) {
		if (run.instance->is_visible()) {
			run.mesh->clear_surfaces();
			run.instance->set_visible(false);
		}
	}
	live_runs_ = 0;
}

} // namespace godot
