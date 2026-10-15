#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

namespace godot {

class FrameFx;
class ParticleRenderer;

// The game's frame effects on a preview device's world (ADR 0046 S23 C; DI-19b listed them as not drawn): the
// runtime's own FrameFx node as the view's terminal compositor (render/frame_fx.h: its Q3 glow source over the
// device's scope, the planner's rows, the bloom of the sun and the glare the Celestial registers, and the one display
// decode a 3D view of retail's gamma-domain shaders takes, which a DisplayDecode alone did before), and the sun-glare
// veil, the fullscreen white quad whose alpha the Celestial pushes as the `opennova_sun_veil_alpha` shader global every
// advanced frame, drawn over the finished scene as the game's PlayerViewEffects draws it (shaders/sun_veil_overlay
// .gdshader) [the witnesses at their engine and binding homes: FrameFX_BloomKernel @0x5841d0,
// Environment_ApplySunVeilAndExposureStopdown @0x5ad8b0], and the FrameFX distortion row, which a class-7 (Distort)
// particle draws on alone: the heat haze [the witnesses at their engine homes: renderer/particle_frame.h,
// renderer/frame_fx_effects.h]. A device with no local player has the planner's default view facts (no death, no
// NVG, no thermal: the bloom alone).
class PreviewFrameEffects {
public:
	// The game world's ClearColor and FrameFx rows under `root` (world/world_scene.h, in
	// game_world.tscn's order, the clear first): FrameFx installs its terminal compositor, the
	// view's one display decode, on the WorldEnvironment beside it as the game's does; the veil
	// over `viewport`'s picture where `veil` (a picture with a sky whose Celestial pushes it: a
	// model's, an effect's or a definition's has none).
	PreviewFrameEffects(Node3D &root, SubViewport &viewport, bool veil = true);
	// Over a world's own FrameFx (the WorldScene row a mission's preview draws: its load's
	// post-step draws the mission texture into it), the veil over `viewport`'s picture.
	PreviewFrameEffects(FrameFx &frame_fx, SubViewport &viewport);
	// A presented frame's legs, after the environment nodes advanced (the Celestial's veil and glare are this
	// frame's): the Q3 frame compiled at the view's camera, the distortion row drawn by `distortion`'s effects device
	// (its class-7 particles, as the game's frame hands FrameFx the EffectWorld's; null none), the screen effects
	// planned.
	void present(ParticleRenderer *distortion = nullptr);
	FrameFx *frame_fx() const;

private:
	static void add_veil_(SubViewport &viewport);
	uint64_t frame_fx_id_ = 0;
};

} // namespace godot
