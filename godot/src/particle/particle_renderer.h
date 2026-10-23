#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "particle/effect_scene.h"

namespace opennova::renderer {
struct ParticleEmitterDrawBounds;
} // namespace opennova::renderer

namespace godot {

class Camera3D;
struct SceneOverlaySubmission;
class EffectDistortionDrawer;

// Thin Godot adapter for the portable particle scene/frame modules. World
// draw lists are immutable values: the water-far subset (pass A) draws as
// render-list runs inside the transparent list at kRungParticleFarSide
// (ParticleFarPass) with only its distortion on a PRE_TRANSPARENT compositor
// effect, the camera-side subset on a POST_TRANSPARENT effect, the mirror on
// two consecutive POST_TRANSPARENT effects, and, while a second scene camera
// is handed in, that view gets its own far runs and camera-side effect; each view's
// camera-side pass is followed by its post-particle overlay pass. Only the
// explicitly diagnosed FirstPerson tool path uses ArrayMesh. Effects,
// emitters, and particles remain values in EffectScene.
class ParticleRenderer : public Node3D {
	GDCLASS(ParticleRenderer, Node3D)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
	Ref<EffectScene> scene_;
	Callable image_provider_;
	String texture_dir_;
	ObjectID environment_source_;
	ObjectID reflection_camera_;
	ObjectID second_scene_camera_;
	ObjectID inset_mirror_camera_;
	float water_height_ = 0.0f;
	// The session's texcompression_level and particle_density (the fresh
	// profile's until an embedder hands the mission's copy over) and whether
	// the main view renders as the NVG scene this frame (D-RMAT-24).
	int session_texcompression_level_ = 1;
	int session_particle_density_ = 2;
	bool main_view_nvg_scene_ = false;
	bool hidden_ = false;
	bool shutdown_ = false;
	bool procedural_fallback_enabled_ = false;
	std::vector<Node *> warm_nodes_;

	void _invalidate_catalog();
	void _restore_device_state();
	void _create_ribbon_device_on_render_thread();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	ParticleRenderer();
	~ParticleRenderer() override;

	void set_scene(const Ref<EffectScene> &p_scene);
	Ref<EffectScene> get_scene() const;

	// What answers a particle frame's name with its decoded Image (the effect
	// world's: the mount's particle loader, ResourceRoot::load_image).
	void set_image_provider(const Callable &p_provider);
	Callable get_image_provider() const;
	void set_texture_dir(const String &p_texture_dir);
	void set_environment_source(Node *p_source);
	Node *get_environment_source() const;
	// One exact render-plane handoff: the height partitions World emitters and
	// the mirror camera receives its own camera-correct pair of submissions.
	void set_water_plane(float p_height, Camera3D *p_reflection_camera);
	// The camera of a second view of the same scene (the weapon Inset pass):
	// the original re-renders the world through it with the very scene routine
	// the main view runs, so that view draws the world's particles too, compiled
	// for its own eye. Null retires the view: its submissions clear and its
	// compositor pair detaches, so a frame without one pays nothing. Only the
	// ObjectID is kept; a freed camera retires the view on the next render.
	void set_second_scene_camera(Camera3D *p_camera);
	Camera3D *get_second_scene_camera() const;
	// The weapon Inset pass's own water mirror camera: its reflected scene
	// runs the mirror's particle pair again for that mirror's eye, on a frame
	// whose Inset pass renders that mirror (Water::advance_inset_frame; engine
	// renderer/particle_frame.h compiles_mirror_particles). Null retires it.
	void set_inset_mirror_camera(Camera3D *p_camera);
	Camera3D *get_inset_mirror_camera() const;
	// The session's game.cfg texcompression_level and particle_density
	// (renderer/texture_compression.h, renderer/particle_density.h): the
	// atlas pages are built again under the flags they give (DXT5 at a
	// compression word of 1 or less, halved once at a density of 1 or less),
	// and every view's particle stride follows the density.
	void set_session_render_settings(int p_texcompression_level, int p_particle_density);
	// Whether the main view renders as the NVG scene this frame, whose stride
	// is twice the main scene's (renderer::ParticleScenePass::NvgScene).
	void set_main_view_nvg_scene(bool p_nvg_scene);

	void set_hidden(bool p_hidden);
	bool get_hidden() const;
	void set_procedural_fallback_enabled(bool p_enabled);
	bool get_procedural_fallback_enabled() const;

	// Deterministic pipeline warm for the loading screen: one centimeter quad
	// per FirstPerson spatial shader plus a one-shot request for all eight
	// World RenderingDevice pipelines on the compositor's real framebuffer.
	// Warming by spawning effects alone is timing-dependent (delayed emitters
	// emit nothing during the warm frames). clear_warm_pipelines frees the
	// quads and cancels an unserviced compositor request.
	void warm_pipelines(const Vector3 &p_position);
	void clear_warm_pipelines();
	// Retires every published submission (both camera views, the far passes,
	// the first-person batch, the distortion row) until the next render_now:
	// what a reset scene leaves draws nothing (EffectWorld::reset_runtime_state).
	void clear_draws();
	// Process-exit boundary for compositor callbacks and device-owned effects.
	// EXIT_TREE calls it too; the retired effects and the latch it leaves
	// behind are undone by the next ENTER_TREE (fresh effects, latch cleared),
	// so a renderer removed from and re-added to the tree renders again.
	void shutdown();
	// FrameFX's type-0 row device: this renderer publishes the class-7
	// distortion subset into it each render; the tracer ribbons join it from
	// the fire presenter. The world registers it with FrameFx.
	std::shared_ptr<EffectDistortionDrawer> distortion_drawer() const;
	// Registers that device with a FrameFx node. The world does this every
	// frame through EffectWorld; previews and tests bind it once.
	void attach_distortion_row(Node *p_frame_fx);
	// The particle materials' device objects as the effect system's atlases build: every
	// particle pass's compositor (each view's pair and the distortion subset) creates its
	// shaders ahead of its first render callback (ParticleCompositorEffect::create_device).
	void create_particle_devices();
	// The tracer pool's materials at the mission textures stage: the distortion ribbon
	// device and each view's overlay stage, whose NVG laser draws the pool's laser material
	// (EffectDistortionDrawer::create_device, SceneOverlayCompositorEffect::create_device).
	void create_tracer_pool_devices();

	// Compiles the latest fixed-tick scene snapshot for both render domains and
	// publishes an immutable World copy across the render-thread boundary.
	// Process-driven rendering calls this automatically; tests and previews may
	// call it explicitly after advancing a scene.
	void render_now(int64_t p_time_ms);
	// The catalog's atlas built now, from the scene's definitions under the
	// session's page flags, when it is not already built for them: the effect
	// system's init builds it once at the mission start, so the first draw
	// builds none (engine renderer/particle_atlas.h carries the witness,
	// D-NET-NEW-L3-26). render_now builds it the same way when it is stale.
	void build_catalog();

	// The post-particle overlay tail (runtime/renderer/scene_overlay.h): every
	// view this renderer composes carries one overlay pass right after its
	// particle pass B (the main view and the scope aperture draw the full
	// tail, the mirror its coronas); one immutable frame reaches all three.
	// Null clears them. Not bound to Godot.
	void publish_scene_overlay(const std::shared_ptr<const SceneOverlaySubmission> &p_submission);

	// Renderer-owned diagnostics are plain values. No MeshInstance or material
	// references escape through the F3/debug seam. They are read from each
	// compiler's retained draw list on demand, so a render spends nothing on
	// them and the first report after render_now() is already live: no
	// capture flag exists to forget.
	int64_t get_rendered_quad_count() const;
	int64_t get_draw_command_count() const;
	Dictionary get_debug_draw_list_report() const;
	Array get_debug_emitter_bounds() const;
	// The native form of get_debug_emitter_bounds for the C++ EffectWorld
	// report (no Dictionary round trip): the compiled emitter bounds of every
	// present draw slot, the world lists first, then first-person. Not bound
	// to Godot.
	void collect_debug_emitter_bounds(
			std::vector<opennova::renderer::ParticleEmitterDrawBounds> &r_out) const;
	PackedStringArray get_unresolved_texture_names() const;
};

} // namespace godot
