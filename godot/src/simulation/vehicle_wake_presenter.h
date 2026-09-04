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

class EffectWorld;
class EntityIndex;
class EntityPresenter;
class ItemEffectDirector;
class ObjectModel;
class Simulation;

// Fixed-tick device owner for the cbot W3/W4 wake groups. The portable world
// decides cadence, liveness, pose, water plane, and magnitudes; this class only
// resolves authored model userpoints and reconciles EffectWorld ownership.
class VehicleWakePresenter : public RefCounted {
	GDCLASS(VehicleWakePresenter, RefCounted)

public:
	void setup(Simulation *p_sim, EntityPresenter *p_entities, const Ref<EntityIndex> &p_index,
			EffectWorld *p_fx, const Ref<ItemEffectDirector> &p_anchors);
	void sync_fixed_tick_effects();
	void sync_visuals(const std::vector<opennova::world::VehicleWakeVisualRow> &p_visuals);
	void reset_runtime_state();
	void teardown();

	Variant resolve_wake_anchor(String p_key);

protected:
	static void _bind_methods();

private:
	struct WakeGroup {
		String effect;
		String owner_key;
		int64_t group_id = 0;
	};

	Simulation *sim() const;
	EntityPresenter *entities() const;
	EffectWorld *fx() const;
	ObjectModel *resolve_model(const opennova::world::VehicleWakeVisualRow &p_row) const;
	void sync_lane(const opennova::world::VehicleWakeVisualRow &p_row, ObjectModel *p_model,
			int p_slot, const std::string &p_effect, const std::string &p_userpoint,
			uint32_t p_magnitude_q16, HashSet<String> &r_seen);
	void present_group(const String &p_key, const String &p_effect, const Transform3D &p_transform,
			float p_magnitude);
	void retire_group(const String &p_key);

	ObjectID sim_id_;
	ObjectID entities_id_;
	Ref<EntityIndex> index_;
	ObjectID fx_id_;
	Ref<ItemEffectDirector> anchors_;
	HashMap<String, Transform3D> transforms_;
	HashMap<String, WakeGroup> groups_;
};

} // namespace godot
