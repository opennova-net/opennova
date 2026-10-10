#pragma once

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/preview/script_viewport.h>

namespace godot {

// The script device's control (ADR 0046 S13 V10; authoring/script_device; CONTEXT.md "Script
// device"): a Godot CodeEdit, decision 11's allowed device-side exception for script text, which
// owns the pointer and the keys in its rect. Its undo is the editor's: its own history is kept
// empty (so its menu offers none) and Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z are left to the editor's
// shortcuts, never taken here. One caret, and its selections not dragged and dropped: a change at
// several places is a step of lines (an indent), never a caret's at each of them. No auto indent
// after a line's end (the text's own). Its paste is its own (the clipboard's CR LFs and CRs alone
// each an LF), which cannot reach the state that lets Godot's paste a line copied with nothing
// selected above the current one, so a copy or a cut with nothing selected takes nothing. The
// findings show as an icon a line in a gutter of their own (the worst one's severity, the shapes
// and colours of the windows' marks), their messages its tooltip over the gutter and the worst one's
// first sentence written after the line's text (S15); each marked line keeps its mark's index as its
// gutter's metadata, so the icon, the tip, the note and what the GUT tests read stay with the line as
// text is inserted or removed above it. Its device answers what a script may use as it is typed
// (the completion list), what a word is (the tooltip over the text) and where it is defined (a
// Ctrl+click on the word, or F12 at the caret: DI-18), from session/script_assist (S15). It tells its
// device what the user did
// (its text changed, the focus left it), each outside any pump (a deferred call), lets go of its
// focus on any press of the window outside its rect (the window's own input, a press and its
// release in one frame included), and does nothing else with it: the device turns the text into
// requests.
class ScriptEdit : public CodeEdit {
	GDCLASS(ScriptEdit, CodeEdit)

public:
	ScriptEdit();

	// What the device is told: the text the user changed (Godot's deferred text_changed), the focus
	// gone. Null: nothing (a device given up).
	void set_listener(std::function<void()> on_text_changed, std::function<void()> on_focus_exited);
	// Nothing told any more, and what was deferred dropped: the device goes (the control is freed at
	// the frame's end, a deferred call of it may run before).
	void clear_listener();
	// `call` run at the next deferred call, outside any pump of the session (what the device raises
	// from a pump or a tick: a burst's end).
	void defer(std::function<void()> call);
	// The gutter marks (lines from 1, as the viewport has them); every line's cleared first.
	void set_marks(const std::vector<opennova::editor::ScriptMark> &marks);

	// The document it shows (its project-relative path) and its gutter marks, for the GUT tests: how
	// many marks it holds, and a line's (from 0) severity ("error", "warning", "info"; "" none) and
	// tip, read from the mark its gutter's metadata names, so they are those of the line's text as
	// the control holds it now.
	void set_document_path(const String &p_path) { path_ = p_path; }
	String get_document_path() const { return path_; }
	int get_mark_count() const { return int(marks_.size()); }
	String get_mark_severity(int p_line) const;
	String get_mark_tip(int p_line) const;

	// A paste: the clipboard's text with each CR LF and each CR alone an LF (a Godot text control drops
	// a CR as it takes text, which would join the lines a CR alone ends), in place of the selection.
	void _paste(int32_t p_caret_index) override;

	// What its device answers of the script (ADR 0046 S15; the data is session/script_assist's): the
	// completions at the caret (the device adds the options and updates the list), a word's words at a
	// place (its tooltip over the text), and the place a Ctrl+click or F12 looks up (the device goes there).
	// Lines and columns from 0, the control's. Null members: nothing.
	struct Assist {
		std::function<void(bool force)> complete;
		std::function<std::string(int line, int column)> hover;
		std::function<void(int line, int column)> lookup;
	};
	void set_assist(Assist assist);
	// The list asked for (Ctrl+Space, or a word's character typed): the device's completions.
	void _request_code_completion(bool p_force) override;
	// The note each marked line shows after its text (the worst finding's first sentence), and what the
	// word at a place is (the tooltip over the text), for the GUT tests: a line's and a column's (from
	// 0), "" for none.
	String get_mark_note(int p_line) const;
	String get_word_tip(int p_line, int p_column) const;
	// A word's words shown at a place (the viewport's assist hover, the MCP gaps lane: what the pointer over
	// the word shows, asked over the wire), in a box under the word drawn over the text, until the next ask
	// ("" none). Line and column from 0. The GUT tests read what it shows.
	void set_hover_note(const String &p_text, int p_line = 0, int p_column = 0);
	String get_hover_note() const { return hover_note_; }
	// Whether the completion list shows (CodeEdit's), for the GUT tests.
	bool is_completion_shown() const { return get_code_completion_options().size() > 0; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void on_text_changed_();
	void on_focus_exited_();
	void run_deferred_();
	void on_gui_input_(const Ref<InputEvent> &p_event);
	void on_window_input_(const Ref<InputEvent> &p_event);
	void on_symbol_validate_(const String &p_symbol);
	void on_symbol_lookup_(const String &p_symbol, int64_t p_line, int64_t p_column);
	// F12: the word at the caret (or the one it ends) looked up, as a Ctrl+click on it is.
	void look_up_at_caret_();
	// Each marked line's note after its text, in its severity's colour, where the line shows.
	void draw_notes_();
	// The hover note's box under its word, where the word shows.
	void draw_hover_note_();
	String hover_note_;
	int hover_line_ = 0, hover_column_ = 0;
	const opennova::editor::ScriptMark *mark_at_(int p_line) const;
	Assist assist_;

	std::function<void()> text_changed_;
	std::function<void()> focus_exited_;
	std::vector<std::function<void()>> deferred_;
	bool deferred_pending_ = false;
	String path_;
	int gutter_ = 0; // the findings' gutter
	std::array<Ref<ImageTexture>, 3> icons_; // by severity: info, warning, error
	std::vector<opennova::editor::ScriptMark> marks_;
	uint64_t window_id_ = 0; // the window whose input it listens to while it is in the tree
};

} // namespace godot
