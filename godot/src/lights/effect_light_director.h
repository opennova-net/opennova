#pragma once

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

#include <runtime/world/present_drains.h>

#include "lights/effect_light_report.h"
#include "lights/light_scene.h"
#include "mission/static_source_provider.h"
#include "object/model_light.h"
#include "object/object_model.h"

namespace godot {

class Camera3D;
class EnvLightValues;
class MissionEnvironment;
class MissionRoot;
class MissionObjectPlacer;
struct SceneOverlaySubmission;
class Simulation;
class Weather;

// The EffectWorld dynamic point-light director (the former
// effect_light_director.gd, ADR 0043 d9): spawns one pool light per
// authored model light record for every placed entity, and drives the
// per-frame select that feeds the technique shaders' global parameters. The
// witness map lives on engine/runtime/renderer/light_scene.h — mission
// start walks the placed pools spawning per-record instances
// [orig: Game_StartMission @ 0x525d19 -> Game_SpawnAllEntityGlowEffects @0x5227b0 ->
// Entity_SpawnGlowEffects @ 0x56c7c0], and each draw selects the nearest
// group-passing three [orig: Light_SelectAndEnableForDraw @ 0x5ab9d0 ->
// Light_CollectNearbyZonesByAABB @ 0x5aa250; the batch collectors' group gate
// @ 0x5d91f8 / @ 0x5d96b8 and 3-cap @ 0x5d9229]. The object pass runs per
// rendered model: one draw context per visible ObjectModel carrying BOTH
// witnessed groups — the owner group its submit declares, and the building
// it stands inside plus that blink volume's section as the interior group —
// so owned lights (muzzle glow, subobject records, interior room lights)
// light only what retail's collectors admit. Corona billboards draw per frame from
// the portable corona walk [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40].
// The remaining D-RLIT-4 residual is foliage sampling. Authored LGHT
// positions/lifetimes are spawn-fixed; powerup respawn is routed, and a
// husk swap neither moves nor rescans lights (retail call graph cited
// below). Device only: the group policies live with the engine pool
// (renderer::resolve_model_light_owner, static_light_row_groups).
class EffectLightDirector : public RefCounted {
	GDCLASS(EffectLightDirector, RefCounted)

public:
	// Model gather half-extent around the camera. Light ranges are authored
	// small (atten_end 8 on the fire barrels), so any model a pool light
	// could touch sits well inside this radius.
	static constexpr float QUERY_RADIUS = 512.0f;

	EffectLightDirector();

	void setup(Node *p_world, const Ref<MissionObjectPlacer> &p_placer);
	// The C++ world's wiring (ADR 0043 slice G10): the same three seams as
	// typed reads off the world's StaticSourceProvider, no Callables lent.
	void setup_with_provider(Node *p_world, StaticSourceProvider *p_provider);
	// Mission teardown: disconnect live node retirement hooks, retire every
	// pool lease, and synchronously clear the shader-global output.
	void reset();
	// Mission start / sim-restart: reset, then respawn from the restored
	// entity set — the same lifecycle the item-effect director uses.
	void reattach();
	// The one owner id space: a model's sim wire handle when the present
	// pass stamped one (live entities — the same domain fire events report
	// shooters in), else the node instance id (placer statics, preview
	// scenes). Light spawns and per-draw contexts must agree on this or
	// owner gating never matches.
	static int64_t owner_id_for_node(ObjectModel *p_node);
	// Wire/late spawns route here through the world's spawn router (shared
	// with the item-effect director).
	void on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id);
	// One authored light record (ObjectData.get_light_info's ModelLight)
	// becomes one pool instance. Public: the GUT seam test feeds records
	// directly. The owner attach is decided by the portable policy the
	// config feeds (renderer::resolve_model_light_owner): a record attached
	// to a subobject is owned by its own entity + that subobject (cabin
	// self-lights), an unattached record spawned INSIDE a blink box is owned
	// by the containing building + that volume's section (interior room
	// lights), and everything else — the fire barrels — spawns unowned and
	// lights the world [orig: Entity_SpawnGlowEffects @ 0x56c89f /
	// @ 0x56c8bd]. Every caller supplies an owner id — live nodes their wire
	// handle/instance id, batched static sources a synthetic negative id —
	// so an owned light passes the per-draw select only for the draws
	// retail admits (per_model_light_isolation_test pins both directions).
	// `blink_owner` is the [owner id, section] pair the blink query at the
	// spawner's position resolved (PackedInt64Array of two), empty outdoors.
	int64_t spawn_light_record(const Ref<ModelLight> &p_info, const Transform3D &p_world_transform,
			int64_t p_owner_id = 0, const PackedInt64Array &p_blink_owner = PackedInt64Array(),
			bool p_spawner_is_building = false);
	// Typed accessors for co-consumers of the shared pool (the render-slot
	// shadow device's dominant-light pick reads the same LightScene).
	Ref<LightScene> scene() const;
	Vector3 light_gain() const;
	// The owner id a containing building's BMS id resolves to -- the id its
	// interior room lights are owned by, for a co-consumer that stamps an
	// entity's interior light group (the render-slot pick).
	int64_t interior_owner_for_bms(int p_bms_id);
	// The per-frame device leg (the GameWorld leg table, after iris, before the
	// material frame): one draw context per visible ObjectModel near the
	// camera (the drawn entity id rides along; LightScene::render_model_frame
	// applies the witnessed owner-group rule) plus the first-person viewmodel
	// parts, which take the local player's query and interior group but
	// declare no owner group. The FLICKER phase reads the live weather wave
	// ring; the ambient scale is the env light-state gain (the ported
	// EffectWorld_AmbientScale channel).
	void render_frame(Camera3D *p_camera, int64_t p_time_ms,
			const TypedArray<ObjectModel> &p_viewmodel_parts = TypedArray<ObjectModel>(),
			bool p_run_census = true);
	// On-demand census refresh for report readers while the capture is off:
	// the skipped select re-runs with the last frame's camera, so an
	// MCP/diagnostics read stays exact without the per-frame report cost.
	void run_census_now();
	// The weapon Inset pass's own legs, once per frame while it renders
	// (GameWorld's material leg, after the Inset collect and both views' level
	// walks settled the twins): retail runs the scene core again for that
	// view (renderer/scene_overlay.h kInsetOverlayOrder carries the witness),
	// so the draws it makes that the main view does not (ObjectModel's view
	// twins) select their own lights, and its corona walk runs over the Inset
	// camera at the next phase, owned coronas gated on each drawn owner's
	// Inset section mask. release_inset_frame drops the walk when the pass
	// stops.
	void render_inset_frame(Camera3D *p_camera, int64_t p_time_ms);
	void release_inset_frame();
	// The Inset walk's quad count this frame (the typed read-back).
	int get_inset_corona_count() const { return static_cast<int>(inset_coronas_.size()); }
	// This frame's corona billboards into the post-particle overlay tail
	// (renderer/scene_overlay.h): the main walk's, and the Inset walk's in its
	// own slot. Not bound to Godot.
	void append_overlay(SceneOverlaySubmission &r_submission);
	// The 62 Hz lifecycle decay [orig: EffectWorld_TickInstancesAndLightScale
	// @ 0x5aa170 from the main loop] — beside EffectWorld.advance_fixed_tick.
	void advance_fixed_tick();
	// One weapon fire with the ammo MF_Light flag [orig: Entity_UpdateMuzzleGlow-
	// Effect @ 0x56c960, called per shot from both fire arms]. Owner = the
	// shooter, so the per-draw owner select (render_model_frame) admits the
	// glow only on draws declaring that owner: the shooter's skinned person
	// draws. The rigid held gun re-scopes to (0, robj) and the first-person
	// pass declares none, so neither takes the glow (renderer::
	// submit_owner_group). The cache is deliberately shared with model LGHT: if
	// mission-start spawn left entity+0x1B4 nonzero, retail re-arms and moves
	// that final authored lease instead of allocating the 1.5-unit
	// muzzle-color light.
	void on_muzzle_fire(int64_t p_shooter_handle, const Vector3 &p_world_pos);
	// One presented round impact whose ammo authors light_impact [orig:
	// AmmoDef_ProcessImpactEffect @ 0x40a2b3 — spawned radius/2 above the
	// impact, mode 2 fade, render flag 0x100]. The 0x100 flag's one witnessed
	// reader is the corona walk: the billboards re-center radius/2 below the
	// light, back onto the impact point [orig: EffectWorld_RenderLightCoronas
	// @ 0x5ab037..0x5ab05c].
	void on_impact_light(const Vector3 &p_world_pos, float p_radius, const Color &p_color,
			int p_duration_ticks);
	// One husk death flash [orig: Entity_SpawnDeathPieces @ 0x49351a — at the
	// entity position, 2x the piece model radius, corona disabled].
	void on_death_light(const Vector3 &p_world_pos, float p_radius);
	// The in-flight light_move glows, diffed against the sim's live rows
	// [orig: RoundData_SpawnRound @ 0x4ec8da spawn (mode 1, radius/2 up,
	// terrain disabled), the per-tick follow @ 0x4eaa9f, Projectile_ReleaseEffects
	// clear]. Rows: world::RoundGlowRow from Simulation::fill_round_glows; the lift
	// law is the engine's (renderer/light_scene.h round_glow_spawn_lift /
	// kRoundGlowFollowLift).
	void sync_round_glows(const std::vector<opennova::world::RoundGlowRow> &p_rows);
	// The bound data leg: the same sync over test-authored RoundGlowRow records.
	void sync_round_glow_records(const Array &p_rows);
	Ref<EffectLightReport> get_report();

	// The bound signal targets: a spawned wire node leaving the tree, and
	// the MissionObjects container's membership changing.
	void _on_wire_node_exiting(int64_t p_node_id);
	void _on_container_membership_changed(Node *p_node);

protected:
	static void _bind_methods();

private:
	// One wire node whose authored lights this director spawned: the node,
	// the light owner id it was scoped to, and the one-shot tree_exiting
	// callback bound to it.
	struct SpawnedNode {
		ObjectID node;
		int64_t owner_id = 0;
		Callable tree_exiting;
	};
	// The blink-box owner at one world point: the containing building's
	// owner id + section, owner 0 outdoors.
	struct BlinkOwner {
		int64_t owner = 0;
		int section = 0;
		bool valid() const { return owner != 0; }
	};

	Node *_world() const;
	MissionRoot *_runtime() const;
	Ref<Simulation> _sim() const;
	// Native placer reads through the live world or a retained placer provider.
	std::vector<opennova::mission::StaticEffectSource> _static_sources() const;
	std::vector<opennova::mission::StaticLightDrawSource> _static_draw_sources() const;
	int64_t _static_draw_source_revision() const;
	MissionEnvironment *_environment() const;
	Weather *_weather() const;
	Node *_mission_objects() const;
	void _disconnect_wire_node_exit(const SpawnedNode &p_record);
	Vector<int64_t> _spawn_model_lights(const Ref<ObjectData> &p_data,
			const Transform3D &p_world_transform, int64_t p_owner_id,
			const BlinkOwner &p_blink_owner, bool p_spawner_is_building);
	int64_t _spawn_node_lights(ObjectModel *p_node, int64_t p_owner_id,
			const BlinkOwner &p_blink_owner, bool p_spawner_is_building);
	int64_t _spawn_light_at(const Ref<ModelLight> &p_info, const Vector3 &p_world_pos,
			int64_t p_owner_id, const BlinkOwner &p_blink_owner, bool p_spawner_is_building);
	BlinkOwner _blink_owner_at(const Vector3 &p_world_pos);
	int64_t _owner_id_for_bms(int p_bms_id);
	// A placed record's BUILDING identity: its items.def type, not its BMS
	// record family (engine: mission::placed_record_is_building).
	bool _record_is_building(int p_kind, int p_item_id) const;
	void _render_static_light_rows(const Vector3 &p_gain, Weather *p_weather, int p_time_ms);
	void _rebuild_static_light_rows();
	void _ensure_model_registry(Node *p_container);
	void _rebuild_model_registry(Node *p_container);
	BlinkOwner _local_player_interior_group();
	// One registry row's draw context into the frame arrays (the entity
	// query, the owner, the ROBJ scope and the interior group).
	void _push_model_draw(ObjectModel *p_model, int64_t p_reg_index);
	void _clear_frame_draws();
	void _render_coronas(Camera3D *p_camera, const Vector3 &p_gain, int p_time_ms,
			Weather *p_weather,
			const TypedArray<Node3D> &p_models, const PackedInt64Array &p_owners,
			MissionEnvironment *p_env);
	Ref<EnvLightValues> _corona_fog(MissionEnvironment *p_env) const;
	void _clear_coronas();
	Ref<ImageTexture> _corona_texture();

	ObjectID world_id_;
	// Native provider; bound setup retains its real placer for the same seam.
	StaticSourceProvider *provider_ = nullptr;
	Ref<MissionObjectPlacer> placer_provider_;
	// The packed static atlas rows, rebuilt only when the placer's
	// draw-source revision (rows appended, table reset, carve state) or the
	// static source snapshot changes. Rows are immutable identities; only
	// the light SELECTION over them runs per frame, as retail's per-batch
	// select does.
	int64_t static_rows_revision_ = -1;
	PackedVector3Array static_rows_positions_;
	PackedInt32Array static_rows_bound_radii_q16_;
	PackedInt64Array static_rows_owner_entities_;
	PackedInt32Array static_rows_owner_sections_;
	PackedInt64Array static_rows_interior_owners_;
	PackedInt32Array static_rows_interior_sections_;
	PackedByteArray static_rows_active_;
	PackedVector4Array static_rows_entity_lights_;
	Ref<LightScene> scene_;
	HashMap<int, Vector<int64_t>> spawned_static_;
	std::vector<opennova::mission::StaticEffectSource> static_sources_snapshot_;
	HashMap<int, int64_t> static_owner_by_bms_;
	HashMap<uint64_t, SpawnedNode> spawned_nodes_;
	// Entity owner id -> its ONE cached EffectWorld handle. Retail does not
	// own a model-light handle array: every LGHT spawn overwrites
	// entity+0x1B4, the MF_Light muzzle path reuses that same word, and
	// Entity_Destroy clears only its final value [orig: Entity_SpawnGlowEffects
	// @0x56c925..0x56c92c; Entity_UpdateMuzzleGlowEffect @0x56c965..0x56c9d5;
	// Entity_Destroy @0x43e903..0x43e916]. Earlier model lights intentionally
	// remain in the pool until mission teardown, matching retail's lifecycle.
	HashMap<int64_t, int64_t> entity_effect_handles_;
	// round presentation id -> pool light handle (the light_move follow).
	HashMap<int64_t, int64_t> round_handles_;
	// This frame's corona quads for the post-particle overlay stage, from the
	// portable corona walk [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 —
	// the witness map lives on renderer::LightScene::collect_corona_quads].
	std::vector<opennova::renderer::LightCoronaQuad> coronas_;
	// The weapon Inset pass's own walk this frame (render_inset_frame).
	std::vector<opennova::renderer::LightCoronaQuad> inset_coronas_;
	// The frame & 3 jitter phase: every walk advances it, as retail's
	// effect-world prologue does once per scene pass (scene_overlay.h
	// kInsetOverlayOrder).
	int corona_frame_ = 0;
	// The procedural corona texture "texlightcrn": the law (the 128x128
	// 0.4 - 0.45 d falloff, truncated, the transparent border) lives
	// portable in renderer::corona_texture_argb; this only wraps the bytes in
	// a texture, once per director.
	Ref<ImageTexture> corona_texture_;
	// Containing-building bms_id -> owner id. Owner identity is fixed for a
	// node's tree lifetime (entity_ref is stamped once at creation), so the
	// cache survives across frames and clears on container membership
	// change, reset, and reattach.
	HashMap<int, int64_t> blink_owner_cache_;
	// The report-only census skipped this frame (F3 capture off);
	// run_census_now refreshes it on demand with the last frame's camera and clock.
	bool census_stale_ = false;
	Vector3 census_cam_pos_;
	int census_time_ms_ = 0;
	// Per-frame walk registry: MissionObjects children that are ObjectModels
	// with their entity_ref identity read once at (re)build. Membership
	// changes mark it dirty (child_entered_tree/child_exiting_tree on the
	// container); a husk swap neither exits the node nor rewrites
	// entity_ref, so rows stay valid.
	Vector<ObjectID> reg_models_;
	PackedInt64Array reg_owners_;
	PackedByteArray reg_robj_scoped_;
	bool reg_dirty_ = true;
	uint64_t reg_container_id_ = 0;
	// Reused per-frame draw-context arrays (the native call reads them
	// whole, so they are cleared, not tail-truncated).
	TypedArray<Node3D> frame_models_;
	PackedVector3Array frame_entity_positions_;
	PackedInt32Array frame_entity_bound_radii_q16_;
	PackedInt64Array frame_owners_;
	PackedInt64Array frame_interior_owners_;
	PackedInt32Array frame_interior_sections_;
	PackedByteArray frame_robj_scoped_;
};

} // namespace godot
