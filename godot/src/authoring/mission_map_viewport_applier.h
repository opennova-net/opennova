#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
// Over the pass, the models' wireframes seen from above (the viewport's outline_lines, mission metres): one canvas item
// of thin lines, made again when they move and carried by the camera's transform when only the camera does. It answers
// the ground under a point (ground_at, the terrain's heights) for a drag's stick. It reads none of the process-wide
// render state a mission publishes.
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
	// The wireframes' edges drawn, and how many chunks of them it has uploaded in all (a drag of one record uploads its
	// own chunks alone).
	int outline_edges() const { return outline_edges_; }
	int outline_chunks_uploaded() const { return chunks_uploaded_; }
	// S25: the map's background picture the overlay holds (null: the colour map), and how many it has uploaded.
	Ref<ImageTexture> background_picture() const { return picture_; }
	int background_uploads() const { return picture_uploads_; }
	// Texels converted to the texture's bytes since the applier began (S25: a stroke's patch converts its rects alone).
	int background_converted() const { return picture_converted_; }
	// The placed tiles drawn over the map (S25), and the times they were drawn again.
	int tiles_drawn() const { return tiles_drawn_; }
	int tiles_redrawn() const { return tiles_redrawn_; }

private:
	// The terrain and its water mask given the overlay; the grid's origin.
	void install_terrain_();
	// The pass compiled at the map's camera and drawn, where what it shows moved.
	void draw_(const opennova::editor::MissionMapViewport &map);
	// The wireframes drawn again where they moved; their transform the camera's.
	void draw_outlines_(const opennova::editor::MissionMapViewport &map);
	// S25: the map's background (MissionMapTerrainView) given the overlay, once a serial.
	void apply_picture_(const opennova::editor::MissionMapViewport &map);
	// S25: the placed tiles drawn again where they moved (or the tile set did); their transform the camera's.
	void draw_tiles_(const opennova::editor::MissionMapViewport &map);
	// The camera's transform: a mission point (metres, x east, y north) to the picture's pixel.
	Transform2D map_transform_(const opennova::editor::MissionMapViewport &map) const;

	uint64_t overlay_id_ = 0;
	uint64_t canvas_id_ = 0;
	RID outline_item_; // the wireframes', under the Outlines node's own item
	uint64_t outline_serial_ = UINT64_MAX; // the viewport's outline serial drawn
	std::vector<RID> chunk_items_; // a canvas item a chunk of edges, under outline_item_
	std::vector<uint64_t> chunk_drawn_; // each chunk's serial drawn
	int chunks_uploaded_ = 0;
	int outline_edges_ = 0;
	// S25: the placed tiles over the map, MED's 2D view's way: a canvas item of their quads under the wireframes, the
	// tile set (the terrain's tilestrip as it loaded) with each texel's alpha 0 or opaque, sampled nearest.
	RID tiles_item_;
	Ref<ImageTexture> tile_set_;
	uint64_t tile_set_from_ = 0; // the terrain data it was made from
	int tile_set_width_ = 0, tile_set_height_ = 0;
	uint64_t tiles_serial_ = UINT64_MAX; // the viewport's tiles serial drawn
	int tiles_drawn_ = 0, tiles_redrawn_ = 0;
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
	// S25: the background picture uploaded, and the view's serial it was made at (UINT64_MAX: none yet, or the overlay's
	// slot given back to a colour map since).
	Ref<ImageTexture> picture_;
	uint64_t picture_serial_ = UINT64_MAX;
	int picture_uploads_ = 0;
	// The picture's RGBA bytes as last uploaded: of the view's whole make `picture_whole_`, its first
	// `picture_rects_` patched rects converted into them (a patch converts its rects alone).
	PackedByteArray picture_bytes_;
	uint64_t picture_whole_ = UINT64_MAX;
	size_t picture_rects_ = 0;
	int picture_converted_ = 0; // texels converted, a test's measure
	bool pass_visible_ = false;
	int pass_terrain_tris_ = 0, pass_sprites_ = 0, pass_labels_ = 0;
};

} // namespace godot
