#include <editor/ui/record_drag.h>

#include <imgui.h>

#include <editor/assets/asset_kinds.h>
#include <editor/model/document.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

void record_drag_source(const Document &document, const NodeAddress &record, const std::string &words, bool anywhere) {
	if (document.kind() != AssetKind::Mission || !record.row) return;
	if (!ImGui::BeginDragDropSource(anywhere ? ImGuiDragDropFlags_SourceAllowNullID : ImGuiDragDropFlags_None)) return;
	RecordDrag drag;
	drag.document = document.identity();
	drag.record = record;
	ImGui::SetDragDropPayload(kRecordDragPayload, &drag, sizeof(drag));
	ImGui::TextUnformatted(ui_kit::fit(words, ImGui::GetFontSize() * 24.0f).c_str());
	ImGui::TextDisabled("Drop it on a value that names it.");
	ImGui::EndDragDropSource();
}

} // namespace opennova::editor
