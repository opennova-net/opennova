#include <editor/preview/model_canvas.h>

#include <algorithm>
#include <cmath>

#include <editor/documents/model_document.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// What the canvas shows: the model's document (this instance of it), none for a model an
// animation plays on.
CanvasSubject subject_of(const ModelCanvasFrame &frame) {
	CanvasSubject subject;
	if (frame.document) {
		subject.path = frame.document->path();
		subject.identity = frame.document->identity();
	}
	return subject;
}

bool is_selected(const ModelCanvasFrame &frame, const ModelOverlay &overlay) {
	return frame.selected >= 0 && overlay.kind == frame.selected_kind &&
			overlay.index == frame.selected;
}

// The viewport's camera set to `camera` (a SetViewport of its path).
void set_camera(const ModelCanvasFrame &frame, const OrbitCamera &camera, CanvasRequests &out) {
	if (frame.model)
		out.request(request::set_viewport(frame.model->path(), model_camera_change(camera)));
}

} // namespace

int model_canvas_under(const ModelCanvasFrame &frame, const CanvasInput &in) {
	if (!in.hovered || !frame.model)
		return -1;
	return pick_model_overlay(frame.overlays, frame.model->camera(), in.width, in.height,
			in.mouse.x, in.mouse.y, kModelPickSlop);
}

ModelGrab model_canvas_grab(const ModelCanvasFrame &frame, const CanvasInput &in, int under) {
	ModelGrab grab;
	grab.pan = in.middle || in.keys.shift;
	grab.pick = under;
	if (grab.pan || !frame.current || frame.selected < 0 || frame.document->blocked() ||
			!frame.editable)
		return grab;
	const OrbitCamera &camera = frame.model->camera();
	for (const ModelOverlay &overlay : frame.overlays) {
		if (!is_selected(frame, overlay))
			continue;
		float hx = 0.0f, hy = 0.0f;
		const auto near_pointer = [&](const PreviewVec3 &point) {
			return camera.project(point, in.width, in.height, hx, hy) &&
					std::fabs(hx - in.mouse.x) <= kModelPickSlop &&
					std::fabs(hy - in.mouse.y) <= kModelPickSlop;
		};
		if (overlay.has_direction && near_pointer(frame.model->axis_tip(overlay)))
			grab.which = ModelHandle::Axis;
		else if (overlay.kind != ModelOverlayKind::Pivot && near_pointer(overlay.at))
			grab.which = ModelHandle::Place;
		else
			break;
		grab.handle = true;
		grab.marker = overlay;
		if (grab.which == ModelHandle::Place)
			grab.others = frame.others;
		grab.offset = CanvasPoint{ hx - in.mouse.x, hy - in.mouse.y };
		break;
	}
	return grab;
}

// --- ModelCanvas -----------------------------------------------------------------------------

void ModelCanvas::follow(
		const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) {
	frame_ = static_cast<const ModelViewport &>(viewport).canvas_frame(context);
	follow(frame_, out);
}

void ModelCanvas::input(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	input(frame_, in, model_canvas_under(frame_, in), out);
}

OverlayList ModelCanvas::shapes(const ViewportContext &, const CanvasInput &in) const {
	return shapes(frame_, in, model_canvas_under(frame_, in));
}

std::string ModelCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	return hover_tip(frame_, model_canvas_under(frame_, in));
}

void ModelCanvas::follow(const ModelCanvasFrame &frame, CanvasRequests &out) {
	gesture_.frame(subject_of(frame), out);
	if (!gesture_.pressed())
		grab_ = ModelGrab();
}

void ModelCanvas::input(
		const ModelCanvasFrame &frame, const CanvasInput &in, int under, CanvasRequests &out) {
	if (!frame.model)
		return;
	const ModelViewport &model = *frame.model;
	// The camera this frame moves to, an orbit or a pan then the wheel's dolly on it: one SetViewport
	// (two would each start from the camera as the frame began, the second undoing the first).
	OrbitCamera camera = model.camera();
	bool camera_moved = false;
	if (in.pressed) {
		gesture_.press(subject_of(frame), in.screen, out);
		grab_ = model_canvas_grab(frame, in, under);
	}
	if (gesture_.pressed() && !in.down) {
		// A click on a marker selects its record (while the picture is the document's).
		if (!gesture_.dragging() && grab_.pick >= 0 && size_t(grab_.pick) < frame.overlays.size() &&
				frame.current) {
			const NodeAddress record = model_overlay_record(
					*frame.document, frame.overlays[size_t(grab_.pick)], model.lod());
			if (record.row)
				out.request(request::select_record(frame.document->path(), record));
		}
		gesture_.release(out);
		grab_ = ModelGrab();
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
		if (gesture_.dragging() && grab_.handle && frame.clock) {
			// The handle follows the pointer (kept where the press took it) in the plane that faces
			// the eye; each step is planned from the markers as they were pressed.
			const float snap = in.keys.alt ? 0.0f : frame.snap;
			std::vector<Edit> edits;
			if (model.handle_edits(*frame.document, grab_.marker, grab_.which,
						in.mouse.x + grab_.offset.x, in.mouse.y + grab_.offset.y, in.width,
						in.height, snap, gesture_.token(), *frame.clock, edits, &grab_.others) &&
					!edits.empty()) {
				out.request(request::edit_record(gesture_.path(), std::move(edits)));
				gesture_.sent();
			}
		} else if (gesture_.dragging() && !grab_.handle && (in.delta.x != 0.0f || in.delta.y != 0.0f)) {
			if (grab_.pan)
				camera.pan(in.delta.x, in.delta.y, in.width);
			else
				camera.orbit(in.delta.x, in.delta.y);
			camera_moved = true;
		}
	}
	if (in.hovered && in.wheel != 0.0f) {
		camera.dolly(std::pow(kModelWheelDolly, in.wheel));
		camera_moved = true;
	}
	if (camera_moved)
		set_camera(frame, camera, out);
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame))
		frame_selected(frame, in.width, in.height, out);
}

void ModelCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	grab_ = ModelGrab();
}

void ModelCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
	if (!gesture_.pressed())
		grab_ = ModelGrab();
}

void ModelCanvas::frame_selected(
		const ModelCanvasFrame &frame, int width, int height, CanvasRequests &out) const {
	if (!frame.model)
		return;
	const ModelViewport &model = *frame.model;
	for (const ModelOverlay &overlay : frame.overlays) {
		if (!is_selected(frame, overlay) || !model.model())
			continue;
		set_camera(frame, model.framed_on(overlay, width, height), out);
		return;
	}
	if (model.model())
		set_camera(frame, model.framed(width, height), out);
}

OverlayList ModelCanvas::shapes(
		const ModelCanvasFrame &frame, const CanvasInput &in, int under) const {
	OverlayList list;
	if (!frame.model)
		return list;
	const ModelViewport &model = *frame.model;
	const OrbitCamera &camera = model.camera();
	const int width = in.width, height = in.height;
	for (size_t i = 0; i < frame.overlays.size(); ++i) {
		const ModelOverlay &overlay = frame.overlays[i];
		float x = 0.0f, y = 0.0f, depth = 0.0f;
		if (!camera.project(overlay.at, width, height, x, y, &depth))
			continue;
		const CanvasPoint at{ x, y };
		const auto line_to = [&](const PreviewVec3 &tip, uint32_t rgb) {
			float tx = 0.0f, ty = 0.0f;
			if (camera.project(tip, width, height, tx, ty))
				list.line(at, CanvasPoint{ tx, ty }, rgb, 1.5f);
		};
		switch (overlay.kind) {
			case ModelOverlayKind::UserPoint:
				if (overlay.has_direction)
					line_to(model.axis_tip(overlay), kUserPointRgb);
				list.quad(CanvasPoint{ at.x, at.y - 4.0f }, CanvasPoint{ at.x + 4.0f, at.y },
						CanvasPoint{ at.x, at.y + 4.0f }, CanvasPoint{ at.x - 4.0f, at.y },
						kUserPointRgb);
				break;
			case ModelOverlayKind::Light: {
				const uint32_t rgb = overlay.color & 0xFFFFFFu;
				if (overlay.radius > 0.0f && depth > 0.0f) {
					const float reach = overlay.radius * OrbitCamera::focal_pixels(width) / depth;
					if (reach > 2.0f && reach < 4.0f * float(width))
						list.ring(at, reach, OverlayRole::Normal, 1.0f, rgb, 0x60);
				}
				if (overlay.has_direction)
					line_to(model.axis_tip(overlay), rgb);
				list.marker(at, OverlayGlyph::Dot, 4.5f, OverlayRole::Normal, rgb);
				break;
			}
			case ModelOverlayKind::Pivot:
				list.marker(at, OverlayGlyph::Cross, 5.0f, OverlayRole::Normal, kPivotRgb, 1.5f);
				break;
		}
		if (int(i) == under)
			list.ring(at, 8.0f, OverlayRole::Hover, 1.5f);
		if (is_selected(frame, overlay)) {
			list.ring(at, 9.0f, OverlayRole::Selected, 2.0f);
			// The selected marker's axis tip: the handle that turns it.
			float tx = 0.0f, ty = 0.0f;
			if (overlay.has_direction &&
					camera.project(model.axis_tip(overlay), width, height, tx, ty))
				list.disc(CanvasPoint{ tx, ty }, 4.0f, OverlayRole::Selected);
		} else {
			// Another selected record's marker, ringed thin: a drag of the primary's place moves it.
			for (const ModelOverlay &other : frame.others)
				if (other.kind == overlay.kind && other.index == overlay.index)
					list.ring(at, 9.0f, OverlayRole::Selected, 1.0f);
		}
	}
	return list;
}

std::string ModelCanvas::hover_tip(const ModelCanvasFrame &frame, int under) const {
	if (gesture_.dragging() || under < 0 || size_t(under) >= frame.overlays.size())
		return std::string();
	return frame.overlays[size_t(under)].name;
}

} // namespace opennova::editor
