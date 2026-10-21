#include <editor/ui/hud_layout_inspector.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/model/document_base.h>
#include <editor/model/text_document.h>
#include <editor/preview/hud_layout_edit.h>
#include <editor/preview/hud_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/reference_fields.h>
#include <editor/ui/reference_picker.h>
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
	return field.words + ": " + field.range + ".\n" + field.cite + "\nEnter, or leaving it, sets it, one undo step, on its line (" +
	       field.key + ").";
}

// The picker the font fields share (its popups each the document's and the field's own).
ReferencePicker &picker() {
	static ReferencePicker shared;
	return shared;
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
		// A font's tip on its name (its field's tools carry their own).
		if (field.kind == HudFieldKind::Name) ui_kit::tooltip(tip_of(field));
		ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
		ImGui::SetNextItemWidth(width);
		switch (field.kind) {
		case HudFieldKind::Number: {
			// The value as it is typed or stepped, held while the field is active and set once it is left (Enter, Tab,
			// a click elsewhere, a step button let go): one edit a gesture. No EnterReturnsTrue: InputScalar asserts on
			// it, and with it a value typed and clicked away from was never set.
			static std::string held_id;
			static int held_value = 0;
			int value = held_id == field.id ? held_value : std::atoi(field.value.c_str());
			if (ImGui::InputInt("##value", &value, 1, 8) || ImGui::IsItemActive()) {
				held_id = field.id;
				held_value = value;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && held_id == field.id) {
				value = std::clamp(held_value, field.least, field.most);
				held_id.clear();
				if (std::to_string(value) != field.value) set_field(workspace, *model, *picked, field, std::to_string(value));
			} else if (ImGui::IsItemDeactivated()) {
				held_id.clear();
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
			// A font by its file (the graph's Font edge of the layout's key): completion, its state's dot and fixes,
			// Pick, a .fnt dropped from Files, its jumps; a name given set through the viewport's command.
			TextPlace place;
			place.document = text_of(document);
			place.key = field.key;
			place.value = field.value;
			place.kind = ReferenceKind::Font;
			std::string given;
			if (place.document && text_reference_field(workspace, picker(), place, given) && given != field.value &&
			    !given.empty())
				set_field(workspace, *model, *picked, field, given);
			break;
		}
		}
		if (field.kind != HudFieldKind::Name) ui_kit::tooltip(tip_of(field));
		ImGui::PopID();
	}
	ImGui::EndDisabled();
	ImGui::Spacing();
}

} // namespace opennova::editor
