#include <editor/ui/script_view.h>

#include <editor/model/text_document.h>
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
}

} // namespace opennova::editor
