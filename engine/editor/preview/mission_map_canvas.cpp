#include <editor/preview/mission_map_canvas.h>

#include <algorithm>
#include <cmath>

#include <editor/model/document.h>
#include <editor/preview/mission_overlay.h>
#include <editor/session/request_factories.h>
#include <editor/session/selection.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <runtime/hud/hud_map_view.h>

namespace opennova::editor {

namespace {

const Document *document_of(const ViewportContext &context) {
	return context.input.document ? records_of(*context.input.document) : nullptr;
}

const Selection *selection_of(const ViewportContext &context, const Document &document) {
	const DocumentsView &documents = context.input.view.documents;
	return documents.active == document.path() ? &documents.selection : nullptr;
}

uint32_t pool_rgb(MissionPool pool) {
	switch (pool) {
	case MissionPool::Item: return kMissionItemRgb;
	case MissionPool::Building: return kMissionBuildingRgb;
	case MissionPool::Marker: return kMissionMarkerRgb;
	case MissionPool::Organic: return kMissionOrganicRgb;
	}
	return kMissionItemRgb;
}

// The most labels drawn with the labels option on (the nearest the middle first is no matter on a map: in order).
constexpr size_t kMapLabelsDrawn = 300;

} // namespace

void MissionMapCanvas::follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) {
	viewport_ = static_cast<const MissionMapViewport *>(&viewport);
	CanvasSubject subject;
	subject.path = viewport.path();
	if (context.input.document) subject.identity = uint64_t(context.input.document->identity());
	gesture_.frame(subject, out);
	marks_ = viewport_->marks(context.width, context.height);
	selected_.clear();
	primary_ = -1;
	const Document *document = document_of(context);
	const Selection *selection = document && viewport.current(context.input) ? selection_of(context, *document) : nullptr;
	if (!selection) return;
	for (const NodeAddress &record : selection->records) {
		const int index = viewport_->scene().mark_index(record.row);
		if (index < 0 || size_t(index) >= marks_.size()) continue;
		if (record == selection->primary) primary_ = index;
		else selected_.push_back(index);
	}
}

void MissionMapCanvas::camera_(const MissionMapCamera &camera, CanvasRequests &out) {
	if (!viewport_ || camera == viewport_->camera()) return;
	out.request(request::set_viewport(viewport_->path(), viewport_->camera_change(camera)));
}

void MissionMapCanvas::input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_) return;
	const MissionMapViewport &viewport = *viewport_;
	if (viewport.status() != ViewportStatus::Ready) return;
	const MissionMapView view = viewport.view(in.width, in.height);
	const std::string &path = viewport.path();
	CanvasSubject subject;
	subject.path = path;
	if (context.input.document) subject.identity = uint64_t(context.input.document->identity());
	const Document *document = document_of(context);
	const bool current = document && viewport.current(context.input);

	// The right or the middle button pans: the mission point under the pointer as it began stays under it.
	if ((in.right_pressed || (in.pressed && in.middle)) && in.hovered && !gesture_.pressed()) {
		panning_ = true;
		pan_from_ = viewport.camera();
		pan_at_ = in.mouse;
	}
	if (panning_) {
		if (!in.right_down && !in.down) {
			panning_ = false;
		} else {
			MissionMapCamera camera = pan_from_;
			// The map follows the pointer, the way it turns (a RotateMap180 mission's south up).
			const double turn = view.flip_180 ? -1.0 : 1.0;
			camera.center[0] = pan_from_.center[0] - turn * double(in.mouse.x - pan_at_.x) * double(view.scale);
			camera.center[1] = pan_from_.center[1] + turn * double(in.mouse.y - pan_at_.y) * double(view.scale);
			camera_(camera, out);
			return;
		}
	}
	// The wheel: the CMAP's zoom step a notch, about the point under the pointer.
	if (in.hovered && in.wheel != 0.0f && !gesture_.pressed()) {
		MissionMapCamera camera = viewport.camera();
		const float step = in.wheel > 0.0f ? hud::kMapViewZoomInStep : hud::kMapViewZoomOutStep;
		camera.zoom = std::clamp(camera.zoom * std::pow(step, std::fabs(in.wheel)), kMissionMapZoomMin, kMissionMapZoomMax);
		double before[2], after[2];
		view.unproject(in.mouse.x, in.mouse.y, before[0], before[1]);
		viewport.view_of(camera, in.width, in.height).unproject(in.mouse.x, in.mouse.y, after[0], after[1]);
		camera.center[0] += before[0] - after[0];
		camera.center[1] += before[1] - after[1];
		camera_(camera, out);
	}
	// F frames the selection, else everything.
	if (in.keyboard.focused && in.keyboard.frame && !gesture_.pressed()) {
		std::string error;
		viewport.command(context, "frame", {}, out, error);
	}

	if (in.pressed && !in.middle && !in.panning && in.hovered) {
		gesture_.press(subject, in.screen, out);
		press_ = Press();
		press_.mark = pick_mission_map_mark(marks_, view, in.mouse.x, in.mouse.y);
		press_.box_from = in.mouse;
		view.unproject(in.mouse.x, in.mouse.y, press_.from[0], press_.from[1]);
		if (press_.mark >= 0 && current) {
			const NodeAddress &record = marks_[size_t(press_.mark)].record;
			press_.taken = viewport.taken(context, *document, record, press_.grabbed);
			const Selection *selection = selection_of(context, *document);
			press_.selected = selection && selection->holds(record);
		}
	}
	if (!gesture_.pressed()) return;
	if (!in.down) {
		// Let go: a click selects the pin under it (joined as the keys say; nothing under it clears the selection), a
		// box selects what it holds; a drag's end is its gesture's.
		const CanvasJoin join = canvas_join(in.keys);
		if (current && !gesture_.dragging()) {
			if (press_.mark >= 0)
				out.request(request::select_record(path, marks_[size_t(press_.mark)].record, select_mode(join)));
			else if (join == CanvasJoin::Replace && (primary_ >= 0 || !selected_.empty()))
				out.request(request::select_record(path, NodeAddress()));
		} else if (current && press_.mark < 0) {
			std::vector<NodeAddress> boxed = mission_map_box_records(marks_, view, press_.box_from, in.mouse);
			if (!boxed.empty())
				out.request(request::select_record(path, boxed.back(), select_mode(join), boxed));
			else if (join == CanvasJoin::Replace && (primary_ >= 0 || !selected_.empty()))
				out.request(request::select_record(path, NodeAddress()));
		}
		gesture_.release(out);
		press_ = Press();
		return;
	}
	const bool began = gesture_.move(in.screen);
	if (!gesture_.dragging() || press_.mark < 0 || press_.taken.empty() || !current) return;
	// A drag of a pin not selected selects it first, so what moves is what is selected (the 3D view's rule).
	if (began && !press_.selected) out.request(request::select_record(path, marks_[size_t(press_.mark)].record));
	if (!context.editable()) return;
	// Each sample from the records as the press found them: the grabbed one as far as the point under the pointer went.
	double at[2];
	view.unproject(in.mouse.x, in.mouse.y, at[0], at[1]);
	const MissionPressed &held = press_.taken[press_.grabbed];
	const double target[2] = { held.x + (at[0] - press_.from[0]), held.y + (at[1] - press_.from[1]) };
	const float snap = in.keys.ctrl ? 0.0f : viewport.options().snap;
	std::vector<Edit> edits;
	if (!mission_move_edits(*document, press_.taken, press_.grabbed, target, snap, viewport.options().stick, context.device,
				gesture_.token(), edits) ||
			edits.empty())
		return;
	out.request(request::edit_record(path, std::move(edits)));
	gesture_.sent();
}

void MissionMapCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	press_ = Press();
	panning_ = false;
}

void MissionMapCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList MissionMapCanvas::shapes(const ViewportContext &context, const CanvasInput &in) const {
	OverlayList list;
	if (!viewport_ || viewport_->status() != ViewportStatus::Ready) return list;
	const MissionMapViewport &viewport = *viewport_;
	const MissionScene &scene = viewport.scene();
	const MissionMapOptions &options = viewport.options();
	const MissionMapView view = viewport.view(in.width, in.height);
	const auto at = [&](double x, double y) {
		CanvasPoint point;
		view.project(x, y, point.x, point.y);
		return point;
	};
	// The paths first, under the pins: a line through each one's stops, the player's route numbered.
	if (options.paths) {
		const int route = mission_map_route(scene);
		for (size_t p = 0; p < scene.paths().size(); ++p) {
			const MissionPathMark &path = scene.paths()[p];
			std::vector<CanvasPoint> stops;
			for (const NodeId stop : path.stops)
				if (const MissionEntityMark *marker = stop ? scene.entity(stop) : nullptr) stops.push_back(at(marker->x, marker->y));
			if (stops.size() < 1) continue;
			const uint32_t rgb = (path.flags & uint32_t(bms::WaypointFlags::PlayerRoute)) ? kMissionBlueRgb
					: (path.flags & uint32_t(bms::WaypointFlags::RedTeam))             ? kMissionRedRgb
					                                                                    : kMissionPathRgb;
			const float thickness = int(p) == route ? 2.0f : 1.0f;
			for (size_t i = 0; i + 1 < stops.size(); ++i) list.line(stops[i], stops[i + 1], rgb, thickness);
			if (stops.size() > 2 && !(path.flags & uint32_t(bms::WaypointFlags::DoesNotLoop)))
				list.line(stops.back(), stops.front(), rgb, thickness);
			if (int(p) == route)
				for (size_t i = 0; i < stops.size(); ++i)
					list.text(CanvasPoint{ stops[i].x + 5.0f, stops[i].y - 7.0f }, std::to_string(i + 1), kMissionBlueRgb);
		}
	}
	// The areas' boxes.
	if (options.areas)
		for (const MissionAreaMark &area : scene.areas()) {
			const CanvasPoint a = at(area.min[0], area.max[1]), b = at(area.max[0], area.min[1]);
			list.line(a, CanvasPoint{ b.x, a.y }, kMissionAreaRgb, 1.0f);
			list.line(CanvasPoint{ b.x, a.y }, b, kMissionAreaRgb, 1.0f);
			list.line(b, CanvasPoint{ a.x, b.y }, kMissionAreaRgb, 1.0f);
			list.line(CanvasPoint{ a.x, b.y }, a, kMissionAreaRgb, 1.0f);
		}
	// The pins (a model drawn as its wireframe, the device's, has none).
	for (const MissionMapMark &mark : marks_) {
		if (!mark.shown || mark.outlined) continue;
		const CanvasPoint point{ mark.px, mark.py };
		if (mark.area >= 0) {
			list.marker(point, OverlayGlyph::Square, 3.0f, OverlayRole::Normal, kMissionAreaRgb);
			continue;
		}
		const uint32_t rgb = pool_rgb(mark.pool);
		switch (mark.pool) {
		case MissionPool::Item:
			list.quad(CanvasPoint{ point.x, point.y - 4.0f }, CanvasPoint{ point.x + 4.0f, point.y },
					CanvasPoint{ point.x, point.y + 4.0f }, CanvasPoint{ point.x - 4.0f, point.y }, rgb);
			break;
		case MissionPool::Building: list.marker(point, OverlayGlyph::Square, 4.0f, OverlayRole::Normal, rgb); break;
		case MissionPool::Marker: list.marker(point, OverlayGlyph::Cross, 5.0f, OverlayRole::Normal, rgb, 1.5f); break;
		case MissionPool::Organic: list.marker(point, OverlayGlyph::Dot, 4.0f, OverlayRole::Normal, rgb); break;
		}
		if (mark.team == 1 || mark.team == 2) list.ring(point, 6.0f, OverlayRole::Normal, 1.0f, mission_team_rgb(mark.team));
	}
	// A wireframe's footprint outlined in `role` (over a dark outline where `dark`, so it reads on any ground).
	const auto footprint = [&](const MissionMapFootprint &shape, OverlayRole role, float thickness, bool dark) {
		const size_t n = shape.hull.size() / 2;
		for (size_t i = 0; i < n; ++i) {
			const size_t j = (i + 1) % n;
			const CanvasPoint a = at(shape.hull[2 * i], shape.hull[2 * i + 1]), b = at(shape.hull[2 * j], shape.hull[2 * j + 1]);
			if (dark) list.line(a, b, 0x000000, thickness + 2.0f, OverlayRole::Normal, 170);
			list.line(a, b, 0xFFFFFF, thickness, role);
		}
	};
	// The selected rings over a dark ring (they read on any ground), the primary's thicker; the hovered one. A
	// wireframe's are its footprint's outline.
	const auto ringed = [&](int index, float thickness) {
		if (index < 0 || size_t(index) >= marks_.size()) return;
		const MissionMapMark &mark = marks_[size_t(index)];
		if (mark.outlined) return footprint(*mark.footprint, OverlayRole::Selected, thickness, true);
		const CanvasPoint point{ mark.px, mark.py };
		list.ring(point, 9.0f, OverlayRole::Normal, thickness + 2.0f, 0x000000, 170);
		list.ring(point, 9.0f, OverlayRole::Selected, thickness);
	};
	for (const int index : selected_) ringed(index, 1.5f);
	ringed(primary_, 2.5f);
	const int hovered = in.hovered && !gesture_.dragging() ? pick_mission_map_mark(marks_, view, in.mouse.x, in.mouse.y) : -1;
	if (hovered >= 0) {
		const MissionMapMark &mark = marks_[size_t(hovered)];
		if (mark.outlined) footprint(*mark.footprint, OverlayRole::Hover, 1.5f, false);
		else list.ring(CanvasPoint{ mark.px, mark.py }, 11.0f, OverlayRole::Hover, 1.5f);
	}
	// The labels: the hovered and the selected; every shown pin's with the labels option.
	std::vector<int> labelled;
	if (hovered >= 0) labelled.push_back(hovered);
	if (primary_ >= 0) labelled.push_back(primary_);
	labelled.insert(labelled.end(), selected_.begin(), selected_.end());
	if (options.labels)
		for (size_t i = 0; i < marks_.size() && labelled.size() < kMapLabelsDrawn; ++i)
			if (marks_[i].shown) labelled.push_back(int(i));
	std::vector<int> drawn;
	for (const int index : labelled) {
		if (std::find(drawn.begin(), drawn.end(), index) != drawn.end()) continue;
		drawn.push_back(index);
		const MissionMapMark &mark = marks_[size_t(index)];
		list.text(CanvasPoint{ mark.px + 8.0f, mark.py + 4.0f }, viewport.title(context, mark.record));
	}
	// The box being dragged from nothing.
	if (gesture_.pressed() && gesture_.dragging() && press_.mark < 0) {
		CanvasPoint a = press_.box_from, b = in.mouse;
		list.rect(CanvasPoint{ std::min(a.x, b.x), std::min(a.y, b.y) }, CanvasPoint{ std::max(a.x, b.x), std::max(a.y, b.y) },
				OverlayRole::Marquee);
	}
	return list;
}

CanvasCursor MissionMapCanvas::cursor(const ViewportContext &, const CanvasInput &in) const {
	if (!viewport_ || in.panning || panning_) return CanvasCursor::Default;
	if (gesture_.pressed()) return press_.mark >= 0 && gesture_.dragging() ? CanvasCursor::Move : CanvasCursor::Default;
	const MissionMapView view = viewport_->view(in.width, in.height);
	return in.hovered && pick_mission_map_mark(marks_, view, in.mouse.x, in.mouse.y) >= 0 ? CanvasCursor::Move
	                                                                                        : CanvasCursor::Default;
}

std::string MissionMapCanvas::hover_tip(const ViewportContext &context, const CanvasInput &in) const {
	if (!viewport_ || gesture_.dragging() || !in.hovered) return std::string();
	const int index = pick_mission_map_mark(marks_, viewport_->view(in.width, in.height), in.mouse.x, in.mouse.y);
	if (index < 0) return std::string();
	const MissionMapMark &mark = marks_[size_t(index)];
	char at[64];
	std::snprintf(at, sizeof(at), " at %.1f east, %.1f north", mark.x, mark.y);
	return viewport_->title(context, mark.record) + at +
	       "\nClick to select it (Shift adds, Ctrl toggles); drag to move it. Right-drag pans, the wheel zooms.";
}

} // namespace opennova::editor
