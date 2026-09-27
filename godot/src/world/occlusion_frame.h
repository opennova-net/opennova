#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>

#include "devtools/frame_stats.h"

namespace opennova::renderer {
struct ScenePassGateEdges;
} // namespace opennova::renderer

namespace godot {

class Celestial;
class EntityIndex;
class EntityPresenter;
class FoliageDispatcher;
class MissionEnvironment;
class MissionObjectPlacer;
class ObjectModel;
class Simulation;
class SkyDome;
class SlotShadow;
class Terrain;
class Water;

// The render-occlusion frame (the former occlusion_frame_pass.gd, ADR 0043
// d9; docs/render/render-occlusion-re.md §3/§4/§5): the blink letter gates,
// the per-render-frame section-mask/portal apply, the probe A/B seam edges,
// and the unload reset. Owned by GameWorld as _occlusion — constructed in
// the world's _init, wired to the retained scene nodes in _ready (setup),
// re-handed the mission runtime's sim / entity index / entity presenter on
// every load (bind_mission) and released on unload (reset). GameWorld's
// frame leg drives it through apply_occlusion_frame().
//
// HOT PATH: GameWorld's device frame calls apply_blink_gates()/apply_frame()
// directly every frame. Every entry here is a plain method call over
// ObjectID-resolved members — ZERO Callables/lambdas anywhere in the frame
// path (the dispatcher-perf rule) — and steady frames allocate nothing at
// this seam (the sim's delta reads return empty packed arrays).
//
// Diff-applied: the sim emits verdict CHANGES (get_building_visibility_changes /
// get_render_culled_changes) and only transitions touch nodes, so a steady frame
// does no per-node work. Two ownership bits on each ObjectModel decide final
// visibility: the entity presenter's placed walk owns the sim's intent
// (PF_HIDDEN -> set_present_visible), this frame owns the occlusion hide
// (set_occlusion_hidden), and the node's visible flag is their product — the
// placed walk never fights an occlusion hide, and an occlusion release lands on
// the sim's current intent so a sim-hidden entity never flashes (the bit
// contract is pinned by mission_present_pass_test).
//
// ORDER, preserved from the GDScript pass:
//  * present THEN occlude: the entity presenter's placed walk runs inside
//    advance_session_frame (MissionRoot presents once per session frame) and
//    GameWorld's occlusion leg runs AFTER it, so present's base
//    visibility is re-asserted first each frame and this frame's hides land
//    on top. The leg table keeps that order; nothing here
//    reorders it.
//  * water.visible has TWO writers in one frame: GameWorld.
//    render_water_frame reads LAST frame's blink-water verdict
//    (Water.set_blink_water_visible, exactly as retail reads it — the
//    Terrain_SetupViewAndLighting 0x60fe40 witness cited on that leg),
//    then apply_frame below rewrites the node's visibility from THIS
//    frame's verdict. Both stay.
//  * the world's is_water_render_active() gate decides the water_z handed to
//    run_occlusion_frame: an inactive water (no load, a load in progress)
//    passes the -100000 sentinel, never its retained authored height.
//  * the FoliageDispatcher sits BESIDE the Terrain (game_world.tscn), so the
//    terrain hide never reaches the MODEL masks: the indoors letter closes
//    the dispatcher's detail passes alone (set_detail_passes_drawn), while
//    the masks and their anchors keep running, as retail's BySide waves do.
//  * the 11 OCCL_* FrameStats slots land here while the Stats tab captures,
//    OCCL_GLUE as the bound-call REMAINDER (native span minus the sim's
//    build + probe split) so the pane's Occlusion group still sums to the
//    whole frame cost; frame_stats_probe requires the occl_apply row.
//  * a building or collector verdict whose bms_id resolves to no
//    ObjectModel (a batched static population, never a node) lands on the
//    placer's retained instance instead
//    (MissionObjectPlacer::set_static_instance_occlusion_hidden): the next
//    static LOD walk in the same frame drops or restores its rows.
class OcclusionFrame : public RefCounted {
	GDCLASS(OcclusionFrame, RefCounted)

public:
	// One-time wiring to the world's retained scene nodes (the terrain,
	// foliage, sky dome, celestial and water renders the blink letters gate,
	// and the environment the frame reads fog distance + light direction
	// off). Any may be null (a code-built world without that node).
	void setup(Terrain *p_terrain, FoliageDispatcher *p_foliage, SkyDome *p_sky,
			Celestial *p_celestial, Water *p_water, MissionEnvironment *p_env);
	// The per-mission members, re-handed on every runtime start (the pass
	// re-read them off the world's runtime per frame; they only change at a
	// load, and reset() forgets them at unload). Held by ObjectID and
	// re-resolved on use, so a freed runtime reads as absent, never dangling.
	void bind_mission(const Ref<Simulation> &p_sim, const Ref<EntityIndex> &p_index,
			EntityPresenter *p_entities, const Ref<MissionObjectPlacer> &p_placer);
	// The shared F3 frame-stats board (null outside the game shell), re-handed
	// by GameWorld wherever its own board changes: this frame lands the
	// OCCL_* split spans itself from apply_frame.
	void set_frame_stats(const Ref<FrameStats> &p_board);

	// --- Blink frame gates (docs/render/render-occlusion-re.md §4) ---------
	// The local player's accumulated blink letters gate whole render passes.
	// The letters are authored PER BOX (init 0x3E; letters clear bits), so
	// windowed buildings simply don't carry the indoors letter and keep the
	// outside world rendering — no portal special-casing at the gate level.
	// The per-section interior visibility masks (the portal traversal) are
	// apply_frame's.
	//
	// Re-apply the letter gates after a sim tick. `forces_indoors` is
	// GameWorld._mission_forces_indoors, passed per call — the mission
	// attribute is mission state and stays (test-pinned) on the world.
	// Returns the passes the new letters reopen (renderer::
	// scene_pass_gate_edges): the frame's terrain and water legs already ran
	// under the old letters, so the caller re-runs the reopened ones.
	opennova::renderer::ScenePassGateEdges apply_blink_gates(bool p_forces_indoors);

	// --- The render-occlusion frame (render-occlusion-re.md §3/§5) ---------
	// Per render frame: run the sim's occlusion pipeline (camera blink query ->
	// portal traversal -> section masks + TOC occluder culling + the entity
	// render gates), then drive the de-batched building nodes' per-section
	// masks and the gated entities' visibility. Runs after the present pass
	// (inside the session frame) so present's base visibility is re-asserted
	// first each frame. `camera` is the camera drawing the frame's image (the
	// stretched-frame target's camera while it is live; null for a headless
	// world): the frustum comes off it (ObjectLodFrame::camera_tangents), else
	// the 70 deg / 0.05 / 16:9 defaults. `viewport_width` is the surface width
	// in pixels, the projector's focal source. (The marched iris-exposure
	// weather feed that renders alongside stays on GameWorld's frozen-pose
	// iris stamp.)
	void apply_frame(Camera3D *p_camera, float p_viewport_width,
			const Transform3D &p_camera_xform, bool p_forces_indoors);
	// The weapon Inset pass's own collect, after this frame's main one
	// (engine: world/occlusion.h OcclusionView carries the witness): the sim
	// runs it over `camera` on the Inset view's frame words, and its verdicts
	// land as changes on the models' and the batched statics' Inset state
	// (ObjectModel::set_inset_occlusion_*, MissionObjectPlacer::
	// set_static_instance_inset_occlusion_hidden); an entity the Inset collect
	// gives no verdict follows the main view's. `viewport_width` is the Inset
	// target's width, the projector's focal source for that view.
	void apply_inset_frame(Camera3D *p_camera, float p_viewport_width, bool p_forces_indoors);
	// The Inset stopped rendering (or the world unloads): every Inset verdict
	// releases onto the main view's and the sim forgets the Inset baselines.
	void release_inset();
	// The last Inset collect's MODEL foliage anchors (Godot space; the main
	// collect's are Simulation::get_foliage_mask_anchor_positions), empty
	// while the Inset is closed.
	const PackedVector3Array &get_inset_foliage_mask_anchors() const {
		return inset_foliage_mask_anchors_;
	}

	// The probe A/B seam, entering the occlusion skip: restore the water to
	// the authored blink state, then release every occlusion override.
	// Mission blink/indoors semantics remain authoritative (release keeps
	// them; iris keeps sampling on the world).
	void enter_probe_skip();
	// Leaving the skip: re-arm a full re-emit from the sim's delta baseline.
	void leave_probe_skip();
	// Placed nodes arrived after the occlusion frames started (a joiner's
	// streamed statics, placed once the host's world stream settles): forget
	// the node lookups that missed and the sim's applied-state baseline, so
	// the next frame re-emits every building/cull/sun verdict onto the new
	// nodes instead of only the transitions since the mission began.
	void rebind_placed_nodes();
	// Unload teardown. The ordering replicates the pre-extraction unload
	// EXACTLY:
	// 1. the blink letter gates restore [orig: the letter-bit clear @ 0x525c45
	//    at mission start] — an unload while indoors must not leave the next
	//    mission's terrain/sky/water hidden;
	// 2. every occlusion override releases (the hidden bits this frame set
	//    clear onto each node's own present intent) and the sim delta
	//    baseline resets;
	// then the mission binding is forgotten (the runtime dies with the load).
	void reset();
	// Release every occlusion override: clear the occlusion-hidden bit on
	// every node this frame touched (the node lands on the sim's CURRENT
	// present intent — the placed walk's own bit — so a WAC/sim-hidden entity
	// never flashes for a frame), clear section masks to fully-visible, drop
	// the caches, and forget the sim's delta baseline so a later re-enable
	// re-emits full state. The A/B seam keeps mission blink/indoors semantics
	// (reset_semantics=false); reset_semantics = true marks the unload-time
	// release — the mission force-indoors attribute itself stays on GameWorld
	// (test-pinned by name) and unload clears it THERE, so the flag carries
	// no extra work here.
	void release_overrides(bool p_reset_semantics);

	// The local player's latched indoors letter (accum bit 0x2); the frame
	// legs read it natively.
	bool is_blink_indoors() const { return blink_indoors_; }
	// Re-derive the scene pass gates from the latched letters and the eye's
	// CURRENT waterline side: the letters change per sim tick
	// (apply_blink_gates), the eye per display frame (GameWorld's scene
	// environment leg calls this after classifying the render eye).
	void apply_scene_pass_gates();
	// Mirrors GameWorld._perf_probe_enabled, written only at the probe
	// toggle edge (set_perf_probe_enabled): the manual A/B probe shares
	// apply_frame's clock reads with the F3 Stats capture.
	void set_probe_timing(bool p_enabled) { probe_timing_ = p_enabled; }

protected:
	static void _bind_methods();

private:
	// The live sim for the occlusion apply paths (null before a mission
	// runtime exists — a real state on the unload/A-B seams).
	Simulation *sim() const;
	EntityIndex *entity_index() const;
	EntityPresenter *entities() const;
	MissionObjectPlacer *placer() const;
	Terrain *terrain() const;
	FoliageDispatcher *foliage() const;
	SkyDome *sky() const;
	Celestial *celestial() const;
	Water *water() const;
	MissionEnvironment *environment() const;
	// The world's slot-shadow device (the terrain's sibling, resolved at
	// setup): its drapes ride the terrain gate.
	SlotShadow *slot_shadow() const;
	// Resolve (and cache) the ObjectModel a bms_id drives. Cache entries
	// revalidate through ObjectDB (LIVENESS); a freed node re-resolves
	// through the registry (reloads recreate nodes under the same ids).
	ObjectModel *occlusion_node(EntityIndex *p_registry, int64_t p_bms_id);
	// The cache holds only ObjectModels; entries revalidate for LIVENESS on use.
	ObjectModel *occlusion_hidden_release_node(int64_t p_bms_id) const;
	// The wire walk's applied render-gate verdicts pair with the sim's
	// baseline: both forget together, so the next frame's full re-emit
	// lands on a clean set.
	void clear_wire_render_culled();
	// Forget the sim's applied-state baseline so the next occlusion frame
	// re-emits everything (the shell caches were dropped or the A/B skip
	// ended).
	void reset_apply_baseline();
	void reset_blink_frame_gates();

	// The retained scene nodes (setup) and the per-mission members
	// (bind_mission), every one an ObjectID re-resolved on use.
	ObjectID terrain_id_;
	ObjectID foliage_id_;
	ObjectID sky_id_;
	ObjectID celestial_id_;
	ObjectID water_id_;
	ObjectID env_id_;
	ObjectID slot_shadow_id_;
	ObjectID sim_id_;
	ObjectID index_id_;
	ObjectID entities_id_;
	ObjectID placer_id_;
	Ref<FrameStats> frame_stats_;
	// The local player's latched blink letters (render-occlusion-re.md §4):
	// accum bit 0x2 hides the terrain render, the detail-foliage passes (not
	// the MODEL masks) and the mirror's sky bracket, bit 0x4 the main frame's
	// sky bracket (apply_scene_pass_gates), bit 0x8 the water passes.
	uint32_t blink_letters_ = 0;
	bool blink_indoors_ = false;
	// The last terrain gate written (edge-triggered).
	bool terrain_gate_ = true;
	bool blink_water_suppressed_ = false;
	bool probe_timing_ = false;
	// bms_id -> resolved ObjectModel, so steady frames skip registry lookups;
	// every node this frame hid or masked is in it, so the release walk (A/B
	// seam, unload) finds the bits it set. Entries revalidate through
	// ObjectDB on use (LIVENESS, not typing); reset on unload/A-B seams.
	HashMap<int64_t, ObjectID> occlusion_node_cache_;
	// The models an Inset collect gave a verdict since the Inset opened (the
	// release walk's set), and whether an Inset collect ran since then.
	HashMap<uint64_t, ObjectID> inset_nodes_;
	bool inset_active_ = false;
	PackedVector3Array inset_foliage_mask_anchors_;
	// The two apply_frame halves, valid while probe/stats timing runs: the
	// native run_occlusion_frame call and the node application.
	int64_t perf_occl_native_us_ = 0;
	int64_t perf_occl_apply_us_ = 0;
};

} // namespace godot
