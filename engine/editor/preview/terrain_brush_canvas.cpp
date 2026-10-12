#include <editor/preview/terrain_brush_canvas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/json.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/terrain_brush_shapes.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_tools_parts.h>
#include <editor/session/view/session_view.h>

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

} // namespace

void TerrainBrushHalf::follow(const SessionView &view, const DocumentBase *document) {
	brush_ = view.workspace.terrain_brush;
	view_path_ = document ? document->path() : std::string();
	live_ = document && terrain_brush_live(view, *document);
	if (!live_) {
		stroking_ = false;
		dabs_.reset();
	}
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
	}
}

bool TerrainBrushHalf::input(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space,
                             const CanvasInput &in, CanvasRequests &out) {
	if (!live_) return false;
	bool took = false;
	keys_(gesture, in, out, took);
	const bool camera = in.middle || in.keys.alt || in.right_pressed || in.right_down || in.panning;
	// A stroke under way: its drag lays the dabs its path passes, its release ends it.
	if (stroking_) {
		if (gesture.pressed() && in.down) {
			gesture.move(in.screen);
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
	if (brush_.tool == TerrainTool::Tiles) {
		// A click selects the tile under the pointer (the viewport's click while the brush is live).
		ViewportCommand click;
		click.name = "click";
		click.kind = kind_;
		click.has_at = true;
		click.at_x = in.mouse.x;
		click.at_y = in.mouse.y;
		click.mode = select_mode(canvas_join(in.keys));
		out.request(request::edit_in_viewport(view_path_, click));
		return true;
	}
	return took;
}

void TerrainBrushHalf::shapes(const TerrainBrushSpace *space, const CanvasInput &in, OverlayList &out) const {
	if (!live_ || !space) return;
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
		terrain_tile_square(*space, at, brush_.rotation, brush_.flip_h, brush_.flip_v, OverlayRole::Normal, out);
		return;
	}
	if (!strokes(brush_.tool)) return;
	if (space->planar()) {
		terrain_brush_circle(*space, brush_, at, out);
		return;
	}
	const bool flat = brush_.tool == TerrainTool::Set;
	terrain_brush_ring(*space, brush_, at, flat, flat ? double(brush_.height) : 0.0, out);
}

std::string terrain_brush_hint(const TerrainBrush &brush, bool on_ground, TerrainPoint at, double height,
                               const MissionGround *ground) {
	const TerrainToolFamily family = terrain_tool_family(brush.tool);
	if (family == TerrainToolFamily::None) return std::string();
	if (!on_ground) return std::string(terrain_tool_label(brush.tool)) + ": point at the ground.";
	switch (family) {
	case TerrainToolFamily::Sculpt: {
		// The original editor's depth-pixel readout [orig: JOTACmed.exe sub_417B30].
		std::string line = "Height " + metres(height) + " m";
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
			return std::string("Paint the colour map ") + rgb + " (Ctrl+click picks the colour under the pointer).";
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

} // namespace opennova::editor
