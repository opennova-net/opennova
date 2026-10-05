#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <cstdint>
#include <functional>
#include <string>

#include <editor/preview/script_viewport.h>
#include <editor/preview/text_burst.h>
#include <editor/preview/viewport_device.h>

#include "authoring/script_highlighter.h"
#include "authoring/viewport_devices.h"

namespace opennova::editor {
class DocumentBase;
class TextDocument;
struct EditorRequest;
struct ScriptAssistAsk;
} // namespace opennova::editor

namespace godot {

class ScriptEdit;

// The script device (ADR 0046 S13 V10; ViewportKind::Script's row of authoring/viewport_devices;
// CONTEXT.md "Script device"): the Control flavour the viewport seam admits beside the SubViewport's
// texture (preview/viewport_device.h), a Godot CodeEdit (authoring/script_edit) in a CanvasLayer
// over the ImGui pass's, placed in the rect the script view reserves on each frame it is drawn and
// hidden on a tick no draw came before, where it owns the pointer and the keys: decision 11's
// allowed device-side exception for script text. It lets its focus go on a press of the window
// outside it (the control's own listening to the window's input).
//
// It follows its viewport (preview/script_viewport), never changing it: a Rebuild sets the control's
// text anew; at every pump where the document or the control moved, the control takes the document's
// text again where it holds another (an undo, a redo, a reload, another client's edit, an edit the
// session refused: the document's text is the truth), the fewest characters replaced, its caret and
// its selection kept where the text around them stands; the gutter marks, the highlights and
// whether it takes an edit (read only for a document held so, or while an operation holds the
// documents); a reveal's span selected and scrolled to, once.
//
// Every change the user makes is a request: when the control's text changes, its viewport plans the
// span replacements that take the document to it (ScriptViewport::edit) under the keystroke burst's
// gesture, and the device hands them to the Shell (the sink's `request`), which serves them at once
// (a change it cannot take, a character the game's code page has no byte for, is a notice the Shell
// shows on the status line, and the control takes the document back). The burst ends after a quiet
// second, when the focus leaves the control, or when the document moved under it by another client's
// edit: one EndEdit, raised at a deferred call (never inside a pump). An undo, a redo or a reload
// that moved the document ended its step itself and drops the burst, no EndEdit. A device given up
// with a burst open (the cache's least recently used, a document closed) raises its EndEdit through
// the sink's `request_later`, which the Shell serves at its next pump: the device goes inside one. A
// change made while the document moved under the control (the same frame as another client's edit)
// is not sent: the control takes the document back.
class ScriptDevice final : public opennova::editor::ViewportDevice {
public:
	// The control's layer, one over the imgui-godot layer's (its ImGuiConfig Layer: 128 by default and
	// at most), so it is drawn over the windows.
	static constexpr int kLayer = 129;

	ScriptDevice(Node &owner, ViewportDeviceSink sink);
	~ScriptDevice() override;

	void draw(const opennova::editor::ViewportPicture &picture) override;
	void take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
			const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;

	// Its control (null once freed).
	ScriptEdit *edit() const;

private:
	// The document state the control last held: its instance, its load and its revision.
	struct Held {
		uint64_t identity = 0, load = 0, revision = 0;
		bool operator==(const Held &other) const {
			return identity == other.identity && load == other.load && revision == other.revision;
		}
	};
	static Held held_of(const opennova::editor::DocumentBase &document);
	// Whether the document's move since the control last held it was its history's (an undo, a reload),
	// which ended the burst's step itself, rather than another client's edit.
	bool history_moved_(const opennova::editor::DocumentBase &document) const;
	const opennova::editor::DocumentBase *document_() const;
	const opennova::editor::TextDocument *text_() const;
	void on_text_changed_();
	void on_focus_exited_();
	void end_burst_();
	// What the control asks of the script (S15, session/script_assist): the completions at its caret
	// added to its list, a word's words at a place, the place a Ctrl+click looks up gone to (a request),
	// else a notice that nothing defines it.
	void complete_(bool force);
	std::string hover_(int line, int column) const;
	void lookup_(int line, int column);
	// The viewport's assist shown (the MCP gaps lane): the list or the words at its place, or neither.
	void show_assist_(const opennova::editor::ScriptAssistAsk &assist);
	// The control takes `shown` where it holds another text (the fewest characters replaced, its caret
	// and selection kept): true when it changed.
	bool take_text_(const std::u32string &shown);

	ViewportDeviceSink sink_;
	uint64_t layer_id_ = 0, edit_id_ = 0;
	Ref<ScriptHighlighter> highlighter_;
	// The session's view (the session outlives its devices) and the document's path, as the last take
	// gave them.
	const opennova::editor::SessionView *view_ = nullptr;
	std::string path_;
	Held held_;
	bool control_moved_ = false; // the control's text changed since the last take
	bool drawn_ = false;         // a draw came since the last tick
	bool ending_ = false;        // the burst's end is deferred
	opennova::editor::TextBurst burst_;
	uint64_t marks_serial_ = 0, highlights_serial_ = 0, reveal_seq_ = 0, assist_serial_ = 0;
	// What the assist it took last asked (what its report says shows still).
	opennova::editor::ScriptAssistOp assist_op_ = opennova::editor::ScriptAssistOp::None;
};

} // namespace godot
