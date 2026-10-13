#include <editor/ui/mission_map_view.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include <imgui.h>

#include <editor/documents/mission_document.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/mission_map.h>
#include <editor/preview/mission_place.h>
#include <editor/preview/mission_options.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/mission_mark_menu.h>
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

// The map's right-click menu: Frame (the record, else the selection), Duplicate (the selection a snap step east, as
// Ctrl+D), Delete (the selection; a stop its marker), then the record's own items (ui/mission_mark_menu).
void map_menu(Workspace &workspace, const MissionMapViewport &model, const ViewportContext &context, const NodeAddress &record,
		const NodeAddress &event) {
	const SessionView &view = workspace.view();
	const Document *records = context.input.document ? records_of(*context.input.document) : nullptr;
	const auto *mission = dynamic_cast<const MissionDocument *>(records);
	if (!mission) {
		ImGui::TextDisabled("No mission.");
		return;
	}
	const bool edits = context.editable();
	const bool selected = view.documents.active == model.path() && !view.documents.selection.empty();
	if (ImGui::MenuItem("Frame", "F")) {
		ViewportCommand frame;
		frame.name = "frame";
		frame.kind = ViewportKind::Map;
		if (record.row) frame.ids = { record.row };
		workspace.request(request::edit_in_viewport(model.path(), std::move(frame)));
	}
	if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, edits && selected)) {
		const double step = model.options().snap > 0.0f ? double(model.options().snap) : 1.0;
		ViewportCommand duplicate;
		duplicate.name = "duplicate";
		duplicate.kind = ViewportKind::Mission;
		duplicate.by = { step, 0.0 };
		workspace.request(request::edit_in_viewport(model.path(), std::move(duplicate)));
	}
	ui_kit::tooltip("A copy of the selection a snap step east (Ctrl+D).");
	if (ImGui::MenuItem("Delete", "Delete", false, edits && selected)) {
		std::vector<Edit> removes;
		for (const NodeAddress &each : mission_view_removals(*mission, view.documents.selection.records)) {
			Edit edit;
			edit.operation = EditOperation::Remove;
			edit.address = each;
			removes.push_back(std::move(edit));
		}
		workspace.request(request::edit_record(model.path(), std::move(removes)));
	}
	ImGui::Separator();
	MissionMarkMenu menu;
	menu.record = record;
	menu.event = event;
	menu.selected = selected;
	draw_mission_mark_items(workspace, *mission, menu, edits);
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
		{ "Links", &options.links,
		  "The selected event's links: what its triggers watch and what its actions act on, ringed, a line from one "
		  "to the other, and a distance trigger's reach seen from above. And on each pin some event names, how many "
		  "events name it." },
	};
	for (const Kind &kind : kinds) {
		row.next(ui_kit::checkbox_width(kind.label));
		ImGui::Checkbox(kind.label, kind.shown);
		ui_kit::tooltip(kind.tip);
	}
	{
		const char *const words[] = { "Team", "Group" };
		const float width = ui_kit::text_width("Group") + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
		row.next(ui_kit::field_width(width, "Colour"));
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("Colour", words[size_t(options.color_by)])) {
			for (size_t i = 0; i < 2; ++i)
				if (ImGui::Selectable(words[i], size_t(options.color_by) == i)) options.color_by = MissionColorBy(i);
			ImGui::EndCombo();
		}
		ui_kit::tooltip("What the pins and the wireframes are coloured by: the team (the game's map colours) or the "
		                "group (the editor's own colours, group 0 none, with a legend of the groups in view).");
	}
	row.next(ui_kit::checkbox_width("Stick"));
	ImGui::Checkbox("Stick", &options.stick);
	ui_kit::tooltip("A moved entity keeps its height over the ground (as the 3D view's Stick).");
	{
		// S25: what the map draws under its marks, the original tools' views of the heights among them.
		constexpr MissionMapBackground kBackgrounds[] = {MissionMapBackground::Colour, MissionMapBackground::Height,
		                                                 MissionMapBackground::Delta, MissionMapBackground::Flatness,
		                                                 MissionMapBackground::None};
		const char *const words[] = {"Colour map", "Height", "Delta", "Flatness", "None"};
		const char *const tips[] = {
				"The commander map's own colours (the terrain's colour map, its water cut as the game's map cuts it).",
				"Nile's Height map: each texel's height in green between the terrain's lowest and highest.",
				"Nile's Delta map: green where a texel's height equals its four neighbours', red where it does not.",
				"The original mission editor's flatness view: black where a texel equals its four neighbours, grey "
				"where it does not.",
				"Nothing under the marks."};
		size_t current = 0;
		for (size_t i = 0; i < std::size(kBackgrounds); ++i)
			if (kBackgrounds[i] == options.background) current = i;
		const float width = ui_kit::text_width("Colour map") + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
		row.next(ui_kit::field_width(width, "Background"));
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("Background", words[current])) {
			for (size_t i = 0; i < std::size(kBackgrounds); ++i) {
				if (ImGui::Selectable(words[i], i == current)) options.background = kBackgrounds[i];
				ui_kit::tooltip(tips[i]);
			}
			ImGui::EndCombo();
		}
		ui_kit::tooltip(std::string("What the map draws under its marks: ") + tips[current]);
		const bool heights = options.background == MissionMapBackground::Height ||
		                     options.background == MissionMapBackground::Delta ||
		                     options.background == MissionMapBackground::Flatness;
		if (!heights) options.water = false;
		row.next(ui_kit::checkbox_width("Water"));
		ImGui::BeginDisabled(!heights);
		ImGui::Checkbox("Water", &options.water);
		ImGui::EndDisabled();
		ui_kit::tooltip(heights ? "The original mission editor's water over the heights' view: each texel under the water "
		                          "level darkened toward blue."
		                        : "Water lays the original mission editor's over the Height, Delta and Flatness views; the "
		                          "colour map shows the commander map's own.");
	}
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
	const SessionView &view = workspace.view();
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y), [&](const CanvasInput &in) {
		// S24: the right button's click: the pin under it selected unless it already is, and the menu over it.
		if (canvas_ui().right_clicked() && !(half() && half()->gesture().pressed())) {
			const std::vector<MissionMapMark> marks = model.marks(in.width, in.height);
			const int mark = pick_mission_map_mark(marks, model.view(in.width, in.height), in.mouse.x, in.mouse.y);
			menu_record_ = mark >= 0 ? marks[size_t(mark)].record : NodeAddress();
			menu_event_ = view.documents.active == model.path() ? mission_menu_event(view.documents.selection.primary)
			                                                    : NodeAddress();
			if (menu_record_.row && !view.documents.selection.holds(menu_record_))
				workspace.request(request::select_record(model.path(), menu_record_));
			ImGui::OpenPopup("mission_map_menu");
		}
		if (ImGui::BeginPopup("mission_map_menu")) {
			map_menu(workspace, model, context, menu_record_, menu_event_);
			ImGui::EndPopup();
		}
	});
}

} // namespace opennova::editor
