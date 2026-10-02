#include <editor/ui/mission_viewport_view.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/json.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_viewport.h>
#include <editor/session/play_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>

namespace opennova::editor {

namespace {

void set_options(Workspace &workspace, const MissionViewport &mission, const MissionViewportOptions &options) {
	workspace.request(request::set_viewport(
			mission.path(), viewport_change(ViewportKind::Mission, "options", mission_options_to_json(options))));
}

// A camera command (frame, top) over the selection, through the viewport's planner.
void camera_command(Workspace &workspace, const MissionViewport &mission, const ViewportContext &context,
		const char *name) {
	CanvasWindowRequests requests(workspace);
	std::string error;
	mission.command(context, name, {}, requests, error);
}

} // namespace

// What the view keeps of its own: the snaps.
struct MissionViewportView::Tools {
	int snap = 2; // kMissionSnaps: 1 m
	int turn = 2; // kMissionTurns: 15 degrees

	void toolbar(Workspace &workspace, const MissionViewport &mission, const ViewportContext &context);
	void show_popup(MissionViewportOptions &options);
	void time_popup(MissionViewportOptions &options, const MissionViewport &mission);
	void notes(const MissionViewport &mission);
};

MissionViewportView::MissionViewportView() : ViewportView(ViewportKind::Mission), tools_(std::make_unique<Tools>()) {}

MissionViewportView::~MissionViewportView() = default;

void MissionViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &mission = static_cast<const MissionViewport &>(viewport);
	tools_->toolbar(workspace, mission, context);
	snap = kMissionSnaps[std::clamp(tools_->snap, 0, 4)];
	context.snap = snap;
	// The notes under the canvas: a line while the picture lacks a file.
	const float notes = mission.missing().empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing();
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y - notes));
	if (notes > 0.0f) tools_->notes(mission);
}

void MissionViewportView::Tools::toolbar(Workspace &workspace, const MissionViewport &mission,
		const ViewportContext &context) {
	MissionViewportOptions options = mission.options();
	const float unit = ImGui::GetFontSize();
	ui_kit::WrapRow row;
	static const char *const kSnapNames[] = { "Free", "1/4 m", "1 m", "5 m", "10 m" };
	row.next(ui_kit::field_width(unit * 5.0f, "Snap"));
	ImGui::SetNextItemWidth(unit * 5.0f);
	ImGui::Combo("Snap", &snap, kSnapNames, IM_ARRAYSIZE(kSnapNames));
	ui_kit::tooltip("A moved or lifted record snaps to this on each of the file's axes, and the arrows "
					"nudge it by this. Hold Alt to move freely.");
	static const char *const kTurnNames[] = { "1 deg", "5 deg", "15 deg", "45 deg", "90 deg" };
	row.next(ui_kit::field_width(unit * 5.0f, "Turn"));
	ImGui::SetNextItemWidth(unit * 5.0f);
	ImGui::Combo("Turn", &turn, kTurnNames, IM_ARRAYSIZE(kTurnNames));
	ui_kit::tooltip("A turned entity's heading snaps to this. Hold Alt to turn freely.");
	row.next(ui_kit::checkbox_width("Stick"));
	ImGui::Checkbox("Stick", &options.stick);
	ui_kit::tooltip("A move keeps each entity's height over the ground; off, its height stands.");
	row.next(ui_kit::button_width("Show"));
	if (ImGui::Button("Show")) ImGui::OpenPopup("show");
	ui_kit::tooltip("What the picture draws, and what the viewport marks over it.");
	if (ImGui::BeginPopup("show")) {
		show_popup(options);
		ImGui::EndPopup();
	}
	row.next(ui_kit::button_width("Time"));
	if (ImGui::Button("Time")) ImGui::OpenPopup("time");
	ui_kit::tooltip("The time of day the picture shows: the mission's start time, or an hour.");
	if (ImGui::BeginPopup("time")) {
		time_popup(options, mission);
		ImGui::EndPopup();
	}
	row.next(ui_kit::button_width("Frame"));
	if (ImGui::Button("Frame")) camera_command(workspace, mission, context, "frame");
	ui_kit::tooltip("Look at the selected records, or at every entity (F, a double click).");
	row.next(ui_kit::button_width("Top"));
	if (ImGui::Button("Top")) camera_command(workspace, mission, context, "top");
	ui_kit::tooltip("Look straight down over the camera's target, north up.");
	// Play mission: the build, then the game started in this mission (the session's own rule for
	// the active document, play_mission_for: Ctrl+F5 is the same request).
	const SessionView &view = workspace.view();
	const std::string played = play_mission_for(view);
	const bool plays = view.project.open && view.activity.play_state == PlayState::Stopped &&
			view.allows(EditorRequestKind::Play) && !played.empty();
	row.next(ui_kit::button_width("Play mission"));
	ImGui::BeginDisabled(!plays);
	if (ImGui::Button("Play mission")) workspace.request(request::play(played));
	ImGui::EndDisabled();
	ui_kit::tooltip(played.empty() ? std::string("Make the mission the active document to start the game in it.")
								  : "Build, then start the game in " + played + " (Ctrl+F5).");
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
	const int start = mission.scene().header().start_time;
	char label[64];
	std::snprintf(label, sizeof(label), "The mission's start time (%02d:%02d)", (start >> 8) & 0xFF, start & 0xFF);
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
