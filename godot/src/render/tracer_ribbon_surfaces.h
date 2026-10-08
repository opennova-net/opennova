#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <array>
#include <cstddef>

#include <runtime/renderer/tracer_frame.h>

#include "resource_index/resource_root.h"

namespace godot {

// The tracer ribbons' device half: the witnessed normal-pass materials, one per (shader, fog) pair
// (godot/shaders/tracer_ribbon_{stock,smoke,nvg}.gdshader, each fogged per its style's +0 word), the pool's one
// texture (smoktest.pcx, read through the resource root), and the upload of a compiled ribbon frame
// (engine/runtime/renderer/tracer_frame.h, the build of CEffectChannel_RenderRibbon @ 0x5DB8A0) as ArrayMesh
// surfaces, one per run of same-material draws in pool order, on the frame's tracer rung. The game's fire
// presenter draws the sim's trails through it, and the editor's weapon preview (ADR 0046 DI-22) its range's.
class TracerRibbonSurfaces {
public:
	// The root the smoke texture is read through: another drops the materials, which bind it.
	void set_resource_root(const Ref<ResourceRoot> &p_root);
	// The material a draw binds, made on first use; null when its shader does not load.
	Ref<ShaderMaterial> material(opennova::renderer::TracerShader p_shader, bool p_fog_black);
	// The pool's one texture, with its palette-luminance alpha (renderer::kEmitterPoolTexture carries the name
	// and the witness); null when the root holds none.
	Ref<Texture2D> smoke_texture();
	// `p_frame`'s draws added to `p_mesh`, one surface per run of consecutive same-material draws, each surface's
	// material on the ladder rung `p_rung` (renderer::tracer_rung).
	void emit(const opennova::renderer::TracerRibbonFrame &p_frame, const Ref<ArrayMesh> &p_mesh, int p_rung);

private:
	void emit_run_(const opennova::renderer::TracerRibbonFrame &p_frame, std::size_t p_first_draw,
			std::size_t p_end_draw, const Ref<ArrayMesh> &p_mesh, int p_rung);

	Ref<ResourceRoot> resource_root_;
	// One material per (normal-pass shader, fog-black) pair, created on first use.
	std::array<Ref<ShaderMaterial>, 6> materials_;
	// smoktest.pcx, the pool+0x3000 texture [orig: CEffectEmitterPool_CreateShaders @ 0x5DC8F0 (the store
	// @ 0x5dc926)].
	Ref<Texture2D> smoke_texture_;
	bool smoke_texture_loaded_ = false;
};

} // namespace godot
