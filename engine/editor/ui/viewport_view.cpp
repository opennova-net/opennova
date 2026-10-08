#include <editor/ui/viewport_view.h>

#include <base/io/json.h>
#include <editor/model/document_base.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

} // namespace

ViewportView::ViewportView(ViewportKind kind) : kind_(kind) {}

ViewportView::~ViewportView() = default;

void ViewportView::draw(Workspace &workspace, const std::string &path) {
	const SessionView &view = workspace.view();
	path_ = path;
	const ViewportModel *model =
			view.documents.viewports ? view.documents.viewports->find(path, kind_) : nullptr;
	// A viewport drawn has a device, asked for whatever it shows yet: the Shell's pump makes one and
	// attaches it, and the viewport follows its document from then on (a Main-role viewport, which no
	// Preview target names, would otherwise never follow).
	ViewportDevice *device = nullptr;
	if (ViewportDeviceSource *devices = model ? workspace.devices() : nullptr) device = devices->device(path, kind_);
	if (!model || model->status() != ViewportStatus::Ready) {
		draw_empty(workspace, model, path);
		return;
	}
	// A design picture whose size moved (a HUD's screen, DI-20) is laid out on a canvas of the new size.
	const ViewportLayout layout = model->layout();
	if (!canvas_ || layout.design_width != layout_.design_width || layout.design_height != layout_.design_height) {
		canvas_ = std::make_unique<ViewportCanvas>(layout.design_width, layout.design_height);
		layout_ = layout;
	}
	if (!half_) half_ = model->make_canvas();
	// The context carries the device from the first (a toolbar's planner, the canvas's frame: a
	// mission's ground, its area anchors and its stick read it).
	ViewportContext context{ ViewportInput{ view, view.documents.viewports->clock(), open_at(view, path),
									 ChangeClass::None },
		model->size().width, model->size().height, snap, device };
	draw_ready(workspace, *model, context);
}

void ViewportView::draw_empty(Workspace &, const ViewportModel *model, const std::string &) {
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", model ? model->message().c_str() : "This file shows in no viewport yet.");
	ImGui::PopTextWrapPos();
}

void ViewportView::canvas(Workspace &workspace, const ViewportModel &model, ViewportContext &context,
		float height, const std::function<void(const CanvasInput &)> &inside) {
	CanvasWindowRequests requests(workspace);
	// The device before the frame is made: what the half maps of the picture (a mission's marks, its
	// ground) reads it.
	ViewportDeviceSource *devices = workspace.devices();
	ViewportDevice *device = devices ? devices->device(path_, kind_) : nullptr;
	context.device = device;
	half_->follow(model, context, requests);
	ViewportCanvas &ui = *canvas_;
	// What the picture draws behind it: the editor's preference on a kind that draws it, else its own.
	ui.set_backdrop(viewport_kind_row(kind_).backdrop ? workspace.view().project.preview_background
	                                                  : PreviewBackground::Dark);
	if (ui.begin(height, model.state().width, model.state().height)) {
		const CanvasInput &in = ui.input();
		context.width = in.width;
		context.height = in.height;
		// The mouse's place on the picture goes to its device while the canvas shows no pointer of its own
		// there (no handle or selected window under it, no press, drag or pan): a picture that draws the
		// game's pointer draws it there (a menu's, DI-08).
		const bool on_picture = in.hovered && !in.panning && (!in.down || game_input(model)) && in.mouse.x >= 0.0f &&
				in.mouse.y >= 0.0f && in.mouse.x < float(in.width) && in.mouse.y < float(in.height);
		const bool pointer = on_picture && !half_->gesture().pressed() &&
				half_->cursor(context, in) == CanvasCursor::Default;
		// A picture that fills the canvas, or a design picture fitted or scaled, is drawn at the
		// canvas's size: its device sizes itself as it draws, and reports it at the next pump.
		ui.picture(
				[device](const ViewportPicture &picture) {
					if (device) device->draw(picture);
					else ImGui::Dummy(ImVec2(float(picture.width), float(picture.height))); // made at the next pump
				},
				[&] { return half_->hover_tip(context, in); }, pointer);
		// The last picture shows while its device builds the next (S13 V6), or after that build failed.
		switch (model.picture_status()) {
		case ViewportStatus::Loading: {
			// What it builds now and how far (S15: "Loading terrain 120/348 (34%)").
			const OperationProgress &units = model.build().progress;
			const int percent = units.total ? int(units.done * 100 / units.total) : 0;
			ui.badge("Loading " + (units.label.empty() ? std::string() : units.label + " ") + std::to_string(units.done) +
					"/" + std::to_string(units.total) + " (" + std::to_string(percent) + "%)");
			break;
		}
		case ViewportStatus::Failed: ui.badge(model.picture_message()); break;
		default: break;
		}
		if (inside) inside(in);
		half_->input(context, in, requests);
		const CanvasCursor cursor = half_->cursor(context, in);
		// Where the picture draws a pointer of its own the system's is hidden, so one pointer shows: the
		// game's. A press begun this frame shows the canvas's own (a handle's, a move's) instead.
		if (pointer && cursor == CanvasCursor::Default && draws_pointer(model, context, in)) workspace.hide_pointer();
		ui.draw(half_->shapes(context, in), cursor);
	}
	ui.end();
}

bool ViewportView::draws_pointer(const ViewportModel &, const ViewportContext &, const CanvasInput &) {
	return false;
}

bool ViewportView::game_input(const ViewportModel &) const {
	return false;
}

void ViewportView::end_frame(Workspace &workspace) {
	if (half_) {
		CanvasWindowRequests requests(workspace);
		half_->end_frame(requests);
	}
	if (canvas_) canvas_->end_frame();
}

} // namespace opennova::editor
