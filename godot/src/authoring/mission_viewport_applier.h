#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/mission_people.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/mission_poses.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_shots.h>
#include <editor/preview/viewport_follow.h>

#include "authoring/ground_overlay_upload.h"
#include "authoring/terrain_patch_applier.h"
#include "authoring/preview_effects.h"
#include "authoring/preview_scene_layers.h"
#include "authoring/preview_sound_player.h"
#include "authoring/viewport_applier.h"
#include "env/mission_environment.h"
#include "env/slot_shadow.h"
#include "env/water.h"
#include "env/weather.h"
#include "lights/effect_light_director.h"
#include "mission/mission_object_placer.h"
#include "mission/mission_placement_run.h"
#include "mission/static_source_provider.h"
#include "object/object_model.h"
#include "simulation/husk_swap.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "world/scar_presenter.h"

namespace opennova::editor {
class MissionViewport;
class ProjectAssetSource;
}

namespace godot {

// A mission viewport's device work (ADR 0046 S14): a mission as the game draws it without a
// simulation, its world the layers' (PreviewSceneLayers over WorldScene's rows, S26: the
// environment over the mission's .env with the header's overrides and its clock, its weather from
// the mission start's settle, the sky, the sun, moon, glare and glint, the drops, the water, the
// terrain and its foliage, the environment cube, FrameFx), the entities placed by the game's placer
// (MissionObjectPlacer: the static populations, the individual models), all read through a
// ResourceRoot mounted over the project's files, and the camera where the viewport's orbit camera
// is. The frame's legs run in the game's order (godot/src/world/game_world_frame.cpp's frame-leg
// table, no simulation): the scene environment and the environment nodes, the terrain, the water,
// the foliage, the sun veil's feed, the lights, the static and the individual models' levels, the
// slot shadows, the particles, the drops, the overlay tail, the frame effects, the clear colour and
// the cube. The mission's marks are the canvas's, drawn over the picture by the viewport's portable
// half on the pixels this camera projects them to.
//
// It builds over the Shell's frames (S13 V6), a unit a step, in three layers: the environment and
// the terrain (the layers' units: the .env and its clock, the terrain's files a unit each, the
// mission's .til with the last, its build a tile a step; the sky and the water bound to them); the
// entities (the item table; each distinct graphic the entities name, its model parsed, its batches
// warmed where a placement batches it and its terrain shadow geometry resolved; the placement,
// MissionPlacementRun's units; the static shadows bound; the added entities lifted, eight a unit);
// then the pose. A Rebuild plans only the units of what differs from what it holds (each layer's
// key, the entities by row: their item, group and attributes), and one whose plan is the pose
// alone, or the pose after one unit of lifts over warm graphics, is made whole as it is taken.
// Nothing a unit replaces leaves the picture as the Rebuild is taken: the frame that takes it may
// still render the last whole scene (ViewportDevice::take), so what goes (the entities of a moved
// file, a terrain the mission no longer names) goes in a unit. A Rebuild that comes while the
// terrain loads or builds under a key that stands carries that work on. An entity's transform is
// moved in place (an Update: move_static_instance, or the individual model's node), its terrain
// shadow source following at the gesture's end; an entity added since the placement, or given
// another item, group or attributes, is lifted (an individual ObjectModel under "Lifted", built as
// the placement builds an entity's model: MissionObjectPlacer::build_entity_model, its terrain
// shadow source keyed apart); one given back what was placed shows the placed one again; one
// removed is hidden with its terrain shadow (shown again when it comes back, an undo); the lifted
// set folds back into a whole placement past 256 rows. A terrain whose files do not load or build
// is a layer with no terrain and a note, never a failed picture. The root notes every file it is
// asked for (StampedFiles), each layer the names its units read: a file the picture read that moved
// (the viewport's Rebuild) mounts the root afresh, and only the layers that read what moved are
// built again. Its scene state is its own (E13): the environment and the water hold their
// process-wide shader globals (set_globals_held), as does the retained statics' light atlas (each
// row's lighting lane, no lights), but while it publishes them (publish_scene_state, whenever
// another state was published last or its own moved: a new scene state each time its environment,
// its water or its time changes) and while it presents its frame; its frame's legs run only in a
// frame it renders (present), and the water's mirror pass is on only in such a frame (off from each
// frame's tick until its present).
//
// DI-31: the game's foliage, effects and lights over it, each a layer its options switch. The
// foliage is the game's FoliageDispatcher beside the terrain, configured from the terrain's foliage
// definitions as its build ends (their models and :fd textures read through the root, noted as the
// terrain layer's) and drawn each presented frame from the terrain's detail cells around the
// camera, as the game's foliage leg draws them. The effects are the scene the viewport's
// MissionEffects steps (each placed item's particle slot as the mission's start attaches it), drawn
// by the game's particle renderer (PreviewEffects, DI-14's helper) under the environment; the
// device draws single-sampled, as the game's view does and the renderer's passes need (DI-14's
// rule). The lights are the game's EffectLightDirector over the placer's statics, as the picture
// stands them (moved where their rows stand, a removed one's dropped: PictureSources), and the
// entities' individual models: every LGHT record spawned as the mission's start spawns it, each
// frame's selection written to the models and the statics' light atlas, the terrain's projected
// pass fed the same pool, the coronas drawn in the overlay tail; spawned again when the placement,
// a lift, a hide or a move's gesture end moves what stands. The FLICKER ring and the detail tier's
// sway read the weather's oscillator: the whole weather runs on the preview clock's game ticks (the
// mission's start settle first), its clock standing at the hour the options show.
//
// S23 C: the shadows as the game casts them, one layer: the terrain's static shadows (the placer's
// casters composed into its pages) and the entities' moving ground shadows, the game's render-slot
// device (SlotShadow) over the picture's casters (the placer flags a person's and a DynamicShadow
// item's model, as the game's presenter does), its captures on the clear's compositor and its drape
// over the terrain, planned in the frame's slot-shadow leg (after the lights, before the particles:
// game_world_frame.cpp's order) with the light director's pool and gain. With the layer off nothing
// plans and the drape hides with the terrain's pass.
//
// DI-36: the Listen. While the options listen, each channel the viewport's MissionListen binds
// plays its wave looping at its place (PreviewSoundLoops, DI-02's player), the SubViewport's camera
// the 3D listener, as the game's ambient channels play; heard while the picture is drawn (held
// paused once it has not been for PreviewSoundLoops::kHeldFrames frames).
class MissionViewportApplier final : public ViewportApplier {
public:
	explicit MissionViewportApplier(SubViewport &viewport);
	~MissionViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return build_ != nullptr; }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}
	bool surface_at(float x, float y, float point[3]) const override;
	bool surface_between(const double from[3], const double to[3], double point[3]) const override;
	bool ground_at(double x, double y, double &height) const override;
	// What the segment meets first (ADR 0046, the polish after its review): the faces of the placed
	// entities shown (a static's graphic's finest level through its instance's transform, an individual
	// model's meshes through its nodes'), the nearer first, before the terrain; the terrain; nothing.
	// Unknown until the placement stands.
	opennova::editor::ViewportRayHit ray_between(const double from[3], const double to[3]) const override;
	uint64_t scene_state() const override { return layers_->scene_state(); }
	void publish_scene_state() override;
	void present(double dt) override;

	// Its nodes and its placer, for the parity tests.
	Camera3D *camera() const { return camera_; }
	// S23 C: the entities' moving ground shadows, the game's render-slot device over the picture's casters.
	SlotShadow *slot_shadow() const { return slot_shadow_; }
	// The Shoot tool's (DI-23): the scars its shots left, drawn by the game's ScarPresenter (their effects play in
	// the items' effect scene, effects()).
	ScarPresenter *shot_scars() const { return shot_scars_; }
	Terrain *terrain() const { return terrain_; }
	MissionEnvironment *environment() const { return environment_; }
	Ref<MissionObjectPlacer> placer() const { return placer_; }
	// DI-31's: the foliage dispatcher, the effects drawn, the light director.
	FoliageDispatcher *foliage() const { return foliage_; }
	// DI-36's: the Listen's channels' player.
	const PreviewSoundLoops &listen() const { return *listen_; }
	PreviewEffects &effects() { return layers_->effects(); }
	Ref<EffectLightDirector> lights() const { return lights_; }
	// How the entities stand: placed by the last whole placement and shown, lifted since and shown,
	// hidden; how many whole placements ran and what the last one cost (microseconds).
	int placed_count() const;
	int lifted_count() const;
	int hidden_count() const;
	int placements() const { return placements_; }
	int64_t last_place_us() const { return last_place_us_; }
	// The placer's key of the entity whose row is `row` (0: none, or lifted).
	int key_of(opennova::editor::NodeId row) const;

	// What a pick casts against for one placed entity: its faces in its own space (three vertices a
	// triangle) and their box.
	struct PickShape {
		std::vector<opennova::editor::PreviewVec3> faces; // three points a triangle
		opennova::editor::PreviewVec3 low, high; // their box
		bool any = false;
	};

private:
	// The placer's statics as the picture stands them (DI-31), the light director's provider: each static's
	// effect source where its rows are drawn now (a move rewrites the rows alone), a removed (hidden) one's
	// source without its model, so no light of it spawns; the rest the placer's own.
	class PictureSources final : public StaticSourceProvider {
	public:
		void bind(const Ref<MissionObjectPlacer> &placer) { placer_ = placer; }
		std::vector<opennova::mission::StaticEffectSource> static_item_effect_sources() override;
		std::vector<opennova::mission::StaticLightDrawSource> static_light_draw_sources() override;
		uint64_t static_light_draw_source_revision() override;
		Ref<ItemDatabase> static_source_item_db() override;
		Ref<ObjectData> static_source_object_data(uint64_t asset_id) const override;

	private:
		Ref<MissionObjectPlacer> placer_;
	};

	// One unit of a build, in the order they run.
	struct Unit {
		enum class Kind : uint8_t {
			DropEntities, // the placement and the lifted models of a moved file dropped, the placer
						  // with them
			Layers, // the environment's and the terrain's units (PreviewSceneLayers), one a step
			Items, // items.def, through a placer made over the root
			Model, // a graphic's model parsed, its static batches warmed, its shadow geometry
				   // resolved
			Place, // one unit of the placement run
			Shadows, // the placement's static shadows bound to the terrain
			Lift, // up to kLiftPerUnit added entities lifted
			Pose, // the entities moved, shown or hidden; the options, the time, the camera
		};
		Kind kind = Kind::Pose;
		String graphic; // Model
		std::vector<int64_t> items; // Model: the items the entities draw it for
		size_t first = 0; // Lift: the first of its rows among the adds
	};
	// What one entity is to the device: placed (a static's key, or an individual model) or lifted,
	// shown or hidden, and the item and stamp it stands for.
	struct Placed {
		int64_t item = 0;
		int group = 0;
		uint32_t attributes = 0;
		uint32_t stamp = 0;
		// The placer's key: a placed entity's ordinal at the placement, from 1; a lifted one's past
		// every placed key (its terrain shadow source's bms id).
		int key = 0;
		int kind = -1, index = -1; // its terrain shadow source's key: a placed one's pool and index, a lifted one's kind and key
		uint64_t model = 0; // its ObjectModel's instance id: a placed individual model, or a lifted one
		bool lifted = false;
		bool hidden = false;
		// The spawn pose its model was last given (DI-38): the model it went to and the pose's stamp.
		uint64_t posed_model = 0;
		uint32_t pose_stamp = 0;
	};
	static bool places_alike_(const Placed &placed, const opennova::editor::MissionEntityMark &entity) {
		return placed.item == entity.item && placed.group == entity.group && placed.attributes == entity.attributes;
	}
	// A build in flight: whether it places whole, and its units.
	struct Build {
		bool place = false;
		std::vector<Unit> units;
		size_t next = 0;
		std::vector<opennova::editor::NodeId> adds; // the rows the Lift units lift
		Ref<MissionPlacementRun> place_run; // the placement in flight (the Place units step it)
	};

	static constexpr size_t kLiftPerUnit = 8;
	static constexpr size_t kLiftedMost = 256;

	// The layers' root mounted over the session's files (PreviewSceneLayers::mount), a move no
	// layer read the entities': the entities of a moved file dropped by the build's first unit
	// (drop_pending_), the waves the Listen decoded read again.
	void mount_(const opennova::editor::SessionView &view);
	// The entities' units after the layers' (whose own plan carries a terrain load or build in
	// flight, m6).
	void plan_(Build &build, const opennova::editor::MissionViewport &mission);
	std::vector<Unit> model_units_(
			const std::vector<const opennova::editor::MissionEntityMark *> &entities);
	// One of the entities' units run, its reads the entities': false when the build failed
	// (`failure` says why).
	bool run_(Build &build, const Unit &unit, const opennova::editor::ViewportModel &model,
			std::string &failure);
	void run_items_();
	void run_model_(const Unit &unit);
	// The placement as a run (MissionPlacementRun): begun at the first Place unit, one unit of it
	// stepped per Place unit, the run's units joined to the build's as the run counts them.
	void run_place_begin_(const opennova::editor::MissionScene &scene, Build &build);
	void run_place_step_(Build &build);
	void run_lift_(const opennova::editor::MissionScene &scene, Build &build, size_t first);
	// One entity lifted as an individual model of its own under `key` (null: its item draws nothing).
	ObjectModel *lift_(const opennova::editor::MissionEntityMark &entity, int key);
	// A lifted model let go: its terrain shadow source made inactive (the row stays until the next
	// placement), the node freed.
	void free_lifted_(const Placed &placed);
	// What the device shows of an entity, retired (hidden: a static's rows, a model's node, its
	// terrain shadow) or shown.
	void show_(Placed &placed, bool shown);
	// The placement and the lifted models dropped, the placer with them.
	void drop_entities_();
	// The state over the picture that stands: the entities moved, shown or hidden; the layers'
	// visibility, the time of day, the camera.
	void apply_state_(const opennova::editor::ViewportModel &model);
	// A posed person's model stands where its spawn stands it (its pose's lift over the record).
	void move_entities_(const opennova::editor::MissionScene &scene, const opennova::editor::MissionPoses &poses);
	// Each person's model in the pose the game spawns it in and where the spawn stands it (DI-38, the
	// viewport's MissionPoses), where its pose or its model is another than it was given.
	void pose_people_(const opennova::editor::MissionScene &scene, const opennova::editor::MissionPoses &poses,
			const opennova::editor::MissionPeople &people);
	// A person's model posed by its body (a clip at its playhead, or two blended).
	void pose_body_(ObjectModel &model, const opennova::world::InfantryBodyPose &body);
	// The people as they play their clips now (MissionViewport::people), posed again whenever they played on.
	void play_people_(const opennova::editor::MissionViewport &mission);
	uint64_t people_serial_ = UINT64_MAX; // the viewport's people's serial last posed
	// The moved entities' terrain shadow sources moved too, once no gesture is open.
	void flush_shadows_(const opennova::editor::MissionScene &scene);
	// The lights spawned again over the entities as they stand (DI-31): the director's scene the shown
	// individual models, its statics the placer's as the picture stands them; none while the layer is off
	// (the statics' atlas lanes still published).
	void relight_();
	// What the last presented frame drew of the foliage, the lights, the effects and the
	// environment (the report's `drawn`).
	opennova::io::JsonValue drawn_json_() const;
	void place_camera_(const opennova::editor::ViewportModel &model);
	String graphic_of_(int64_t item);
	Transform3D transform_of_(const opennova::editor::MissionEntityMark &entity) const;
	static ObjectModel *model_of_(const Placed &placed);
	// What a pick casts against for the placed entity of row `row`, and its transform in the picture's
	// space now; null for none (hidden, a static not warm, a model with no meshes).
	const PickShape *pick_shape_(opennova::editor::NodeId row, const Placed &placed, Transform3D &xform) const;

	// The Shoot tool's scars (DI-23), presented where the shots' run moved, cleared with no shot.
	void apply_shots_(const opennova::editor::MissionViewport &mission);
	// S23 C: the husks of the items the shots destroyed, at the clock's tick (the run's deaths
	// through editor::mission_husk_frame): swapped in through the game's destruction presenter's
	// own swap (simulation/husk_swap: a placed individual model's husk built under it, its own
	// parts hidden; a retained static carved out of its batches, the husk grafted at its placed
	// transform), the destroy fade's registers and the sections the pieces left written each frame;
	// let go (the item shown again) where the clock steps back past the swap or the shots clear.
	struct Husk {
		std::string husk;
		HuskGraft graft; // what the swap changed (simulation/husk_swap), the husk model with it
		uint64_t intact = 0; // the intact individual model it hangs under (0: a static's graft)
		std::map<std::string, int64_t> ctrl;
		uint32_t sections = 0;
	};
	void apply_husks_(const opennova::editor::MissionViewport &mission, int32_t tick);
	void unhusk_(Husk &husk);
	std::unordered_map<opennova::editor::NodeId, Husk> husks_;
	ScarPresenter *shot_scars_ = nullptr;
	uint64_t shot_scars_shown_ = UINT64_MAX;

	// The world the picture stands in (the scene stack, the environment and terrain layers, the
	// camera, the effects' renderer, the frame effects), and its nodes the device's legs read.
	std::unique_ptr<PreviewSceneLayers> layers_;
	MissionEnvironment *environment_ = nullptr;
	Water *water_ = nullptr;
	Terrain *terrain_ = nullptr;
	uint64_t terrain_id_ = 0;
	// S25 (the heights): the sculpt's patches taken into the terrain built (terrain_patch_applier.h).
	TerrainPatchApplier terrain_patches_;
	// The entities' moving ground shadows (S23 C): the game's SlotShadow, its silhouette captures on the clear's
	// compositor and its drape over the terrain, run in the frame's slot-shadow leg while the shadows show.
	SlotShadow *slot_shadow_ = nullptr;
	Camera3D *camera_ = nullptr;
	Node3D *objects_ = nullptr;
	Node3D *lifted_root_ = nullptr;
	Ref<MissionObjectPlacer> placer_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	// Whether the entities stand placed.
	bool placed_ = false;
	std::unordered_map<opennova::editor::NodeId, Placed> entities_;
	// The placed records of rows lifted since (another item, group or attributes): shown again when
	// the row is given back what was placed (an undo).
	std::unordered_map<opennova::editor::NodeId, Placed> origins_;
	std::vector<opennova::editor::NodeId> lifted_; // the rows lifted since the placement
	std::map<std::string, bool> warm_; // the graphics warmed (false: its model did not load)
	int64_t warm_epoch_ = -1; // the cache epoch they were warmed under
	std::vector<opennova::editor::NodeId> shadow_pending_; // moved, their shadow sources not yet
	bool drop_pending_ = false; // the entities' files moved: the next build's first unit drops them
	int next_key_ = 0; // the last placer key given
	int placements_ = 0;
	int64_t last_place_us_ = 0;
	int64_t place_started_us_ = 0;
	int place_units_planned_ = 0;
	std::unique_ptr<Build> build_;
	// DI-31: the foliage beside the terrain; the effects (the project's files mounted for their
	// graphics since the last Rebuild); the weather whose oscillator the lights and the foliage
	// read; the light director and its statics; the layers shown; the clock's milliseconds the
	// frame draws at.
	FoliageDispatcher *foliage_ = nullptr;
	PreviewEffects *effects_ = nullptr;
	bool effects_mounted_ = false;
	Weather *weather_ = nullptr;
	// The retained statics' light atlas (opennova_static_point_light_rows, a scene-state global) is the
	// director's scene's: a static row reads its lighting lane there, as no MultiMesh instance carries one,
	// so a picture without it draws every static as if the sun were blocked.
	Ref<EffectLightDirector> lights_;
	PictureSources sources_;
	bool relight_pending_ = false;
	bool lights_dirty_ = true; // the pool or its scene moved since the last light pass
	uint64_t lit_rows_revision_ = UINT64_MAX; // the statics' rows the last light pass read
	bool shown_foliage_ = true, shown_effects_ = true, shown_lights_ = true;
	int64_t clock_ms_ = 0;
	// DI-36: the Listen's channels and the project's root its waves are read under. apply_listen_ binds the channels as
	// the viewport's mix holds them now.
	void apply_listen_(const opennova::editor::MissionViewport &mission);
	std::unique_ptr<PreviewSoundLoops> listen_;
	std::string project_root_;
	// The frames presented, and the last one's foliage, light and effect legs (microseconds).
	uint64_t presented_ = 0;
	int64_t leg_us_[3] = { 0, 0, 0 };
	// The pick shapes kept: a static's by its graphic, an individual model's by its node (dropped with
	// the entities), and the last ray's answer while the picture stands (a frame's hover and hint ask
	// alike).
	mutable std::unordered_map<std::string, PickShape> static_shapes_;
	mutable std::unordered_map<uint64_t, PickShape> model_shapes_;
	// Each row's shape as last found, while its entity is placed alike (its item, its key, its model):
	// a ray asks every entity, its graphic named once.
	struct RowShape {
		int64_t item = 0;
		int key = 0;
		uint64_t model = 0;
		const PickShape *shape = nullptr;
	};
	mutable std::unordered_map<opennova::editor::NodeId, RowShape> row_shapes_;
	void drop_shapes_() {
		static_shapes_.clear();
		model_shapes_.clear();
		row_shapes_.clear();
	}
	mutable bool ray_kept_ = false;
	mutable double ray_from_[3] = { 0.0, 0.0, 0.0 }, ray_to_[3] = { 0.0, 0.0, 0.0 };
	mutable opennova::editor::ViewportRayHit ray_hit_;
	// The picture's records moved, placed, lifted or hidden: the kept ray goes.
	void picture_moved_() { ray_kept_ = false; }
	// The time applied (-2: none yet; -1 the mission's own; else the option's hour); the layers shown.
	double applied_time_ = -2.0;
	bool shown_water_ = true, shown_shadows_ = true;
	uint64_t overlay_serial_ = 0; // the mission's ground overlay given the terrain (DI-29)
	GroundOverlayUpload overlay_upload_; // its upload, the rows a patch changed alone (S25)
};

} // namespace godot
