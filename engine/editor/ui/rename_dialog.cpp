#include "rename_dialog.h"

#include <editor/graph/reference_kinds.h>
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

void RenameDialog::follow(const SessionView &view) {
	const SessionView::RenamePreview &preview = view.rename_preview;
	if (preview.ask_serial == ask_serial_) return;
	ask_serial_ = preview.ask_serial;
	if (!preview.symbol) return;
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

void RenameDialog::draw(EditorHost &host) {
	const SessionView &view = host.view();
	if (open_) {
		open_ = false;
		ImGui::OpenPopup(kTitle);
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!view.project_open) {
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
		host.request(preview(path_, locator_, field_, asked_, false));
	}
	// The plan shown is the typed name's: the one it was asked for (its new name is the name as the
	// definition takes it: a style variable's %NAME% typed is its NAME, an item id "0100302" 100302).
	const SessionView::RenamePreview &plan = view.rename_preview;
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
		const size_t uses = plan.sites.empty() ? 0 : plan.sites.size() - 1;
		ImGui::TextDisabled("The definition and %zu use%s:", uses, uses == 1 ? "" : "s");
		for (size_t i = 0; i < plan.sites.size(); ++i) {
			const std::string line = site_line(plan.sites[i]);
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
		host.request(std::move(request));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace opennova::editor
