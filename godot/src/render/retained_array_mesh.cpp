#include "render/retained_array_mesh.h"

#include <godot_cpp/classes/mesh.hpp>

#include <utility>

namespace godot {

void RetainedArrayMesh::add_retained_surface(Mesh::PrimitiveType p_primitive,
		const Array &p_arrays, std::vector<opennova::renderer::BoneBindBox> p_bone_boxes) {
	add_surface_from_arrays(p_primitive, p_arrays);
	Array retained = p_arrays.duplicate(false);
	if (retained.size() > Mesh::ARRAY_TANGENT)
		retained[Mesh::ARRAY_TANGENT] = Variant();
	surfaces_.push_back(RetainedSurface{retained, std::move(p_bone_boxes)});
}

Array RetainedArrayMesh::retained_surface_arrays(int p_surface) const {
	if (p_surface < 0 || static_cast<std::size_t>(p_surface) >= surfaces_.size())
		return Array();
	return surfaces_[static_cast<std::size_t>(p_surface)].arrays;
}

const std::vector<opennova::renderer::BoneBindBox> &
RetainedArrayMesh::retained_surface_bone_boxes(int p_surface) const {
	static const std::vector<opennova::renderer::BoneBindBox> kNone;
	if (p_surface < 0 || static_cast<std::size_t>(p_surface) >= surfaces_.size())
		return kNone;
	return surfaces_[static_cast<std::size_t>(p_surface)].bone_boxes;
}

Array surface_arrays(Mesh *p_mesh, int p_surface, bool &r_read_back) {
	r_read_back = false;
	if (p_mesh == nullptr)
		return Array();
	if (const RetainedArrayMesh *retained = Object::cast_to<RetainedArrayMesh>(p_mesh)) {
		const Array arrays = retained->retained_surface_arrays(p_surface);
		if (!arrays.is_empty())
			return arrays;
	}
	r_read_back = true;
	return p_mesh->surface_get_arrays(p_surface);
}

} // namespace godot
