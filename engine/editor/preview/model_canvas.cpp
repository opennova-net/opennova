#include <editor/preview/model_canvas.h>

#include <algorithm>
#include <cmath>

#include <editor/documents/model_document.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_state.h>

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

} // namespace

int model_canvas_under(const ModelCanvasFrame &frame, const CanvasInput &in) {
	if (!in.hovered)
		return -1;
	return pick_model_overlay(frame.overlays, frame.model->camera(), in.width, in.height,
			in.mouse.x, in.mouse.y, kModelPickSlop);
}

ModelGrab model_canvas_grab(const ModelCanvasFrame &frame, const CanvasInput &in, int under) {
	ModelGrab grab;
	grab.pan = in.middle || in.keys.shift;
	grab.pick = under;
	if (grab.pan || !frame.current || frame.selected < 0 || frame.document->blocked() || !frame.editable)
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
		grab.offset = CanvasPoint{ hx - in.mouse.x, hy - in.mouse.y };
		break;
	}
	return grab;
}

// --- ModelCanvas -----------------------------------------------------------------------------

void ModelCanvas::follow(const ModelCanvasFrame &frame, CanvasRequests &out) {
	gesture_.frame(subject_of(frame), out);
	if (!gesture_.pressed())
		grab_ = ModelGrab();
}

void ModelCanvas::input(
		const ModelCanvasFrame &frame, const CanvasInput &in, int under, CanvasRequests &out) {
	ModelPreviewModel &model = *frame.model;
	OrbitCamera &camera = model.camera();
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
				out.select(frame.document->path(), record, CanvasJoin::Replace);
		}
		gesture_.release(out);
		grab_ = ModelGrab();
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
		if (gesture_.dragging() && grab_.handle) {
			// The handle follows the pointer (kept where the press took it) in the plane that faces
			// the eye; each step is planned from the marker as it was pressed.
			const float snap = in.keys.alt ? 0.0f : frame.snap;
			std::vector<Edit> edits;
			if (model.handle_edits(*frame.document, grab_.marker, grab_.which,
						in.mouse.x + grab_.offset.x, in.mouse.y + grab_.offset.y, snap,
						gesture_.token(), edits) &&
					!edits.empty()) {
				out.edits(gesture_.path(), std::move(edits));
				gesture_.sent();
			}
		} else if (gesture_.dragging()) {
			if (grab_.pan)
				camera.pan(in.delta.x, in.delta.y, in.width);
			else
				camera.orbit(in.delta.x, in.delta.y);
		}
	}
	if (in.hovered && in.wheel != 0.0f)
		camera.dolly(std::pow(kModelWheelDolly, in.wheel));
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame))
		frame_selected(frame);
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

void ModelCanvas::frame_selected(const ModelCanvasFrame &frame) const {
	ModelPreviewModel &model = *frame.model;
	if (frame.selected >= 0 && model.model()) {
		for (const ModelOverlay &overlay : model.overlays()) {
			if (!is_selected(frame, overlay))
				continue;
			PreviewVec3 center;
			float radius = 1.0f;
			model_preview_sphere(*model.model(), center, radius);
			const bool reach = overlay.kind == ModelOverlayKind::Light && overlay.radius > 0.0f;
			const float around = reach ? overlay.radius : std::max(0.25f, radius * 0.15f);
			model.camera().frame(overlay.at, around, model.device_width(), model.device_height());
			return;
		}
	}
	model.frame();
}

OverlayList ModelCanvas::shapes(
		const ModelCanvasFrame &frame, const CanvasInput &in, int under) const {
	OverlayList list;
	const ModelPreviewModel &model = *frame.model;
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
