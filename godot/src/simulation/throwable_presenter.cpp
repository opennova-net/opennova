#include "simulation/throwable_presenter.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <formats/mission/mission.h>

#include "object/object_model.h"
#include "particle/effect_spawn_records.h"
#include "particle/effect_world.h"
#include "simulation/effect_owner_keys.h"
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

EffectWorld *ThrowablePresenter::fx() const {
	return fx_id_.is_valid()
			? Object::cast_to<EffectWorld>(ObjectDB::get_instance(fx_id_))
			: nullptr;
}

void ThrowablePresenter::setup(Simulation *p_sim, Node3D *p_container,
		const Ref<MissionObjectPlacer> &p_placer, const Ref<ItemDatabase> &p_item_db,
		EffectWorld *p_fx, const Ref<ItemEffectDirector> &p_anchors) {
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	container_id_ = p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	placer_ = p_placer;
	item_db_ = p_item_db;
	fx_id_ = p_fx != nullptr ? p_fx->get_instance_id() : ObjectID();
	anchors_ = p_anchors;
}

Ref<ThrowablePresentStats> ThrowablePresenter::get_stats() const {
	Ref<ThrowablePresentStats> stats;
	stats.instantiate();
	stats->live = static_cast<int64_t>(models_.size());
	stats->move_effects = static_cast<int64_t>(move_effects_.size());
	stats->move_effect_transforms = static_cast<int64_t>(move_effect_transforms_.size());
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
	sync_move_effects(p_visuals);
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
		// The model and the continuously attached ammo "move" particle read the
		// same authoritative round transform. [orig: tag-1 -> AmmoDef+0x70 at
		// @0x409fc2; spawn/update @0x4e9f58/@0x4ea8ae/@0x5f7410.]
		const Transform3D next_transform(bms_to_godot_basis(rot), pos);
		ModelSlot *rec = models_.getptr(key);
		if (rec == nullptr || rec->item_id != item_id) {
			if (rec != nullptr) {
				free_model(*rec);
			}
			ModelSlot built;
			if (!build_model(item_id, built)) {
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

void ThrowablePresenter::sync_fixed_tick_effects() {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	std::vector<opennova::world::ThrowableVisualRow> rows;
	s->fill_throwable_visual_rows(rows);
	sync_move_effects(rows);
}

void ThrowablePresenter::sync_move_effects(
		const std::vector<opennova::world::ThrowableVisualRow> &p_visuals) {
	HashSet<int64_t> seen;
	for (const opennova::world::ThrowableVisualRow &entry : p_visuals) {
		const int64_t key = entry.key;
		if (key < 0) {
			continue;
		}
		seen.insert(key);
		const Transform3D transform(
				bms_to_godot_basis(Vector3(entry.pitch_deg, entry.yaw_deg, entry.roll_deg)),
				mission_to_godot(entry.pos));
		// The sim's emitter liveness (the round+0x1CC handle mirror): a row
		// whose emitter is released — a ClipWaterFx round under the water
		// plane — retires its group and forgets the handle, so the same round
		// spawns a FRESH group on surfacing. The release is not latched.
		// [orig: Projectile_UpdatePhysics @0x4ea019..0x4ea03e — the
		//  ammoFlags & 0x20000000 release arm; the lazy spawn @0x4e9f58]
		present_move_effect(key, opennova::to_gd(entry.move_effect), transform,
				entry.move_effect_live);
	}
	Vector<int64_t> gone;
	for (const KeyValue<int64_t, MoveEffect> &kv : move_effects_) {
		if (!seen.has(kv.key)) {
			gone.push_back(kv.key);
		}
	}
	for (int64_t key : gone) {
		retire_move_effect(key);
	}
}

void ThrowablePresenter::present_move_effect(int64_t p_key, const String &p_effect,
		const Transform3D &p_transform, bool p_live) {
	bool has_rec = move_effects_.has(p_key);
	if (p_effect.is_empty() || !p_live) {
		if (has_rec) {
			retire_move_effect(p_key);
		} else {
			move_effect_transforms_.erase(p_key);
		}
		return;
	}
	if (has_rec && move_effects_[p_key].effect != p_effect) {
		retire_move_effect(p_key);
		has_rec = false;
	}
	move_effect_transforms_[p_key] = p_transform;
	if (has_rec) {
		return;
	}
	EffectWorld *fx_world = fx();
	if (fx_world == nullptr || anchors_.is_null()) {
		move_effect_transforms_.erase(p_key);
		return;
	}
	const String owner_key = throwable_move_owner_key(p_key);
	anchors_->register_effect_anchor(owner_key,
			callable_mp(this, &ThrowablePresenter::resolve_move_effect_anchor).bind(p_key));
	const Ref<EffectSpawnReceipt> receipt = fx_world->spawn_effect_owned_request(
			owner_key, p_effect, p_transform.origin, p_transform.basis.get_column(2));
	if (receipt.is_null() || !receipt->get_spawned()) {
		anchors_->unregister_effect_anchor(owner_key);
		move_effect_transforms_.erase(p_key);
		fx_world->release_effect_binding(owner_key);
		return;
	}
	MoveEffect rec;
	rec.effect = p_effect;
	rec.owner_key = owner_key;
	rec.group_id = receipt->get_group_id();
	move_effects_[p_key] = rec;
}

Variant ThrowablePresenter::resolve_move_effect_anchor(int64_t p_key) {
	if (!move_effects_.has(p_key)) {
		return Variant();
	}
	const Transform3D *transform = move_effect_transforms_.getptr(p_key);
	return transform != nullptr ? Variant(*transform) : Variant();
}

void ThrowablePresenter::retire_move_effect(int64_t p_key) {
	const MoveEffect *found = move_effects_.getptr(p_key);
	const bool had_rec = found != nullptr;
	const MoveEffect rec = had_rec ? *found : MoveEffect();
	move_effects_.erase(p_key);
	move_effect_transforms_.erase(p_key);
	if (!had_rec) {
		return;
	}
	const String owner_key = rec.owner_key;
	if (anchors_.is_valid()) {
		anchors_->unregister_effect_anchor(owner_key);
	}
	const int64_t group_id = rec.group_id;
	EffectWorld *fx_world = fx();
	if (fx_world == nullptr) {
		return;
	}
	if (group_id > 0) {
		// Stop emission in the same presenter pass that observes round removal;
		// already-live particles drain naturally. [orig:
		// Projectile_ReleaseEffects @0x4e8280 -> Entity_ReleaseEffectEmitter @0x5f75d0.]
		fx_world->stop_group(group_id);
	}
	fx_world->release_effect_binding(owner_key);
}

bool ThrowablePresenter::build_model(int p_item_id, ModelSlot &r_slot) {
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
	Vector<int64_t> keys;
	for (const KeyValue<int64_t, MoveEffect> &kv : move_effects_) {
		keys.push_back(kv.key);
	}
	for (int64_t key : keys) {
		retire_move_effect(key);
	}
	move_effect_transforms_.clear();
}

void ThrowablePresenter::teardown() {
	reset_runtime_state();
}

} // namespace godot
