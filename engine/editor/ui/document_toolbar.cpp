#include "document_toolbar.h"

#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

void draw_document_toolbar(EditorHost &host, const Document &document) {
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Reload", true, "Read the file again (asks first when it has unsaved changes)."))
		host.request(make_request(EditorRequestKind::ReloadDocument, document.path()));
	if (ui_kit::tool(row, "Undo", document.can_undo(),
	                 document.can_undo() ? "Undo the last change to this file (Ctrl+Z)." : "Nothing to undo in this file."))
		host.request(make_request(EditorRequestKind::Undo, document.path()));
	if (ui_kit::tool(row, "Redo", document.can_redo(),
	                 document.can_redo() ? "Redo what Undo took back (Ctrl+Y)." : "Nothing to redo in this file."))
		host.request(make_request(EditorRequestKind::Redo, document.path()));
	if (document.blocked())
		ImGui::TextWrapped("This file has unsupported or malformed input. See Problems, correct the source, then Reload.");
	else if (const size_t ignored = document.ignored_lines())
		ImGui::TextWrapped("%zu line(s) the game ignores will be dropped when this file is saved. See Problems.", ignored);
}

} // namespace opennova::editor
