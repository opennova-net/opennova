#include <editor/ui/script_view.h>

#include <editor/model/text_document.h>
#include <editor/preview/script_viewport.h>
#include <editor/ui/script_viewport_view.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

ScriptView::ScriptView() : viewport_(std::make_unique<ScriptViewportView>()) {}

ScriptView::~ScriptView() = default;

void ScriptView::draw(Workspace &workspace, const DocumentBase &base) {
	// The events its document was sent are the lines' (a RevealText marks its line as they draw); the
	// device's reveal is its viewport's.
	for (const ViewEvent &event : take_events()) lines_.receive(event);
	const TextDocument *document = text_of(base);
	if (!document) {
		device_drawn_ = false;
		return ui_kit::empty_state("This file holds no text to show.");
	}
	lines_.draw_toolbar(workspace, *document);
	// Why the document takes no edit now, where the toolbar says none of it: a file held read only
	// says so itself (the toolbar's notice and its issues), an operation holding the documents does
	// not, and the control under it, read only, would show no reason.
	if (!document->blocked()) {
		const std::string reason = script_read_only_reason(workspace.view(), *document);
		if (!reason.empty()) ui_kit::empty_state(reason.c_str());
	}
	if (main_viewport(workspace, base)) return;
	lines_.draw_lines(workspace, *document);
}

void ScriptView::rebind(const DocumentBase &document) {
	lines_.rebind(document);
}

bool ScriptView::main_viewport(Workspace &workspace, const DocumentBase &document) {
	device_drawn_ = false;
	if (!workspace.devices()) return false;
	viewport_->draw(workspace, document.path());
	device_drawn_ = viewport_->placed();
	return device_drawn_;
}

void ScriptView::end_frame(Workspace &workspace) {
	viewport_->end_frame(workspace);
	// The device placed in this frame may have been hidden at its end (a window begun after the tab,
	// over its rect): the lines come the frame after.
	device_drawn_ = device_drawn_ && viewport_->placed();
}

} // namespace opennova::editor
