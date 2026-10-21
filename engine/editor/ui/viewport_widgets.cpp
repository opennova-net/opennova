#include <editor/ui/viewport_widgets.h>

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include <base/io/tick_rate.h>
#include <formats/trn/charmap_legend.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view/terrain_tools_view.h>
#include <editor/ui/editor_requests.h>
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

std::string tick_seconds(int32_t ticks) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f s", ticks / io::kTickHz);
	return text;
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

void ground_line(const std::string &ground, int surface) {
	if (ground.empty()) {
		ui_kit::quiet_text("Ground: point at the terrain.",
		                   "What the game reads there: its surface class, the footsteps a body plays, the row a round plays "
		                   "and the foliage it grows.");
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

void files_line(const std::vector<std::string> &names, const char *what, const char *then) {
	if (names.empty()) return;
	std::string line = std::to_string(names.size()) + (names.size() == 1 ? " file " : " files ") + what + ": ";
	std::string all;
	for (size_t i = 0; i < names.size(); ++i) {
		if (i < 3) line += (i ? ", " : "") + names[i];
		all += (i ? "\n" : "") + names[i];
	}
	if (names.size() > 3) line += ", ...";
	ui_kit::warning_line(line, all + "\n" + then);
}

void picture_notes(const io::JsonValue &notes, const char *skip) {
	for (const io::JsonValue &note : notes.array)
		if (!skip || note.get_string("code", "") != skip) ui_kit::clipped_text(note.get_string("message", ""));
}

void popup_line(const std::string &text, const std::string &tip, LineTone tone, float ems) {
	// The room: what is left of the line, or `ems` from the popup's left edge where that is wider.
	const float least = ImGui::GetFontSize() * ems - (ImGui::GetCursorPosX() - ImGui::GetCursorStartPos().x);
	const std::string shown = ui_kit::fit(text, std::max(ImGui::GetContentRegionAvail().x, least));
	if (tone != LineTone::Plain)
		ImGui::PushStyleColor(ImGuiCol_Text, tone == LineTone::Warning ? ui_kit::warning_color()
		                                                                : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextUnformatted(shown.c_str());
	if (tone != LineTone::Plain) ImGui::PopStyleColor();
	ui_kit::tooltip(!tip.empty() ? tip : shown != text ? text : std::string());
}

bool listen_tool(ui_kit::WrapRow &row, bool on, const char *hears, const char *detail) {
	return lit_tool(row, "Listen", on, true,
	                on ? std::string("Stop listening.")
	                   : std::string("Hear ") + hears + " where the camera stands, as the game plays it. " + detail);
}

void terrain_pane_tool(Workspace &workspace, ui_kit::WrapRow &row, const std::string &path) {
	const WorkspaceView &shown = workspace.view().workspace;
	const bool pane = shown.document(path).terrain;
	const TerrainTool brushing = shown.terrain_brush.tool;
	const std::string label =
			pane && brushing != TerrainTool::None ? std::string("Terrain: ") + terrain_tool_label(brushing) : std::string("Terrain");
	if (lit_tool(row, label.c_str(), pane, true,
	             pane ? "Hide the Terrain pane (Ctrl+E)." : "Sculpt the ground, paint its maps and lay its tiles here (Ctrl+E)."))
		window_requests::show_terrain(workspace, path, !pane);
}

std::string play_from_here_words(const char *where, const char *key) {
	return std::string("Build, then start the game in this mission with the player on the ground ") + where +
	       ", facing the way the camera looks" + (*key ? std::string(" (") + key + ")" : std::string()) +
	       ". The build's copy of the mission gets the start; its own file is left as it is.";
}

int64_t item_choice(const SessionView &view, const std::vector<int64_t> &items) {
	int64_t picked = -1;
	for (const int64_t item : items) {
		MissionItemFacts facts;
		std::string ignored;
		mission_item_facts(view, item, facts, ignored);
		const std::string label = (facts.name.empty() ? std::string("Item") : facts.name) + " (" + std::to_string(item) + ")";
		if (ImGui::Selectable(label.c_str())) picked = item;
	}
	return picked;
}

} // namespace opennova::editor
