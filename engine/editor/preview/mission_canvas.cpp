#include <editor/preview/mission_canvas.h>

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <editor/model/document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

CanvasSubject subject_of(const MissionCanvasFrame &frame) {
	CanvasSubject subject;
	if (frame.document) {
		subject.path = frame.document->path();
		subject.identity = frame.document->identity();
	}
	return subject;
}

bool is_selected(const MissionCanvasFrame &frame, int mark) {
	return mark >= 0 && (mark == frame.primary ||
								std::find(frame.selected.begin(), frame.selected.end(), mark) != frame.selected.end());
}

void set_camera(const MissionCanvasFrame &frame, const OrbitCamera &camera, CanvasRequests &out) {
	if (frame.viewport) out.request(request::set_viewport(frame.viewport->path(), mission_camera_change(camera)));
}

// A handle's pixel within the pick slop of the pointer; `offset` from the pointer to it.
bool near_pointer(const OrbitCamera &camera, const CanvasInput &in, const PreviewVec3 &point, CanvasPoint &offset) {
	float x = 0.0f, y = 0.0f;
	if (!camera.project(point, in.width, in.height, x, y) || std::fabs(x - in.mouse.x) > kMissionPickSlop ||
			std::fabs(y - in.mouse.y) > kMissionPickSlop)
		return false;
	offset = CanvasPoint{ x - in.mouse.x, y - in.mouse.y };
	return true;
}

// A turn of `to` from `from`, degrees in -180..180.
double turned(double from, double to) {
	double by = std::fmod(to - from, 360.0);
	if (by > 180.0) by -= 360.0;
	if (by < -180.0) by += 360.0;
	return by;
}

// The selected records of the frame as a press finds them: the entities, and the areas where `areas`.
std::vector<MissionPressed> selected_pressed(const MissionCanvasFrame &frame, bool areas, int grabbed_mark,
		size_t &grabbed) {
	std::vector<MissionPressed> out;
	grabbed = 0;
	const auto take = [&](int mark) {
		MissionPressed pressed;
		if (mark < 0 || !frame.viewport->pressed(frame.marks[size_t(mark)].record, pressed) || (pressed.area && !areas))
			return;
		if (mark == grabbed_mark) grabbed = out.size();
		out.push_back(pressed);
	};
	take(frame.primary);
	for (const int mark : frame.selected) take(mark);
	return out;
}

} // namespace

int mission_canvas_under(const MissionCanvasFrame &frame, const CanvasInput &in) {
	if (!in.hovered || !frame.viewport) return -1;
	return pick_mission_mark(frame.marks, in.mouse.x, in.mouse.y);
}

MissionGrab mission_canvas_grab(const MissionCanvasFrame &frame, const ViewportContext &context, const CanvasInput &in,
		int under) {
	MissionGrab grab;
	grab.pick = under;
	grab.join = canvas_join(in.keys);
	grab.from = grab.to = in.mouse;
	if (in.middle) {
		grab.what = MissionGrab::What::Pan;
		return grab;
	}
	if (!frame.viewport) return grab;
	const MissionViewport &viewport = *frame.viewport;
	const OrbitCamera &camera = viewport.camera();
	const bool edits = frame.current && frame.editable && !frame.document->blocked() && grab.join == CanvasJoin::Replace;
	// The primary's own handles first: its height and its yaw, an area's edges.
	if (edits && frame.primary >= 0) {
		const MissionMark &primary = frame.marks[size_t(frame.primary)];
		for (const MissionHandle handle : { MissionHandle::Yaw, MissionHandle::Height, MissionHandle::XMin,
					 MissionHandle::XMax, MissionHandle::YMin, MissionHandle::YMax }) {
			PreviewVec3 at;
			if (!viewport.handle_at(primary, handle, at) || !near_pointer(camera, in, at, grab.offset)) continue;
			grab.what = MissionGrab::What::Handle;
			grab.handle = handle;
			grab.through = at;
			if (mission_handle_is_edge(handle)) {
				MissionPressed area;
				viewport.pressed(primary.record, area);
				grab.pressed.push_back(area);
			} else {
				grab.pressed = selected_pressed(frame, false, frame.primary, grab.grabbed);
			}
			if (grab.pressed.empty()) continue;
			const MissionPressed &held = grab.pressed[grab.grabbed];
			double anchor[3];
			preview_to_mission(primary.at, anchor);
			grab.grounded = mission_ground_point(context, camera, in.mouse.x + grab.offset.x, in.mouse.y + grab.offset.y,
					held.area ? anchor[2] : held.z, grab.ground);
			return grab;
		}
	}
	if (under >= 0) {
		if (!edits) return grab; // a click selects it; nothing a drag moves
		// The mark moves on the ground with every selected record; one not selected alone, selected
		// first.
		const MissionMark &mark = frame.marks[size_t(under)];
		grab.what = MissionGrab::What::Handle;
		grab.handle = MissionHandle::Move;
		grab.through = mark.at;
		grab.offset = CanvasPoint{ mark.x - in.mouse.x, mark.y - in.mouse.y };
		if (is_selected(frame, under)) {
			grab.pressed = selected_pressed(frame, true, under, grab.grabbed);
		} else {
			MissionPressed alone;
			if (viewport.pressed(mark.record, alone)) grab.pressed.push_back(alone);
			grab.select_first = true;
		}
		if (grab.pressed.empty()) {
			grab.what = MissionGrab::What::None;
			return grab;
		}
		double anchor[3];
		preview_to_mission(mark.at, anchor);
		grab.grounded = mission_ground_point(context, camera, mark.x, mark.y, anchor[2], grab.ground);
		return grab;
	}
	grab.what = in.keys.alt ? MissionGrab::What::Orbit : MissionGrab::What::Marquee;
	return grab;
}

// --- MissionCanvas -------------------------------------------------------------------------------

void MissionCanvas::follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) {
	frame_ = static_cast<const MissionViewport &>(viewport).canvas_frame(context);
	gesture_.frame(subject_of(frame_), out);
	// A nudge is of the selection it began on.
	if (gesture_.nudging() && nudged_ != frame_.records) gesture_.end(out);
	if (!gesture_.pressed()) grab_ = MissionGrab();
}

void MissionCanvas::input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (!frame_.viewport) return;
	const MissionViewport &viewport = *frame_.viewport;
	const int under = mission_canvas_under(frame_, in);
	// A nudge lasts while an arrow is held on the canvas that has the keyboard.
	if (gesture_.nudging() && (!in.keyboard.arrow_held || !in.keyboard.focused)) end(out);
	// The camera this frame moves to: a look and a fly, a pan or an orbit, then the wheel's dolly on it,
	// one SetViewport (two would each start from the camera as the frame began).
	OrbitCamera camera = viewport.camera();
	bool camera_moved = false;
	if (in.right_pressed) looking_ = true;
	if (!in.right_down) looking_ = false;
	if (looking_) {
		if (in.delta.x != 0.0f || in.delta.y != 0.0f) {
			mission_camera_look(camera, in.delta.x, in.delta.y);
			camera_moved = true;
		}
		const CanvasKeyboard &keys = in.keyboard;
		if (keys.move_x || keys.move_y || keys.move_z) {
			const float step = kMissionFlySpeed * (keys.fast ? kMissionFlyFast : 1.0f) * in.dt;
			mission_camera_fly(camera, float(keys.move_x) * step, float(keys.move_y) * step, float(keys.move_z) * step);
			camera_moved = camera_moved || step != 0.0f;
		}
	} else {
		keys_(context, in, out);
	}
	if (in.pressed) {
		gesture_.press(subject_of(frame_), in.screen, out);
		grab_ = mission_canvas_grab(frame_, context, in, under);
	}
	if (gesture_.pressed() && !in.down) {
		release_(out);
	} else if (gesture_.pressed()) {
		if (gesture_.move(in.screen) && grab_.what == MissionGrab::What::Handle && grab_.select_first &&
				grab_.pick >= 0 && size_t(grab_.pick) < frame_.marks.size())
			out.request(request::select_record(gesture_.path(), frame_.marks[size_t(grab_.pick)].record));
		if (gesture_.dragging()) {
			switch (grab_.what) {
			case MissionGrab::What::Handle: drag_(context, in, out); break;
			case MissionGrab::What::Marquee: grab_.to = in.mouse; break;
			case MissionGrab::What::Pan:
				if (in.delta.x != 0.0f || in.delta.y != 0.0f) {
					camera.pan(in.delta.x, in.delta.y, in.width);
					camera_moved = true;
				}
				break;
			case MissionGrab::What::Orbit:
				if (in.delta.x != 0.0f || in.delta.y != 0.0f) {
					camera.orbit(in.delta.x, in.delta.y);
					camera_moved = true;
				}
				break;
			case MissionGrab::What::None: break;
			}
		}
	}
	if (in.hovered && in.wheel != 0.0f) {
		// Toward the ground under the pointer (the eye goes to it as far as the distance shrinks), else
		// along the view.
		const float factor = std::pow(kMissionWheelDolly, in.wheel);
		double target[3], ground[3];
		preview_to_mission(camera.target, target);
		if (mission_ground_point(context, camera, in.mouse.x, in.mouse.y, target[2], ground)) {
			const PreviewVec3 point = mission_to_preview(ground);
			camera.target = PreviewVec3{ point.x + (camera.target.x - point.x) * factor,
				point.y + (camera.target.y - point.y) * factor, point.z + (camera.target.z - point.z) * factor };
		}
		camera.dolly(factor);
		camera_moved = true;
	}
	if (camera_moved) set_camera(frame_, camera, out);
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame && !looking_))
		frame_selected(in.width, in.height, out);
}

void MissionCanvas::keys_(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	const CanvasKeyboard &keyboard = in.keyboard;
	if (!keyboard.focused || gesture_.pressed() || !frame_.document) return;
	const std::string &path = frame_.document->path();
	if (keyboard.escape && !frame_.records.empty()) {
		out.request(request::select_record(path, NodeAddress()));
		return;
	}
	if (!frame_.editable || !frame_.current || frame_.records.empty()) return;
	if (keyboard.remove) {
		// The selected records removed, one batch: the type repairs what named them.
		std::vector<Edit> edits;
		for (const NodeAddress &record : frame_.records) {
			Edit edit;
			edit.operation = EditOperation::Remove;
			edit.address = record;
			edits.push_back(std::move(edit));
		}
		out.request(request::edit_record(path, std::move(edits)));
		return;
	}
	if (!keyboard.arrow_x && !keyboard.arrow_y && !keyboard.page) return;
	// A step along the file's axis nearest the camera's right (the left and right arrows) and its
	// forward (up and down); PgUp and PgDn the height.
	const float step = frame_.snap > 0.0f ? frame_.snap : 1.0f;
	const double heading = mission_camera_heading(frame_.viewport->camera()) * io::kRadiansPerDegree;
	const double forward_east = std::round(std::sin(heading)), forward_north = std::round(std::cos(heading));
	const bool diagonal = forward_east != 0.0 && forward_north != 0.0;
	const double fe = diagonal ? 0.0 : forward_east, fn = diagonal ? (forward_north > 0.0 ? 1.0 : -1.0) : forward_north;
	// Right of a heading (east, north) is (north, -east).
	const double east = (double(keyboard.arrow_x) * fn - double(keyboard.arrow_y) * fe) * step;
	const double north = (-double(keyboard.arrow_x) * fe - double(keyboard.arrow_y) * fn) * step;
	nudge_by_(east, north, double(keyboard.page) * step, out);
}

void MissionCanvas::drag_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (grab_.pressed.empty() || !frame_.viewport || !frame_.document) return;
	const MissionViewport &viewport = *frame_.viewport;
	const OrbitCamera &camera = viewport.camera();
	const MissionPressed &held = grab_.pressed[grab_.grabbed];
	// The handle follows the pointer, kept where the press took it.
	const float x = in.mouse.x + grab_.offset.x, y = in.mouse.y + grab_.offset.y;
	const float snap = in.keys.alt ? 0.0f : frame_.snap;
	std::vector<Edit> edits;
	switch (grab_.handle) {
	case MissionHandle::Move: {
		// As far over the ground as the pointer went since the press.
		double ground[3];
		if (!grab_.grounded || !mission_ground_point(context, camera, x, y, grab_.ground[2], ground)) return;
		const double to[2] = { held.x + (ground[0] - grab_.ground[0]), held.y + (ground[1] - grab_.ground[1]) };
		mission_move_edits(*frame_.document, grab_.pressed, grab_.grabbed, to, snap, viewport.options().stick,
				frame_.device, gesture_.token(), edits);
		break;
	}
	case MissionHandle::Height: {
		PreviewVec3 at;
		if (!camera.on_view_plane(x, y, in.width, in.height, grab_.through, at)) return;
		mission_height_edits(*frame_.document, grab_.pressed, grab_.grabbed, double(at.y) - double(grab_.through.y), snap,
				gesture_.token(), edits);
		break;
	}
	case MissionHandle::Yaw: {
		double at[3];
		if (!mission_camera_on_height(camera, x, y, in.width, in.height, held.z, at) ||
				(at[0] == held.x && at[1] == held.y))
			return;
		const double heading = std::atan2(at[0] - held.x, at[1] - held.y) / io::kRadiansPerDegree;
		mission_yaw_edits(*frame_.document, grab_.pressed, grab_.grabbed, turned(double(held.yaw), heading),
				in.keys.alt ? 0.0f : frame_.turn_snap, gesture_.token(), edits);
		break;
	}
	default: {
		double ground[3];
		if (!grab_.grounded || !mission_ground_point(context, camera, x, y, grab_.ground[2], ground)) return;
		const bool east = grab_.handle == MissionHandle::XMin || grab_.handle == MissionHandle::XMax;
		mission_area_edge_edits(*frame_.document, held, grab_.handle, ground[east ? 0 : 1], snap, gesture_.token(), edits);
		break;
	}
	}
	if (edits.empty()) return;
	out.request(request::edit_record(gesture_.path(), std::move(edits)));
	gesture_.sent();
}

void MissionCanvas::release_(CanvasRequests &out) {
	const std::string &path = gesture_.path();
	if (grab_.what == MissionGrab::What::Marquee && gesture_.dragging()) {
		// The records in the box, one selection: nearest last, the primary; joined as the keys say. A
		// box that takes nothing selects nothing.
		if (frame_.current) {
			std::vector<NodeAddress> boxed = mission_box_records(frame_.marks, grab_.from, grab_.to);
			std::reverse(boxed.begin(), boxed.end());
			const bool replace = grab_.join == CanvasJoin::Replace;
			if (!boxed.empty())
				out.request(request::select_record(path, boxed.back(), select_mode(grab_.join), boxed));
			else if (replace && !frame_.records.empty())
				out.request(request::select_record(path, NodeAddress()));
		}
	} else if (!gesture_.dragging() && grab_.what != MissionGrab::What::Pan && frame_.current) {
		// A click: the mark under it selected, joined as the keys say; on nothing, nothing selected.
		if (grab_.pick >= 0 && size_t(grab_.pick) < frame_.marks.size())
			out.request(request::select_record(path, frame_.marks[size_t(grab_.pick)].record, select_mode(grab_.join)));
		else if (grab_.join == CanvasJoin::Replace && !frame_.records.empty())
			out.request(request::select_record(path, NodeAddress()));
	}
	gesture_.release(out);
	grab_ = MissionGrab();
}

void MissionCanvas::nudge_by_(double east, double north, double up, CanvasRequests &out) {
	if ((east == 0.0 && north == 0.0 && up == 0.0) || gesture_.pressed() || !frame_.viewport) return;
	if (!gesture_.nudging()) {
		size_t grabbed = 0;
		nudge_ = selected_pressed(frame_, true, frame_.primary, grabbed);
		if (nudge_.empty()) return;
		gesture_.nudge(subject_of(frame_), out);
		nudged_ = frame_.records;
		nudge_east_ = nudge_north_ = nudge_up_ = 0.0;
	}
	nudge_east_ += east;
	nudge_north_ += north;
	nudge_up_ += up;
	// From the records as the nudge began, each entity lifted as far as the nudge has: one move writes
	// the place and the height together.
	std::vector<MissionPressed> lifted = nudge_;
	for (MissionPressed &each : lifted)
		if (!each.area) each.z += nudge_up_;
	const double to[2] = { lifted.front().x + nudge_east_, lifted.front().y + nudge_north_ };
	std::vector<Edit> edits;
	// Its height over the ground kept only by a move along it: a lift is the author's own.
	const bool stick = frame_.viewport->options().stick && nudge_up_ == 0.0;
	if (mission_move_edits(*frame_.document, lifted, 0, to, 0.0f, stick, frame_.device, gesture_.token(), edits) &&
			!edits.empty()) {
		out.request(request::edit_record(gesture_.path(), std::move(edits)));
		gesture_.sent();
	}
}

void MissionCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	grab_ = MissionGrab();
	nudge_.clear();
}

void MissionCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
	if (!gesture_.pressed()) grab_ = MissionGrab();
}

void MissionCanvas::frame_selected(int width, int height, CanvasRequests &out) const {
	if (!frame_.viewport) return;
	std::vector<int> of = frame_.selected;
	if (frame_.primary >= 0) of.push_back(frame_.primary);
	set_camera(frame_, frame_.viewport->framed(frame_.marks, of, width, height), out);
}

OverlayList MissionCanvas::shapes(const ViewportContext &, const CanvasInput &in) const {
	if (!frame_.viewport) return OverlayList();
	const MissionViewport &viewport = *frame_.viewport;
	MissionOverlayInput overlay;
	overlay.scene = &viewport.scene();
	overlay.options = &viewport.options();
	overlay.camera = &viewport.camera();
	overlay.width = in.width;
	overlay.height = in.height;
	overlay.marks = &frame_.marks;
	// Nothing is under the pointer while it looks or drags.
	overlay.hover = looking_ || gesture_.dragging() ? -1 : mission_canvas_under(frame_, in);
	overlay.primary = frame_.primary;
	overlay.selected = &frame_.selected;
	std::vector<NodeId> rows;
	for (const NodeAddress &record : frame_.records) rows.push_back(record.row);
	overlay.selected_rows = &rows;
	overlay.handle_reach = frame_.editable && frame_.current ? viewport.handle_reach() : 0.0f;
	overlay.device = frame_.device;
	if (const Document *document = frame_.document)
		overlay.title = [document](const NodeAddress &record) { return document->record_title(record); };
	overlay.marquee = grab_.what == MissionGrab::What::Marquee && gesture_.dragging();
	overlay.marquee_from = grab_.from;
	overlay.marquee_to = grab_.to;
	return mission_overlay_shapes(overlay);
}

CanvasCursor MissionCanvas::cursor(const ViewportContext &, const CanvasInput &) const {
	return grab_.what == MissionGrab::What::Handle && gesture_.dragging() ? CanvasCursor::Move : CanvasCursor::Default;
}

std::string MissionCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	// The label beside the hovered mark says it (mission_overlay): no tip beside it.
	(void)in;
	return std::string();
}

} // namespace opennova::editor
