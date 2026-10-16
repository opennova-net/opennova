#pragma once

#include <functional>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <runtime/renderer/particle_frame.h>

namespace godot {

// The FirstPerson particle domain as one retained ArrayMesh on the viewmodel
// layer (the diagnosed tool path; the World domain draws through the
// compositor effects and ParticleFarPass). Its surfaces are rebuilt per frame,
// one per draw command, while the mesh RID, the instance binding and the
// packed upload scratch survive across frames.
class ParticleFirstPersonBatch {
public:
	using MaterialFor = std::function<Ref<ShaderMaterial>(
			const opennova::renderer::ParticleDrawCommand &)>;

	// Creates the batch instance under `p_owner` once.
	void ensure(Node *p_owner);
	// Rebuilds the surfaces from a FirstPerson draw list; a hidden renderer
	// keeps them built but invisible.
	void upload(const opennova::renderer::ParticleDrawList &p_draw_list, bool p_hidden,
			const MaterialFor &p_material_for);
	// Drops the surfaces and hides the instance.
	void clear();
	bool has_surfaces() const;
	// The renderer's hidden switch: visible only while unhidden with surfaces.
	void set_hidden(bool p_hidden);

private:
	MeshInstance3D *instance_ = nullptr;
	Ref<ArrayMesh> mesh_;
	PackedVector3Array vertices_;
	PackedVector2Array uvs_;
	PackedColorArray colors_;
	PackedByteArray custom0_;
	PackedInt32Array indices_;
	Array arrays_;
};

} // namespace godot
