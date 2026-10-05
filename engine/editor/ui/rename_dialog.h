#pragma once

#include <cstdint>
#include <string>

#include <editor/model/value.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// Rename everywhere (ADR 0046 S12): the modal over the workspace's rename (the MCP gaps lane:
// workspace.rename, which a PreviewRename that asks the new name opens: the Inspector's Rename... on a
// field that defines a name, F2 there), drawn by the workspace every frame with its others. What is
// typed is planned again as it changes (PreviewRename, whose plan of that name is the workspace's name
// typed): every site it rewrites (file, record, field, the value before and after) and why it would be
// refused, each refusal naming its file; Rename raises RenameSymbol while the plan shown is the typed
// name's and is not refused (it writes the files on disk, which Undo does not reach), and the session
// closes the dialog as it takes it. And Rename back (workspace.rename_back, a PreviewRenameBack that
// asks opens it): the last rename's way back, its plan shown, Rename back raising RenameBack.
class RenameDialog {
public:
	void draw(Workspace &workspace);
	// The PreviewRename of a name's rename everywhere: the field `field` of the record at
	// `locator` in `path`, renamed to `name`; `ask` opens the dialog.
	static EditorRequest preview(const std::string &path, const std::string &locator, const std::string &field,
	                             const std::string &name, bool ask);

private:
	void draw_back(Workspace &workspace);

	ui_kit::HeldPopup popup_, back_popup_;
	ui_kit::HeldText<kWorkspaceText> name_;
	std::string asked_; // the target and the name the last PreviewRename was raised for
};

} // namespace opennova::editor
