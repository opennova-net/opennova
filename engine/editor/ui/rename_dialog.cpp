#include "rename_dialog.h"

#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstring>

#include <imgui.h>

namespace opennova::editor {
namespace {

constexpr const char *kTitle = "Rename everywhere";

// A site of a rename, on one line.
std::string site_line(const RenameSite &site) {
	return site.file + ": " + (site.record.empty() ? std::string() : site.record + " - ") + site.field + ": " + site.before +
	       " -> " + site.after;
}

} // namespace

EditorRequest RenameDialog::preview(const std::string &path, const std::string &locator, const std::string &field,
                                    const std::string &name, bool ask) {
	EditorRequest request = make_request(EditorRequestKind::PreviewRename, path, locator);
	request.edit.field = field;
	request.edit.value = name;
	request.flag = ask;
	return request;
}

void RenameDialog::draw(Workspace &workspace) {
	const SessionView &view = workspace.view();
	// An ask opens the dialog on the preview it names: a name's rename everywhere (a file's has
	// its own place in Files), while the view still holds that preview.
	for (const ViewEvent &ask : events_.take()) {
		const DialogsView::RenamePreview &preview = view.dialogs.rename_preview;
		if (ask.kind != ViewEventKind::AskRename || ask.tag != preview.serial || !preview.symbol)
			continue;
		open_ = true;
		path_ = preview.path;
		locator_ = preview.locator;
		field_ = preview.field;
		old_name_ = preview.old_name;
		kind_ = preview.kind;
		const size_t n = std::min(preview.requested.size(), sizeof(name_) - 1);
		std::memcpy(name_, preview.requested.data(), n);
		name_[n] = '\0';
		asked_ = name_;
	}
	if (open_) {
		open_ = false;
		ImGui::OpenPopup(kTitle);
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!view.project.open) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	const float em = ImGui::GetFontSize();
	ImGui::Text("Rename %s '%s' and every use of it, in every file, to", reference_row(kind_).phrase, old_name_.c_str());
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(em * 24.0f);
	const bool enter = ImGui::InputText("##name", name_, sizeof(name_), ImGuiInputTextFlags_EnterReturnsTrue);
	if (asked_ != name_ && view.allows(EditorRequestKind::PreviewRename)) {
		asked_ = name_;
		workspace.request(preview(path_, locator_, field_, asked_, false));
	}
	// The plan shown is the typed name's: the one it was asked for (its new name is the name as the
	// definition takes it: a style variable's %NAME% typed is its NAME, an item id "0100302" 100302).
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	const bool current = plan.symbol && plan.path == path_ && plan.locator == locator_ && plan.field == field_ &&
	                     plan.requested == asked_;
	const bool ready = current && plan.refusals.empty() && !asked_.empty();
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
		EditorRequest request = preview(path_, locator_, field_, asked_, false);
		request.kind = EditorRequestKind::RenameSymbol;
		workspace.request(std::move(request));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace opennova::editor
