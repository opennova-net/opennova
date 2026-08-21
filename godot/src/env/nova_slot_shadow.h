#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/projection.hpp>

#include <vector>

#include <renderer/render_slot_shadow.h>

namespace godot {

class LightScene;
class MissionEnvironment;
class ObjectModel;
class ResourceRoot;
class Weather;

// The render-slot entity ground-shadow device (the Godot half of
// engine/runtime/renderer/render_slot_shadow.h — the planner carries the
// witness map). Owns the 12 silhouette-capture SubViewports (the retail RT
// chain), assigns per-slot capture layers to the admitted caster models,
// steers each capture camera along the planner's slot direction (the clamped
// sun, or the dominant nearby point light), and publishes the drape
// projection matrices + per-channel shadow terms the terrain drape next-pass
// multiplies in (godot/shaders/slot_shadow_drape.gdshader). Authored
// items.def `shadow` blob decals ride a chained second drape pass for bound
// slots past the capture budget.
//
// Device folds (documented on docs/render/render-lighting-re.md): the drape
// projects per-pixel — the retail 21x21 anchor-marched patch is that
// projection's placement device, so the march needs no separate device leg;
// mounted/standing children capture through their own slots rather than the
// parent's RT walk; the attached-light drape darkening folds the light's
// attenuation at the entity into the per-slot term.
//
// Driven once per display frame by GameFramePipeline through
// GameWorld.render_slot_shadow_frame(), after the light select has pushed
// this frame's LightScene and context into it — never self-clocked.
class SlotShadow : public Node3D {
	GDCLASS(SlotShadow, Node3D)

public:
	// Models flagged as dynamic shadow casters join this group
	// (ObjectModel::set_shadow_caster_enabled).
	static const StringName &caster_group();
	static uint32_t capture_layer_bit(int p_order);
	static uint32_t capture_layer_mask();
	// The shared terrain drape next-pass (silhouette pass chained to the
	// authored-blob pass). Terrain installs it at build.
	static Ref<ShaderMaterial> get_drape_material();
	// Module shutdown hook (register_types uninitialize).
	static void cleanup_statics();

	SlotShadow();

	void set_environment_node(MissionEnvironment *p_environment);
	void set_light_scene(const Ref<LightScene> &p_scene);
	void set_light_context(const Vector3 &p_gain, int p_time_ms,
			Weather *p_weather);
	void set_resource_root(const Ref<ResourceRoot> &p_root);
	// The retail shadow-detail option (0..4) driving the RT chain base and
	// the refresh cadence. The packaged runtime serves the top setting.
	void set_shadow_detail(int p_detail);
	int get_shadow_detail() const { return shadow_detail_; }
	// The local player's model: halved slot priority, the every-frame
	// refresh exception, and the first-person drape gates (detail >= 2, not
	// prone) (retail: RenderSlot_DrawAllDrapes @0x5d6e70..0x5d6e90, see
	// docs/render/render-lighting-re.md).
	void set_local_player_model(Node *p_model);
	void set_local_player_first_person(bool p_first_person);
	void set_local_player_prone(bool p_prone);

	// One display frame: plan, stamp the capture channels, steer the capture
	// cameras, publish the drape terms.
	void advance_frame();
	Dictionary get_report() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct CasterInfo {
		ObjectModel *model = nullptr;
		renderer::SlotCandidateState state;
	};

	void _ensure_captures();
	void _clear_all_terms();
	void _apply_capture_layers(ObjectModel *p_model, uint32_t p_bit);
	Ref<Texture2D> _blob_texture(const String &p_name);
	Projection _drape_projection(const Transform3D &p_pose, float p_half_u,
			float p_half_v, float p_far) const;

	renderer::RenderSlotPlan plan_;
	SubViewport *viewports_[renderer::kSlotCaptureCount] = {};
	Camera3D *cameras_[renderer::kSlotCaptureCount] = {};
	// instance id -> applied capture bit (for removal on churn).
	HashMap<uint64_t, uint32_t> applied_bits_;
	HashMap<String, Ref<Texture2D>> blob_textures_;
	// The per-slot dominant-light query buffer (reused across frames).
	std::vector<renderer::SlotPointLight> slot_lights_;
	ObjectID environment_node_id_;
	Ref<LightScene> light_scene_;
	Vector3 light_gain_ = Vector3(1, 1, 1);
	int light_time_ms_ = 0;
	ObjectID weather_id_;
	Ref<ResourceRoot> resource_root_;
	ObjectID local_player_id_;
	bool local_first_person_ = true;
	bool local_prone_ = false;
	int shadow_detail_ = 4;
	uint32_t frame_ = 0;
	int report_captures_ = 0;
	int report_blobs_ = 0;
	int report_bound_ = 0;

	static Ref<ShaderMaterial> drape_material_;
	static Ref<ShaderMaterial> blob_material_;
};

}  // namespace godot
