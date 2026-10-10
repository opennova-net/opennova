#include <editor/ui/mission_logic_view.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <imgui.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_uses.h>
#include <editor/graph/display_names.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

const MissionDocument *mission_of(const Document &document) { return dynamic_cast<const MissionDocument *>(&document); }

// The words a parameter's value reads in: the project's names where the asset graph stands (the
// wire's and the outline's words, documents/mission_labels.h), the document's own otherwise.
class ViewNames {
public:
	ViewNames(const SessionView &view, const MissionDocument &mission) {
		if (view.findings.graph) source_.emplace(*view.findings.graph);
		names_ = mission_label_names(mission, source_ ? &*source_ : nullptr);
	}
	ViewNames(const ViewNames &) = delete;
	ViewNames &operator=(const ViewNames &) = delete;
	const MissionNames &operator*() const { return *names_; }
	const NameSource *source() const { return source_ ? &*source_ : nullptr; }

private:
	std::optional<GraphNameSource> source_;
	std::unique_ptr<MissionNames> names_;
};

// What the type picker's filter box holds: one picker is open at a time.
char g_filter[64] = {};
// What the Move to another event picker's filter holds.
char g_events_filter[64] = {};

const ImVec4 kMuted(0.70f, 0.70f, 0.70f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.35f, 1.0f);

// The types of a list in a popup's body, by group, a line each in its tooltip, filtered by name or
// group: the one chosen (the popup closed), null while none is.
const LogicType *type_menu(bool actions, const LogicType *current) {
	const float em = ImGui::GetFontSize();
	ui_kit::filter_box("##logic_filter", g_filter, sizeof(g_filter), actions ? "Find an action" : "Find a trigger", em * 22.0f);
	const LogicType *chosen = nullptr;
	if (ImGui::BeginChild("types", ImVec2(em * 22.0f, em * 22.0f))) {
		const char *group = nullptr;
		size_t shown = 0;
		for (const LogicType &type : logic_types(actions)) {
			if (g_filter[0] && !window_requests::matches(std::string(type.title) + " " + type.group, g_filter)) continue;
			if (group != type.group) {
				group = type.group;
				ImGui::SeparatorText(group);
			}
			++shown;
			ImGui::PushID(type.type);
			ImGui::PushID(type.sub);
			if (ImGui::Selectable(ui_kit::fit(type.title, ImGui::GetContentRegionAvail().x).c_str(), &type == current))
				chosen = &type;
			ui_kit::tooltip(std::string(type.title) + "\n" + type.tip);
			ImGui::PopID();
			ImGui::PopID();
		}
		if (!shown) ui_kit::empty_state("No type matches the filter.");
	}
	ImGui::EndChild();
	if (chosen) {
		g_filter[0] = '\0';
		ImGui::CloseCurrentPopup();
	}
	return chosen;
}

// The planned edits raised as one batch (one undo step), or the refusal told.
void raise(Workspace &workspace, const Document &document, bool ok, std::vector<Edit> edits) {
	if (ok && !edits.empty()) window_requests::edits(workspace, document, std::move(edits));
}

// The events of the mission in a popup's body, each by its sentence, filtered: the one chosen.
NodeId event_menu(const MissionDocument &mission, const ViewNames &names, NodeId except) {
	const float em = ImGui::GetFontSize();
	ui_kit::filter_box("##events_filter", g_events_filter, sizeof(g_events_filter), "Find an event", em * 26.0f);
	NodeId chosen = 0;
	if (ImGui::BeginChild("events", ImVec2(em * 26.0f, em * 18.0f))) {
		size_t index = 0;
		for (const Node *row : mission.rows_of(MissionKind::Event)) {
			++index;
			if (row->id == except) continue;
			const std::string words =
			        "Event " + std::to_string(index) + ": " + mission_record_label(mission, {row->id, row->kind, 0}, names.source());
			if (g_events_filter[0] && !window_requests::matches(words, g_events_filter)) continue;
			ImGui::PushID(static_cast<int>(row->id));
			if (ImGui::Selectable(ui_kit::fit(words, ImGui::GetContentRegionAvail().x).c_str())) chosen = row->id;
			ui_kit::tooltip(words);
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	if (chosen) {
		g_events_filter[0] = '\0';
		ImGui::CloseCurrentPopup();
	}
	return chosen;
}

// A trigger's or an action's tools: Up, Down, Move to another event (a picker of the events by their
// sentences), Remove; on `row`, or a context menu's items (`menu`).
void record_tools(Workspace &workspace, const MissionDocument &mission, const ViewNames &names, const NodeAddress &record,
                  size_t index, size_t count, bool editable) {
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Up", editable && index > 0, index > 0 ? "Moves it before the one above it." : "It is the first.", true))
		window_requests::edit(workspace, mission, EditOperation::Move, record, index - 1);
	if (ui_kit::tool(row, "Down", editable && index + 1 < count,
	                 index + 1 < count ? "Moves it after the one below it." : "It is the last.", true))
		window_requests::edit(workspace, mission, EditOperation::Move, record, index + 1);
	if (ui_kit::tool(row, "Move to event...", editable, "Moves it to the end of another event's list (one undo step).", true))
		ImGui::OpenPopup("move_to");
	if (ImGui::BeginPopup("move_to")) {
		if (const NodeId to = event_menu(mission, names, record.row)) {
			std::vector<Edit> edits;
			std::string refusal;
			const bool ok = logic_move_edits(mission, record, to, SIZE_MAX, edits, refusal);
			raise(workspace, mission, ok, std::move(edits));
		}
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Remove", editable, "Removes it from its event.", true))
		window_requests::edit(workspace, mission, EditOperation::Remove, record);
}

// An event's form value as a number.
int64_t number_of(const Document &document, const NodeAddress &address, const char *field) {
	Value value;
	const int64_t *number = document.get(address, field, value) ? std::get_if<int64_t>(&value) : nullptr;
	return number ? *number : 0;
}

// A count of 64-tick steps, typed, with its seconds and its range beside it.
void steps_field(Workspace &workspace, const MissionDocument &mission, const NodeAddress &event, const char *label,
                 const char *field, int64_t steps, int64_t most, const char *tip) {
	ImGui::PushID(field);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ui_kit::tooltip(tip);
	ImGui::SameLine();
	int typed = int(steps);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
	if (ImGui::InputInt("##steps", &typed, 1, 10)) {
		const int64_t held = std::clamp<int64_t>(typed, 0, most);
		if (held != steps) window_requests::set(workspace, mission, event, field, held, true);
	}
	if (ImGui::IsItemDeactivatedAfterEdit()) window_requests::end_edit(workspace, mission.path());
	ui_kit::tooltip(tip);
	ImGui::SameLine();
	// What the steps come to in the game: past 512 the countdown wraps and ends on the next pass (S15).
	const std::string seconds = "steps = " + logic_steps_words(steps) + " (0 to " + std::to_string(most) + ")";
	if (bms::event_steps_wrap(steps))
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Warning), "%s", seconds.c_str());
	else
		ImGui::TextDisabled("%s", seconds.c_str());
	ImGui::PopID();
}

// A flag bit of the event as a tick box in words.
void flag_box(Workspace &workspace, const MissionDocument &mission, const NodeAddress &event, int64_t flags, int64_t bit,
              const char *label, const char *tip) {
	bool on = (flags & bit) != 0;
	if (ImGui::Checkbox(label, &on)) window_requests::set(workspace, mission, event, "flags", on ? (flags | bit) : (flags & ~bit), false);
	ui_kit::tooltip(tip);
}

// An event's list of triggers or actions: its heading with Add (by type, off with why where the event
// holds the most it holds), then each record in words, a click selecting it, its tools in its context
// menu.
void logic_list(Workspace &workspace, const MissionDocument &mission, const LogicEventForm &form, const ViewNames &names,
                bool actions, bool editable) {
	const NodeKind kind = k(actions ? MissionKind::Action : MissionKind::Trigger);
	std::vector<NodeId> ids;
	for (const Document::Collection &collection : mission.collections_of(form.event))
		if (collection.spec.kind == kind) ids = collection.ids;
	ImGui::PushID(actions ? "actions" : "triggers");
	const std::string heading =
	        std::string(actions ? "Actions" : "Triggers") + " (" + std::to_string(ids.size()) + " of " + std::to_string(form.most) + ")";
	ImGui::SeparatorText(heading.c_str());
	const std::string &refusal = actions ? form.action_refusal : form.trigger_refusal;
	ui_kit::WrapRow row;
	const char *add = actions ? "Add action..." : "Add trigger...";
	if (ui_kit::tool(row, add, editable && refusal.empty(),
	                 refusal.empty() ? std::string(actions ? "What the event does: pick it by name." : "What the event waits on: pick it by name.")
	                                 : refusal,
	                 true))
		ImGui::OpenPopup("add");
	if (ImGui::BeginPopup("add")) {
		if (const LogicType *type = type_menu(actions, nullptr)) {
			std::vector<Edit> edits;
			std::string error;
			const bool ok = logic_add_edits(mission, form.event.row, *type, SIZE_MAX, edits, error);
			raise(workspace, mission, ok, std::move(edits));
		}
		ImGui::EndPopup();
	}
	if (ids.empty())
		ui_kit::empty_state(actions ? "No action: the event does nothing." : "No trigger: the event fires on its first check.");
	const SessionView &view = workspace.view();
	for (size_t i = 0; i < ids.size(); ++i) {
		const NodeAddress address{form.event.row, kind, ids[i]};
		ImGui::PushID(static_cast<int>(ids[i]));
		const std::string words = std::to_string(i + 1) + ". " + mission_record_label(mission, address, names.source());
		if (ImGui::Selectable(ui_kit::fit(words, ImGui::GetContentRegionAvail().x).c_str(),
		                      view.documents.selection.holds(address)))
			window_requests::select(workspace, mission, address);
		ui_kit::tooltip(words + "\nRight-click: move it, or remove it.");
		if (ImGui::BeginPopupContextItem("tools")) {
			record_tools(workspace, mission, names, address, i, ids.size(), editable);
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	ImGui::PopID();
}

void event_form(Workspace &workspace, const MissionDocument &mission, NodeId event, InspectorTaken &taken, bool editable) {
	const ViewNames names(workspace.view(), mission);
	LogicEventForm form;
	if (!logic_event_form(mission, event, *names, form)) return;
	taken.fields = {"flags", "reset_after", "delay"};
	taken.collections = {k(MissionKind::Trigger), k(MissionKind::Action)};
	ImGui::TextWrapped("%s", form.words.sentence.c_str());
	ImGui::Spacing();
	const int64_t flags = number_of(mission, form.event, "flags");
	ImGui::BeginDisabled(!editable);
	flag_box(workspace, mission, form.event, flags, int64_t(bms::EventFlags::PreMission), "Once, at the mission's start",
	         "Checked once as the mission starts, before its first tick, and never again (the PreMission pass).");
	flag_box(workspace, mission, form.event, flags, int64_t(bms::EventFlags::PostMission), "Once, at the mission's end",
	         "Checked once as the mission ends, after its soldiers, items and buildings are gone (the PostMission pass).");
	flag_box(workspace, mission, form.event, flags, int64_t(bms::EventFlags::ResetAfter), "Repeats",
	         "Checked again after its triggers held (the wait counts from then); without it, it fires once.");
	const char *steps_tip = "In steps of 64 ticks, about 1.02 s each: the file holds 0 to 1023 of them, and past 512 the "
	                        "game's countdown wraps and ends on the next pass.";
	steps_field(workspace, mission, form.event, "Wait before acting", "delay", form.delay, form.most_steps, steps_tip);
	if (form.repeats)
		steps_field(workspace, mission, form.event, "Check again after", "reset_after", form.repeat, form.most_steps, steps_tip);
	// A start or end event is checked once and never processed again: a wait never ends, and it is never
	// checked again by its repeat [bms-event-runtime-re.md 1.2, 1.6].
	const bool once = (flags & (int64_t(bms::EventFlags::PreMission) | int64_t(bms::EventFlags::PostMission))) != 0;
	if (once && (form.delay > 0 || form.repeats)) {
		ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Warning));
		ImGui::TextWrapped("%s", "A start or end event is checked once: its wait never ends and it is never checked again.");
		ImGui::PopStyleColor();
	}
	ImGui::EndDisabled();
	logic_list(workspace, mission, form, names, false, editable);
	logic_list(workspace, mission, form, names, true, editable);
}

void record_form(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record, InspectorTaken &taken,
                 bool editable) {
	const ViewNames names(workspace.view(), mission);
	LogicForm form;
	if (!logic_form(mission, record, *names, form)) return;
	taken.fields = form.action ? std::vector<std::string>{"action_type", "action_sub_type"}
	                           : std::vector<std::string>{"condition_flags", "main_type", "sub_type"};
	// A parameter the type does not read that holds nothing is no field to fill.
	for (int slot = 0; slot < 4; ++slot) {
		bool shown = false;
		for (const LogicParam &param : form.params) shown = shown || param.slot == slot;
		for (const LogicParam &param : form.unread) shown = shown || param.slot == slot;
		if (!shown) taken.fields.push_back("param" + std::to_string(slot + 1));
	}
	taken.collections = {k(MissionKind::Trigger), k(MissionKind::Action)};
	// Its event, in words: a click goes there.
	ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
	const std::string event = mission.record_name(form.event) + ": " + form.sentence;
	ImGui::TextWrapped("%s", event.c_str());
	ImGui::PopStyleColor();
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Go to its event", true, "Selects the event this " + std::string(form.action ? "action" : "trigger") + " is in.", true))
		window_requests::select(workspace, mission, form.event);
	ImGui::Spacing();
	ImGui::BeginDisabled(!editable);
	// Its type, by name.
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Type");
	ImGui::SameLine();
	const float room = ImGui::GetContentRegionAvail().x;
	if (ui_kit::fitted_button(form.type_words, "type", room)) ImGui::OpenPopup("type");
	// A type with no row: an action type the dispatcher knows whose sub-type selects nothing, or a type the
	// game has no case for (S15).
	std::string type_tip;
	if (form.type) type_tip = std::string(form.type->group) + ": " + form.type->tip;
	else if (form.type_words.rfind("Unknown", 0) != 0) type_tip = "Its sub-type selects nothing the game does: it does nothing.";
	else type_tip = std::string("A type the game has no case for: it ") + (form.action ? "does nothing." : "reads false.");
	ui_kit::tooltip(type_tip + "\nClick to pick another; what still applies is kept.");
	if (ImGui::BeginPopup("type")) {
		if (const LogicType *type = type_menu(form.action, form.type)) {
			std::vector<Edit> edits;
			std::string error;
			const bool ok = logic_retype_edits(mission, record, *type, edits, error);
			raise(workspace, mission, ok, std::move(edits));
		}
		ImGui::EndPopup();
	}
	if (!form.action) {
		bool negated = form.negated;
		if (ImGui::Checkbox("Not: it holds when this is not so", &negated)) {
			Edit edit;
			if (logic_negate_edit(mission, record, negated, edit)) window_requests::edits(workspace, mission, {edit});
		}
		ui_kit::tooltip("The game negates the trigger's result (its condition's first bit).");
		if (!form.last) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Then");
			int join = int(form.join);
			const char *words[] = {"and", "or", "or else"};
			const char *tips[] = {"Both this and the next must hold.", "This or the next must hold.",
			                      "This or the next must hold, not both."};
			for (int i = 0; i < 3; ++i) {
				ImGui::SameLine();
				if (ImGui::RadioButton(words[i], &join, i)) {
					Edit edit;
					if (logic_join_edit(mission, record, static_cast<LogicJoin>(join), edit))
						window_requests::edits(workspace, mission, {edit});
				}
				ui_kit::tooltip(std::string(tips[i]) +
				                " The game folds the triggers left to right, so a join that changes holds what came before it "
				                "together.");
			}
			ImGui::SameLine();
			ImGui::TextUnformatted("the next trigger");
		}
	}
	ImGui::EndDisabled();
	// What it reads, in words.
	ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
	ImGui::TextWrapped("Reads: %s", form.words.c_str());
	ImGui::PopStyleColor();
	if (!form.unread.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
		ImGui::TextWrapped("It holds values its type does not read (shown below, marked ignored).");
		ImGui::PopStyleColor();
	}
	record_tools(workspace, mission, names, record, form.index, form.count, editable);
	ImGui::Separator();
}

} // namespace

bool draw_mission_inspector(Workspace &workspace, const Document &document, const NodeAddress &record, InspectorTaken &taken) {
	const MissionDocument *mission = mission_of(document);
	if (!mission) return false;
	const bool editable = workspace.view().allows(EditorRequestKind::EditRecord) && !document.blocked();
	if (!record.child && record.kind == k(MissionKind::Event)) {
		ImGui::PushID("mission_event");
		event_form(workspace, *mission, record.row, taken, editable);
		ImGui::PopID();
		return true;
	}
	if (record.child && (record.kind == k(MissionKind::Trigger) || record.kind == k(MissionKind::Action))) {
		ImGui::PushID("mission_record");
		record_form(workspace, *mission, record, taken, editable);
		ImGui::PopID();
		return true;
	}
	return false;
}

void draw_mission_uses(Workspace &workspace, const Document &document, const NodeAddress &record, InspectorTaken &taken) {
	const MissionDocument *mission = mission_of(document);
	if (!mission) return;
	// Kept while the document, the record and the graph's names stand: the Inspector draws every frame
	// (S15), and wording each use's sentence is not a frame's work.
	static MissionUsesCache cache;
	const SessionView &view = workspace.view();
	const ViewNames names(view, *mission);
	const MissionUses &uses = cache.uses(*mission, record, *names, view.findings.graph != nullptr,
	                                     view.findings.graph ? view.findings.graph->generation() : 0);
	if (uses.what.empty()) return;
	taken.own_uses = true;
	ImGui::PushID("mission_uses");
	ImGui::Separator();
	const std::string heading = uses.uses.empty() ? "No event names " + uses.what + "."
	                                              : "Used by " + std::to_string(uses.uses.size()) + " trigger" +
	                                                        (uses.uses.size() == 1 ? "" : "s") + " or action" +
	                                                        (uses.uses.size() == 1 ? "" : "s") + " of the events";
	ImGui::TextWrapped("%s", heading.c_str());
	if (!uses.inert.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
		ImGui::TextWrapped("%s", uses.inert.c_str());
		ImGui::PopStyleColor();
	}
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(uses.uses.size()));
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const MissionUse &use = uses.uses[size_t(i)];
			ImGui::PushID(i);
			const std::string line = "Event " + std::to_string(use.event_index + 1) + ": " + use.words;
			if (ImGui::Selectable(ui_kit::fit(line, ImGui::GetContentRegionAvail().x).c_str()))
				workspace.request(request::open_record(document.path(), use.record, use.field));
			ui_kit::tooltip(line + "\n\n" + use.sentence + "\n\nA click goes there.");
			ImGui::PopID();
		}
	ImGui::PopID();
}

bool mission_logic_by_name(const Document &document, const NodeAddress &record) {
	if (!mission_of(document)) return false;
	return (!record.child && record.kind == k(MissionKind::Event)) ||
	       (record.child && (record.kind == k(MissionKind::Trigger) || record.kind == k(MissionKind::Action)));
}

bool mission_adds_by_menu(NodeKind kind) { return kind == k(MissionKind::Trigger) || kind == k(MissionKind::Action); }

void draw_mission_add_menu(Workspace &workspace, const Document &document, const NodeAddress &owner, NodeKind kind) {
	const MissionDocument *mission = mission_of(document);
	if (!mission || !mission_adds_by_menu(kind)) return;
	const bool actions = kind == k(MissionKind::Action);
	if (const LogicType *type = type_menu(actions, nullptr)) {
		std::vector<Edit> edits;
		std::string error;
		const bool ok = logic_add_edits(*mission, owner.row, *type, SIZE_MAX, edits, error);
		raise(workspace, *mission, ok, std::move(edits));
	}
}

} // namespace opennova::editor
