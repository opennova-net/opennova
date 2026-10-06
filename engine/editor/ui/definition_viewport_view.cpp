#include <editor/ui/definition_viewport_view.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <base/io/tick_rate.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/definition_viewport.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

void set_clock(Workspace &workspace, const DefinitionViewport &model, const char *member, io::JsonValue value) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set(member, std::move(value));
	workspace.request(
			request::set_viewport(model.path(), viewport_change(ViewportKind::Definition, "clock", std::move(clock))));
}

std::string seconds(int32_t ticks) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f s", ticks / io::kTickHz);
	return text;
}

const char *state_label(DefinitionState state) {
	switch (state) {
	case DefinitionState::Alive: return "Alive";
	case DefinitionState::Destroying: return "Destroying";
	case DefinitionState::Husk: return "Husk";
	case DefinitionState::HuskFinal: return "Husk final";
	}
	return "Alive";
}

// The options the record's kind takes: an item's State, Enemy, Occupied and a person's SSN; a weapon's view; an
// ammo's Enemy.
void options_row(Workspace &workspace, const DefinitionViewport &model, ui_kit::WrapRow &row) {
	DefinitionViewportOptions options = model.options();
	const std::string &kind = model.drawn().kind;
	if (kind == "item") {
		const float width = ImGui::GetFontSize() * 7.5f;
		row.next(ui_kit::field_width(width, "##state"));
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("##state", state_label(options.state))) {
			for (const DefinitionState state : {DefinitionState::Alive, DefinitionState::Destroying, DefinitionState::Husk,
			                                    DefinitionState::HuskFinal})
				if (ImGui::Selectable(state_label(state), state == options.state)) options.state = state;
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The state the item is drawn in: alive as its mission starts; destroying, its death as the game "
		                "runs it from the clock's start; its husk, the wreck after the death; its final husk, the model "
		                "its death pieces are cut from.");
	}
	if (kind == "item" || kind == "ammo") {
		row.next(ui_kit::checkbox_width("Enemy"));
		ImGui::Checkbox("Enemy", &options.enemy);
		ui_kit::tooltip(kind == "item" ? "As its enemies see it: its graphic_enemy."
		                               : "As the other side sees the round: the enemy tracer item (foe_trcr_type_id).");
	}
	if (kind == "item" && model.particle_slot().controller) {
		row.next(ui_kit::checkbox_width("Occupied"));
		ImGui::Checkbox("Occupied", &options.occupied);
		ui_kit::tooltip("A driver controls it: a drivable item's particle slot attaches only then.");
	}
	if (kind == "item" && !model.person().status.empty()) {
		const float width = ImGui::GetFontSize() * 5.0f;
		row.next(ui_kit::field_width(width, "SSN"));
		ImGui::SetNextItemWidth(width);
		int ssn = options.ssn;
		if (ImGui::InputInt("SSN", &ssn, 1, 8)) options.ssn = std::clamp(ssn, 0, 65535);
		ui_kit::tooltip("The record id the person's spawn warms its animation up by (10 to 490 updates by its bits): "
		                "a definition has no record, so the preview picks one.");
	}
	if (kind == "weapon") {
		const char *labels[] = {"Third person", "First person"};
		const float width = ImGui::GetFontSize() * 8.0f;
		row.next(ui_kit::field_width(width, "##view"));
		ImGui::SetNextItemWidth(width);
		int view = int(options.weapon);
		if (ImGui::Combo("##view", &view, labels, 2)) options.weapon = DefinitionWeaponView(view);
		ui_kit::tooltip("The gun in a soldier's hands (gfx3) or in the player's own view (gfx1).");
	}
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), definition_options_change(options)));
}

// A Go to of the project's file at `file` (project-relative), worded.
void file_link(Workspace &workspace, ui_kit::WrapRow &row, const std::string &file, const std::string &tip) {
	const SessionView &view = workspace.view();
	if (file.empty() || !view.project.scan) return;
	if (ui_kit::tool(row, "Go to", true, tip)) window_requests::go_to(workspace, file_target(*view.project.scan, file));
}

} // namespace

DefinitionViewportView::DefinitionViewportView() : ViewportView(ViewportKind::Definition) {}

void DefinitionViewportView::draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) {
	ViewportView::draw_empty(workspace, model, path);
	// A record that draws nothing in one state or view may in another: its options stay.
	if (!model || model->kind() != ViewportKind::Definition) return;
	const auto &definition = static_cast<const DefinitionViewport &>(*model);
	if (definition.drawn().kind.empty()) return;
	ui_kit::WrapRow row;
	options_row(workspace, definition, row);
}

void DefinitionViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const DefinitionViewport &>(viewport);
	const SessionView &view = workspace.view();
	const PreviewClock &clock = context.input.clock;
	ui_kit::WrapRow row;
	options_row(workspace, model, row);
	// The preview clock: Run or Pause (Play is the game's), and Replay (the state from its start).
	const bool playing = clock.playing();
	const char *run = playing ? "Pause" : "Run";
	row.next(ui_kit::button_width(run));
	if (ImGui::Button(run)) set_clock(workspace, model, "playing", io::JsonValue::make_bool(!playing));
	ui_kit::tooltip("Run or hold the preview clock: the game ticks the effects, the death and its sound play on.");
	if (ui_kit::tool(row, "Replay", true, "The state from its start: the preview clock at tick 0, run.")) {
		io::JsonValue from = io::JsonValue::make_object();
		from.set("ticks", io::json_number(0));
		from.set("playing", io::JsonValue::make_bool(true));
		workspace.request(
				request::set_viewport(model.path(), viewport_change(ViewportKind::Definition, "clock", std::move(from))));
	}
	DefinitionViewportOptions options = model.options();
	row.next(ui_kit::checkbox_width("Grid"));
	ImGui::Checkbox("Grid", &options.grid);
	ui_kit::tooltip("A metre's squares on the ground under the item (the editor's aid).");
	if (model.drawn().kind == "item" && options.state == DefinitionState::Destroying) {
		row.next(ui_kit::checkbox_width("Mute"));
		ImGui::Checkbox("Mute", &options.mute);
		ui_kit::tooltip("The death's sound fires and says what it plays, and nothing is heard.");
	}
	if (ui_kit::tool(row, "Frame", true, "The camera on the model (F, or a double click on the picture)."))
		workspace.request(
				request::set_viewport(model.path(), definition_camera_change(model.framed(context.width, context.height))));
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), definition_options_change(options)));
	// What it draws: the model (a Go to of its file), and the item an ammo's round becomes.
	const DefinitionDrawn &drawn = model.drawn();
	{
		ui_kit::WrapRow line;
		file_link(workspace, line, drawn.file, "Open " + drawn.file + ".");
		ImGui::SameLine();
		ui_kit::clipped_text("Draws " + drawn.name + " (" + drawn.field + ")" +
		                     (drawn.via.empty() ? std::string() : ", the model of " + drawn.via + ", the item its rounds become."));
	}
	if (!drawn.via_file.empty()) {
		ui_kit::WrapRow line;
		if (ui_kit::tool(line, "Go to the item", true, "Open " + drawn.via + " in " + drawn.via_file + ".")) {
			ReferenceTarget target;
			target.file = drawn.via_file;
			target.locator = drawn.via_locator;
			target.editable = true;
			window_requests::go_to(workspace, target);
		}
	}
	if (!model.person().status.empty() && model.person().status == "posed")
		ui_kit::clipped_text("Posed as its spawn poses it: " + model.person().adm + "'s " +
		                     world::infantry_anim_key(model.person().pose.state) + " after " +
		                     std::to_string(model.person().updates) + " warm-up updates.");
	for (const std::string &note : model.notes())
		if (!note.empty()) ui_kit::clipped_text(note, note);
	if (!model.particle_slot().effect.empty() && !model.particle_slot().words.empty())
		ui_kit::clipped_text("Particle slot " + model.particle_slot().effect + ": " + model.particle_slot().words);
	// The effects it spawns, each a Go to of the definition the game spawns for its name.
	const DefinitionEffects &effects = model.effects();
	if (!effects.spawns().empty() &&
	    ImGui::TreeNode("effects", "Effects (%d)", int(effects.spawns().size()))) {
		for (size_t i = 0; i < effects.spawns().size(); ++i) {
			const DefinitionSpawn &spawn = effects.spawns()[i];
			ImGui::PushID(int(i));
			std::string line = spawn.effect + (spawn.point.empty() ? std::string(" at the item") : " at " + spawn.point) +
			                   ", " + seconds(spawn.tick) + " (" + spawn.source + ")";
			const particle::EffectClosure *closure = effects.closure_of(spawn.effect);
			if (closure && !closure->spawns()) line += ": the game spawns nothing for it";
			else if (closure && closure->stock) line += ": no file defines it (the stock effect)";
			const GraphSymbol *symbol = view.findings.graph
			                                    ? view.findings.graph->resolve_symbol(ReferenceKind::Particle, spawn.effect)
			                                    : nullptr;
			if (ImGui::Selectable((ui_kit::fit(line, ImGui::GetContentRegionAvail().x) + "###spawn").c_str()) && symbol &&
			    view.project.scan)
				window_requests::go_to(workspace, symbol_target(*view.project.scan, *symbol));
			ui_kit::tooltip(symbol ? "Go to " + spawn.effect + " in " + symbol->file + "."
			                       : spawn.effect + ": no particle file of the project defines it.");
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
	// The death in order while it plays, each leg lit as the clock passes it.
	if (model.drawn().kind == "item" && options.state == DefinitionState::Destroying && model.plan().swaps &&
	    ImGui::TreeNode("death", "The death, in order (%s)", model.plan().class_words.c_str())) {
		for (const DamageLeg &leg : model.plan().legs) {
			const bool due = clock.ticks() >= leg.tick;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(due ? ImGuiCol_Text : ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", (seconds(leg.tick) + "  " + leg.words).c_str());
			ImGui::PopStyleColor();
			ui_kit::tooltip(leg.cite);
		}
		ImGui::TreePop();
	}
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
