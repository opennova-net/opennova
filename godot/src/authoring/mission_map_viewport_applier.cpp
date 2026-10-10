#include "authoring/mission_map_viewport_applier.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <algorithm>
#include <cmath>

#include <base/vfs/file_stamps.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <runtime/hud/hud_minimap.h>

#include "hud/hud_pos.h"
#include "mission/mission_object_placer.h"
#include "render/d3d9_raster_device.h"
#include "util/string_convert.h"

namespace godot {

namespace {

template <typename T>
T *node(uint64_t id) {
	return id ? Object::cast_to<T>(ObjectDB::get_instance(id)) : nullptr;
}

const opennova::editor::MissionMapViewport &map_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::MissionMapViewport &>(model);
}

// What the picture shows under a terrain: the CMAP's payload clear stands in for it until the pass draws.
const Color kUnderMap(0.0f, 0.0f, 0.0f, 1.0f);

} // namespace

MissionMapViewportApplier::MissionMapViewportApplier(SubViewport &viewport) {
	viewport.set_disable_3d(true);
	// The SubViewport's own size until the device sizes it (a size it already has is never given again).
	resize(viewport.get_size().x, viewport.get_size().y);
	ColorRect *under = memnew(ColorRect);
	under->set_name("UnderMap");
	under->set_color(kUnderMap);
	under->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	under->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport.add_child(under);
	// The overlay holds the map's state and compiles the pass; it draws no HUD of its own here.
	HudOverlay *overlay = memnew(HudOverlay);
	overlay->set_name("MapState");
	overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	overlay->set_visible(false);
	viewport.add_child(overlay);
	overlay_id_ = overlay->get_instance_id();
	// The canvas the pass's items hang under (the map renderer's parent item).
	Control *canvas = memnew(Control);
	canvas->set_name("Map");
	canvas->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	canvas->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport.add_child(canvas);
	canvas_id_ = canvas->get_instance_id();
	// The wireframes over the pass, in a canvas item of their own under a node of their own (a redraw of the node
	// clears the node's item, never a child item).
	Control *outlines = memnew(Control);
	outlines->set_name("Outlines");
	outlines->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	outlines->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport.add_child(outlines);
	RenderingServer *rs = RenderingServer::get_singleton();
	outline_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(outline_item_, outlines->get_canvas_item());
	root_.instantiate();
	cmap_.on_load();
}

MissionMapViewportApplier::~MissionMapViewportApplier() {
	renderer_.release();
	for (const RID &chunk : chunk_items_) RenderingServer::get_singleton()->free_rid(chunk);
	if (outline_item_.is_valid()) RenderingServer::get_singleton()->free_rid(outline_item_);
}

HudOverlay *MissionMapViewportApplier::overlay() const {
	return node<HudOverlay>(overlay_id_);
}

void MissionMapViewportApplier::rebuild(const opennova::editor::ViewportModel &model,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &) {
	clear();
	HudOverlay *hud = overlay();
	if (!hud || !view.findings.assets) return;
	const opennova::editor::MissionMapViewport &map = map_of(model);
	// The project's files, the open documents standing in for theirs, each read noted with its stamp.
	stamped_ = std::make_shared<opennova::StampedFiles>(view.findings.assets);
	root_->mount_files(stamped_);
	// The HUD's layout the map draws through (its fonts, its icon strip, its colours): the project's hudpos.def, else
	// the unauthored layout the game carries on over.
	Ref<HudPos> layout;
	layout.instantiate();
	if (root_->has_file("hudpos.def")) layout->load_from_resource_root(root_, "hudpos.def");
	hud->configure(layout, root_);
	// The terrain, a file a unit (the build's units).
	const std::string &terrain = map.scene().header().terrain;
	const String trn = opennova::to_gd(terrain) + ".trn";
	if (!terrain.empty() && root_->has_file(trn)) {
		loading_.instantiate();
		loading_->set_mission_tile_set(opennova::to_gd(map.scene().header().tile_set));
		loading_->set_mission_environment(opennova::to_gd(map.scene().header().environment));
		if (loading_->begin_load_from_resource_root(root_, trn) != OK) loading_.unref();
	}
	ground_ = map.ground();
	dirty_ = true;
}

ApplierStep MissionMapViewportApplier::step(const opennova::editor::ViewportModel &model,
		const opennova::editor::PreviewClock &, std::string &) {
	if (loading_.is_null()) return ApplierStep::Built;
	const TerrainData::LoadStep result = loading_->load_step();
	if (result == TerrainData::LOAD_STEP_MORE) return ApplierStep::More;
	// A terrain that does not load leaves the map with no ground under its pins, never a failed picture.
	if (result == TerrainData::LOAD_STEP_DONE) terrain_data_ = loading_;
	loading_.unref();
	ground_ = map_of(model).ground();
	install_terrain_();
	dirty_ = true;
	return ApplierStep::Built;
}

opennova::editor::OperationProgress MissionMapViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (loading_.is_null()) return progress;
	progress.done = uint64_t(loading_->get_load_steps_done());
	progress.total = uint64_t(loading_->get_load_step_count());
	progress.label = "terrain files";
	return progress;
}

void MissionMapViewportApplier::install_terrain_() {
	HudOverlay *hud = overlay();
	if (!hud) return;
	// The colour map and the depthspin water mask at the mission's water plane, as the game's world load gives its HUD
	// (GameWorld::build_minimap_water_mask; the game's hud presenter's set_minimap_terrain).
	Ref<Texture2D> water;
	if (terrain_data_.is_valid() && ground_.water) {
		const Vector3 plane = MissionObjectPlacer::bms_to_godot_position(Vector3(0.0f, 0.0f, float(ground_.water_height)));
		water = terrain_data_->build_minimap_water_mask(plane.y);
	}
	hud->set_minimap_terrain(terrain_data_, water);
	hud->set_minimap_grid_origin(Vector2(float(ground_.grid_x), float(ground_.grid_y)), ground_.grid_origin);
	// The mission's RotateMap180 as the game's presenter hands its HUD the map state (the CMAP's render sets the rest
	// of it from the view: its mode, its centre, its zoom); no markers of the HUD's own (the map draws the records).
	hud->set_minimap_state(Vector2(), 0.0f, 0, opennova::hud::kSpinmapZoomMin, opennova::hud::kSpinmapZoomMin, 0,
			ground_.flip_180, PackedInt32Array());
}

void MissionMapViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	const opennova::editor::MissionMapViewport &map = map_of(model);
	const opennova::editor::MissionMapGround &ground = map.ground();
	if (ground.water != ground_.water || ground.water_height != ground_.water_height || ground.flip_180 != ground_.flip_180 ||
			ground.grid_origin != ground_.grid_origin || ground.grid_x != ground_.grid_x || ground.grid_y != ground_.grid_y) {
		ground_ = ground;
		install_terrain_();
	}
	dirty_ = true;
}

void MissionMapViewportApplier::clear() {
	loading_.unref();
	terrain_data_.unref();
	ground_ = opennova::editor::MissionMapGround();
	if (HudOverlay *hud = overlay()) {
		hud->set_minimap_terrain(Ref<TerrainData>());
		hud->configure(Ref<HudPos>(), Ref<ResourceRoot>());
	}
	renderer_.clear();
	for (const RID &chunk : chunk_items_) RenderingServer::get_singleton()->free_rid(chunk);
	chunk_items_.clear();
	chunk_drawn_.clear();
	outline_serial_ = UINT64_MAX;
	outline_edges_ = 0;
	stamped_.reset();
	pass_visible_ = false;
	pass_terrain_tris_ = pass_sprites_ = pass_labels_ = 0;
	dirty_ = true;
}

void MissionMapViewportApplier::draw_(const opennova::editor::MissionMapViewport &map) {
	const opennova::editor::MissionMapCamera &camera = map.camera();
	const opennova::editor::MissionMapOptions &options = map.options();
	if (!dirty_ && camera == drawn_camera_ && options.grid == drawn_grid_ && options.text == drawn_text_ &&
			width_ == drawn_width_ && height_ == drawn_height_)
		return;
	dirty_ = false;
	drawn_camera_ = camera;
	drawn_grid_ = options.grid;
	drawn_text_ = options.text;
	drawn_width_ = width_;
	drawn_height_ = height_;
	renderer_.clear();
	pass_visible_ = false;
	pass_terrain_tris_ = pass_sprites_ = pass_labels_ = 0;
	HudOverlay *hud = overlay();
	Control *canvas = node<Control>(canvas_id_);
	if (!hud || !canvas || width_ <= 1 || height_ <= 1) return;
	// The CMAP's view: centred on the camera's centre (the player's place, no pan), at its zoom, its GRID and TEXT
	// toggles the options'; the payload rect the whole picture, as MissionMapView and the engine's
	// CommandMapView::render lay it out.
	cmap_.view.zoom = camera.zoom;
	cmap_.view.pan_x = 0;
	cmap_.view.pan_y = 0;
	cmap_.toggles.grid = options.grid;
	cmap_.toggles.text = options.text;
	cmap_.toggles.waypoints = true;
	cmap_.toggles.create_waypoints = false;
	opennova::hud::DeathMapFacts facts;
	facts.player_present = false; // no player: no crosshair through it
	facts.player_x = opennova::bms::to_fixed_16_16(camera.center[0]);
	facts.player_y = opennova::bms::to_fixed_16_16(camera.center[1]);
	const opennova::hud::MapViewRect rect{ 0, 0, width_, height_ };
	const int32_t scaled_800 = opennova::hud::map_view_design_to_device(800, float(width_) / 800.0f);
	const opennova::hud::HudFrameCompiler::MapWindowDraw *draw =
			hud->compile_command_map(cmap_, rect, scaled_800, facts, float(width_), float(height_));
	if (draw == nullptr) return;
	renderer_.ensure(canvas->get_canvas_item(), 0, false, hud->map_additive_material(), hud->map_water_material(),
			hud->map_modulate2x_material());
	// The pass is in the picture's pixels (the original's window coordinates, D3D9 pixel centres on the integers).
	renderer_.set_transform(d3d9_screen_to_canvas());
	renderer_.render(draw->pass.map, draw->glyphs, hud->map_pass_textures(), &draw->pass.over_lines);
	pass_visible_ = draw->pass.map.visible;
	pass_terrain_tris_ = int(draw->pass.map.terrain.size());
	pass_sprites_ = int(draw->pass.map.sprites.size());
	pass_labels_ = int(draw->pass.map.labels.size());
}

void MissionMapViewportApplier::draw_outlines_(const opennova::editor::MissionMapViewport &map) {
	RenderingServer *rs = RenderingServer::get_singleton();
	// The camera's transform: a mission point (metres, x east, y north) to the picture's pixel (MissionMapView::project).
	const opennova::editor::MissionMapView view = map.view(width_, height_);
	// Turned half a turn about the centre for a RotateMap180 mission (MissionMapView::flip_180).
	const double inv = (view.flip_180 ? -1.0 : 1.0) / double(view.scale);
	rs->canvas_item_set_transform(outline_item_,
			Transform2D(Vector2(float(inv), 0.0f), Vector2(0.0f, float(-inv)),
					Vector2(float(double(view.middle_x) - view.center[0] * inv),
							float(double(view.middle_y) + view.center[1] * inv))));
	if (map.outline_serial() == outline_serial_) return;
	outline_serial_ = map.outline_serial();
	const std::vector<float> &lines = map.outline_lines();
	const std::vector<uint32_t> &rgb = map.outline_rgb();
	const std::vector<uint64_t> &chunks = map.outline_chunks();
	outline_edges_ = int(rgb.size());
	// A canvas item a chunk under the wireframes' own (carried by its transform): the chunks whose edges moved drawn
	// again, the others kept; a chunk past the edges freed.
	while (chunk_items_.size() > chunks.size()) {
		rs->free_rid(chunk_items_.back());
		chunk_items_.pop_back();
		chunk_drawn_.pop_back();
	}
	while (chunk_items_.size() < chunks.size()) {
		const RID item = rs->canvas_item_create();
		rs->canvas_item_set_parent(item, outline_item_);
		chunk_items_.push_back(item);
		chunk_drawn_.push_back(UINT64_MAX);
	}
	constexpr size_t kChunk = opennova::editor::MissionMapViewport::kMissionMapLinesChunk;
	for (size_t chunk = 0; chunk < chunks.size(); ++chunk) {
		if (chunk_drawn_[chunk] == chunks[chunk]) continue;
		chunk_drawn_[chunk] = chunks[chunk];
		rs->canvas_item_clear(chunk_items_[chunk]);
		const size_t begin = chunk * kChunk, end = std::min(rgb.size(), begin + kChunk);
		if (end <= begin) continue;
		PackedVector2Array points;
		points.resize(int64_t(end - begin) * 2);
		PackedColorArray colours;
		colours.resize(int64_t(end - begin));
		Vector2 *to = points.ptrw();
		Color *colour = colours.ptrw();
		for (size_t i = begin; i < end; ++i) {
			to[2 * (i - begin)] = Vector2(lines[4 * i], lines[4 * i + 1]);
			to[2 * (i - begin) + 1] = Vector2(lines[4 * i + 2], lines[4 * i + 3]);
			colour[i - begin] = Color(float((rgb[i] >> 16) & 0xFFu) / 255.0f, float((rgb[i] >> 8) & 0xFFu) / 255.0f,
					float(rgb[i] & 0xFFu) / 255.0f);
		}
		// Thin lines (a negative width: a pixel whatever the transform), a colour an edge.
		rs->canvas_item_add_multiline(chunk_items_[chunk], points, colours, -1.0f);
		++chunks_uploaded_;
	}
}

void MissionMapViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	if (stamped_) report.files = stamped_->stamps();
	if (loading_.is_valid()) return;
	report.surface = terrain_data_.is_valid();
	draw_(map_of(model));
	draw_outlines_(map_of(model));
	opennova::io::JsonValue drawn = opennova::io::JsonValue::make_object();
	drawn.set("visible", opennova::io::JsonValue::make_bool(pass_visible_));
	drawn.set("terrain_tris", opennova::io::json_number(pass_terrain_tris_));
	drawn.set("sprites", opennova::io::json_number(pass_sprites_));
	drawn.set("labels", opennova::io::json_number(pass_labels_));
	drawn.set("terrain", opennova::io::JsonValue::make_bool(terrain_data_.is_valid()));
	drawn.set("outline_edges", opennova::io::json_number(outline_edges_));
	report.drawn = std::move(drawn);
}

void MissionMapViewportApplier::tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	if (loading_.is_null()) {
		draw_(map_of(model));
		draw_outlines_(map_of(model));
	}
}

void MissionMapViewportApplier::resize(int width, int height) {
	width_ = std::max(1, width);
	height_ = std::max(1, height);
}

bool MissionMapViewportApplier::ground_at(double x, double y, double &height) const {
	if (terrain_data_.is_null()) return false;
	const Vector3 at = MissionObjectPlacer::bms_to_godot_position(Vector3(float(x), float(y), 0.0f));
	const float found = terrain_data_->get_height_world_bilinear(at);
	if (!std::isfinite(found)) return false;
	height = double(MissionObjectPlacer::godot_to_bms_position(Vector3(at.x, found, at.z)).z);
	return true;
}

} // namespace godot
