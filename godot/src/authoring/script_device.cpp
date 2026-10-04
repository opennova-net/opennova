#include "authoring/script_device.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/resource.hpp>
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
#include <editor/model/text_document.h>
#include <editor/session/editor_request.h>
#include <editor/session/script_assist.h>
#include <editor/session/view/session_view.h>

#include "authoring/script_edit.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::ShownDiff;
using opennova::editor::ShownText;

using RequestSink = std::function<void(const opennova::editor::EditorRequest &)>;

// What a planner raises, handed to the Shell as it is raised.
struct Forward final : opennova::editor::CanvasRequests {
	explicit Forward(const RequestSink &to) : requests(to) {}
	void request(opennova::editor::EditorRequest request) override {
		if (requests) requests(request);
	}
	const RequestSink &requests;
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

ScriptDevice::ScriptDevice(Node &owner, ViewportDeviceSink sink) : sink_(std::move(sink)) {
	CanvasLayer *layer = memnew(CanvasLayer);
	layer->set_name("Script device");
	layer->set_layer(kLayer);
	ScriptEdit *edit = memnew(ScriptEdit);
	edit->set_name("ScriptEdit");
	edit->hide();
	highlighter_.instantiate();
	edit->set_syntax_highlighter(highlighter_);
	edit->set_listener([this] { on_text_changed_(); }, [this] { on_focus_exited_(); });
	edit->set_assist({[this](bool force) { complete_(force); },
	                  [this](int line, int column) { return hover_(line, column); },
	                  [this](int line, int column) { lookup_(line, column); }});
	layer->add_child(edit);
	owner.add_child(layer);
	layer_id_ = layer->get_instance_id();
	edit_id_ = edit->get_instance_id();
}

ScriptDevice::~ScriptDevice() {
	// A burst still open ends all the same. The device goes inside a pump (the cache gave it up, its
	// document closed), where a request is not served: its EndEdit is the Shell's to serve at its next.
	if (burst_.open()) {
		Forward later(sink_.request_later);
		burst_.end(later);
	}
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

bool ScriptDevice::history_moved_(const opennova::editor::DocumentBase &document) const {
	// The document is another instance or load than the control held (a reload: its history is gone), or
	// has a step to redo, which an edit the burst left open cannot leave (the burst's own edit discards
	// the redo branch), so an undo took it back. Another client's edit is neither.
	const Held now = held_of(document);
	return now.identity != held_.identity || now.load != held_.load || document.can_redo();
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
	// A picture with no room (what the script view's second look at the frame's end draws where a
	// window begun after the tab lies over the rect) hides the control the same frame.
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
	// A burst going on when the document moved under the control, to a state it did not hold (its own
	// edits leave it holding the document's): where its history moved it (an undo, a reload) the step
	// the burst made ended with it and the burst is dropped, no EndEdit; where another client's edit
	// moved it the burst ends, one EndEdit at a deferred call.
	if (document && burst_.open() && !(held_of(*document) == held_)) {
		if (history_moved_(*document)) {
			burst_.drop();
		} else if (!ending_) {
			ending_ = true;
			edit->defer([this] { end_burst_(); });
		}
	}
	switch (action) {
	case opennova::editor::ViewportAction::Rebuild: {
		// The document first shown or another one at its path: its text anew, the caret kept on its line
		// where the text has it, the marks and the highlights set again.
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
	// another client's, an undo, a reload, a refused edit of its own).
	if (action != opennova::editor::ViewportAction::Rebuild && (control_moved_ || action == opennova::editor::ViewportAction::Update))
		take_text_(script.shown_text().text());
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
	// The help asked at a place (the viewport's assist, the MCP gaps lane), once an ask.
	const opennova::editor::ScriptAssistAsk &assist = script.assist();
	if (assist.serial != assist_serial_) {
		assist_serial_ = assist.serial;
		show_assist_(assist);
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
	Forward out(sink_.request);
	std::string error;
	// Refused (held read only, a character the code page has none for): the person is told why, and
	// the control takes the document back at the next pump.
	if (!script->edit(context, control, caret, now_seconds(), burst_, out, error)) {
		if (!error.empty() && sink_.notice) sink_.notice(error);
		return;
	}
	// The Shell served it at once: the document holds the control's text where it took the edit.
	if (const opennova::editor::DocumentBase *after = document_()) held_ = held_of(*after);
}

void ScriptDevice::on_focus_exited_() {
	end_burst_();
}

const opennova::editor::TextDocument *ScriptDevice::text_() const {
	const opennova::editor::DocumentBase *document = document_();
	return document ? opennova::editor::text_of(*document) : nullptr;
}

void ScriptDevice::complete_(bool force) {
	ScriptEdit *edit = this->edit();
	const opennova::editor::TextDocument *text = text_();
	if (!edit || !text || !view_) return;
	// The control's places are the document's (a script's text is its code page's characters, one
	// each), from 0. The line as the control holds it: the keystroke that asked for the list reaches the
	// document at the next deferred call. A character past ASCII is no word's.
	const int caret_line = edit->get_caret_line();
	const String held = edit->get_line(caret_line);
	std::string line(size_t(held.length()), '?');
	for (int64_t i = 0; i < held.length(); ++i)
		if (held[i] < 0x80) line[size_t(i)] = char(held[i]);
	const opennova::editor::ScriptCompletions completions = opennova::editor::script_completions(
			*view_, *text, size_t(caret_line) + 1, size_t(edit->get_caret_column()) + 1, &line);
	for (const opennova::editor::ScriptCompletion &item : completions.items) {
		const CodeEdit::CodeCompletionKind kind = item.kind == "command" ? CodeEdit::KIND_FUNCTION
		                                          : item.kind == "keyword" ? CodeEdit::KIND_PLAIN_TEXT
		                                          : item.kind == "text key" || item.kind == "effect" || item.kind == "ammo"
		                                                  ? CodeEdit::KIND_MEMBER
		                                                  : CodeEdit::KIND_CONSTANT;
		edit->add_code_completion_option(kind, opennova::to_gd(item.label), opennova::to_gd(item.insert), Color(1, 1, 1),
				Ref<Resource>(), opennova::to_gd(item.detail));
	}
	edit->update_code_completion_options(force);
}

std::string ScriptDevice::hover_(int line, int column) const {
	const opennova::editor::TextDocument *text = text_();
	if (!text || !view_) return std::string();
	opennova::editor::ScriptHover hover;
	return opennova::editor::script_hover(*view_, *text, size_t(line) + 1, size_t(column) + 1, hover) ? hover.text
	                                                                                                     : std::string();
}

void ScriptDevice::lookup_(int line, int column) {
	const opennova::editor::TextDocument *text = text_();
	if (!text || !view_) return;
	opennova::editor::EditorRequest request;
	if (opennova::editor::script_definition(*view_, *text, size_t(line) + 1, size_t(column) + 1, request)) {
		if (sink_.request) sink_.request(request);
	} else if (sink_.notice) {
		sink_.notice("The editor knows no place that defines this word.");
	}
}

// The caret put at the place, then the completion list there as typing a word's character shows it, or the
// word's words there in a box under it as the pointer shows them; with neither asked, both closed.
void ScriptDevice::show_assist_(const opennova::editor::ScriptAssistAsk &assist) {
	ScriptEdit *edit = this->edit();
	if (!edit) return;
	edit->cancel_code_completion();
	edit->set_hover_note(String());
	if (assist.op == opennova::editor::ScriptAssistOp::None) return;
	edit->deselect();
	edit->set_caret_line(int(assist.shown_line), false);
	edit->set_caret_column(int(assist.shown_column), false);
	edit->center_viewport_to_caret();
	if (assist.op == opennova::editor::ScriptAssistOp::Complete) {
		complete_(true);
		return;
	}
	const std::string words = hover_(int(assist.shown_line), int(assist.shown_column));
	edit->set_hover_note(opennova::to_gd(words.empty() ? std::string("Nothing the editor knows is here.") : words),
			int(assist.shown_line), int(assist.shown_column));
}

void ScriptDevice::end_burst_() {
	ending_ = false;
	Forward out(sink_.request);
	burst_.end(out);
}

} // namespace godot
