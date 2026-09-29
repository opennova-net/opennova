#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

class MnuDocument;

// What the clipboard does with a menu's selection (ADR 0046 S9k2; S13 V1: one rule, which the
// menu view's tree and the Preview's canvas both ask). Copy, Cut and Duplicate take the
// selection only while every selected record is a window the tree lists: a window of the
// screen that the screen or windows alone hold, never one a part holds (a list's scrollbar,
// a combo's list box: a part goes with its owner). A Paste goes after the window the tree
// lists that is the primary record or holds it, among its siblings, else at the end of the
// screen's root windows (the screen itself selected, or nothing of it).
struct MenuClipboard {
	bool copy = false;                // Copy, Cut and Duplicate take the selection
	bool paste = false;               // Paste has records to paste
	NodeAddress paste_row;            // the list a Paste goes into: the screen's windows
	NodeId paste_parent = 0;          // the window holding them (0: the screen's root windows)
	size_t paste_position = SIZE_MAX; // the place among them (SIZE_MAX: the end)
};

// The clipboard's answer on the screen row `screen` of `document`, `primary` and `selected`
// being the selection there (none when the menu is not the active document: nothing to copy,
// a Paste at the end) and `clipboard_full` whether the session's clipboard holds records.
MenuClipboard menu_clipboard(const MnuDocument &document, NodeId screen, const NodeAddress &primary,
                             const std::vector<NodeAddress> &selected, bool clipboard_full);

} // namespace opennova::editor
