#include "reference_fields.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include <imgui.h>

#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/project_find.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

using Targets = std::function<std::vector<ReferenceTarget>()>;

// go_to_tool over the targets `targets_of` makes (asked only while the tool is hovered, pressed or its menu open).
void go_to_tool(Workspace &workspace, const Targets &targets_of, bool pressed, const std::string &tip) {
	const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	if (pressed || hovered) {
		const std::vector<ReferenceTarget> targets = targets_of();
		if (pressed && targets.size() == 1) window_requests::go_to(workspace, targets.front());
		else if (pressed && !targets.empty()) ImGui::OpenPopup("go to");
		if (hovered)
			ui_kit::tooltip(targets.empty() ? tip
			                                : tip + (tip.empty() ? "" : "\n") + go_to_words(targets) +
			                                          "\nF12, or Ctrl+click the value; Shift+F12 lists its uses.");
	}
	if (!ImGui::BeginPopup("go to")) return;
	for (const ReferenceTarget &target : targets_of())
		if (ImGui::MenuItem(target.label.c_str())) window_requests::go_to(workspace, target);
	ImGui::EndPopup();
}

// reference_jumps over the targets `targets_of` makes (jump_targets: asked only while the item is under the pointer,
// has the keyboard, has its menu open or Ctrl is held over it).
void reference_jumps(Workspace &workspace, const Targets &targets_of) {
	const SessionView &view = workspace.view();
	if (!view.findings.graph || !view.project.scan) return;
	const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
	const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_AllowWhenOverlappedByItem);
	const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
	const bool menu = ImGui::BeginPopupContextItem("reference jumps");
	// Over its value with Ctrl held (the click's button over it active through the click, its release too).
	const bool ctrl = ImGui::GetIO().KeyCtrl && ImGui::IsMouseHoveringRect(min, max) &&
	                  ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	if (!hovered && !focused && !menu && !ctrl) return;
	const std::vector<ReferenceTarget> targets = targets_of();
	const auto usages = [&] {
		ProjectFind::open_usages(workspace, targets.front().file, targets.front().missing ? std::string() : targets.front().locator);
	};
	if (menu) {
		if (targets.empty()) ui_kit::quiet_text("Names nothing in the project.");
		for (size_t i = 0; i < targets.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			const std::string label = targets.size() == 1 ? std::string("Go to definition") : "Go to " + targets[i].label;
			if (ImGui::MenuItem(label.c_str(), i == 0 ? "F12" : nullptr)) window_requests::go_to(workspace, targets[i]);
			ui_kit::tooltip(window_requests::go_to_words(targets[i]) + (i == 0 ? " (F12, or Ctrl+click the value)" : ""));
			ImGui::PopID();
		}
		if (!targets.empty()) {
			if (ImGui::MenuItem("Find usages", "Shift+F12")) usages();
			ui_kit::tooltip("Who names " + targets.front().label + ", each a Go to.");
			const bool reveals = view.allows(EditorRequestKind::ShowInFiles);
			if (ImGui::MenuItem("Show in Files", nullptr, false, reveals) && reveals)
				workspace.request(request::show_in_files(targets.front().file));
			ui_kit::tooltip("Select " + targets.front().file + " in Files.");
		}
		ImGui::EndPopup();
	}
	if (targets.empty()) return;
	JumpSubject subject;
	subject.definition = targets;
	subject.usages_file = targets.front().file;
	if (!targets.front().missing) subject.usages_locator = targets.front().locator;
	subject.pointer = hovered || ctrl;
	workspace.offer_jump(subject);
	if (!ctrl) return;
	// Ctrl held over the value: a hand, and a click goes where it names.
	ImGui::SetCursorScreenPos(min);
	const bool clicked = ImGui::InvisibleButton("##ctrl go to", ImVec2(std::max(max.x - min.x, 1.0f), std::max(max.y - min.y, 1.0f)));
	ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	ui_kit::tooltip(window_requests::go_to_words(targets.front()) + " (Ctrl+click)");
	if (clicked) window_requests::go_to(workspace, targets.front());
}

// The state of a name as the game's lookup finds it (NotAReference for none, Unverified without a graph).
ReferenceStatus status_of(const SessionView &view, const ReferenceSubject &name) {
	if (name.kind == ReferenceKind::None || name.target.empty()) return ReferenceStatus::NotAReference;
	if (!view.findings.graph) return ReferenceStatus::Unverified;
	return view.findings.graph->resolve(name.kind, name.target, name.scope, nullptr, name.loader_arg);
}

} // namespace

std::string go_to_words(const std::vector<ReferenceTarget> &targets) {
	if (targets.empty()) return std::string();
	if (targets.size() > 1) return "Go to one of the " + std::to_string(targets.size()) + " places it leads.";
	return window_requests::go_to_words(targets.front());
}

std::vector<ReferenceTarget> jump_targets(const SessionView &view, const FieldUse &field, const Value &value) {
	if (!view.findings.graph || !view.project.scan) return {};
	std::vector<ReferenceTarget> out = reference_targets(*view.findings.graph, *view.project.scan, field, value);
	ReferenceTarget home;
	if (out.empty() && editor::reference_status(*view.findings.graph, field, value) == ReferenceStatus::Missing &&
	    missing_target(field, value, view, home))
		out.push_back(std::move(home));
	return out;
}

std::vector<ReferenceTarget> jump_targets(const SessionView &view, const ReferenceSubject &name) {
	if (!view.findings.graph || !view.project.scan) return {};
	std::vector<ReferenceTarget> out =
			reference_targets(*view.findings.graph, *view.project.scan, name.kind, name.target, name.scope, name.loader_arg);
	ReferenceTarget home;
	if (out.empty() && status_of(view, name) == ReferenceStatus::Missing && missing_target(name, view, home))
		out.push_back(std::move(home));
	return out;
}

void go_to_tool(Workspace &workspace, const FieldUse &field, const Value &value, bool pressed, const std::string &tip) {
	const SessionView &view = workspace.view();
	if (!view.findings.graph || !view.project.scan) return;
	go_to_tool(workspace, [&] { return reference_targets(*view.findings.graph, *view.project.scan, field, value); }, pressed,
	           tip);
}

void go_to_tool(Workspace &workspace, const ReferenceSubject &name, bool pressed, const std::string &tip) {
	const SessionView &view = workspace.view();
	if (!view.findings.graph || !view.project.scan) return;
	go_to_tool(workspace,
	           [&] {
		           return reference_targets(*view.findings.graph, *view.project.scan, name.kind, name.target, name.scope,
		                                    name.loader_arg);
	           },
	           pressed, tip);
}

void reference_jumps(Workspace &workspace, const FieldUse &field, const Value &value) {
	reference_jumps(workspace, [&] { return jump_targets(workspace.view(), field, value); });
}

void reference_jumps(Workspace &workspace, const ReferenceSubject &name) {
	reference_jumps(workspace, [&] { return jump_targets(workspace.view(), name); });
}

bool name_link(Workspace &workspace, const ReferenceSubject &name, const char *id, const std::string &tip) {
	if (name.target.empty()) {
		ImGui::TextDisabled("none");
		if (!tip.empty()) ui_kit::tooltip(tip);
		return false;
	}
	const SessionView &view = workspace.view();
	const ReferenceStatus status = status_of(view, name);
	const bool missing = status == ReferenceStatus::Missing;
	if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::reference_color(status));
	// Its jumps over it: the Selectable sized to its text, overlapped by the Ctrl+click's button.
	ImGui::SetNextItemAllowOverlap();
	const bool clicked = ImGui::Selectable((name.target + "##" + id).c_str(), false, ImGuiSelectableFlags_None,
	                                       ImVec2(ui_kit::text_width(name.target.c_str()), 0.0f));
	if (missing) ImGui::PopStyleColor();
	const std::vector<ReferenceTarget> targets = clicked ? jump_targets(view, name) : std::vector<ReferenceTarget>();
	if (clicked && !targets.empty()) window_requests::go_to(workspace, targets.front());
	ui_kit::tooltip_lazy([&] {
		const std::vector<ReferenceTarget> leads = jump_targets(view, name);
		const std::string where = leads.empty() ? std::string(reference_row(name.kind).label) + " " + name.target +
		                                                  ": nothing of the project defines it."
		                                        : go_to_words(leads) + "\nF12, or Ctrl+click; Shift+F12 lists its uses.";
		return tip.empty() ? where : tip + "\n" + where;
	});
	reference_jumps(workspace, name);
	return clicked;
}

bool text_reference_field(Workspace &workspace, ReferencePicker &picker, const TextPlace &place, std::string &given) {
	const SessionView &view = workspace.view();
	const ReferenceSubject name = place.subject();
	const ImGuiStyle &style = ImGui::GetStyle();
	const float frame = ImGui::GetFrameHeight();
	const float tools = ui_kit::button_width("Pick") + style.ItemSpacing.x + frame * 0.5f + style.ItemSpacing.x;
	// The box, the name as the text writes it; Enter sets a name typed.
	char text[256] = {};
	std::strncpy(text, place.value.c_str(), sizeof(text) - 1);
	ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - tools, frame * 3.0f));
	ImGui::SetNextItemAllowOverlap();
	const bool entered = ImGui::InputText("##name", text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue) && text[0] &&
	                     place.value != text;
	// A completion taken (Enter on the one marked, Tab, a click) over the text typed; else the text Enter set.
	std::string picked;
	bool done = picker.draw_completions(workspace, place, text, picked);
	if (done) given = picked;
	else if (entered) {
		given = text;
		done = true;
	}
	if (!done && ReferencePicker::accept_file(view, place.kind, place.loader_arg, picked)) {
		given = picked;
		done = true;
	}
	reference_jumps(workspace, name);
	// Pick: the names of its kind the project has.
	ImGui::SameLine();
	if (picker.draw(workspace, place, false, picked)) {
		given = picked;
		done = true;
	}
	// Its state's dot: a click its Go to where the name is found, a missing name's fixes.
	const ReferenceStatus status = status_of(view, name);
	ImGui::SameLine();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton("go to dot", ImVec2(frame * 0.5f, frame));
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + frame * 0.25f, at.y + frame * 0.5f), frame * 0.2f,
	                                            ImGui::GetColorU32(ui_kit::reference_color(status)));
	const std::string words = std::string(ui_kit::reference_word(status)) + ": " + reference_row(place.kind).label + " " +
	                          place.value;
	if (status == ReferenceStatus::Present) {
		go_to_tool(workspace, name, pressed, words);
	} else if (status == ReferenceStatus::Missing) {
		ui_kit::tooltip(words + ", which nothing of the project defines.");
		picker.draw_fixes(workspace, place, pressed);
	} else {
		ui_kit::tooltip(words);
	}
	return done;
}

} // namespace opennova::editor
