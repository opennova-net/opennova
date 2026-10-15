#include <editor/ui/viewport_widgets.h>

#include <imgui.h>

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
	if (!replay_tip || !ui_kit::tool(row, "Replay", true, replay_tip)) return;
	io::JsonValue from = io::JsonValue::make_object();
	from.set("ticks", io::json_number(0));
	if (replay_runs) from.set("playing", io::JsonValue::make_bool(true));
	set_viewport_member(workspace, viewport, "clock", std::move(from));
}

bool lit_tool(ui_kit::WrapRow &row, const char *label, bool active, bool enabled, const std::string &tip) {
	if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	const bool pressed = ui_kit::tool(row, label, enabled, tip);
	if (active) ImGui::PopStyleColor();
	return pressed;
}

} // namespace opennova::editor
