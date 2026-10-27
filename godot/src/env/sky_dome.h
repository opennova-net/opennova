#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <runtime/environment/sky_frame.h>

namespace godot {

class MissionEnvironment;
class Weather;

// The sky-dome applier — the ADR 0033 device leg over the engine's
// env::build_sky_frame (engine/runtime/environment) and the witnessed 21x21
// dome mesh (env::build_sky_dome_mesh, built ONCE at the reference height:
// the Y-only height scale + anisotropic normals live in the vertex shader,
// so height changes never rebuild — env #20's ratified fold; retail re-bakes
// per smoothed-height change). This node keeps only device work: the
// ArrayMesh/ShaderMaterial ownership (the gradient pass and its cloud-pass
// next_pass on the sky-cloud rung), per-frame shader-parameter pushes from
// the typed SkyFrameState, the camera-anchored dome position, change-detected
// cloud Texture2D binds, and the per-scene-pass draw gates. The frame clear is
// not the dome's: GameWorld::update_frame_clear_color issues it.
// Ported from sky.gd (2026-08-10 de-scripting); RE record:
// docs/env/env-tod-re.md.
class SkyDome : public Node3D {
	GDCLASS(SkyDome, Node3D)

public:
	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }
	void set_weather_path(const NodePath &p_path);
	NodePath get_weather_path() const { return weather_path_; }
	void set_environment_capture_layer_mask(uint32_t p_mask);
	uint32_t get_environment_capture_layer_mask() const {
		return environment_capture_layer_mask_;
	}

	void build();
	bool is_built() const { return built_; }
	Ref<ShaderMaterial> get_sky_material() const { return sky_material_; }
	// The cloud pass (dome pass 2) rides the gradient material's next_pass
	// while the environment authors cloud layers on the shader path.
	Ref<ShaderMaterial> get_cloud_material() const { return cloud_material_; }
	MeshInstance3D *get_mesh_instance() const { return mesh_instance_; }

	// Which scene passes draw the dome this frame: the main frame's sky
	// bracket and the water mirror's own (OcclusionFrame owns the gates;
	// renderer/scene_pass_gates.h carries the witnesses).
	void set_pass_gates(bool p_beauty_drawn, bool p_mirror_drawn);
	bool is_beauty_pass_drawn() const { return beauty_pass_drawn_; }
	bool is_mirror_pass_drawn() const { return mirror_pass_drawn_; }
	// The frame uniforms pushed to the materials so far: an unchanged frame pushes none (a read seam).
	int64_t get_frame_parameter_pushes() const { return frame_param_pushes_; }

	// One render-frame advance — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;

protected:
	static void _bind_methods();

private:
	MissionEnvironment *_env_node();
	Weather *_weather_node();
	void _update_cloud_textures(MissionEnvironment *p_env);
	// One uniform onto both dome passes (the shared sky.gdshaderinc stage).
	void _set_dome_parameter(const StringName &p_name, const Variant &p_value);
	// The per-frame uniforms, in kFrameParams' order (sky_dome.cpp): each pushed only when its value
	// moved, as the slot shadow's drape pushes are. An unchanged push still marks the material
	// dirty, and the server rebuilds a dirty material's uniform buffer at the next draw
	// (D-NET-1353).
	enum FrameParam {
		FRAME_FLAT_PASS,
		FRAME_FLAT_COLOR,
		FRAME_SKY_BASE,
		FRAME_SKY_BRIGHT,
		FRAME_SKY_HIGHLIGHT,
		FRAME_CLOUD_BASE,
		FRAME_CLOUD_HIGHLIGHT,
		FRAME_CLOUD_EDGE,
		FRAME_SUN_DIR,
		FRAME_LIGHT_DIR,
		FRAME_FOG_COLOR,
		FRAME_FOG_END,
		FRAME_SKY_HEIGHT,
		FRAME_SCROLL_OFFSET1,
		FRAME_SCROLL_OFFSET2,
		FRAME_PARAM_COUNT,
	};
	void _set_frame_parameter(FrameParam p_param, const Variant &p_value);
	// Attach or drop the cloud pass: the shader path with a bound layer.
	void _apply_cloud_pass(bool p_drawn);

	NodePath environment_path_;
	NodePath weather_path_;
	MeshInstance3D *mesh_instance_ = nullptr;
	Ref<ShaderMaterial> sky_material_;
	Ref<ShaderMaterial> cloud_material_;
	bool built_ = false;
	bool has_clouds_ = false;
	bool flat_pass_ = false;
	bool beauty_pass_drawn_ = true;
	bool mirror_pass_drawn_ = true;
	Ref<Texture2D> bound_cloud_tex1_;
	Ref<Texture2D> bound_cloud_tex2_;
	ObjectID env_node_id_;
	ObjectID weather_node_id_;
	ObjectID cached_cam_id_;
	uint32_t environment_capture_layer_mask_ = 0;
	// Standalone fallback (owners with no weather node): the engine-owned
	// private scroll core + 62 Hz credit.
	opennova::env::ScrollFallback fallback_scroll_;
	// The per-frame uniforms' names (made once) and the values last pushed (build() clears them).
	StringName frame_param_names_[FRAME_PARAM_COUNT];
	Variant frame_param_written_[FRAME_PARAM_COUNT];
	bool frame_param_pushed_[FRAME_PARAM_COUNT] = {};
	int64_t frame_param_pushes_ = 0;
};

} // namespace godot
