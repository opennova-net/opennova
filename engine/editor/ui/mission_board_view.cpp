#include <editor/ui/mission_board_view.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/documents/mission_board.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_recipes.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_variables.h>
#include <editor/graph/display_names.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_views.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/mission_card.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

bool logic_record(const NodeAddress &address) {
	return (!address.child && address.kind == k(MissionKind::Event)) ||
	       (address.child && (address.kind == k(MissionKind::Trigger) || address.kind == k(MissionKind::Action)));
}

// What the board was built over, so it is built again only when one of them moves: the document (its
// instance, load and revision), the names (the graph's revision), what the pane shows (its filter, its folds,
// Needs attention) and the running game's report.
struct BoardKey {
	uint64_t document = 0, load = 0, revision = 0, names = 0, live = 0;
	std::string filter;
	std::vector<NodeId> folded;
	bool attention = false;
	bool operator==(const BoardKey &o) const {
		return document == o.document && load == o.load && revision == o.revision && names == o.names && live == o.live &&
		       filter == o.filter && folded == o.folded && attention == o.attention;
	}
};

class MissionBoardPane final : public DocumentPane {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &) override {
		revealing_ = false;
		key_.reset();
	}
	void reveal(const ViewEvent &event) override {
		if (!logic_record(event.address)) return;
		revealed_ = event.address;
		revealing_ = true;
	}

private:
	void toolbar(Workspace &workspace, const MissionDocument &mission, const WorkspaceView::DocumentView &shown, bool editable);
	void keys(Workspace &workspace, const MissionDocument &mission, const WorkspaceView::DocumentView &shown, bool editable);
	void set_shown(Workspace &workspace, const MissionDocument &mission, const char *member, io::JsonValue value);

	void variables(Workspace &workspace, const MissionDocument &mission);

	std::optional<BoardKey> key_;
	MissionBoard board_;
	std::vector<MissionVariable> variables_;
	std::map<int64_t, std::vector<ScriptUse>> script_variables_;
	std::map<int64_t, std::string> variable_names_;
	std::string new_variable_;
	size_t built_ = 0;
	CardState state_;
	char filter_[kWorkspaceText] = {};
	std::string filter_held_;
	bool focus_filter_ = false;
	NodeAddress revealed_;
	bool revealing_ = false;
	uint64_t selection_serial_ = 0;
};

void MissionBoardPane::set_shown(Workspace &workspace, const MissionDocument &mission, const char *member, io::JsonValue value) {
	io::JsonValue members = io::JsonValue::make_object();
	members.set("path", io::JsonValue::make_string(mission.path()));
	members.set(member, std::move(value));
	window_requests::set_workspace(workspace, "document", std::move(members));
}

void MissionBoardPane::toolbar(Workspace &workspace, const MissionDocument &mission, const WorkspaceView::DocumentView &shown,
                               bool editable) {
	const float em = ImGui::GetFontSize();
	ui_kit::WrapRow row;
	// New event: a recipe (the shipped missions' compositions, the most used first), its subject left to its chip.
	row.next(ui_kit::button_width("New event"));
	ImGui::BeginDisabled(!editable);
	if (ImGui::Button("New event")) ImGui::OpenPopup("new_event");
	ImGui::EndDisabled();
	ui_kit::tooltip("A new event at the end of the events, from what the shipped missions' events wait on.");
	if (ImGui::BeginPopup("new_event")) {
		for (const MissionRecipe &recipe : mission_recipes()) {
			if (ImGui::MenuItem(recipe.title)) {
				std::vector<Edit> edits;
				std::string error;
				if (recipe_event_edits(mission, recipe, NodeAddress(), edits, error)) window_requests::edits(workspace, mission, std::move(edits));
			}
			ui_kit::tooltip(std::string(recipe.tip) + "\n" + std::to_string(recipe.events) + " events of the shipped missions are this (" +
			                recipe.seen_in + (recipe.missions > 3 ? ", ..." : "") + ").");
		}
		ImGui::EndPopup();
	}
	// The filter (Ctrl+F), as the workspace holds it; what is typed set as it changes.
	if (filter_held_ != shown.logic_filter) {
		filter_held_ = shown.logic_filter;
		std::snprintf(filter_, sizeof(filter_), "%s", shown.logic_filter.c_str());
	}
	row.next(em * 12.0f);
	if (focus_filter_) {
		ImGui::SetKeyboardFocusHere();
		focus_filter_ = false;
	}
	if (ui_kit::filter_box("##board_filter", filter_, sizeof(filter_), "Filter the events (Ctrl+F)", em * 12.0f)) {
		filter_held_ = filter_;
		set_shown(workspace, mission, "logic_filter", io::JsonValue::make_string(filter_));
	}
	const std::string attention = "Needs attention (" + std::to_string(board_.noted) + ")";
	row.next(ui_kit::checkbox_width(attention.c_str()));
	bool attend = shown.logic_attention;
	if (ImGui::Checkbox(attention.c_str(), &attend)) set_shown(workspace, mission, "logic_attention", io::JsonValue::make_bool(attend));
	ui_kit::tooltip("Only the events the board notes something on (an event that does nothing, a value not set, an area "
	                "the game drops...): the editor's reading, never a build's refusal.");
	const bool all_folded = !board_.cards.empty() &&
	                        std::all_of(board_.cards.begin(), board_.cards.end(), [](const BoardCard &card) { return card.folded; });
	const char *fold = all_folded ? "Unfold all" : "Fold all";
	row.next(ui_kit::button_width(fold));
	if (ImGui::Button(fold)) {
		io::JsonValue ids = io::JsonValue::make_array();
		if (!all_folded)
			for (const Node *event : mission.rows_of(MissionKind::Event)) ids.push(io::JsonValue::make_number(double(event->id)));
		set_shown(workspace, mission, "logic_folded", std::move(ids));
	}
	ImGui::TextDisabled("%zu of %zu events", board_.cards.size(), board_.events);
}

// The variables the events use (S24 A7), folded at first: each by its name with where it is used, a click there
// selecting that trigger or action; what a new one is called.
void MissionBoardPane::variables(Workspace &workspace, const MissionDocument &mission) {
	// The events' variables and the script's (its V#), by number.
	std::map<int64_t, const MissionVariable *> events;
	for (const MissionVariable &variable : variables_) events[variable.index] = &variable;
	std::vector<int64_t> indexes;
	for (const auto &[index, variable] : events) indexes.push_back(index);
	for (const auto &[index, lines] : script_variables_)
		if (!events.count(index)) indexes.push_back(index);
	std::sort(indexes.begin(), indexes.end());
	const std::string heading = "Variables (" + std::to_string(indexes.size()) + ")###variables";
	if (!ImGui::CollapsingHeader(heading.c_str())) return;
	if (indexes.empty()) ImGui::TextDisabled("No trigger or action, and no line of the script, uses a variable.");
	for (const int64_t index : indexes) {
		const auto event = events.find(index);
		const auto scripted = script_variables_.find(index);
		const size_t count = (event != events.end() ? event->second->uses.size() : 0) +
		                     (scripted != script_variables_.end() ? scripted->second.size() : 0);
		const std::string name = event != events.end() ? event->second->name : variable_names_[index];
		ImGui::PushID(int(index));
		const std::string label = name + " (" + std::to_string(count) + " use" + (count == 1 ? ")" : "s)");
		if (ImGui::TreeNode(label.c_str())) {
			if (event != events.end())
				for (size_t i = 0; i < event->second->uses.size(); ++i) {
					const MissionVariableUse &use = event->second->uses[i];
					ImGui::PushID(int(i));
					const std::string line = "Event " + std::to_string(use.event_index + 1) + ": " + use.words;
					if (ImGui::Selectable(ui_kit::fit(line, ImGui::GetContentRegionAvail().x).c_str()))
						window_requests::select(workspace, mission, use.record);
					ui_kit::tooltip(line);
					ImGui::PopID();
				}
			if (scripted != script_variables_.end())
				for (size_t i = 0; i < scripted->second.size(); ++i) {
					const ScriptUse &use = scripted->second[i];
					ImGui::PushID(int(1000 + i));
					const std::string line = use.file + ", line " + std::to_string(use.line);
					if (ImGui::Selectable(ui_kit::fit(line, ImGui::GetContentRegionAvail().x).c_str()))
						workspace.request(request::open_document(use.file, use.locator));
					ui_kit::tooltip("The mission's script uses it there: open it.");
					ImGui::PopID();
				}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::TextDisabled("A new variable is %s.", new_variable_.c_str());
}

void MissionBoardPane::keys(Workspace &workspace, const MissionDocument &mission, const WorkspaceView::DocumentView &shown,
                            bool editable) {
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
	const SessionView &view = workspace.view();
	const NodeAddress selected = view.documents.active == mission.path() ? view.documents.selection.primary : NodeAddress();
	const auto card_of = [&](NodeId event) -> const BoardCard * {
		for (const BoardCard &card : board_.cards)
			if (card.event.row == event) return &card;
		return nullptr;
	};
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) focus_filter_ = true;
	// Up and Down: the card before or after the selected one.
	for (const bool down : {false, true}) {
		if (!ImGui::Shortcut(down ? ImGuiKey_DownArrow : ImGuiKey_UpArrow, ImGuiInputFlags_Repeat) || board_.cards.empty()) continue;
		size_t at = SIZE_MAX;
		for (size_t i = 0; i < board_.cards.size(); ++i)
			if (board_.cards[i].event.row == selected.row) at = i;
		const size_t to = at == SIZE_MAX ? 0 : down ? std::min(at + 1, board_.cards.size() - 1) : at == 0 ? 0 : at - 1;
		window_requests::select(workspace, mission, board_.cards[to].event);
	}
	const BoardCard *card = selected.row ? card_of(selected.row) : nullptr;
	if (!card) return;
	// Space folds or unfolds it; Enter opens it in the Inspector.
	if (ImGui::Shortcut(ImGuiKey_Space)) {
		std::vector<NodeId> folded = shown.logic_folded;
		const auto at = std::find(folded.begin(), folded.end(), card->event.row);
		if (at == folded.end()) folded.push_back(card->event.row);
		else folded.erase(at);
		io::JsonValue ids = io::JsonValue::make_array();
		for (const NodeId id : folded) ids.push(io::JsonValue::make_number(double(id)));
		set_shown(workspace, mission, "logic_folded", std::move(ids));
	}
	if (ImGui::Shortcut(ImGuiKey_Enter)) window_requests::focus(workspace, "inspector");
	if (!editable) return;
	// The clipboard over the selection, Delete, a line moved up or down, a trigger or an action added.
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) window_requests::clipboard(workspace, mission, EditorRequestKind::Copy);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) window_requests::clipboard(workspace, mission, EditorRequestKind::Cut);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) window_requests::clipboard(workspace, mission, EditorRequestKind::Paste);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) window_requests::clipboard(workspace, mission, EditorRequestKind::Duplicate);
	if (ImGui::Shortcut(ImGuiKey_Delete)) window_requests::edit(workspace, mission, EditOperation::Remove, selected);
	if (selected.child) {
		const bool action = selected.kind == k(MissionKind::Action);
		const std::vector<BoardLine> &lines = action ? card->actions : card->triggers;
		for (const BoardLine &line : lines) {
			if (line.record != selected) continue;
			if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_UpArrow) && line.index > 0)
				window_requests::edit(workspace, mission, EditOperation::Move, line.record, line.index - 1);
			if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_DownArrow) && line.index + 1 < lines.size())
				window_requests::edit(workspace, mission, EditOperation::Move, line.record, line.index + 1);
		}
	}
	for (const bool actions : {false, true}) {
		if (!ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | (actions ? ImGuiKey_A : ImGuiKey_T))) continue;
		state_.picker_event = card->event.row;
		state_.picker_actions = actions;
	}
}

void MissionBoardPane::draw(Workspace &workspace, const DocumentBase &document) {
	const auto *mission = dynamic_cast<const MissionDocument *>(records_of(document));
	if (!mission) return;
	const SessionView &view = workspace.view();
	const WorkspaceView::DocumentView &shown = view.workspace.document(mission->path());
	const bool editable = view.allows(EditorRequestKind::EditRecord) && !made_by_import(view, document);
	// The board, built again only when what it reads moved.
	BoardKey key;
	key.document = mission->identity();
	key.load = mission->load_generation();
	key.revision = mission->revision();
	key.names = view.revisions.stamp_of(concern_set({ViewConcern::Graph}));
	key.live = view.activity.live_logic.empty() ? 0 : view.activity.live_logic.serial + 1;
	key.filter = shown.logic_filter;
	key.folded = shown.logic_folded;
	key.attention = shown.logic_attention;
	std::optional<GraphNameSource> source;
	if (view.findings.graph) source.emplace(*view.findings.graph);
	if (!key_ || !(*key_ == key)) {
		BoardQuery query;
		query.filter = key.filter;
		query.folded = key.folded;
		query.attention = key.attention;
		const std::unique_ptr<MissionNames> names = mission_label_names(*mission, source ? &*source : nullptr);
		board_ = mission_board(*mission, *names, query, view.activity.live_logic.empty() ? nullptr : &view.activity.live_logic);
		variables_ = mission_variables(*mission, *names);
		script_variables_ = script_variable_uses(view, *mission);
		new_variable_ = names->variable(new_variable_index(view, *mission));
		variable_names_.clear();
		for (const auto &[index, lines] : script_variables_) variable_names_[index] = names->variable(index);
		key_ = key;
		++built_;
	}
	toolbar(workspace, *mission, shown, editable);
	keys(workspace, *mission, shown, editable);
	variables(workspace, *mission);
	ImGui::Separator();
	// The selection moved (a new event made, a record selected in a view): its card brought into sight.
	const bool here = view.documents.active == mission->path();
	const NodeAddress selected = here ? view.documents.selection.primary : NodeAddress();
	if (here && view.documents.selection.serial != selection_serial_) {
		selection_serial_ = view.documents.selection.serial;
		if (logic_record(selected)) {
			revealed_ = selected;
			revealing_ = true;
		}
	}
	CardContext context{workspace, *mission, source ? &*source : nullptr, editable, selected, shown.pick, state_};
	if (ImGui::BeginChild("cards")) {
		if (board_.cards.empty())
			ui_kit::empty_state(board_.events ? "No event matches the filter." : "The mission has no event yet.",
			                    board_.events ? "Clear the filter, or Needs attention." : "New event adds one.");
		for (const BoardCard &card : board_.cards) {
			draw_mission_card(context, card);
			if (revealing_ && revealed_.row == card.event.row) {
				ImGui::SetScrollHereY(0.0f);
				revealing_ = false;
			}
		}
		revealing_ = false;
	}
	ImGui::EndChild();
}

} // namespace

std::unique_ptr<DocumentPane> make_mission_board_pane() { return std::make_unique<MissionBoardPane>(); }

} // namespace opennova::editor
