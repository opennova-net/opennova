#pragma once

#include <vector>

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
	// add_surface_from_arrays, keeping the arrays for surface_arrays().
	void add_retained_surface(Mesh::PrimitiveType p_primitive, const Array &p_arrays);
	// The retained arrays of one surface (empty when none were retained).
	Array retained_surface_arrays(int p_surface) const;

private:
	std::vector<Array> surfaces_;
};

// A surface's CPU arrays: the retained copy of a RetainedArrayMesh, else the
// arrays read back through the server (r_read_back set).
Array surface_arrays(Mesh *p_mesh, int p_surface, bool &r_read_back);

} // namespace godot
