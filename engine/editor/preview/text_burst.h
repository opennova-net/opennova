#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::editor {

class CanvasRequests;

// A keystroke burst in a text document's script device (ADR 0046 S13 V10; CONTEXT.md "Script
// device"): the span edits one run of typing makes, which carry one gesture token
// (next_edit_gesture), so the text's history (S13 D9's TextHistory) folds them into one undo step,
// until the burst ends: a pause of kQuietSeconds with no edit, the focus leaving the control, an
// edit that does not go on where the last one left the text (the caret moved away first: a click,
// an arrow key), another document, or a change of the document the device did not make. Its end is
// one EndEdit for its document (the Problems wait for it, as for a drag's), raised only when an edit
// of it went out and nothing ended its step already: an undo, a redo or a reload ends the step itself
// (the session ends the document's gesture with it), so the device drops the burst then (drop); the
// focus leaving, the quiet second, an edit elsewhere, another client's edit and the device given up
// end it with its EndEdit.
class TextBurst {
public:
	// The pause that ends a burst.
	static constexpr double kQuietSeconds = 1.0;

	// An edit of it went out and it has not ended.
	bool open() const { return sent_; }
	const std::string &path() const { return path_; }
	// Whether an edit of `path`'s shown text replacing the characters [from, from + removed) goes on
	// the open burst: one of the same document whose change touches the place the last one left the
	// text at (typing on, a Backspace, a Delete).
	bool continues(const std::string &path, size_t from, size_t removed) const;
	// The token its edits carry, made at its first (a new burst's anew).
	uint64_t token();
	// An edit of `path` went out at `now` (seconds), leaving the text at shown offset `at`.
	void sent(const std::string &path, size_t at, double now);
	// Whether it is open and has had no edit for kQuietSeconds at `now`.
	bool quiet(double now) const;
	// It ends: its EndEdit raised for its document when an edit went out; the next edit begins
	// another, with a token of its own.
	void end(CanvasRequests &out);
	// It ends with no EndEdit: what ended its step already ended it (an undo, a redo, a reload).
	void drop();

private:
	std::string path_;
	uint64_t token_ = 0;
	bool sent_ = false;
	size_t at_ = 0;
	double last_ = 0.0;
};

} // namespace opennova::editor
