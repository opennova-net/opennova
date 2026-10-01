#include "authoring/script_edit.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cmath>

#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::DiagnosticSeverity;

// A mark's icon a side (the gutter a little wider), the size the windows draw theirs at a line.
constexpr int kIconSize = 16;
constexpr int kGutterWidth = kIconSize + 4;

// The windows' colours of a severity (ui_kit::severity_color).
Color severity_color(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return Color(0.75f, 0.75f, 0.75f);
	case DiagnosticSeverity::Warning: return Color(0.95f, 0.80f, 0.40f);
	case DiagnosticSeverity::Error: return Color(0.95f, 0.55f, 0.45f);
	}
	return Color(1.0f, 1.0f, 1.0f);
}

size_t severity_index(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return 0;
	case DiagnosticSeverity::Warning: return 1;
	case DiagnosticSeverity::Error: return 2;
	}
	return 0;
}

// A severity's mark as the windows draw it (ui_kit::severity_marker): an error a filled circle, a
// warning a triangle, a note a ring, in the severity's colour, on a clear square.
Ref<ImageTexture> severity_icon(DiagnosticSeverity severity) {
	Ref<Image> image = Image::create_empty(kIconSize, kIconSize, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.0f, 0.0f, 0.0f, 0.0f));
	const Color color = severity_color(severity);
	const float centre = (kIconSize - 1) * 0.5f;
	const float radius = kIconSize * 0.3f;
	for (int y = 0; y < kIconSize; ++y)
		for (int x = 0; x < kIconSize; ++x) {
			const float dx = float(x) - centre, dy = float(y) - centre;
			const float distance = std::sqrt(dx * dx + dy * dy);
			bool inside = false;
			switch (severity) {
			case DiagnosticSeverity::Error: inside = distance <= radius; break;
			case DiagnosticSeverity::Info: inside = distance <= radius && distance >= radius - 1.5f; break;
			case DiagnosticSeverity::Warning: {
				// Apex up at (centre, centre - 1.2r), base corners at (centre +- 1.2r, centre + 0.9r).
				const float top = centre - radius * 1.2f, bottom = centre + radius * 0.9f;
				if (float(y) < top || float(y) > bottom) break;
				const float half = radius * 1.2f * (float(y) - top) / (bottom - top);
				inside = std::fabs(dx) <= half;
				break;
			}
			}
			if (inside) image->set_pixel(x, y, color);
		}
	return ImageTexture::create_from_image(image);
}

const char *severity_word(DiagnosticSeverity severity) {
	return opennova::editor::diagnostic_severity_label(severity);
}

} // namespace

void ScriptEdit::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_document_path"), &ScriptEdit::get_document_path);
	ClassDB::bind_method(D_METHOD("get_mark_count"), &ScriptEdit::get_mark_count);
	ClassDB::bind_method(D_METHOD("get_mark_severity", "line"), &ScriptEdit::get_mark_severity);
	ClassDB::bind_method(D_METHOD("get_mark_tip", "line"), &ScriptEdit::get_mark_tip);
}

ScriptEdit::ScriptEdit() {
	// A script's text as written: its line numbers, no folding, completion, braces or the debugger's
	// gutters, tabs kept as tabs.
	set_draw_line_numbers(true);
	set_line_folding_enabled(false);
	set_draw_fold_gutter(false);
	set_draw_breakpoints_gutter(false);
	set_draw_bookmarks_gutter(false);
	set_draw_executing_lines_gutter(false);
	set_auto_brace_completion_enabled(false);
	set_code_completion_enabled(false);
	set_indent_using_spaces(false);
	set_highlight_current_line(true);
	// One caret and no selection dragged elsewhere in the text: a change at several places is a step of
	// lines (an indent, planned as one span each: preview/shown_text), never a caret's typing at each
	// of them or a move of text from one place to another. No indent added after a line's end.
	set_multiple_carets_enabled(false);
	set_drag_and_drop_selection_enabled(false);
	set_auto_indent_prefixes(TypedArray<String>());
	// Our paste (_paste) cannot reach the state Godot's pastes a line copied with nothing selected above
	// the current one by, so a copy or a cut with nothing selected takes nothing.
	set_empty_selection_clipboard_enabled(false);
	// The findings' gutter, before the line numbers.
	add_gutter(0);
	gutter_ = 0;
	set_gutter_name(gutter_, "findings");
	set_gutter_type(gutter_, TextEdit::GUTTER_TYPE_ICON);
	set_gutter_width(gutter_, kGutterWidth);
	set_gutter_draw(gutter_, true);
	for (const DiagnosticSeverity severity : {DiagnosticSeverity::Info, DiagnosticSeverity::Warning, DiagnosticSeverity::Error})
		icons_[severity_index(severity)] = severity_icon(severity);
	connect("text_changed", callable_mp(this, &ScriptEdit::on_text_changed_));
	connect("focus_exited", callable_mp(this, &ScriptEdit::on_focus_exited_));
	connect("gui_input", callable_mp(this, &ScriptEdit::on_gui_input_));
}

void ScriptEdit::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// The press that takes the focus from it is the window's, which sees every one before the GUI
		// does (its deferred polling would miss a press and its release in one frame).
		if (Window *window = get_window()) {
			const Callable call = callable_mp(this, &ScriptEdit::on_window_input_);
			if (!window->is_connected("window_input", call)) window->connect("window_input", call);
			window_id_ = window->get_instance_id();
		}
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (Window *window = Object::cast_to<Window>(ObjectDB::get_instance(window_id_))) {
			const Callable call = callable_mp(this, &ScriptEdit::on_window_input_);
			if (window->is_connected("window_input", call)) window->disconnect("window_input", call);
		}
		window_id_ = 0;
	}
}

void ScriptEdit::set_listener(std::function<void()> on_text_changed, std::function<void()> on_focus_exited) {
	text_changed_ = std::move(on_text_changed);
	focus_exited_ = std::move(on_focus_exited);
}

void ScriptEdit::clear_listener() {
	text_changed_ = nullptr;
	focus_exited_ = nullptr;
	deferred_.clear();
}

void ScriptEdit::defer(std::function<void()> call) {
	deferred_.push_back(std::move(call));
	if (deferred_pending_) return;
	deferred_pending_ = true;
	callable_mp(this, &ScriptEdit::run_deferred_).call_deferred();
}

void ScriptEdit::run_deferred_() {
	deferred_pending_ = false;
	std::vector<std::function<void()>> calls = std::move(deferred_);
	deferred_.clear();
	for (const std::function<void()> &call : calls) call();
}

void ScriptEdit::on_text_changed_() {
	// Godot emits it deferred, after the frame's input: outside any pump of the session.
	if (text_changed_) text_changed_();
}

void ScriptEdit::on_focus_exited_() {
	// Emitted where the focus moved, which may be the device's own take (it hides the control): told
	// at the next deferred call, outside it.
	if (focus_exited_) defer(focus_exited_);
}

void ScriptEdit::_paste(int32_t p_caret_index) {
	if (!is_editable()) return;
	// The clipboard's line ends as the text's own, each an LF whether the clipboard's was a CR LF or a
	// CR alone (Godot drops a CR as it takes text, which would join the lines a CR alone ended).
	const String text = DisplayServer::get_singleton()->clipboard_get().replace("\r\n", "\n").replace("\r", "\n");
	if (text.is_empty()) return;
	const int caret = p_caret_index < 0 ? 0 : p_caret_index;
	begin_complex_operation();
	if (has_selection(caret)) delete_selection(caret);
	insert_text_at_caret(text, caret);
	end_complex_operation();
}

void ScriptEdit::on_window_input_(const Ref<InputEvent> &p_event) {
	// A press of a button outside its rect lets its focus go, where the press lands: on a window of the
	// pass (which takes it) or on nothing the GUI would give the focus to. The window's input is in the
	// window's pixels, the rect in its canvas's.
	const InputEventMouseButton *button = Object::cast_to<InputEventMouseButton>(p_event.ptr());
	if (!button || !button->is_pressed() || !has_focus()) return;
	const MouseButton which = button->get_button_index();
	if (which != MOUSE_BUTTON_LEFT && which != MOUSE_BUTTON_RIGHT && which != MOUSE_BUTTON_MIDDLE) return;
	const Transform2D to_canvas = get_viewport()->get_final_transform().affine_inverse();
	if (!get_global_rect().has_point(to_canvas.xform(button->get_position()))) release_focus();
}

void ScriptEdit::on_gui_input_(const Ref<InputEvent> &p_event) {
	// Undo and redo are the editor's (its shortcuts, on the active document): the control's own never
	// runs.
	if (p_event.is_valid() && Object::cast_to<InputEventKey>(p_event.ptr()) &&
			(p_event->is_action("ui_undo", true) || p_event->is_action("ui_redo", true))) {
		accept_event();
		return;
	}
	// Over the gutters: the findings of the line under the pointer, its tooltip.
	const InputEventMouseMotion *motion = Object::cast_to<InputEventMouseMotion>(p_event.ptr());
	if (!motion) return;
	const Vector2 at = motion->get_position();
	String tip;
	if (at.x < float(get_total_gutter_width())) {
		const Vector2i place = get_line_column_at_pos(Vector2i(int(at.x), int(at.y)), false);
		if (const opennova::editor::ScriptMark *mark = mark_at_(place.y)) tip = opennova::to_gd(mark->tip());
	}
	if (tip != get_tooltip_text()) set_tooltip_text(tip);
}

void ScriptEdit::set_marks(const std::vector<opennova::editor::ScriptMark> &marks) {
	// Every mark shown goes: its icon and the index it kept in the line's gutter metadata, both of
	// which move with the line as the control is edited, so a line's mark is the line's own whatever
	// was inserted or removed above it since the findings were made.
	for (int line = 0; line < get_line_count(); ++line)
		if (get_line_gutter_metadata(line, gutter_).get_type() != Variant::NIL) {
			set_line_gutter_icon(line, gutter_, Ref<Texture2D>());
			set_line_gutter_metadata(line, gutter_, Variant());
		}
	marks_ = marks;
	for (size_t i = 0; i < marks_.size(); ++i) {
		const int line = int(marks_[i].line) - 1;
		if (line < 0 || line >= get_line_count()) continue;
		set_line_gutter_icon(line, gutter_, icons_[severity_index(marks_[i].severity)]);
		set_line_gutter_metadata(line, gutter_, int64_t(i));
	}
}

const opennova::editor::ScriptMark *ScriptEdit::mark_at_(int p_line) const {
	if (p_line < 0 || p_line >= get_line_count()) return nullptr;
	const Variant kept = get_line_gutter_metadata(p_line, gutter_);
	if (kept.get_type() != Variant::INT) return nullptr;
	const int64_t index = kept;
	return index >= 0 && size_t(index) < marks_.size() ? &marks_[size_t(index)] : nullptr;
}

String ScriptEdit::get_mark_severity(int p_line) const {
	const opennova::editor::ScriptMark *mark = mark_at_(p_line);
	return mark ? String(severity_word(mark->severity)) : String();
}

String ScriptEdit::get_mark_tip(int p_line) const {
	const opennova::editor::ScriptMark *mark = mark_at_(p_line);
	return mark ? opennova::to_gd(mark->tip()) : String();
}

} // namespace godot
