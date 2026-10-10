#include <editor/ui/environment_viewport_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/tick_rate.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>
#include <formats/env/env.h>
#include <formats/env/tod_clock.h>
#include <runtime/environment/precipitation.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

void set_viewport(Workspace &workspace, const EnvironmentViewport &model, const char *member, JsonValue value) {
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Environment, member, std::move(value))));
}

void set_clock(Workspace &workspace, const EnvironmentViewport &model, const char *member, JsonValue value) {
	JsonValue clock = JsonValue::make_object();
	clock.set(member, std::move(value));
	set_viewport(workspace, model, "clock", std::move(clock));
}

// One weather command as a script issues it: issued each time, whatever the last one was.
void issue(Workspace &workspace, const EnvironmentViewport &model, const char *name, const EnvironmentWeatherCommand &command) {
	JsonValue value = JsonValue::make_object();
	value.set("percent", io::json_number(command.percent));
	value.set("seconds", io::json_number(command.seconds));
	JsonValue options = JsonValue::make_object();
	options.set(name, std::move(value));
	set_viewport(workspace, model, "options", std::move(options));
}

std::string day_words(int seconds) {
	if (seconds <= 0) return "The game's rate";
	if (seconds % 60 == 0) return "A day in " + std::to_string(seconds / 60) + " min";
	return "A day in " + std::to_string(seconds) + " s";
}

// What the game's rate is here, in words.
std::string rate_words(const EnvironmentViewport &model) {
	if (model.advance_per_tick() == 0) return "the clock stands (a day length of 0)";
	const double seconds = double(env::kTodDayFixed24) / double(model.advance_per_tick()) / io::kTickHz;
	char text[64];
	if (seconds >= 3600.0) std::snprintf(text, sizeof(text), "a day of %.1f h", seconds / 3600.0);
	else std::snprintf(text, sizeof(text), "a day of %.0f min", seconds / 60.0);
	return text;
}

const char *rate_source(const EnvironmentViewport &model) {
	switch (model.rate_from()) {
	case EnvironmentClockFrom::Option: return "the day length chosen here";
	case EnvironmentClockFrom::Mission: return "the mission header's day length";
	case EnvironmentClockFrom::Environment: return "the file's tod_rate (a mission's header sets its own)";
	case EnvironmentClockFrom::Default: break;
	}
	return "the engine's default (no mission and no tod_rate)";
}

// The day: 24 hours across the room, a mark at each keyframe, the segment the clock is in, the time now.
// A click or a drag runs the clock from the hour under the pointer.
void day_track(Workspace &workspace, const EnvironmentViewport &model) {
	const float width = std::max(ImGui::GetContentRegionAvail().x, 48.0f);
	const float height = ImGui::GetFrameHeight() * 1.5f;
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##day", ImVec2(width, height));
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	ImDrawList *draw = ImGui::GetWindowDrawList();
	const auto x_of = [&](double hours) { return at.x + float(hours / 24.0) * width; };
	const float bar_top = at.y + height * 0.35f;
	draw->AddRectFilled(ImVec2(at.x, bar_top), ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg));
	// The hours, every three.
	for (int hour = 0; hour <= 24; hour += 3) {
		const float x = x_of(hour);
		draw->AddLine(ImVec2(x, bar_top), ImVec2(x, bar_top + height * 0.15f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
		if (hour < 24) {
			char label[8];
			std::snprintf(label, sizeof(label), "%02d", hour);
			draw->AddText(ImVec2(x + 2.0f, bar_top), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
		}
	}
	// The keyframes and the segment the clock is in.
	const std::vector<int> times = model.keyframe_times();
	const EnvironmentViewport::Segment segment = model.segment();
	const auto hours_of = [](int hhmm) { return double(env::hhmm_to_hours_fp(float(hhmm))) / 65536.0; };
	if (segment.from >= 0 && times.size() > 1) {
		const double a = hours_of(times[size_t(segment.from)]), b = hours_of(times[size_t(segment.to)]);
		const ImU32 colour = ImGui::GetColorU32(ImGuiCol_PlotHistogram, 0.35f);
		const float bottom = at.y + height;
		if (b > a) draw->AddRectFilled(ImVec2(x_of(a), bar_top), ImVec2(x_of(b), bottom), colour);
		else {
			draw->AddRectFilled(ImVec2(x_of(a), bar_top), ImVec2(x_of(24.0), bottom), colour);
			draw->AddRectFilled(ImVec2(x_of(0.0), bar_top), ImVec2(x_of(b), bottom), colour);
		}
	}
	for (size_t i = 0; i < times.size(); ++i) {
		const float x = x_of(hours_of(times[i]));
		const float size = ImGui::GetFontSize() * 0.3f;
		draw->AddTriangleFilled(ImVec2(x - size, at.y), ImVec2(x + size, at.y), ImVec2(x, at.y + size * 1.6f),
				ImGui::GetColorU32(ImGuiCol_PlotHistogram));
		draw->AddLine(ImVec2(x, at.y + size), ImVec2(x, at.y + height), ImGui::GetColorU32(ImGuiCol_PlotHistogram, 0.8f));
	}
	// The time now.
	const float now = x_of(model.hours());
	draw->AddLine(ImVec2(now, at.y), ImVec2(now, at.y + height), ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
	const double pointed = std::clamp(double(ImGui::GetIO().MousePos.x - at.x) / double(width) * 24.0, 0.0, 24.0 - 1.0 / 60.0);
	if (hovered && !active) {
		// The whole minute a click sets (below).
		std::string tip = environment_clock_words(std::floor(pointed * 60.0) / 60.0) + ": click or drag to run the clock from here.";
		for (size_t i = 0; i < times.size(); ++i)
			if (std::fabs(x_of(hours_of(times[i])) - ImGui::GetIO().MousePos.x) < ImGui::GetFontSize() * 0.4f)
				tip += "\nThe keyframe at " + environment_clock_words(hours_of(times[i])) +
				       ": the clock reaches its colours there, then blends toward the next.";
		if (times.empty()) tip += "\nThe file has no keyframe: the game holds its colours all day.";
		ui_kit::tooltip(tip);
	}
	// A whole minute: the scrub sends a change only as the minute under the pointer moves.
	if (active) {
		const double minute = std::floor(pointed * 60.0) / 60.0;
		if (std::fabs(minute - model.options().time) > 1e-6) {
			JsonValue options = JsonValue::make_object();
			options.set("time", io::json_number(minute));
			set_viewport(workspace, model, "options", std::move(options));
		}
	}
}

// One weather control: its percent and seconds, the button that issues it as a script would, and where it
// stands.
void weather_control(Workspace &workspace, const EnvironmentViewport &model, ui_kit::WrapRow &row, const char *name,
		const char *label, EnvironmentWeatherCommand &command, int32_t current_q16, int32_t target_q16) {
	ImGui::PushID(name);
	const float slider = ImGui::GetFontSize() * 7.0f;
	row.next(slider);
	ImGui::SetNextItemWidth(slider);
	ImGui::SliderInt("##percent", &command.percent, 0, 100, (std::string(label) + " %d%%").c_str());
	ui_kit::tooltip(std::string("The percent the next ") + name + " command sets.");
	const float seconds = ImGui::GetFontSize() * 4.5f;
	row.next(ui_kit::field_width(seconds, "s"));
	ImGui::SetNextItemWidth(seconds);
	if (ImGui::InputInt("s", &command.seconds, 0, 0)) command.seconds = std::clamp(command.seconds, 0, kEnvironmentCommandSecondsMost);
	ui_kit::tooltip("The seconds the game takes to get there (0: the next tick).");
	char button[48];
	std::snprintf(button, sizeof(button), "%s(%d, %d)", name, command.percent, command.seconds);
	if (ui_kit::tool(row, button, true,
	                 std::string("Issue it as a mission's script does: the game's ") + name +
	                         " command, the percent reached over the seconds on the game's ticks."))
		issue(workspace, model, name, command);
	char state[64];
	std::snprintf(state, sizeof(state), "%.0f%% (to %.0f%%)", double(current_q16) * 100.0 / 65536.0,
	              double(target_q16) * 100.0 / 65536.0);
	row.next(ui_kit::text_width(state));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", state);
	ImGui::PopID();
}

} // namespace

EnvironmentViewportView::EnvironmentViewportView() : ViewportView(ViewportKind::Environment) {}

void EnvironmentViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const EnvironmentViewport &>(viewport);
	const EnvironmentViewportOptions &options = model.options();
	ui_kit::WrapRow row;
	// The preview clock: Run or Pause (Play is the game's).
	const bool playing = context.input.clock.playing();
	const char *run = playing ? "Pause" : "Run";
	row.next(ui_kit::button_width(run));
	if (ImGui::Button(run)) set_clock(workspace, model, "playing", JsonValue::make_bool(!playing));
	ui_kit::tooltip("Run or hold the preview clock: the time of day runs on its game ticks.");
	const std::string now = environment_clock_words(model.hours());
	row.next(ui_kit::text_width(now.c_str()));
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(now.c_str());
	ui_kit::tooltip(std::string(model.weather().is_night_phase() ? "Night: the moon lights it." : "Day: the sun lights it.") +
	                " The game's clock, from " + environment_clock_words(double(model.start_fixed24()) / 16777216.0) +
	                " at " + rate_words(model) + ".");
	// The day's length.
	const float speed_width = ImGui::GetFontSize() * 11.0f;
	row.next(speed_width);
	ImGui::SetNextItemWidth(speed_width);
	if (ImGui::BeginCombo("##day", day_words(options.day_seconds).c_str())) {
		for (const int seconds : kEnvironmentDaySeconds)
			if (ImGui::Selectable(day_words(seconds).c_str(), seconds == options.day_seconds)) {
				JsonValue change = JsonValue::make_object();
				change.set("day_seconds", io::json_number(seconds));
				set_viewport(workspace, model, "options", std::move(change));
			}
		ImGui::EndCombo();
	}
	ui_kit::tooltip(std::string("The game's rate here: ") + rate_words(model) + ", from " + rate_source(model) +
	                ". A shorter day is the editor's aid.");
	if (ui_kit::tool(row, "Start", true, "The clock back to the time the game starts it on (the mission header's start, else "
	                                     "the file's curtime)."))
		workspace.request(request::set_viewport(model.path(),
				R"({"kind": "environment", "options": {"time": null}, "clock": {"ticks": 0}})"));
	// The mission it is drawn over.
	const EnvironmentUses &uses = model.uses();
	const EnvironmentMissionUse *drawn = model.mission();
	if (uses.missions.size() > 1) {
		const float width = ImGui::GetFontSize() * 10.0f;
		row.next(width);
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("##mission", drawn ? io::utf8_file_name(drawn->mission).c_str() : "")) {
			for (const EnvironmentMissionUse &use : uses.missions)
				if (ImGui::Selectable(io::utf8_file_name(use.mission).c_str(), drawn && use.mission == drawn->mission)) {
					JsonValue change = JsonValue::make_object();
					change.set("mission", io::json_string(use.mission));
					set_viewport(workspace, model, "options", std::move(change));
				}
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The mission whose terrain, tiles and clock the sky is drawn with.");
	} else if (drawn) {
		const std::string over = "Over " + io::utf8_file_name(drawn->mission);
		row.next(ui_kit::text_width(over.c_str()));
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", over.c_str());
		ui_kit::tooltip("The terrain, tiles and clock of the mission that runs on it.");
	}
	if (drawn) {
		const std::vector<EnvironmentOverride> overrides = environment_overrides(*drawn);
		bool header = options.header;
		row.next(ui_kit::checkbox_width("Mission header"));
		ImGui::BeginDisabled(overrides.empty());
		if (ImGui::Checkbox("Mission header", &header)) {
			JsonValue change = JsonValue::make_object();
			change.set("header", JsonValue::make_bool(header));
			set_viewport(workspace, model, "options", std::move(change));
		}
		ImGui::EndDisabled();
		std::string tip = overrides.empty() ? "Its header sets nothing over this environment." : "Its header sets, as the game loads the two:";
		for (const EnvironmentOverride &each : overrides) tip += "\n  " + each.words;
		ui_kit::tooltip(tip);
	}
	// The layers.
	row.next(ui_kit::button_width("Show"));
	if (ImGui::Button("Show")) ImGui::OpenPopup("show");
	if (ImGui::BeginPopup("show")) {
		bool terrain = options.terrain, water = options.water;
		const bool changed = ImGui::Checkbox("Terrain", &terrain) | ImGui::Checkbox("Water", &water);
		if (changed) {
			JsonValue show = JsonValue::make_object();
			show.set("terrain", JsonValue::make_bool(terrain));
			show.set("water", JsonValue::make_bool(water));
			JsonValue change = JsonValue::make_object();
			change.set("show", std::move(show));
			set_viewport(workspace, model, "options", std::move(change));
		}
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Frame", true, "The eye back on the ground at the terrain's middle (F, or a double click)."))
		workspace.request(request::set_viewport(model.path(), environment_camera_change(model.framed(context.width, context.height))));
	// The day.
	day_track(workspace, model);
	// The weather a script sets.
	ui_kit::WrapRow weather;
	const env::EnvScalarChannels &channels = model.weather().core.scalar_channels;
	weather_control(workspace, model, weather, "rain", "Rain", rain_, channels.rain_pct_fp, channels.rain_pct_target_fp);
	weather_control(workspace, model, weather, "overcast", "Overcast", overcast_, channels.overcast_fp,
	                channels.overcast_target_fp);
	if (ui_kit::tool(weather, "Clear", true, "rain(0, 0) and overcast(0, 0): the sky clear at the next tick."))
		workspace.request(request::set_viewport(model.path(),
				R"({"kind": "environment", "options": {"rain": {"percent": 0, "seconds": 0}, "overcast": {"percent": 0, "seconds": 0}}})"));
	// Listen (S23 C): the rain's loops and the lightning's thunder heard at the camera, as the game plays them.
	const MissionListenOptions &listen = options.listen;
	if (listen.on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	const bool toggled = ui_kit::tool(weather, "Listen", true,
			listen.on ? std::string("Stop listening.")
			          : std::string("Hear the weather where the camera stands, as the game plays it: the rain's two loops "
			                        "beside the listener while it rains, the thunder of the lightning."));
	if (listen.on) ImGui::PopStyleColor();
	if (toggled) {
		MissionListenOptions next = listen;
		next.on = !next.on;
		JsonValue change = JsonValue::make_object();
		change.set("listen", mission_listen_options_to_json(next));
		set_viewport(workspace, model, "options", std::move(change));
	}
	if (listen.on) {
		const float width = ImGui::GetFontSize() * 5.0f;
		weather.next(ui_kit::field_width(width, "Volume"));
		ImGui::SetNextItemWidth(width);
		float volume = listen.volume;
		if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp) && volume != listen.volume) {
			MissionListenOptions next = listen;
			next.volume = volume;
			JsonValue change = JsonValue::make_object();
			change.set("listen", mission_listen_options_to_json(next));
			set_viewport(workspace, model, "options", std::move(change));
		}
		ui_kit::tooltip("The master volume of everything Listen plays (the editor's, not the game's).");
	}
	if (model.weather().raining()) {
		char drops[48];
		std::snprintf(drops, sizeof(drops), "%d drops", env::PrecipitationField::active_count(channels.rain_pct_fp));
		weather.next(ui_kit::text_width(drops));
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", drops);
		ui_kit::tooltip("The drops the game draws at this rain, of its 3072.");
	}
	// What the picture says of itself: no mission, a terrain the project lacks, a standing clock.
	const JsonValue notes = model.notes_json(context.input);
	for (const JsonValue &note : notes.array) ui_kit::clipped_text(note.get_string("message", ""));
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
