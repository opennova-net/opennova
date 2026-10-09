#include <editor/ui/model_viewport_view.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_ctrl_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/animation_uses.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_damage.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_placement.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/anim/anim_event_bits.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/world/ammo_table.h>

namespace opennova::editor {

namespace {

constexpr int64_t kRegisterRange = 32767; // a slider's reach (the value is the register's word)
// A clip event's tick on the timeline (a step's is green, a shot's red).
constexpr ImU32 kEventColor = IM_COL32(255, 220, 90, 255);
// A weapon action's leg on the timeline (DI-13).
constexpr ImU32 kLegColor = IM_COL32(110, 190, 255, 255);

void set_options(Workspace &workspace, const ModelViewport &model, const ModelViewportOptions &options) {
	workspace.request(request::set_viewport(
			model.path(), viewport_change(ViewportKind::Model, "options", model_options_to_json(options))));
}

// The preview clock run or held, or sought to a clip tick (and held).
void set_playing(Workspace &workspace, const ModelViewport &model, bool playing) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set("playing", io::JsonValue::make_bool(playing));
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Model, "clock", std::move(clock))));
}
void seek_ticks(Workspace &workspace, const ModelViewport &model, int32_t ticks, bool hold) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set("ticks", io::json_number(std::max(ticks, 0)));
	if (hold) clock.set("playing", io::JsonValue::make_bool(false));
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Model, "clock", std::move(clock))));
}
void set_rate(Workspace &workspace, const ModelViewport &model, double rate) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set("rate", io::json_number(rate));
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Model, "clock", std::move(clock))));
}

// The playback speeds the timeline offers, and their words.
constexpr double kRates[] = {0.1, 0.25, 0.5, 1.0, 2.0};
constexpr const char *kRateWords[] = {"0.1x", "0.25x", "0.5x", "1x", "2x"};

// An event's mark on the timeline: its letter by what it does (preview_event_letter) and its colour
// by the letter; one of bits the engine does not read alone is muted, with no letter.
struct EventMark {
	ImU32 color;
	const char *letter;
};
EventMark event_mark(uint32_t trigger) {
	const char *letter = preview_event_letter(trigger);
	if (*letter == 'F') return {IM_COL32(240, 90, 80, 255), letter};
	if (*letter == 'L' || *letter == 'R') return {IM_COL32(120, 220, 120, 255), letter};
	if (*letter == 'S') return {kEventColor, letter};
	return {IM_COL32(128, 128, 128, 255), letter};
}

std::string seconds_text(double seconds) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f s", seconds);
	return text;
}

} // namespace

// What the view keeps of its own (the snap is the viewport's option).
struct ModelViewportView::Tools {
	// The Plays on choice's filter, and the models items animate, made again when the graph moves.
	char rig_filter[64] = {};
	// A press on an event's mark, held: the clock stays on the event's tick until it is let go.
	bool event_press = false;
	const AssetGraph *animated_graph = nullptr;
	uint64_t animated_generation = 0;
	std::vector<std::string> animated;

	void toolbar(Workspace &workspace, const ModelViewport &model, const ViewportContext &context);
	void registers(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock);
	void damage(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock);
	void rig_chooser(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model);
	void sound(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model);
	void fire(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model);
	void first_person(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model);
	void timeline(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock);
	void place_in_mission(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model);
};

// Place in mission (ADR 0046 DI-12): the mission last active's Place tool armed with the model's item, made
// first where none draws it (the viewport's place_in_mission command, which says what it did); where several
// items draw it, a choice of them, the one picked armed.
void ModelViewportView::Tools::place_in_mission(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model) {
	const SessionView &view = workspace.view();
	const std::string mission = place_in_mission_target(view);
	const std::vector<int64_t> drawers = mission_items_of_model(view, model.path());
	row.next(ui_kit::button_width("Place in mission"));
	ImGui::BeginDisabled(mission.empty() || !view.allows(EditorRequestKind::EditInViewport));
	if (ImGui::Button("Place in mission")) {
		if (drawers.size() > 1) {
			ImGui::OpenPopup("place_in_mission");
		} else {
			ViewportCommand command;
			command.name = "place_in_mission";
			command.kind = ViewportKind::Model;
			workspace.request(request::edit_in_viewport(model.path(), std::move(command)));
		}
	}
	ImGui::EndDisabled();
	const std::string name = basename_of(mission);
	ui_kit::tooltip(mission.empty()
	                        ? std::string("Open the mission to place this model in: Place in mission arms its Place tool with "
	                                      "the model's item.")
	                        : "Arms Place in " + name + " with the model's item: each click on its picture places one on the "
	                          "ground there. " +
	                                  (drawers.empty() ? std::string("No item draws the model yet: its item is made in the item "
	                                                                 "catalog first, a decoration or a building as its parts say.")
	                                   : drawers.size() > 1 ? std::string("Several items draw it: pick one.")
	                                                        : std::string("")));
	if (ImGui::BeginPopup("place_in_mission")) {
		for (const int64_t item : drawers) {
			MissionItemFacts facts;
			std::string ignored;
			mission_item_facts(view, item, facts, ignored);
			const std::string label = (facts.name.empty() ? std::string("Item") : facts.name) + " (" + std::to_string(item) + ")";
			if (ImGui::Selectable(label.c_str())) {
				window_requests::go_to_file(workspace, mission);
				io::JsonValue options = io::JsonValue::make_object();
				options.set("tool", io::json_string(mission_tool_token(MissionTool::Place)));
				options.set("item", io::json_number(double(item)));
				workspace.request(request::set_viewport(mission, viewport_change(ViewportKind::Mission, "options", std::move(options))));
			}
		}
		ImGui::EndPopup();
	}
}

ModelViewportView::ModelViewportView() : ViewportView(ViewportKind::Model), tools_(std::make_unique<Tools>()) {}

ModelViewportView::~ModelViewportView() = default;

void ModelViewportView::draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) {
	ViewportView::draw_empty(workspace, model, path);
	// An animation no item pairs: the author picks the model it plays on.
	const auto *shown = static_cast<const ModelViewport *>(model);
	if (shown && (shown->view_status() == ModelViewStatus::NoRig || shown->view_status() == ModelViewStatus::Reading)) {
		ui_kit::WrapRow row;
		tools_->rig_chooser(workspace, row, *shown);
	}
}

void ModelViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const ModelViewport &>(viewport);
	if (!model.model()) {
		draw_empty(workspace, &viewport, viewport.path());
		return;
	}
	// An animation: the model it plays on (the Preview window's line names it) and where the pairing
	// comes from, then the clip the selection plays (its slot in words, its file, which of the row's
	// clips it is), and why it is not the one selected where it is not, each cut to the room left
	// (whole in its tooltip).
	if (model.animating()) {
		const PreviewRig &rig = model.rig();
		ui_kit::WrapRow row;
		tools_->rig_chooser(workspace, row, model);
		// The pairing's record by its name, its file after it ("ITEMS.DEF: Ranger" as "paired by Ranger
		// (ITEMS.DEF)").
		const size_t colon = rig.source.find(": ");
		const std::string paired = rig.source.empty() ? std::string()
		                           : rig.source == "chosen" ? std::string("(chosen)")
		                           : colon == std::string::npos
		                                   ? "paired by " + rig.source
		                                   : "paired by " + rig.source.substr(colon + 2) + " (" + rig.source.substr(0, colon) + ")";
		if (!paired.empty() && !rig.pairing.file.empty()) {
			// A link to the record that pairs them (DI-05): a Go to, so the history records it.
			const float room = std::min(ui_kit::text_width(paired.c_str()), ImGui::GetFontSize() * 8.0f);
			row.next(room);
			ImGui::AlignTextToFramePadding();
			if (ImGui::TextLink((ui_kit::fit(paired, ImGui::GetContentRegionAvail().x) + "###paired").c_str()))
				window_requests::go_to(workspace, rig.pairing);
			ui_kit::tooltip(paired + "\nThe record whose graphic and animation map pair this model with the map: a click " +
			                (rig.pairing.editable ? "opens " : "shows ") + rig.pairing.file + " at it.");
		} else if (!paired.empty()) {
			row.next(std::min(ui_kit::text_width(paired.c_str()), ImGui::GetFontSize() * 8.0f));
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ui_kit::clipped_text(paired, "The item whose graphic and anim_def pair this model with the map.");
			ImGui::PopStyleColor();
		}
		const std::string &key = model.clip_key();
		if (!key.empty()) {
			const int count = model.skeleton() ? model.skeleton()->clip_variant_count(key) : 0;
			std::string playing = "Playing " + animation_key_title(key) + ": " + model.clip_file();
			if (count > 1)
				playing += " (clip " + std::to_string(model.clip_variant() + 1) + " of " + std::to_string(count) +
				           ", played in turn from the last)";
			ui_kit::clipped_text(playing, playing + "\n" + key);
		}
		if (!model.clip_note().empty() || !model.detail().empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			if (!model.clip_note().empty()) ImGui::TextWrapped("%s", model.clip_note().c_str());
			if (!model.detail().empty()) ImGui::TextWrapped("%s", model.detail().c_str());
			ImGui::PopStyleColor();
		}
	}
	tools_->toolbar(workspace, model, context);
	// A LOD with no part draws nothing (retail ships empty LODs): said, not left a blank picture.
	const int lod = model.lod();
	if (lod >= 0 && size_t(lod) < model.model()->lod_count && model.model()->lods[lod].render_object_count == 0)
		ImGui::TextDisabled("LOD %d holds no part: there is nothing to draw at it.", lod);
	snap = model.options().snap;
	context.snap = snap;
	// The timeline's rows: the transport, the track, the legend and the last sound heard (DI-04).
	const float timeline = model.animating() ? ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetFontSize() * 2.0f +
	                                                   ImGui::GetTextLineHeightWithSpacing() + 16.0f
	                                         : 0.0f;
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y - timeline));
	if (model.animating()) tools_->timeline(workspace, model, context.input.clock);
}

// The model an animation plays on: Auto (the one an item pairs with the table) or a model of the
// project's, found by typing part of its name, the models items animate first (S17: a new clip's
// likely rigs).
void ModelViewportView::Tools::rig_chooser(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model) {
	const SessionView &view = workspace.view();
	ModelViewportOptions options = model.options();
	const float width = ImGui::GetFontSize() * 10.0f;
	row.next(ui_kit::field_width(width, "Plays on"));
	ImGui::SetNextItemWidth(width);
	if (ImGui::BeginCombo("Plays on", options.rig_model.empty() ? "Auto" : options.rig_model.c_str(),
	                      ImGuiComboFlags_HeightLarge)) {
		if (ImGui::IsWindowAppearing()) {
			rig_filter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##rig_filter", "Type part of a model's name", rig_filter, sizeof(rig_filter));
		const std::string wanted = strutil::to_lower(rig_filter);
		const auto shown = [&](const std::string &name) {
			return wanted.empty() || strutil::to_lower(name).find(wanted) != std::string::npos;
		};
		if (wanted.empty() && ImGui::Selectable("Auto", options.rig_model.empty())) options.rig_model.clear();
		if (view.findings.graph && (animated_graph != view.findings.graph.get() ||
		                            animated_generation != view.findings.graph->generation())) {
			animated_graph = view.findings.graph.get();
			animated_generation = view.findings.graph->generation();
			animated = view.project.scan ? animated_models(*view.findings.graph, *view.project.scan)
			                             : std::vector<std::string>();
		}
		bool any = false;
		for (const std::string &name : animated) {
			if (!shown(name)) continue;
			if (!any) ImGui::SeparatorText("Models items animate");
			any = true;
			if (ImGui::Selectable((name + "##animated").c_str(), name == options.rig_model)) options.rig_model = name;
		}
		ImGui::SeparatorText("Every model");
		for (const AssetEntry &entry : view.project.scan->entries)
			if (entry.kind == AssetKind::Model && shown(entry.logical_name) &&
			    ImGui::Selectable(entry.logical_name.c_str(), entry.logical_name == options.rig_model))
				options.rig_model = entry.logical_name;
		ImGui::EndCombo();
	}
	ui_kit::tooltip("The model the animation plays on. Auto takes the graphic of an item whose "
	                "anim_def names the map. A clip's bones pair with the model's parts by their order.");
	if (options != model.options()) set_options(workspace, model, options);
}

// How the clip's events are heard (DI-04, preview/preview_clip_sounds): Mute; the Surface under the feet a
// footstep's slot follows; the body that reads the events (its tick half); a female player's profile; the
// profile, the paired item's or one picked. Each a SetViewport of the options' `sound`.
void ModelViewportView::Tools::sound(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model) {
	ModelViewportOptions options = model.options();
	ClipSoundOptions &sound = options.sound;
	const ClipSoundBinding &binding = model.sound_binding();
	const char *label = sound.mute ? "Sound (muted)###sound" : "Sound###sound";
	row.next(ui_kit::button_width(label));
	if (ImGui::Button(label)) ImGui::OpenPopup("sound");
	ui_kit::tooltip("What the clip's events play as it runs, as the game plays them: " + binding.profile_words + "\n" +
	                binding.body_words);
	if (ImGui::BeginPopup("sound")) {
		const float unit = ImGui::GetFontSize();
		ImGui::Checkbox("Mute", &sound.mute);
		ui_kit::tooltip("The events still fire and say what they play; nothing is heard.");
		static const char *const kSurfaces[] = {"Ground", "Snow", "On an object", "In water"};
		int surface = int(sound.surface);
		ImGui::SetNextItemWidth(unit * 8.0f);
		if (ImGui::Combo("Surface", &surface, kSurfaces, IM_ARRAYSIZE(kSurfaces))) sound.surface = audio::FootSurface(surface);
		ui_kit::tooltip("What is under the feet, as the game tests it for each footstep, in this order: feet under "
		                "the water plane play SSFootWater (both feet); standing on an object, SSLFootOBJ or "
		                "SSRFootOBJ; on snow (the terrain's surface class 3), SSLFootSnow or SSRFootSnow; else the "
		                "ground, SSLFootGND or SSRFootGND (Entity_UpdateInfantryAI @ 0x4bf23e, the player body's "
		                "@ 0x4b77c6). The sounds 1 to 6 play SSAudio1 to SSAudio6 whatever is underfoot.");
		static const char *const kBodies[] = {"Auto", "NPC body", "Player body"};
		int body = int(sound.body);
		ImGui::SetNextItemWidth(unit * 8.0f);
		if (ImGui::Combo("Body", &body, kBodies, IM_ARRAYSIZE(kBodies))) sound.body = ClipSoundBody(body);
		ui_kit::tooltip(binding.body_words + "\nAn NPC's body reads a clip's events on odd game ticks, a player's on "
		                "even ones (Entity_UpdateInfantryAI @ 0x4bf144, Entity_UpdateInfantryPlayerBody @ 0x4b76e6); "
		                "Auto takes the body the paired item's move_function runs.");
		ImGui::BeginDisabled(!binding.player);
		ImGui::Checkbox("Female", &sound.female);
		ImGui::EndDisabled();
		ui_kit::tooltip("A female avatar: a player's body plays its item's sound_profileFemale (an NPC's never does).");
		ImGui::SetNextItemWidth(unit * 10.0f);
		if (ImGui::BeginCombo("Profile", sound.profile.empty() ? "Auto" : sound.profile.c_str())) {
			if (ImGui::Selectable("Auto", sound.profile.empty())) sound.profile.clear();
			ui_kit::tooltip("The paired item's sound_profile (default where it names none).");
			for (const audio::SoundProfile &profile : model.sound_sources().profiles())
				if (ImGui::Selectable(profile.name.c_str(), profile.name == sound.profile)) sound.profile = profile.name;
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The SndProf.def profile whose slots the events play: Auto is the paired item's.");
		ImGui::PushTextWrapPos(unit * 24.0f);
		ImGui::TextDisabled("%s", binding.profile_words.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndPopup();
	}
	if (options != model.options()) set_options(workspace, model, options);
}

// A clip's fire events (DI-24, preview/preview_clip_fire): why they fire or not, the item's ammo by slot, and the
// range they fly in (its target, its surface and distance, the side seeing them). Each a SetViewport of the options'
// `fire`.
void ModelViewportView::Tools::fire(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model) {
	ModelViewportOptions options = model.options();
	ClipFireOptions &fire = options.fire;
	const ClipFire &clip_fire = model.clip_fire();
	const char *label = clip_fire.armed() ? "Fire###fire" : "Fire (none)###fire";
	row.next(ui_kit::button_width(label));
	if (ImGui::Button(label)) ImGui::OpenPopup("fire");
	ui_kit::tooltip("What the clip's fire events fire as it runs, as the game's NPC body fires them: " + clip_fire.words());
	if (ImGui::BeginPopup("fire")) {
		const float unit = ImGui::GetFontSize();
		ImGui::PushTextWrapPos(unit * 26.0f);
		ImGui::TextUnformatted(clip_fire.words().c_str());
		// The item's ammo by slot, as its organic init resolves them.
		const world::OrganicWeapons &weapons = clip_fire.weapons();
		for (int slot = 0; slot < world::kOrganicAmmoSlots; ++slot) {
			const uint8_t byte = weapons.ammo[size_t(slot)];
			const world::AmmoTable *table = clip_fire.range().ammo_table();
			const world::AmmoTableEntry *ammo = table && byte != 0 ? table->by_index(byte) : nullptr;
			ImGui::TextDisabled("%s: %s", world::kOrganicAmmoFields[slot], ammo ? ammo->name.c_str() : "none");
		}
		ImGui::PopTextWrapPos();
		ImGui::Checkbox("Target", &fire.target.shown);
		ui_kit::tooltip("A wall down the body's line of fire, whose face plays the ammo's impact row for its surface; "
		                "off, the rounds fly on until they age out.");
		if (fire.target.shown) {
			ImGui::SetNextItemWidth(unit * 7.0f);
			const int tag = std::clamp(fire.target.tag, kWeaponRangeFirstTag, world::kImpactEffectTagCount - 1);
			if (ImGui::BeginCombo("Surface", world::kImpactEffectTagWords[tag])) {
				for (int i = kWeaponRangeFirstTag; i < world::kImpactEffectTagCount; ++i)
					if (ImGui::Selectable(world::kImpactEffectTagWords[i], i == tag)) fire.target.tag = i;
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The target's surface: a round striking a face of material b plays the ammo's row b + 4.");
			ImGui::SetNextItemWidth(unit * 6.0f);
			float range = fire.target.range;
			if (ImGui::InputFloat("Range", &range, 5.0f, 25.0f, "%.0f m"))
				fire.target.range = std::clamp(range, kWeaponRangeNearest, kWeaponRangeFarthest);
			ui_kit::tooltip("The target's distance down the body's line of fire, metres.");
		}
		ImGui::Checkbox("Seen by the enemy", &fire.enemy);
		ui_kit::tooltip("The shots as the other side sees them: the ammo's enemy tracer style (RoundData_SpawnRound "
		                "@ 0x4EC740 selects it against the seeing client's team).");
		ImGui::EndPopup();
	}
	if (options != model.options()) set_options(workspace, model, options);
}

// A weapon's map in first person (DI-13, preview/preview_first_person): the view (the orbit, or the eye the
// game draws the view model before), the character whose arms draw, the action whose sets the row plays,
// what draws and why, and each of the action's legs in words with a Play of it. Each a SetViewport of the
// options' `first_person`.
void ModelViewportView::Tools::first_person(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model) {
	ModelViewportOptions options = model.options();
	FirstPersonOptions &chosen = options.first_person;
	const FirstPersonSources &sources = model.first_person();
	const char *label = model.eye_view() ? "First person (eye)###first_person" : "First person###first_person";
	row.next(ui_kit::button_width(label));
	if (ImGui::Button(label)) ImGui::OpenPopup("first_person");
	ui_kit::tooltip(sources.words());
	if (ImGui::BeginPopup("first_person")) {
		const float unit = ImGui::GetFontSize();
		ImGui::PushTextWrapPos(unit * 26.0f);
		ImGui::TextUnformatted(sources.words().c_str());
		if (!sources.arms_note().empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", sources.arms_note().c_str());
		ImGui::PopTextWrapPos();
		int view = chosen.eye ? 1 : 0;
		ImGui::RadioButton("Orbit", &view, 0);
		ui_kit::tooltip("Turn around the gun and the arms with the orbit camera.");
		ImGui::SameLine();
		ImGui::RadioButton("Eye", &view, 1);
		ui_kit::tooltip("The camera the game draws the weapon before: the model at the weapon's pos at the hip, turned by "
		                "its cant, seen through its renderfov (Player_UpdateFirstPersonCamera @ 0x4dd380, "
		                "Player_RenderFirstPersonViewModel @ 0x4ded60). Nothing moves it.");
		chosen.eye = view == 1;
		const FirstPersonCharacter *who = sources.character();
		ImGui::SetNextItemWidth(unit * 16.0f);
		if (ImGui::BeginCombo("Character", who ? who->words.c_str() : "None")) {
			for (const FirstPersonCharacter &character : sources.characters()) {
				const std::string line = character.words + (character.arms.empty() ? "  (no arms)" : "  (" + character.arms + ")") +
				                         "##" + std::to_string(character.id);
				if (ImGui::Selectable(line.c_str(), &character == who)) chosen.character = int32_t(character.id);
			}
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The character the player is: the game draws its Avatars.def arms with the gun's bones. A fresh "
		                "profile is the first of the good side.");
		const std::vector<const WeaponActionRun *> playing = sources.actions_playing(model.clip_key());
		const WeaponActionRun *action = model.first_person_action();
		if (playing.size() > 1) {
			ImGui::SetNextItemWidth(unit * 10.0f);
			if (ImGui::BeginCombo("Action", action ? action->suffix.c_str() : "")) {
				for (const WeaponActionRun *run : playing)
					if (ImGui::Selectable(run->suffix.c_str(), run == action)) chosen.action = run->suffix;
				ImGui::EndCombo();
			}
			ui_kit::tooltip("Several of the weapon's actions play this row: the sets of the one chosen play.");
		}
		ImGui::PushTextWrapPos(unit * 26.0f);
		if (!action) {
			ImGui::TextDisabled("No action of %s plays this row: no set plays with it.", sources.weapon().c_str());
		} else {
			ImGui::TextUnformatted(action->words.c_str());
			for (size_t i = 0; i < action->legs.size(); ++i) {
				const WeaponActionLeg &leg = action->legs[i];
				ImGui::PushID(int(i));
				ImGui::BeginDisabled(leg.tick < 0);
				if (ImGui::SmallButton("Play")) workspace.request(request::play_action_leg(model.path(), leg.end));
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::TextUnformatted(leg.words.c_str());
				ImGui::PopID();
			}
			if (action->legs.empty()) ImGui::TextDisabled("Its row plays no set.");
		}
		ImGui::TextDisabled("The game reads no first-person clip event; Mute (Sound) mutes the sets too.");
		ImGui::PopTextWrapPos();
		ImGui::EndPopup();
	}
	if (options != model.options()) set_options(workspace, model, options);
}

// The clip the selection plays (ADR 0046 S17): its transport (Run or Pause, Space; the first
// frame, Home; a frame back and on, the arrows; Repeat for a one-shot; the speed), the frame and the
// time shown, then a track the mouse scrubs with its frames ruled and its events marked by what
// they do (a footstep L or R, a shot F, a sound S; hovered, the event in words; clicked, the clock
// held there and, in the clip's own document, the event selected), and the marks' legend.
void ModelViewportView::Tools::timeline(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock) {
	const int32_t length = model.clip_length_ticks();
	if (model.clip_key().empty() || length <= 0) {
		ImGui::TextDisabled("%s", !model.clip_note().empty() ? "Nothing plays here."
		                          : model.rig().table.empty() || model.clip_key().empty()
		                                  ? "Select a row of the map (or a clip of a row) to play it."
		                                  : "The clip does not load in the rig.");
		return;
	}
	const int32_t ticks = model.clip_ticks(clock);
	const int32_t shown = model.clip_loops() ? ticks % length : std::min(ticks, length);
	const uint32_t frames = model.clip_frame_count();
	const double frame = model.clip_frame(clock);
	// The keys, while the Preview has the keyboard and no text field takes it; an arrow with Alt held is
	// Back's or Forward's (the navigation history), never a frame's step.
	const bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
	const bool arrows = keys && !ImGui::GetIO().KeyAlt;
	ui_kit::WrapRow row;
	const char *run = clock.playing() ? "Pause##clip" : "Run##clip";
	row.next(ui_kit::button_width(run));
	if (ImGui::Button(run) || (keys && ImGui::IsKeyPressed(ImGuiKey_Space, false))) set_playing(workspace, model, !clock.playing());
	ui_kit::tooltip(clock.playing() ? "Hold the clip where it is (Space)." : "Run the clip (Space).");
	row.next(ui_kit::button_width("|<"));
	if (ImGui::Button("|<") || (keys && ImGui::IsKeyPressed(ImGuiKey_Home, false))) seek_ticks(workspace, model, 0, true);
	ui_kit::tooltip("The first frame (Home).");
	row.next(ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::ArrowButton("##back", ImGuiDir_Left) || (arrows && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)))
		seek_ticks(workspace, model, model.tick_of_step(shown, -1), true);
	ui_kit::tooltip("A frame back (Left).");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##forward", ImGuiDir_Right) || (arrows && ImGui::IsKeyPressed(ImGuiKey_RightArrow)))
		seek_ticks(workspace, model, model.tick_of_step(shown, 1), true);
	ui_kit::tooltip("A frame on (Right).");
	char where[96];
	std::snprintf(where, sizeof(where), "Frame %d of %u, %s of %s", int(std::floor(frame + 1e-6)), frames,
	              seconds_text(shown / io::kTickHz).c_str(), seconds_text(length / io::kTickHz).c_str());
	row.next(ui_kit::text_width(where));
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(where);
	ui_kit::tooltip("The frame the clip shows (from 0) and the time, as the game's ticks pass (62.5 a "
	                "second; tick " + std::to_string(shown) + " of " + std::to_string(length) + ").");
	ModelViewportOptions options = model.options();
	row.next(ui_kit::checkbox_width("Repeat"));
	ImGui::BeginDisabled(model.clip_loops());
	bool repeat = model.clip_loops() || options.repeat;
	if (ImGui::Checkbox("Repeat", &repeat)) {
		options.repeat = repeat;
		set_options(workspace, model, options);
	}
	ImGui::EndDisabled();
	ui_kit::tooltip(model.clip_loops()
	                        ? "This clip loops (its Loops flag): it plays again from its start as the game plays it."
	                        : "Play this one-shot again from its start after a moment. The game plays a one-shot "
	                          "once and holds its last frame.");
	int rate = 3;
	for (int i = 0; i < int(std::size(kRates)); ++i)
		if (std::fabs(clock.rate() - kRates[i]) < 1e-6) rate = i;
	const float rate_width = ImGui::GetFontSize() * 3.5f;
	row.next(ui_kit::field_width(rate_width, "Speed"));
	ImGui::SetNextItemWidth(rate_width);
	if (ImGui::Combo("Speed", &rate, kRateWords, int(std::size(kRateWords)))) set_rate(workspace, model, kRates[rate]);
	ui_kit::tooltip("How fast the preview plays: 1x is the game's speed.");

	// The track: scrubbed by the mouse (the clock held where it is let go); the events' letters on its
	// top line, the frames ruled and numbered on its bottom one.
	const float height = ImGui::GetFontSize() * 2.0f + 8.0f;
	const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##track", ImVec2(width, height));
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
	const float pad = 6.0f;
	const float left = at.x + pad, right = at.x + width - pad;
	const auto x_of = [&](int32_t tick) { return left + (right - left) * float(tick) / float(length); };
	// The event under the mouse, if any (within 4 px of its mark).
	const float mouse = ImGui::GetIO().MousePos.x;
	const PreviewClipEvent *under = nullptr;
	for (const PreviewClipEvent &event : model.clip_events())
		if (hovered && std::fabs(mouse - x_of(event.tick)) <= 4.0f) under = &event;
	// A weapon's action playing the row (DI-13): its legs' marks on the track's foot, B as it begins, E as it
	// finishes (a leg past the clip's end at its right end).
	const WeaponActionRun *action = model.first_person_action();
	const WeaponActionLeg *leg_under = nullptr;
	if (action && !under)
		for (const WeaponActionLeg &leg : action->legs)
			if (leg.tick >= 0 && hovered && std::fabs(mouse - x_of(std::min(leg.tick, length))) <= 4.0f) leg_under = &leg;
	// A press on an event's mark holds the clock on the event's own tick until it is let go; any other
	// press scrubs. A loop's right end is its last tick (its wrap tick shows the first frame again).
	if (clicked) event_press = under != nullptr || leg_under != nullptr;
	if (!active) event_press = false;
	if (active && !event_press) {
		const float t = std::clamp((mouse - left) / std::max(1.0f, right - left), 0.0f, 1.0f);
		int32_t to = int32_t(std::lround(t * float(length)));
		if (model.clip_loops()) to = std::min(to, length - 1);
		if (to != ticks || clock.playing()) seek_ticks(workspace, model, to, true);
	}
	ImDrawList *paint = ImGui::GetWindowDrawList();
	const float top = at.y, bottom = at.y + height;
	paint->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + width, bottom), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
	// The frames ruled: every frame where they are far enough apart, else every 5th or 10th; the 10ths
	// longer and numbered where there is room.
	const float per_frame = frames > 0 ? (right - left) / float(frames) : 0.0f;
	const int step = per_frame >= 5.0f ? 1 : per_frame >= 1.5f ? 5 : 10;
	const float label_room = ui_kit::text_width("000") + 4.0f;
	for (uint32_t f = 0; frames > 0 && f <= frames; f += uint32_t(step)) {
		const int32_t tick = model.tick_of_frame(int(f));
		const float x = f == frames ? right : tick >= 0 ? x_of(tick) : left + per_frame * float(f);
		const bool tenth = f % 10 == 0;
		paint->AddLine(ImVec2(x, bottom - (tenth ? 8.0f : 4.0f)), ImVec2(x, bottom), ImGui::GetColorU32(ImGuiCol_TextDisabled));
		if (tenth && f > 0 && f < frames && per_frame * 10.0f >= label_room) {
			const std::string label = std::to_string(f);
			paint->AddText(ImVec2(x - ui_kit::text_width(label.c_str()) * 0.5f, bottom - 8.0f - ImGui::GetFontSize()),
			               ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
		}
	}
	// The events: a mark and its letter at the top of the track.
	for (const PreviewClipEvent &event : model.clip_events()) {
		const float x = x_of(event.tick);
		const EventMark mark = event_mark(event.trigger);
		paint->AddRectFilled(ImVec2(x - 1.5f, top + 2.0f), ImVec2(x + 1.5f, top + 9.0f), mark.color);
		paint->AddText(ImVec2(x + 2.5f, top), mark.color, mark.letter);
	}
	// The action's legs: a mark and its letter at the foot of the track.
	for (size_t i = 0; action && i < action->legs.size(); ++i) {
		const WeaponActionLeg &leg = action->legs[i];
		if (leg.tick < 0) continue;
		const float x = x_of(std::min(leg.tick, length));
		paint->AddRectFilled(ImVec2(x - 1.5f, bottom - 9.0f), ImVec2(x + 1.5f, bottom - 2.0f), kLegColor);
		paint->AddText(ImVec2(x + 2.5f, bottom - ImGui::GetFontSize() - 1.0f), kLegColor, leg.end ? "E" : "B");
	}
	// The playhead.
	const float head = x_of(shown);
	paint->AddLine(ImVec2(head, top), ImVec2(head, bottom), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f);
	if (under) {
		// What it plays (DI-04), a line a sound.
		std::string plays;
		for (const std::string &line : model.event_sound_words(under->trigger)) {
			std::string said = line;
			if (!said.empty()) said[0] = char(std::toupper(static_cast<unsigned char>(said[0])));
			plays += "\n" + said;
		}
		// What it fires (DI-24), a line a shot.
		std::string fires;
		for (const std::string &line : model.event_fire_words(under->trigger)) {
			std::string said = line;
			if (!said.empty()) said[0] = char(std::toupper(static_cast<unsigned char>(said[0])));
			fires += "\n" + said;
		}
		// A weapon's first-person clip (DI-13): nothing reads its events, so a press plays nothing.
		const bool audible = !plays.empty() && !model.first_person().active();
		const bool shoots = model.clip_fire().armed() && !model.first_person().active() &&
		                    (under->trigger & anim::kAnimEventFireMask) != 0;
		ui_kit::tooltip("Frame " + std::to_string(under->frame) + " (" + seconds_text(under->tick / io::kTickHz) +
		                "): " + animation_trigger_words(under->trigger) + "." + plays + fires +
		                (shoots    ? "\nClick to go there and fire it once."
		                 : audible ? "\nClick to go there and hear it."
		                           : "\nClick to go there."));
		if (clicked) {
			seek_ticks(workspace, model, under->tick, true);
			// A press plays the event once, as the game plays it, and fires its shots once (play_sound {frame}).
			if (audible || shoots) workspace.request(request::play_clip_event(model.path(), under->frame));
			// In the clip's own document the event is a record: select it.
			const SessionView &view = workspace.view();
			for (const auto &open : view.documents.open) {
				const auto *clip_document = dynamic_cast<const AnimationDocument *>(open.get());
				if (!clip_document || clip_document->path() != model.path() || clip_document->rows().empty()) continue;
				const Node &clip_row = *clip_document->rows().front();
				if (size_t(under->frame) < clip_row.collections[1].size())
					window_requests::select(workspace, *clip_document,
					                        {clip_row.id, node_kind(AnimationKind::Event),
					                         clip_row.collections[1][size_t(under->frame)]});
			}
		}
	} else if (leg_under) {
		// A leg of the weapon's action (DI-13): what it plays, and a press plays it once.
		ui_kit::tooltip(leg_under->words + "\nClick to go there and hear it.");
		if (clicked) {
			seek_ticks(workspace, model, std::min(leg_under->tick, length), true);
			workspace.request(request::play_action_leg(model.path(), leg_under->end));
		}
	} else if (hovered) {
		ui_kit::tooltip("Drag to scrub; the clip holds where you let go.");
	}
	// The marks' legend: a weapon's action's legs (DI-13), whose clip's own events the game reads not.
	const bool legs = action && !action->legs.empty();
	if (legs) {
		std::string letters;
		for (const WeaponActionLeg &leg : action->legs) {
			if (!letters.empty()) letters += ' ';
			letters += leg.end ? 'E' : 'B';
		}
		const std::string legend = letters + " " + action->suffix + (action->legs.size() == 1 ? "'s set" : "'s sets");
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kLegColor), "%s", legend.c_str());
		ui_kit::tooltip("B: the set the action plays as it begins (its soundset); E: as it finishes (its soundsetend).");
	}
	if (legs && !model.clip_events().empty()) ImGui::SameLine();
	if (model.first_person().active() && !model.clip_events().empty()) {
		ImGui::TextDisabled("Events: not read in first person");
	} else if (!model.clip_events().empty()) {
		ImGui::TextColored(ImVec4(120 / 255.0f, 220 / 255.0f, 120 / 255.0f, 1.0f), "L R footstep");
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(240 / 255.0f, 90 / 255.0f, 80 / 255.0f, 1.0f), "F fires");
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 220 / 255.0f, 90 / 255.0f, 1.0f), "S sound");
	}
	// The clip's fire events (DI-24): how many shots the run fired to the clock, or why none fires.
	bool fire_marks = false;
	for (const PreviewClipEvent &event : model.clip_events())
		fire_marks = fire_marks || (event.trigger & anim::kAnimEventFireMask) != 0;
	if (fire_marks && !model.first_person().active()) {
		const ClipFire &clip_fire = model.clip_fire();
		const std::string shots = clip_fire.armed()
		                                  ? "Shots: " + std::to_string(clip_fire.range().shots()) + " fired to this tick."
		                                  : "Shots: none. " + clip_fire.words();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text(shots, clip_fire.words());
		ImGui::PopStyleColor();
	}
	// The last sound the clip's events fired (DI-04), in words.
	if (!model.sounds_fired().empty()) {
		const ClipSoundFired &last = model.sounds_fired().back();
		const std::string heard = (last.state == "played" ? "Heard: " : "Fired: ") + last.words;
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text(heard, heard);
		ImGui::PopStyleColor();
	}
}

// The level (Auto or one held), what Auto picks and why, the clock, what the overlays
// mark, Frame, and the registers, on a row that wraps whole controls in a narrow window.
void ModelViewportView::Tools::toolbar(Workspace &workspace, const ModelViewport &model, const ViewportContext &context) {
	const threedi::Threedi3di3 &shown = *model.model();
	ModelViewportOptions options = model.options();
	const float unit = ImGui::GetFontSize();
	int32_t projected = 0;
	const int automatic = model.auto_lod(&projected);
	// The LODs as the outline names them (documents/model_labels.h): by index, what each draws at and
	// its parts; a LOD the game's walk never reaches says so.
	std::vector<int32_t> thresholds;
	for (size_t i = 0; i < shown.lod_count; ++i) thresholds.push_back(shown.lods[i].lod_threshold);
	const int held = options.lod < 0 ? -1 : std::min(options.lod, int(shown.lod_count) - 1);
	const std::string label = options.lod < 0 ? "Auto (LOD " + std::to_string(automatic) + ")" : "LOD " + std::to_string(held);
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(unit * 11.0f, "LOD"));
	ImGui::SetNextItemWidth(unit * 11.0f);
	if (ImGui::BeginCombo("LOD", label.c_str())) {
		if (ImGui::Selectable("Auto", options.lod < 0)) options.lod = -1;
		ui_kit::tooltip("The LOD the game picks at this distance.");
		for (size_t i = 0; i < shown.lod_count; ++i) {
			const size_t parts = shown.lods[i].render_object_count;
			const std::string text = "LOD " + std::to_string(i) + ": " + model_lod_range(thresholds, i) + ", " +
			                         (parts ? std::to_string(parts) + (parts == 1 ? " part" : " parts") : std::string("no parts"));
			if (!renderer::object_lod_reachable(thresholds, i)) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::Selectable(text.c_str(), options.lod == int(i))) options.lod = int(i);
			if (!renderer::object_lod_reachable(thresholds, i)) ImGui::PopStyleColor();
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Auto draws the LOD the game picks at this distance: the model's projected radius "
	                "against each LOD's threshold, LOD 0 first, the walk stopping at the first 0. A LOD held "
	                "stays at any distance.");
	// The projected radius Auto measures; the game's own reading inside the model's sphere.
	const bool inside = projected >= renderer::kObjectLodBehindEyeRadiusQ16;
	char radius[48];
	if (inside) std::snprintf(radius, sizeof(radius), "inside its sphere");
	else std::snprintf(radius, sizeof(radius), "radius %.0f px", projected / 65536.0);
	row.next(ui_kit::text_width(radius));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", radius);
	ui_kit::tooltip(inside ? "The eye is inside the model's projection sphere: the game reads a 4096 px radius "
	                         "there and draws LOD 0."
	                       : "The model's projected radius on the picture, the size Auto measures.");
	// The preview clock: Run or Pause (Play is the game's); a clip's timeline runs and holds the same
	// clock, so an animation has its button there alone.
	const bool playing = context.input.clock.playing();
	const char *clock = playing ? "Pause" : "Run";
	if (!model.animating()) {
		row.next(ui_kit::button_width(clock));
		if (ImGui::Button(clock)) set_playing(workspace, model, !playing);
		ui_kit::tooltip("Run or hold the preview clock: the model's part animations, flipbooks and colour "
		                "generators.");
	}
	if (model.animating()) sound(workspace, row, model);
	if (model.animating() && !model.first_person().active()) fire(workspace, row, model);
	if (model.animating() && model.first_person().active()) first_person(workspace, row, model);
	row.next(ui_kit::button_width("Show"));
	if (ImGui::Button("Show")) ImGui::OpenPopup("marks");
	ui_kit::tooltip("What the viewport marks over the model, and the collision it draws.");
	if (ImGui::BeginPopup("marks")) {
		ImGui::Checkbox("User points", &options.overlays.user_points);
		ImGui::Checkbox("Lights", &options.overlays.lights);
		ImGui::Checkbox("Part pivots", &options.overlays.pivots);
		if (model.animating()) {
			ImGui::Checkbox("Bones", &options.bones);
			ui_kit::tooltip("The rig's bones as the clip poses them, each named under the pointer; in the "
			                "clip's own document a click on one selects it.");
		} else {
			// The collision (S17, preview/model_collision): a layer per thing the game tests the model
			// against, each in its colour, its count, what the game does with it in its tooltip.
			ImGui::SeparatorText("Collision");
			const float swatch = ImGui::GetFrameHeight() * 0.6f;
			for (size_t l = 0; l < size_t(ModelCollisionLayer::kCount); ++l) {
				const ModelCollisionLayer layer = ModelCollisionLayer(l);
				const ModelCollisionLayerRow &row = model_collision_layer(layer);
				const size_t count = model_collision_layer_count(shown, layer, model.lod());
				bool on = model_collision_layer_on(options.overlays, layer);
				ImGui::PushID(row.token);
				ImGui::BeginDisabled(count == 0 && !on);
				const ImVec4 color(((row.rgb >> 16) & 0xFF) / 255.0f, ((row.rgb >> 8) & 0xFF) / 255.0f, (row.rgb & 0xFF) / 255.0f, 1.0f);
				ImGui::AlignTextToFramePadding();
				ImGui::ColorButton("##swatch", color,
				                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
				                   ImVec2(swatch, swatch));
				ImGui::SameLine();
				const std::string text = std::string(row.label) + " (" + std::to_string(count) + ")";
				if (ImGui::Checkbox(text.c_str(), &on)) model_collision_layer_set(options.overlays, layer, on);
				ImGui::EndDisabled();
				ui_kit::tooltip(count == 0 ? std::string("This model has none.\n") + row.words : std::string(row.words));
				ImGui::PopID();
			}
		}
		ImGui::EndPopup();
	}
	// The snap is the viewport's option (the MCP gaps lane); one the wire set off the list shown as its value.
	static const char *const kSnapNames[] = {"Free", "1/64 m", "1/16 m", "1/4 m", "1 m"};
	row.next(ui_kit::field_width(unit * 5.0f, "Snap"));
	ImGui::SetNextItemWidth(unit * 5.0f);
	int snap = -1;
	for (int i = 0; i < IM_ARRAYSIZE(kSnapNames); ++i)
		if (kModelHandleSnaps[i] == options.snap) snap = i;
	char custom[32];
	std::snprintf(custom, sizeof(custom), "%g m", double(options.snap));
	if (ImGui::BeginCombo("Snap", snap >= 0 ? kSnapNames[snap] : custom)) {
		for (int i = 0; i < IM_ARRAYSIZE(kSnapNames); ++i)
			if (ImGui::Selectable(kSnapNames[i], i == snap)) options.snap = kModelHandleSnaps[i];
		ImGui::EndCombo();
	}
	ui_kit::tooltip("A dragged marker's place snaps to this grid on each of the file's axes. Hold "
	                "Alt to place freely.");
	row.next(ui_kit::button_width("Frame"));
	if (ImGui::Button("Frame")) {
		// The selected record's marker or collision shape, else the whole model (a record with neither, a
		// texture, a material, a section that stores no sphere, frames the whole model, as F does).
		CanvasWindowRequests requests(workspace);
		std::string error;
		model.command(context, "frame", model.frame_ids(context), requests, error);
	}
	ui_kit::tooltip("Look at the selected marker or collision record, or at the whole model (F).");
	if (!model.animating()) place_in_mission(workspace, row, model);
	row.next(ui_kit::button_width("Registers"));
	ImGui::BeginDisabled(shown.ctrl.count == 0);
	if (ImGui::Button("Registers")) ImGui::OpenPopup("registers");
	ImGui::EndDisabled();
	ui_kit::tooltip(shown.ctrl.count == 0 ? "The model declares no CTRL registers."
	                                      : "The model's CTRL registers by what drives them in the game: hold one at "
	                                        "a value, as the game's entity would drive it.");
	if (options != model.options()) set_options(workspace, model, options);
	if (ImGui::BeginPopup("registers")) {
		registers(workspace, model, context.input.clock);
		ImGui::EndPopup();
	}
	// The damage states (DI-10): shown where an item names the model as its graphic or its husk.
	if (!model.animating() && !model.damage_uses().empty()) {
		const bool destroyed = model.options().damage.state == DamageState::Destroyed;
		const char *label = destroyed ? "Damage (destroyed)###damage" : "Damage###damage";
		row.next(ui_kit::button_width(label));
		if (ImGui::Button(label)) ImGui::OpenPopup("damage");
		ui_kit::tooltip("The model intact or destroyed, as the game destroys the item naming it: the husk swapped "
		                "in, the pieces, the destroy fade on the preview clock; Play destroy runs it from the death.");
		if (ImGui::BeginPopup("damage")) {
			damage(workspace, model, context.input.clock);
			ImGui::EndPopup();
		}
	}
	// From the model to its animations (S17): the maps the items pairing it play, each opened to play
	// on it.
	const SessionView &view = workspace.view();
	if (!model.animating() && view.findings.graph && view.project.scan) {
		const std::vector<ModelAnimation> maps = model_animations(*view.findings.graph, *view.project.scan, model.path());
		if (!maps.empty()) {
			const std::string label = "Animations (" + std::to_string(maps.size()) + ")";
			row.next(ui_kit::button_width(label.c_str()));
			if (ImGui::Button(label.c_str())) ImGui::OpenPopup("animations");
			ui_kit::tooltip("The animation maps the items using this model play on it. Open one to play its clips here.");
			if (ImGui::BeginPopup("animations")) {
				for (const ModelAnimation &played : maps) {
					const std::string name = played.map.substr(played.map.find_last_of('/') + 1);
					const std::string line = name + "  (" + played.record + (played.via.empty() ? "" : ", " + played.via) + ")";
					if (ImGui::Selectable(line.c_str())) {
						window_requests::go_to_file(workspace, played.map);
						// On this model: chosen where the map's own pairing takes another item's model.
						const std::string here = model.path().substr(model.path().find_last_of('/') + 1);
						const PreviewRig paired = resolve_preview_rig(*view.findings.graph, *view.project.scan, name,
						                                              AssetKind::AnimationMap, std::string());
						if (!strutil::iequals(paired.model, here)) {
							// The model alone: the map's viewport keeps its own options (its level, its overlays,
							// Repeat, Bones).
							io::JsonValue chosen = io::JsonValue::make_object();
							chosen.set("rig_model", io::json_string(here));
							workspace.request(request::set_viewport(
									played.map, viewport_change(ViewportKind::Model, "options", std::move(chosen))));
						}
					}
					ui_kit::tooltip(played.record + " in " + played.file + " plays " + played.map + " on this model.");
				}
				ImGui::EndPopup();
			}
		}
	}
}

// The model's registers by their group (DI-10, documents/model_ctrl_words): each named in the game's words,
// what writes it in the game in its tooltip, its slider over the values the game gives it (a share as a
// percent too). While the damage state drives the destroy fade, its six registers show the fade's values
// and hold none.
void ModelViewportView::Tools::registers(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock) {
	const threedi::Threedi3di3 &shown = *model.model();
	ModelViewportOptions options = model.options();
	const std::map<std::string, int64_t> now = model.ctrl_at(clock);
	const float unit = ImGui::GetFontSize();
	for (const CtrlRegisterGroup &group : ctrl_register_groups()) {
		bool titled = false;
		for (uint32_t i = 0; i < shown.ctrl.count; ++i) {
			const std::string name =
					strutil::fixed_string(shown.ctrl.registers[i].name, sizeof(shown.ctrl.registers[i].name));
			if (name.empty()) continue;
			const int ordinal = threedi::threedi_ctrl_register_ordinal(name.c_str());
			const CtrlRegisterWords &words = ctrl_register_words(ordinal);
			// A name the catalog lacks reads LOD_FRAC's register: it is listed with the level of detail.
			const char *token = ordinal < 0 ? "lod" : words.group;
			if (std::string(token) != group.token) continue;
			if (!titled) ImGui::SeparatorText(group.title);
			titled = true;
			const bool driven = model.damage_driven() && ctrl_register_is_destroy_phase(ordinal);
			const std::map<std::string, int64_t> &source = driven ? now : options.ctrl;
			const auto held = source.find(name);
			int64_t value = held == source.end() ? 0 : held->second;
			const int64_t low = ordinal < 0 ? -kRegisterRange : words.min;
			const int64_t high = ordinal < 0 ? kRegisterRange : words.max;
			char format[48];
			if (words.share) std::snprintf(format, sizeof(format), "%%lld (%.0f%%%%)", double(value) * 100.0 / 65536.0);
			else std::snprintf(format, sizeof(format), "%%lld");
			const std::string label = (ordinal < 0 ? name : std::string(words.label) + " (" + name + ")") + "##" + name;
			ImGui::SetNextItemWidth(unit * 12.0f);
			ImGui::BeginDisabled(driven);
			if (ImGui::SliderScalar(label.c_str(), ImGuiDataType_S64, &value, &low, &high, format) && !driven) {
				if (value == 0) options.ctrl.erase(name);
				else options.ctrl[name] = value;
			}
			ImGui::EndDisabled();
			ui_kit::tooltip(ordinal < 0
			                        ? name + " is no register of the game's: the loader reads it as LOD_FRAC's, which "
			                                 "nothing in the game writes."
			                        : std::string(words.driven) + "\n" + words.cite +
			                                  (driven ? "\nThe damage state drives it now (Damage)." : ""));
		}
	}
	if (ImGui::Button("Reset all")) options.ctrl.clear();
	ui_kit::tooltip("Let every register held go back to 0.");
	if (options != model.options()) set_options(workspace, model, options);
}

// The model's damage states (DI-10, preview/model_damage): the items naming it and what it is to each (each
// a Go to: the item's record, the husk and the pieces' models), the item the state plays, Intact or
// Destroyed, Play destroy (the death from tick 0, the clock run), and the death in order as the game runs it.
void ModelViewportView::Tools::damage(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock) {
	const SessionView &view = workspace.view();
	ModelViewportOptions options = model.options();
	const float unit = ImGui::GetFontSize();
	ImGui::PushTextWrapPos(unit * 30.0f);
	// Who names the model: the uses read from the graph as it stands now (their edges are what Go to opens).
	const std::vector<DamageUse> uses =
			view.findings.graph ? model_damage_uses(*view.findings.graph, model.path()) : std::vector<DamageUse>();
	const DamageUse *chosen = model.damage_use();
	ImGui::SeparatorText("Named by");
	for (size_t i = 0; i < uses.size(); ++i) {
		const DamageUse &use = uses[i];
		const char *as = use.role == DamageRole::Graphic        ? "draws it intact"
		                 : use.role == DamageRole::EnemyGraphic ? "draws it intact, to its enemies"
		                 : use.role == DamageRole::Husk         ? "draws it once destroyed (its husk)"
		                                                        : "flies its death pieces from it (its final husk)";
		const bool is_chosen = chosen && strutil::iequals(chosen->item, use.item) && chosen->field == use.field;
		ImGui::PushID(int(i));
		if (ImGui::RadioButton("##play", is_chosen)) options.damage.item = use.item;
		ui_kit::tooltip("Play this item's death.");
		ImGui::SameLine();
		const std::string line = use.item + " (" + use.file.substr(use.file.find_last_of('/') + 1) + ") " + as;
		if (ImGui::Selectable((ui_kit::fit(line, ImGui::GetContentRegionAvail().x) + "###use").c_str()) && use.edge &&
		    view.project.scan)
			window_requests::go_to(workspace, usage_target(*view.project.scan, *use.edge));
		ui_kit::tooltip("Go to " + use.item + "'s " + use.field + " in " + use.file + ".");
		ImGui::PopID();
	}
	const DamageItem &item = model.damage_item();
	const DamagePlan &plan = model.damage_plan();
	// Destroyed as: the husk the game swaps in and the model the pieces come from, each a Go to.
	if (item.found && view.project.scan) {
		const auto model_link = [&](const char *words, const std::string &name, const char *id) {
			if (name.empty()) return;
			const AssetEntry *entry = view.project.scan->find(name);
			if (!entry && !strutil::ends_with_icase(name, ".3di")) entry = view.project.scan->find(name + ".3di");
			if (!entry) {
				ImGui::TextDisabled("%s %s (the project has none)", words, name.c_str());
				return;
			}
			ImGui::PushID(id);
			if (ImGui::Selectable((std::string(words) + " " + entry->logical_name + "###model").c_str()))
				window_requests::go_to(workspace, file_target(*view.project.scan, entry->relative_path));
			ui_kit::tooltip("Go to " + entry->relative_path + ".");
			ImGui::PopID();
		};
		ImGui::SeparatorText(("Destroyed, as " + item.name).c_str());
		model_link("Its husk:", plan.husk, "husk");
		model_link("Its pieces from:", plan.pieces_from, "pieces");
	}
	// The state.
	ImGui::SeparatorText("State");
	int state = int(options.damage.state);
	ImGui::RadioButton("Intact", &state, int(DamageState::Intact));
	ImGui::SameLine();
	ImGui::RadioButton("Destroyed", &state, int(DamageState::Destroyed));
	options.damage.state = DamageState(state);
	ImGui::SameLine();
	ImGui::BeginDisabled(!item.found || !plan.swaps);
	const bool play = ImGui::Button("Play destroy");
	ImGui::EndDisabled();
	ui_kit::tooltip("Destroy the item from its death: the clock from tick 0, run.");
	if (!model.damage_note().empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped("%s", model.damage_note().c_str());
		ImGui::PopStyleColor();
	}
	if (item.found) {
		ImGui::TextDisabled("%s", plan.class_words.c_str());
		// Where the death stands on the clock.
		if (model.damage_playing()) {
			const DamageFrame frame = model.damage_frame_at(clock);
			const int32_t end = damage_fade_end_tick(plan, item);
			char where[160];
			std::snprintf(where, sizeof(where), "%s of the death: %s; the fade %s.", seconds_text(clock.ticks() / io::kTickHz).c_str(),
			              frame.husked ? "the husk drawn" : "intact still",
			              frame.fade_elapsed < 0 ? "waits" : clock.ticks() >= end ? "done" : "runs");
			ImGui::TextUnformatted(where);
		}
		ImGui::SeparatorText("The death, in order");
		for (const DamageLeg &leg : plan.legs) {
			const bool due = model.damage_playing() && clock.ticks() >= leg.tick;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(due ? ImGuiCol_Text : ImGuiCol_TextDisabled));
			std::string line = seconds_text(leg.tick / io::kTickHz) + "  " + leg.words;
			if (leg.shown == "named") line += " (named: not drawn here)";
			ImGui::TextWrapped("%s", line.c_str());
			ImGui::PopStyleColor();
			ui_kit::tooltip(leg.cite + (leg.kind == "effect"
			                                    ? std::string("\nNo effect preview on this branch: the effect is named, "
			                                                  "not drawn.")
			                                    : std::string()));
		}
	}
	ImGui::PopTextWrapPos();
	if (play) options.damage.state = DamageState::Destroyed;
	if (options != model.options()) set_options(workspace, model, options);
	if (play) {
		// The death from its tick: the clock at 0, run.
		io::JsonValue from = io::JsonValue::make_object();
		from.set("ticks", io::json_number(0));
		from.set("playing", io::JsonValue::make_bool(true));
		workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Model, "clock", std::move(from))));
	}
}

} // namespace opennova::editor
