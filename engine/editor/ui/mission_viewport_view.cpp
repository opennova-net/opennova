#include <editor/ui/mission_viewport_view.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_viewport.h>
#include <editor/session/play_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>

namespace opennova::editor {

namespace {

void set_options(Workspace &workspace, const MissionViewport &mission, const MissionViewportOptions &options) {
	workspace.request(request::set_viewport(
			mission.path(), viewport_change(ViewportKind::Mission, "options", mission_options_to_json(options))));
}

// A command (frame, top, ground) over the selection, as an EditInViewport the session plans over its
// own context (the viewport's device, the selection): a refusal is the request's outcome, which the
// editor reports, never dropped here.
void viewport_command(Workspace &workspace, const MissionViewport &mission, const char *name) {
	ViewportCommand command;
	command.name = name;
	command.kind = ViewportKind::Mission;
	workspace.request(request::edit_in_viewport(mission.path(), std::move(command)));
}

} // namespace

// What the view keeps of its own: the snaps, the item the Place tool places (0: off) and its name,
// the Place popup's filter.
struct MissionViewportView::Tools {
	int snap = 2; // kMissionSnaps: 1 m
	int turn = 2; // kMissionTurns: 15 degrees
	int64_t place = 0;
	std::string place_name;
	char filter[64] = {};

	void toolbar(Workspace &workspace, const MissionViewport &mission, const ViewportContext &context);
	void show_popup(MissionViewportOptions &options);
	void time_popup(MissionViewportOptions &options, const MissionViewport &mission);
	void place_popup(const SessionView &view);
	void notes(const MissionViewport &mission);
};

MissionViewportView::MissionViewportView() : ViewportView(ViewportKind::Mission), tools_(std::make_unique<Tools>()) {}

MissionViewportView::~MissionViewportView() = default;

void MissionViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &mission = static_cast<const MissionViewport &>(viewport);
	tools_->toolbar(workspace, mission, context);
	snap = kMissionSnaps[std::clamp(tools_->snap, 0, 4)];
	context.snap = snap;
	if (CanvasHalf *canvas_half = half()) {
		auto *canvas = static_cast<MissionCanvas *>(canvas_half);
		canvas->set_place(tools_->place);
		canvas->set_turn(kMissionTurns[std::clamp(tools_->turn, 0, 4)]);
	}
	// The notes under the canvas: a line while the picture lacks a file.
	const float notes = mission.missing().empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing();
	// A Files row let go over the picture: a drop of the file there (a model; the viewport finds its
	// item and refuses another file, naming why).
	const SessionView &view = workspace.view();
	const std::string path = mission.path();
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y - notes),
			[&workspace, &view, path](const CanvasInput &in) {
				if (!ImGui::BeginDragDropTarget()) return;
				const ImGuiPayload *dragged = ImGui::GetDragDropPayload();
				const AssetEntry *entry = nullptr;
				if (dragged && dragged->IsDataType(kFileDragPayload) && dragged->Data && view.project.scan) {
					const std::string file(static_cast<const char *>(dragged->Data));
					for (const AssetEntry &candidate : view.project.scan->entries)
						if (candidate.relative_path == file) entry = &candidate;
				}
				// Only a model is taken: another file is never accepted.
				if (entry && entry->kind == AssetKind::Model && ImGui::AcceptDragDropPayload(kFileDragPayload)) {
					ViewportDrop drop;
					drop.file = entry->logical_name;
					drop.x = in.mouse.x;
					drop.y = in.mouse.y;
					drop.kind = ViewportKind::Mission;
					workspace.request(request::edit_in_viewport(path, std::move(drop)));
				}
				ImGui::EndDragDropTarget();
			});
	if (notes > 0.0f) tools_->notes(mission);
}

void MissionViewportView::Tools::place_popup(const SessionView &view) {
	ui_kit::filter_box("##place_filter", filter, sizeof(filter), "Filter items");
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return;
	// The items the project's catalogs define where a lookup finds them, by name or id.
	const std::string wanted = strutil::to_lower(filter);
	if (ImGui::BeginChild("items", ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 14.0f))) {
		for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::Item)) {
			if (symbol->inert) continue;
			const std::string label = symbol->record + " (" + symbol->display + ")";
			if (!wanted.empty() && strutil::to_lower(label).find(wanted) == std::string::npos) continue;
			const std::optional<int> id = strutil::parse_int(symbol->name);
			if (!id) continue;
			ImGui::PushID(*id);
			if (ImGui::Selectable(ui_kit::fit(label, ImGui::GetContentRegionAvail().x).c_str(), place == *id)) {
				place = *id;
				place_name = symbol->record;
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
}

void MissionViewportView::Tools::toolbar(Workspace &workspace, const MissionViewport &mission,
		const ViewportContext &context) {
	MissionViewportOptions options = mission.options();
	const float unit = ImGui::GetFontSize();
	// The row wraps whole controls; a tab narrower than a control's full form (the Document window
	// beside the Preview in a narrow layout) gets its narrow one: a snap's combo as wide as the room
	// leaves beside its label, then with no label (its tooltip names it), never wider than the line.
	const float line = ImGui::GetContentRegionAvail().x;
	ui_kit::WrapRow row;
	const auto combo = [&](const char *label, int &index, const char *const *names, int count, const char *tip) {
		const float least = unit * 3.0f;
		const bool labelled = ui_kit::field_width(least, label) <= line;
		const float width = std::max(least, std::min(unit * 5.0f, labelled ? line - ui_kit::field_width(0.0f, label) : line));
		const std::string id = labelled ? std::string(label) : "##" + std::string(label);
		row.next(labelled ? ui_kit::field_width(width, label) : width);
		ImGui::SetNextItemWidth(width);
		ImGui::Combo(id.c_str(), &index, names, count);
		ui_kit::tooltip(labelled ? std::string(tip) : std::string(label) + ": " + tip);
	};
	static const char *const kSnapNames[] = { "Free", "1/4 m", "1 m", "5 m", "10 m" };
	combo("Snap", snap, kSnapNames, IM_ARRAYSIZE(kSnapNames),
			"A moved or lifted record snaps to this on each of the file's axes, and the arrows nudge it by this. "
			"Hold Alt to move freely.");
	static const char *const kTurnNames[] = { "1 deg", "5 deg", "15 deg", "45 deg", "90 deg" };
	combo("Turn", turn, kTurnNames, IM_ARRAYSIZE(kTurnNames), "A turned entity's heading snaps to this. Hold Alt to turn freely.");
	row.next(ui_kit::checkbox_width("Stick"));
	ImGui::Checkbox("Stick", &options.stick);
	ui_kit::tooltip("A move keeps each entity's height over the ground; off, its height stands.");
	if (ui_kit::tool(row, "Show", true, "What the picture draws, and what the viewport marks over it.")) ImGui::OpenPopup("show");
	if (ImGui::BeginPopup("show")) {
		show_popup(options);
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Time", true, "The time of day the picture shows: the mission's start time, or an hour."))
		ImGui::OpenPopup("time");
	if (ImGui::BeginPopup("time")) {
		time_popup(options, mission);
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Frame", true, "Look at the selected records, or at every entity (F, a double click)."))
		viewport_command(workspace, mission, "frame");
	if (ui_kit::tool(row, "Top", true, "Look straight down over the camera's target, north up."))
		viewport_command(workspace, mission, "top");
	// The Place tool: an item picked, then each click on the picture places one of it there.
	const bool edits = context.editable();
	if (place == 0) {
		if (ui_kit::tool(row, "Place", edits,
					edits ? "Pick an item, then click the picture to place one of it there (Esc stops)."
						  : context.not_editable()))
			ImGui::OpenPopup("place");
	} else if (ui_kit::tool(row, "Stop placing", true, "Placing " + place_name + " (" + std::to_string(place) +
					") at each click on the picture. Click to stop (or Esc).")) {
		place = 0;
	}
	if (ImGui::BeginPopup("place")) {
		place_popup(workspace.view());
		ImGui::EndPopup();
	}
	if (place != 0 && (!edits || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
								  ImGui::IsKeyPressed(ImGuiKey_Escape, false))))
		place = 0;
	// What the ground under the selected entities is: each set down on it.
	if (ui_kit::tool(row, "Ground", edits && mission.ground(),
				!edits ? context.not_editable()
				: mission.ground() ? std::string("Set each selected entity down on the ground under it.")
								   : std::string("The picture has no ground yet (its terrain is not built).")))
		viewport_command(workspace, mission, "ground");
	// Play mission: the build, then the game started in this mission (the session's own rule for
	// the active document, play_mission_for: Ctrl+F5 is the same request).
	const SessionView &view = workspace.view();
	const std::string played = play_mission_for(view);
	const bool plays = view.project.open && view.activity.play_state == PlayState::Stopped &&
			view.allows(EditorRequestKind::Play) && !played.empty();
	if (ui_kit::tool(row, "Play mission", plays,
				played.empty() ? std::string("Make the mission the active document to start the game in it.")
							   : "Build, then start the game in " + played + " (Ctrl+F5)."))
		workspace.request(request::play(played));
	if (options != mission.options()) set_options(workspace, mission, options);
}

void MissionViewportView::Tools::show_popup(MissionViewportOptions &options) {
	ImGui::TextDisabled("The picture");
	ImGui::Checkbox("Terrain", &options.terrain);
	ImGui::Checkbox("Sky", &options.sky);
	ImGui::Checkbox("Water", &options.water);
	ImGui::Checkbox("Models", &options.models);
	ImGui::Checkbox("Static shadows", &options.shadows);
	ImGui::Separator();
	ImGui::TextDisabled("The marks");
	ImGui::Checkbox("Items", &options.items);
	ImGui::Checkbox("Buildings", &options.buildings);
	ImGui::Checkbox("Markers", &options.markers);
	ImGui::Checkbox("Organics", &options.organics);
	ImGui::Checkbox("Areas", &options.areas);
	ImGui::Checkbox("Paths", &options.paths);
	ImGui::Checkbox("Labels", &options.labels);
	ui_kit::tooltip("A label beside every mark, not only the hovered and the selected ones.");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	float range = options.mark_range;
	if (ImGui::SliderFloat("Mark range", &range, 0.0f, 2000.0f, range <= 0.0f ? "no limit" : "%.0f m",
				ImGuiSliderFlags_AlwaysClamp))
		options.mark_range = range;
	ui_kit::tooltip("How far from the eye a mark is still drawn and picked, metres; 0 for no limit.");
}

void MissionViewportView::Tools::time_popup(MissionViewportOptions &options, const MissionViewport &mission) {
	bool own = options.time < 0.0;
	// The header's start time is hours in 8.8 fixed point (the game shifts it into its 8.24 clock
	// [orig: Game_StartMission @ 0x525371]): 0x0C80 is 12:30.
	const int start = mission.scene().header().start_time;
	char label[64];
	std::snprintf(label, sizeof(label), "The mission's start time (%02d:%02d)", (start >> 8) & 0xFF, ((start & 0xFF) * 60) >> 8);
	if (ImGui::Checkbox(label, &own)) options.time = own ? -1.0 : 12.0;
	ImGui::BeginDisabled(own);
	float hour = options.time < 0.0 ? 12.0f : float(options.time);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	if (ImGui::SliderFloat("Hour", &hour, 0.0f, 24.0f, "%.1f h", ImGuiSliderFlags_AlwaysClamp) && !own)
		options.time = double(hour);
	ImGui::EndDisabled();
}

void MissionViewportView::Tools::notes(const MissionViewport &mission) {
	const std::vector<std::string> &missing = mission.missing();
	std::string line = std::to_string(missing.size()) + (missing.size() == 1 ? " file missing: " : " files missing: ");
	std::string all;
	for (size_t i = 0; i < missing.size(); ++i) {
		if (i < 3) line += (i ? ", " : "") + missing[i];
		all += (i ? "\n" : "") + missing[i];
	}
	if (missing.size() > 3) line += ", ...";
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
	ui_kit::clipped_text(line, all + "\nImport them to see them (Problems).");
	ImGui::PopStyleColor();
}

} // namespace opennova::editor
