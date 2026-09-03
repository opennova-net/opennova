#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "mission/static_source_records.h"
#include "object/entity_ref.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "particle/effect_spawn_records.h"
#include "simulation/present_event_records.h"

namespace godot {

class EffectWorld;
class MissionRoot;

// Value-only census of ItemEffectDirector's per-item effect bookkeeping (the
// F3/GUT read seam, ADR 0018): how many placed nodes and static sources hold
// an attached effect group, how many wait on a hidden particle switch or on
// their controller lifecycle, and how many controller nodes are active.
#define ITEM_EFFECT_DIRECTOR_STATS_FIELDS(X) \
	X(registered_nodes)   /* placed nodes whose authored effect groups are attached */ \
	X(registered_static)  /* static sources whose effect groups are attached */ \
	X(pending_nodes)      /* nodes deferred by the hidden particle switch (retry once) */ \
	X(pending_static)     /* static sources deferred the same way */ \
	X(control_nodes)      /* PlayerControl item nodes waiting on controller edges */ \
	X(control_active)     /* identity aliases with a controlling occupant */ \
	X(owner_keys)         /* per-item owner keys with a presented node */

class ItemEffectDirectorStats : public RefCounted {
	GDCLASS(ItemEffectDirectorStats, RefCounted)

public:
#define ITEM_EFFECT_DIRECTOR_STATS_ACCESSORS(m_name)          \
	int get_##m_name() const { return m_name##_; }            \
	void set_##m_name(int p_value) { m_name##_ = p_value; }
	ITEM_EFFECT_DIRECTOR_STATS_FIELDS(ITEM_EFFECT_DIRECTOR_STATS_ACCESSORS)
#undef ITEM_EFFECT_DIRECTOR_STATS_ACCESSORS

protected:
	static void _bind_methods();

private:
#define ITEM_EFFECT_DIRECTOR_STATS_MEMBER(m_name) int m_name##_ = 0;
	ITEM_EFFECT_DIRECTOR_STATS_FIELDS(ITEM_EFFECT_DIRECTOR_STATS_MEMBER)
#undef ITEM_EFFECT_DIRECTOR_STATS_MEMBER
};

// The per-item ITEMS.DEF particle-effect director (the former
// item_effect_director.gd, ADR 0043 d9): the mission-lifetime owner of the
// entity-attached / static / controller-gated item emitters, the
// owner-registered effect-anchor resolvers, and the retail master particle
// switch. Owned by GameWorld as _item_fx, constructed in the world's _init
// and wired once through setup(). GameWorld keeps one-line public delegates
// (set_particles_hidden, register_effect_anchor/unregister_effect_anchor) so
// the owner-facing names never moved.
//
// The law — the pool/attrib gates, the identity aliases and the
// matched-userpoint / origin-fallback attach plan — is the engine's
// runtime/world/item_effects; this class is the device half: the node
// registry, the anchor resolvers, the pending / control bookkeeping, and
// the owner-pose resolver the effect world polls. Shared state is reached
// through the world's PUBLIC surface — get_effect_world() / get_runtime() /
// get_node_or_null — with TWO lent private seams arriving as setup()
// Callables, null-guarded by the world: the placer's static item-effect
// sources, and the placer's ItemDatabase (resolved lazily; a placer exists
// only once a mission is placed).
class ItemEffectDirector : public RefCounted {
	GDCLASS(ItemEffectDirector, RefCounted)

public:
	void setup(Node *p_world, const Callable &p_static_sources, const Callable &p_item_db_source);
	// The retail master particle switch (the dev tools' "Hide particles"),
	// delegated from the world: flips the effect world's spawn facade and,
	// on re-enable, retries the deferred persistent item effects exactly
	// once.
	void set_particles_hidden(bool p_hidden);
	// The persisted switch state: the world re-asserts it onto each freshly
	// built EffectWorld (the preference survives mission reloads).
	bool particles_hidden() const;
	// An owner registers a live pose resolver for an owner-bound effect
	// group it spawned (e.g. the local muzzle flash riding the viewmodel
	// userpoint). The resolver is polled by the effect world's owner-pose
	// sync while any group bound to owner_key is alive; re-registering the
	// same key overwrites.
	void register_effect_anchor(const Variant &p_owner_key, const Callable &p_resolver);
	void unregister_effect_anchor(const Variant &p_owner_key);
	bool has_effect_anchor(const Variant &p_owner_key) const;
	// Value-only census of the per-item effect bookkeeping (ADR 0018 read seam).
	Ref<ItemEffectDirectorStats> get_stats() const;
	// The world just built a fresh EffectWorld for a mission
	// (_start_effect_world): wire the owner-pose provider and attach the
	// persistent per-item effects. The wire-spawn callback is
	// single-subscriber; GameWorld registers one router that fans out to
	// this director AND the effect-light director.
	void on_effect_world_started();
	// Mission unload: forget every owner key / pending record. Per-item
	// attached-effect owner keys reference nodes in the freed MissionObjects
	// container — never let a reload's provider resolve against freed
	// instances. The anchor resolvers are deliberately NOT cleared: their
	// owners (LocalPlayerPresenter, the present passes) unregister their own
	// keys; particles_hidden survives reloads by design.
	void reset();
	// Owner-transform provider for the effect world's owned/attached groups
	// (Transform3D | Vector3 | null). Int keys are WAC fx2ssn SSNs (the
	// runtime resolves the live entity transform; null = entity gone, the
	// group detaches [orig: CEffect_UpdateEmitterTransform @ 0x5f7410]);
	// String keys are the per-item effect attaches registered by reattach
	// (the placed entity's current value snapshot; the Node remains only as
	// a pre-first-tick seed and lifetime fallback for non-sim-owned callers).
	Variant resolve_owner_transform(const Variant &p_owner_key);
	// Mission-start attach of the per-item ITEMS.DEF effects — slot A
	// ('particlefx <effect> <userpoint>') only: for every presented animated
	// entity whose item def authors it, spawn one entity-attached emitter at
	// EVERY model userpoint matching the authored name. Static MultiMesh
	// entities use the placer's value descriptors and spawn the same
	// authored effects world-bound at their final placement transform; no
	// owner/render node is synthesized for them. Public: the sim-restart and
	// warm-pass paths re-register the persistent effects for the restored
	// entity set.
	void reattach();
	void on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id);
	// Consume a vehicle-control lifecycle effect from the runtime's drained
	// batch (called by the world's effect router, on_runtime_effects).
	// Returns true when the effect was a control event and was handled here.
	bool consume_control_effect(const Ref<MissionEffect> &p_effect);

protected:
	static void _bind_methods();

private:
	// One PlayerControl item node waiting on its controller lifecycle edges
	// (a despawned wire node is validity-checked before any typed read).
	struct ControlNode {
		ObjectID node;
		int kind = -1;
		int item_id = 0;
		std::vector<std::string> aliases;
	};
	// The live effect groups and owner keys one controller-activated node
	// spawned.
	struct ControlInstance {
		Vector<int64_t> group_ids;
		Vector<String> owner_keys;
	};
	// One item attachment deferred while particles were hidden (the retail
	// master switch), replayed once by the re-enable retry.
	struct PendingNode {
		ObjectID node;
		int kind = -1;
		int item_id = 0;
		bool controller_active = false;
	};

	Node *_world() const;
	EffectWorld *_effect_world() const;
	MissionRoot *_runtime() const;
	Ref<ItemDatabase> _resolve_item_db() const;
	void _control_node_aliases(ObjectModel *p_node, std::vector<std::string> &r_out) const;
	bool _control_node_is_active(const ControlNode &p_entry) const;
	bool _register_control_node(ObjectModel *p_node, int p_kind, int p_item_id);
	void _track_control_spawn(uint64_t p_node_id, const String &p_owner_key,
			const Ref<EffectSpawnReceipt> &p_receipt);
	void _stop_control_node(uint64_t p_node_id);
	void _activate_control_nodes(const std::vector<std::string> &p_event_aliases);
	void _deactivate_control_nodes(const std::vector<std::string> &p_event_aliases);
	int _attach_item_effect_to_node(ObjectModel *p_node, int p_kind, int p_item_id,
			const Ref<ItemDatabase> &p_item_db_override = Ref<ItemDatabase>(),
			bool p_controller_active = false);
	bool _spawn_static_item_effect(EffectWorld *p_effect_world, const String &p_effect,
			const Transform3D &p_transform);
	int _attach_item_effect_to_static(const Ref<StaticEffectSource> &p_source, int p_source_index,
			const Ref<ItemDatabase> &p_item_db_override = Ref<ItemDatabase>());
	void _retry_pending_item_effects();

	// The GameWorld whose entities carry the effects (public surface only).
	ObjectID world_id_;
	Callable static_sources_;  // () -> Array (the placer's static item-effect sources)
	Callable item_db_source_;  // () -> ItemDatabase or null (the placer's db, lent by the world)
	// Debug: hide every particle effect (the dev tools' "Hide particles" —
	// the retail master particle switch, mimicked). Off by default; survives
	// mission reloads.
	bool particles_hidden_ = false;
	// owner key -> Callable returning the live anchor Transform3D (or null
	// once stale) for registered owner-bound effect groups; consulted before
	// the item-fx/SSN legs by resolve_owner_transform.
	HashMap<Variant, Callable, VariantHasher, VariantComparator> effect_anchor_resolvers_;
	// owner key (String) -> presented Node3D, for the per-item attached
	// effect groups, and the copied entity_ref value identity (bms/origin or
	// wire handle).
	HashMap<String, ObjectID> item_fx_nodes_;
	HashMap<String, Ref<EntityRef>> item_fx_owner_refs_;
	HashMap<uint64_t, ObjectID> item_fx_registered_nodes_;
	HashMap<uint64_t, PendingNode> item_fx_pending_nodes_;
	HashSet<int> item_fx_registered_static_;
	HashMap<int, Ref<StaticEffectSource>> item_fx_pending_static_;
	// Controller/Driver-only PlayerControl item effects are dormant at
	// mission startup. Portable lifecycle events activate them without
	// polling: the identity aliases with a controlling occupant, the
	// registered control nodes and their live instances, by node id.
	std::unordered_set<std::string> item_fx_control_active_;
	HashMap<uint64_t, ControlNode> item_fx_control_nodes_;
	HashMap<uint64_t, ControlInstance> item_fx_control_instances_;
};

} // namespace godot
