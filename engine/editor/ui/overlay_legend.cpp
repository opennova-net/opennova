#include <editor/ui/overlay_legend.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include <base/io/json.h>
#include <editor/preview/terrain_brush_overlay.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/viewport_canvas.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

// A row's tip: its key and words, and its share of the picture.
std::string row_tip(const MissionOverlayRow &row) {
	std::string tip = row.key + ": " + row.words;
	if (row.share > 0.0) {
		char share[32];
		std::snprintf(share, sizeof share, " (%.1f%% of the picture)", row.share * 100.0);
		tip += share;
	}
	return tip;
}

ViewportCanvas::LegendRow legend_row(const MissionOverlayRow &row, bool marked) {
	ViewportCanvas::LegendRow line;
	std::copy(std::begin(row.rgb), std::end(row.rgb), line.rgb);
	line.key = row.key;
	line.name = row.name;
	line.tip = row_tip(row);
	line.marked = marked;
	return line;
}

} // namespace

ViewportCanvas::Legend overlay_legend_of(const SessionView &view, const std::string &path, const MissionOverlayImage &overlay,
                                         const std::vector<FoliageDef> &defs, bool open) {
	// The value the brush lays, while it paints the overlay's map here: its class or code, or with Erase the game's
	// default in their place (class 1, code 0: session/terrain_stroke_paint.cpp).
	const bool surfaces = overlay.kind == MissionGroundOverlay::Surfaces;
	const bool brushes = overlay.kind != MissionGroundOverlay::None && terrain_brush_overlay(view, path) == overlay.kind;
	const TerrainBrush &brush = view.workspace.terrain_brush;
	const int value = brush.erase ? (surfaces ? 1 : 0) : surfaces ? brush.surface : brush.foliage;
	const MissionOverlayRow laid = brushes ? mission_overlay_brush_row(overlay.kind, value, defs) : MissionOverlayRow();
	ViewportCanvas::Legend legend;
	legend.open = open;
	legend.title = overlay.kind == MissionGroundOverlay::Surfaces ? "Surface classes" : "Foliage";
	legend.tip = overlay.title;
	if (!overlay.words.empty()) legend.tip += (legend.tip.empty() ? "" : "\n") + overlay.words;
	bool held = false;
	for (const MissionOverlayRow &row : overlay.legend) {
		const bool marked = brushes && row.value >= 0 && row.value == laid.value;
		held = held || marked;
		legend.rows.push_back(legend_row(row, marked));
	}
	// The brush's value where the picture holds none of it yet: its row, outlined, after those it holds.
	if (brushes && !held) {
		ViewportCanvas::LegendRow line = legend_row(laid, true);
		line.tip = "The brush lays " + line.tip + " (none here yet).";
		legend.rows.push_back(std::move(line));
	}
	if (legend.rows.empty()) legend.empty = overlay.words;
	return legend;
}

void draw_overlay_legend(Workspace &workspace, ViewportCanvas &ui, const std::string &path, ViewportKind kind,
                         const MissionOverlayImage &overlay, const std::vector<FoliageDef> &defs, bool open) {
	if (overlay.kind == MissionGroundOverlay::None) return;
	if (!ui.legend(overlay_legend_of(workspace.view(), path, overlay, defs, open))) return;
	io::JsonValue options = io::JsonValue::make_object();
	options.set("legend", io::JsonValue::make_bool(!open));
	workspace.request(request::set_viewport(path, viewport_change(kind, "options", std::move(options))));
}

} // namespace opennova::editor
