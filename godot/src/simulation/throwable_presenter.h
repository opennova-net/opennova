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
#include <vector>

#include <runtime/world/present_drains.h>

#include "mission/mission_object_placer.h"
#include "object/item_database.h"
#include "simulation/present_stats.h"

namespace godot {

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
// A round's move emitter is the engine's (world::RoundEmitters). A RefCounted
// (registered internally, never script-visible).
class ThrowablePresenter : public RefCounted {
	GDCLASS(ThrowablePresenter, RefCounted)

public:
	void setup(Simulation *p_sim, Node3D *p_container, const Ref<MissionObjectPlacer> &p_placer,
			const Ref<ItemDatabase> &p_item_db);
	// Typed diagnostic snapshot (ADR 0017), built per call.
	Ref<ThrowablePresentStats> get_stats() const;
	void present();
	// The pure-data presentation leg (the present_snapshot precedent): production
	// present() feeds the typed sim's rows; tests feed the same rows directly.
	void present_visuals(const std::vector<opennova::world::ThrowableVisualRow> &p_visuals);
	// One scene collect's flying-round draws (the engine's
	// OcclusionWorld::collect_round_draws, by presentation key): a round model
	// the view collected takes its admit, every other round model the view's
	// hide, as the destruction pass's piece models take their collect's; the
	// main view's on the model's occlusion claim, the weapon Inset's on its own
	// Inset verdict. Placed devices are pool-1 entities, not the RoundPool's.
	void apply_round_draws(const std::vector<int64_t> &p_keys, bool p_inset);
	void reset_runtime_state();
	void teardown();

protected:
	static void _bind_methods();

private:
	// One reconciled throwable model; `node` stays invalid for an unresolved
	// graphic so the miss is remembered instead of re-tried every frame.
	struct ModelSlot {
		ObjectID node;
		int item_id = 0;
	};

	Simulation *sim() const;
	Node3D *container() const;
	bool build_model(int p_item_id, bool p_mirror_collected, ModelSlot &r_slot);
	static void free_model(const ModelSlot &p_slot);

	ObjectID sim_id_; // null in data-driven tests
	ObjectID container_id_;
	Ref<MissionObjectPlacer> placer_;
	Ref<ItemDatabase> item_db_;
	// key (int64) -> ModelSlot; flying-round keys combine the pool slot with its
	// monotonic lifetime generation, while placed devices occupy a disjoint
	// high-bit namespace.
	HashMap<int64_t, ModelSlot> models_;
};

} // namespace godot
