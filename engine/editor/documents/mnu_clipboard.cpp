#include <editor/documents/mnu_clipboard.h>

#include <editor/documents/mnu_document.h>

namespace opennova::editor {

namespace {

constexpr NodeKind kWindow = node_kind(MenuKind::Window);

} // namespace

NodeAddress listed_window(const MnuDocument &document, NodeId screen, const NodeAddress &record) {
	if (!screen || record.row != screen || !record.child) return NodeAddress();
	const std::vector<NodeAddress> owners = document.ancestors(record);
	if (owners.empty()) return NodeAddress();
	NodeAddress found;
	for (const NodeAddress &owner : owners) {
		if (!owner.child) continue;              // the screen
		if (owner.kind != kWindow) return found; // a part: nothing it holds is listed
		found = owner;
	}
	return record.kind == kWindow ? record : found;
}

MenuClipboard menu_clipboard(const MnuDocument &document, NodeId screen, const NodeAddress &primary,
                             const std::vector<NodeAddress> &selected, bool clipboard_full) {
	MenuClipboard out;
	out.copy = !selected.empty();
	for (const NodeAddress &record : selected)
		out.copy = out.copy && listed_window(document, screen, record) == record;
	out.paste = clipboard_full;
	out.paste_row = {screen, kWindow, 0};
	Document::Placement at;
	const NodeAddress after = listed_window(document, screen, primary);
	if (after.child && document.placement(after, at)) {
		out.paste_parent = at.owner.child;
		out.paste_position = at.index + 1;
	}
	return out;
}

} // namespace opennova::editor
