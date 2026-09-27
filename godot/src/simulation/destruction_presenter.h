#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

#include <runtime/world/death_piece_draw.h>
#include <runtime/world/destruction.h>
#include <runtime/world/present_drains.h>

#include "mission/mission_object_placer.h"
#include "object/entity_index.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "simulation/present_stats.h"
#include "world/item_effect_director.h"

namespace godot {

class EffectLightDirector;
class EffectWorld;
class EntityPresenter;
class MissionAudio;
class Simulation;

// THE shell destruction-presentation pass (the former
// destruction_present_pass.gd, ADR 0043 d9), an owned member of
// EntityPresenter: presents the sim's item destruction on the viewing peer —
// the husk model swap on destroyed items, the death-piece debris (trail
// effects riding the sim's piece pool), resolved section-triangle debris and
// glass userpoint effects, the death/fire/other wreck effect families with
// the random fire crackle, and the destruction sounds. Drains
// Simulation.drain_destruction_events() + get_death_pieces() once per
// present, the fire pass precedent. Joiners run it too: their world raises
// the same events from the S2C 0x13-driven death chain and the client-side
// explosion/piece drains (retail's client runs the identical presentation
// from its own pools).
// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 -> deathCallback(entity,4,0);
//  Entity_UpdateAllEntities @0x4c2100 drains unconditionally on every peer]
//
// [orig map — docs/world/world-wac-ai-re.md §24:
//  husk swap: Flags & 4 switches render + collision to the def husk model
//    (Entity_RaycastCollisionModel @ 0x413086 pick; no husk -> the graphic keeps
//    standing, the witnessed fallback);
//  pieces: Entity_SpawnDeathPieces @ 0x493400 -> the 256-slot pool ticked by
//    DeathPiece_TickAll @ 0x57b900 (each piece renders ONE husk section with a
//    per-type trail effect from g_DeathPieceTypes @ 0x8404f0); the draw is
//    the occlusion frame's collect (world/death_piece_draw.h) applied by
//    apply_piece_draws;
//  section debris: Entity_SpawnSectionDebris @ 0x43f580 — collision-face
//    centroid sampling at stride (scale<<8)/150, Effect_TreeWoodExp per tri
//    (material 17 -> Effect_TreeFoliageExp), directions away from the blast;
//  wreck effects: Entity_InitDeathSounds @ 0x4939b0 (the particledeath family
//    at the Dead bones) + Entity_UpdateDeadWreckEffects @ 0x493140 (the fire
//    family's random crackle Effect_BoatExpSec + EXPLO_SHIP_SM, underwater
//    steam-out Effect_Boat01Steam).]
//
// Remaining stand-ins (tracked in the §24 record + D-ITEM ledger rows): effect
// anchors ride the entity origin, not the husk Dead/Fire/Other user points.
// Section debris and glass already arrive as resolved transient effect rows
// from the simulation.
//
// The husk render pick, the presentation identity key and the settle tilt
// retention are the engine's (runtime/world/present_passes.h); the owner-key
// grammar is simulation/effect_owner_keys.h. A RefCounted (registered
// internally, never script-visible) only because the anchor resolvers it
// registers with the ItemEffectDirector are Callables bound to its methods.
class DestructionPresenter : public RefCounted {
	GDCLASS(DestructionPresenter, RefCounted)

public:
	// `owner` resolves runtime-only packed handles (the wire walk's nodes);
	// `anchors` (nullable) is the owner-anchor registry (GameWorld's
	// ItemEffectDirector); `audio` (nullable) gates the sound leg, `fx`
	// (nullable) the effect legs; `lights` (nullable) takes the death-flash
	// light route (EffectLightDirector.on_death_light) [orig: the
	// Entity_SpawnDeathPieces glow @ 0x49351a].
	void setup(EntityPresenter *p_owner, Simulation *p_sim, Node3D *p_container,
			const Ref<EntityIndex> &p_index, const Ref<MissionObjectPlacer> &p_placer,
			const Ref<ItemDatabase> &p_item_db, const Ref<ItemEffectDirector> &p_anchors,
			MissionAudio *p_audio, EffectWorld *p_fx, EffectLightDirector *p_lights);
	void teardown();
	// Discard mission-run presentation state without discarding setup dependencies.
	// This is the Stop -> Play boundary as well as the teardown primitive: restore
	// intact visuals, remove transient husk grafts, and retire every anchor whose
	// resolver points into the prior simulation incarnation.
	void reset_runtime_state();
	// Once per present, after the sim advanced (beside the fire pass).
	void present();
    // Retained rows can arrive before or after the transient death drain.
    void apply_husk_swap(const opennova::world::HuskSwapEvent &p_husk);
    ObjectModel *visual_model(ObjectModel *p_entity_model) const;
	// The pure-data presentation leg (the present_snapshot precedent): production
	// present() drains the typed sim; tests feed the same event/piece rows.
	void present_drained(const opennova::world::DestructionEvents &p_events,
			const std::vector<opennova::world::DeathPieceRow> &p_pieces);
	// The frame's piece draws (the occlusion frame's collect): each drawn
	// piece's model shows its level with every other section collapsed,
	// placed by the section draw's matrix; every other piece model hides.
	// Rows whose slot generation is not the presented one are skipped.
	void apply_piece_draws(const std::vector<opennova::world::DeathPieceDraw> &p_draws);
	// The weapon Inset pass's own piece draws (its collect's; the models'
	// Inset state, drawn by twins where the views differ).
	void apply_piece_draws_inset(const std::vector<opennova::world::DeathPieceDraw> &p_draws);
	// A piece slot's model (null when the slot presents none).
	ObjectModel *piece_model(int p_slot) const;
	// The section draw's matrix in Godot space: the piece pose (the BAM
	// heading, pitch, roll) scaled at the piece position, times the drawn
	// section's centre moved to the origin when the draw pivots.
	static Transform3D piece_draw_transform(const opennova::world::DeathPieceDraw &p_draw);
	// Whether an owned fire-family effect is still registered for retail wreck
	// crackle updates. The owner key is the same public identity used by the
	// effect-anchor registry.
	bool has_active_wreck_fire(const String &p_owner_key) const;
	// Typed diagnostic counters (ADR 0017: cross-object contracts are typed
	// records) — probes assert the presentation legs actually ran.
	Ref<DestructionPresentStats> get_stats() const;

	// The anchor resolvers (Callable targets the ItemEffectDirector polls),
	// each over explicitly bound state: a wreck riding its presented node, a
	// batched-static wreck resolving the authoritative present pose while it
	// settles (the event pose as its identity fallback), and a piece riding
	// the last presented slot position.
	Variant resolve_wreck_node_anchor(int64_t p_node_id, const Vector3 &p_local);
	Variant resolve_wreck_pinned_anchor(bool p_dynamic_identity, int p_bms_id,
			int64_t p_spawn_origin, const Transform3D &p_fixed, const Vector3 &p_local);
	Variant resolve_piece_anchor(int p_slot);

protected:
	static void _bind_methods();

private:
	// One burning wreck's crackle anchor: the presented node when the wreck has
	// one, else `pinned` for a batched-static wreck anchored at its event pose.
	// `node` is an ObjectID so a despawned wreck can be validity-checked before
	// any typed read.
	struct WreckFire {
		ObjectID node;
		bool pinned = false;
	};
	// One intact child's authored visibility, restored on reset.
	struct HuskRestoreChild {
		ObjectID node;
		bool visible = true;
	};
	// The original state behind one husk graft: an individual node's hidden
	// children, or a carved batched static's placed transform + replacement.
	struct HuskRestore {
		enum Kind { INDIVIDUAL, STATIC };
		Kind kind = INDIVIDUAL;
		int bms_id = 0;
		int64_t spawn_origin = 0;
		int wire_handle = -1;
		std::vector<HuskRestoreChild> children;
		Transform3D placed_transform;
		String husk_graphic;
		bool casts_static_shadow = false;
	};

	Simulation *sim() const;
	Node3D *container() const;
	MissionAudio *audio() const;
	EffectWorld *fx() const;
	EffectLightDirector *lights() const;
	Node3D *resolve_entity_node(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle) const;
	void restore_intact(const String &p_key);
	static bool node_has_static_shadow_caster(Node *p_root);
	static void set_husk_static_shadow(ObjectModel *p_model, bool p_enabled);
	Variant present_transform_for_identity(int p_bms_id, int64_t p_spawn_origin) const;
	void sync_static_husks();
	void apply_effect(const opennova::world::DestructionEffectEvent &p_effect);
	void apply_sound(const String &p_name, const Vector3 &p_pos);
	void present_pieces(const std::vector<opennova::world::DeathPieceRow> &p_pieces);
	void unregister_piece_anchor(int p_slot);
	ObjectModel *build_piece_model(int p_slot, int p_item_id);
	void free_piece_model(int p_slot);
	void unregister_effect_anchor(const Variant &p_key);
	void tick_wreck_fires();

	EntityPresenter *owner_ = nullptr; // runtime-only packed handles
	ObjectID sim_id_;                  // live pose source; null in data-driven tests
	ObjectID container_id_;            // mission container (node-less husk grafts land here)
	Ref<EntityIndex> index_;
	Ref<MissionObjectPlacer> placer_;  // husk model builds
	Ref<ItemDatabase> item_db_;        // husk graphic names
	Ref<ItemEffectDirector> anchors_;  // owner-anchor registry (GameWorld's), or null
	ObjectID audio_id_;                // MissionAudio (or null)
	ObjectID fx_id_;                   // EffectWorld (or null)
	ObjectID lights_id_;               // the death-flash light route (or null)
	HashMap<String, ObjectID> husked_;          // canonical mission identity -> husk node (invalid = null)
	HashMap<String, HuskRestore> husk_restore_; // canonical mission identity -> original state
	HashMap<String, int64_t> attached_groups_;
	HashMap<String, WreckFire> burning_;        // canonical wreck owner key -> WreckFire
	HashSet<String> wreck_anchor_keys_;         // registered wreck owner keys
	HashMap<int, Vector3> piece_pos_;           // piece slot -> Vector3 (anchor resolver source)
	HashMap<int, int64_t> piece_generation_;    // piece slot -> presented allocation generation
	HashMap<int, ObjectID> piece_models_;       // piece slot -> its piece model (invalid = none)
	int64_t stat_husk_swaps_ = 0;
	int64_t stat_no_husk_ = 0;
	int64_t stat_pieces_peak_ = 0;
	int64_t stat_debris_triangles_ = 0;
	int64_t stat_effects_ = 0;
	int64_t stat_sounds_ = 0;
	int64_t stat_glass_points_ = 0;
	int64_t stat_crackles_ = 0;
};

} // namespace godot
