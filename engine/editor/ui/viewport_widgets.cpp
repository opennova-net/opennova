#include <editor/ui/viewport_widgets.h>

#include <imgui.h>

#include <formats/trn/charmap_legend.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

float combo_width(float widest_text) {
	return widest_text + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void set_viewport_member(Workspace &workspace, const ViewportModel &viewport, const char *member, io::JsonValue value) {
	workspace.request(request::set_viewport(viewport.path(), viewport_change(viewport.kind(), member, std::move(value))));
}

void set_clock(Workspace &workspace, const ViewportModel &viewport, const char *member, io::JsonValue value) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set(member, std::move(value));
	set_viewport_member(workspace, viewport, "clock", std::move(clock));
}

void clock_buttons(Workspace &workspace, ui_kit::WrapRow &row, const ViewportModel &viewport, const PreviewClock &clock,
		const char *run_tip, const char *replay_tip, bool replay_runs) {
	const bool playing = clock.playing();
	const char *run = playing ? "Pause" : "Run";
	row.next(ui_kit::button_width(run));
	if (ImGui::Button(run)) set_clock(workspace, viewport, "playing", io::JsonValue::make_bool(!playing));
	ui_kit::tooltip(run_tip);
	if (replay_tip && ui_kit::tool(row, "Replay", true, replay_tip)) replay_clock(workspace, viewport, replay_runs);
}

void replay_clock(Workspace &workspace, const ViewportModel &viewport, bool runs) {
	io::JsonValue from = io::JsonValue::make_object();
	from.set("ticks", io::json_number(0));
	if (runs) from.set("playing", io::JsonValue::make_bool(true));
	set_viewport_member(workspace, viewport, "clock", std::move(from));
}

bool lit_tool(ui_kit::WrapRow &row, const char *label, bool active, bool enabled, const std::string &tip) {
	if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	const bool pressed = ui_kit::tool(row, label, enabled, tip);
	if (active) ImGui::PopStyleColor();
	return pressed;
}

bool show_layers(ui_kit::WrapRow &row, const char *tip, std::initializer_list<std::pair<const char *, bool *>> layers) {
	bool changed = false;
	if (ui_kit::tool_popup(row, "Show", "show", true, tip)) {
		for (const auto &[label, value] : layers) changed = ImGui::Checkbox(label, value) || changed;
		ImGui::EndPopup();
	}
	return changed;
}

void ground_line(const std::string &ground, int surface, const char *reads) {
	if (ground.empty()) {
		ui_kit::quiet_text("Ground: point at the terrain.", reads);
		return;
	}
	// The class's colour in the char map legend, the colour a surface map paints it (formats/trn/charmap_legend.h).
	if (surface >= 0 && surface < kCharmapLegendCount) {
		const CharmapLegendColour &c = kCharmapLegend[surface];
		const float side = ImGui::GetTextLineHeight();
		// Its own id: a canvas's surface is "##surface" in the same window (ImGui's conflicting-id warning).
		ImGui::ColorButton("##ground_class", ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
		                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
		                   ImVec2(side, side));
		ImGui::SameLine();
	}
	ui_kit::clipped_text("Ground: " + ground);
}

void missing_files_line(const std::vector<std::string> &missing) {
	if (missing.empty()) return;
	std::string line = std::to_string(missing.size()) + (missing.size() == 1 ? " file missing: " : " files missing: ");
	std::string all;
	for (size_t i = 0; i < missing.size(); ++i) {
		if (i < 3) line += (i ? ", " : "") + missing[i];
		all += (i ? "\n" : "") + missing[i];
	}
	if (missing.size() > 3) line += ", ...";
	ui_kit::warning_line(line, all + "\nImport them to see them (Problems).");
}

} // namespace opennova::editor
