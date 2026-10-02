#pragma once
// Menu helpers the editor tests share: a record of a window's list by its kind token, a
// window's native struct and depth, and a window's first APPEARANCE row made an image (the
// full mnu schema has no image field on a window: the rows are records, ADR 0046 S9h).
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>

namespace menu_test {

// The native window (or part) a nested address names, found down its path in the screen as the
// document locates it (TableDocument::locate over Document::path_in: the root's index, then each list
// and index; null for anything else): what a test reads a window's native members through.
inline const opennova::mnu::Window *window_of(const opennova::editor::Document &document,
                                              const opennova::editor::NodeAddress &address) {
	using namespace opennova::editor;
	const auto *menu = dynamic_cast<const MnuDocument *>(&document);
	const auto *screen = dynamic_cast<const MenuScreen *>(document.row(address.row));
	TableDocument::Located at;
	if (!menu || !screen || !address.child || !menu->locate(*screen, address.child, at) ||
	    !is_window_kind(at.record.kind))
		return nullptr;
	return &at.record.as<opennova::mnu::Window>();
}

// The windows and parts above a window inside its screen (0 = a root window).
inline int depth_of(const opennova::editor::Document &document, const opennova::editor::NodeAddress &address) {
	const size_t owners = document.ancestors(address).size(); // the row first
	return owners ? static_cast<int>(owners) - 1 : 0;
}

// The record at `index` of the owner's collection of `token` ("appearance", "action",
// "window"), or an empty address.
inline opennova::editor::NodeAddress child_of(const opennova::editor::Document &document,
                                              const opennova::editor::NodeAddress &owner, const char *token,
                                              size_t index = 0) {
	for (const opennova::editor::Document::Collection &collection : document.collections_of(owner))
		if (std::string(document.kind_token(collection.spec.kind)) == token && index < collection.ids.size())
			return {owner.row, collection.spec.kind, collection.ids[index]};
	return {};
}

inline opennova::editor::Edit set_edit(const opennova::editor::NodeAddress &address, const char *field,
                                       opennova::editor::Value value) {
	opennova::editor::Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// One batch: the window's APPEARANCE row `row` made an IMAGE row showing `texture`.
inline std::vector<opennova::editor::Edit> image_edits(const opennova::editor::Document &document,
                                                       const opennova::editor::NodeAddress &window,
                                                       const std::string &texture, size_t row = 0) {
	const opennova::editor::NodeAddress appearance = child_of(document, window, "appearance", row);
	return {set_edit(appearance, "type", std::string("IMAGE")), set_edit(appearance, "value", texture)};
}

} // namespace menu_test
