#include "document_toolbar.h"

#include <editor/session/request_factories.h>
#include <editor/session/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

// Each tool enabled while the busy gate takes its request too (SessionView::allows).
void draw_document_toolbar(Workspace &workspace, const Document &document) {
	const SessionView &view = workspace.view();
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Reload", view.allows(EditorRequestKind::ReloadDocument),
	                 "Read the file again (asks first when it has unsaved changes)."))
		workspace.request(request::reload_document(document.path()));
	if (ui_kit::tool(row, "Undo", document.can_undo() && view.allows(EditorRequestKind::Undo),
	                 document.can_undo() ? "Undo the last change to this file (Ctrl+Z)." : "Nothing to undo in this file."))
		workspace.request(request::undo(document.path()));
	if (ui_kit::tool(row, "Redo", document.can_redo() && view.allows(EditorRequestKind::Redo),
	                 document.can_redo() ? "Redo what Undo took back (Ctrl+Y)." : "Nothing to redo in this file."))
		workspace.request(request::redo(document.path()));
	if (document.blocked())
		ImGui::TextWrapped("This file has unsupported or malformed input. See Problems, correct the source, then Reload.");
	else if (const size_t ignored = document.ignored_lines())
		ImGui::TextWrapped("%zu line(s) the game ignores will be dropped when this file is saved. See Problems.", ignored);
}

} // namespace opennova::editor
