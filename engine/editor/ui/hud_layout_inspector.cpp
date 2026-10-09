#include <editor/ui/hud_layout_inspector.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/model/document_base.h>
#include <editor/preview/hud_layout_edit.h>
#include <editor/preview/hud_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <formats/def/def_hudpos_write.h>

namespace opennova::editor {

namespace {

// A set of one of the picked element's fields: the viewport's command, one undo step.
void set_field(Workspace &workspace, const HudViewport &model, const HudPreviewElement &element, const HudField &field,
		const std::string &value) {
	ViewportCommand command;
	command.name = "set";
	command.kind = ViewportKind::Hud;
	command.item = opennova::hud::hud_element_token(element.element);
	command.field = field.id;
	command.value = value;
	workspace.request(request::edit_in_viewport(model.path(), command));
}

std::string tip_of(const HudField &field) {
	return field.words + ": " + field.range + ".\n" + field.cite + "\nEnter sets it, one undo step, on its line (" +
	       field.key + ").";
}

// The words before a field, under its key's name: the value's name (a font's line holds its file alone).
std::string label_of(const HudField &field) {
	const size_t dot = field.id.rfind('.');
	return dot == std::string::npos ? std::string("file") : field.id.substr(dot + 1);
}

} // namespace

void draw_hud_layout_inspector(Workspace &workspace, const DocumentBase &document) {
	const SessionView &view = workspace.view();
	const ViewportModel *found =
			view.documents.viewports ? view.documents.viewports->find(document.path(), ViewportKind::Hud) : nullptr;
	const auto *model = static_cast<const HudViewport *>(found);
	if (!model || model->view_status() != HudViewStatus::Ready) return;
	const HudPreviewElement *picked = model->picked();
	if (!picked) {
		ui_kit::empty_state("Pick an element in the HUD preview.",
		                    "Its place, size, anchor, font, colour and detail level show here; drag it in the preview to "
		                    "move it, a corner of it to size it.");
		return;
	}
	ImGui::SeparatorText(hud_element_words(picked->element).words);
	const def::DefHudPosDef *layout = model->layout_model();
	const std::vector<HudField> fields = layout ? hud_element_fields(picked->element, *layout) : std::vector<HudField>();
	if (fields.empty()) {
		ImGui::TextDisabled("No line of hudpos.def places it: the game does.");
		ImGui::Spacing();
		return;
	}
	const bool editable = view.allows(EditorRequestKind::EditInViewport);
	ImGui::BeginDisabled(!editable);
	std::string group;
	const float width = ImGui::GetFontSize() * 8.0f;
	for (const HudField &field : fields) {
		// Each key's fields under its line's name.
		std::string heading = field.key + (field.first.empty() ? std::string() : " " + field.first);
		if (heading != group) {
			group = heading;
			ImGui::TextDisabled("%s", heading.c_str());
		}
		ImGui::PushID(field.id.c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label_of(field).c_str());
		ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
		ImGui::SetNextItemWidth(width);
		switch (field.kind) {
		case HudFieldKind::Number: {
			int value = std::atoi(field.value.c_str());
			if (ImGui::InputInt("##value", &value, 1, 8, ImGuiInputTextFlags_EnterReturnsTrue)) {
				value = std::clamp(value, field.least, field.most);
				if (std::to_string(value) != field.value) set_field(workspace, *model, *picked, field, std::to_string(value));
			}
			break;
		}
		case HudFieldKind::Align: {
			if (ImGui::BeginCombo("##value", field.value.c_str())) {
				for (int align = 0; align < 3; ++align) {
					// The alignment's words as hudpos.def's writer writes them.
					const char *word = def::hudpos_align_word(align);
					if (ImGui::Selectable(word, field.value == word) && field.value != word)
						set_field(workspace, *model, *picked, field, word);
				}
				ImGui::EndCombo();
			}
			break;
		}
		case HudFieldKind::Name: {
			char text[128] = {};
			std::strncpy(text, field.value.c_str(), sizeof(text) - 1);
			if (ImGui::InputText("##value", text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue) && field.value != text &&
			    text[0])
				set_field(workspace, *model, *picked, field, text);
			break;
		}
		}
		ui_kit::tooltip(tip_of(field));
		ImGui::PopID();
	}
	ImGui::EndDisabled();
	ImGui::Spacing();
}

} // namespace opennova::editor
