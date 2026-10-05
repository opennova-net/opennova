#include <editor/ui/texture_source_dialog.h>

#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <editor/preview/texture_thumbnails.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/texture_preview.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

constexpr const char *kTitle = "Make a texture from an image###texture_source";

// A stored form's words in the choice.
const char *form_words(const std::string &form) {
	if (form == "tga") return "32-bit TGA (colour and alpha)";
	if (form == "tga24") return "24-bit TGA (colour alone)";
	if (form == "pcx") return "8-bit PCX (256 colours)";
	if (form == "pcx24") return "24-bit PCX (three planes of colour)";
	if (form == "dxt5") return "DXT5 DDS (graded alpha)";
	if (form == "dxt1") return "DXT1 DDS (alpha on or off)";
	if (form == "argb") return "A8R8G8B8 DDS (uncompressed)";
	return "this form";
}

// The options a form is asked by over the plan's own.
std::vector<std::pair<std::string, std::string>> form_values(const std::vector<std::pair<std::string, std::string>> &values,
                                                             const std::string &form) {
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto &each : values)
		if (each.first != "format" && each.first != "dds") out.push_back(each);
	if (form == "dxt5" || form == "dxt1" || form == "argb") {
		out.emplace_back("format", "dds");
		out.emplace_back("dds", form);
	} else {
		out.emplace_back("format", form);
	}
	return out;
}

} // namespace

void TextureSourceDialog::draw(Workspace &workspace) {
	const SessionView &view = workspace.view();
	const DialogsView::TextureSourcePreview &preview = view.dialogs.texture_source;
	// It shows while no dialog before it in the session's order is held (shown_modal); one that takes its place
	// leaves it waiting, opened again (its serial asked anew) once that one closes.
	const bool shows = preview.open && view.project.open && modal_may_show(view, HeldModal::TextureSource);
	if (!shows && !ImGui::IsPopupOpen(kTitle)) shown_ = 0;
	if (shows && preview.serial != shown_ && !ImGui::IsPopupOpen(kTitle)) {
		shown_ = preview.serial;
		ImGui::OpenPopup(kTitle);
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!shows) {
		shown_ = 0;
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	shown_ = preview.serial;
	const float em = ImGui::GetFontSize();
	const bool replace = !preview.image.empty();
	const std::string name = basename_of(preview.texture);
	ImGui::PushTextWrapPos(em * 34.0f);
	if (replace)
		ImGui::Text("Replace %s with %s?", name.c_str(), basename_of(preview.image).c_str());
	else
		ImGui::Text("Give %s a source of its own, which its program edits?", name.c_str());
	if (!preview.refusal.empty()) {
		ImGui::Spacing();
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Error), "%s", preview.refusal.c_str());
	} else {
		ImGui::Spacing();
		const float side = em * 9.0f;
		if (ImGui::BeginTable("before_after", 2, ImGuiTableFlags_SizingFixedSame)) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("Now");
			texture_preview::picture(workspace, preview.before.get(), side);
			ImGui::TextUnformatted(preview.before_words.empty() ? "(none)" : preview.before_words.c_str());
			ImGui::TableNextColumn();
			ImGui::TextDisabled("Then");
			texture_preview::picture(workspace, preview.after.get(), side);
			ImGui::TextUnformatted(preview.after_words.c_str());
			ImGui::EndTable();
		}
		// The stored form: the plan's, the name's others offered (a Replace only: an Edit keeps its form).
		if (replace && preview.forms.size() > 1) {
			ImGui::SetNextItemWidth(em * 18.0f);
			if (ImGui::BeginCombo("Store as", form_words(preview.form))) {
				for (const std::string &form : preview.forms)
					if (ImGui::Selectable(form_words(form), form == preview.form) && form != preview.form)
						workspace.request(request::preview_texture_source(preview.texture, preview.image, form_values(preview.values, form)));
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The form the texture is stored in now is kept unless you choose another.");
		}
		if (!preview.changes.empty()) {
			ImGui::Spacing();
			ImGui::TextDisabled("What changes");
			for (const std::string &change : preview.changes) ImGui::BulletText("%s", change.c_str());
		}
	}
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	const bool ready = preview.refusal.empty() &&
	                   view.allows(replace ? EditorRequestKind::ReplaceTexture : EditorRequestKind::EditExternally);
	ImGui::BeginDisabled(!ready);
	if (ImGui::Button(replace ? "Replace" : "Make its source and open it")) {
		workspace.request(replace ? request::replace_texture(preview.texture, preview.image, preview.values)
		                          : request::edit_externally(preview.texture));
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) workspace.request(request::cancel_texture_source());
	ImGui::EndPopup();
}

} // namespace opennova::editor
