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
#include "simulation/husk_swap.h"
#include "simulation/present_stats.h"

namespace godot {

class EffectLightDirector;
class EntityPresenter;
class MissionAudio;
class Simulation;

// THE shell destruction-presentation pass (the former
// destruction_present_pass.gd, ADR 0043 d9), an owned member of
// EntityPresenter: presents the sim's item destruction on the viewing peer —
// the husk model swap on destroyed items, the death-piece models riding the
// sim's piece pool, the death flashes and the destruction sounds. The effect
// rows (the section debris, the wreck banks, the piece trails, the vehicle
// smoke and fire) are the engine's, spawned and re-posed in the world's scene
// (world::DestructionEmitters). Drains
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
//    (render: Render_SectorEntity @ 0x5C4190 (the Flags & 4 test @ 0x5C41A0);
//    collision: Entity_RaycastCollisionModel @ 0x413060 (the Flags & 4 pick
//    @ 0x41307B..0x413088); no husk -> the graphic keeps standing, the
//    witnessed fallback);
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
// The husk render pick, the presentation identity key and the settle tilt
// retention are the engine's (runtime/world/present_passes.h). A RefCounted
// (registered internally, never script-visible).
class DestructionPresenter : public RefCounted {
	GDCLASS(DestructionPresenter, RefCounted)

public:
	// `owner` resolves runtime-only packed handles (the wire walk's nodes);
	// `audio` (nullable) gates the sound leg; `lights` (nullable) takes the
	// death-flash light route (EffectLightDirector.on_death_light) (engine:
	// runtime/renderer/light_scene.h).
	void setup(EntityPresenter *p_owner, Simulation *p_sim, Node3D *p_container,
			const Ref<EntityIndex> &p_index, const Ref<MissionObjectPlacer> &p_placer,
			const Ref<ItemDatabase> &p_item_db, MissionAudio *p_audio,
			EffectLightDirector *p_lights);
	void teardown();
	// Discard mission-run presentation state without discarding setup dependencies.
	// This is the Stop -> Play boundary as well as the teardown primitive: restore
	// intact visuals, remove transient husk grafts and the piece models.
	void reset_runtime_state();
	// Once per present, after the sim advanced (beside the fire pass).
	void present();
    // Retained rows can arrive before or after the transient death drain. A swap
    // on an entity's own node stamps the husk (and the swap stage's pending final
    // husk) on that ObjectModel, which the present walks read directly
    // (ObjectModel::get_husk_visual / get_final_husk_overlay). False only when
    // a runtime-only owner's node is not built yet: the caller hands the pick
    // again (a retained row's walk), every other outcome stands.
    bool apply_husk_swap(const opennova::world::HuskSwapEvent &p_husk);
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
	// A water mirror's own piece draws (view 0 the main scene's mirror, 1 the
	// Inset's: the models' mirror state, drawn by twins where it differs).
	void apply_piece_draws_mirror(int p_view,
			const std::vector<opennova::world::DeathPieceDraw> &p_draws);
	// A piece slot's model (null when the slot presents none).
	ObjectModel *piece_model(int p_slot) const;
	// The section draw's matrix in Godot space: the piece pose (the BAM
	// heading, pitch, roll) scaled at the piece position, times the drawn
	// section's centre moved to the origin when the draw pivots.
	static Transform3D piece_draw_transform(const opennova::world::DeathPieceDraw &p_draw);
	// Typed diagnostic counters (ADR 0017: cross-object contracts are typed
	// records) — probes assert the presentation legs actually ran.
	Ref<DestructionPresentStats> get_stats() const;

protected:
	static void _bind_methods();

private:
	// The original state behind one husk graft (simulation/husk_swap.h) and
	// the identity it was keyed by; `entity` the model an individual graft
	// stamped its husk on (null for a carved static).
	struct HuskRestore {
		HuskGraft graft;
		int64_t spawn_origin = 0;
		int wire_handle = -1;
		ObjectID entity;
	};
	// The swap stage's pending final husk and the entity model it is stamped
	// on (null when it draws beside a carved static's graft).
	struct FinalOverlay {
		ObjectID overlay;
		ObjectID entity;
	};

	Simulation *sim() const;
	Node3D *container() const;
	MissionAudio *audio() const;
	EffectLightDirector *lights() const;
	Node3D *resolve_entity_node(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle) const;
	void restore_intact(const String &p_key);
	// The husk's candidate graphics for one swap: the def's husk, or the final
	// husk's (runtime/world/present_passes.h) once the swap stage promoted it.
	std::vector<String> husk_graphics(int p_def_id, bool p_husk_final) const;
	// The swap stage's pending final husk drawn beside the husk (a show builds it
	// beside the graft, or under the entity's node when no husk grafted).
	void apply_final_overlay(const String &p_key, const opennova::world::HuskSwapEvent &p_husk);
	void drop_final_overlay(const String &p_key);
	// A show whose entity node had not materialized (a budgeted cold row) waits here and
	// builds once that node's husk grafts.
	void retry_pending_final_overlay(const String &p_key);
	Variant present_transform_for_identity(int p_bms_id, int64_t p_spawn_origin) const;
	void sync_static_husks();
	void apply_sound(const String &p_name, const Vector3 &p_pos);
	void present_pieces(const std::vector<opennova::world::DeathPieceRow> &p_pieces);
	ObjectModel *build_piece_model(int p_slot, int p_item_id);
	void free_piece_model(int p_slot);

	EntityPresenter *owner_ = nullptr; // runtime-only packed handles
	ObjectID sim_id_;                  // live pose source; null in data-driven tests
	ObjectID container_id_;            // mission container (node-less husk grafts land here)
	Ref<EntityIndex> index_;
	Ref<MissionObjectPlacer> placer_;  // husk model builds
	Ref<ItemDatabase> item_db_;        // husk graphic names
	ObjectID audio_id_;                // MissionAudio (or null)
	ObjectID lights_id_;               // the death-flash light route (or null)
	HashMap<String, ObjectID> husked_;          // canonical mission identity -> husk node (invalid = null)
	HashMap<String, HuskRestore> husk_restore_; // canonical mission identity -> original state
	HashSet<String> final_husked_;              // identities whose applied husk is the final husk
	HashMap<String, FinalOverlay> final_overlays_; // identity -> the pending final husk drawn beside it
	HashMap<String, opennova::world::HuskSwapEvent> pending_final_overlays_; // identity -> a show awaiting its node
	HashMap<int, int64_t> piece_generation_;    // piece slot -> presented allocation generation
	HashMap<int, ObjectID> piece_models_;       // piece slot -> its piece model (invalid = none)
	int64_t stat_husk_swaps_ = 0;
	int64_t stat_no_husk_ = 0;
	int64_t stat_pieces_peak_ = 0;
	int64_t stat_debris_triangles_ = 0;
	int64_t stat_sounds_ = 0;
	int64_t stat_crackles_ = 0;
};

} // namespace godot
