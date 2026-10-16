#include "particle/particle_first_person_batch.h"

#include <cstddef>
#include <cstdint>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "render/visual_layers.h"

namespace godot {

namespace {

// The FirstPerson batch rides the viewmodel layer, so every view that masks the
// viewmodel out (the water mirror, the second scene view) drops it as well.
constexpr std::uint32_t kFirstPersonVisibilityMask = visual_layers::VIEWMODEL;
static_assert((visual_layers::SECOND_SCENE_VIEW_EXCLUDED & kFirstPersonVisibilityMask) != 0,
		"a second scene view must never draw the FirstPerson particle batch");

Color unpack_argb(std::uint32_t value) {
	constexpr float inv = 1.0f / 255.0f;
	return Color(
			static_cast<float>((value >> 16) & 0xffu) * inv,
			static_cast<float>((value >> 8) & 0xffu) * inv,
			static_cast<float>(value & 0xffu) * inv,
			static_cast<float>((value >> 24) & 0xffu) * inv);
}

} // namespace

void ParticleFirstPersonBatch::ensure(Node *p_owner) {
	if (instance_ != nullptr)
		return;
	instance_ = memnew(MeshInstance3D);
	instance_->set_name("ParticleFirstPersonBatch");
	instance_->set_cast_shadows_setting(
			GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	instance_->set_extra_cull_margin(200.0f);
	instance_->set_as_top_level(true);
	instance_->set_transform(Transform3D());
	instance_->set_layer_mask(kFirstPersonVisibilityMask);
	p_owner->add_child(instance_);
	instance_->set_owner(p_owner->get_owner());
}

void ParticleFirstPersonBatch::clear() {
	if (mesh_.is_valid())
		mesh_->clear_surfaces();
	if (instance_ != nullptr)
		instance_->set_visible(false);
}

bool ParticleFirstPersonBatch::has_surfaces() const {
	return mesh_.is_valid() && mesh_->get_surface_count() > 0;
}

void ParticleFirstPersonBatch::set_hidden(bool p_hidden) {
	if (instance_ != nullptr)
		instance_->set_visible(!p_hidden && has_surfaces());
}

void ParticleFirstPersonBatch::upload(
		const opennova::renderer::ParticleDrawList &p_draw_list, bool p_hidden,
		const MaterialFor &p_material_for) {
	if (instance_ == nullptr)
		return;
	// One retained ArrayMesh: surfaces are rebuilt per frame while the mesh
	// RID, the instance binding, and the packed scratch arrays survive.
	if (mesh_.is_null())
		mesh_.instantiate();
	if (instance_->get_mesh().ptr() != mesh_.ptr())
		instance_->set_mesh(mesh_);
	mesh_->clear_surfaces();
	if (p_draw_list.commands.empty() || p_draw_list.vertices.empty()) {
		instance_->set_visible(false);
		return;
	}

	if (arrays_.size() != Mesh::ARRAY_MAX)
		arrays_.resize(Mesh::ARRAY_MAX);
	for (const opennova::renderer::ParticleDrawCommand &command : p_draw_list.commands) {
		const std::size_t first_vertex =
				static_cast<std::size_t>(command.first_quad) * 4u;
		const std::size_t vertex_count =
				static_cast<std::size_t>(command.quad_count) * 4u;
		if (first_vertex > p_draw_list.vertices.size() ||
				vertex_count > p_draw_list.vertices.size() - first_vertex)
			continue;

		vertices_.resize(static_cast<int64_t>(vertex_count));
		uvs_.resize(static_cast<int64_t>(vertex_count));
		colors_.resize(static_cast<int64_t>(vertex_count));
		custom0_.resize(static_cast<int64_t>(vertex_count * 4u));
		indices_.resize(static_cast<int64_t>(command.quad_count) * 6);
		Vector3 *vertices = vertices_.ptrw();
		Vector2 *uvs = uvs_.ptrw();
		Color *colors = colors_.ptrw();
		std::uint8_t *custom_bytes = custom0_.ptrw();
		std::int32_t *indices = indices_.ptrw();

		for (std::size_t vertex_index = 0; vertex_index < vertex_count;
				++vertex_index) {
			const opennova::renderer::ParticleVertex &source =
					p_draw_list.vertices[first_vertex + vertex_index];
			vertices[vertex_index] = Vector3(source.x, source.y, source.z);
			uvs[vertex_index] = Vector2(source.u, source.v);
			colors[vertex_index] = unpack_argb(source.primary_color);
			const std::size_t byte_offset = vertex_index * 4u;
			custom_bytes[byte_offset + 0] = static_cast<std::uint8_t>(
					(source.secondary_color >> 16) & 0xffu);
			custom_bytes[byte_offset + 1] = static_cast<std::uint8_t>(
					(source.secondary_color >> 8) & 0xffu);
			custom_bytes[byte_offset + 2] = static_cast<std::uint8_t>(
					source.secondary_color & 0xffu);
			custom_bytes[byte_offset + 3] = static_cast<std::uint8_t>(
					(source.secondary_color >> 24) & 0xffu);
		}

		for (std::uint32_t quad = 0; quad < command.quad_count; ++quad) {
			const std::int32_t vertex = static_cast<std::int32_t>(quad * 4u);
			const std::size_t index = static_cast<std::size_t>(quad) * 6u;
			indices[index + 0] = vertex + 0;
			indices[index + 1] = vertex + 1;
			indices[index + 2] = vertex + 2;
			indices[index + 3] = vertex + 1;
			indices[index + 4] = vertex + 3;
			indices[index + 5] = vertex + 2;
		}

		arrays_[Mesh::ARRAY_VERTEX] = vertices_;
		arrays_[Mesh::ARRAY_TEX_UV] = uvs_;
		arrays_[Mesh::ARRAY_COLOR] = colors_;
		arrays_[Mesh::ARRAY_CUSTOM0] = custom0_;
		arrays_[Mesh::ARRAY_INDEX] = indices_;
		const std::uint64_t flags =
				static_cast<std::uint64_t>(Mesh::ARRAY_FORMAT_CUSTOM0);
		mesh_->add_surface_from_arrays(
				Mesh::PRIMITIVE_TRIANGLES, arrays_,
				TypedArray<Array>(), Dictionary(),
				static_cast<Mesh::ArrayFormat>(flags));
		// The surface owns a converted copy. Drop the Array's shares so the
		// next frame's ptrw() writes in place instead of copying on write.
		arrays_.fill(Variant());
		const int surface = mesh_->get_surface_count() - 1;
		mesh_->surface_set_material(surface, p_material_for(command));
	}

	instance_->set_visible(!p_hidden && mesh_->get_surface_count() > 0);
}

} // namespace godot
