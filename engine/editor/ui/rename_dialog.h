#pragma once

#include <cstdint>
#include <string>

#include <editor/model/value.h>
#include <editor/ui/editor_host.h>

namespace opennova::editor {

// Rename everywhere (ADR 0046 S12): the modal a PreviewRename that asks the new name opens (the
// Inspector's Rename... on a field that defines a name, F2 there), which the workspace draws
// every frame with its others. What is typed is planned again as it changes (PreviewRename):
// every site it rewrites (file, record, field, the value before and after) and why it would be
// refused, each refusal naming its file; Rename raises RenameSymbol while the plan shown is the
// typed name's and is not refused (it writes the files on disk, which Undo does not reach).
class RenameDialog {
public:
	// Opens when the view's rename_preview asks the new name (its ask_serial moved), the name
	// typed starting as the preview's.
	void follow(const SessionView &view);
	void draw(EditorHost &host);
	// The PreviewRename of a name's rename everywhere: the field `field` of the record at
	// `locator` in `path`, renamed to `name`; `ask` opens the dialog.
	static EditorRequest preview(const std::string &path, const std::string &locator, const std::string &field,
	                             const std::string &name, bool ask);

private:
	uint64_t ask_serial_ = 0;
	bool open_ = false;
	std::string path_, locator_, field_, old_name_;
	ReferenceKind kind_ = ReferenceKind::None;
	char name_[128]{};
	std::string asked_; // the name the last PreviewRename was raised for
};

} // namespace opennova::editor
