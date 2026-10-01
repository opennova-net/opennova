#include "authoring/script_device.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>

#include <editor/model/document_base.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/script_viewport.h>
#include <editor/preview/shown_text.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/session_view.h>

#include "authoring/script_edit.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::ShownDiff;
using opennova::editor::ShownText;

// What a planner raises, handed to the Shell as it is raised.
struct Forward final : opennova::editor::CanvasRequests {
	explicit Forward(const ScriptDevice::Requests &to) : requests(to) {}
	void request(opennova::editor::EditorRequest request) override {
		if (requests) requests(request);
	}
	const ScriptDevice::Requests &requests;
};

std::u32string text_of(const ScriptEdit &edit) {
	const String text = edit.get_text();
	return text.length() ? std::u32string(text.ptr(), size_t(text.length())) : std::u32string();
}

String to_string(const std::u32string &text) {
	return String(text.c_str());
}

double now_seconds() {
	return double(Time::get_singleton()->get_ticks_msec()) / 1000.0;
}

// Where a shown offset lands once `diff` replaced its characters: before the change it stays, past it
// it moves with the text after it, inside it it goes to the change's end.
size_t moved(size_t offset, const ShownDiff &diff) {
	if (offset <= diff.from) return offset;
	if (offset >= diff.from + diff.removed) return offset - diff.removed + diff.inserted;
	return diff.from + diff.inserted;
}

} // namespace

ScriptDevice::ScriptDevice(Node &owner, Requests requests) : requests_(std::move(requests)) {
	CanvasLayer *layer = memnew(CanvasLayer);
	layer->set_name("Script device");
	layer->set_layer(kLayer);
	ScriptEdit *edit = memnew(ScriptEdit);
	edit->set_name("ScriptEdit");
	edit->hide();
	highlighter_.instantiate();
	edit->set_syntax_highlighter(highlighter_);
	edit->set_listener([this] { on_text_changed_(); }, [this] { on_focus_exited_(); });
	layer->add_child(edit);
	owner.add_child(layer);
	layer_id_ = layer->get_instance_id();
	edit_id_ = edit->get_instance_id();
}

ScriptDevice::~ScriptDevice() {
	// Nothing told any more: the control and its layer are freed at the frame's end (gone already
	// where the owner's teardown freed them first).
	if (ScriptEdit *edit = this->edit()) edit->clear_listener();
	if (CanvasLayer *layer = Object::cast_to<CanvasLayer>(ObjectDB::get_instance(layer_id_)))
		if (layer->is_inside_tree()) layer->queue_free();
}

ScriptEdit *ScriptDevice::edit() const {
	return Object::cast_to<ScriptEdit>(ObjectDB::get_instance(edit_id_));
}

ScriptDevice::Held ScriptDevice::held_of(const opennova::editor::DocumentBase &document) {
	return Held{ document.identity(), document.load_generation(), document.revision() };
}

const opennova::editor::DocumentBase *ScriptDevice::document_() const {
	if (!view_) return nullptr;
	for (const auto &document : view_->documents.open)
		if (document && document->path() == path_) return document.get();
	return nullptr;
}

void ScriptDevice::draw(const opennova::editor::ViewportPicture &picture) {
	ScriptEdit *edit = this->edit();
	if (!edit) return;
	drawn_ = true;
	// The rect where it shows: the picture within the window's part the canvas shows it in.
	float left = picture.x, top = picture.y;
	float right = picture.x + float(picture.width), bottom = picture.y + float(picture.height);
	if (picture.clip_right > picture.clip_left && picture.clip_bottom > picture.clip_top) {
		left = std::max(left, picture.clip_left);
		top = std::max(top, picture.clip_top);
		right = std::min(right, picture.clip_right);
		bottom = std::min(bottom, picture.clip_bottom);
	}
	if (right <= left || bottom <= top) {
		edit->hide();
		return;
	}
	// The OS window's pixels to its canvas's (the root's stretch, none by default).
	const Transform2D to_canvas = edit->get_viewport()->get_final_transform().affine_inverse();
	edit->set_position(to_canvas.xform(Vector2(left, top)));
	edit->set_size(to_canvas.basis_xform(Vector2(right - left, bottom - top)));
	if (!edit->is_visible()) edit->show();
}

bool ScriptDevice::take_text_(const std::u32string &shown) {
	ScriptEdit *edit = this->edit();
	const std::u32string control = text_of(*edit);
	const size_t caret = ShownText::offset_of(control, size_t(edit->get_caret_line()), size_t(edit->get_caret_column()));
	const ShownDiff diff = ShownText::diff(control, shown, caret);
	if (diff.empty()) return false;
	const bool selected = edit->has_selection();
	const size_t origin = selected ? ShownText::offset_of(control, size_t(edit->get_selection_origin_line()),
											  size_t(edit->get_selection_origin_column()))
								   : caret;
	size_t from_line = 0, from_column = 0, to_line = 0, to_column = 0;
	ShownText::place_of(control, diff.from, from_line, from_column);
	ShownText::place_of(control, diff.from + diff.removed, to_line, to_column);
	edit->begin_complex_operation();
	edit->deselect();
	if (diff.removed) edit->remove_text(int(from_line), int(from_column), int(to_line), int(to_column));
	if (diff.inserted)
		edit->insert_text(to_string(shown.substr(diff.from, diff.inserted)), int(from_line), int(from_column));
	edit->end_complex_operation();
	edit->clear_undo_history();
	// The caret and the selection where the text around them stands.
	size_t caret_line = 0, caret_column = 0, origin_line = 0, origin_column = 0;
	ShownText::place_of(shown, moved(caret, diff), caret_line, caret_column);
	ShownText::place_of(shown, moved(origin, diff), origin_line, origin_column);
	if (selected && moved(origin, diff) != moved(caret, diff)) {
		edit->select(int(origin_line), int(origin_column), int(caret_line), int(caret_column));
	} else {
		edit->set_caret_line(int(caret_line), false);
		edit->set_caret_column(int(caret_column), false);
	}
	return true;
}

void ScriptDevice::take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	ScriptEdit *edit = this->edit();
	if (!edit) return;
	view_ = &view;
	path_ = model.path();
	edit->set_document_path(opennova::to_gd(path_));
	const auto &script = static_cast<const opennova::editor::ScriptViewport &>(model);
	// The size where no canvas drew it this frame (a headless Shell's, its tab hidden): its state's.
	if (!drawn_) edit->set_size(Vector2(float(model.state().width), float(model.state().height)));
	const opennova::editor::DocumentBase *document = document_();
	switch (action) {
	case opennova::editor::ViewportAction::Rebuild: {
		// The document read, read again or first shown: its text anew, the caret kept on its line where
		// the text has it (a reload), the marks and the highlights set again.
		const int line = edit->get_caret_line(), column = edit->get_caret_column();
		edit->set_text(to_string(script.shown_text().text()));
		edit->clear_undo_history();
		edit->set_caret_line(std::min(line, std::max(edit->get_line_count() - 1, 0)), false);
		edit->set_caret_column(column, false);
		burst_.drop();
		marks_serial_ = highlights_serial_ = 0;
		break;
	}
	case opennova::editor::ViewportAction::Clear:
		edit->set_text(String());
		edit->clear_undo_history();
		edit->set_marks({});
		highlighter_->set_highlights({});
		edit->set_editable(false);
		burst_.drop();
		held_ = Held();
		control_moved_ = false;
		report.width = int(edit->get_size().x);
		report.height = int(edit->get_size().y);
		report.canvas_sized = drawn_;
		return;
	case opennova::editor::ViewportAction::Update:
	case opennova::editor::ViewportAction::Keep: break;
	}
	// The document's text is the truth: the control takes it again where it holds another (an edit of
	// another client's, an undo, a refused edit of its own). A burst going on ends with it.
	if (action != opennova::editor::ViewportAction::Rebuild && (control_moved_ || action == opennova::editor::ViewportAction::Update))
		if (take_text_(script.shown_text().text()) && burst_.open() && !ending_) {
			ending_ = true;
			edit->defer([this] { end_burst_(); });
		}
	control_moved_ = false;
	if (document) held_ = held_of(*document);
	if (script.marks_serial() != marks_serial_) {
		edit->set_marks(script.marks());
		marks_serial_ = script.marks_serial();
	}
	if (script.highlights_serial() != highlights_serial_) {
		highlighter_->set_highlights(script.highlights());
		highlights_serial_ = script.highlights_serial();
	}
	edit->set_editable(script.editable());
	// A reveal's span selected and scrolled to, once.
	const opennova::editor::ScriptReveal &reveal = script.reveal();
	if (reveal.seq > reveal_seq_) {
		reveal_seq_ = reveal.seq;
		edit->select(int(reveal.line), int(reveal.column), int(reveal.end_line), int(reveal.end_column));
		edit->set_caret_line(int(reveal.end_line), false);
		edit->set_caret_column(int(reveal.end_column), false);
		edit->center_viewport_to_caret();
	}
	report.width = int(edit->get_size().x);
	report.height = int(edit->get_size().y);
	report.canvas_sized = drawn_;
}

void ScriptDevice::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) {
	ScriptEdit *edit = this->edit();
	if (!edit) return;
	// Shown only on a frame a canvas drew it.
	if (!drawn_ && edit->is_visible()) edit->hide();
	drawn_ = false;
	// A press outside it (on a window of the pass, which takes it) lets its focus go.
	Input *input = Input::get_singleton();
	const bool pressed = input->is_mouse_button_pressed(MOUSE_BUTTON_LEFT) ||
			input->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) || input->is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE);
	if (pressed && !pressed_ && edit->has_focus() && !edit->get_global_rect().has_point(edit->get_global_mouse_position()))
		edit->release_focus();
	pressed_ = pressed;
	// A quiet second ends the burst.
	if (burst_.quiet(now_seconds()) && !ending_) {
		ending_ = true;
		edit->defer([this] { end_burst_(); });
	}
}

void ScriptDevice::on_text_changed_() {
	ScriptEdit *edit = this->edit();
	if (!edit || !view_ || !view_->documents.viewports) return;
	control_moved_ = true;
	edit->clear_undo_history(); // the control's own history stays empty: undo is the editor's
	const opennova::editor::DocumentBase *document = document_();
	const auto *script = dynamic_cast<const opennova::editor::ScriptViewport *>(
			view_->documents.viewports->find(path_, opennova::editor::ViewportKind::Script));
	// The document moved under the control since it last held it: nothing is sent, the control takes
	// the document back at the next pump.
	if (!document || !script || !(held_of(*document) == held_)) return;
	const std::u32string control = text_of(*edit);
	const size_t caret = ShownText::offset_of(control, size_t(edit->get_caret_line()), size_t(edit->get_caret_column()));
	const opennova::editor::ViewportContext context{
		opennova::editor::ViewportInput{ *view_, view_->documents.viewports->clock(), document,
				opennova::editor::ChangeClass::None },
		int(edit->get_size().x), int(edit->get_size().y), 0.0f, this
	};
	Forward out(requests_);
	std::string error;
	// Refused (held read only, a character the code page has none for): the control takes the
	// document back at the next pump.
	if (!script->edit(context, control, caret, now_seconds(), burst_, out, error)) return;
	// The Shell served it at once: the document holds the control's text where it took the edit.
	if (const opennova::editor::DocumentBase *after = document_()) held_ = held_of(*after);
}

void ScriptDevice::on_focus_exited_() {
	end_burst_();
}

void ScriptDevice::end_burst_() {
	ending_ = false;
	Forward out(requests_);
	burst_.end(out);
}

} // namespace godot
