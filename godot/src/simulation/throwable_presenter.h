#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>

#include "mission/mission_object_placer.h"
#include "object/item_database.h"
#include "simulation/present_event_records.h"
#include "simulation/present_stats.h"
#include "world/item_effect_director.h"

namespace godot {

class EffectWorld;
class Simulation;

// Viewing-client presentation for throwables (the former
// throwable_present_pass.gd, ADR 0043 d9), an owned member of
// EntityPresenter: item-modeled flying rounds (grenades, satchels, claymores
// in the air) and known placed devices — the render half of
// engine/runtime/world's ThrowableSim/RoundSim state (world-wac-ai-re §27).
// Joiners currently learn flying rounds from S2C tag-2; placed-device 0x59/0x12
// replication remains deferred and this pass only renders state the sim has.
// [orig: the round renders as its TrcrID item model via Entity_InitFromItemDef
// @ 0x49e550 with the motor-integrated angles; a placed device is a pool-1
// item entity drawn like any other. The sim stays render-free — this pass
// reconciles model nodes against get_throwable_visuals() each frame.]
// A RefCounted (registered internally, never script-visible) only because
// the move-effect anchor it registers with the ItemEffectDirector is a
// Callable bound to its method.
class ThrowablePresenter : public RefCounted {
	GDCLASS(ThrowablePresenter, RefCounted)

public:
	// `fx` (nullable) spawns the round-bound move groups; `anchors`
	// (nullable) is the owner-anchor registry (GameWorld's ItemEffectDirector)
	// — null when the owner runs without an effect world.
	void setup(Simulation *p_sim, Node3D *p_container, const Ref<MissionObjectPlacer> &p_placer,
			const Ref<ItemDatabase> &p_item_db, EffectWorld *p_fx,
			const Ref<ItemEffectDirector> &p_anchors);
	// Typed diagnostic snapshot (ADR 0017), built per call.
	Ref<ThrowablePresentStats> get_stats() const;
	void present();
	// The pure-data presentation leg (the present_snapshot precedent): production
	// present() feeds the typed sim's rows; tests feed the same rows directly.
	void present_visuals(const TypedArray<ThrowableVisualRow> &p_visuals);
	// Reconcile only the round-bound effects_table "move" groups at the fixed-tick
	// seam. Scene nodes remain batched in present(), but particles must see every
	// simulated pose and a release before the same tick's EffectWorld advance.
	void sync_fixed_tick_effects();
	void reset_runtime_state();
	void teardown();

	// The move-effect anchor (the Callable target the ItemEffectDirector
	// polls): the live round's full Godot-space transform while its group
	// lives, null once retired.
	Variant resolve_move_effect_anchor(int64_t p_key);

protected:
	static void _bind_methods();

private:
	// One reconciled throwable model; `node` stays invalid for an unresolved
	// graphic so the miss is remembered instead of re-tried every frame.
	struct ModelSlot {
		ObjectID node;
		int item_id = 0;
	};
	// One live round-bound "move" effect group and its effect-world identity.
	struct MoveEffect {
		String effect;
		String owner_key;
		int64_t group_id = 0;
	};

	Simulation *sim() const;
	Node3D *container() const;
	EffectWorld *fx() const;
	void sync_move_effects(const TypedArray<ThrowableVisualRow> &p_visuals);
	void present_move_effect(int64_t p_key, const String &p_effect,
			const Transform3D &p_transform, bool p_live);
	void retire_move_effect(int64_t p_key);
	bool build_model(int p_item_id, ModelSlot &r_slot);
	static void free_model(const ModelSlot &p_slot);

	ObjectID sim_id_; // null in data-driven tests
	ObjectID container_id_;
	Ref<MissionObjectPlacer> placer_;
	Ref<ItemDatabase> item_db_;
	ObjectID fx_id_;
	Ref<ItemEffectDirector> anchors_;
	// key (int64) -> ModelSlot; flying-round keys combine the pool slot with its
	// monotonic lifetime generation, while placed devices occupy a disjoint
	// high-bit namespace.
	HashMap<int64_t, ModelSlot> models_;
	// Live round key -> full Godot-space transform, polled by the effect-world
	// owner resolver.
	HashMap<int64_t, Transform3D> move_effect_transforms_;
	// Live round key -> MoveEffect.
	HashMap<int64_t, MoveEffect> move_effects_;
};

} // namespace godot
