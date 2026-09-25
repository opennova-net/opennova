#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

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

// The World particle pass on the water's far side (retail pass A) as
// transparent render-list geometry. Retail submits it after the far-side
// tracers and before the far-side foliage and the water surface (retail
// Terrain_RenderWorldScene @ 0x5c95ac..0x5c95dc), a slot no
// compositor callback reaches, so every non-distortion draw command of the
// compiled far-side list becomes one run instance at kRungParticleFarSide
// (the material's render priority). Runs share one sort origin (the world
// origin, sorting_use_aabb_center off) and take increasing sorting offsets, so
// the transparent sort draws them in the compiler's (retail's) order. Each run
// carries the view it was compiled for; the far-pass shaders drop it in every
// other eye (render/visual_layers.h: the main view is the camera carrying the
// viewmodel layer, the second scene view the one that draws the water layer
// without it, the mirror draws no water layer). Distortion commands stay with the
// compositor distortion path. The instance pool, its meshes and the upload
// scratch survive across frames.
class ParticleFarPass {
public:
	using MaterialFor = std::function<Ref<ShaderMaterial>(
			const opennova::renderer::ParticleDrawCommand &)>;

	enum class View : int {
		Main = 0,
		SecondScene = 1,
	};
	// Rebuilds the runs from `p_draw_list` for `p_view`, parenting new
	// instances to `p_owner`.
	void upload(Node *p_owner, const opennova::renderer::ParticleDrawList &p_draw_list,
			View p_view, const MaterialFor &p_material_for);
	// Hides every run (a frame without this view, or hidden particles). The
	// pooled instances are children of the renderer node and die with it.
	void clear();
	std::size_t run_count() const { return live_runs_; }

private:
	struct Run {
		MeshInstance3D *instance = nullptr;
		Ref<ArrayMesh> mesh;
	};
	Run &run_at(Node *p_owner, std::size_t p_index);

	std::vector<Run> runs_;
	std::size_t live_runs_ = 0;
	PackedVector3Array vertices_;
	PackedVector2Array uvs_;
	PackedColorArray colors_;
	PackedByteArray custom0_;
	PackedInt32Array indices_;
	Array arrays_;
};

} // namespace godot
