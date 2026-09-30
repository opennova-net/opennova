#include "table_cells.h"

#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

namespace opennova::editor {

void text_cell(Workspace &workspace, const Document &document, NodeAddress address, const char *field) {
	Value value;
	if (!document.get(address, field, value)) return;
	const auto *current = std::get_if<std::string>(&value);
	if (!current) return;
	const FieldSchema *schema = nullptr;
	for (const FieldSchema &candidate : document.fields(address.kind))
		if (candidate.id == field) schema = &candidate;
	if (!schema) return;
	std::vector<char> text(std::max<size_t>(schema->width, 2), 0);
	std::memcpy(text.data(), current->data(), std::min(current->size(), text.size() - 1));
	// The whole cell: its column's header names the field, so the label is the id alone.
	const std::string id = std::string("##") + field;
	ImGui::SetNextItemWidth(-FLT_MIN);
	bool changed = false;
	if (schema->multiline) {
		// As tall as its lines, eight at most (it scrolls past them); Enter starts a new line.
		const int lines = 1 + static_cast<int>(std::count(current->begin(), current->end(), '\n'));
		const float height = ImGui::GetTextLineHeight() * float(std::clamp(lines, 1, 8)) + ImGui::GetStyle().FramePadding.y * 2.0f;
		changed = ImGui::InputTextMultiline(id.c_str(), text.data(), text.size(), ImVec2(-FLT_MIN, height));
	} else {
		changed = ImGui::InputText(id.c_str(), text.data(), text.size());
	}
	if (changed) window_requests::set(workspace, document, address, field, std::string(text.data()));
	if (ImGui::IsItemDeactivatedAfterEdit()) workspace.request(make_request(EditorRequestKind::EndEdit, document.path()));
}

} // namespace opennova::editor
