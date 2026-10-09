#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <editor/preview/mission_map.h>
#include <runtime/hud/hud_map_view.h>

#include "authoring/viewport_applier.h"
#include "hud/hud_map_pass_renderer.h"
#include "hud/hud_overlay.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain_data.h"

namespace opennova {
class StampedFiles;
}

namespace godot {

// A mission's 2D map's device work (ADR 0046 S23 C; editor/preview/mission_map.h): the game's commander map pass drawn
// as the game's CMAP window draws it, over the project's files. The runtime's own HudOverlay holds the map's state (its
// layout from the project's hudpos.def, the unauthored one where it has none, its fonts and its icon strip; the
// mission's terrain sampled into the minimap's sector layout with its colour map and the depthspin water mask at the
// mission's water plane; the grid's origin) and compiles the pass (hud::HudFrameCompiler::compile_command_map, the CMAP
// view centred on the map camera's centre at its zoom, the payload rect the whole picture); the shared map renderer
// (hud/hud_map_pass_renderer, MapViewWindow's) draws it into the SubViewport's canvas. The terrain loads a file a unit
// over the Shell's frames (TerrainData::begin_load_from_resource_root / load_step), the last picture drawn meanwhile.
// It answers the ground under a point (ground_at, the terrain's heights) for a drag's stick. It reads none of the
// process-wide render state a mission publishes.
class MissionMapViewportApplier final : public ViewportApplier {
public:
	explicit MissionMapViewportApplier(SubViewport &viewport);
	~MissionMapViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return loading_.is_valid(); }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int width, int height) override;
	bool ground_at(double x, double y, double &height) const override;
	bool reads_scene_state() const override { return false; }

	// What it holds, for the GUT device test: the overlay holding the map's state, the terrain read, what the last
	// compiled pass drew (its terrain triangles, its sprites, its labels).
	HudOverlay *overlay() const;
	Ref<TerrainData> terrain_data() const { return terrain_data_; }
	int pass_terrain_tris() const { return pass_terrain_tris_; }
	int pass_sprites() const { return pass_sprites_; }
	int pass_labels() const { return pass_labels_; }
	bool pass_visible() const { return pass_visible_; }

private:
	// The terrain and its water mask given the overlay; the grid's origin.
	void install_terrain_();
	// The pass compiled at the map's camera and drawn, where what it shows moved.
	void draw_(const opennova::editor::MissionMapViewport &map);

	uint64_t overlay_id_ = 0;
	uint64_t canvas_id_ = 0;
	Ref<ResourceRoot> root_;
	std::shared_ptr<opennova::StampedFiles> stamped_;
	Ref<TerrainData> loading_; // the terrain a build loads
	Ref<TerrainData> terrain_data_;
	opennova::editor::MissionMapGround ground_; // what the terrain was installed with
	opennova::hud::CommandMapView cmap_;
	HudMapPassRenderer renderer_;
	int width_ = 1, height_ = 1;
	// What the drawn pass was compiled at: redrawn where any of it moved.
	opennova::editor::MissionMapCamera drawn_camera_;
	bool drawn_grid_ = true, drawn_text_ = true;
	int drawn_width_ = 0, drawn_height_ = 0;
	bool dirty_ = true;
	bool pass_visible_ = false;
	int pass_terrain_tris_ = 0, pass_sprites_ = 0, pass_labels_ = 0;
};

} // namespace godot
