#include "player/player_weapon_effects.h"

#include "audio/mission_audio.h"
#include "mission/mission_root.h"
#include "object/object_data.h"
#include "particle/effect_scene.h"
#include "particle/effect_spawn_records.h"
#include "particle/effect_world.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "simulation/simulation.h"
#include "simulation/entity_presenter.h"
#include "world/game_world.h"
#include "world/game_world_access.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/string_convert.h"

using namespace godot;

void PlayerWeaponEffects::setup(Node *p_world, LocalPlayerPresenter *p_presenter) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id()) : ObjectID();
}

void PlayerWeaponEffects::teardown() {
	// Release the live action handles BEFORE losing the world reference (their
	// groups would otherwise keep emitting where they stand).
	release_action_effects();
	world_id_ = ObjectID();
	presenter_id_ = ObjectID();
}

Node *PlayerWeaponEffects::world() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
}

LocalPlayerPresenter *PlayerWeaponEffects::presenter() const {
	return Object::cast_to<LocalPlayerPresenter>(ObjectDB::get_instance(presenter_id_));
}

Ref<LocalPlayerVisuals> PlayerWeaponEffects::visuals() const {
	LocalPlayerPresenter *owner = presenter();
	return owner != nullptr ? owner->visuals() : Ref<LocalPlayerVisuals>();
}

// The sim, re-resolved per use: mission reloads free the runtime and its sim,
// so a cached reference would go stale (the presenter follows the same rule).
Ref<Simulation> PlayerWeaponEffects::sim() const {
	return world_access::sim(world());
}

EffectWorld *PlayerWeaponEffects::effect_world() const {
	return world_access::effect_world(world());
}

MissionAudio *PlayerWeaponEffects::mission_audio() const {
	return world_access::mission_audio(world());
}

void PlayerWeaponEffects::reset() {
	weapon_play_serial_ = -1;
	weapon_view_.unref();
}

void PlayerWeaponEffects::on_viewmodel_refresh() {
	++viewmodel_generation_;
	release_action_effects();
}

// A committed weapon switch from the sim: the FP viewmodel follows the newly
// equipped def as the engine's world::weapon_switch_present says (it carries
// the witness): kept, presented from the sim's own mount, or mounted here.
bool PlayerWeaponEffects::apply_weapon_switch(const String &p_weapon_name,
		bool p_preserve_slot_state, bool p_installed) {
	const Ref<LocalPlayerVisuals> world_visuals = visuals();
	LocalPlayerPresenter *owner = presenter();
	if (world() == nullptr || world_visuals.is_null() || owner == nullptr) {
		return false;
	}
	bool switched = false;
	switch (opennova::world::weapon_switch_present(opennova::to_std(world_visuals->local_player_weapon_name()),
			opennova::to_std(p_weapon_name), owner->viewmodel() != nullptr, p_installed)) {
		case opennova::world::WeaponSwitchPresent::kKeep:
			return true;
		case opennova::world::WeaponSwitchPresent::kPresentInstalled:
			switched = world_visuals->present_local_player_weapon(p_weapon_name);
			break;
		case opennova::world::WeaponSwitchPresent::kMount:
			switched = world_visuals->set_local_player_weapon_by_name(p_weapon_name, p_preserve_slot_state);
			break;
	}
	if (switched) {
		owner->refresh_viewmodel();
	}
	return switched;
}

void PlayerWeaponEffects::apply_weapon_clear() {
	const Ref<LocalPlayerVisuals> world_visuals = visuals();
	LocalPlayerPresenter *owner = presenter();
	if (world() == nullptr || world_visuals.is_null() || owner == nullptr) {
		return;
	}
	world_visuals->clear_local_player_weapon();
	owner->refresh_viewmodel();
}

void PlayerWeaponEffects::release_action_effects() {
	if (EffectWorld *fx = effect_world()) {
		for (const KeyValue<String, ActionEffect> &kv : action_effects_) {
			fx->stop_group(kv.value.group_id);
			fx->release_slot_key(kv.key);
		}
	}
	action_effects_.clear();
}

// The per-tick re-anchor of the live handles (engine world/player_present.h
// kActionEffectSpawnPolicy carries the witness).
void PlayerWeaponEffects::follow_action_effects() {
	EffectWorld *fx = effect_world();
	if (fx == nullptr) {
		action_effects_.clear();
		return;
	}
	Vector<String> gone;
	for (const KeyValue<String, ActionEffect> &kv : action_effects_) {
		const Variant pose = resolve_anchor(kv.value.userpoint);
		if (pose.get_type() != Variant::TRANSFORM3D) {
			fx->stop_group(kv.value.group_id);
			gone.push_back(kv.key);
		} else if (!fx->move_group(kv.value.group_id, pose)) {
			gone.push_back(kv.key);
		}
	}
	for (const String &key : gone) {
		action_effects_.erase(key);
		fx->release_slot_key(key);
	}
}

void PlayerWeaponEffects::consume_pending(const Ref<PlayerWeaponView> &p_view) {
	const Ref<LocalPlayerVisuals> world_visuals = visuals();
	if (world() == nullptr || world_visuals.is_null()) {
		return;
	}
	consume(p_view, world_visuals->drain_local_player_weapon_events());
}

void PlayerWeaponEffects::consume(const Ref<PlayerWeaponView> &p_view,
		const TypedArray<PlayerWeaponEvent> &p_events) {
	weapon_view_ = p_view;
	// The live action handles re-pose to this tick's userpoints first.
	follow_action_effects();
	// The batch facts the engine ordering reads, one per event.
	std::vector<Ref<PlayerWeaponEvent>> events;
	std::vector<opennova::world::WeaponBatchEvent> facts;
	events.reserve(static_cast<size_t>(p_events.size()));
	facts.reserve(static_cast<size_t>(p_events.size()));
	for (int64_t i = 0; i < p_events.size(); ++i) {
		Ref<PlayerWeaponEvent> event = p_events[i];
		opennova::world::WeaponBatchEvent fact;
		if (event.is_valid()) {
			fact.starts_clip = !event->get_anim_key().is_empty();
			fact.action_started = event->get_action_started() >= 0;
			fact.action_effect = event->get_action_effect() >= 0;
			fact.action_finished = event->get_action_finished() >= 0;
			fact.clear_weapon = event->get_clear_weapon();
			fact.switch_weapon = !event->get_switch_to_weapon().is_empty();
			fact.switch_denied = event->get_switch_denied();
		}
		events.push_back(event);
		facts.push_back(fact);
	}
	opennova::world::WeaponBatchPlan plan;
	opennova::world::weapon_batch_plan(p_view.is_valid(),
			p_view.is_valid() ? p_view->get_play_serial() : 0, weapon_play_serial_,
			facts.data(), facts.size(), plan);
	for (const opennova::world::WeaponPresentStep &step : plan.steps) {
		const Ref<PlayerWeaponEvent> event = step.event >= 0 ? events[static_cast<size_t>(step.event)]
															 : Ref<PlayerWeaponEvent>();
		switch (step.op) {
			case opennova::world::WeaponPresentOp::kPoseChannel:
				pose_viewmodel_channel(p_view);
				break;
			case opennova::world::WeaponPresentOp::kPlayClip:
				play_viewmodel_clip(event->get_anim_key(), event->get_anim_variant(),
						p_view->get_anim_advance_ticks());
				break;
			case opennova::world::WeaponPresentOp::kActionBegin:
				fire_action_effects(event);
				break;
			case opennova::world::WeaponPresentOp::kDirectEffect:
				fire_direct_action_effect(event);
				break;
			case opennova::world::WeaponPresentOp::kActionEnd:
				fire_action_end_sound(event);
				break;
			case opennova::world::WeaponPresentOp::kClearWeapon:
				apply_weapon_clear();
				break;
			case opennova::world::WeaponPresentOp::kSwitchWeapon:
				apply_weapon_switch(event->get_switch_to_weapon(), event->get_preserve_slot_state(),
						event->get_installed());
				break;
			case opennova::world::WeaponPresentOp::kSwitchDenied:
				play_switch_deny_sound();
				break;
		}
	}
	weapon_play_serial_ = plan.play_serial;
}

// The action-begin SOUND + MUZZLE legs: play the started ACTION's soundset
// 3D-positional at the firing entity and spawn its particle effect at the
// weapon model's user point, one live handle per action slot (the engine's
// kActionEffectSpawnPolicy carries the spawn's witness). The ordered event
// batch preserves each begin leg when several ticks land in one frame. The
// particle GATE (FIRE only; suppressed at the settled scope in first person)
// is the engine's local_fire_effect_admitted (world/player_present.h carries
// the witnesses).
void PlayerWeaponEffects::fire_action_effects(const Ref<PlayerWeaponEvent> &p_event) {
	if (world() == nullptr) {
		return;
	}
	if (!p_event->get_action_soundset().is_empty()) {
		if (MissionAudio *audio = mission_audio()) {
			audio->fire_soundset(p_event->get_action_soundset(), p_event->get_world_position(), -1);
		}
	}
	const String particle = p_event->get_action_particle();
	if (!opennova::world::local_fire_effect_admitted(p_event->get_action_started(),
				Simulation::WEAPON_ACTION_FIRE, !particle.is_empty(), p_event->get_scope_settled(),
				p_event->get_third_person(), p_event->get_vehicle_attack_context())) {
		return;
	}
	EffectWorld *fx = effect_world();
	if (fx == nullptr) {
		return;
	}
	const String userpoint = p_event->get_action_particle_userpoint();
	const Vector3 pos = action_particle_world_position(userpoint);
	const Vector3 forward = action_particle_world_forward(userpoint);
	// The live handle belongs to the runtime ACTION slot, not the whole
	// player presenter. A weapon re-mount creates a new slot generation. (The
	// instance id is this presentation object's -- it lives
	// setup-to-teardown with the presenter.)
	const String slot_key = vformat("%d:%d:%d", static_cast<int64_t>(get_instance_id()),
			viewmodel_generation_, p_event->get_action_started());
	// Retail hands the group the userpoint's direction alone (the spawn
	// descriptor's orientation, which an EMITVECTOR emitter takes as its axis),
	// and the effect scene reads the pose's forward alone, so the anchor is
	// the one forward pose every effect spawn shares (ptl-format-re.md,
	// ActionSlot_SpawnEffect).
	const Transform3D anchor_transform = EffectWorld::forward_pose(pos, forward);
	// The engine's kActionEffectSpawnPolicy: one live handle per action slot, re-posed to the
	// live userpoint each fixed tick for its whole life (follow_action_effects), the world render
	// domain.
	const opennova::world::ActionEffectSpawnPolicy &policy = opennova::world::kActionEffectSpawnPolicy;
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_admission(policy.suppress_while_owned ? EffectScene::ADMISSION_SUPPRESS_WHILE_OWNED
														: EffectScene::ADMISSION_ALWAYS);
	options->set_render_domain(policy.world_render_domain ? EffectScene::RENDER_DOMAIN_WORLD
														   : EffectScene::RENDER_DOMAIN_FIRST_PERSON);
	options->set_slot_key(slot_key);
	options->set_initial_age_ticks(std::max(p_event->get_age_ticks(), 0));
	// The action slot's descriptor carries its entity as the tag (retail
	// ActionSlot_SpawnEffect @ 0x401F20): the group takes the section gate.
	options->set_section_tagged(true);
	const Ref<EffectSpawnReceipt> receipt =
			fx->spawn_effect_request(particle, anchor_transform, options);
	// A spawned group's handle is held; a suppressed one leaves the live handle as it was.
	if (policy.follow_owner && receipt.is_valid() && receipt->get_spawned()) {
		ActionEffect held;
		held.group_id = receipt->get_group_id();
		held.userpoint = userpoint;
		action_effects_.insert(slot_key, held);
	}
}

// The recoil-row DIRECT effect leg is entirely data-defined. Retail submits
// every authored particle (muzzle, casing, smoke, or another user point) with
// param7=0: no scope gate, name/user-point classification, or live-slot
// handle. Each event is therefore an Always transient in its production
// tick's render domain, pre-aged when multiple fixed ticks are presented
// together. (engine: runtime/world/weapon_fsm.h)
void PlayerWeaponEffects::fire_direct_action_effect(const Ref<PlayerWeaponEvent> &p_event) {
	const String particle = p_event->get_effect_particle();
	if (world() == nullptr || particle.is_empty()) {
		return;
	}
	EffectWorld *fx = effect_world();
	if (fx == nullptr) {
		return;
	}
	const String userpoint = p_event->get_effect_particle_userpoint();
	const Vector3 pos = action_particle_world_position(userpoint);
	const Vector3 forward = action_particle_world_forward(userpoint);
	fx->spawn_effect_transient(particle, pos, forward, std::max(p_event->get_age_ticks(), 0),
			opennova::world::kActionEffectSpawnPolicy.world_render_domain
					? EffectScene::RENDER_DOMAIN_WORLD
					: EffectScene::RENDER_DOMAIN_FIRST_PERSON,
			0, 0, true);
}

Variant PlayerWeaponEffects::resolve_anchor(const String &p_userpoint) {
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return Variant(); // torn down; a stale resolver poll must degrade, not error
	}
	const opennova::world::ActionAnchorPlan plan = anchor_plan(p_userpoint);
	if (plan.count > 0 && plan.sources[0] == opennova::world::ActionAnchorSource::kMountedGun) {
		const ActionPoint mounted = mounted_action_particle(p_userpoint);
		if (mounted.valid) {
			return EffectWorld::forward_pose(mounted.pos, mounted.dir);
		}
	}
	if (owner->viewmodel() == nullptr) {
		return Variant();
	}
	const Vector3 pos = action_particle_world_position(p_userpoint);
	const Vector3 forward = action_particle_world_forward(p_userpoint);
	return EffectWorld::forward_pose(pos, forward);
}

// Map a model-space action userpoint through the posed weapon bone. Rigid
// first-person gun parts ride the .adm rig by subobject/bone index, so the
// part's frame is the bone's model->world matrix from the engine's own pose
// (world::posed_part_to_world, through ObjectModel::subobject_model_to_world).
Transform3D PlayerWeaponEffects::action_particle_model_to_world(ObjectModel *p_part,
		const Ref<ModelUserPoint> &p_info) {
	return p_part != nullptr ? p_part->subobject_model_to_world(p_info->get_subobject()) : Transform3D();
}

opennova::world::ActionAnchorPlan PlayerWeaponEffects::anchor_plan(const String &p_userpoint) const {
	LocalPlayerPresenter *owner = presenter();
	const bool view = weapon_view_.is_valid();
	return opennova::world::action_anchor_plan(owner != nullptr && !p_userpoint.is_empty(), view,
			view && weapon_view_->get_borrowed_usegun_slot(), owner != nullptr && owner->is_third_person(),
			view && weapon_view_->value().first_person_action_model);
}

// The borrowed UseGun carrier's named part point (the plan's mounted source).
PlayerWeaponEffects::ActionPoint PlayerWeaponEffects::mounted_action_particle(
		const String &p_userpoint) const {
	ActionPoint out;
	if (weapon_view_.is_null()) {
		return out;
	}
	GameWorld *game_world = Object::cast_to<GameWorld>(world());
	MissionRoot *runtime = game_world != nullptr ? game_world->get_runtime() : nullptr;
	EntityPresenter *entities = runtime != nullptr ? runtime->get_entity_presenter() : nullptr;
	ObjectModel *gun = entities != nullptr
			? entities->resolve_wire_handle(weapon_view_->get_usegun_mount_handle()) : nullptr;
	const Ref<ObjectData> data = gun != nullptr ? gun->get_object_data() : Ref<ObjectData>();
	if (data.is_null()) {
		return out;
	}
	// The first user point of the name, case aside (the model's by-name lookup).
	const int index = opennova::threedi::threedi_3di3_find_user_point(
			&data->native_model(), p_userpoint.utf8().get_data());
	const Ref<ModelUserPoint> point = index >= 0 ? data->get_user_point_info(index) : Ref<ModelUserPoint>();
	if (point.is_null()) {
		return out;
	}
	const Transform3D pose = action_particle_model_to_world(gun, point);
	const Vector3 direction = pose.basis.xform(point->get_rotation());
	out.pos = pose.xform(point->get_position());
	out.dir = direction.length_squared() > 0.000001f
			? direction.normalized() : -pose.basis.get_column(2).normalized();
	out.valid = true;
	return out;
}

// The THIRD-PERSON action-particle anchor (the plan's third-person source):
// the same authored userpoint name resolved against the gfx3 world gun
// instead of the first-person viewmodel. The viewmodel is re-pinned to the
// camera every frame and merely HIDDEN in third person, never detached, so
// resolving against it there would anchor the muzzle flash to the player's
// own eye. The weapon model is drawn rigid at its attach transform, so a
// model-space userpoint just rides that transform.
PlayerWeaponEffects::ActionPoint PlayerWeaponEffects::third_person_action_particle(
		const String &p_userpoint) const {
	ActionPoint out;
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return out;
	}
	ObjectModel *held_weapon = owner->held_weapon();
	if (held_weapon == nullptr || !held_weapon->is_visible()) {
		return out;
	}
	const Ref<ObjectData> data = held_weapon->get_object_data();
	if (data.is_null()) {
		return out;
	}
	const Transform3D xform = held_weapon->get_global_transform();
	// The first user point of the name, case aside (the model's by-name lookup).
	const int index = opennova::threedi::threedi_3di3_find_user_point(
			&data->native_model(), p_userpoint.utf8().get_data());
	const Ref<ModelUserPoint> info = index >= 0 ? data->get_user_point_info(index) : Ref<ModelUserPoint>();
	if (info.is_null()) {
		return out;
	}
	const Vector3 direction = xform.basis.xform(info->get_rotation());
	out.pos = xform.xform(info->get_position());
	out.dir = direction.length_squared() > 0.000001f ? direction.normalized()
													  : -xform.basis.get_column(2).normalized();
	out.valid = true;
	return out;
}

bool PlayerWeaponEffects::first_person_particle_position(const String &p_userpoint, Vector3 &r_pos) const {
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return false;
	}
	bool found = false;
	owner->viewmodel_rig()->for_each_vm_part([&](ObjectModel *p_part) {
		const Ref<ObjectData> data = found ? Ref<ObjectData>() : p_part->get_object_data();
		if (data.is_null()) {
			return;
		}
		const int count = data->get_user_point_count();
		for (int j = 0; j < count && !found; ++j) {
			const Ref<ModelUserPoint> info = data->get_user_point_info(j);
			if (info.is_valid() && info->get_name().nocasecmp_to(p_userpoint) == 0) {
				r_pos = action_particle_model_to_world(p_part, info).xform(info->get_position());
				found = true;
			}
		}
	});
	return found;
}

bool PlayerWeaponEffects::first_person_particle_forward(const String &p_userpoint, Vector3 &r_dir) const {
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return false;
	}
	bool found = false;
	owner->viewmodel_rig()->for_each_vm_part([&](ObjectModel *p_part) {
		const Ref<ObjectData> data = found ? Ref<ObjectData>() : p_part->get_object_data();
		if (data.is_null()) {
			return;
		}
		const int count = data->get_user_point_count();
		for (int j = 0; j < count && !found; ++j) {
			const Ref<ModelUserPoint> info = data->get_user_point_info(j);
			if (info.is_null() || info->get_name().nocasecmp_to(p_userpoint) != 0) {
				continue;
			}
			const Vector3 world_direction =
					action_particle_model_to_world(p_part, info).basis.xform(info->get_rotation());
			if (world_direction.length_squared() > 0.000001f) {
				r_dir = world_direction.normalized();
				found = true;
			}
		}
	});
	return found;
}

// World-space spawn point for an ACTION particle: the plan's sources in order,
// the entity origin last (world::action_anchor_plan carries the witness).
Vector3 PlayerWeaponEffects::action_particle_world_position(const String &p_userpoint) const {
	const opennova::world::ActionAnchorPlan plan = anchor_plan(p_userpoint);
	for (int i = 0; i < plan.count; ++i) {
		switch (plan.sources[i]) {
			case opennova::world::ActionAnchorSource::kMountedGun: {
				const ActionPoint mounted = mounted_action_particle(p_userpoint);
				if (mounted.valid) {
					return mounted.pos;
				}
				break;
			}
			case opennova::world::ActionAnchorSource::kThirdPersonGun: {
				const ActionPoint tp = third_person_action_particle(p_userpoint);
				if (tp.valid) {
					return tp.pos;
				}
				break;
			}
			case opennova::world::ActionAnchorSource::kFirstPersonParts: {
				Vector3 pos;
				if (first_person_particle_position(p_userpoint, pos)) {
					return pos;
				}
				break;
			}
			case opennova::world::ActionAnchorSource::kEntityOrigin:
				break;
		}
	}
	const Ref<Simulation> origin_sim = sim();
	return origin_sim.is_valid() ? origin_sim->get_local_player_position() : Vector3();
}

// The spawn direction: the plan's gun sources in order, then the view forward.
Vector3 PlayerWeaponEffects::action_particle_world_forward(const String &p_userpoint) const {
	const opennova::world::ActionAnchorPlan plan = anchor_plan(p_userpoint);
	for (int i = 0; i < plan.count; ++i) {
		switch (plan.sources[i]) {
			case opennova::world::ActionAnchorSource::kMountedGun: {
				const ActionPoint mounted = mounted_action_particle(p_userpoint);
				if (mounted.valid) {
					return mounted.dir;
				}
				break;
			}
			case opennova::world::ActionAnchorSource::kThirdPersonGun: {
				const ActionPoint tp = third_person_action_particle(p_userpoint);
				if (tp.valid) {
					return tp.dir;
				}
				break;
			}
			case opennova::world::ActionAnchorSource::kFirstPersonParts: {
				Vector3 dir;
				if (first_person_particle_forward(p_userpoint, dir)) {
					return dir;
				}
				break;
			}
			case opennova::world::ActionAnchorSource::kEntityOrigin:
				break;
		}
	}
	LocalPlayerPresenter *owner = presenter();
	if (owner != nullptr) {
		if (Camera3D *camera = owner->camera()) {
			return -camera->get_global_transform().basis.get_column(2).normalized();
		}
	}
	return Vector3(0.0f, 0.0f, 1.0f);
}

// The action-END sound leg: the finished ACTION's soundsetend, 3D-positional
// at the firing entity -- the fire rows' per-shot gunshot (GS_*) and the
// reload completion; the sim's end leg gates it (world/weapon_fsm carries the
// witness).
void PlayerWeaponEffects::fire_action_end_sound(const Ref<PlayerWeaponEvent> &p_event) {
	const String soundset = p_event->get_action_end_soundset();
	if (world() == nullptr || soundset.is_empty()) {
		return;
	}
	if (MissionAudio *audio = mission_audio()) {
		audio->fire_soundset(soundset, p_event->get_world_position(), -1);
	}
}

// The switch/equip DENY click (D-WPN-22): the engine's
// kWeaponSwitchDenySoundset (world/player_present.h carries the witnesses and
// the SP-client gate reading) as a non-positional interface one-shot.
void PlayerWeaponEffects::play_switch_deny_sound() {
	if (world() == nullptr) {
		return;
	}
	if (MissionAudio *audio = mission_audio()) {
		audio->ui_soundset(opennova::world::kWeaponSwitchDenySoundset);
	}
}

// Pose an FSM clip on every viewmodel part (arms + gun share the animadm) at
// the sim's gated channel position -- a replay resets the position to 0
// through the sim (fire/recoil re-triggers). `variant` is the sim ring's
// latched serve for multi-clip .adm rows -- both parts follow the ONE latch,
// so arms and gun never split variants (engine: runtime/world/player_weapon.h).
// The playhead is PINNED (world::weapon_batch_plan carries the witness).
void PlayerWeaponEffects::play_viewmodel_clip(const String &p_key, int p_variant, int p_advance_ticks) {
	if (p_key.is_empty()) {
		return;
	}
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return;
	}
	const TypedArray<ObjectModel> parts = owner->vm_parts();
	for (int64_t i = 0; i < parts.size(); ++i) {
		if (ObjectModel *visual = Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i]))) {
			visual->play_body_clip_variant_at_tick(p_key, p_variant, p_advance_ticks);
		}
	}
}

// The FP channel as the sim holds it: the primary clip alone, or, while a
// loop wrap fades the slot's next ring entry in, the primary slerped toward it
// by the weight (world/player_weapon.h carries the witnesses).
void PlayerWeaponEffects::pose_viewmodel_channel(const Ref<PlayerWeaponView> &p_view) {
	if (!p_view->get_anim_blending()) {
		play_viewmodel_clip(p_view->get_anim_key(), p_view->get_anim_variant(),
				p_view->get_anim_advance_ticks());
		return;
	}
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return;
	}
	const TypedArray<ObjectModel> parts = owner->vm_parts();
	for (int64_t i = 0; i < parts.size(); ++i) {
		if (ObjectModel *visual = Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i]))) {
			visual->play_body_blend_at(p_view->get_anim_key(), p_view->get_anim_advance_ticks(),
					p_view->get_anim_blend_key(), p_view->get_anim_blend_ticks(),
					p_view->get_anim_blend_weight(), p_view->get_anim_variant(),
					p_view->get_anim_blend_variant());
		}
	}
}

void PlayerWeaponEffects::_bind_methods() {
	// Nothing is script-visible.
}
