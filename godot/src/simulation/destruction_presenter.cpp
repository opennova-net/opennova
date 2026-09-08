#include "simulation/destruction_presenter.h"

#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

#include <runtime/world/present_passes.h>

#include "audio/mission_audio.h"
#include "lights/effect_light_director.h"
#include "mission/mission_data.h"
#include "particle/effect_world.h"
#include "render/visual_layers.h"
#include "simulation/effect_owner_keys.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "util/axes.h"

namespace godot {

namespace {

// The engine's presentation identity key, as the HashMap key type.
String identity_key(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle) {
	return String(opennova::world::husk_identity_key(p_bms_id, p_spawn_origin, p_wire_handle).c_str());
}

bool uses_dynamic_husk_identity(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle) {
	return opennova::world::husk_identity_is_dynamic(p_bms_id, p_spawn_origin, p_wire_handle);
}

Node3D *live_node3d(const ObjectID &p_id) {
	return p_id.is_valid() ? Object::cast_to<Node3D>(ObjectDB::get_instance(p_id)) : nullptr;
}

} // namespace

void DestructionPresenter::_bind_methods() {}

Simulation *DestructionPresenter::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

Node3D *DestructionPresenter::container() const {
	return live_node3d(container_id_);
}

MissionAudio *DestructionPresenter::audio() const {
	return audio_id_.is_valid()
			? Object::cast_to<MissionAudio>(ObjectDB::get_instance(audio_id_))
			: nullptr;
}

EffectWorld *DestructionPresenter::fx() const {
	return fx_id_.is_valid()
			? Object::cast_to<EffectWorld>(ObjectDB::get_instance(fx_id_))
			: nullptr;
}

EffectLightDirector *DestructionPresenter::lights() const {
	return lights_id_.is_valid()
			? Object::cast_to<EffectLightDirector>(ObjectDB::get_instance(lights_id_))
			: nullptr;
}

void DestructionPresenter::setup(EntityPresenter *p_owner, Simulation *p_sim, Node3D *p_container,
		const Ref<EntityIndex> &p_index, const Ref<MissionObjectPlacer> &p_placer,
		const Ref<ItemDatabase> &p_item_db, const Ref<ItemEffectDirector> &p_anchors,
		MissionAudio *p_audio, EffectWorld *p_fx, EffectLightDirector *p_lights) {
	owner_ = p_owner;
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	container_id_ = p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	index_ = p_index;
	placer_ = p_placer;
	item_db_ = p_item_db;
	anchors_ = p_anchors;
	audio_id_ = p_audio != nullptr ? p_audio->get_instance_id() : ObjectID();
	fx_id_ = p_fx != nullptr ? p_fx->get_instance_id() : ObjectID();
	lights_id_ = p_lights != nullptr ? p_lights->get_instance_id() : ObjectID();
}

void DestructionPresenter::teardown() {
	reset_runtime_state();
}

void DestructionPresenter::reset_runtime_state() {
	Vector<int> piece_slots;
	for (const KeyValue<int, int64_t> &kv : piece_generation_) {
		piece_slots.push_back(kv.key);
	}
	for (int slot : piece_slots) {
		unregister_piece_anchor(slot);
	}
	piece_generation_.clear();
	piece_pos_.clear();

	if (EffectWorld *effects = fx()) {
		for (const KeyValue<String, int64_t> &group : attached_groups_) {
			effects->stop_group(group.value);
			effects->release_effect_binding(group.key);
		}
	}
	attached_groups_.clear();
	for (const String &key : wreck_anchor_keys_) {
		unregister_effect_anchor(key);
	}
	wreck_anchor_keys_.clear();
	burning_.clear();

	for (const KeyValue<String, HuskRestore> &kv : husk_restore_) {
		const HuskRestore &restore = kv.value;
		const int restored_bms_id = restore.bms_id;
		if (placer_.is_valid() && restored_bms_id != 0) {
			placer_->clear_static_terrain_shadow_replacement(restored_bms_id);
		}
		if (restore.kind == HuskRestore::STATIC) {
			if (placer_.is_valid()) {
				placer_->show_static_instance(restored_bms_id);
			}
			continue;
		}
		for (const HuskRestoreChild &saved : restore.children) {
			Node3D *child = live_node3d(saved.node);
			if (child != nullptr) {
				child->set_visible(saved.visible);
			}
		}
	}
	husk_restore_.clear();

	for (const KeyValue<String, ObjectID> &kv : husked_) {
		Node3D *husk = live_node3d(kv.value);
		if (husk != nullptr) {
			husk->set_visible(false);
			husk->queue_free();
		}
	}
	husked_.clear();
}

void DestructionPresenter::present() {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	opennova::world::DestructionEvents events;
	s->drain_destruction_events(events);
	std::vector<opennova::world::DeathPieceRow> pieces;
	s->fill_death_pieces(pieces);
	present_drained(events, pieces);
}

// The events cross in mission space (x, y, z-up); every position axis-maps to
// Godot (x, z, -y) here, the fire pass's rule.
void DestructionPresenter::present_drained(const opennova::world::DestructionEvents &p_events,
		const std::vector<opennova::world::DeathPieceRow> &p_pieces) {
	for (const opennova::world::HuskSwapEvent &husk : p_events.husk_swaps) {
		apply_husk_swap(husk);
	}
	for (const opennova::world::DestructionEffectEvent &eff : p_events.effects) {
		apply_effect(eff);
	}
	for (const opennova::world::DestructionSoundEvent &sound : p_events.sounds) {
		apply_sound(String::utf8(sound.sound.c_str()), mission_to_godot(sound.pos));
	}
	if (EffectLightDirector *light_director = lights()) {
		for (const opennova::world::DeathLightEvent &light : p_events.death_lights) {
			light_director->on_death_light(mission_to_godot(light.pos), light.radius);
		}
	}
	stat_debris_triangles_ += p_events.debris_triangles;
	stat_glass_points_ += p_events.glass_points;
	// Sim-side rolls (S12b): the crackle EFFECT rides the ordinary effects
	// drain above; its sound rides the fire pass's drain_fire_sounds.
	stat_crackles_ += p_events.crackles;
	sync_static_husks();
	present_pieces(p_pieces);
	tick_wreck_fires();
}

Node3D *DestructionPresenter::resolve_entity_node(int p_bms_id, int64_t p_spawn_origin,
		int p_wire_handle) const {
	const bool dynamic_identity = uses_dynamic_husk_identity(
			p_bms_id, p_spawn_origin, p_wire_handle);
	if (dynamic_identity) {
		// A runtime-only owner has no authored mission identity. Never fall
		// through to the BMS/static lookup when its wire node is unavailable.
		if (owner_ == nullptr) {
			return nullptr;
		}
		return owner_->resolve_wire_handle(p_wire_handle);
	}
	if (index_.is_null()) {
		return nullptr;
	}
	ObjectModel *node = index_->resolve(p_bms_id,
			Simulation::spawn_origin_kind(p_spawn_origin),
			Simulation::spawn_origin_index(p_spawn_origin));
	return node;
}

// The husk model swap: on a per-entity node, hide the intact node's visual
// children and graft the husk model as a child (it inherits the node transform,
// so settling wrecks keep moving with the present pass). Batched statics have no
// node: the instance is carved out of its graphic's MultiMesh batches and the
// husk grafts into the mission container at the placed transform. No husk
// authored -> the intact graphic keeps standing, dead — the witnessed
// render-pick fallback (batched statics stay in their batches).
void DestructionPresenter::restore_intact(const String &key) {
	if (const HuskRestore *restore = husk_restore_.getptr(key)) {
		if (placer_.is_valid() && restore->bms_id != 0)
			placer_->clear_static_terrain_shadow_replacement(restore->bms_id);
		if (restore->kind == HuskRestore::STATIC && placer_.is_valid())
			placer_->show_static_instance(restore->bms_id);
		for (const HuskRestoreChild &saved : restore->children)
			if (Node3D *child = live_node3d(saved.node))
				child->set_visible(saved.visible);
	}
	husk_restore_.erase(key);
	if (const ObjectID *id = husked_.getptr(key)) {
		if (Node3D *husk = live_node3d(*id)) {
			husk->set_visible(false);
			husk->queue_free();
		}
	}
	husked_.erase(key);
}

void DestructionPresenter::apply_husk_swap(const opennova::world::HuskSwapEvent &p_husk) {
	const int bms_id = p_husk.bms_id;
	const int64_t spawn_origin = static_cast<int64_t>(p_husk.spawn_origin);
	const int wire_handle = static_cast<int>(p_husk.wire_handle);
	const String husk_key = identity_key(bms_id, spawn_origin, wire_handle);
	if (p_husk.restore_intact) {
		restore_intact(husk_key);
		return;
	}
	if (husked_.has(husk_key)) {
		return;
	}
	++stat_husk_swaps_;
	const int item_id = p_husk.item_id;
	const int def_id = item_id + MissionData::ITEM_ID_OFFSET; // wire type id -> items.def id
	String husk_graphic;
	if (item_db_.is_valid()) {
		// husk first, huskfinal fallback (runtime/world/present_passes.h).
		husk_graphic = String(opennova::world::husk_render_graphic(
				item_db_->get_husk(def_id).utf8().get_data(),
				item_db_->get_huskfinal(def_id).utf8().get_data()).c_str());
	}
	if (husk_graphic.is_empty() || placer_.is_null()) {
		husked_[husk_key] = ObjectID();
		++stat_no_husk_;
		return;
	}
	Node3D *node = resolve_entity_node(bms_id, spawn_origin, wire_handle);
	if (node != nullptr) {
		// A qualifying intact model transfers its static-caster role to the husk.
		const bool individual_casts_static_shadow = node_has_static_shadow_caster(node);
		ObjectModel *intact_model = Object::cast_to<ObjectModel>(node);
		const bool individual_mirror_reflected =
				intact_model != nullptr && intact_model->get_mirror_reflected();
		ObjectModel *model = placer_->build_model_from_graphic(
				husk_graphic, String(), node, String(), String(), true);
		if (model == nullptr) {
			husked_[husk_key] = ObjectID();
			++stat_no_husk_;
			return;
		}
		model->set_name("HuskModel");
		set_husk_static_shadow(model, individual_casts_static_shadow);
		// The reflect flag belongs to the entity, not its current graphic. The
		// individual branch must preserve it just like the batched carve branch
		// below; build_model_from_graphic has already built the replacement, so
		// apply the layer choice through one rebuild.
		if (individual_mirror_reflected) {
			model->set_mirror_reflected(true);
			model->rebuild();
		}
		HuskRestore restore;
		restore.kind = HuskRestore::INDIVIDUAL;
		restore.bms_id = bms_id;
		restore.spawn_origin = spawn_origin;
		restore.wire_handle = wire_handle;
		for (int i = 0; i < node->get_child_count(); ++i) {
			Node3D *child = Object::cast_to<Node3D>(node->get_child(i));
			if (child != nullptr && child != model) {
				HuskRestoreChild saved;
				saved.node = child->get_instance_id();
				saved.visible = child->is_visible();
				restore.children.push_back(saved);
				child->set_visible(false);
			}
		}
		husk_restore_[husk_key] = restore;
		husked_[husk_key] = model->get_instance_id();
		if (bms_id != 0) {
			placer_->set_static_terrain_shadow_replacement(bms_id,
					husk_graphic, node->get_transform(),
					individual_casts_static_shadow);
		}
		return;
	}
	if (uses_dynamic_husk_identity(bms_id, spawn_origin, wire_handle)) {
		// The dynamic row may already have retired or failed model resolution.
		// There is no safe static fallback: bms_id zero is a valid authored key.
		husked_[husk_key] = ObjectID();
		++stat_no_husk_;
		return;
	}
	// Batched static (world-wac-ai-re §24.6): carve the instance, graft at its
	// placed transform. An unknown bms_id (individual entity whose node is gone)
	// grafts nothing.
	Node3D *container_node = container();
	if (container_node == nullptr) {
		husked_[husk_key] = ObjectID();
		return;
	}
	// Batched replacements inherit the carved instance's authored eligibility.
	const bool batched_casts_static_shadow =
			placer_->static_instance_casts_terrain_shadow(bms_id);
	const bool batched_mirror_reflected =
			placer_->static_instance_is_mirror_reflected(bms_id);
	ObjectModel *graft = placer_->build_model_from_graphic(
			husk_graphic, String(), container_node, String(), String(), true);
	if (graft == nullptr) {
		husked_[husk_key] = ObjectID();
		++stat_no_husk_;
		return;
	}
	// The placer owns its static lookup by raw BMS id; canonical ownership above
	// must not change the key used to carve and later restore this batch slot.
	const Variant xform_v = placer_->hide_static_instance(bms_id);
	if (xform_v.get_type() != Variant::TRANSFORM3D) {
		graft->set_visible(false);
		graft->queue_free();
		husked_[husk_key] = ObjectID();
		return;
	}
	const Transform3D placed_transform = xform_v;
	HuskRestore restore;
	restore.kind = HuskRestore::STATIC;
	restore.bms_id = bms_id;
	restore.spawn_origin = spawn_origin;
	restore.placed_transform = placed_transform;
	restore.husk_graphic = husk_graphic;
	restore.casts_static_shadow = batched_casts_static_shadow;
	husk_restore_[husk_key] = restore;
	graft->set_name(vformat("HuskModel_%d", bms_id));
	graft->set_transform(placed_transform);
	set_husk_static_shadow(graft, batched_casts_static_shadow);
	// Retail's husk swap keeps the entity's reflect flag: the mirror
	// collectors keep filtering on entity+36 & 0x400, which destruction never
	// clears [orig: Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b writer;
	// husk swap flips only Flags & 4].
	if (batched_mirror_reflected) {
		graft->set_mirror_reflected(true);
		graft->rebuild();
	}
	placer_->set_static_terrain_shadow_replacement(bms_id, husk_graphic,
			graft->get_transform(), batched_casts_static_shadow);
	husked_[husk_key] = graft->get_instance_id();
}

bool DestructionPresenter::node_has_static_shadow_caster(Node *p_root) {
	if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_root)) {
		if ((visual->get_layer_mask() & visual_layers::STATIC_SHADOW_CASTER) != 0) {
			return true;
		}
	}
	for (int i = 0; i < p_root->get_child_count(); ++i) {
		if (node_has_static_shadow_caster(p_root->get_child(i))) {
			return true;
		}
	}
	return false;
}

void DestructionPresenter::set_husk_static_shadow(ObjectModel *p_model, bool p_enabled) {
	if (p_enabled && p_model != nullptr) {
		p_model->set_static_shadow_caster_enabled(true);
	}
}

// Node-less wrecks still move while death physics settles them. Resolve the
// same compact present pose consumed by the other shell presentation paths.
Variant DestructionPresenter::present_transform_for_identity(int p_bms_id,
		int64_t p_spawn_origin) const {
	Simulation *s = sim();
	if (s == nullptr) {
		return Variant();
	}
	PackedVector3Array state;
	if (p_bms_id > 0) {
		state = s->get_present_effect_state_for_bms_id(p_bms_id);
	}
	if (state.size() != Simulation::EFFECT_STATE_COUNT &&
			p_spawn_origin != static_cast<int64_t>(Simulation::SPAWN_ORIGIN_NONE)) {
		state = s->get_present_effect_state_for_origin(
				Simulation::spawn_origin_kind(p_spawn_origin),
				Simulation::spawn_origin_index(p_spawn_origin));
	}
	if (state.size() != Simulation::EFFECT_STATE_COUNT) {
		return Variant();
	}
	const Vector3 rotation_deg = state[Simulation::EFFECT_STATE_ROTATION_DEG];
	Basis basis = bms_to_godot_basis(rotation_deg);
	// Compact peer poses carry yaw only. Static death motion changes position but
	// not orientation, so retain the exact authored basis carved from the batch
	// when pitch/roll are unavailable. Host/listen poses carry the full Euler
	// angles and take the live-basis path above (the rule is the engine's:
	// runtime/world/present_passes.h husk_settle_keeps_carved_tilt).
	if (opennova::world::husk_settle_keeps_carved_tilt(rotation_deg.x, rotation_deg.z)) {
		const String husk_key = identity_key(p_bms_id, p_spawn_origin, -1);
		if (const HuskRestore *restore = husk_restore_.getptr(husk_key)) {
			if (restore->kind == HuskRestore::STATIC) {
				basis = restore->placed_transform.basis;
			}
		}
	}
	return Transform3D(basis, state[Simulation::EFFECT_STATE_POSITION]);
}

void DestructionPresenter::sync_static_husks() {
	for (const KeyValue<String, HuskRestore> &kv : husk_restore_) {
		const HuskRestore &restore = kv.value;
		if (restore.kind != HuskRestore::STATIC) {
			continue;
		}
		const ObjectID *graft_id = husked_.getptr(kv.key);
		Node3D *graft = graft_id != nullptr ? live_node3d(*graft_id) : nullptr;
		if (graft == nullptr) {
			continue;
		}
		const Variant live_v = present_transform_for_identity(restore.bms_id, restore.spawn_origin);
		if (live_v.get_type() == Variant::TRANSFORM3D) {
			const Transform3D live = live_v;
			// The husk registration set the replacement once; this per-frame
			// sync only re-pushes on an actual transform change.
			if (graft->get_transform().is_equal_approx(live)) {
				continue;
			}
			graft->set_transform(live);
			if (placer_.is_valid()) {
				placer_->set_static_terrain_shadow_replacement(
						restore.bms_id, restore.husk_graphic, live,
						restore.casts_static_shadow);
			}
		}
	}
}

void DestructionPresenter::apply_effect(const opennova::world::DestructionEffectEvent &p_effect) {
	EffectWorld *fx_world = fx();
	if (fx_world == nullptr) {
		return;
	}
	const String effect = String::utf8(p_effect.effect.c_str());
	if (effect.is_empty() && !p_effect.release) {
		return;
	}
	const Vector3 pos = mission_to_godot(p_effect.pos);
	const int family = static_cast<int>(p_effect.family);
	const int net_id = static_cast<int>(p_effect.attach_net_id);
	const int bms_id = p_effect.attach_bms_id;
	const int64_t spawn_origin = static_cast<int64_t>(p_effect.attach_spawn_origin);
	const int wire_handle = static_cast<int>(p_effect.attach_wire_handle);
	const bool dynamic_identity = uses_dynamic_husk_identity(
			bms_id, spawn_origin, wire_handle);
	if (family == 0 || (net_id == 0 && !dynamic_identity)) {
		fx_world->spawn_effect(effect, pos, mission_to_godot(p_effect.dir));
		++stat_effects_;
		return;
	}
	// Each retained emitter follows its own model point under the live owner.
	String key = dynamic_identity ? wreck_wire_owner_key(wire_handle, family)
								  : wreck_owner_key(net_id, family);
	if (p_effect.bank_slot != 0)
		key += ":" + String::num_int64(p_effect.bank_slot);
	const auto &local = p_effect.attach_local_pos;
	const Vector3 local_point(-local.y, local.z, local.x);
	if (p_effect.release) {
		if (const int64_t *group = attached_groups_.getptr(key))
			fx_world->stop_group(*group);
		attached_groups_.erase(key);
		fx_world->release_effect_binding(key);
		unregister_effect_anchor(key);
		wreck_anchor_keys_.erase(key);
		burning_.erase(key);
		return;
	}
	attached_groups_[key] =
			fx_world->spawn_effect_owned(key, effect, pos, mission_to_godot(p_effect.dir));
	++stat_effects_;
	if (anchors_.is_valid()) {
		Node3D *node = nullptr;
		if (dynamic_identity) {
			node = resolve_entity_node(bms_id, spawn_origin, wire_handle);
		} else if (index_.is_valid() && bms_id != 0) {
			node = index_->resolve_single(bms_id);
		}
		if (node != nullptr) {
			anchors_->register_effect_anchor(key,
					callable_mp(this, &DestructionPresenter::resolve_wreck_node_anchor)
							.bind(static_cast<int64_t>(node->get_instance_id()), local_point));
			if (family == 2) {
				WreckFire fire;
				fire.node = node->get_instance_id();
				fire.pinned = false;
				burning_[key] = fire;
			}
		} else {
			// Batched-static wreck: no node. Resolve the authoritative present
			// pose while it settles, with the event pose as an identity fallback.
			const Transform3D fixed(Basis(), pos);
			anchors_->register_effect_anchor(key,
					callable_mp(this, &DestructionPresenter::resolve_wreck_pinned_anchor)
							.bind(dynamic_identity, bms_id, spawn_origin, fixed, local_point));
			if (family == 2) {
				// A dynamic identity with no runtime node has no sibling-safe
				// positional lookup; either way the wreck stays pinned at its
				// event pose instead of querying the shared sentinel.
				WreckFire fire;
				fire.pinned = true;
				burning_[key] = fire;
			}
		}
		wreck_anchor_keys_.insert(key);
	}
}

Variant DestructionPresenter::resolve_wreck_node_anchor(int64_t p_node_id, const Vector3 &p_local) {
	Node3D *node = live_node3d(ObjectID(static_cast<uint64_t>(p_node_id)));
	if (node == nullptr)
		return Variant();
	Transform3D pose = node->get_global_transform();
	pose.origin = pose.xform(p_local);
	return pose;
}

Variant DestructionPresenter::resolve_wreck_pinned_anchor(bool p_dynamic_identity, int p_bms_id,
		int64_t p_spawn_origin, const Transform3D &p_fixed, const Vector3 &p_local) {
	const Variant live_v = p_dynamic_identity
			? Variant()
			: present_transform_for_identity(p_bms_id, p_spawn_origin);
	if (live_v.get_type() != Variant::TRANSFORM3D)
		return p_fixed;
	Transform3D pose = live_v;
	pose.origin = pose.xform(p_local);
	return pose;
}

Variant DestructionPresenter::resolve_piece_anchor(int p_slot) {
	if (!piece_generation_.has(p_slot)) {
		return Variant();
	}
	const Vector3 *pos = piece_pos_.getptr(p_slot);
	return pos != nullptr ? Variant(*pos) : Variant();
}

void DestructionPresenter::apply_sound(const String &p_name, const Vector3 &p_pos) {
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	if (p_name.is_empty()) {
		return;
	}
	audio_node->fire_soundset(p_name, p_pos, 0);
	++stat_sounds_;
}

// Death pieces: the sim owns positions/physics; each live piece carries its
// type's trail effect as an owned follow group. The single-section husk mesh
// chunk is the tracked residual (§24).
void DestructionPresenter::present_pieces(const std::vector<opennova::world::DeathPieceRow> &p_pieces) {
	EffectWorld *fx_world = fx();
	stat_pieces_peak_ = MAX(stat_pieces_peak_, static_cast<int64_t>(p_pieces.size()));
	HashSet<int> seen;
	for (const opennova::world::DeathPieceRow &piece : p_pieces) {
		const int slot = piece.slot;
		if (slot < 0) {
			continue;
		}
		seen.insert(slot);
		const Vector3 pos = mission_to_godot(piece.pos);
		piece_pos_[slot] = pos;
		const int64_t generation = static_cast<int64_t>(piece.generation);
		const int64_t *presented = piece_generation_.getptr(slot);
		const bool is_new_generation = (presented != nullptr ? *presented : -1) != generation;
		if (is_new_generation) {
			if (presented != nullptr) {
				unregister_piece_anchor(slot);
			}
			piece_generation_[slot] = generation;
		}
		if (piece.settled) {
			continue;
		}
		if (is_new_generation) {
			// The type's trail effect from the ONE native table
			// (world/destruction death_piece_trail_effect, S12b)
			// [orig: g_death_piece_types @ 0x8404f0 +0x2C].
			const String trail(opennova::world::death_piece_trail_effect(piece.type_index));
			if (fx_world != nullptr && !trail.is_empty()) {
				const String key = piece_owner_key(slot);
				fx_world->spawn_effect_owned(key, trail, pos, Vector3(0, 1, 0));
				if (anchors_.is_valid()) {
					anchors_->register_effect_anchor(key,
							callable_mp(this, &DestructionPresenter::resolve_piece_anchor)
									.bind(slot));
				}
			}
		}
	}
	Vector<int> retired;
	for (const KeyValue<int, int64_t> &kv : piece_generation_) {
		if (!seen.has(kv.key)) {
			retired.push_back(kv.key);
		}
	}
	for (int slot : retired) {
		unregister_piece_anchor(slot);
		piece_generation_.erase(slot);
		piece_pos_.erase(slot);
	}
}

void DestructionPresenter::unregister_piece_anchor(int p_slot) {
	unregister_effect_anchor(piece_owner_key(p_slot));
}

void DestructionPresenter::unregister_effect_anchor(const Variant &p_key) {
	if (anchors_.is_valid()) {
		anchors_->unregister_effect_anchor(p_key);
	}
}

// The wreck-fire registry prune: drop entries whose node died. The random
// crackle itself rolls in the SIM on the engine PRNG stream, per logic tick,
// and arrives as an ordinary transient effect + distance-delay-gated sound
// (S12b; world/destruction destruction_tick_dead_items
// [orig: Entity_UpdateDeadWreckEffects @ 0x493140]).
void DestructionPresenter::tick_wreck_fires() {
	if (burning_.is_empty()) {
		return;
	}
	Vector<String> dead;
	for (const KeyValue<String, WreckFire> &kv : burning_) {
		const WreckFire &entry = kv.value;
		if (entry.node.is_valid()) {
			if (live_node3d(entry.node) == nullptr) {
				dead.push_back(kv.key);
			}
			continue;
		}
		if (!entry.pinned) {
			dead.push_back(kv.key);
		}
	}
	for (const String &key : dead) {
		burning_.erase(key);
	}
}

bool DestructionPresenter::has_active_wreck_fire(const String &p_owner_key) const {
	return burning_.has(p_owner_key);
}

Ref<DestructionPresentStats> DestructionPresenter::get_stats() const {
	Ref<DestructionPresentStats> stats;
	stats.instantiate();
	stats->husk_swaps = stat_husk_swaps_;
	stats->no_husk = stat_no_husk_;
	stats->pieces_peak = stat_pieces_peak_;
	stats->debris_triangles = stat_debris_triangles_;
	stats->effects = stat_effects_;
	stats->sounds = stat_sounds_;
	stats->glass_points = stat_glass_points_;
	stats->crackles = stat_crackles_;
	return stats;
}

} // namespace godot
