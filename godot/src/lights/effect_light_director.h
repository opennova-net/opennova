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
#include <memory>
#include <vector>

#include <runtime/renderer/light_spawn.h>
#include <runtime/world/present_drains.h>

#include "lights/effect_light_report.h"
#include "lights/light_scene.h"
#include "mission/static_source_provider.h"
#include "object/model_light.h"
#include "object/env_light_state.h"
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
// (engine: runtime/renderer/light_scene.h), and each draw selects the nearest
// group-passing three (engine: runtime/renderer/light_scene.h). The object pass runs per
// rendered model: one draw context per visible ObjectModel carrying BOTH
// witnessed groups — the owner group its submit declares, and the building
// it stands inside plus that blink volume's section as the interior group —
// so owned lights (muzzle glow, subobject records, interior room lights)
// light only what retail's collectors admit. Corona billboards draw per frame from
// the portable corona walk (engine: runtime/renderer/light_scene.h).
// The remaining D-RLIT-4 residual is foliage sampling. Authored LGHT
// positions/lifetimes are spawn-fixed; powerup respawn is routed, and a
// husk swap neither moves nor rescans lights (retail call graph cited
// below). Device only: the group policies live with the engine pool
// (renderer::resolve_model_light_owner, static_light_row_groups).
class EffectLightDirector : public RefCounted {
	GDCLASS(EffectLightDirector, RefCounted)

public:
	EffectLightDirector();
	~EffectLightDirector() override;

	void setup(Node *p_world, const Ref<MissionObjectPlacer> &p_placer);
	// The C++ world's wiring (ADR 0043 slice G10): the same three seams as
	// typed reads off the world's StaticSourceProvider, no Callables lent.
	void setup_with_provider(Node *p_world, StaticSourceProvider *p_provider);
	// An embedder with no GameWorld (the editor's mission picture, ADR 0046
	// DI-31): the entities' individual models it draws, in place of the
	// world's MissionObjects walk (what reattach spawns their lights for and
	// what each frame's per-model pass selects for), and the environment and
	// weather the lights read (the gain, the corona fog, the FLICKER ring).
	// Its statics stay the provider's. No simulation stands behind it: no
	// blink query contains a light or a draw (every one outdoors).
	void set_scene(const TypedArray<ObjectModel> &p_models, MissionEnvironment *p_environment, Weather *p_weather);
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
	// A wire entity left the live present rows (the presenter's retirement,
	// retail's Entity_Destroy): its cached lease retires
	// (renderer::LightSpawner::destroy_entity).
	void on_wire_entity_destroyed(int p_wire_handle);
	// One authored light record (ObjectData.get_light_info's ModelLight)
	// becomes one pool instance. Public: the GUT seam test feeds records
	// directly. The owner attach is decided by the portable policy the
	// config feeds (renderer::resolve_model_light_owner): a record attached
	// to a subobject is owned by its own entity + that subobject (cabin
	// self-lights), an unattached record spawned INSIDE a blink box is owned
	// by the containing building + that volume's section (interior room
	// lights), and everything else — the fire barrels — spawns unowned and
	// lights the world (engine: runtime/renderer/light_scene.h). Every caller supplies an owner id — live nodes their wire
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
	// Each water mirror's own corona walk, once per frame after both mirrors' cameras are placed
	// (GameWorld's scene_overlay leg): the reflected subscene walks the coronas again at the
	// mirror's eye, under its dry fog, owned coronas gated on the section masks that mirror's own
	// collect wrote, at the phase its host pass's prologue had not yet advanced
	// (renderer/scene_overlay.h kMirrorOverlayOrder carries the witness). A null camera (a mirror
	// that does not render this frame) drops that mirror's walk. Not bound to Godot.
	void render_mirror_coronas(Camera3D *p_mirror, Camera3D *p_inset_mirror, int64_t p_time_ms);
	// This frame's corona billboards into the post-particle overlay tail
	// (renderer/scene_overlay.h): the main walk's, and the Inset's and each
	// mirror's walk in its own slot. Not bound to Godot.
	void append_overlay(SceneOverlaySubmission &r_submission);
	// The 62 Hz lifecycle decay (engine: runtime/renderer/light_scene.h) — beside EffectWorld.advance_fixed_tick.
	void advance_fixed_tick();
	// One weapon fire with the ammo MF_Light flag (engine: runtime/renderer/light_scene.h). Owner = the
	// shooter, so the per-draw owner select (render_model_frame) admits the
	// glow only on draws declaring that owner: the shooter's skinned person
	// draws. The rigid held gun re-scopes to (0, robj) and the first-person
	// pass declares none, so neither takes the glow (renderer::
	// submit_owner_group). The cache is deliberately shared with model LGHT: if
	// mission-start spawn left entity+0x1B4 nonzero, retail re-arms and moves
	// that final authored lease instead of allocating the 1.5-unit
	// muzzle-color light.
	void on_muzzle_fire(int64_t p_shooter_handle, const Vector3 &p_world_pos);
	// One presented round impact whose ammo authors light_impact
	// (renderer::impact_flash_spawn carries the witness: radius/2 above the
	// impact, mode 2 fade, render flag 0x100, whose one reader is the corona
	// walk).
	void on_impact_light(const Vector3 &p_world_pos, float p_radius, const Color &p_color,
			int p_duration_ticks);
	// One husk death flash (renderer::death_flash_spawn carries the witness:
	// at the entity position, 2x the piece model radius, corona disabled).
	void on_death_light(const Vector3 &p_world_pos, float p_radius);
	// The in-flight light_move glows, diffed against the sim's live rows
	// (renderer::LightSpawner::sync_round_glows carries the witness). Rows:
	// world::RoundGlowRow from Simulation::fill_round_glows.
	void sync_round_glows(const std::vector<opennova::world::RoundGlowRow> &p_rows);
	// The bound data leg: the same sync over test-authored RoundGlowRow records.
	void sync_round_glow_records(const Array &p_rows);
	Ref<EffectLightReport> get_report();

	// The bound signal target: the MissionObjects container's membership
	// changing (the walk registry rebuilds).
	void _on_container_membership_changed(Node *p_node);

protected:
	static void _bind_methods();

private:
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
	// A live entity's BMS id into the interior-owner registry (a static's
	// tagged id, registered first, wins).
	void _register_entity(ObjectModel *p_node, int64_t p_owner_id);
	int64_t _spawn_light_at(const Ref<ModelLight> &p_info, const Vector3 &p_world_pos,
			int64_t p_owner_id, const BlinkOwner &p_blink_owner, bool p_spawner_is_building);
	BlinkOwner _blink_owner_at(const Vector3 &p_world_pos);
	int64_t _owner_id_for_bms(int p_bms_id);
	// A placed record's BUILDING identity: its items.def type, not its BMS
	// record family (engine: mission::placed_record_is_building).
	bool _record_is_building(int p_kind, int p_item_id) const;
	void _render_static_light_rows(const Vector3 &p_gain, Weather *p_weather, int p_time_ms);
	void _rebuild_static_light_rows(int64_t p_revision);
	void _ensure_model_registry(Node *p_container);
	void _rebuild_model_registry(const TypedArray<Node> &p_models);
	// The models the per-model pass walks: the world's MissionObjects
	// children, or the embedder's scene (set_scene); the registry kept in step.
	bool _walk_models();
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
	// The embedder's scene (set_scene): its models, environment and weather.
	bool scene_set_ = false;
	TypedArray<Node> scene_models_;
	ObjectID scene_environment_;
	ObjectID scene_weather_;
	Ref<MissionObjectPlacer> placer_provider_;
	// The static atlas rows' revision (the placer's draw-source revision: rows
	// appended, table reset, carve state); the rows are handed to the scene
	// only when it moves, and only the light SELECTION over them runs per
	// frame, as retail's per-batch select does.
	int64_t static_rows_revision_ = -1;
	Ref<LightScene> scene_;
	// The pool's spawners and entity lifecycle over scene_'s engine pool: the
	// LGHT walks, each entity's cached word, the round glows and the
	// interior-owner registry.
	std::unique_ptr<opennova::renderer::LightSpawner> spawner_;
	std::vector<opennova::mission::StaticEffectSource> static_sources_snapshot_;
	// This frame's corona quads for the post-particle overlay stage, from the
	// portable corona walk (engine: runtime/renderer/light_scene.h).
	std::vector<opennova::renderer::LightCoronaQuad> coronas_;
	// The weapon Inset pass's own walk this frame (render_inset_frame).
	std::vector<opennova::renderer::LightCoronaQuad> inset_coronas_;
	// Each water mirror's own walk this frame (render_mirror_coronas): [0] the
	// main scene's mirror, [1] the weapon Inset's.
	std::vector<opennova::renderer::LightCoronaQuad> mirror_coronas_[2];
	// The frame & 3 jitter phase: every scene walk advances it, as retail's
	// effect-world prologue does once per scene pass (scene_overlay.h
	// kInsetOverlayOrder). A mirror's subscene runs ahead of its host pass's
	// prologue, so the main scene's mirror walks at the phase before the main
	// walk's advance and the Inset's mirror at the main walk's.
	int corona_frame_ = 0;
	int mirror_corona_phase_[2] = {0, 0};
	// The mirrors' fog for their walks: the reflected subscene's dry block.
	Ref<EnvLightValues> mirror_corona_fog_;
	// The procedural corona texture "texlightcrn": the law (the 128x128
	// 0.4 - 0.45 d falloff, truncated, the transparent border) lives
	// portable in renderer::corona_texture_argb; this only wraps the bytes in
	// a texture, once per director.
	Ref<ImageTexture> corona_texture_;
	// The report census's camera box: a diagnostic of the pool, no retail
	// rule (the gameplay pass selects per drawn entity).
	static constexpr float kCensusRadius = 512.0f;
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
