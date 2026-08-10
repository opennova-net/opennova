#pragma once

// The witnessed edit-field input operations — the text-mutation half of the
// menu edit widget, pure over an (text, caret) pair so the shell only routes
// keys. [orig: the event router edit_widget_handle_input_event @ 0x661510
// (click -> g_ui_focus_wnd; iscntrl-filtered chars -> the insert; specials ->
// the key handler); insert edit_widget_insert_char @ 0x661ee0; special keys
// edit_widget_handle_key_event @ 0x6623a0. Field homes: caret at widget+764,
// text at widget+732.]

#include <cstdlib>
#include <string>

namespace opennova::menu {

struct EditField {
	std::string text;
	int caret = 0;
};

struct EditLimits {
	// Max text length in chars (widget+800); <0 = unlimited.
	int max_len = -1;
	bool read_only = false;  // widget+776: focus/typing rejected
	// Numeric mode (widget+784): only '0'..'9' inserts, and the RESULTING
	// value must stay within [min_value, max_value] or the WHOLE insert
	// rolls back [orig: the atol range check @ 0x661f8f].
	bool numeric = false;
	long min_value = 0;
	long max_value = 0;
};

// The witnessed special-key codes the handler consumes (VK_* values).
inline constexpr int kEditKeyBackspace = 0x08;
inline constexpr int kEditKeyEnter = 0x0D;
inline constexpr int kEditKeyEnd = 0x23;
inline constexpr int kEditKeyHome = 0x24;
inline constexpr int kEditKeyLeft = 0x25;
inline constexpr int kEditKeyRight = 0x27;
inline constexpr int kEditKeyDelete = 0x2E;

enum class EditKeyResult {
	kNone,     // unhandled / caret-only move
	kChanged,  // the text mutated
	kCommit,   // Enter: focus releases and the value commits
};

// Delete `count` chars at the caret [orig: sub_6622c0 via Backspace/Delete].
inline bool edit_delete_at_caret(EditField &f, int count) {
	if (count <= 0 || f.caret >= static_cast<int>(f.text.size())) {
		return false;
	}
	const int avail = static_cast<int>(f.text.size()) - f.caret;
	f.text.erase(static_cast<size_t>(f.caret),
			static_cast<size_t>(count < avail ? count : avail));
	return true;
}

// Insert `count` copies of `ch` at the caret [orig: edit_widget_insert_char
// @ 0x661ee0]: read-only and CR/LF reject; numeric mode admits only digits
// and rolls the WHOLE insert back when the resulting atol leaves
// [min_value, max_value]; max_len clamps per char. The caret rides the
// inserted run.
inline bool edit_insert_char(EditField &f, const EditLimits &lim, char ch,
		int count = 1) {
	if (lim.read_only || ch == '\r' || ch == '\n' || count <= 0) {
		return false;
	}
	if (lim.numeric && (ch < '0' || ch > '9')) {
		return false;
	}
	const EditField before = f;
	for (int i = 0; i < count; ++i) {
		if (lim.max_len >= 0 &&
				static_cast<int>(f.text.size()) >= lim.max_len) {
			break;
		}
		f.text.insert(static_cast<size_t>(f.caret), 1, ch);
		++f.caret;
	}
	if (f.text.size() == before.text.size()) {
		return false;
	}
	if (lim.numeric) {
		const long value = std::strtol(f.text.c_str(), nullptr, 10);
		if (value > lim.max_value || value < lim.min_value) {
			f = before;  // the witnessed whole-insert rollback
			return false;
		}
	}
	return true;
}

// One special key [orig: edit_widget_handle_key_event @ 0x6623a0]. Backspace
// moves the caret back (clamped to 0) THEN deletes the run; Enter commits
// (the embedder releases focus and fires its commit event); Left never moves
// under Shift in the original (selection reserved); Up/Down only refresh the
// scroll window.
inline EditKeyResult edit_apply_key(EditField &f, int key, int count = 1,
		bool shift_held = false) {
	const int len = static_cast<int>(f.text.size());
	switch (key) {
		case kEditKeyBackspace: {
			int run = count;
			if (f.caret - run < 0) {
				run = f.caret;
			}
			f.caret -= run;
			if (f.caret < 0) {
				f.caret = 0;
			}
			return edit_delete_at_caret(f, run) ? EditKeyResult::kChanged
												: EditKeyResult::kNone;
		}
		case kEditKeyEnter:
			return EditKeyResult::kCommit;
		case kEditKeyEnd:
			f.caret = len;
			return EditKeyResult::kNone;
		case kEditKeyHome:
			f.caret = 0;
			return EditKeyResult::kNone;
		case kEditKeyLeft:
			if (!shift_held) {
				f.caret = f.caret - 1 <= 0 ? 0 : f.caret - 1;
			}
			return EditKeyResult::kNone;
		case kEditKeyRight:
			f.caret = f.caret + 1 >= len ? len : f.caret + 1;
			return EditKeyResult::kNone;
		case kEditKeyDelete:
			return edit_delete_at_caret(f, count) ? EditKeyResult::kChanged
												  : EditKeyResult::kNone;
		default:
			return EditKeyResult::kNone;
	}
}

}  // namespace opennova::menu
