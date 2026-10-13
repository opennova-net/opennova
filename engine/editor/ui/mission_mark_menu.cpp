#include <editor/ui/mission_mark_menu.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <imgui.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_links.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_recipes.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/mission_link_shapes.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/mission_row_menu.h>
#include <editor/ui/project_find.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// The mission's words for what its records name, by the project's names where the graph stands.
std::unique_ptr<MissionNames> names_of(const SessionView &view, const MissionDocument &mission,
		std::optional<GraphNameSource> &source) {
	if (view.findings.graph) source.emplace(*view.findings.graph);
	return mission_label_names(mission, source ? &*source : nullptr);
}

std::string capitalized(std::string words) {
	if (!words.empty() && words[0] >= 'a' && words[0] <= 'z') words[0] = char(words[0] - 'a' + 'A');
	return words;
}

// The events (and any other record of the project) that name the record: an entity by its SSN, an area by its zone
// id, each a Go to.
void events_using(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record) {
	const SessionView &view = workspace.view();
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) {
		ImGui::TextDisabled("Not known yet (the project is still being read).");
		return;
	}
	const Node *row = mission.row(record.row);
	ReferenceKind kind = ReferenceKind::MissionEntity;
	std::string name;
	if (row && !record.child && is_entity_kind(row->kind)) {
		name = std::to_string(static_cast<const EntityRow &>(*row).native.id);
	} else if (row && !record.child && row->kind == k(MissionKind::Area)) {
		kind = ReferenceKind::MissionZone;
		name = std::to_string(static_cast<const AreaRow &>(*row).native.id);
	}
	const std::vector<const GraphEdge *> users =
			name.empty() ? std::vector<const GraphEdge *>() : graph->referrers_of(kind, name, mission_scope(mission));
	if (users.empty()) {
		ImGui::TextDisabled("No event names it.");
		return;
	}
	const GraphNameSource names(*graph);
	for (size_t i = 0; i < users.size(); ++i) {
		const GraphEdge &edge = *users[i];
		std::string label = edge.record.empty() ? edge.source : edge.record;
		if (edge.source == mission.path() && edge.address.row) {
			// The event (its sentence) and the trigger or action, by the display names.
			const NodeAddress event{ edge.address.row, edge.address.kind, 0 };
			label = record_display(mission, event, &names);
			if (edge.address.child) label += ": " + record_display(mission, edge.address, &names);
		}
		ImGui::PushID(int(i));
		if (ImGui::MenuItem(ui_kit::fit(label, ImGui::GetFontSize() * 24.0f).c_str()))
			window_requests::go_to(workspace, usage_target(*view.project.scan, edge));
		ImGui::PopID();
	}
}

// New event: the recipes made over the record (an area, an entity) or over its group.
void new_event_menu(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record, bool editable,
		const MissionNames &names) {
	struct Offer {
		const MissionRecipe *recipe;
		MissionSubject subject;
	};
	std::vector<Offer> offers;
	const std::vector<MissionSubject> subjects = mission_subjects(mission, record);
	for (const MissionSubject &subject : subjects)
		for (const MissionRecipe *recipe : mission_recipes_for(subject.kind)) offers.push_back({ recipe, subject });
	if (!ImGui::BeginMenu("New event", editable && !offers.empty())) {
		ui_kit::tooltip(offers.empty() ? "No recipe is made over this record (an area, an entity or its group)."
		                               : "An event made from where this stands, on the Logic pane.");
		return;
	}
	for (size_t i = 0; i < offers.size(); ++i) {
		const Offer &offer = offers[i];
		// A group's recipe says which group ("When this group is wiped out (group 4)").
		std::string label = offer.recipe->title;
		if (offer.subject.kind == mission::ParamKind::Group && subjects.front().kind != mission::ParamKind::Group)
			label += " (" + names.group(offer.subject.value) + ")";
		ImGui::PushID(int(i));
		if (ImGui::MenuItem(label.c_str())) {
			CanvasWindowRequests out(workspace);
			std::string error;
			// The menu offers only the recipes the record (or its group) is made over: none refuses.
			mission_new_event_requests(mission, offer.recipe->token, record, out, error);
		}
		std::string tip = offer.recipe->tip;
		if (offer.recipe->events)
			tip += "\nThe shipped missions hold " + std::to_string(offer.recipe->events) + " such events in " +
			       std::to_string(offer.recipe->missions) + " missions" +
			       (*offer.recipe->seen_in ? std::string(" (") + offer.recipe->seen_in + ")" : std::string()) + ".";
		ui_kit::tooltip(tip);
		ImGui::PopID();
	}
	ImGui::EndMenu();
}

// The entities a group change sets: the record and, when it is selected, every selected entity.
std::vector<NodeAddress> grouped_entities(const SessionView &view, const MissionDocument &mission, const NodeAddress &record) {
	std::vector<NodeAddress> out;
	const auto add = [&](const NodeAddress &each) {
		const Node *row = mission.row(each.row);
		if (row && !each.child && is_entity_kind(row->kind) && std::find(out.begin(), out.end(), each) == out.end())
			out.push_back(each);
	};
	add(record);
	if (view.documents.active == mission.path() && view.documents.selection.holds(record))
		for (const NodeAddress &each : view.documents.selection.records) add(each);
	return out;
}

// Add to group: the entities' group set (an existing group by its words and how many it holds, or the lowest one
// none holds).
void group_menu(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record, bool editable,
		const MissionNames &names) {
	const std::vector<NodeAddress> entities = grouped_entities(workspace.view(), mission, record);
	if (!ImGui::BeginMenu("Add to group", editable && !entities.empty())) {
		ui_kit::tooltip(entities.empty() ? "Groups are entities': right-click an entity."
		                                 : "Set the group of this entity (and the others selected with it).");
		return;
	}
	const auto set_group = [&](int64_t group) {
		std::vector<Edit> edits;
		for (const NodeAddress &entity : entities) {
			Edit edit;
			edit.address = entity;
			edit.field = "group";
			edit.value = group;
			edits.push_back(std::move(edit));
		}
		EditorRequest set = request::edit_record(mission.path(), std::move(edits));
		set.keep_selection = true;
		workspace.request(std::move(set));
	};
	const int64_t fresh = mission_fresh_group(mission);
	bool any = false;
	for (int64_t group = 1; group < 64; ++group) {
		const size_t members = mission.group_members(group);
		if (!members) continue;
		any = true;
		const std::string label = capitalized(names.group(group)) + " (" + std::to_string(members) + ")###group" +
		                          std::to_string(group);
		if (ImGui::MenuItem(label.c_str())) set_group(group);
	}
	if (any) ImGui::Separator();
	if (ImGui::MenuItem("New group", nullptr, false, fresh > 0) && fresh > 0) set_group(fresh);
	ui_kit::tooltip(fresh > 0 ? "Group " + std::to_string(fresh) + ", the lowest no entity carries, no event names and no name is given."
	                          : "Every group of the 63 holds an entity, is named by an event or has a name.");
	if (ImGui::MenuItem("No group")) set_group(0);
	ui_kit::tooltip("Group 0: in none (what names a group names no unit of it).");
	ImGui::EndMenu();
}

// Use in the event: a trigger or an action of a type that takes the record (or its group), added at its list's end
// with the record in its slot, every other parameter its kind's default (logic_add_edits' presets).
void use_menu(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record, const NodeAddress &event,
		bool editable, const MissionNames &names) {
	const std::vector<MissionSubject> subjects = mission_subjects(mission, record);
	const Node *row = event.row ? mission.row(event.row) : nullptr;
	const bool has_event = row && row->kind == k(MissionKind::Event);
	const std::string title = has_event ? "Use in event " + std::to_string(mission.index_of(*row) + 1) : std::string("Use in the selected event");
	if (!ImGui::BeginMenu((title + "###use_in").c_str(), editable && has_event && !subjects.empty())) {
		ui_kit::tooltip(!has_event ? "Select an event (on the board or in the outline) first, then right-click what it should "
		                             "name."
		                           : "A trigger watching this, or an action acting on it, added to the event.");
		return;
	}
	for (size_t s = 0; s < subjects.size(); ++s) {
		const MissionSubject &subject = subjects[s];
		const std::string what = subject.kind == mission::ParamKind::Group ? names.group(subject.value) : "this";
		for (const bool actions : { false, true }) {
			const std::vector<MissionTypeSlot> types = mission_types_taking(subject.kind, actions);
			if (types.empty()) continue;
			const std::string heading = std::string(actions ? "An action on " : "A trigger watching ") + what;
			ImGui::PushID(int(s * 2 + (actions ? 1 : 0)));
			if (ImGui::BeginMenu(heading.c_str())) {
				const std::string refusal = logic_add_refusal(mission, event.row, actions);
				const char *group = "";
				for (size_t i = 0; i < types.size(); ++i) {
					const MissionTypeSlot &use = types[i];
					if (std::string(use.type->group) != group) {
						group = use.type->group;
						ImGui::SeparatorText(group);
					}
					ImGui::PushID(int(i));
					if (ImGui::MenuItem(use.type->title, nullptr, false, refusal.empty())) {
						std::vector<Edit> edits;
						std::string error;
						// Refused only where the event holds the most of its list, which disables the item.
						if (logic_add_edits(mission, event.row, *use.type, SIZE_MAX, edits, error, { { use.slot, subject.value } })) {
							EditorRequest add = request::edit_record(mission.path(), std::move(edits));
							add.keep_selection = true;
							workspace.request(std::move(add));
						}
					}
					ui_kit::tooltip(refusal.empty() ? std::string(use.type->tip) : refusal);
					ImGui::PopID();
				}
				ImGui::EndMenu();
			}
			ImGui::PopID();
		}
	}
	ImGui::EndMenu();
}

// Select group members: every entity of the group the record carries or is.
void select_members(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record) {
	const int64_t group = mission_group_of(mission, record);
	const bool grouped = group > 0;
	if (ImGui::MenuItem("Select group members", nullptr, false, grouped)) {
		CanvasWindowRequests out(workspace);
		std::string error;
		mission_select_group(mission, { record }, record, out, error); // a grouped record: never refused
	}
	ui_kit::tooltip(grouped ? "Select every entity of group " + std::to_string(group) + "."
	                        : "It is in no group (group 0).");
}

} // namespace

NodeAddress mission_menu_event(const NodeAddress &primary) {
	return mission_logic_record(primary) ? NodeAddress{ primary.row, k(MissionKind::Event), 0 } : NodeAddress();
}

void draw_mission_mark_items(Workspace &workspace, const MissionDocument &mission, const MissionMarkMenu &menu,
		bool editable) {
	const SessionView &view = workspace.view();
	const NodeAddress &record = menu.record;
	const Node *row = record.row ? mission.row(record.row) : nullptr;
	const bool entity = row && !record.child && is_entity_kind(row->kind);
	if (ImGui::MenuItem("Select all like this", nullptr, false, entity || menu.selected)) {
		ViewportCommand command;
		command.name = "select_same";
		command.kind = ViewportKind::Mission;
		if (entity) command.ids = { record.row };
		workspace.request(request::edit_in_viewport(mission.path(), std::move(command)));
	}
	ui_kit::tooltip("Select every entity of the mission placed from the same item.");
	const std::string locator = row ? mission.locator(record) : std::string();
	if (ImGui::MenuItem("Go to in outline", nullptr, false, !locator.empty())) {
		window_requests::go_to(workspace, window_requests::target_at(mission.path(), locator, entity ? "item" : "id"));
	}
	const bool finds = view.findings.graph && !locator.empty();
	if (ImGui::MenuItem("Find usages", "Shift+F12", false, finds) && finds)
		ProjectFind::open_usages(workspace, mission.path(), locator);
	ui_kit::tooltip("Who names it (an entity by its SSN, an area by its zone): the events and the other files, each a "
	                "Go to.");
	if (ImGui::BeginMenu("Show events using this", record.row != 0)) {
		events_using(workspace, mission, record);
		ImGui::EndMenu();
	}
	if (!row) return;
	// S24: the logic made from where it stands.
	ImGui::Separator();
	std::optional<GraphNameSource> source;
	const std::unique_ptr<MissionNames> names = names_of(view, mission, source);
	new_event_menu(workspace, mission, record, editable, *names);
	group_menu(workspace, mission, record, editable, *names);
	select_members(workspace, mission, record);
	use_menu(workspace, mission, record, menu.event, editable, *names);
}

// The outline line menu's items for a placed record (S0's seam): Frame it in the 3D view, Select group members.
void mission_space_row_items(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record) {
	const Node *row = mission.row(record.row);
	const bool placed = row && !record.child && (is_entity_kind(row->kind) || row->kind == k(MissionKind::Area));
	const bool group = record.child && record.kind == k(MissionKind::Group);
	if (!placed && !group) return;
	if (placed) {
		if (ImGui::MenuItem("Frame in the 3D view")) {
			ViewportCommand frame;
			frame.name = "frame";
			frame.kind = ViewportKind::Mission;
			frame.ids = { record.row };
			workspace.request(request::edit_in_viewport(mission.path(), std::move(frame)));
		}
		ui_kit::tooltip("The 3D view's camera on it (a double click on the line does the same).");
	}
	if (placed && !is_entity_kind(row->kind)) return;
	select_members(workspace, mission, record);
}

} // namespace opennova::editor
