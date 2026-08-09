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
#include <godot_cpp/variant/node_path.hpp>

#include <environment/celestial_frame.h>

#include "env/env_file.h"
#include "env/nova_glare_occlusion.h"
#include "env/nova_star_field.h"
#include "object/nova_object_data.h"
#include "resource_index/nova_resource_root.h"
#include "terrain/nova_terrain_data.h"

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
// nova_celestial.gd (2026-08-10 de-scripting); RE record:
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

	// Whether a source surface uses additive blending that the celestial
	// replacement material must preserve (the stock sun/moon models author
	// FF_ST_AD_LUM with opaque black as the additive zero; forcing those
	// surfaces through blend_mix exposes a dark quad at the horizon).
	static bool source_material_uses_additive(const Ref<Material> &p_source);

	// One render-frame advance (the _process body) — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

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
	};

	MissionEnvironment *_env_node();
	Ref<EnvFile> _env_data();
	void _rebuild_if_needed();
	Ref<ObjectData> _load_object_data(const String &p_graphic);
	Ref<ShaderMaterial> _make_celestial_material(bool p_additive,
			int p_priority);
	Vector<Ref<ShaderMaterial>> _apply_material_override(Node3D *p_model,
			const Ref<ShaderMaterial> &p_base_material);
	static void _collect_meshes(Node *p_node, Vector<MeshInstance3D *> &r_out);
	void _build_star_field(const String &p_star_name);
	void _update_star_field(const Vector3 &p_light_dir);
	void _set_body_parameter(const Body &p_body, const StringName &p_parameter,
			const Variant &p_value);
	bool _glare_ray_clear(const Vector3 &p_from, const Vector3 &p_sun_dir,
			float p_ray_length, const Vector3 &p_jitter);

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
};

} // namespace godot
