#include <editor/ui/mission_board_live.h>

#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <base/io/tick_rate.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

namespace {

const ImVec4 kTrue(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kFalse(0.95f, 0.45f, 0.40f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.35f, 1.0f);

// A countdown in the game's units (64 a step, one step a quarter pass of 64 ticks) as the seconds it lasts.
std::string seconds_of(int64_t countdown) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1f s", double(countdown) / io::kTickHz);
	return text;
}

int64_t steps_of(int64_t countdown) { return (countdown + bms::kEventStepTicks - 1) / bms::kEventStepTicks; }

// A dot a line high, coloured, where the cursor stands.
void dot(const ImVec4 &colour, bool filled) {
	const float size = ImGui::GetTextLineHeight();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const ImVec2 centre(at.x + size * 0.5f, at.y + size * 0.5f);
	ImDrawList *draw = ImGui::GetWindowDrawList();
	if (filled) draw->AddCircleFilled(centre, size * 0.3f, ImGui::GetColorU32(colour));
	else draw->AddCircle(centre, size * 0.3f, ImGui::GetColorU32(colour));
	ImGui::Dummy(ImVec2(size, size));
}

// The event's state in words: what the game holds of it now.
std::string state_words(const LiveEvent &event) {
	if (event.activate_countdown > 0)
		return "Waiting: acts in " + seconds_of(event.activate_countdown) + " (" + std::to_string(steps_of(event.activate_countdown)) +
		       " steps)";
	if (event.repeat_countdown > 0)
		return std::string(event.fired ? "Fired; " : "") + "checked again in " + seconds_of(event.repeat_countdown);
	if (event.fired) return "Fired";
	if (event.active) return "Latched";
	return "Not fired";
}

void heading(Workspace &workspace, const LiveLogic &live, const LiveEvent &event) {
	const std::string words = state_words(event);
	const ImVec4 colour = event.fired ? kTrue : event.activate_countdown > 0 || event.active ? kWarn
	                                                                                     : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	ImGui::TextColored(colour, "%s", words.c_str());
	std::string tip = "In the running game, at tick " + std::to_string(live.tick) + ": " + words + ".";
	if (event.last_checked_tick >= 0)
		tip += "\nIts triggers were last checked at tick " + std::to_string(event.last_checked_tick) + ", and " +
		       (event.chain == 1 ? "held." : event.chain == 0 ? "did not hold." : "were not run.");
	else
		tip += "\nIts triggers have not been checked yet.";
	ui_kit::tooltip(tip);
	if (live.stale) {
		ImGui::SameLine();
		ImGui::TextColored(kWarn, "stale");
		ui_kit::tooltip("Played at revision " + std::to_string(live.played_revision) +
		                ": the mission was edited since, and the game runs it as it was then. Play again to see the edits run.");
	}
	ImGui::SameLine();
	if (event.fire_pending) {
		ImGui::TextDisabled("fire asked");
		ui_kit::tooltip("Its triggers read true at its next check, its delay, latch and repeat then its own.");
		return;
	}
	if (ImGui::SmallButton("Fire")) workspace.request(request::play_debug_fire(int64_t(event.index)));
	ui_kit::tooltip("Asks the running game to treat this event's triggers as holding at its next check: its delay, "
	                "latch and repeat then run as its own. The game refuses a start or end event, and a fire-once "
	                "event that already fired.");
}

void trigger(const LiveLogic &live, const LiveEvent &event, size_t index) {
	const int8_t truth = index < event.truth.size() ? event.truth[index] : int8_t(-1);
	dot(truth == 1 ? kTrue : truth == 0 ? kFalse : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), truth >= 0);
	ui_kit::tooltip(truth < 0 ? std::string("Not checked yet in the running game.")
	                          : std::string(truth == 1 ? "Held" : "Did not hold") + " when the game last checked it (tick " +
	                                    std::to_string(event.last_checked_tick) + "; now " + std::to_string(live.tick) + ").");
}

} // namespace

void draw_card_live(Workspace &workspace, const std::string &, const LiveLogic &live, const CardLiveSpot &spot) {
	const LiveEvent *event = live.event(spot.event);
	if (!event) return;
	ImGui::PushID("live");
	if (spot.trigger < 0) heading(workspace, live, *event);
	else trigger(live, *event, size_t(spot.trigger));
	ImGui::PopID();
}

} // namespace opennova::editor
