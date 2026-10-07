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
#include <runtime/world/ammo_table.h>

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

// A weapon's firing options (DI-22): the first person's Eye and character, the third person's shooter, the
// range's surface, distance and target.
void weapon_options(const DefinitionViewport &model, DefinitionViewportOptions &options, ui_kit::WrapRow &row) {
	DefinitionFireOptions &fire = options.fire;
	if (options.weapon == DefinitionWeaponView::First) {
		row.next(ui_kit::checkbox_width("Eye"));
		ImGui::Checkbox("Eye", &fire.eye);
		ui_kit::tooltip("Seen from the first-person eye, where the game's camera stands the view model, through the "
		                "weapon's renderfov; off, the camera orbits the gun and the arms.");
		const std::vector<FirstPersonCharacter> &characters = model.weapon().first_person().characters();
		if (!characters.empty()) {
			const FirstPersonCharacter *who = model.weapon().first_person().character();
			const float width = ImGui::GetFontSize() * 9.0f;
			row.next(ui_kit::field_width(width, "##character"));
			ImGui::SetNextItemWidth(width);
			if (ImGui::BeginCombo("##character", who ? who->words.c_str() : "No character")) {
				for (const FirstPersonCharacter &character : characters)
					if (ImGui::Selectable(character.words.c_str(), who == &character)) fire.character = character.id;
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The character whose arms draw (Avatars.def): a fresh profile's is the good side's first.");
		}
	} else {
		const char *shooters[] = {"As a soldier's", "As a player's"};
		const float width = ImGui::GetFontSize() * 8.0f;
		row.next(ui_kit::field_width(width, "##shooter"));
		ImGui::SetNextItemWidth(width);
		int shooter = fire.shooter == WeaponShotView::Player ? 1 : 0;
		if (ImGui::Combo("##shooter", &shooter, shooters, 2))
			fire.shooter = shooter == 1 ? WeaponShotView::Player : WeaponShotView::Soldier;
		ui_kit::tooltip("How another sees the shot: a soldier's as the ammo's ai_launch and ai_launcheffect; another "
		                "player's as the weapon's FIRE and RECOIL rows at the gun.");
	}
	row.next(ui_kit::checkbox_width("Target"));
	ImGui::Checkbox("Target", &fire.target.shown);
	ui_kit::tooltip("A wall down the line of fire, whose face plays the ammo's impact row for its surface; off, the "
	                "rounds fly on until they age out.");
	if (fire.target.shown) {
		const float width = ImGui::GetFontSize() * 7.0f;
		row.next(ui_kit::field_width(width, "##surface"));
		ImGui::SetNextItemWidth(width);
		const int tag = std::clamp(fire.target.tag, kWeaponRangeFirstTag, world::kImpactEffectTagCount - 1);
		if (ImGui::BeginCombo("##surface", world::kImpactEffectTagWords[tag])) {
			for (int i = kWeaponRangeFirstTag; i < world::kImpactEffectTagCount; ++i)
				if (ImGui::Selectable(world::kImpactEffectTagWords[i], i == tag)) fire.target.tag = i;
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The target's surface: a round striking a face of material b plays the ammo's row b + 4.");
		const float range_width = ImGui::GetFontSize() * 6.0f;
		row.next(ui_kit::field_width(range_width, "##range"));
		ImGui::SetNextItemWidth(range_width);
		float range = fire.target.range;
		if (ImGui::InputFloat("##range", &range, 5.0f, 25.0f, "%.0f m"))
			fire.target.range = std::clamp(range, kWeaponRangeNearest, kWeaponRangeFarthest);
		ui_kit::tooltip("The target's distance down the line of fire, metres.");
	}
}

// A weapon's gesture (DI-22): one on the clock's tick, the clock run; "clear" none.
void gesture(Workspace &workspace, const DefinitionViewport &model, const char *name) {
	io::JsonValue change = io::JsonValue::make_object();
	change.set("kind", io::json_string(viewport_kind_token(ViewportKind::Definition)));
	if (std::string(name) == "clear") {
		change.set("gestures", io::JsonValue::make_array());
	} else {
		change.set("gesture", io::json_string(name));
		io::JsonValue clock = io::JsonValue::make_object();
		clock.set("playing", io::JsonValue::make_bool(true));
		change.set("clock", std::move(clock));
	}
	workspace.request(request::set_viewport(model.path(), io::json_write(change)));
}

// The fire key held at the clock's tick: the last hold or release at or before it.
bool holding_at(const DefinitionViewport &model, int32_t tick) {
	bool held = false;
	for (const WeaponGestureAt &at : model.weapon().range().gestures()) {
		if (at.tick > tick) break;
		if (at.gesture == WeaponGesture::Hold) held = true;
		if (at.gesture == WeaponGesture::Release) held = false;
	}
	return held;
}

// The weapon's gestures and what it does (DI-22): Fire, Hold fire, Reload, Scope, Switch, Clear; the weapon as the
// game holds it; the last things the run did, newest first.
void weapon_row(Workspace &workspace, const DefinitionViewport &model, const PreviewClock &clock) {
	const DefinitionWeapon &weapon = model.weapon();
	const WeaponRange &range = weapon.range();
	{
		ui_kit::WrapRow row;
		const bool ready = range.ready();
		if (ui_kit::tool(row, "Fire", ready, "The fire key pressed and let go on the clock's tick, the clock run."))
			gesture(workspace, model, "fire");
		const bool held = holding_at(model, clock.ticks());
		if (ui_kit::tool(row, held ? "Release" : "Hold fire", ready,
		                 held ? "Let the fire key go." : "The fire key held from the clock's tick: an auto weapon fires on."))
			gesture(workspace, model, held ? "release" : "hold");
		if (ui_kit::tool(row, "Reload", ready, "The reload key: the game refuses it on a full clip or an empty reserve."))
			gesture(workspace, model, "reload");
		if (ui_kit::tool(row, "Scope", ready, "The scope toggle: a scoped or sighted weapon raises or lowers its sight."))
			gesture(workspace, model, "scope");
		if (ui_kit::tool(row, "Switch", ready, "The weapon put away and drawn again (SWITCHFROM, then SWITCHTO)."))
			gesture(workspace, model, "switch");
		if (ui_kit::tool(row, "Clear", ready && !range.gestures().empty(),
		                 "Forget the gestures: the weapon as it was drawn, from the clock's start."))
			gesture(workspace, model, "clear");
	}
	if (!range.ready()) return;
	const world::LocalPlayerWeaponView &held = range.weapon_view();
	const auto action = [](int32_t id) {
		return id >= 0 && id < world::weapon_action::kCount ? world::kWeaponActionSuffixes[id] : "";
	};
	char line[160];
	std::snprintf(line, sizeof(line), "%s, then %s; clip %d, reserve %d; %d shot%s%s", action(held.current_action),
	              action(held.next_action), held.clip, held.reserve, range.shots(), range.shots() == 1 ? "" : "s",
	              range.scoped() ? "; scoped" : "");
	ui_kit::clipped_text(std::string(line) + (range.ammo().empty() ? std::string() : " of " + range.ammo()));
	if (weapon.card_up())
		ui_kit::clipped_text("The sights card is up: the game draws the scope's card in the gun's place (the HUD "
		                     "preview draws cards).");
	const std::vector<WeaponRangeEvent> &events = range.events();
	if (!events.empty() && ImGui::TreeNode("fired", "What it did (%d)", int(events.size()))) {
		const size_t shown = std::min<size_t>(events.size(), 48);
		for (size_t i = 0; i < shown; ++i) {
			const WeaponRangeEvent &event = events[events.size() - 1 - i];
			ImGui::TextWrapped("%s", (seconds(event.tick) + "  " + event.words).c_str());
		}
		ImGui::TreePop();
	}
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
	if (kind == "item" || kind == "ammo" || (kind == "weapon" && options.weapon == DefinitionWeaponView::Third)) {
		row.next(ui_kit::checkbox_width("Enemy"));
		ImGui::Checkbox("Enemy", &options.enemy);
		ui_kit::tooltip(kind == "item"   ? "As its enemies see it: its graphic_enemy."
		                : kind == "ammo" ? "As the other side sees the round: the enemy tracer item (foe_trcr_type_id)."
		                                 : "The shots as the other side sees them: the enemy's tracer style.");
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
		weapon_options(model, options, row);
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
	if (model.weapon_record()) {
		row.next(ui_kit::checkbox_width("Mute"));
		ImGui::Checkbox("Mute", &options.mute);
		ui_kit::tooltip("The weapon's sounds fire and say what they play, and nothing is heard.");
	}
	const bool eye = model.camera().posed;
	if (ui_kit::tool(row, "Frame", !eye,
	                 eye ? "The first-person eye stands where the game's camera does: Eye off to frame the model."
	                     : "The camera on the model (F, or a double click on the picture)."))
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
	if (model.weapon_record()) weapon_row(workspace, model, clock);
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
