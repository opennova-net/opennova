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
	if (ViewportDeviceSource *devices = model ? workspace.devices() : nullptr) devices->device(path, kind_);
	if (!model || model->status() != ViewportStatus::Ready) {
		draw_empty(workspace, model, path);
		return;
	}
	if (!canvas_) {
		const ViewportLayout layout = model->layout();
		canvas_ = std::make_unique<ViewportCanvas>(layout.design_width, layout.design_height);
	}
	if (!half_) half_ = model->make_canvas();
	ViewportContext context{ ViewportInput{ view, view.documents.viewports->clock(), open_at(view, path),
									 ChangeClass::None },
		model->state().width, model->state().height, snap, nullptr };
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
	half_->follow(model, context, requests);
	ViewportCanvas &ui = *canvas_;
	if (ui.begin(height, model.state().width, model.state().height)) {
		const CanvasInput &in = ui.input();
		context.width = in.width;
		context.height = in.height;
		ViewportDeviceSource *devices = workspace.devices();
		ViewportDevice *device = devices ? devices->device(path_, kind_) : nullptr;
		context.device = device;
		// A picture that fills the canvas is drawn at the canvas's size, and its device with it.
		if (model.layout().design_width == 0 &&
				(in.width != model.state().width || in.height != model.state().height)) {
			io::JsonValue size = io::JsonValue::make_object();
			size.set("width", io::json_number(in.width));
			size.set("height", io::json_number(in.height));
			requests.request(request::set_viewport(path_, viewport_change(kind_, "device", std::move(size))));
		}
		ui.picture(
				[device](int width, int tall) {
					if (device) device->draw(width, tall);
					else ImGui::Dummy(ImVec2(float(width), float(tall))); // made at the next pump
				},
				[&] { return half_->hover_tip(context, in); });
		if (inside) inside(in);
		half_->input(context, in, requests);
		ui.draw(half_->shapes(context, in), half_->cursor(context, in));
	}
	ui.end();
}

void ViewportView::end_frame(Workspace &workspace) {
	if (half_) {
		CanvasWindowRequests requests(workspace);
		half_->end_frame(requests);
	}
	if (canvas_) canvas_->end_frame();
}

} // namespace opennova::editor
