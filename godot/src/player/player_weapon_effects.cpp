#include "player/player_weapon_effects.h"

#include "audio/mission_audio.h"
#include "object/object_data.h"
#include "particle/effect_scene.h"
#include "particle/effect_spawn_records.h"
#include "particle/effect_world.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "simulation/simulation.h"
#include "world/item_effect_director.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include <runtime/world/player_present.h>

using namespace godot;

namespace {

const Vector3 kNoPoint(INFINITY, INFINITY, INFINITY);

} // namespace

void PlayerWeaponEffects::setup(Node *p_world, LocalPlayerPresenter *p_presenter) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id()) : ObjectID();
}

void PlayerWeaponEffects::teardown() {
	// Drop our anchors from the world-side ItemEffectDirector BEFORE losing
	// the world reference: the director's registry is world-lifetime and its
	// reset() deliberately leaves owner keys to their owners -- a discarded
	// effects object would otherwise leave its bound-resolver entries pinned
	// there forever (the Callable keeps this object alive, so the
	// invalid-callable reap never fires).
	unregister_effect_anchors();
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
	Node *node = world();
	if (node == nullptr) {
		return Ref<Simulation>();
	}
	return Ref<Simulation>(node->call("get_sim"));
}

EffectWorld *PlayerWeaponEffects::effect_world() const {
	Node *node = world();
	if (node == nullptr) {
		return nullptr;
	}
	return Object::cast_to<EffectWorld>(static_cast<Object *>(node->call("get_effect_world")));
}

MissionAudio *PlayerWeaponEffects::mission_audio() const {
	Node *node = world();
	if (node == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionAudio>(static_cast<Object *>(node->call("get_mission_audio")));
}

Ref<ItemEffectDirector> PlayerWeaponEffects::effect_anchors() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<ItemEffectDirector>();
	}
	return Ref<ItemEffectDirector>(node->call("get_item_effect_director"));
}

void PlayerWeaponEffects::reset() {
	weapon_play_serial_ = -1;
	weapon_view_.unref();
}

void PlayerWeaponEffects::on_viewmodel_refresh() {
	++viewmodel_generation_;
	unregister_effect_anchors();
}

// A committed weapon switch from the sim: reinstall the FP viewmodel/FSM for
// the newly equipped def [orig: the mount's model re-resolve -- the FP render
// model follows the equipped slot, count_weapon_effects_and_update_viewmodel
// @ 0x4dc9e0]. Redundant reinstalls (the installed def already IS the target
// and its viewmodel exists) are skipped so the queued SWITCHTO draw-in
// survives.
void PlayerWeaponEffects::apply_weapon_switch(const String &p_weapon_name, bool p_preserve_slot_state) {
	const Ref<LocalPlayerVisuals> world_visuals = visuals();
	LocalPlayerPresenter *owner = presenter();
	if (world() == nullptr || world_visuals.is_null() || owner == nullptr) {
		return;
	}
	if (world_visuals->local_player_weapon_name().nocasecmp_to(p_weapon_name) == 0 &&
			owner->viewmodel() != nullptr) {
		return;
	}
	const bool switched = world_visuals->set_local_player_weapon_by_name(p_weapon_name, p_preserve_slot_state);
	if (switched) {
		owner->refresh_viewmodel();
	}
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

void PlayerWeaponEffects::unregister_effect_anchors() {
	const Ref<ItemEffectDirector> anchors = effect_anchors();
	if (anchors.is_valid()) {
		for (const String &key : registered_effect_anchor_keys_) {
			anchors->unregister_effect_anchor(key);
		}
	}
	registered_effect_anchor_keys_.clear();
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
				play_viewmodel_clip(p_view->get_anim_key(), p_view->get_anim_variant(),
						p_view->get_anim_advance_ticks());
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
				apply_weapon_switch(event->get_switch_to_weapon(), event->get_preserve_slot_state());
				break;
			case opennova::world::WeaponPresentOp::kSwitchDenied:
				play_switch_deny_sound();
				break;
		}
	}
	weapon_play_serial_ = plan.play_serial;
}

// EffectPose carries forward in basis column 2 (rather than Godot's camera
// -Z convention). Build a complete orthonormal frame so every generic
// producer reaches the same portable spawn contract.
Transform3D PlayerWeaponEffects::weapon_effect_transform(const Vector3 &p_position, const Vector3 &p_forward) {
	if (p_forward.length_squared() <= 0.0001f) {
		return Transform3D(Basis(), p_position);
	}
	const Vector3 z_axis = p_forward.normalized();
	Vector3 seed_up(0.0f, 1.0f, 0.0f);
	if (Math::abs(z_axis.dot(seed_up)) > 0.999f) {
		seed_up = Vector3(0.0f, 0.0f, -1.0f);
	}
	const Vector3 x_axis = seed_up.cross(z_axis).normalized();
	const Vector3 y_axis = z_axis.cross(x_axis).normalized();
	return Transform3D(Basis(x_axis, y_axis, z_axis), p_position);
}

// The action-begin SOUND + MUZZLE legs: play the started ACTION's soundset
// 3D-positional at the firing entity and spawn its particle effect at the
// weapon model's user point [orig: ActionSlot_ExecuteActionWithEffect
// @0x541860 plays the row's soundset and calls ActionSlot_SpawnEffect
// @0x401f20 with the row's particle + userpoint; the one-shot 3D placement is
// Sound_Play3DPositional @0x527cb0]. The ordered event batch preserves each
// begin leg when several ticks land in one frame. The particle GATE (FIRE
// only; suppressed at the settled scope in first person) is the engine's
// local_fire_effect_admitted (world/player_present.h carries the witnesses).
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
	const Transform3D anchor_transform = weapon_effect_transform(pos, forward);
	// The owner-bound group re-reads the live userpoint pose through its
	// anchor resolver for its whole life (the engine's
	// kActionEffectSpawnPolicy: one live handle per action slot, follow the
	// owner, the world render domain).
	if (const Ref<ItemEffectDirector> anchors = effect_anchors(); anchors.is_valid()) {
		anchors->register_effect_anchor(slot_key,
				callable_mp(this, &PlayerWeaponEffects::resolve_anchor).bind(userpoint));
		registered_effect_anchor_keys_.insert(slot_key);
	}
	const opennova::world::ActionEffectSpawnPolicy &policy = opennova::world::kActionEffectSpawnPolicy;
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_admission(policy.suppress_while_owned ? EffectScene::ADMISSION_SUPPRESS_WHILE_OWNED
														: EffectScene::ADMISSION_ALWAYS);
	options->set_binding(policy.follow_owner ? EffectScene::BINDING_FOLLOW_OWNER : EffectScene::BINDING_WORLD);
	options->set_render_domain(policy.world_render_domain ? EffectScene::RENDER_DOMAIN_WORLD
														   : EffectScene::RENDER_DOMAIN_FIRST_PERSON);
	options->set_slot_key(slot_key);
	options->set_owner_key(slot_key);
	options->set_owner_transform(anchor_transform);
	options->set_has_owner_transform(true);
	options->set_initial_age_ticks(std::max(p_event->get_age_ticks(), 0));
	fx->spawn_effect_request(particle, anchor_transform, options);
}

// The recoil-row DIRECT effect leg is entirely data-defined. Retail submits
// every authored particle (muzzle, casing, smoke, or another user point) with
// param7=0: no scope gate, name/user-point classification, or live-slot
// handle. Each event is therefore an Always transient in its production
// tick's render domain, pre-aged when multiple fixed ticks are presented
// together. [orig: WeaponAction_Recoil @ 0x542dd0, spawn @ 0x542f64 with
// param7=0]
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
			0, 0);
}

Variant PlayerWeaponEffects::resolve_anchor(const String &p_userpoint) {
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return Variant(); // torn down; a stale resolver poll must degrade, not error
	}
	if (owner->viewmodel() == nullptr) {
		return Variant();
	}
	const Vector3 pos = action_particle_world_position(p_userpoint);
	const Vector3 forward = action_particle_world_forward(p_userpoint);
	return weapon_effect_transform(pos, forward);
}

// Map a model-space action userpoint through the live fake-skinned weapon
// bone. Rigid first-person gun parts ride the .adm skeleton by subobject/bone
// index, so applying only the model root leaves authored muzzle points in the
// rest pose (and, for the AK, behind the gameplay camera). Convert model
// space into the bone's rest frame, then back through its current global
// pose -- the ported equivalent of the original action-bone transform.
// [orig: Entity_ComputeActionTransform @0x401310 -> ActionSlot_SpawnEffect @0x401f20]
Transform3D PlayerWeaponEffects::action_particle_model_to_world(ObjectModel *p_part,
		const Ref<ModelUserPoint> &p_info) {
	if (p_part != nullptr) {
		Skeleton3D *skeleton = p_part->get_skeleton();
		const int subobject = p_info->get_subobject();
		if (skeleton != nullptr && subobject >= 0 && subobject < skeleton->get_bone_count()) {
			return skeleton->get_global_transform() * skeleton->get_bone_global_pose(subobject) *
					skeleton->get_bone_global_rest(subobject).affine_inverse();
		}
	}
	return p_part != nullptr ? p_part->get_global_transform() : Transform3D();
}

// World-space spawn point for an ACTION particle: the named user point on a
// viewmodel part (the gun carries the muzzle points), composed through its
// live subobject/bone pose. Falls back to the first part's origin, then the
// player eye. The THIRD-PERSON action-particle anchor: the same authored
// userpoint name resolved against the gfx3 world gun instead of the
// first-person viewmodel -- the gfx1/gfx3 pick by the FP bit is the engine's
// action_particle_uses_third_person_gun (world/player_present.h carries the
// witness).
//
// This matters because the presenter's vm_parts are the FIRST-PERSON
// viewmodel: it is re-pinned to the camera every frame and merely HIDDEN in
// third person, never detached, so resolving against it while in third person
// anchors the muzzle flash to the player's own eye. The weapon model is drawn
// rigid at its attach transform, so a model-space userpoint just rides that
// transform.
PlayerWeaponEffects::ActionPoint PlayerWeaponEffects::third_person_action_particle(
		const String &p_userpoint) const {
	ActionPoint out;
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr ||
			!opennova::world::action_particle_uses_third_person_gun(owner->is_third_person()) ||
			p_userpoint.is_empty()) {
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
	const int count = data->get_user_point_count();
	for (int i = 0; i < count; ++i) {
		const Ref<ModelUserPoint> info = data->get_user_point_info(i);
		if (info.is_null() || info->get_name().nocasecmp_to(p_userpoint) != 0) {
			continue;
		}
		const Vector3 direction = xform.basis.xform(info->get_rotation());
		out.pos = xform.xform(info->get_position());
		out.dir = direction.length_squared() > 0.000001f ? direction.normalized()
														  : -xform.basis.get_column(2).normalized();
		out.valid = true;
		return out;
	}
	return out;
}

Vector3 PlayerWeaponEffects::action_particle_world_position(const String &p_userpoint) const {
	const ActionPoint tp = third_person_action_particle(p_userpoint);
	if (tp.valid) {
		return tp.pos;
	}
	LocalPlayerPresenter *owner = presenter();
	Vector3 fallback = kNoPoint;
	if (owner != nullptr) {
		const TypedArray<ObjectModel> parts = owner->vm_parts();
		for (int64_t i = 0; i < parts.size(); ++i) {
			ObjectModel *part = Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i]));
			if (part == nullptr) {
				continue;
			}
			if (fallback == kNoPoint) {
				fallback = part->get_global_transform().origin;
			}
			if (p_userpoint.is_empty()) {
				continue;
			}
			const Ref<ObjectData> data = part->get_object_data();
			if (data.is_null()) {
				continue;
			}
			const int count = data->get_user_point_count();
			for (int j = 0; j < count; ++j) {
				const Ref<ModelUserPoint> info = data->get_user_point_info(j);
				if (info.is_valid() && info->get_name().nocasecmp_to(p_userpoint) == 0) {
					const Transform3D model_to_world = action_particle_model_to_world(part, info);
					return model_to_world.xform(info->get_position());
				}
			}
		}
	}
	if (fallback != kNoPoint) {
		return fallback;
	}
	// Retail's deepest fallback is the ENTITY ORIGIN [orig:
	// Entity_ComputeActionTransform @0x401310, fallback site @0x401867..0x401887
	// copies entity+4/+8/+0xC]. The eye was our own invention and put the
	// flash on the player's face whenever a userpoint failed to resolve.
	const Ref<Simulation> origin_sim = sim();
	return origin_sim.is_valid() ? origin_sim->get_local_player_position() : Vector3();
}

Vector3 PlayerWeaponEffects::action_particle_world_forward(const String &p_userpoint) const {
	const ActionPoint tp = third_person_action_particle(p_userpoint);
	if (tp.valid) {
		return tp.dir;
	}
	LocalPlayerPresenter *owner = presenter();
	if (owner != nullptr) {
		const TypedArray<ObjectModel> parts = owner->vm_parts();
		for (int64_t i = 0; i < parts.size(); ++i) {
			ObjectModel *part = Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i]));
			if (part == nullptr) {
				continue;
			}
			const Ref<ObjectData> data = part->get_object_data();
			if (data.is_null()) {
				continue;
			}
			const int count = data->get_user_point_count();
			for (int j = 0; j < count; ++j) {
				const Ref<ModelUserPoint> info = data->get_user_point_info(j);
				if (info.is_null() || info->get_name().nocasecmp_to(p_userpoint) != 0) {
					continue;
				}
				const Transform3D model_to_world = action_particle_model_to_world(part, info);
				const Vector3 world_direction = model_to_world.basis.xform(info->get_rotation());
				if (world_direction.length_squared() > 0.000001f) {
					return world_direction.normalized();
				}
			}
		}
		if (Camera3D *camera = owner->camera()) {
			return -camera->get_global_transform().basis.get_column(2).normalized();
		}
	}
	return Vector3(0.0f, 0.0f, 1.0f);
}

// The action-END sound leg: the finished ACTION's soundsetend, 3D-positional
// at the firing entity -- the fire rows' per-shot gunshot (GS_*) and the
// reload completion. [orig: ActionSlot_FinishActivePhase @0x53f7b0 -> the end
// shim @0x401100 plays ActionDef+12 at the owner entity, gated on the phase
// byte being 2 (ACTIVE); its dupsound repeat loop (+44/+48) is data-dead in
// the JOX/REVX corpora]
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
// so arms and gun never split variants [orig: AnimMap_PlayAnimBySlot
// @0x40bda0 latches the served entry at animState+68]. The playhead is
// PINNED (external phase): retail's channel moves only in the weapon pump's
// gated tick shim, never per render frame [orig: AnimChannel_AdvancePlayback
// @ 0x40b140 rate = one 62 Hz tick of clip time; the gate
// ActionSlot_ExecuteActionNoEffect @ 0x541a4d].
void PlayerWeaponEffects::play_viewmodel_clip(const String &p_key, int p_variant, int p_advance_ticks) {
	if (p_key.is_empty()) {
		return;
	}
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return;
	}
	const double seconds = static_cast<double>(std::max(p_advance_ticks, 0)) * Simulation::tick_dt();
	const TypedArray<ObjectModel> parts = owner->vm_parts();
	for (int64_t i = 0; i < parts.size(); ++i) {
		if (ObjectModel *visual = Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i]))) {
			visual->play_body_clip_variant_at_time(p_key, p_variant, seconds);
		}
	}
}

void PlayerWeaponEffects::_bind_methods() {
	// The anchor resolvers bind through callable_mp; nothing is script-visible.
}
