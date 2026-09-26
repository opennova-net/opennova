#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <array>
#include <memory>
#include <unordered_map>

#include <runtime/environment/celestial_frame.h>
#include <runtime/renderer/q3_frame.h>

#include "env/env_file.h"
#include "env/glare_occlusion.h"
#include "object/object_data.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain_data.h"

namespace godot {

class MissionEnvironment;
class ObjectModel;

// The celestial applier — the ADR 0033 device leg over the engine's
// per-frame body selection (environment/celestial_frame.h) and the witnessed
// fixed-point math already in engine/formats/env (env_celestial.h behind the
// GlareOcclusion device helper). Renders the sun/moon/glare 3DI bodies named
// in the mission .env, attached to the sky at camera + direction * 64; retail
// loads the star 3DI but never draws it (env_celestial.h carries the
// witness). This node keeps only device work: the ObjectModel children with
// per-surface material installs, per-frame shader-parameter pushes, and the
// two terrain line-of-sight rays the glare occlusion window consumes. Each
// body renders its authored SELFLUM material at its own UPL_INTENSITY
// submit value (celestial_frame.h). Ported from
// celestial.gd (2026-08-10 de-scripting); RE record:
// docs/env/env-tod-re.md "Celestial bodies".
class Celestial : public Node3D {
	GDCLASS(Celestial, Node3D)

public:
	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }
	// The loaded terrain for the glare occlusion rays; no terrain =
	// unobstructed.
	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const { return terrain_data_; }
	void set_resource_root(const Ref<ResourceRoot> &p_root);
	void set_environment_capture_layer_mask(uint32_t p_mask);
	uint32_t get_environment_capture_layer_mask() const {
		return environment_capture_layer_mask_;
	}

	// Whether a source surface's material was classified additive at its
	// ObjectModel creation seam (the FF_ST_AD* rows: the stock sun/moon/glare
	// models author FF_ST_AD_LUM with opaque black as the additive zero). A
	// material without a registered classification is never additive; no
	// shader text is inspected.
	static bool source_material_uses_additive(const Ref<Material> &p_source);

	// The glow and the glint for the post-particle overlay stage (the render
	// order owner draws them): the ObjectModel rendering the authored
	// material, the frame's UPL_INTENSITY submit value (16.16) and whether
	// retail submits it (the last advanced frame's values).
	struct OverlayBody {
		ObjectModel *model = nullptr;
		int32_t upl = 0;
		bool drawn = false;
	};
	struct OverlayBodies {
		OverlayBody glare;
		OverlayBody glint;
	};
	OverlayBodies get_overlay_bodies() const;
	// The script read of the same seam: the node for "glare" or "glint"
	// (null when the mission names no such model).
	Node3D *get_overlay_body_node(const String &p_body) const;

	// The water mirror's post-dim redraw (retail render_main_scene after the
	// dim: render_celestial_bodies(0) @ 0x5c18fb, render_skybox_sun_glow(0, 0)
	// @ 0x5c1904): the discs at their beauty submit value (their live
	// materials), the glow at the no-occlusion value of the MIRROR camera's
	// view dot (env::mirror_glare_upl) with each surface's SelfLumColor
	// evaluated there. The bodies sit at `anchor` + direction * 64; the
	// mirror places them at its own camera.
	struct MirrorRedraw {
		ObjectModel *sun = nullptr;
		ObjectModel *moon = nullptr;
		ObjectModel *glare = nullptr;
		bool glare_drawn = false;
		std::unordered_map<const MeshInstance3D *, std::array<float, 3>> glare_self_lum;
		Vector3 anchor;
	};
	MirrorRedraw get_mirror_redraw(const Vector3 &p_mirror_forward);

	// Which scene passes draw the sun/moon discs this frame: they ride the sky
	// bracket (OcclusionFrame owns the gates; renderer/scene_pass_gates.h
	// carries the witnesses). The glare and glint are never gated here.
	void set_sky_pass_gates(bool p_beauty_drawn, bool p_mirror_drawn);
	bool is_sky_beauty_pass_drawn() const { return sky_beauty_pass_drawn_; }
	bool is_sky_mirror_pass_drawn() const { return sky_mirror_pass_drawn_; }

	// One render-frame advance — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	// The frozen-fixture glare settle (GameWorld's capture-refresh seam):
	// the occlusion brightness needs ~4 frames to fill the 8-sample window
	// and up to 16 more to step +-16 onto the dead-band target
	// [orig: render_skybox_sun_glow @ 0x5acdfb..0x5acf7f, see docs/env/env-tod-re.md], so a single
	// zero-delta advance leaves a fresh accumulator dark at any pose. Runs
	// ONLY the witnessed per-frame occlusion leg (two jittered rays + tick)
	// until the dead-band holds across a full window turnover (frame cap
	// p_max_frames); the next advance_frame publishes the settled
	// brightness. Returns the settled brightness.
	int settle_glare_occlusion(int p_max_frames = 64);

	// Read-only celestial diagnostics for the render-diagnostics snapshot:
	// glare occlusion brightness/window, the sun-veil pair, and per-body
	// opacity/visibility (the last advanced frame's values).
	Dictionary get_diagnostics() const;

	// The last advanced frame's sun-veil outputs [orig: // Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0, see docs/env/env-tod-re.md]: the fullscreen
	// white veil alpha (0..1 after the > 2 draw gate — consumed by the
	// PlayerViewEffects veil rect via the opennova_sun_veil_alpha shader
	// global this node pushes) and the modulator-2 exposure stop-down input
	// (0..40 — the world's veil leg forwards it to
	// Weather.set_sun_veil_stopdown).
	float get_sun_veil_alpha() const;
	int get_sun_veil_stopdown() const;

	void _ready() override;

protected:
	static void _bind_methods();

private:
	struct Body {
		ObjectModel *model = nullptr;
		// The model's surface-slot meshes, their authored materials and the
		// 3DI material index each renders (parallel arrays).
		Vector<MeshInstance3D *> meshes;
		Vector<Ref<ShaderMaterial>> materials;
		Vector<int> material_indices;
		// The sun/moon discs ride the sky bracket (far pin + pass gates).
		bool disc = false;
		// The bloom-pass redraw this body registers (none for the glint).
		opennova::renderer::Q3Source q3_source = opennova::renderer::Q3Source::CelestialBody;
		bool q3_drawn = false;
		// The model scene build the surfaces above were bound from.
		uint32_t build_serial = 0;
		// The last advanced frame's UPL_INTENSITY submit value (16.16) and
		// whether retail submitted the draw.
		int32_t last_upl = 0;
		int32_t last_q3_upl = 0;
		bool drawn = true;
	};

	MissionEnvironment *_env_node();
	Ref<EnvFile> _env_data();
	// The active render camera (cached; re-resolved when it leaves the tree
	// or loses currency) — the advance_frame/settle shared resolution.
	Camera3D *_resolve_camera();
	void _rebuild_if_needed();
	// No sun model / no loaded environment: publish a zero veil so the
	// overlay never holds the previous mission's alpha across a load.
	void _publish_idle_veil();
	Ref<ObjectData> _load_object_data(const String &p_graphic);
	static void _collect_meshes(Node *p_node, Vector<MeshInstance3D *> &r_out);
	void _stamp_environment_capture_layer(Node3D *p_model);
	// Bind a body's live surfaces: collect them, register the Q3 redraw and
	// stamp the static sky-hook parameters.
	void _bind_body_surfaces(Body &p_body);
	void _rebind_rebuilt_bodies();
	void _set_body_parameter(const Body &p_body, const StringName &p_parameter,
			const Variant &p_value);
	void _set_body_upl(Body &p_body, int32_t p_upl, int32_t p_q3_upl);
	// Terrain line-of-sight between two points (the glare and water-glint
	// visibility rays): clear when the raycast misses.
	bool _segment_clear(const Vector3 &p_from, const Vector3 &p_to);
	// One frame of the glare occlusion at this camera: the engine's ray
	// sequence (GlareOcclusion::advance) over _segment_clear.
	void _advance_glare_occlusion(const opennova::env::EnvironmentState &p_state,
			const Vector3 &p_cam_pos, const Vector3 &p_sun_dir);
	// One frame of the water-glint leg [orig: update_sun_glare @ 0x5ad130, see docs/env/env-tod-re.md]:
	// tick the accumulator at this camera, place the mirrored glint body and
	// write its submit value.
	void _advance_water_glint(const opennova::env::EnvironmentState &p_state,
			const Vector3 &p_cam_pos, const Vector3 &p_sun_dir,
			const Vector3 &p_forward, Body &p_body);

	NodePath environment_path_;
	Ref<TerrainData> terrain_data_;
	Ref<ResourceRoot> resource_root_;
	std::unique_ptr<GlareOcclusion> glare_occlusion_;
	HashMap<String, Body> bodies_;
	// name-signature change detection (undo/scrub safe rebuilds).
	HashMap<String, String> loaded_names_;
	ObjectID env_node_id_;
	ObjectID cached_cam_id_;
	uint32_t environment_capture_layer_mask_ = 0;
	bool sky_beauty_pass_drawn_ = true;
	bool sky_mirror_pass_drawn_ = true;
	// The camera position the last advance placed the bodies at.
	Vector3 body_anchor_;
	// Stamp the current sky pass gates onto the sun/moon materials.
	void _apply_sky_pass_gates();
	// The last advanced frame's sun-veil pair (env_celestial.h SunVeil).
	int sun_veil_glare_ = 0;
	int sun_veil_stopdown_ = 0;
	// The water-reflected sun glint accumulator
	// [orig: update_sun_glare @ 0x5ad130, see docs/env/env-tod-re.md].
	opennova::env::WaterGlintState water_glint_;
};

} // namespace godot
