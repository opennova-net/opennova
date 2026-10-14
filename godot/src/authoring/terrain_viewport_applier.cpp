#include "authoring/terrain_viewport_applier.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <editor/preview/preview_clock.h>
#include <editor/preview/terrain_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>

#include "env/water.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "terrain/terrain_tile_info.h"

namespace godot {

namespace {

using opennova::editor::TerrainViewport;

const TerrainViewport &terrain_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const TerrainViewport &>(model);
}

} // namespace

TerrainViewportApplier::TerrainViewportApplier(SubViewport &viewport) :
		layers_(std::make_unique<PreviewSceneLayers>(viewport, "TerrainPreview", false)) {}

TerrainViewportApplier::~TerrainViewportApplier() = default;

// --- the build -----------------------------------------------------------------------------------

void TerrainViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &) {
	building_ = false;
	const TerrainViewport &model = terrain_of(viewport);
	if (model.view_status() != opennova::editor::TerrainViewStatus::Ready || !view.findings.assets) {
		clear();
		return;
	}
	// What moved that neither layer read is the terrain's (an edit of its document moves its .trn).
	layers_->mount(view, PreviewSceneLayers::kTerrain);
	PreviewSceneLayers::Source source;
	source.header = model.header();
	source.mission = model.mission_name();
	// The mission's .env, which its terrain's load reads after the .trn (D-TERRAIN-18); none with
	// no mission, whose clock is the engine's own.
	source.environment = model.header().environment.empty() ? std::string()
															: model.header().environment + ".env";
	source.clock = model.mission() != nullptr;
	const PreviewSceneLayers::Plan plan = layers_->plan(source);
	// The maps, tiles and heights a new load reads hold every paint's, tile edit's and sculpt's
	// patch the viewport made so far (S25).
	if (plan.loads) {
		terrain_patches_.maps_built(model.maps_built_serial());
		terrain_patches_.built(model.heights_built_serial());
	}
	// The pose alone: made whole as it is taken.
	if (!layers_->building()) {
		apply_state_(model);
		return;
	}
	building_ = true;
}

ApplierStep TerrainViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &) {
	if (layers_->step())
		return ApplierStep::More;
	// The options, the overlay and the camera after the layers' last unit.
	apply_state_(terrain_of(viewport));
	building_ = false;
	return ApplierStep::Built;
}

opennova::editor::OperationProgress TerrainViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!building_)
		return progress;
	progress.done = layers_->units_done();
	progress.total = layers_->units_planned() + 1;
	progress.label = layers_->building() ? layers_->step_label() : "pose";
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void TerrainViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	Terrain *terrain = layers_->nodes().terrain;
	const Ref<TerrainData> &data = layers_->terrain_data();
	const bool standing =
			layers_->terrain_built() && !building_ && data.is_valid() && !terrain->is_building();
	// S25 (the heights): a stroke's regions and a landing's tiles into the terrain built, never building it again; one
	// it cannot take has the next follow build it again (the patches it took are reported, the rest kept).
	if (standing &&
			!terrain_patches_.apply(terrain_of(viewport).heights_patches(), **data, *terrain)) {
		UtilityFunctions::push_warning("The terrain view's terrain did not take a sculpt's patch: it builds again.");
	}
	// S25 (the maps and the tiles): a paint's regions and the placed tiles into the terrain built, in place, each
	// file's read taking the stamp it was patched at (so its move does not build the terrain again); one not taken is
	// reported and the picture made again.
	if (standing) {
		FoliageDispatcher *foliage = layers_->nodes().foliage;
		terrain_patches_.apply_maps(terrain_of(viewport).maps_patches(), *terrain,
				layers_->foliage_configured() ? foliage : nullptr,
				[this](const std::string &name, uint64_t stamp) { layers_->restamp(name, stamp); });
		// The foliage keeps candidates off the tiles the terrain draws (a new override where it held none).
		if (layers_->foliage_configured() &&
				foliage->get_tile_info() != terrain->get_tile_info_override())
			foliage->set_tile_info(terrain->get_tile_info_override());
	}
	apply_state_(terrain_of(viewport));
}

void TerrainViewportApplier::apply_state_(const TerrainViewport &model) {
	const opennova::editor::TerrainViewportOptions &options = model.options();
	PreviewSceneLayers::Shown shown;
	shown.water = options.water;
	shown.foliage = options.foliage;
	layers_->set_shown(shown);
	// The ground overlay (DI-29): the picture the viewport made of what the game reads at each point, laid over the
	// terrain from above (presentation x east, z the negated mission y), past it the one value the game reads all
	// round where there is one (the mission device's leg).
	// S25: a stroke's patch of it the rows it changed alone, the terrain's texture taken in place (GroundOverlayUpload).
	if (model.overlay_serial() != overlay_serial_) {
		overlay_serial_ = model.overlay_serial();
		overlay_upload_.apply(*layers_->nodes().terrain, model.overlay());
	}
	layers_->place_camera(model.camera());
}

void TerrainViewportApplier::clear() {
	building_ = false;
	layers_->clear();
	overlay_serial_ = UINT64_MAX;
	overlay_upload_.reset();
	layers_->nodes().terrain->clear_ground_overlay();
}

void TerrainViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	layers_->report(report);
	report.heights_taken = terrain_patches_.taken();
	report.heights_failed = terrain_patches_.take_failure();
	report.drawn = drawn_json_();
	report.maps_taken = terrain_patches_.maps_taken();
	report.maps_refused = terrain_patches_.take_maps_refused();
	if (building_)
		return;
	apply_state_(terrain_of(viewport));
}

opennova::io::JsonValue TerrainViewportApplier::drawn_json_() const {
	using opennova::io::json_number;
	using opennova::io::JsonValue;
	JsonValue out = JsonValue::make_object();
	out.set("frames", json_number(double(presented_)));
	// The foliage the last presented frame drew (the mission device's words).
	out.set("foliage", layers_->foliage_json());
	// The placed tiles the terrain draws: the tiles the load took (the mission's .til, else the terrain's own), and
	// with none the terrain's own its data reads.
	Ref<TerrainTileInfo> tiles = layers_->nodes().terrain->get_tile_info_override();
	bool own = layers_->tiles_read().terrain_own;
	if (tiles.is_null() && layers_->terrain_data().is_valid()) {
		tiles = layers_->terrain_data()->get_tileinfo_resource();
		own = true;
	}
	JsonValue placed = JsonValue::make_object();
	placed.set("count", json_number(double(tiles.is_valid() ? tiles->get_entry_count() : 0)));
	placed.set("from", opennova::io::json_string(tiles.is_null() ? "none" : own ? "terrain" : "mission"));
	out.set("tiles", std::move(placed));
	// S25: the ground overlay's last upload (its rows and its cost: the painting's frame cost).
	out.set("overlay", overlay_upload_.to_json());
	return out;
}

void TerrainViewportApplier::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &clock) {
	clock_ms_ = int64_t(clock.ms());
	// A paint's colour map, once its strokes pause, into the texture the foliage's tint reads (S25).
	if (layers_->terrain_built())
		terrain_patches_.settle_colour(
				*layers_->nodes().terrain, Time::get_singleton()->get_ticks_msec());
	// No mirror pass unless this frame presents the picture.
	layers_->nodes().water->set_mirror_enabled(false);
	// The weather on the preview clock's game ticks.
	layers_->follow_clock(clock.ticks());
}

// --- the frame -----------------------------------------------------------------------------------

void TerrainViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs.
	if (building_ && !layers_->environment_built())
		return;
	// In the game's leg order (game_world_frame.cpp).
	layers_->begin_frame();
	layers_->present_environment(dt);
	layers_->present_ground(dt, clock_ms_);
	layers_->present_sun_veil();
	layers_->present_particles(clock_ms_);
	layers_->present_drops();
	layers_->present_overlay(nullptr);
	layers_->present_frame_effects();
	layers_->present_clear_and_cube();
	++presented_;
	layers_->end_frame();
}

} // namespace godot
