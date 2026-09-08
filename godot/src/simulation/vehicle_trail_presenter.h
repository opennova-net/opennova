#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <runtime/world/present_drains.h>

#include <vector>

namespace godot {

class MeshInstance3D;
class ArrayMesh;
class ShaderMaterial;
class EffectWorld;
class ItemEffectDirector;
class Simulation;

// Fixed-tick device owner for the simulation's shared movement-emitter bank.
class VehicleTrailPresenter : public RefCounted {
	GDCLASS(VehicleTrailPresenter, RefCounted)

public:
	void setup(Simulation *p_sim, EffectWorld *p_fx, const Ref<ItemEffectDirector> &p_anchors);
	void sync_fixed_tick_effects();
	void sync_visuals(const std::vector<opennova::world::VehicleTrailVisualRow> &p_visuals);
	void reset_runtime_state();
	void teardown();

	Variant resolve_trail_anchor(String p_key);

protected:
	static void _bind_methods();

private:
	struct TrailGroup {
		String effect;
		String owner_key;
		int64_t group_id = 0;
	};

	void sync_water_wakes();
	ObjectID water_mesh_id_;
	Ref<ArrayMesh> water_mesh_;
	Ref<ShaderMaterial> water_material_;

	Simulation *sim() const;
	EffectWorld *fx() const;
	void present_group(const String &p_key, const String &p_effect, const Transform3D &p_transform,
			float p_magnitude);
	void retire_group(const String &p_key);

	ObjectID sim_id_;
	ObjectID fx_id_;
	Ref<ItemEffectDirector> anchors_;
	HashMap<String, Transform3D> transforms_;
	HashMap<String, TrailGroup> groups_;
};

} // namespace godot
