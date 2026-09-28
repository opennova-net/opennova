#pragma once

#include <vector>

#include <runtime/renderer/model_mesh_prepare.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/array.hpp>

namespace godot {

class Mesh;

// An ArrayMesh that keeps the CPU arrays each surface was built from
// (tangents dropped; the Q3 packers never read them). The slot-capture and Q3
// packers read these instead of Mesh::surface_get_arrays, which reads the
// vertex, attribute, skin and index buffers back from the GPU; under the
// RenderingDevice drivers every such read stalls on all in-flight frames, a
// 20-30 ms hitch the first time a shared model's surfaces are packed.
class RetainedArrayMesh : public ArrayMesh {
	GDCLASS(RetainedArrayMesh, ArrayMesh)

protected:
	static void _bind_methods() {}

public:
	// add_surface_from_arrays, keeping the arrays for surface_arrays() and,
	// for a skinned surface, its per-bone bind boxes
	// (renderer::prepared_surface_bone_boxes) for retained_surface_bone_boxes().
	void add_retained_surface(Mesh::PrimitiveType p_primitive, const Array &p_arrays,
			std::vector<opennova::renderer::BoneBindBox> p_bone_boxes = {});
	// The retained arrays of one surface (empty when none were retained).
	Array retained_surface_arrays(int p_surface) const;
	// The retained per-bone bind boxes of one surface (empty for a rigid
	// surface): the source of the posed culling box of a strip the object
	// shaders pose from the model's bone palette
	// (ObjectModel::build_skin_palette).
	const std::vector<opennova::renderer::BoneBindBox> &retained_surface_bone_boxes(
			int p_surface) const;

private:
	struct RetainedSurface {
		Array arrays;
		std::vector<opennova::renderer::BoneBindBox> bone_boxes;
	};
	std::vector<RetainedSurface> surfaces_;
};

// A surface's CPU arrays: the retained copy of a RetainedArrayMesh, else the
// arrays read back through the server (r_read_back set).
Array surface_arrays(Mesh *p_mesh, int p_surface, bool &r_read_back);

} // namespace godot
