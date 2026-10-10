#include <editor/ui/mission_map_view.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include <imgui.h>

#include <editor/preview/mission_map.h>
#include <editor/preview/mission_options.h>
#include <editor/session/request_factories.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

// A command of the map's (frame, zoom_in, zoom_out) over the selection, as the wire's.
void command(Workspace &workspace, const ViewportModel &model, const char *name) {
	ViewportCommand asked;
	asked.name = name;
	asked.kind = ViewportKind::Map;
	workspace.request(request::edit_in_viewport(model.path(), std::move(asked)));
}

} // namespace

MissionMapViewportView::MissionMapViewportView() : ViewportView(ViewportKind::Map) {}

void MissionMapViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const MissionMapViewport &>(viewport);
	MissionMapOptions options = model.options();
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Frame", true, "Frame the selection on the map, else every entity and area (F)."))
		command(workspace, model, "frame");
	if (ui_kit::tool(row, "+##zoom_in", model.camera().zoom > kMissionMapZoomMin,
				"Zoom in a step, as the commander map's ZOOMIN does (the wheel zooms about the pointer)."))
		command(workspace, model, "zoom_in");
	if (ui_kit::tool(row, "-##zoom_out", model.camera().zoom < kMissionMapZoomMax,
				"Zoom out a step, as the commander map's ZOOMOUT does."))
		command(workspace, model, "zoom_out");
	row.next(ui_kit::checkbox_width("Grid"));
	ImGui::Checkbox("Grid", &options.grid);
	ui_kit::tooltip("The commander map's grid (its GRID toggle): the lines and their labels about the mission's Map "
	                "centerpoint marker, where it has one.");
	row.next(ui_kit::checkbox_width("Text"));
	ImGui::Checkbox("Text", &options.text);
	ui_kit::tooltip("The commander map's text (its TEXT toggle): the names it writes on the map.");
	struct Kind {
		const char *label;
		bool *shown;
		const char *tip;
	};
	const Kind kinds[] = {
		{ "Items", &options.items, "The items' pins (a diamond)." },
		{ "Buildings", &options.buildings, "The buildings' pins (a square)." },
		{ "Markers", &options.markers, "The markers' pins (a cross)." },
		{ "People", &options.organics, "The people's pins (a dot)." },
		{ "Areas", &options.areas, "The area triggers' boxes." },
		{ "Paths", &options.paths, "The paths through their stops, the player's route numbered." },
		{ "Labels", &options.labels, "Every pin's name, beside the hovered and the selected ones' always." },
	};
	for (const Kind &kind : kinds) {
		row.next(ui_kit::checkbox_width(kind.label));
		ImGui::Checkbox(kind.label, kind.shown);
		ui_kit::tooltip(kind.tip);
	}
	row.next(ui_kit::checkbox_width("Stick"));
	ImGui::Checkbox("Stick", &options.stick);
	ui_kit::tooltip("A moved entity keeps its height over the ground (as the 3D view's Stick).");
	{
		int current = 0;
		for (size_t i = 0; i < std::size(kMissionSnaps); ++i)
			if (kMissionSnaps[i] == options.snap) current = int(i);
		const auto words = [](float snap) {
			char text[24];
			if (snap <= 0.0f) return std::string("Free");
			std::snprintf(text, sizeof(text), "%g m", double(snap));
			return std::string(text);
		};
		const float width = ui_kit::text_width("0.25 m") + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
		row.next(ui_kit::field_width(width, "Snap"));
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("Snap", words(options.snap).c_str())) {
			for (size_t i = 0; i < std::size(kMissionSnaps); ++i)
				if (ImGui::Selectable(words(kMissionSnaps[i]).c_str(), int(i) == current)) options.snap = kMissionSnaps[i];
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The grid a moved record's place snaps to (Ctrl held: free).");
	}
	if (options != model.options())
		workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Map, "options",
		                                                                      mission_map_options_to_json(options))));
	snap = options.snap;
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
