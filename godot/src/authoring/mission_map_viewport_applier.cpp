#include "authoring/mission_map_viewport_applier.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/io/fixed.h>
#include <base/vfs/file_stamps.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/til/til.h>
#include <runtime/hud/hud_minimap.h>

#include "hud/hud_pos.h"
#include "mission/mission_object_placer.h"
#include "render/d3d9_raster_device.h"
#include "terrain/terrain_image_convert.h"
#include "util/string_convert.h"
#include "world/world_scene.h"

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
	// S25: the placed tiles over the pass (MED's 2D view draws them over its map), under the wireframes.
	Control *tiles = memnew(Control);
	tiles->set_name("Tiles");
	tiles->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	tiles->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport.add_child(tiles);
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
	// Each tile's cell sampled nearest over its square (MED's 2D view).
	tiles_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(tiles_item_, tiles->get_canvas_item());
	rs->canvas_item_set_default_texture_filter(tiles_item_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
	root_.instantiate();
	cmap_.on_load();
}

MissionMapViewportApplier::~MissionMapViewportApplier() {
	renderer_.release();
	for (const RID &chunk : chunk_items_) RenderingServer::get_singleton()->free_rid(chunk);
	if (outline_item_.is_valid()) RenderingServer::get_singleton()->free_rid(outline_item_);
	if (tiles_item_.is_valid()) RenderingServer::get_singleton()->free_rid(tiles_item_);
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
		// As every load of a mission's terrain is prepared (world/world_scene.h): its tile set, and
		// its .env by the header's name with the extension the time-of-day load opens it by.
		prepare_terrain_load(**loading_, opennova::to_gd(map.scene().header().tile_set),
				opennova::to_gd(map.scene().header().environment));
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
	// The colour map installed takes the slot back: the background given again at the next draw.
	picture_serial_ = UINT64_MAX;
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
	picture_.unref();
	picture_serial_ = UINT64_MAX;
	for (const RID &chunk : chunk_items_) RenderingServer::get_singleton()->free_rid(chunk);
	chunk_items_.clear();
	chunk_drawn_.clear();
	outline_serial_ = UINT64_MAX;
	outline_edges_ = 0;
	if (tiles_item_.is_valid()) RenderingServer::get_singleton()->canvas_item_clear(tiles_item_);
	tile_set_.unref();
	tile_set_from_ = 0;
	tiles_serial_ = UINT64_MAX;
	tiles_drawn_ = 0;
	stamped_.reset();
	pass_visible_ = false;
	pass_terrain_tris_ = pass_sprites_ = pass_labels_ = 0;
	dirty_ = true;
}

void MissionMapViewportApplier::apply_picture_(const opennova::editor::MissionMapViewport &map) {
	HudOverlay *hud = overlay();
	if (!hud) return;
	const opennova::editor::MissionMapTerrainView &view = map.terrain_view();
	if (view.serial() == picture_serial_) return;
	picture_serial_ = view.serial();
	dirty_ = true;
	// The colour map, or nothing: the slot given back to the colour map the terrain installed.
	const std::vector<uint32_t> &pixels = view.picture();
	constexpr int kSide = 1024;
	if (pixels.size() != size_t(kSide) * kSide) {
		if (picture_.is_valid()) hud->set_minimap_terrain_picture(Ref<Texture2D>());
		picture_.unref();
		picture_bytes_ = PackedByteArray();
		picture_whole_ = UINT64_MAX;
		return;
	}
	// The picture's 0xAARRGGBB words as RGBA bytes, in the atlas's own rows (the colour map's layout: the CMAP's
	// compile lays both over the sector grid alike): all of them for a whole make, else the rects its patches changed
	// since the last upload alone (S25: a stroke's dab converts its region, not the 1M texels). Godot takes the
	// texture's bytes whole (ImageTexture::update has no sub-rect), one upload a frame at most.
	const auto convert = [&](uint8_t *out, int x0, int y0, int x1, int y1) {
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) {
				const size_t i = size_t(y) * kSide + size_t(x);
				const uint32_t p = pixels[i];
				out[4 * i] = uint8_t(p >> 16);
				out[4 * i + 1] = uint8_t(p >> 8);
				out[4 * i + 2] = uint8_t(p);
				out[4 * i + 3] = uint8_t(p >> 24);
			}
		picture_converted_ += (x1 - x0) * (y1 - y0);
	};
	if (picture_whole_ != view.whole_serial() || picture_bytes_.size() != int64_t(pixels.size()) * 4) {
		picture_bytes_.resize(int64_t(pixels.size()) * 4);
		convert(picture_bytes_.ptrw(), 0, 0, kSide, kSide);
		picture_whole_ = view.whole_serial();
		picture_rects_ = view.dirty().size();
	} else {
		uint8_t *out = picture_bytes_.ptrw();
		const std::vector<opennova::editor::MissionMapAtlasRect> &rects = view.dirty();
		for (size_t i = picture_rects_; i < rects.size(); ++i)
			convert(out, std::max(rects[i].x0, 0), std::max(rects[i].y0, 0), std::min(rects[i].x1, kSide), std::min(rects[i].y1, kSide));
		picture_rects_ = rects.size();
	}
	const Ref<Image> image = Image::create_from_data(kSide, kSide, false, Image::FORMAT_RGBA8, picture_bytes_);
	if (picture_.is_valid()) picture_->update(image);
	else picture_ = ImageTexture::create_from_image(image);
	++picture_uploads_;
	hud->set_minimap_terrain_picture(picture_);
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

Transform2D MissionMapViewportApplier::map_transform_(const opennova::editor::MissionMapViewport &map) const {
	// The camera's transform: a mission point (metres, x east, y north) to the picture's pixel (MissionMapView::project).
	const opennova::editor::MissionMapView view = map.view(width_, height_);
	// Turned half a turn about the centre for a RotateMap180 mission (MissionMapView::flip_180).
	const double inv = (view.flip_180 ? -1.0 : 1.0) / double(view.scale);
	return Transform2D(Vector2(float(inv), 0.0f), Vector2(0.0f, float(-inv)),
			Vector2(float(double(view.middle_x) - view.center[0] * inv), float(double(view.middle_y) + view.center[1] * inv)));
}

void MissionMapViewportApplier::draw_tiles_(const opennova::editor::MissionMapViewport &map) {
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_set_transform(tiles_item_, map_transform_(map));
	// The tile set as the terrain loaded it, made once a load: each texel's alpha 0 (not drawn) or opaque, as MED's 2D
	// view writes a texel whose alpha byte is not 0 with its alpha cleared [orig: JOTACmed sub_417D30, the texel test
	// and the & 0x00FFFFFF; docs/tiles/til-re.md "The editors' tile tools"].
	const uint64_t from = terrain_data_.is_valid() ? terrain_data_->get_instance_id() : 0;
	bool remade = false;
	if (from != tile_set_from_) {
		tile_set_from_ = from;
		tile_set_.unref();
		tile_set_width_ = tile_set_height_ = 0;
		opennova::terrain::Rgba8Image texels;
		if (terrain_data_.is_valid() && texture_to_rgba8(terrain_data_->get_tilestrip_tex(), texels)) {
			for (size_t i = 3; i < texels.pixels.size(); i += 4) texels.pixels[i] = texels.pixels[i] ? 255 : 0;
			PackedByteArray bytes;
			bytes.resize(int64_t(texels.pixels.size()));
			std::memcpy(bytes.ptrw(), texels.pixels.data(), texels.pixels.size());
			const Ref<Image> image = Image::create_from_data(int(texels.width), int(texels.height), false,
					Image::FORMAT_RGBA8, bytes);
			if (image.is_valid()) {
				tile_set_ = ImageTexture::create_from_image(image);
				tile_set_width_ = int(texels.width);
				tile_set_height_ = int(texels.height);
			}
		}
		remade = true;
	}
	if (!remade && map.tiles_serial() == tiles_serial_) return;
	tiles_serial_ = map.tiles_serial();
	rs->canvas_item_clear(tiles_item_);
	tiles_drawn_ = 0;
	if (tile_set_.is_null()) return;
	++tiles_redrawn_;
	// Each record in file order (MED draws its layers in ascending order, the order it writes them in, which is the
	// file's), its cell over its 16-unit square, the cell's corners where the game's drawing puts them: the square from
	// the record's corner (x, the stored z: its north-west) 16 units east and south, the cell's first row along its
	// north edge, turned and flipped by the record's flags (til_build_entry_uv_quad).
	const PackedColorArray white = PackedColorArray({ Color(1.0f, 1.0f, 1.0f, 1.0f) });
	for (const opennova::TilOverlayEntry &entry : map.placed_tiles().entries) {
		const opennova::TilUvQuad quad =
				opennova::til_build_entry_uv_quad(entry.tile_index, entry.flags, tile_set_width_, tile_set_height_);
		if (!quad.valid) continue;
		// The square's north-west corner the record's (x, stored z), its first texel's (preview/tile_tools.h).
		const float x0 = float(double(entry.x_fixed) / opennova::io::kFp16OneD), north = float(double(entry.z_fixed) / opennova::io::kFp16OneD);
		const float side = float(opennova::TIL_CELL_WORLD_UNITS);
		const PackedVector2Array points = PackedVector2Array({ Vector2(x0, north), Vector2(x0 + side, north),
				Vector2(x0 + side, north - side), Vector2(x0, north - side) });
		const auto uv = [&](size_t corner) { return Vector2(quad.corners[corner].u, quad.corners[corner].v); };
		// The corners in order round the square: north-west (the cell's first texel), north-east, south-east, south-west.
		const PackedVector2Array uvs = PackedVector2Array({ uv(0), uv(1), uv(3), uv(2) });
		rs->canvas_item_add_primitive(tiles_item_, points, white, uvs, tile_set_->get_rid());
		++tiles_drawn_;
	}
}

void MissionMapViewportApplier::draw_outlines_(const opennova::editor::MissionMapViewport &map) {
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_set_transform(outline_item_, map_transform_(map));
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
	apply_picture_(map_of(model));
	draw_(map_of(model));
	draw_tiles_(map_of(model));
	draw_outlines_(map_of(model));
	opennova::io::JsonValue drawn = opennova::io::JsonValue::make_object();
	drawn.set("tiles", opennova::io::json_number(tiles_drawn_));
	drawn.set("visible", opennova::io::JsonValue::make_bool(pass_visible_));
	drawn.set("terrain_tris", opennova::io::json_number(pass_terrain_tris_));
	drawn.set("sprites", opennova::io::json_number(pass_sprites_));
	drawn.set("labels", opennova::io::json_number(pass_labels_));
	drawn.set("terrain", opennova::io::JsonValue::make_bool(terrain_data_.is_valid()));
	drawn.set("outline_edges", opennova::io::json_number(outline_edges_));
	drawn.set("background", opennova::io::JsonValue::make_bool(picture_.is_valid()));
	// S25: the background's uploads and the texels converted for them (a stroke's patch its rects alone).
	drawn.set("background_uploads", opennova::io::json_number(double(picture_uploads_)));
	drawn.set("background_converted", opennova::io::json_number(double(picture_converted_)));
	report.drawn = std::move(drawn);
}

void MissionMapViewportApplier::tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	if (loading_.is_null()) {
		apply_picture_(map_of(model));
		draw_(map_of(model));
		draw_tiles_(map_of(model));
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
