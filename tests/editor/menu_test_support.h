#pragma once
// Menu helpers the editor tests share: a record of a window's list by its kind token, a
// window's native struct and depth, and a window's first APPEARANCE row made an image (the
// full mnu schema has no image field on a window: the rows are records, ADR 0046 S9h).
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <formats/mnu/mnu_schema.h>

namespace menu_test {

// The native window (or part) a nested address names, followed down its path in the screen as
// the document locates it (Document::path_in: the root's index, then each list and index; null
// for anything else): what a test reads a window's native members through.
inline const opennova::mnu::Window *window_of(const opennova::editor::Document &document,
                                              const opennova::editor::NodeAddress &address) {
	namespace mnu = opennova::mnu;
	const auto *screen = dynamic_cast<const opennova::editor::MenuScreen *>(document.row(address.row));
	if (!screen || !address.child) return nullptr;
	const opennova::editor::Document::RecordPath steps = document.path_in(*screen, address.child);
	if (steps.empty()) return nullptr;
	std::vector<mnu::Window> &roots = const_cast<opennova::editor::MenuScreen *>(screen)->screen.roots;
	if (steps[0].index >= roots.size()) return nullptr;
	mnu::SchemaRecord record = mnu::schema_window(roots[steps[0].index]);
	for (size_t i = 1; i < steps.size() && record; ++i)
		record = mnu::schema_list_at(record, steps[i].collection, steps[i].index);
	if (!record || (record.shape != mnu::SchemaShape::Window && record.shape != mnu::SchemaShape::Part)) return nullptr;
	return static_cast<const mnu::Window *>(record.data);
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
