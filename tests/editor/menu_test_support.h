#pragma once
// Menu helpers the editor tests share: a record of a window's list by its kind token, a
// window's native struct and depth, a record by the name it defines, a menu's text built from its
// parts, two positions and a rect compared, and a window's first APPEARANCE row made an image (the
// full mnu schema has no image field on a window: the rows are records, ADR 0046 S9h).
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>

#include "editor/editor_test_support.h"

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

// A record by the name it defines (a window's, a style's): find_definition over no graph.
inline opennova::editor::NodeAddress named(const opennova::editor::Document &document, const char *name) {
	opennova::editor::NodeAddress address;
	opennova::editor::find_definition(opennova::editor::AssetGraph(), document, name, address);
	return address;
}

// A menu's text built from its parts: a window of a type and name (its rect 0, 0, 100, 20) holding
// `body`, a screen of a name holding `body`, an IMAGE row of a texture.
inline std::string window(const char *type, const char *name, const std::string &body = std::string()) {
	return std::string("<WINDOW TYPE=\"") + type + "\" NAME=\"" + name + "\">\r\n" +
	       "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n" + body + "</WINDOW>\r\n";
}
inline std::string screen(const char *name, const std::string &body) {
	return std::string("<SCREEN>\r\n<NAME>") + name + "</NAME>\r\n" + body + "</SCREEN>\r\n";
}
inline std::string image(const std::string &texture) {
	return "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">" + texture + "</APPEARANCE>\r\n";
}

// Whether two positions write the same edges, and a rect's edges.
inline bool same(const opennova::mnu::Position &a, const opennova::mnu::Position &b) {
	return a.has_left == b.has_left && a.has_top == b.has_top && a.has_right == b.has_right &&
	       a.has_bottom == b.has_bottom && (!a.has_left || a.left == b.left) && (!a.has_top || a.top == b.top) &&
	       (!a.has_right || a.right == b.right) && (!a.has_bottom || a.bottom == b.bottom);
}
inline bool rect_is(const opennova::mnu::RectEdges &r, int left, int top, int right, int bottom) {
	return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}

// One batch: the window's APPEARANCE row `row` made an IMAGE row showing `texture`.
inline std::vector<opennova::editor::Edit> image_edits(const opennova::editor::Document &document,
                                                       const opennova::editor::NodeAddress &window,
                                                       const std::string &texture, size_t row = 0) {
	const opennova::editor::NodeAddress appearance = child_of(document, window, "appearance", row);
	return {editor_test::set_edit(appearance, "type", std::string("IMAGE")),
	        editor_test::set_edit(appearance, "value", texture)};
}

} // namespace menu_test
