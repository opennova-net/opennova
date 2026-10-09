#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "object/model_user_point.h"
#include "object/object_model.h"
#include "simulation/player_weapon_event.h"
#include "simulation/player_weapon_view.h"

namespace godot {

class EffectWorld;
class ItemEffectDirector;
class LocalPlayerPresenter;
class LocalPlayerVisuals;
class MissionAudio;
class Simulation;

// The local player's weapon-EVENT presentation (the former
// player_weapon_effects.gd, ADR 0043 slice G8), owned by LocalPlayerPresenter
// for its setup -> teardown span: the fixed-tick event batch consume
// (switch/clear/fire/recoil/end-sound), the FSM event clips on the viewmodel
// parts, the muzzle/shell userpoint resolution (FP + 3P), and the
// owner-bound effect anchors. The presenter keeps the camera/avatar/viewmodel
// NODES and the fixed-tick entry (present_fixed_weapon_tick -- the
// cross-concern interlock that stamps the camera at production-tick pose);
// this class presents each batch against the presenter's live nodes through
// its accessors. RefCounted only so the anchor resolvers have an Object
// target (callable_mp); registered internally, never script-visible.
class PlayerWeaponEffects : public RefCounted {
	GDCLASS(PlayerWeaponEffects, RefCounted)

public:
	// The world serves the presentation seams: the event drain, mission
	// audio, the effect world, and the effect-anchor registry; the presenter
	// serves the presentation state (viewmodel parts, held weapon, camera,
	// 1P/3P mode).
	void setup(Node *p_world, LocalPlayerPresenter *p_presenter);
	void teardown();

	// This tick's FSM view snapshot -- the presenter's avatar body channel and
	// the emplaced viewmodel controls read it here.
	Ref<PlayerWeaponView> weapon_view() const { return weapon_view_; }
	// The presenter's pre-adoption stamp: adopt the tick's snapshot before the
	// camera pass so the avatar's body channel is current while visual roots
	// are placed.
	void set_weapon_view(const Ref<PlayerWeaponView> &p_view) { weapon_view_ = p_view; }
	// Drop the presentation latches (the presenter's no-player / reset-state path).
	void reset();
	// Re-sync the clip serial: fresh viewmodel parts replay the active clip.
	void reset_play_serial() { weapon_play_serial_ = -1; }
	// A viewmodel re-mount (the presenter's refresh_viewmodel): the action
	// slots get a new owner generation and the old generation's live anchors
	// drop.
	void on_viewmodel_refresh();

	// Drain any ordered presentation events left for owners that do not
	// install the fixed-tick callback. In the game, present_fixed_weapon_tick
	// consumes each 62.5 Hz batch before that tick's particle update. The
	// batch ORDER is the engine's weapon_batch_plan (world/player_present.h
	// carries the witnesses); this class executes its steps over the live
	// nodes.
	void consume_pending(const Ref<PlayerWeaponView> &p_view);
	void consume(const Ref<PlayerWeaponView> &p_view, const TypedArray<PlayerWeaponEvent> &p_events);

	// Mount a weapon.def entry (the def, its FSM and the viewmodel rebuilt for
	// it), or drop the equipped one: the path a committed switch or clear takes,
	// which the dev tools' viewmodel controls take too. The switch answers
	// whether the named weapon is mounted afterwards.
	bool apply_weapon_switch(const String &p_weapon_name, bool p_preserve_slot_state);
	void apply_weapon_clear();

	// The live anchor for an owner-bound weapon-effect group: the spawning
	// action's userpoint through the CURRENT viewmodel pose, or null once the
	// viewmodel is gone (the effect world then unpins the group at its last
	// pose) [orig: the actionEffectHandle tracker @ 0x540edf re-reads
	// actionTable[slot+0x28]'s bone until the emitter dies]. The bound target
	// of the registered resolvers (callable_mp, bound with the userpoint).
	Variant resolve_anchor(const String &p_userpoint);

protected:
	static void _bind_methods();

private:
	// A resolved action userpoint: position + direction in world space (the
	// ex {pos, dir} Dictionary), or invalid when unresolved.
	struct ActionPoint {
		Vector3 pos;
		Vector3 dir;
		bool valid = false;
	};

	Node *world() const;
	LocalPlayerPresenter *presenter() const;
	Ref<LocalPlayerVisuals> visuals() const;
	Ref<Simulation> sim() const;
	EffectWorld *effect_world() const;
	MissionAudio *mission_audio() const;
	Ref<ItemEffectDirector> effect_anchors() const;
	void unregister_effect_anchors();
	void fire_action_effects(const Ref<PlayerWeaponEvent> &p_event);
	void fire_direct_action_effect(const Ref<PlayerWeaponEvent> &p_event);
	static Transform3D action_particle_model_to_world(ObjectModel *p_part, const Ref<ModelUserPoint> &p_info);
	ActionPoint mounted_action_particle(const String &p_userpoint) const;
	ActionPoint third_person_action_particle(const String &p_userpoint) const;
	Vector3 action_particle_world_position(const String &p_userpoint) const;
	Vector3 action_particle_world_forward(const String &p_userpoint) const;
	void fire_action_end_sound(const Ref<PlayerWeaponEvent> &p_event);
	void play_switch_deny_sound();
	void play_viewmodel_clip(const String &p_key, int p_variant, int p_advance_ticks);
	void pose_viewmodel_channel(const Ref<PlayerWeaponView> &p_view);

	ObjectID world_id_;
	ObjectID presenter_id_;
	int viewmodel_generation_ = 0; // action-slot owner identity across weapon re-mounts
	// Live owner-bound effect anchors registered on the world (slot keys);
	// dropped whenever the viewmodel generation turns over.
	HashSet<String> registered_effect_anchor_keys_;
	int weapon_play_serial_ = -1;
	Ref<PlayerWeaponView> weapon_view_; // this tick's FSM view (body channel rides it)
};

} // namespace godot
