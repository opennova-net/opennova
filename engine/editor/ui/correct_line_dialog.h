#pragma once

#include <editor/session/view/workspace_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// Problems' Correct the line... (the workspace's correct_line, which a correct_line with ask_name opens: a
// blocking source line's fix, session/problem_fixes.h): the line as the file holds it, editable, drawn by the
// workspace every frame with its other modals. Apply raises correct_line with the text typed ("" takes the line
// out), which writes the file on disk (Undo does not reach it) and reads it again; the session closes the dialog
// as it takes it.
class CorrectLineDialog {
public:
	void draw(Workspace &workspace);

private:
	ui_kit::HeldPopup popup_;
	ui_kit::HeldText<kWorkspaceLine> text_;
};

} // namespace opennova::editor
