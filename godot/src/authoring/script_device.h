#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <cstdint>
#include <functional>
#include <string>

#include <editor/preview/text_burst.h>
#include <editor/preview/viewport_device.h>

#include "authoring/script_highlighter.h"

namespace opennova::editor {
class DocumentBase;
struct EditorRequest;
} // namespace opennova::editor

namespace godot {

class ScriptEdit;

// The script device (ADR 0046 S13 V10; ViewportKind::Script's row of authoring/viewport_devices;
// CONTEXT.md "Script device"): the Control flavour the viewport seam admits beside the SubViewport's
// texture (preview/viewport_device.h), a Godot CodeEdit (authoring/script_edit) in a CanvasLayer
// over the ImGui pass's, placed in the rect the script view reserves on each frame it is drawn and
// hidden on a tick no draw came before, where it owns the pointer and the keys: decision 11's
// allowed device-side exception for script text. A press outside it lets its focus go.
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
// span replacement that takes the document to it (ScriptViewport::edit) under the keystroke burst's
// gesture, and the device hands it to the Shell (`requests`), which serves it at once. The burst ends
// after a quiet second, when the focus leaves the control, or when the document moved under it: one
// EndEdit, raised at a deferred call (never inside a pump). A change made while the document moved
// under the control (the same frame as another client's edit) is not sent: the control takes the
// document back.
class ScriptDevice final : public opennova::editor::ViewportDevice {
public:
	// What a request the device makes is handed to (the Shell serves it at once, EditorApp).
	using Requests = std::function<void(const opennova::editor::EditorRequest &)>;
	// The control's layer, one over the imgui-godot layer's (its ImGuiConfig Layer: 128 by default and
	// at most), so it is drawn over the windows.
	static constexpr int kLayer = 129;

	ScriptDevice(Node &owner, Requests requests);
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
	const opennova::editor::DocumentBase *document_() const;
	void on_text_changed_();
	void on_focus_exited_();
	void end_burst_();
	// The control takes `shown` where it holds another text (the fewest characters replaced, its caret
	// and selection kept): true when it changed.
	bool take_text_(const std::u32string &shown);

	Requests requests_;
	uint64_t layer_id_ = 0, edit_id_ = 0;
	Ref<ScriptHighlighter> highlighter_;
	// The session's view (the session outlives its devices) and the document's path, as the last take
	// gave them.
	const opennova::editor::SessionView *view_ = nullptr;
	std::string path_;
	Held held_;
	bool control_moved_ = false; // the control's text changed since the last take
	bool drawn_ = false;         // a draw came since the last tick
	bool pressed_ = false;       // a mouse button was down at the last tick
	bool ending_ = false;        // the burst's end is deferred
	opennova::editor::TextBurst burst_;
	uint64_t marks_serial_ = 0, highlights_serial_ = 0, reveal_seq_ = 0;
};

} // namespace godot
