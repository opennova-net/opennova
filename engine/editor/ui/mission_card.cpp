#include <editor/ui/mission_card.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_uses.h>
#include <editor/documents/mission_variables.h>
#include <editor/documents/mission_links.h>
#include <editor/documents/mission_recipes.h>
#include <editor/documents/mission_table.h>
#include <base/io/strutil.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/text_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/script_assist.h>
#include <editor/session/view/session_view.h>
#include <editor/session/problem_fixes.h>
#include <editor/ui/document_views.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/field_widgets.h>
#include <editor/ui/mission_board_live.h>
#include <editor/ui/record_drag.h>
#include <editor/ui/ui_kit.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

const ImVec4 kMuted(0.62f, 0.62f, 0.62f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kTypeWords(0.70f, 0.82f, 1.00f, 1.0f);
const ImVec4 kNote(0.85f, 0.65f, 0.45f, 1.0f);

// A batch raised as one edit_record that leaves the selection as it is (the card stays selected).
void raise_kept(CardContext &context, std::vector<Edit> edits) {
	if (edits.empty()) return;
	EditorRequest request = request::edit_record(context.mission.path(), std::move(edits));
	request.keep_selection = true;
	context.workspace.request(std::move(request));
}

void set_field(CardContext &context, const NodeAddress &record, const std::string &field, Value value, bool coalesce = false) {
	Edit set;
	set.address = record;
	set.field = field;
	set.value = std::move(value);
	set.coalesce = coalesce;
	raise_kept(context, {set});
}

// Words and values placed side by side, a piece on the next line (at `left`) where it does not fit the room.
class Flow {
public:
	Flow(float left, float right) : left_(left), right_(right) {}
	// How wide a line of it is.
	float room() const { return std::max(1.0f, right_ - left_); }
	void place(float width, float gap = 0.0f) {
		if (started_) {
			ImGui::SameLine(0.0f, gap);
			if (ImGui::GetCursorPosX() + width > right_ && ImGui::GetCursorPosX() > left_ + gap + 0.5f) {
				ImGui::NewLine();
				ImGui::SetCursorPosX(left_);
			}
		}
		started_ = true;
	}

private:
	float left_, right_;
	bool started_ = false;
};

// A run's text split after each space, so the words wrap.
std::vector<std::string> words_of(const std::string &text) {
	std::vector<std::string> out;
	std::string word;
	for (const char c : text) {
		word += c;
		if (c == ' ') {
			out.push_back(word);
			word.clear();
		}
	}
	if (!word.empty()) out.push_back(word);
	return out;
}

const FieldSchema *schema_of(const MissionDocument &mission, NodeKind kind, const std::string &field) {
	for (const FieldSchema &schema : mission.fields(kind))
		if (schema.id == field) return &schema;
	return nullptr;
}

// Whether a line's chip at `slot` forms a Triggered Text key (Show text's number): New text makes one.
bool makes_text(const FieldUse &use) {
	return use.picks == ReferenceKind::TextId && use.key_prefix && *use.key_prefix;
}

// The New text popup of a chip that forms a text key: what is typed made the next free key of the mission's own
// table (set_string_text's create), the chip set to its number.
void new_text_popup(CardContext &context, const BoardLine &line, const BoardChip &chip, const FieldUse &use) {
	if (!ImGui::BeginPopup("new_text")) return;
	const float em = ImGui::GetFontSize();
	const size_t slash = use.scope.find('/');
	const std::string table = use.scope.substr(0, slash);
	const SessionView &view = context.workspace.view();
	const bool held = view.project.scan && view.project.scan->find(table);
	// The game install's table of the mission's name: imported (with what it needs), never shadowed by a new one.
	const std::string installed = held ? std::string() : install_file_name(view, table);
	ImGui::TextUnformatted("New text");
	if (!installed.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * 24.0f);
		ImGui::TextColored(kWarn, "The project has no %s, but the game install has one: the mission's title and its other "
		                          "texts. Import it into the project first; a new %s would hide it.",
		                   table.c_str(), table.c_str());
		ImGui::PopTextWrapPos();
		ImGui::BeginDisabled(!view.allows(EditorRequestKind::PreviewInstallImport));
		if (ImGui::Button(("Import " + installed + "...").c_str())) {
			context.workspace.request(request::preview_install_import({installed}, view.project.import_dependencies));
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ui_kit::tooltip("Opens the import dialog on the game's own " + installed +
		                (view.project.import_dependencies ? ", with the files it needs." : "."));
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (ImGui::IsWindowAppearing()) {
		context.state.new_text[0] = '\0';
		ImGui::SetKeyboardFocusHere();
	}
	ImGui::SetNextItemWidth(em * 24.0f);
	const bool entered = ImGui::InputText("##new_text", context.state.new_text, sizeof(context.state.new_text),
	                                      ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * 24.0f);
	if (held)
		ImGui::TextDisabled("Added to %s's %s under the next free key, and the %s set to its number: two files changed, "
		                    "which Save All writes.",
		                    table.c_str(), use.scope.substr(slash + 1).c_str(), chip.label.c_str());
	else
		ImGui::TextColored(kWarn, "The mission has no text table of its own: the game reads %s in its place, which the editor "
		                          "never writes. Enter makes %s, which the game then reads for this mission instead (its "
		                          "title and its other texts too).",
		                   use.scope_alternate.c_str(), table.c_str());
	ImGui::PopTextWrapPos();
	if (entered && context.state.new_text[0] && context.editable) {
		EditorRequest create = request::set_string_text(context.mission.path(), line.record, chip.field, context.state.new_text);
		create.create = true;
		context.workspace.request(std::move(create));
		context.state.new_text[0] = '\0';
		ImGui::CloseCurrentPopup();
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// One value of a line: picked by name (the reference picker's frame: its list, a record dropped on it), a number
// typed with its unit, or a choice; its menu Pick in view where a mark gives it, and New text where it forms a
// text key. Unset in the warning colour.
void draw_chip(CardContext &context, Flow &flow, const BoardLine &line, const BoardChip &chip, const std::string &words) {
	const FieldSchema *schema = schema_of(context.mission, line.record.kind, chip.field);
	if (!schema) return;
	const FieldUse use = context.mission.field_on(line.record, *schema);
	const SessionView &view = context.workspace.view();
	const ImGuiStyle &style = ImGui::GetStyle();
	ImGui::PushID(chip.slot);
	Value value = int64_t(chip.value);
	const bool by_name = field_widgets::picks_by_name(use);
	const std::string raw = std::to_string(chip.value);
	if (chip.unset) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.32f, 0.10f, 0.85f));
	ImGui::BeginDisabled(!context.editable);
	if (chip.kind == mission::ParamKind::MissionVar) {
		// A variable: picked from those the events use, by name, or a new one (the lowest no event uses).
		const std::unique_ptr<MissionNames> names = mission_label_names(context.mission, context.names);
		const std::string preview = names->variable(chip.value);
		const float width = std::min(ui_kit::text_width(preview.c_str()) + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f, flow.room());
		flow.place(width, style.ItemSpacing.x * 0.5f);
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("##variable", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
			// The events' and the script's, by number.
			std::map<int64_t, size_t> used;
			const std::map<int64_t, std::vector<ScriptUse>> scripted = script_variable_uses(view, context.mission);
			for (const MissionVariable &variable : mission_variables(context.mission, *names)) used[variable.index] += variable.uses.size();
			for (const auto &[index, lines] : scripted) used[index] += lines.size();
			for (const auto &[index, count] : used) {
				ImGui::PushID(int(index));
				const std::string label = names->variable(index) + "  (" + std::to_string(count) + " use" + (count == 1 ? ")" : "s)");
				if (ImGui::Selectable(label.c_str(), index == chip.value)) set_field(context, line.record, chip.field, index);
				ImGui::PopID();
			}
			const int64_t fresh = mission_new_variable(context.mission, script_variable_indexes(scripted));
			const std::string label = "New variable: " + names->variable(fresh);
			if (ImGui::Selectable(label.c_str())) set_field(context, line.record, chip.field, fresh);
			ui_kit::tooltip("The lowest number no trigger or action uses, the mission names none, and the mission's script "
			                "does not use.");
			ImGui::EndCombo();
		}
	} else if (by_name) {
		DisplayName shown;
		shown.text = chip.unset ? "(" + chip.label + ": not set)" : words;
		shown.raw = raw;
		shown.dangling = chip.missing;
		const float width = std::min({ui_kit::text_width(shown.text.c_str()) + ui_kit::text_width(raw.c_str()) + style.ItemSpacing.x +
		                                      ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f,
		                              ImGui::GetFontSize() * 22.0f, flow.room()});
		flow.place(width, style.ItemSpacing.x * 0.5f);
		ImGui::SetNextItemWidth(width);
		std::string picked;
		const FieldUse picking = picked_as(use);
		if (context.state.picker.draw_field(context.workspace, context.mission, line.record, picking, value, shown, false, picked))
			set_field(context, line.record, chip.field, field_widgets::picked_value(picking, picked));
	} else {
		std::vector<FieldChoice> own;
		const std::vector<FieldChoice> &choices = context.mission.choices_on(line.record, use, own);
		const float width = std::min(choices.empty() ? ImGui::GetFontSize() * 4.5f + (*chip.unit ? ui_kit::text_width(chip.unit) + style.ItemSpacing.x : 0.0f)
		                                             : std::min(ui_kit::text_width(words.c_str()) + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f,
		                                                        ImGui::GetFontSize() * 16.0f),
		                             flow.room());
		flow.place(width, style.ItemSpacing.x * 0.5f);
		ImGui::SetNextItemWidth(width);
		field_widgets::Edited edited;
		if (choices.empty()) edited = field_widgets::number(*schema, value, true);
		else edited = field_widgets::choice(*schema, choices, value, context.state.choice_typed);
		if (edited.changed) set_field(context, line.record, chip.field, value, edited.coalesce);
		if (edited.finished) window_requests::end_edit(context.workspace, context.mission.path());
	}
	ImGui::EndDisabled();
	if (chip.unset) ImGui::PopStyleColor();
	// A record dropped on a by-name chip (an outline row, a card, the Inspector's heading) is the picker's frame's to
	// take (ReferencePicker::draw_field accepts it): what it is to the slot, one set.
	ui_kit::tooltip_lazy([&] {
		return chip.label + ": " + (chip.unset ? std::string("not set") : words) + (by_name ? "\nThe file holds " + raw : std::string()) +
		       (chip.missing ? "\nIt names nothing the mission holds." : std::string()) + "\nRight-click for more.";
	});
	// Its menu, and its New text opened as the line's add asked.
	MissionPickSlot slot;
	const bool marks = mission_pick_slot(context.mission, line.record, chip.slot, slot) && slot.marks;
	const bool text = makes_text(use);
	if (ImGui::BeginPopupContextItem("chip_menu")) {
		// An event is picked on the board: a card's title clicked answers the pick.
		const bool event = slot.kind == mission::ParamKind::Event;
		if (ImGui::MenuItem(event ? "Pick on the board" : "Pick in view", nullptr, false, (marks || event) && context.editable)) {
			// The board shown where it is hidden: its cards' titles answer the pick, and its Esc ends it.
			if (event && !view.workspace.document(context.mission.path()).logic)
				window_requests::show_logic(context.workspace, context.mission.path(), true);
			window_requests::pick_in_view(context.workspace, context.mission.path(), line.record, chip.slot);
		}
		ui_kit::tooltip(event   ? std::string("Click a card's title on the board to set it (Esc cancels).")
		                : marks ? "Click " + slot.what + " in the 3D view or the map to set it (Esc cancels)."
		                        : std::string("No mark in the views gives this value."));
		if (text && ImGui::MenuItem("New text...", nullptr, false, context.editable)) context.state.open_new_text = true;
		ImGui::EndPopup();
	}
	if (text && context.state.open_new_text && context.state.open_event == 0) {
		context.state.open_new_text = false;
		ImGui::OpenPopup("new_text");
	}
	if (text) new_text_popup(context, line, chip, use);
	ImGui::PopID();
}

// A line's runs and chips, flowing from `left`: the type's own words open the retype picker, a parameter's
// run is its chip, a note muted; the chips its words do not place after them, each with its label.
void draw_runs(CardContext &context, Flow &flow, const BoardLine &line, bool &retype) {
	std::vector<bool> drawn(4, false);
	const auto chip_of = [&](int slot) -> const BoardChip * {
		for (const BoardChip &chip : line.chips)
			if (chip.slot == slot) return &chip;
		return nullptr;
	};
	for (size_t r = 0; r < line.runs.size(); ++r) {
		const BoardRun &run = line.runs[r];
		const BoardChip *chip = run.slot >= 0 && run.slot < 4 ? chip_of(run.slot) : nullptr;
		if (chip && !drawn[size_t(run.slot)]) {
			drawn[size_t(run.slot)] = true;
			// A run that is a chip's words with words of its own after them (" (never recorded...)") keeps them.
			draw_chip(context, flow, line, *chip, run.text);
			continue;
		}
		const bool type_words = run.slot == kLogicTypeSlot;
		ImGui::PushStyleColor(ImGuiCol_Text, type_words ? kTypeWords : run.slot < 0 ? kNote : ImGui::GetStyleColorVec4(ImGuiCol_Text));
		int index = 0;
		for (const std::string &whole : words_of(run.text)) {
			// A word wider than a whole line is cut to it (its run's words whole in the line's tooltip).
			const std::string word = ui_kit::text_width(whole.c_str()) > flow.room() ? ui_kit::fit(whole, flow.room()) : whole;
			flow.place(ui_kit::text_width(word.c_str()));
			ImGui::PushID(int(r) * 1000 + index++);
			ImGui::TextUnformatted(word.c_str());
			if (type_words && context.editable && ImGui::IsItemHovered()) {
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) retype = true;
			}
			ImGui::PopID();
		}
		ImGui::PopStyleColor();
		if (type_words) ui_kit::tooltip_lazy([&] { return line.type_words + "\nClick to give it another type."; });
	}
	for (const BoardChip &chip : line.chips) {
		if (drawn[size_t(chip.slot)]) continue;
		const std::string label = ui_kit::fit(chip.label + ":", flow.room());
		flow.place(ui_kit::text_width(label.c_str()), ImGui::GetStyle().ItemSpacing.x);
		ImGui::TextDisabled("%s", label.c_str());
		draw_chip(context, flow, line, chip, chip.words);
	}
}

// The menu of a line: select it, negate a trigger, move it up or down, remove it.
void line_menu(CardContext &context, const BoardLine &line, size_t count) {
	if (!ImGui::BeginPopup("line_menu")) return;
	const MissionDocument &mission = context.mission;
	if (ImGui::MenuItem("Select")) window_requests::select(context.workspace, mission, line.record);
	if (!line.action) {
		Edit negate;
		if (ImGui::MenuItem("Not: holds when this is not so", nullptr, line.negated, context.editable) &&
		    logic_negate_edit(mission, line.record, !line.negated, negate))
			raise_kept(context, {negate});
	}
	if (ImGui::MenuItem("Move up", "Alt+Up", false, context.editable && line.index > 0))
		window_requests::edit(context.workspace, mission, EditOperation::Move, line.record, line.index - 1);
	if (ImGui::MenuItem("Move down", "Alt+Down", false, context.editable && line.index + 1 < count))
		window_requests::edit(context.workspace, mission, EditOperation::Move, line.record, line.index + 1);
	if (ImGui::MenuItem("Remove", "Delete", false, context.editable))
		window_requests::edit(context.workspace, mission, EditOperation::Remove, line.record);
	ImGui::EndPopup();
}

// A line dragged over `rect` (a line of the card, its insertion before `position`; or its "+ trigger" / "+ action",
// SIZE_MAX the end): the card's sentence as the move would leave it shown while it hovers, the move raised as it
// drops (one batch). A trigger goes among triggers alone, an action among actions; another document's takes
// nothing.
void drop_line(CardContext &context, const BoardCard &card, bool actions, size_t position, const ImRect &rect, ImGuiID id) {
	if (!context.editable || !ImGui::BeginDragDropTargetCustom(rect, id)) return;
	const ImGuiPayload *held = ImGui::GetDragDropPayload();
	if (held && held->IsDataType(kLogicLineDragPayload) && held->Data && size_t(held->DataSize) == sizeof(LogicLineDrag)) {
		const LogicLineDrag drag = *static_cast<const LogicLineDrag *>(held->Data);
		const bool fits = drag.document == context.mission.identity() &&
		                  drag.record.kind == k(actions ? MissionKind::Action : MissionKind::Trigger);
		// Within its own list, the place it takes once it has left its own.
		size_t to = position;
		if (fits && drag.record.row == card.event.row && position != SIZE_MAX) {
			const std::vector<BoardLine> &lines = actions ? card.actions : card.triggers;
			for (const BoardLine &line : lines)
				if (line.record == drag.record && line.index < position) to = position - 1;
		}
		if (fits) {
			const std::unique_ptr<MissionNames> names = mission_label_names(context.mission, context.names);
			const std::string sentence = logic_move_sentence(context.mission, drag.record, card.event.row, to, *names);
			// The event it leaves, as the move leaves it: a trigger-less one holds on its first check.
			bool untriggered = false;
			const std::string left = logic_move_left_sentence(context.mission, drag.record, card.event.row, to, *names, &untriggered);
			if (!sentence.empty()) {
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
				ImGui::TextDisabled("%s then reads:", card.title.c_str());
				ImGui::TextUnformatted(sentence.c_str());
				if (!left.empty()) {
					size_t from = 0;
					const std::vector<const Node *> events = context.mission.rows_of(MissionKind::Event);
					for (size_t i = 0; i < events.size(); ++i)
						if (events[i]->id == drag.record.row) from = i;
					const std::string title = "Event " + std::to_string(from + 1);
					ImGui::Spacing();
					ImGui::TextDisabled("%s then reads:", title.c_str());
					ImGui::TextUnformatted(left.c_str());
					if (untriggered) {
						const Node *row = context.mission.row(drag.record.row);
						const bool waits = row && row->kind == k(MissionKind::Event) &&
						                   static_cast<const EventRow *>(row)->native.event.delay > 0;
						ImGui::TextColored(kWarn, "%s is left with no trigger: it %s, as the mission runs.", title.c_str(),
						                   waits ? "acts after its wait" : "fires right away");
					}
				}
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			if (ImGui::AcceptDragDropPayload(kLogicLineDragPayload, ImGuiDragDropFlags_AcceptNoPreviewTooltip)) {
				std::vector<Edit> edits;
				std::string error;
				if (logic_move_edits(context.mission, drag.record, card.event.row, to, edits, error)) raise_kept(context, std::move(edits));
			}
		}
	}
	ImGui::EndDragDropTarget();
}

// One trigger or action line: its "+" (add before it), its lead word ("When", the join to the one before,
// "Then"), its runs and chips; the line selected is outlined.
void draw_line(CardContext &context, const BoardCard &card, const BoardLine &line, const char *lead, float lead_width,
               float right, CardAsked &asked) {
	ImGui::PushID(static_cast<int>(line.record.child));
	const float x0 = ImGui::GetCursorPosX();
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const std::string &refusal = line.action ? card.action_refusal : card.trigger_refusal;
	ImGui::BeginDisabled(!context.editable || !refusal.empty());
	if (ImGui::SmallButton("+")) {
		context.state.add_event = card.event.row;
		context.state.add_actions = line.action;
		context.state.add_position = line.index;
		context.state.retype = NodeAddress();
		asked.picker = true;
	}
	ImGui::EndDisabled();
	ui_kit::tooltip(refusal.empty() ? std::string(line.action ? "Add an action before this one." : "Add a trigger before this one.")
	                                : refusal);
	ImGui::SameLine();
	ImGui::SetCursorPosX(x0 + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
	// The grip: dragged, the line moves to where it is dropped (before another line, or on a "+ trigger" or "+
	// action"), its card's sentence as it would then read shown there.
	ImGui::TextDisabled("::");
	if (context.editable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
		LogicLineDrag drag;
		drag.document = context.mission.identity();
		drag.record = line.record;
		ImGui::SetDragDropPayload(kLogicLineDragPayload, &drag, sizeof(drag));
		ImGui::TextUnformatted(ui_kit::fit(logic_runs_text(line.runs), ImGui::GetFontSize() * 24.0f).c_str());
		ImGui::EndDragDropSource();
	}
	ui_kit::tooltip("Drag to move it: before another line, or onto + trigger or + action.");
	ImGui::SameLine();
	// The lead: a trigger's join to the one before is the one before's (the chain's fold reads the previous
	// trigger's join), chosen from its menu.
	ImGui::TextDisabled("%s", lead);
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) window_requests::select(context.workspace, context.mission, line.record);
	if (!line.action && line.index > 0) {
		if (ImGui::IsItemHovered() && context.editable) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && context.editable) ImGui::OpenPopup("join");
		ui_kit::tooltip("How it joins the trigger before (the one before holds the join): click to choose.");
		if (ImGui::BeginPopup("join")) {
			const BoardLine &before = card.triggers[line.index - 1];
			for (const LogicJoin join : {LogicJoin::And, LogicJoin::Or, LogicJoin::Xor}) {
				Edit edit;
				if (ImGui::MenuItem(logic_join_words(join), nullptr, before.join == join) &&
				    logic_join_edit(context.mission, before.record, join, edit))
					raise_kept(context, {edit});
			}
			ImGui::EndPopup();
		}
	}
	float left = x0 + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2.0f + ui_kit::text_width("::") + lead_width;
	// Too narrow beside its lead (a narrow pane, the Inspector): the words under it, from the line's start.
	if (right - left < ImGui::GetFontSize() * 8.0f) {
		left = x0 + ImGui::GetFrameHeight();
		ImGui::SetCursorPosX(left);
	} else {
		ImGui::SameLine();
		ImGui::SetCursorPosX(left);
	}
	Flow flow(left, right);
	bool retype = false;
	draw_runs(context, flow, line, retype);
	if (retype) {
		context.state.add_event = card.event.row;
		context.state.add_actions = line.action;
		context.state.retype = line.record;
		asked.picker = true;
	}
	// The running game's last reading of a trigger.
	const SessionView &view = context.workspace.view();
	if (!line.action && !view.activity.live_logic.empty() && view.activity.live_logic.mission == context.mission.path()) {
		ImGui::SameLine();
		draw_card_live(context.workspace, context.mission.path(), view.activity.live_logic,
		               CardLiveSpot{card.index, int(line.index)});
	}
	const ImVec2 bottom(top.x + (right - x0), ImGui::GetItemRectMax().y);
	if (context.selected == line.record)
		ImGui::GetWindowDrawList()->AddRect(ImVec2(top.x - 2.0f, top.y - 1.0f), ImVec2(bottom.x, bottom.y + 1.0f),
		                                    ImGui::GetColorU32(ImGuiCol_NavCursor), 3.0f);
	// A line dropped on this one goes before it.
	drop_line(context, card, line.action, line.index, ImRect(top, ImVec2(bottom.x, bottom.y)), ImGui::GetID("drop"));
	// A right click anywhere on the line opens its menu.
	if (ImGui::IsMouseHoveringRect(top, ImVec2(bottom.x, bottom.y)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered())
		ImGui::OpenPopup("line_menu");
	line_menu(context, line, line.action ? card.actions.size() : card.triggers.size());
	// The chip its add asked for, opened as its line first draws.
	CardState &state = context.state;
	if (state.open_event == card.event.row && state.open_action == line.action && state.open_index == line.index &&
	    context.mission.revision() != state.open_revision) {
		state.open_event = 0;
		if (state.open_new_text) {
			for (const BoardChip &chip : line.chips) {
				const FieldSchema *schema = schema_of(context.mission, line.record.kind, chip.field);
				if (!schema || !makes_text(context.mission.field_on(line.record, *schema))) continue;
				ImGui::PushID(chip.slot);
				ImGui::OpenPopup("new_text");
				ImGui::PopID();
			}
			state.open_new_text = false;
		}
	}
	ImGui::PopID();
}

// A wait as a chip: seconds typed, set once it is left (Enter, Tab or a click elsewhere; Esc puts back what it
// held), the steps the game counts beside them.
void wait_chip(CardContext &context, ui_kit::WrapRow &row, const BoardCard &card, bool repeat, const char *id,
               const char *field, int64_t steps, const char *tip) {
	const float em = ImGui::GetFontSize();
	CardState &state = context.state;
	row.next(em * 4.0f);
	const bool held = state.waiting_event == card.event.row && std::string(state.waiting_field) == field;
	double seconds = held ? state.waiting_seconds : std::round(logic_units_seconds(steps) * 10.0) / 10.0;
	ImGui::SetNextItemWidth(em * 4.0f);
	ImGui::BeginDisabled(!context.editable);
	// No EnterReturnsTrue (InputScalar asserts on it): the seconds are written as they are typed and kept here while
	// it is active (Esc's revert written too), and set as one edit once it is left.
	const bool typed = ImGui::InputDouble(id, &seconds, 0.0, 0.0, "%.1f");
	if (typed || ImGui::IsItemActive()) {
		state.waiting_event = card.event.row;
		state.waiting_field = field;
		state.waiting_seconds = seconds;
	}
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		const int64_t to = logic_seconds_steps(seconds);
		if (to != steps) set_field(context, card.event, field, to);
		state.waiting_event = 0;
	} else if (ImGui::IsItemDeactivated()) {
		state.waiting_event = 0;
	}
	ImGui::EndDisabled();
	ui_kit::tooltip(std::string(tip) + " In seconds; the game counts steps of 1.024 s (64 ticks), at most 1023.");
	char counted[48];
	std::snprintf(counted, sizeof(counted), "s (%lld %s)", static_cast<long long>(steps), steps == 1 ? "step" : "steps");
	row.next(ui_kit::text_width(counted));
	ImGui::TextDisabled("%s", counted);
	if (bms::event_steps_wrap(steps)) {
		row.next(ui_kit::text_width("(!)"));
		ImGui::TextColored(kWarn, "(!)");
		ui_kit::tooltip(board_wrap_words(repeat, steps));
	}
}

// Move event...: an event's index is its number (an Event parameter names it by it, a script's event(n) too), so
// it moves as a command: what names it listed first (the Event parameters, which the document renumbers with
// the move; a script's numbers it does not follow), then the place chosen, one Move of its row.
void move_event_popup(CardContext &context, const BoardCard &card) {
	if (!ImGui::BeginPopup("move_event")) return;
	const MissionDocument &mission = context.mission;
	const float em = ImGui::GetFontSize();
	const std::unique_ptr<MissionNames> names = mission_label_names(mission, context.names);
	const MissionUses uses = mission_uses(mission, card.event, *names);
	ImGui::TextUnformatted(("Move " + card.title).c_str());
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * 26.0f);
	if (uses.uses.empty()) ImGui::TextDisabled("No trigger or action names it.");
	else
		ImGui::TextDisabled("Named by %zu trigger%s or action%s, renumbered with it:", uses.uses.size(), uses.uses.size() == 1 ? "" : "s",
		                    uses.uses.size() == 1 ? "" : "s");
	for (const MissionUse &use : uses.uses) ImGui::BulletText("Event %zu: %s", use.event_index + 1, use.words.c_str());
	// The mission's script's event(n) of it: a number, which the move does not follow into the script.
	const std::vector<ScriptUse> scripted = script_event_uses(context.workspace.view(), mission, card.index);
	if (!scripted.empty()) {
		ImGui::TextColored(kWarn, "The mission's script names it by its number on %zu line%s, which the move does not change:",
		                   scripted.size(), scripted.size() == 1 ? "" : "s");
		for (size_t i = 0; i < scripted.size(); ++i) {
			ImGui::PushID(int(i));
			const std::string label = scripted[i].file + ", line " + std::to_string(scripted[i].line);
			if (ImGui::Selectable(label.c_str())) context.workspace.request(request::open_document(scripted[i].file, scripted[i].locator));
			ui_kit::tooltip("Open the script there.");
			ImGui::PopID();
		}
	}
	ImGui::TextColored(kWarn, "The events between the place it leaves and the place it takes are renumbered too (each place "
	                          "says which): the triggers and actions follow, a script's numbers do not.");
	ImGui::PopTextWrapPos();
	ImGui::Separator();
	const size_t count = mission.count_of(MissionKind::Event);
	if (ImGui::BeginChild("places", ImVec2(em * 26.0f, std::min(em * 16.0f, ImGui::GetFrameHeightWithSpacing() * float(count + 1))))) {
		for (size_t to = 0; to < count; ++to) {
			if (to == card.index) continue;
			// The others it renumbers: those between its place and `to` (numbers, 1 first), each one down for a move
			// later, one up for a move earlier.
			const size_t others = to > card.index ? to - card.index : card.index - to;
			const size_t first = to > card.index ? card.index + 2 : to + 1, last = to > card.index ? to + 1 : card.index;
			const std::string label = "To place " + std::to_string(to + 1) + (to == 0 ? " (first)" : to + 1 == count ? " (last)" : "") +
			                          ", " + std::to_string(others) + (others == 1 ? " other renumbered" : " others renumbered");
			ImGui::PushID(int(to));
			if (ImGui::Selectable(label.c_str())) {
				window_requests::edit(context.workspace, mission, EditOperation::Move, card.event, to);
				ImGui::CloseCurrentPopup();
			}
			const int shift = to > card.index ? -1 : 1;
			ui_kit::tooltip(first == last ? "Event " + std::to_string(first) + " becomes event " + std::to_string(int(first) + shift) + "."
			                              : "Events " + std::to_string(first) + " to " + std::to_string(last) + " become " +
			                                        std::to_string(int(first) + shift) + " to " + std::to_string(int(last) + shift) + ".");
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

std::vector<ScriptUse> script_uses_of(const std::vector<const GraphEdge *> &edges) {
	std::vector<ScriptUse> out;
	for (const GraphEdge *edge : edges) {
		ScriptUse use;
		use.file = edge->source;
		use.line = size_t(edge->span.line);
		use.locator = TextDocument::locator(size_t(edge->span.line), size_t(edge->span.column));
		out.push_back(std::move(use));
	}
	return out;
}

} // namespace

std::map<int64_t, std::vector<ScriptUse>> script_variable_uses(const SessionView &view, const MissionDocument &mission) {
	std::map<int64_t, std::vector<ScriptUse>> out;
	const AssetGraph *graph = view.findings.graph.get();
	const MissionScript script = mission_script(view, mission.path());
	if (!graph || script.path.empty()) return out;
	const std::string scope = mission_scope(mission);
	for (const GraphEdge *edge : graph->references_of(script.path)) {
		if (edge->kind != ReferenceKind::MissionVariable || !strutil::iequals(edge->scope, scope)) continue;
		const std::optional<int> index = strutil::parse_int(edge->value);
		if (!index) continue;
		std::vector<ScriptUse> one = script_uses_of({edge});
		out[*index].push_back(std::move(one.front()));
	}
	return out;
}

std::vector<ScriptUse> script_event_uses(const SessionView &view, const MissionDocument &mission, size_t index) {
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return {};
	return script_uses_of(graph->referrers_of(ReferenceKind::MissionEventLatch, std::to_string(index), mission_event_scope(mission.path())));
}

std::vector<int64_t> script_variable_indexes(const std::map<int64_t, std::vector<ScriptUse>> &uses) {
	std::vector<int64_t> out;
	for (const auto &[index, lines] : uses) out.push_back(index);
	return out;
}

bool logic_editable(const SessionView &view, const DocumentBase &document) {
	return view.allows(EditorRequestKind::EditRecord) && !document.blocked() && !made_by_import(view, document);
}

bool raise_logic_add(CardContext &context, NodeId event, const LogicType &type, size_t position,
                     const std::vector<LogicPreset> &presets) {
	std::vector<Edit> edits;
	std::string error;
	if (!logic_add_edits(context.mission, event, type, position, edits, error, presets)) return false;
	raise_kept(context, std::move(edits));
	return true;
}

void draw_mission_line(CardContext &context, const BoardCard &card, const BoardLine &line) {
	CardAsked asked;
	const float lead = std::max({ui_kit::text_width("When "), ui_kit::text_width("or else "), ui_kit::text_width("Then ")});
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	const char *word = line.action ? (line.index == 0 ? "Then" : "")
	                               : line.index == 0 ? "When" : logic_join_words(card.triggers[line.index - 1].join);
	draw_line(context, card, line, word, lead, right, asked);
	if (asked.picker) ImGui::OpenPopup("types");
	if (ImGui::BeginPopup("types")) {
		draw_type_picker(context, context.state.add_actions);
		ImGui::EndPopup();
	}
}

bool draw_type_picker(CardContext &context, bool actions) {
	CardState &state = context.state;
	const float em = ImGui::GetFontSize();
	if (ImGui::IsWindowAppearing()) {
		state.filter[0] = '\0';
		ImGui::SetKeyboardFocusHere();
	}
	ui_kit::filter_box("##type_filter", state.filter, sizeof(state.filter), actions ? "Find an action" : "Find a trigger", em * 24.0f);
	const LogicType *chosen = nullptr;
	std::vector<LogicPreset> presets;
	const auto matches = [&](const std::string &words) { return !state.filter[0] || window_requests::matches(words, state.filter); };
	const auto row = [&](const LogicType &type, const char *id) {
		ImGui::PushID(id);
		ImGui::PushID(type.type);
		ImGui::PushID(type.sub);
		const bool picked = ImGui::Selectable(ui_kit::fit(type.title, ImGui::GetContentRegionAvail().x).c_str());
		ui_kit::tooltip(std::string(type.title) + "\n" + type.tip);
		ImGui::PopID();
		ImGui::PopID();
		ImGui::PopID();
		return picked;
	};
	// Common and the recipes in sight, every type under its group's header, folded until it is opened (with a filter,
	// every match under its group's name); the list as tall as it is, the screen's room at most.
	const float most = std::max(em * 12.0f, ImGui::GetMainViewport()->WorkSize.y - em * 8.0f);
	ImGui::SetNextWindowSizeConstraints(ImVec2(em * 24.0f, 0.0f), ImVec2(em * 24.0f, most));
	if (ImGui::BeginChild("types", ImVec2(em * 24.0f, 0.0f), ImGuiChildFlags_AutoResizeY)) {
		size_t shown = 0;
		bool header = false;
		for (const LogicType *type : logic_common_types(actions)) {
			if (!matches(std::string(type->title) + " " + type->group)) continue;
			if (!header) ImGui::SeparatorText("Common");
			header = true;
			++shown;
			if (row(*type, "common")) chosen = type;
		}
		// A trigger's recipes: its type with what the recipe fixes (the player's SSN).
		header = false;
		if (!actions && !state.retype.row)
			for (const MissionRecipe &recipe : mission_recipes()) {
				const LogicType *type = recipe_trigger_type(recipe);
				if (!type || recipe.presets.empty() || !matches(std::string(recipe.title) + " " + type->title)) continue;
				if (!header) ImGui::SeparatorText("Recipes");
				header = true;
				++shown;
				ImGui::PushID(recipe.token);
				if (ImGui::Selectable(ui_kit::fit(recipe.title, ImGui::GetContentRegionAvail().x).c_str())) {
					chosen = type;
					for (const RecipePreset &preset : recipe.presets) presets.push_back({preset.slot, preset.value});
				}
				ui_kit::tooltip(std::string(recipe.title) + "\n" + recipe.tip);
				ImGui::PopID();
			}
		const char *group = nullptr;
		bool open = false;
		const bool filtering = state.filter[0] != 0;
		if (!filtering) ImGui::SeparatorText("By group");
		for (const LogicType &type : logic_types(actions)) {
			if (!matches(std::string(type.title) + " " + type.group)) continue;
			if (group != type.group) {
				group = type.group;
				if (filtering) ImGui::SeparatorText(group);
				open = filtering || ImGui::CollapsingHeader(group);
			}
			++shown;
			if (open && row(type, "all")) chosen = &type;
		}
		if (!shown) ui_kit::empty_state("No type matches the filter.");
	}
	ImGui::EndChild();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
	if (!chosen) return false;
	bool raised = false;
	if (state.retype.row) {
		std::vector<Edit> edits;
		std::string error;
		raised = logic_retype_edits(context.mission, state.retype, *chosen, edits, error);
		if (raised) raise_kept(context, std::move(edits));
	} else {
		raised = raise_logic_add(context, state.add_event, *chosen, state.add_position, presets);
		// Show text: its chip opens on New text once its line is there.
		if (raised && chosen->action && chosen->type == int32_t(bms::ActionType::OutputText)) {
			const Node *row = context.mission.row(state.add_event);
			const auto *event = row && row->kind == k(MissionKind::Event) ? static_cast<const EventRow *>(row) : nullptr;
			const size_t count = event ? event->native.actions.size() : 0;
			state.open_event = state.add_event;
			state.open_revision = context.mission.revision();
			state.open_action = true;
			state.open_index = state.add_position == SIZE_MAX || state.add_position > count ? count : state.add_position;
			state.open_new_text = true;
		}
	}
	state.retype = NodeAddress();
	state.filter[0] = '\0';
	ImGui::CloseCurrentPopup();
	return raised;
}

CardAsked draw_mission_card(CardContext &context, const BoardCard &card) {
	CardAsked asked;
	const MissionDocument &mission = context.mission;
	const SessionView &view = context.workspace.view();
	const float em = ImGui::GetFontSize();
	ImGui::PushID(static_cast<int>(card.event.row));
	const bool selected = context.selected.row == card.event.row;
	if (selected) ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_NavCursor));
	const bool in_sight = ImGui::BeginChild("card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
	if (selected) ImGui::PopStyleColor();
	// Out of sight (scrolled past): nothing drawn, its height kept from when it was; what it was asked (its picker,
	// its Move event..., a chip to open) waits in the state until it is in sight.
	if (!in_sight) {
		ImGui::EndChild();
		ImGui::PopID();
		return asked;
	}
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

	// The header: fold, the title (a click selects the event, or answers a pick waiting on an event), the live
	// state, the flags, the waits.
	if (ImGui::ArrowButton("fold", card.folded ? ImGuiDir_Right : ImGuiDir_Down)) {
		std::vector<NodeId> folded = view.workspace.document(mission.path()).logic_folded;
		const auto at = std::find(folded.begin(), folded.end(), card.event.row);
		if (at == folded.end()) folded.push_back(card.event.row);
		else folded.erase(at);
		io::JsonValue members = io::JsonValue::make_object();
		members.set("path", io::JsonValue::make_string(mission.path()));
		io::JsonValue ids = io::JsonValue::make_array();
		for (const NodeId id : folded) ids.push(io::JsonValue::make_number(double(id)));
		members.set("logic_folded", std::move(ids));
		window_requests::set_workspace(context.workspace, "document", std::move(members));
	}
	ui_kit::tooltip(card.folded ? "Unfold it (Space)." : "Fold it to its sentence (Space).");
	ImGui::SameLine();
	// The title cut to the room (a long name in a narrow pane), its id the whole title's.
	const std::string fitted = ui_kit::fit(card.title, std::max(1.0f, ImGui::GetContentRegionAvail().x));
	const std::string title = fitted + "###" + card.title;
	if (ImGui::Selectable(title.c_str(), selected, ImGuiSelectableFlags_AllowOverlap,
	                      ImVec2(ui_kit::text_width(fitted.c_str()), 0.0f))) {
		asked.selected = true;
		// A pick waiting on an event (an Event parameter's chip) takes this one.
		MissionPickSlot slot;
		if (context.pick.active() && mission_pick_slot(mission, context.pick.record, context.pick.slot, slot) &&
		    slot.kind == mission::ParamKind::Event) {
			set_field(context, context.pick.record, mission_param_field(context.pick.slot), int64_t(card.index));
			window_requests::pick_in_view(context.workspace, mission.path(), NodeAddress(), -1);
		} else {
			window_requests::select(context.workspace, mission, card.event);
		}
	}
	ui_kit::tooltip(card.words.sentence);
	// Dragged, the card carries its event onto a value that names events (an Event parameter's chip).
	record_drag_source(mission, card.event, card.title);
	if (ImGui::BeginPopupContextItem("card_menu")) {
		if (ImGui::MenuItem("Select")) window_requests::select(context.workspace, mission, card.event);
		if (ImGui::MenuItem("Move event...", nullptr, false, context.editable)) context.state.move_event = card.event.row;
		ui_kit::tooltip("Gives it another place among the events, which is its number.");
		ImGui::EndPopup();
	}
	if (context.state.move_event == card.event.row) {
		context.state.move_event = 0;
		ImGui::OpenPopup("move_event");
	}
	move_event_popup(context, card);
	if (!view.activity.live_logic.empty() && view.activity.live_logic.mission == mission.path()) {
		ImGui::SameLine();
		draw_card_live(context.workspace, mission.path(), view.activity.live_logic, CardLiveSpot{card.index, -1});
	}
	const Node *row = mission.row(card.event.row);
	const uint32_t flags = row && row->kind == k(MissionKind::Event) ? uint32_t(static_cast<const EventRow *>(row)->native.event.flags) : 0;
	ui_kit::WrapRow flags_row;
	flags_row.next(0.0f);
	const auto flag = [&](const char *label, bms::EventFlags bit, bool on, const char *tip) {
		flags_row.next(ui_kit::checkbox_width(label));
		bool value = on;
		ImGui::BeginDisabled(!context.editable);
		if (ImGui::Checkbox(label, &value)) set_field(context, card.event, "flags", int64_t(value ? flags | uint32_t(bit) : flags & ~uint32_t(bit)));
		ImGui::EndDisabled();
		ui_kit::tooltip(tip);
	};
	flag("Repeats", bms::EventFlags::ResetAfter, card.repeats,
	     "Checked again its repeat's wait after its triggers held: the wait and the repeat count down together.");
	flag("At start", bms::EventFlags::PreMission, card.at_start, "Checked once, as the mission starts.");
	flag("At end", bms::EventFlags::PostMission, card.at_end, "Checked once, as the mission ends.");
	// The type picker an add or a retype of this card asked for (the board's keys too), drawn at the card's end.
	const auto finish = [&] {
		CardState &state = context.state;
		if (state.picker_event == card.event.row) {
			state.add_event = card.event.row;
			state.add_actions = state.picker_actions;
			state.add_position = SIZE_MAX;
			state.retype = NodeAddress();
			state.picker_event = 0;
			asked.picker = true;
		}
		if (asked.picker) ImGui::OpenPopup("types");
		if (ImGui::BeginPopup("types")) {
			draw_type_picker(context, state.add_actions);
			ImGui::EndPopup();
		}
		ImGui::EndChild();
		ImGui::PopID();
	};
	if (card.folded) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", card.words.sentence.c_str());
		ImGui::PopTextWrapPos();
		finish();
		return asked;
	}
	ui_kit::WrapRow waits;
	waits.next(ui_kit::text_width("Wait"));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Wait");
	wait_chip(context, waits, card, false, "##delay", "delay", card.delay_steps, "How long after its triggers hold it acts.");
	if (card.repeats) {
		waits.next(ui_kit::text_width("again after"));
		ImGui::TextDisabled("again after");
		wait_chip(context, waits, card, true, "##repeat", "reset_after", card.repeat_steps,
		          "How long after its triggers held it is checked again: it counts down with the wait, and the game checks it on "
		          "the pass after it ends.");
	}

	// The triggers, then the actions.
	const float lead = std::max({ui_kit::text_width("When "), ui_kit::text_width("or else "), ui_kit::text_width("Then ")});
	if (card.triggers.empty()) ImGui::TextDisabled("%s", card.words.when.c_str());
	for (const BoardLine &line : card.triggers) {
		const char *word = line.index == 0 ? "When" : logic_join_words(card.triggers[line.index - 1].join);
		draw_line(context, card, line, word, lead, right, asked);
	}
	const auto add_button = [&](bool actions) {
		const std::string &refusal = actions ? card.action_refusal : card.trigger_refusal;
		ImGui::BeginDisabled(!context.editable || !refusal.empty());
		if (ImGui::SmallButton(actions ? "+ action" : "+ trigger")) {
			context.state.add_event = card.event.row;
			context.state.add_actions = actions;
			context.state.add_position = SIZE_MAX;
			context.state.retype = NodeAddress();
			asked.picker = true;
		}
		ImGui::EndDisabled();
		ui_kit::tooltip(refusal.empty() ? std::string(actions ? "Add an action, last (Ctrl+Shift+A)." : "Add a trigger, last (Ctrl+Shift+T).")
		                                : refusal);
		drop_line(context, card, actions, SIZE_MAX, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()),
		          ImGui::GetID(actions ? "drop_actions" : "drop_triggers"));
	};
	add_button(false);
	if (card.actions.empty()) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("Then no action: as it fires it is only marked fired, which a trigger's \"has fired\", a waypoint "
		                    "marker advancing on it and the script's event() read.");
		ImGui::PopTextWrapPos();
	}
	for (const BoardLine &line : card.actions) draw_line(context, card, line, line.index == 0 ? "Then" : "", lead, right, asked);
	add_button(true);
	// The notes: the editor's reading, never gating.
	if (!card.notes.empty()) {
		ImGui::PushTextWrapPos(0.0f);
		for (const BoardNote &note : card.notes) ImGui::TextColored(kNote, "- %s", note.words.c_str());
		ImGui::PopTextWrapPos();
	}
	(void)em;
	finish();
	return asked;
}

} // namespace opennova::editor
