#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <base/io/json.h>
#include <editor/preview/viewport_follow.h>

#include "authoring/ground_overlay_upload.h"
#include "authoring/preview_scene_layers.h"
#include "authoring/terrain_patch_applier.h"
#include "authoring/viewport_applier.h"

namespace opennova::editor {
class TerrainViewport;
} // namespace opennova::editor

namespace godot {

class FoliageDispatcher;
class MissionEnvironment;
class Terrain;
class Water;

// A terrain viewport's device work (ADR 0046 DI-30b): a mission's world as the game draws it with
// no simulation (PreviewSceneLayers over WorldScene's rows, S26), the ground the subject: the
// terrain (Terrain over TerrainData, its .trn as the game would read it were it saved now, its
// tiles the mission's <mission>.til or, with none, the terrain's own polytrn_tileinfo, which
// TerrainData falls back on as the game does), its foliage (configured from the terrain's
// definitions once its build ends, drawn from its detail cells around the camera), the water bound
// to it, and the sky, the bodies, the weather and the light of the environment of the mission the
// viewport draws it under (its .env over the .trn and overcast.def with the header's overrides, the
// clock at its start time; with no mission the engine's own, as a mission with no .env starts), all
// read through a ResourceRoot mounted over the project's files, and the camera where the viewport's
// camera is. The viewport's ground overlay (DI-29) is the terrain's tint
// (Terrain::set_ground_overlay), an Update.
//
// It builds over the Shell's frames in the layers' two layers, each rebuilt only where its key or a
// file it read moved; an edit of the terrain's document moves its file, so the terrain layer is
// built again. The weather runs on the preview clock's game ticks from the mission start's settle,
// its clock standing at the start time. Its scene state is its own (E13).
class TerrainViewportApplier final : public ViewportApplier {
public:
	explicit TerrainViewportApplier(SubViewport &viewport);
	~TerrainViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return building_; }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}
	bool surface_between(const double from[3], const double to[3], double point[3]) const override {
		return layers_->surface_between(from, to, point);
	}
	bool ground_at(double x, double y, double &height) const override {
		return layers_->ground_at(x, y, height);
	}
	uint64_t scene_state() const override { return layers_->scene_state(); }
	void publish_scene_state() override { layers_->publish_scene_state(); }
	void present(double dt) override;

	// Its nodes, for the device tests.
	Camera3D *camera() const { return layers_->camera(); }
	MissionEnvironment *environment() const { return layers_->nodes().environment; }
	Terrain *terrain() const { return layers_->nodes().terrain; }
	Water *water() const { return layers_->nodes().water; }
	FoliageDispatcher *foliage() const { return layers_->nodes().foliage; }
	// How many builds of each layer it ran, and how many frames it presented.
	int environment_builds() const { return layers_->environment_builds(); }
	int terrain_builds() const { return layers_->terrain_builds(); }
	int64_t presented() const { return presented_; }

private:
	void apply_state_(const opennova::editor::TerrainViewport &model);
	opennova::io::JsonValue drawn_json_() const;

	std::unique_ptr<PreviewSceneLayers> layers_;
	bool building_ = false; // the layers' units run, the pose after them
	// S25: the heights', the maps' and the tiles' patches taken in place (terrain_patch_applier.h), as the mission
	// view's.
	TerrainPatchApplier terrain_patches_;
	uint64_t overlay_serial_ = UINT64_MAX; // the viewport's overlay as last given the terrain
	GroundOverlayUpload overlay_upload_; // its upload, the rows a patch changed alone (S25)
	int64_t clock_ms_ = 0;
	int64_t presented_ = 0;
};

} // namespace godot
