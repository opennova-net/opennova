#include "rename_dialog.h"

#include <editor/graph/display_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstring>

#include <imgui.h>

namespace opennova::editor {
namespace {

constexpr const char *kTitle = "Rename everywhere";
constexpr const char *kBackTitle = "Rename back";

// A site of a rename, on one line, its record and its field in words (the plain-words lane, the audit's
// 8.3: "menus/main.mnu: STARTUP/MAIN/BUTTONS/OPTIONS - Text", not "string.value").
std::string site_line(const RenameSite &site) {
	const std::string place = rename_site_place(site);
	return site.file + ": " + (place.empty() ? std::string() : place + ": ") + site.before + " -> " + site.after;
}

} // namespace

EditorRequest RenameDialog::preview(const std::string &path, const std::string &locator, const std::string &field,
                                    const std::string &name, bool ask) {
	return request::preview_rename(path, locator, field, name, ask);
}

void RenameDialog::draw(Workspace &workspace) {
	const SessionView &view = workspace.view();
	const WorkspaceView::Rename &held = view.workspace.rename;
	name_.follow(held.name);
	draw_back(workspace);
	const auto close = [&workspace] {
		window_requests::set_workspace(workspace, "rename", "open", io::JsonValue::make_bool(false));
	};
	// Held open, it shows when no dialog before it in the session's order is held (shown_modal); another rename
	// planned in its place closes it, the session's (workspace_tidies).
	if (!popup_.begin(kTitle, held.open && view.project.open && modal_may_show(view, HeldModal::Rename), true,
	                  ImGuiWindowFlags_AlwaysAutoResize, false, view.workspace.opened)) {
		if (popup_.dismissed()) close();
		return;
	}
	const float em = ImGui::GetFontSize();
	ImGui::Text("Rename %s '%s' and every use of it, in every file, to", reference_row(held.kind).phrase, held.old_name.c_str());
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(em * 24.0f);
	const bool enter = ImGui::InputText("##name", name_.text, sizeof(name_.text), ImGuiInputTextFlags_EnterReturnsTrue);
	const std::string name = name_.sent();
	// The plan shown is the typed name's: the one it was asked for (its new name is the name as the
	// definition takes it: a style variable's %NAME% typed is its NAME, an item id "0100302" 100302); a
	// name typed is planned again (the session's plan of it is the name typed, the workspace's).
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	const bool current = plan.symbol && !plan.back && plan.path == held.path && plan.locator == held.locator &&
	                     plan.field == held.field && plan.requested == name;
	const std::string asked = held.path + '\n' + held.locator + '\n' + held.field + '\n' + name;
	if (current) {
		asked_ = asked;
	} else if (asked_ != asked && view.allows(EditorRequestKind::PreviewRename)) {
		asked_ = asked;
		workspace.request(preview(held.path, held.locator, held.field, name, false));
	}
	const bool ready = current && plan.refusals.empty() && !name.empty();
	ImGui::BeginChild("sites", ImVec2(em * 40.0f, em * 14.0f), ImGuiChildFlags_Borders);
	if (!current) {
		ui_kit::empty_state("Planning...");
	} else {
		for (const Diagnostic &refusal : plan.refusals) {
			ui_kit::severity_marker(refusal.severity);
			ImGui::SameLine();
			ImGui::TextWrapped("%s", refusal.message.c_str());
		}
		const std::vector<RenameSite> &sites = *plan.sites;
		const size_t uses = sites.empty() ? 0 : sites.size() - 1;
		ImGui::TextDisabled("The definition and %zu use%s:", uses, uses == 1 ? "" : "s");
		for (size_t i = 0; i < sites.size(); ++i) {
			const std::string line = site_line(sites[i]);
			ImGui::PushID(static_cast<int>(i));
			ui_kit::clipped_text(line);
			ImGui::PopID();
		}
		if (!plan.companions.empty()) ImGui::TextDisabled("Renamed with it:");
		for (size_t i = 0; i < plan.companions.size(); ++i) {
			ImGui::PushID(static_cast<int>(sites.size() + i));
			ui_kit::clipped_text(plan.companions[i]);
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	// A rename rewrites the project's files: while an operation holds them (a build packing them),
	// the busy gate refuses it, and Rename waits with it (SessionView::allows).
	const bool allowed = view.allows(EditorRequestKind::RenameSymbol);
	ImGui::BeginDisabled(!ready || !allowed);
	const bool rename = ImGui::Button("Rename");
	ImGui::EndDisabled();
	ui_kit::tooltip(!allowed ? "A rename rewrites the project's files: it waits for the running operation."
	                : ready  ? "Rewrites every file listed on disk. It cannot be undone with Undo."
	                         : "Waits for a new name the rename can take (the reasons are listed).");
	if ((rename || enter) && ready && allowed) {
		// The session closes the dialog as it takes the rename (rename_symbol alone, over the wire); its
		// button closes it too.
		workspace.request(request::rename_symbol(held.path, held.locator, held.field, name));
		close();
		popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		close();
		popup_.close();
	}
	ImGui::EndPopup();
}

// The last rename's way back (RenameController's rename_back): the sites it rewrites (only those the rename
// wrote) and why it is refused; Rename back raises RenameBack while the plan shown is the way back and is
// not refused.
void RenameDialog::draw_back(Workspace &workspace) {
	const SessionView &view = workspace.view();
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	const auto close = [&workspace] {
		window_requests::set_workspace(workspace, "rename_back", "open", io::JsonValue::make_bool(false));
	};
	if (!back_popup_.begin(kBackTitle,
	                       view.workspace.rename_back.open && view.project.open && plan.back &&
	                               modal_may_show(view, HeldModal::RenameBack),
	                       true, ImGuiWindowFlags_AlwaysAutoResize, false, view.workspace.opened)) {
		if (back_popup_.dismissed()) close();
		return;
	}
	const float em = ImGui::GetFontSize();
	ImGui::Text("Rename %s back to %s, where the rename wrote it:", plan.old_name.c_str(), plan.new_name.c_str());
	const bool ready = plan.refusals.empty() && plan.sites && !plan.sites->empty();
	ImGui::BeginChild("sites", ImVec2(em * 40.0f, em * 14.0f), ImGuiChildFlags_Borders);
	for (const Diagnostic &refusal : plan.refusals) {
		ui_kit::severity_marker(refusal.severity);
		ImGui::SameLine();
		ImGui::TextWrapped("%s", refusal.message.c_str());
	}
	if (plan.sites)
		for (size_t i = 0; i < plan.sites->size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			ui_kit::clipped_text(site_line((*plan.sites)[i]));
			ImGui::PopID();
		}
	for (size_t i = 0; i < plan.companions.size(); ++i) {
		ImGui::PushID(static_cast<int>(1000000 + i));
		ui_kit::clipped_text("Renamed with it: " + plan.companions[i]);
		ImGui::PopID();
	}
	ImGui::EndChild();
	const bool allowed = view.allows(EditorRequestKind::RenameBack);
	ImGui::BeginDisabled(!ready || !allowed);
	const bool back = ImGui::Button("Rename back");
	ImGui::EndDisabled();
	ui_kit::tooltip(!allowed ? "A rename rewrites the project's files: it waits for the running operation."
	                : ready  ? "Rewrites the files listed on disk, only where the rename wrote. It cannot be undone with Undo."
	                         : "Nothing can be renamed back as it is (the reasons are listed).");
	if (back && ready && allowed) {
		workspace.request(request::rename_back());
		close();
		back_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		close();
		back_popup_.close();
	}
	ImGui::EndPopup();
}

} // namespace opennova::editor
