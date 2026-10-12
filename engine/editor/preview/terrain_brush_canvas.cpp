#include <editor/preview/terrain_brush_canvas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/json.h>
#include <editor/documents/til_document.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/terrain_brush_shapes.h>
#include <editor/preview/terrain_view_commands.h>
#include <editor/preview/tile_tools.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_tools_parts.h>
#include <editor/session/view/session_view.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>

namespace opennova::editor {

namespace {

// One member of the session's brush set (a set_workspace), as the pane's controls set it.
void set_brush(CanvasRequests &out, const char *member, io::JsonValue value) {
	io::JsonValue brush = io::JsonValue::make_object();
	brush.set(member, std::move(value));
	io::JsonValue change = io::JsonValue::make_object();
	change.set("terrain_brush", std::move(brush));
	out.request(request::set_workspace(io::json_write(change)));
}

// The tools that lay values on a plane (Sculpt and Paint).
bool strokes(TerrainTool tool) {
	TerrainPlane plane = TerrainPlane::Heights;
	return terrain_tool_plane(tool, plane);
}

std::string metres(double value) {
	char buffer[32];
	std::snprintf(buffer, sizeof buffer, "%.1f", value);
	return buffer;
}

// A placed tile's square's south-west corner on the mission's plane (x east, y north): the record's corner is its
// north-west (preview/tile_tools.h), the square spanning 16 units south of it.
TerrainPoint tile_corner(const TilOverlayEntry &entry) {
	return TerrainPoint{double(entry.x_fixed) / 65536.0, double(entry.z_fixed) / 65536.0 - double(kTerrainTileUnits)};
}

// The quarter turns and the flip across a tile's orientation bits are, as a stamp makes them (tile_stamp_flags), for
// the corner tick its square shows.
void tile_turn_of(uint8_t flags, int &rotation, bool &flip_h) {
	rotation = 0;
	flip_h = false;
	for (int r = 0; r < 4; ++r)
		for (int h = 0; h < 2; ++h)
			if (tile_stamp_flags(r, h != 0, false) == (flags & 7u)) {
				rotation = r;
				flip_h = h != 0;
				return;
			}
}

} // namespace

bool med_depth_pixel(const terrain::SectorLayout &layout, bool wrap_x, bool wrap_z, TerrainPoint at, int &x, int &y) {
	if (!layout.sector_grid) return false;
	// MED's map: x east, its second axis down the map (the stored frame's z, the mission's y negated), whole units; a
	// sector 512 of them.
	const int ux = int(std::floor(at.x)), uz = int(std::floor(-at.y));
	const terrain::CoordsSectorResolve sector = terrain::coords_resolve_sector(
			layout, int(std::floor(at.x / 512.0)), int(std::floor(-at.y / 512.0)), terrain::coords_runtime_options(wrap_x, wrap_z));
	if (!sector.valid) {
		x = y = 0; // [@ 0x422a55: an empty sector reads (0, 0)]
		return true;
	}
	// Each wrapped into the sector's 512 (@ 0x4229ed..0x422a4e), the second one further (@ 0x4229e8), then the
	// quadrant's halves: bit 2 of the sector's id - 1 the right (@ 0x422a76), bit 1 the lower (@ 0x422a6c).
	x = (ux & 511) + sector.quadrant_x;
	y = ((uz + 1) & 511) + sector.quadrant_z;
	return true;
}

void TerrainBrushHalf::follow(const SessionView &view, const DocumentBase *document) {
	brush_ = view.workspace.terrain_brush;
	view_path_ = document ? document->path() : std::string();
	live_ = document && terrain_brush_live(view, *document);
	if (!live_) {
		stroking_ = false;
		tile_pressing_ = false;
		dabs_.reset();
	}
	tiles_.entries.clear();
	tile_ids_.clear();
	tile_selected_.clear();
	if (live_ && brush_.tool == TerrainTool::Tiles) follow_tiles_(view, *document);
}

void TerrainBrushHalf::follow_tiles_(const SessionView &view, const DocumentBase &document) {
	TerrainFamily family;
	if (!resolve_terrain_family(view, document, family) || family.tiles.path.empty()) return;
	const TilDocument *placement = nullptr;
	for (const auto &open : view.documents.open)
		if (open && open->path() == family.tiles.path) placement = dynamic_cast<const TilDocument *>(open.get());
	if (!placement) return;
	tiles_ = placement->file();
	const std::vector<std::shared_ptr<const Node>> &rows = placement->rows();
	const std::vector<NodeId> &selected = view.workspace.document(document.path()).tiles;
	for (size_t i = 0; i < rows.size() && i < tiles_.entries.size(); ++i) {
		tile_ids_.push_back(rows[i]->id);
		tile_selected_.push_back(std::find(selected.begin(), selected.end(), rows[i]->id) != selected.end());
	}
	tiles_.entries.resize(tile_ids_.size());
}

void TerrainBrushHalf::select_(CanvasPoint at, CanvasJoin join, CanvasRequests &out) const {
	ViewportCommand click;
	click.name = "click";
	click.kind = kind_;
	click.has_at = true;
	click.at_x = at.x;
	click.at_y = at.y;
	click.mode = select_mode(join);
	out.request(request::edit_in_viewport(view_path_, click));
}

bool TerrainBrushHalf::tile_input_(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space,
                                   const CanvasInput &in, CanvasRequests &out, const ViewportContext *context) {
	// A press under way: its drag moves the grabbed tile (the selection with it), its release ends the move, or a click
	// on a selected tile selects it alone.
	if (tile_pressing_) {
		if (gesture.pressed() && in.down) {
			gesture.move(in.screen);
			if (context && space && gesture.dragging() && (!tile_sent_ || in.delta.x != 0.0f || in.delta.y != 0.0f)) {
				// Planned here, as the host plans its own drags: a batch on the .til under the gesture's token (the
				// wire's drag is the same planner's, terrain_view_drag).
				ViewportDrag drag;
				drag.id = tile_grab_;
				drag.handle = "tile";
				drag.by = false;
				drag.x = in.mouse.x - grab_offset_.x;
				drag.y = in.mouse.y - grab_offset_.y;
				drag.snap = in.keys.shift ? 16.0f : 0.0f;
				drag.gesture = gesture.token();
				drag.end = false;
				drag.kind = kind_;
				const TerrainViewContext view{*context, *space, view_path_, kind_, nullptr};
				std::string error;
				if (plan_tile_drag(view, drag, out, error)) gesture.sent();
				tile_sent_ = true;
			}
			return true;
		}
		const bool click = gesture.release(out);
		if (click && select_on_click_) select_(press_mouse_, CanvasJoin::Replace, out);
		tile_pressing_ = false;
		select_on_click_ = false;
		return true;
	}
	TerrainPoint at;
	double height = 0.0;
	const bool on_ground = space && space->pointer_ground(in.mouse.x, in.mouse.y, at, height);
	const size_t under = on_ground ? tile_at(tiles_, at.x, at.y) : size_t(-1);
	const bool grabbed = under < tile_ids_.size();
	const CanvasJoin join = canvas_join(in.keys);
	select_on_click_ = grabbed && tile_selected_[under] && join == CanvasJoin::Replace;
	if (!select_on_click_) select_(in.mouse, join, out);
	if (!grabbed) return true;
	gesture.press(subject, in.screen, out);
	tile_pressing_ = true;
	tile_sent_ = false;
	tile_grab_ = tile_ids_[under];
	press_mouse_ = in.mouse;
	const TerrainPoint corner = tile_corner(tiles_.entries[under]);
	CanvasPoint shown;
	grab_offset_ = space->to_picture(corner, space->planar() ? 0.0 : space->height(corner), shown.x, shown.y)
	                       ? CanvasPoint{in.mouse.x - shown.x, in.mouse.y - shown.y}
	                       : CanvasPoint{};
	return true;
}

TerrainStrokeAsk TerrainBrushHalf::stroke_ask(const TerrainBrush &brush, bool invert) {
	TerrainStrokeAsk ask;
	terrain_tool_plane(brush.tool, ask.plane);
	if (!invert) return ask;
	// Shift inverts: Raise lowers and Lower raises; Foliage paints 0 (grows nothing).
	if (brush.tool == TerrainTool::Raise || brush.tool == TerrainTool::Lower) {
		ask.brush.tool = brush.tool == TerrainTool::Raise ? TerrainTool::Lower : TerrainTool::Raise;
		ask.brush_named |= kBrushTool;
	} else if (brush.tool == TerrainTool::Foliage) {
		ask.brush.foliage = 0;
		ask.brush_named |= kBrushFoliage;
	}
	return ask;
}

void TerrainBrushHalf::send_(CanvasGesture &gesture, const std::vector<TerrainPoint> &dabs, bool invert, CanvasRequests &out) {
	if (dabs.empty()) return;
	TerrainStrokeAsk ask = stroke_ask(brush_, invert);
	for (const TerrainPoint &dab : dabs) {
		if (ask.points.size() == kTerrainStrokePoints) {
			// More dabs in a frame than a stroke's sample holds: the rest go in another sample of the gesture.
			ask.gesture = gesture.token();
			ask.end = false;
			out.request(request::terrain_stroke(view_path_, ask));
			ask.points.clear();
		}
		ask.points.emplace_back(dab.x, dab.y);
	}
	ask.gesture = gesture.token();
	ask.end = false;
	out.request(request::terrain_stroke(view_path_, ask));
	gesture.sent();
}

void TerrainBrushHalf::command_(const char *name, std::vector<double> by, const std::string &value, CanvasRequests &out) const {
	ViewportCommand command;
	command.name = name;
	command.kind = kind_;
	command.by = std::move(by);
	command.value = value;
	out.request(request::edit_in_viewport(view_path_, command));
}

void TerrainBrushHalf::keys_(CanvasGesture &gesture, const CanvasInput &in, CanvasRequests &out, bool &took) {
	const CanvasKeyboard &keys = in.keyboard;
	if (!keys.focused) return;
	if (keys.escape) {
		took = true;
		if (stroking_) {
			// Esc during the press: the stroke dropped wherever it went, no redo left of it.
			gesture.cancel(out);
			stroking_ = false;
			dabs_.reset();
		} else {
			set_brush(out, "tool", io::json_string(terrain_tool_token(TerrainTool::None)));
		}
	}
	if (keys.bracket != 0) {
		took = true;
		if (in.keys.shift) {
			const double strength = std::clamp(double(brush_.strength) + 0.1 * keys.bracket, 0.0, 1.0);
			set_brush(out, "strength", io::json_number(std::round(strength * 100.0) / 100.0));
		} else {
			double radius = keys.bracket > 0 ? std::max(1.0, double(brush_.radius) * 1.25) : double(brush_.radius) / 1.25;
			if (radius < 0.5) radius = 0.0;
			set_brush(out, "radius", io::json_number(std::min(radius, double(kTerrainRadiusMost))));
		}
	}
	if (brush_.tool == TerrainTool::Stamp) {
		// The stamp's own turn and flips.
		if (keys.rotate) {
			took = true;
			set_brush(out, "rotation", io::json_number(double((brush_.rotation + (in.keys.shift ? 3 : 1)) % 4)));
		}
		if (keys.flip_h) {
			took = true;
			set_brush(out, "flip_h", io::JsonValue::make_bool(!brush_.flip_h));
		}
		if (keys.flip_v) {
			took = true;
			set_brush(out, "flip_v", io::JsonValue::make_bool(!brush_.flip_v));
		}
	} else if (brush_.tool == TerrainTool::Tiles) {
		// The selected tiles, in the original tools' words (Rotate CW/CCW, Flip H/V, Tile Layer Up/Down, Delete).
		if (keys.rotate) {
			took = true;
			command_("tile_rotate", {in.keys.shift ? -1.0 : 1.0}, std::string(), out);
		}
		if (keys.flip_h) {
			took = true;
			command_("tile_flip", {}, "h", out);
		}
		if (keys.flip_v) {
			took = true;
			command_("tile_flip", {}, "v", out);
		}
		if (keys.page != 0) {
			took = true;
			command_("tile_layer", {double(keys.page)}, std::string(), out);
		}
		if (keys.remove) {
			took = true;
			command_("tile_delete", {}, std::string(), out);
		}
		// MED's arrows (Shift: 16 units), north up; Ctrl+D a copy a square east.
		if (keys.arrow_x != 0 || keys.arrow_y != 0) {
			took = true;
			command_("tile_nudge", {double(keys.arrow_x), double(-keys.arrow_y)}, in.keys.shift ? "16" : std::string(), out);
		}
		if (keys.duplicate) {
			took = true;
			command_("tile_duplicate", {16.0, 0.0}, std::string(), out);
		}
	}
}

bool TerrainBrushHalf::input(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space,
                             const CanvasInput &in, CanvasRequests &out, const ViewportContext *context) {
	if (!live_) return false;
	bool took = false;
	keys_(gesture, in, out, took);
	const bool camera = in.middle || in.keys.alt || in.right_pressed || in.right_down || in.panning;
	if (tile_pressing_) return tile_input_(gesture, subject, space, in, out, context);
	// A stroke under way: its drag lays the dabs its path passes, its release ends it.
	if (stroking_) {
		if (gesture.pressed() && in.down) {
			gesture.move(in.screen);
			// A Fill is the press's alone (S25): the drag lays nothing more.
			TerrainPlane plane = TerrainPlane::Heights;
			if (brush_.fill && terrain_tool_plane(brush_.tool, plane) && plane != TerrainPlane::Heights) return true;
			TerrainPoint at;
			double height = 0.0;
			std::vector<TerrainPoint> dabs;
			if (space && space->pointer_ground(in.mouse.x, in.mouse.y, at, height))
				dabs_.to(at, terrain_dab_step(brush_, 1.0), dabs);
			send_(gesture, dabs, in.keys.shift, out);
			return true;
		}
		gesture.release(out);
		stroking_ = false;
		dabs_.reset();
		return true;
	}
	if (!in.pressed || camera || !in.hovered) return took;
	TerrainPoint at;
	double height = 0.0;
	const bool on_ground = space && space->pointer_ground(in.mouse.x, in.mouse.y, at, height);
	if (in.keys.ctrl && brush_.tool != TerrainTool::Tiles) {
		// The eyedropper: the value under the pointer taken into the brush (the viewport's command).
		ViewportCommand pick;
		pick.name = "eyedropper";
		pick.kind = kind_;
		pick.has_at = true;
		pick.at_x = in.mouse.x;
		pick.at_y = in.mouse.y;
		out.request(request::edit_in_viewport(view_path_, pick));
		return true;
	}
	if (strokes(brush_.tool)) {
		if (!on_ground) return true; // nothing under the pointer to brush
		gesture.press(subject, in.screen, out);
		stroking_ = true;
		std::vector<TerrainPoint> dabs;
		dabs_.begin(at, dabs);
		press_height_ = height;
		send_(gesture, dabs, in.keys.shift, out);
		return true;
	}
	if (brush_.tool == TerrainTool::Stamp) {
		// A stamp: the tile laid with its corner under the pointer, whole units (Shift: the 16 grid).
		ViewportDrop drop;
		drop.reference = "tile";
		drop.name = std::to_string(brush_.cell);
		drop.x = in.mouse.x;
		drop.y = in.mouse.y;
		drop.kind = kind_;
		drop.snap = in.keys.shift ? 16.0f : 0.0f;
		out.request(request::edit_in_viewport(view_path_, drop));
		return true;
	}
	if (brush_.tool == TerrainTool::Tiles) return tile_input_(gesture, subject, space, in, out, context);
	return took;
}

void TerrainBrushHalf::shapes(const TerrainBrushSpace *space, const CanvasInput &in, OverlayList &out) const {
	if (!live_ || !space) return;
	// The selected tiles, wherever the pointer is.
	for (size_t i = 0; i < tile_ids_.size(); ++i) {
		if (!tile_selected_[i]) continue;
		int rotation = 0;
		bool flip_h = false;
		tile_turn_of(tiles_.entries[i].flags, rotation, flip_h);
		terrain_tile_square(*space, tile_corner(tiles_.entries[i]), rotation, flip_h, false, OverlayRole::Selected, out);
	}
	TerrainPoint at;
	double height = 0.0;
	bool shown = in.hovered && space->pointer_ground(in.mouse.x, in.mouse.y, at, height);
	// An MCP client's cursor where no pointer is over the picture (a screenshot of the ring).
	if (!shown && brush_.has_cursor) {
		at = TerrainPoint{brush_.cursor[0], brush_.cursor[1]};
		height = space->height(at);
		shown = true;
	}
	if (!shown) return;
	if (brush_.tool == TerrainTool::Stamp) {
		// Where the stamp lays the tile: its corner as the click would place it (Shift the 16 grid).
		int32_t x = 0, z = 0;
		tile_corner_at(at.x, at.y, in.keys.shift, x, z);
		TilOverlayEntry corner;
		corner.x_fixed = x;
		corner.z_fixed = z;
		terrain_tile_square(*space, tile_corner(corner), brush_.rotation, brush_.flip_h, brush_.flip_v, OverlayRole::Normal, out);
		return;
	}
	if (!strokes(brush_.tool)) return;
	if (space->planar()) {
		terrain_brush_circle(*space, brush_, at, out);
		return;
	}
	// Set's ring flat at its height; Flatten's, while it strokes, at the height under its press (its target).
	const bool set = brush_.tool == TerrainTool::Set, flatten = brush_.tool == TerrainTool::Flatten && stroking_;
	terrain_brush_ring(*space, brush_, at, set || flatten, set ? double(brush_.height) : flatten ? press_height_ : 0.0, out);
}

std::string terrain_brush_hint(const TerrainBrush &brush, bool on_ground, TerrainPoint at, double height,
                               const MissionGround *ground) {
	const TerrainToolFamily family = terrain_tool_family(brush.tool);
	if (family == TerrainToolFamily::None) return std::string();
	if (!on_ground) return std::string(terrain_tool_label(brush.tool)) + ": point at the ground.";
	switch (family) {
	case TerrainToolFamily::Sculpt: {
		// The original editor's depth-pixel readout (med_depth_pixel), then the height there.
		std::string line;
		int px = 0, py = 0;
		const terrain::TerrainHeightField *field = ground ? ground->height_field() : nullptr;
		if (field && med_depth_pixel(field->layout, field->wrap_x, field->wrap_z, at, px, py)) {
			char readout[64];
			std::snprintf(readout, sizeof readout, "Depth Pixel X: %3d Y: %3d", px, py);
			line = std::string(readout) + " - height " + metres(height) + " m";
		} else {
			line = "Height " + metres(height) + " m";
		}
		if (brush.tool == TerrainTool::Set) line += ": drag to set it to " + metres(brush.height) + " m";
		else if (brush.tool == TerrainTool::Flatten) line += ": drag to flatten to the height under the press";
		else line += std::string(": drag to ") + (brush.tool == TerrainTool::Raise ? "raise" : brush.tool == TerrainTool::Lower ? "lower" : "smooth") + " it (Shift the other way)";
		return line + ".";
	}
	case TerrainToolFamily::Paint: {
		const MissionGroundFacts facts = ground ? ground->terrain_at(at.x, at.y, height) : MissionGroundFacts();
		if (brush.tool == TerrainTool::Surface) {
			std::string line = "Paint " + mission_surface_words(brush.surface) + " (surface " + std::to_string(brush.surface) + ")";
			if (facts.surface >= 0) {
				line += " - here " + mission_surface_words(facts.surface) + " (surface " + std::to_string(facts.surface);
				const char *name = mission_surface_name(facts.surface);
				line += name && *name ? std::string(", ") + name + ")" : std::string(")");
			}
			return line + ".";
		}
		if (brush.tool == TerrainTool::Foliage) {
			std::string line = "Foliage code " + std::to_string(brush.foliage) + (brush.foliage ? "" : " (grows nothing)");
			if (facts.foliage_code >= 0) line += " - here " + std::to_string(facts.foliage_code);
			return line + " (Shift paints 0).";
		}
		if (brush.tool == TerrainTool::Colour) {
			char rgb[16];
			std::snprintf(rgb, sizeof rgb, "#%06X", unsigned(brush.colour & 0xFFFFFFu));
			return std::string("Paint the colour map ") + rgb + ".";
		}
		return "Paint detail layer " + std::to_string(brush.layer + 1) + " into the blend map.";
	}
	case TerrainToolFamily::Tiles:
		if (brush.tool == TerrainTool::Stamp)
			return "Click to stamp cell " + std::to_string(brush.cell) + ", its corner here, turned " +
			       std::to_string(brush.rotation * 90) + " deg" + (brush.flip_h ? ", flipped across" : "") +
			       (brush.flip_v ? ", flipped up and down" : "") + "; Shift snaps to 16.";
		return "Click a tile to select it; R turns, H and V flip, PgUp and PgDn layer, Delete removes.";
	case TerrainToolFamily::None: break;
	}
	return std::string();
}

std::string TerrainBrushHalf::hint(const TerrainBrushSpace *space, const CanvasInput &in, const MissionGround *ground) const {
	if (!live_) return std::string();
	TerrainPoint at;
	double height = 0.0;
	const bool on_ground = space && in.hovered && space->pointer_ground(in.mouse.x, in.mouse.y, at, height);
	return terrain_brush_hint(brush_, on_ground, at, height, ground);
}

bool TerrainBrushHalf::ground(const TerrainBrushSpace *space, const CanvasInput &in, TerrainPoint &at) const {
	double height = 0.0;
	return live_ && space && in.hovered && space->pointer_ground(in.mouse.x, in.mouse.y, at, height);
}

} // namespace opennova::editor
