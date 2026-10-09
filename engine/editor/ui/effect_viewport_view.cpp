#include <editor/ui/effect_viewport_view.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <base/io/tick_rate.h>
#include <editor/preview/effect_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

void set_clock(Workspace &workspace, const EffectViewport &model, const char *member, io::JsonValue value) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set(member, std::move(value));
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Effect, "clock", std::move(clock))));
}

// A combo as wide as its widest choice, its arrow and its padding: none is cut.
float combo_width(float widest_text) {
	return widest_text + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// The effect shown among the file's: each by its id, one the catalog spawns another definition for
// dimmed with why.
void effect_picker(Workspace &workspace, const EffectViewport &model, ui_kit::WrapRow &row) {
	const std::vector<EffectViewportEffect> &effects = model.effects();
	if (effects.empty()) return;
	float widest = 0.0f;
	for (const EffectViewportEffect &effect : effects) widest = std::max(widest, ui_kit::text_width(effect.id.c_str()));
	const float width = std::min(combo_width(widest), std::max(ImGui::GetFontSize() * 14.0f, ImGui::GetContentRegionAvail().x));
	row.next(ui_kit::field_width(width, "##effect"));
	ImGui::SetNextItemWidth(width);
	const std::string shown = model.shown_effect().empty() ? effects.front().id : model.shown_effect();
	EffectViewportOptions options = model.options();
	if (ImGui::BeginCombo("##effect", ui_kit::fit(shown, width - ImGui::GetFrameHeight()).c_str())) {
		for (size_t i = 0; i < effects.size(); ++i) {
			const EffectViewportEffect &effect = effects[i];
			const std::string label = effect.id + "###effect" + std::to_string(i);
			if (!effect.registered) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::Selectable(label.c_str(), effect.id == shown)) options.effect = effect.id;
			if (!effect.registered) ImGui::PopStyleColor();
			ui_kit::tooltip("Line " + std::to_string(effect.line) +
			                (effect.registered ? std::string(".")
			                                   : std::string(": an earlier file, or an earlier block of this one, defines "
			                                                 "it first, so the game spawns that one for the name.")));
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip("The effect played: one the file defines. A Go to of an effect's name shows that one.");
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), effect_options_change(options)));
	// Its block in the text: the script device's caret there (a Go to, so back and forward record it).
	const std::string place = model.place_of(shown);
	if (ui_kit::tool(row, "Go to", !place.empty(), place.empty() ? "The effect is not in the text as it stands." : "Its block in the text.")) {
		ReferenceTarget target;
		target.file = model.path();
		target.locator = place;
		target.editable = true;
		window_requests::go_to(workspace, target);
	}
}

} // namespace

EffectViewportView::EffectViewportView() : ViewportView(ViewportKind::Effect) {}

void EffectViewportView::draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) {
	ViewportView::draw_empty(workspace, model, path);
	// An effect that spawns nothing is one of several: another may be picked here.
	if (!model || model->kind() != ViewportKind::Effect) return;
	const auto &effect = static_cast<const EffectViewport &>(*model);
	if (effect.effects().size() < 2) return;
	ui_kit::WrapRow row;
	effect_picker(workspace, effect, row);
}

void EffectViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const EffectViewport &>(viewport);
	ui_kit::WrapRow row;
	effect_picker(workspace, model, row);
	// The preview clock: Run or Pause (Play is the game's), and Replay (tick 0: the effect spawned anew).
	const bool playing = context.input.clock.playing();
	const char *run = playing ? "Pause" : "Run";
	row.next(ui_kit::button_width(run));
	if (ImGui::Button(run)) set_clock(workspace, model, "playing", io::JsonValue::make_bool(!playing));
	ui_kit::tooltip("Run or hold the preview clock: the effect's game ticks.");
	if (ui_kit::tool(row, "Replay", true, "The effect spawned anew: the preview clock at tick 0."))
		set_clock(workspace, model, "ticks", io::json_number(0));
	EffectViewportOptions options = model.options();
	row.next(ui_kit::checkbox_width("Loop"));
	ImGui::Checkbox("Loop", &options.play.loop);
	ui_kit::tooltip("Spawn the effect again as it dies (the editor's aid: the game spawns it once per event).");
	row.next(ui_kit::checkbox_width("Grid"));
	ImGui::Checkbox("Grid", &options.grid);
	ui_kit::tooltip("A metre's squares on the ground the effect spawns on (the editor's aid).");
	// The mission wind every GLOBALWIND particle drifts with, as a mission's header gives it.
	row.next(ui_kit::button_width("Wind"));
	if (ImGui::Button("Wind")) ImGui::OpenPopup("wind");
	ui_kit::tooltip(options.play.wind_speed ? "The mission wind: speed " + std::to_string(options.play.wind_speed) +
	                                                  ", from " + std::to_string(options.play.wind_direction) + " degrees."
	                                        : std::string("No wind: a mission's header sets the wind its GLOBALWIND "
	                                                      "particles drift with."));
	if (ImGui::BeginPopup("wind")) {
		int speed = options.play.wind_speed, direction = options.play.wind_direction;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
		if (ImGui::InputInt("Speed", &speed)) options.play.wind_speed = std::clamp(speed, 0, 1000);
		ui_kit::tooltip("The mission header's wind speed (0: none).");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
		if (ImGui::InputInt("Direction", &direction)) options.play.wind_direction = std::clamp(direction, -360, 360);
		ui_kit::tooltip("The mission header's wind heading, degrees.");
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Frame", true, "The camera on the live particles (F, or a double click on the picture)."))
		workspace.request(request::set_viewport(model.path(), effect_camera_change(model.framed(context.width, context.height))));
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), effect_options_change(options)));
	// The playing cycle: its tick, its age, and what the scene holds.
	const EffectPlayback &play = model.playback();
	size_t particles = 0;
	if (play.scene()) particles = play.scene()->live_counts().particle_count;
	char readout[128];
	std::snprintf(readout, sizeof(readout), "Tick %d, age %d (%.2f s), %zu particles", play.tick(), play.age(),
	              play.age() / io::kTickHz, particles);
	row.next(ui_kit::text_width(readout));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", readout);
	ui_kit::tooltip(play.alive() ? "The effect as the game's effect scene steps it, a game tick at a time."
	                             : "The effect died: Replay spawns it again, or Loop does as it dies.");
	// Where the game resolves the name, when it is not this block; a file the game does not load; the
	// graphics the project lacks.
	const particle::EffectClosure &closure = model.closure();
	if (!model.loaded())
		ui_kit::clipped_text("The game does not load this file (a gore set the project does not pick): it shows what "
		                     "the game spawns for the name.");
	if (closure.found && closure.source != model.path()) {
		ui_kit::WrapRow note;
		const std::string said = "The game spawns " + model.shown_effect() + " as " + closure.source + " defines it, which it reads first.";
		std::string file, locator;
		if (model.spawned_place(file, locator) &&
				ui_kit::tool(note, "Go to it", true, "The definition the game spawns, in " + file + ".")) {
			ReferenceTarget target;
			target.file = file;
			target.locator = locator;
			target.editable = true;
			window_requests::go_to(workspace, target);
		}
		ImGui::SameLine();
		ui_kit::clipped_text(said);
	}
	else if (closure.stock)
		ui_kit::clipped_text("No file the game loads defines " + model.shown_effect() + ": it spawns the stock effect in its place.");
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
