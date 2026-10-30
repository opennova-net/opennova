#include "simulation/throwable_presenter.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <formats/mission/mission.h>

#include "object/object_model.h"
#include "simulation/simulation.h"
#include "util/axes.h"
#include "util/string_convert.h"

namespace godot {

void ThrowablePresenter::_bind_methods() {}

Simulation *ThrowablePresenter::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

Node3D *ThrowablePresenter::container() const {
	return container_id_.is_valid()
			? Object::cast_to<Node3D>(ObjectDB::get_instance(container_id_))
			: nullptr;
}

void ThrowablePresenter::setup(Simulation *p_sim, Node3D *p_container,
		const Ref<MissionObjectPlacer> &p_placer, const Ref<ItemDatabase> &p_item_db) {
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	container_id_ = p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	placer_ = p_placer;
	item_db_ = p_item_db;
}

Ref<ThrowablePresentStats> ThrowablePresenter::get_stats() const {
	Ref<ThrowablePresentStats> stats;
	stats.instantiate();
	stats->live = static_cast<int64_t>(models_.size());
	return stats;
}

void ThrowablePresenter::present() {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	std::vector<opennova::world::ThrowableVisualRow> rows;
	s->fill_throwable_visual_rows(rows);
	present_visuals(rows);
}

// The rows cross in mission space; the position axis-maps to Godot (x, z, -y)
// here and the rotation stays the placer euler (pitch, MISSION yaw, roll).
void ThrowablePresenter::present_visuals(
		const std::vector<opennova::world::ThrowableVisualRow> &p_visuals) {
	Node3D *container_node = container();
	if (container_node == nullptr) {
		return;
	}
	HashSet<int64_t> seen;
	for (const opennova::world::ThrowableVisualRow &entry : p_visuals) {
		const int64_t key = entry.key;
		if (key < 0) {
			continue;
		}
		seen.insert(key);
		const int item_id = entry.item_id;
		const Vector3 pos = mission_to_godot(entry.pos);
		const Vector3 rot(entry.pitch_deg, entry.yaw_deg, entry.roll_deg);
		// The model reads the authoritative round transform.
		const Transform3D next_transform(bms_to_godot_basis(rot), pos);
		ModelSlot *rec = models_.getptr(key);
		if (rec == nullptr || rec->item_id != item_id) {
			if (rec != nullptr) {
				free_model(*rec);
			}
			ModelSlot built;
			if (!build_model(item_id, entry.mirror_collected, built)) {
				// unresolved graphic: remember the miss so we do not re-try
				// the build every frame
				ModelSlot miss;
				miss.item_id = item_id;
				models_[key] = miss;
				continue;
			}
			models_[key] = built;
			rec = models_.getptr(key);
		}
		Node3D *node = rec->node.is_valid()
				? Object::cast_to<Node3D>(ObjectDB::get_instance(rec->node))
				: nullptr;
		if (node == nullptr) {
			continue;
		}
		// the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		if (node->get_transform() != next_transform) {
			node->set_transform(next_transform);
		}
	}
	// release models whose sim state is gone (detonated, converted, removed)
	Vector<int64_t> gone;
	for (const KeyValue<int64_t, ModelSlot> &kv : models_) {
		if (!seen.has(kv.key)) {
			gone.push_back(kv.key);
		}
	}
	for (int64_t key : gone) {
		free_model(models_[key]);
		models_.erase(key);
	}
}

void ThrowablePresenter::apply_round_draws(const std::vector<int64_t> &p_keys, bool p_inset) {
	HashSet<int64_t> drawn;
	for (const int64_t key : p_keys) {
		drawn.insert(key);
	}
	for (const KeyValue<int64_t, ModelSlot> &kv : models_) {
		if ((kv.key & opennova::world::kThrowableDeviceKeyBit) != 0 || !kv.value.node.is_valid()) {
			continue;
		}
		ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(kv.value.node));
		if (model == nullptr) {
			continue;
		}
		const bool hidden = !drawn.has(kv.key);
		if (p_inset) {
			model->set_inset_occlusion_hidden(hidden);
		} else if (model->is_occlusion_hidden() != hidden) {
			model->set_occlusion_hidden(hidden);
		}
	}
}

bool ThrowablePresenter::build_model(int p_item_id, bool p_mirror_collected, ModelSlot &r_slot) {
	if (placer_.is_null() || item_db_.is_null()) {
		return false;
	}
	const int def_id = p_item_id + opennova::mission::kItemIdOffset;
	const String graphic = item_db_->get_graphic(def_id);
	if (graphic.is_empty()) {
		return false;
	}
	Node3D *container_node = container();
	ObjectModel *model = placer_->build_model_from_graphic(
			graphic, String(), container_node, String(), String(), true);
	if (model == nullptr) {
		return false;
	}
	model->set_name(vformat("Throwable_%d", p_item_id));
	// A flying round's model leaves both water mirrors on either side of the
	// water (its row's population is the RoundPool, which no reflected collect
	// walks: world::ThrowableVisualRow::mirror_collected); a placed device keeps
	// the pool-1 entity's mirror rule.
	model->set_mirror_collected(p_mirror_collected);
	r_slot.node = model->get_instance_id();
	r_slot.item_id = p_item_id;
	return true;
}

void ThrowablePresenter::free_model(const ModelSlot &p_slot) {
	if (!p_slot.node.is_valid()) {
		return;
	}
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(p_slot.node));
	if (node != nullptr) {
		node->queue_free();
	}
}

void ThrowablePresenter::reset_runtime_state() {
	for (const KeyValue<int64_t, ModelSlot> &kv : models_) {
		free_model(kv.value);
	}
	models_.clear();
}

void ThrowablePresenter::teardown() {
	reset_runtime_state();
}

} // namespace godot
