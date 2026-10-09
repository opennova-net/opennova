#pragma once

namespace godot {

class Camera3D;
class Celestial;
class MissionEnvironment;
class SceneOverlayModelSurfaces;
class Water;
struct SceneOverlaySubmission;

// The celestial draws of the post-particle overlay tail (renderer/scene_overlay.h), and the underwater
// murk beside them, one function each, the game's frame (GameWorld's scene_overlay leg) and the editor's
// environment preview (authoring/environment_viewport_applier) alike.

// The underwater murk's full-frame draw at `p_water`'s height, in the environment's lit underwater colour
// and alpha byte (renderer::append_underwater_murk_overlay carries the slot and its witness). The caller asks
// it only while the water renders.
void append_underwater_murk_overlay(const MissionEnvironment &p_env, const Water &p_water,
		SceneOverlaySubmission &r_submission);

// The water glint and the sun glare of the main view through `camera`: Celestial places both models and
// drives their UPL_INTENSITY submit value (the SelfLumColor their SELFLUM materials evaluate) and keeps
// their Q3 copy; the stage draws them at the scene tail with the SELFLUM combine, ONE / ONE, fogged to
// black under the frame's fog, depth ALWAYS (submit 0x110), so their meshes leave every camera (the Q3
// redraw reads the node, not the layers). The glint leg runs only while the mission water height is
// nonzero (retail Environment_UpdateSunGlare @ 0x5c96c0 behind the test @ 0x5c96b5) and draws under the
// frame's own light scale; the glare draws last, under the forced 0xFF404040 modulator (light scale 1.0,
// retail @ 0x5c96fd..0x5c9722). The glare's own gate is the scene's drawShadows argument (test edi
// @ 0x5c970a), which the main view and the scope view both pass as 1. Nothing without the environment's
// light values.
void append_celestial_overlays(SceneOverlayModelSurfaces &r_bodies, Celestial &p_celestial,
		MissionEnvironment &p_env, const Water *p_water, Camera3D &p_camera, SceneOverlaySubmission &r_submission);

// The water mirror's closing draws (runtime/renderer/scene_overlay.h kMirrorOverlayOrder; only the
// mirror's overlay pass admits these slots): the dim over the finished mirror target, then the sun/moon
// discs and the glow redrawn at the MIRROR camera inside the far depth band, fogged by the mirror's own
// dry block (EnvironmentState::build_water_mirror_fog) under the frame's light scale. The discs keep their
// beauty submit value (their live materials); the glow takes the mirror view's no-occlusion value
// (Celestial::get_mirror_redraw). The caller asks it only while the water renders.
void append_water_mirror_overlays(SceneOverlayModelSurfaces &r_bodies, Celestial *p_celestial,
		MissionEnvironment *p_env, Water &p_water, SceneOverlaySubmission &r_submission);

} // namespace godot
