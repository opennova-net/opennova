#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <runtime/environment/celestial_frame.h>

#include "env/env_file.h"
#include "env/glare_occlusion.h"
#include "env/star_field.h"
#include "object/object_data.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain_data.h"

namespace godot {

class MissionEnvironment;

// The celestial applier — the ADR 0033 device leg over the engine's
// per-frame body selection (environment/celestial_frame.h) and the witnessed
// fixed-point math already in engine/formats/env (env_celestial.h behind the
// StarField/GlareOcclusion bindings). Renders the sun/moon/glare 3DI bodies
// and the 256-instance star field named in the mission .env, attached to the
// sky at camera + direction * 64. This node keeps only device work: the
// ObjectModel children with per-surface material installs, the star
// MultiMesh, per-frame shader-parameter pushes, and the two terrain
// line-of-sight rays the glare occlusion window consumes. The 3DI diffuse
// stays; bodies are tinted and dimmed by the TOD sun/moon color. Ported from
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
	// ObjectModel creation seam (the FF_ST_AD* rows: the stock sun/moon
	// models author FF_ST_AD_LUM with opaque black as the additive zero, and
	// forcing those surfaces through blend_mix exposes a dark quad at the
	// horizon), so the celestial replacement material must preserve it. A
	// material without a registered classification is never additive; no
	// shader text is inspected.
	static bool source_material_uses_additive(const Ref<Material> &p_source);

	// One render-frame advance (the _process body) — the externally-callable
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
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	struct Body {
		Node3D *model = nullptr;
		Vector<Ref<ShaderMaterial>> materials;
		// "sun" or "moon" — which TOD color tints this body.
		String tint;
		// The last advanced frame's submit opacity (diagnostics).
		float last_opacity = 0.0f;
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
	Ref<ShaderMaterial> _make_celestial_material(bool p_additive,
			int p_priority);
	// One mesh of a body and the blend each installed surface material was
	// given: bit i set = surface i renders through celestial_additive. The
	// mask rides the Q3 registration so the adapter blends the disc the way
	// its material was installed.
	struct InstalledMesh {
		MeshInstance3D *mesh = nullptr;
		uint32_t additive_surfaces = 0;
	};
	struct InstalledMaterials {
		Vector<Ref<ShaderMaterial>> materials;
		Vector<InstalledMesh> meshes;
	};
	InstalledMaterials _apply_material_override(Node3D *p_model,
			const Ref<ShaderMaterial> &p_base_material);
	static void _collect_meshes(Node *p_node, Vector<MeshInstance3D *> &r_out);
	void _stamp_environment_capture_layer(Node3D *p_model);
	void _build_star_field(const String &p_star_name);
	void _update_star_field(const Vector3 &p_light_dir);
	void _set_body_parameter(const Body &p_body, const StringName &p_parameter,
			const Variant &p_value);
	bool _glare_ray_clear(const Vector3 &p_from, const Vector3 &p_sun_dir,
			float p_ray_length, const Vector3 &p_jitter);
	// Terrain line-of-sight between two points (the water-glint visibility
	// rays) — the same clear-when-miss form as _glare_ray_clear.
	bool _segment_clear(const Vector3 &p_from, const Vector3 &p_to);
	// One frame of the water-glint leg [orig: update_sun_glare @ 0x5ad130, see docs/env/env-tod-re.md]:
	// tick the accumulator at this camera, place the mirrored glint body,
	// return its submit alpha (0 hides it).
	float _advance_water_glint(const opennova::env::EnvironmentState &p_state,
			const Vector3 &p_cam_pos, const Vector3 &p_sun_dir,
			const Vector3 &p_forward, Body &p_body);

	NodePath environment_path_;
	Ref<TerrainData> terrain_data_;
	Ref<ResourceRoot> resource_root_;
	Ref<GlareOcclusion> glare_occlusion_;
	Ref<StarField> star_core_;
	MultiMeshInstance3D *star_mmi_ = nullptr;
	HashMap<String, Body> bodies_;
	// name-signature change detection (undo/scrub safe rebuilds).
	HashMap<String, String> loaded_names_;
	ObjectID env_node_id_;
	ObjectID cached_cam_id_;
	Ref<Shader> celestial_shader_;
	Ref<Shader> celestial_additive_shader_;
	uint32_t environment_capture_layer_mask_ = 0;
	// The last advanced frame's sun-veil pair (env_celestial.h SunVeil).
	int sun_veil_glare_ = 0;
	int sun_veil_stopdown_ = 0;
	// The water-reflected sun glint accumulator
	// [orig: update_sun_glare @ 0x5ad130, see docs/env/env-tod-re.md].
	opennova::env::WaterGlintState water_glint_;
};

} // namespace godot
