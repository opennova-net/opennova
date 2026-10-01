#pragma once

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <editor/preview/script_viewport.h>

namespace godot {

// The script device's control (ADR 0046 S13 V10; authoring/script_device; CONTEXT.md "Script
// device"): a Godot CodeEdit, decision 11's allowed device-side exception for script text, which
// owns the pointer and the keys in its rect. Its undo is the editor's: its own history is kept
// empty (so its menu offers none) and Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z are left to the editor's
// shortcuts, never taken here. The findings show as an icon a line in a gutter of their own (the
// worst one's severity, the shapes and colours of the windows' marks), their messages its tooltip
// over the gutter. It tells its device what the user did (its text changed, the focus left it),
// each outside any pump (a deferred call), and does nothing else with it: the device turns the
// text into requests.
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
	// many lines are marked, and a line's (from 0) severity ("error", "warning", "info"; "" none) and
	// tip.
	void set_document_path(const String &p_path) { path_ = p_path; }
	String get_document_path() const { return path_; }
	int get_mark_count() const { return int(marks_.size()); }
	String get_mark_severity(int p_line) const;
	String get_mark_tip(int p_line) const;

protected:
	static void _bind_methods();

private:
	void on_text_changed_();
	void on_focus_exited_();
	void run_deferred_();
	void on_gui_input_(const Ref<InputEvent> &p_event);
	const opennova::editor::ScriptMark *mark_at_(int p_line) const;

	std::function<void()> text_changed_;
	std::function<void()> focus_exited_;
	std::vector<std::function<void()>> deferred_;
	bool deferred_pending_ = false;
	String path_;
	int gutter_ = 0; // the findings' gutter
	std::array<Ref<ImageTexture>, 3> icons_; // by severity: info, warning, error
	std::vector<opennova::editor::ScriptMark> marks_;
};

} // namespace godot
