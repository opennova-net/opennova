#include <editor/preview/mission_canvas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/bam.h>
#include <optional>

#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/model/document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

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

double snapped(double value, float snap) {
	return snap > 0.0f ? std::round(value / double(snap)) * double(snap) : value;
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

// The handles the primary carries, nearest the pointer first in this order.
constexpr MissionHandle kPrimaryHandles[] = { MissionHandle::Yaw, MissionHandle::Height, MissionHandle::XMin,
	MissionHandle::XMax, MissionHandle::YMin, MissionHandle::YMax };

// A number of metres as the readout says it.
std::string metres(double value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f m", value);
	return text;
}

} // namespace

int mission_canvas_under(const MissionCanvasFrame &frame, const CanvasInput &in, MissionPick by) {
	if (!in.hovered || !frame.viewport) return -1;
	return pick_mission_mark(frame.marks, frame.viewport->camera(), in.width, in.height, in.mouse.x, in.mouse.y,
			frame.device, by);
}

bool mission_canvas_handle_under(const MissionCanvasFrame &frame, const CanvasInput &in, MissionHandle &out) {
	if (!in.hovered || !frame.viewport || frame.primary < 0 || !frame.current || !frame.editable || !frame.document ||
			frame.document->blocked())
		return false;
	const MissionViewport &viewport = *frame.viewport;
	const MissionMark &primary = frame.marks[size_t(frame.primary)];
	for (const MissionHandle handle : kPrimaryHandles) {
		PreviewVec3 at;
		CanvasPoint offset;
		if (!viewport.handle_at(primary, handle, at) || !near_pointer(viewport.camera(), in, at, offset)) continue;
		out = handle;
		return true;
	}
	return false;
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
	// Alt on a mark copies it as it drags; Alt on nothing orbits. Shift joins the selection (a click adds,
	// nothing a drag moves). Ctrl at the press still takes the mark or the handle: a drag goes free of the
	// snap, a click toggles the mark in the selection (release_).
	const bool edits = frame.current && frame.editable && !frame.document->blocked() && !in.keys.shift;
	// The primary's own handles first: its height and its yaw, an area's edges.
	if (edits && !in.keys.alt && frame.primary >= 0) {
		const MissionMark &primary = frame.marks[size_t(frame.primary)];
		for (const MissionHandle handle : kPrimaryHandles) {
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
				// A turn carries the selected areas round the group's centre; a lift leaves them.
				grab.pressed = selected_pressed(frame, handle == MissionHandle::Yaw, frame.primary, grab.grabbed);
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
		// first. With Alt, copies of them do, placed where the drag is let go.
		const MissionMark &mark = frame.marks[size_t(under)];
		grab.what = MissionGrab::What::Handle;
		grab.handle = MissionHandle::Move;
		grab.copy = in.keys.alt;
		grab.through = mark.at;
		// Taken by its surface with its glyph off the picture (a building's origin behind the eye): the drag
		// goes over the ground from the pointer, not from a glyph's pixel.
		const bool glyph = mark.shown;
		grab.offset = glyph ? CanvasPoint{ mark.x - in.mouse.x, mark.y - in.mouse.y } : CanvasPoint{};
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
		grab.grounded = mission_ground_point(context, camera, glyph ? mark.x : in.mouse.x, glyph ? mark.y : in.mouse.y, anchor[2],
				grab.ground);
		return grab;
	}
	grab.what = in.keys.alt ? MissionGrab::What::Orbit : MissionGrab::What::Marquee;
	return grab;
}

// --- MissionCanvas -------------------------------------------------------------------------------

void MissionCanvas::follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) {
	const auto &mission = static_cast<const MissionViewport &>(viewport);
	frame_ = mission.canvas_frame(context);
	if (turn_ >= 0.0f) frame_.turn_snap = turn_;
	// The tool, as the viewport's options say it; the placed item's name, as the catalogs define it.
	const MissionViewportOptions &options = mission.options();
	tool_ = options.tool;
	path_ = options.path;
	if (item_ != options.item || item_name_.empty()) {
		item_ = options.item;
		item_name_.clear();
		const AssetGraph *graph = context.input.view.findings.graph.get();
		if (const GraphSymbol *symbol = graph && item_ ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(item_)) : nullptr)
			item_name_ = symbol->record;
	}
	gesture_.frame(subject_of(frame_), out);
	// A nudge is of the selection it began on.
	if (gesture_.nudging() && nudged_ != frame_.records) gesture_.end(out);
	if (!gesture_.pressed()) grab_ = MissionGrab();
	// A drag of a handle or a nudge writes positions, heights, headings and an area's bounds alone, which
	// no title reads: the titles stand through its samples (each one a revision).
	titles_.hold((gesture_.dragging() && grab_.what == MissionGrab::What::Handle) || gesture_.nudging());
}

void MissionCanvas::input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (!frame_.viewport) return;
	const MissionViewport &viewport = *frame_.viewport;
	// What a press takes: a glyph, else what the device's ray meets (an entity's surface), never more.
	const int under = in.pressed ? mission_canvas_under(frame_, in, MissionPick::Press) : -1;
	// Esc while a press is down cancels it (S15), the tool kept: what its release would raise (an Area
	// box, an Alt-drag's copies, a marquee's selection, a placement) is not raised, and what a drag wrote
	// goes back where the press found it.
	if (gesture_.pressed() && in.keyboard.focused && in.keyboard.escape) {
		cancel_(out);
		return;
	}
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
		// What a click there takes: the press's, else (no device to say) the sphere about an entity.
		grab_.click = under >= 0 ? under : mission_canvas_under(frame_, in, MissionPick::Click);
		// Under a tool: a press takes no mark and draws no marquee (its click places, its drag draws the
		// Area tool's box).
		if (tool() != MissionTool::Select && grab_.what != MissionGrab::What::Pan && grab_.what != MissionGrab::What::Orbit) {
			const CanvasPoint from = grab_.from;
			grab_ = MissionGrab();
			grab_.from = grab_.to = from;
			if (tool() == MissionTool::Area) grab_.what = MissionGrab::What::Area;
		}
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
			case MissionGrab::What::Marquee:
			case MissionGrab::What::Area: grab_.to = in.mouse; break;
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

float MissionCanvas::step_(const CanvasInput &in) const {
	const float step = frame_.snap > 0.0f ? frame_.snap : 1.0f;
	return in.keys.shift ? step * 0.1f : step;
}

void MissionCanvas::step_right(double &east, double &north) const {
	// The file's axis nearest the camera's right: right of a heading (east, north) is (north, -east).
	const double heading = frame_.viewport ? mission_camera_heading(frame_.viewport->camera()) * io::kRadiansPerDegree : 0.0;
	const double forward_east = std::round(std::sin(heading)), forward_north = std::round(std::cos(heading));
	const bool diagonal = forward_east != 0.0 && forward_north != 0.0;
	const double fe = diagonal ? 0.0 : forward_east, fn = diagonal ? (forward_north > 0.0 ? 1.0 : -1.0) : forward_north;
	east = fn;
	north = -fe;
}

void MissionCanvas::keys_(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	const CanvasKeyboard &keyboard = in.keyboard;
	if (!keyboard.focused || gesture_.pressed() || !frame_.document) return;
	const std::string &path = frame_.document->path();
	// Esc under a tool is the tool's (the view goes back to Select); else it selects nothing.
	if (keyboard.escape && tool() == MissionTool::Select && !frame_.records.empty()) {
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
	if (keyboard.duplicate && frame_.viewport) {
		// A copy of the selection a step to the camera's right: one command the viewport plans.
		double east = 0.0, north = 0.0;
		step_right(east, north);
		const double step = frame_.snap > 0.0f ? double(frame_.snap) : 1.0;
		ViewportCommand command;
		command.name = "duplicate";
		command.kind = ViewportKind::Mission;
		command.by = { east * step, north * step };
		out.request(request::edit_in_viewport(frame_.viewport->path(), std::move(command)));
		return;
	}
	if (!keyboard.arrow_x && !keyboard.arrow_y && !keyboard.page) return;
	// A step along the file's axis nearest the camera's right (the left and right arrows) and its
	// forward (up and down); PgUp and PgDn the height. Shift: a tenth of the step.
	const double step = double(step_(in));
	double right_east = 0.0, right_north = 0.0;
	step_right(right_east, right_north);
	// Forward of a right (east, north) is (-north, east).
	const double east = (double(keyboard.arrow_x) * right_east + double(keyboard.arrow_y) * right_north) * step;
	const double north = (double(keyboard.arrow_x) * right_north - double(keyboard.arrow_y) * right_east) * step;
	nudge_by_(east, north, double(keyboard.page) * step, out);
}

void MissionCanvas::drag_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (grab_.pressed.empty() || !frame_.viewport || !frame_.document) return;
	const MissionViewport &viewport = *frame_.viewport;
	const OrbitCamera &camera = viewport.camera();
	const MissionPressed &held = grab_.pressed[grab_.grabbed];
	// The handle follows the pointer, kept where the press took it.
	const float x = in.mouse.x + grab_.offset.x, y = in.mouse.y + grab_.offset.y;
	// Ctrl held: free of the snap.
	const float snap = in.keys.ctrl ? 0.0f : frame_.snap;
	std::vector<Edit> edits;
	switch (grab_.handle) {
	case MissionHandle::Move: {
		// As far over the ground as the pointer went since the press.
		double ground[3];
		if (!grab_.grounded || !mission_ground_point(context, camera, x, y, grab_.ground[2], ground)) return;
		const double to[2] = { held.x + (ground[0] - grab_.ground[0]), held.y + (ground[1] - grab_.ground[1]) };
		if (grab_.copy) {
			// The copies go as far as a move would take the grabbed record; nothing is written yet.
			grab_.copy_by[0] = snapped(to[0], snap) - held.x;
			grab_.copy_by[1] = snapped(to[1], snap) - held.y;
			return;
		}
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
		// The turn is the pointer's bearing about the pivot, from the handle's as pressed: one entity's
		// own anchor (its handle stands along its heading), a group's centre (the records orbit it, the
		// handle with them, so it stays under the pointer's bearing).
		double pivot[2] = { held.x, held.y };
		const bool group = mission_turn_centre(grab_.pressed, pivot);
		double at[3];
		if (!mission_camera_on_height(camera, x, y, in.width, in.height, held.z, at) || (at[0] == pivot[0] && at[1] == pivot[1]))
			return;
		double handle[3];
		preview_to_mission(grab_.through, handle);
		const double from = group && (handle[0] != pivot[0] || handle[1] != pivot[1])
									? std::atan2(handle[0] - pivot[0], handle[1] - pivot[1]) / io::kRadiansPerDegree
									: double(held.yaw);
		const double heading = std::atan2(at[0] - pivot[0], at[1] - pivot[1]) / io::kRadiansPerDegree;
		mission_yaw_edits(*frame_.document, grab_.pressed, grab_.grabbed, turned(from, heading),
				in.keys.ctrl ? 0.0f : frame_.turn_snap, gesture_.token(), edits, viewport.options().stick, frame_.device);
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
	const MissionTool now = tool();
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
	} else if (grab_.what == MissionGrab::What::Area) {
		// The Area tool: a box dragged on the ground is an area trigger over it (a click makes nothing).
		if (gesture_.dragging() && frame_.current && frame_.editable && frame_.viewport) {
			ViewportDrop drop;
			drop.reference = "area";
			drop.box = true;
			drop.x = grab_.from.x;
			drop.y = grab_.from.y;
			drop.x2 = grab_.to.x;
			drop.y2 = grab_.to.y;
			drop.snap = frame_.snap;
			drop.kind = ViewportKind::Mission;
			out.request(request::edit_in_viewport(frame_.viewport->path(), std::move(drop)));
		}
	} else if (!gesture_.dragging() && grab_.what != MissionGrab::What::Pan && frame_.current &&
			(now == MissionTool::Place || now == MissionTool::Path)) {
		// Placing: one of the item, or the path's next stop, where the click was (the viewport plans it);
		// none picked yet, nothing (the hint says to pick one).
		const int64_t picked = now == MissionTool::Place ? item_ : int64_t(path_);
		if (frame_.editable && frame_.viewport && picked != 0) {
			ViewportDrop drop;
			drop.reference = now == MissionTool::Place ? "item" : "path";
			drop.name = std::to_string(picked);
			drop.x = grab_.from.x;
			drop.y = grab_.from.y;
			drop.snap = frame_.snap;
			drop.kind = ViewportKind::Mission;
			out.request(request::edit_in_viewport(frame_.viewport->path(), std::move(drop)));
		}
	} else if (grab_.what == MissionGrab::What::Handle && grab_.copy && gesture_.dragging()) {
		// An Alt-drag let go: copies of what it took, as far as it went (one command the viewport plans).
		if ((grab_.copy_by[0] != 0.0 || grab_.copy_by[1] != 0.0) && frame_.viewport) {
			ViewportCommand command;
			command.name = "duplicate";
			command.kind = ViewportKind::Mission;
			for (const MissionPressed &each : grab_.pressed) command.ids.push_back(each.record.row);
			command.by = { grab_.copy_by[0], grab_.copy_by[1] };
			out.request(request::edit_in_viewport(frame_.viewport->path(), std::move(command)));
		}
	} else if (!gesture_.dragging() && grab_.what == MissionGrab::What::Handle && grab_.pick < 0) {
		// A tap on the primary's yaw, height or edge handle (no mark under it): nothing changes, the
		// selection and its handles stand.
	} else if (!gesture_.dragging() && grab_.what != MissionGrab::What::Pan && frame_.current) {
		// A click: the mark it takes selected (the press's, else with no device to say the sphere about an
		// entity), joined as the keys say; on nothing, nothing selected.
		if (grab_.click >= 0 && size_t(grab_.click) < frame_.marks.size())
			out.request(request::select_record(path, frame_.marks[size_t(grab_.click)].record, select_mode(grab_.join)));
		else if (grab_.join == CanvasJoin::Replace && !frame_.records.empty())
			out.request(request::select_record(path, NodeAddress()));
	}
	gesture_.release(out);
	grab_ = MissionGrab();
}

void MissionCanvas::cancel_(CanvasRequests &out) {
	// A drag that wrote: its records put back as pressed, under its gesture (its one undo step then
	// changes nothing). An Alt-drag wrote nothing.
	if (grab_.what == MissionGrab::What::Handle && !grab_.copy && gesture_.dragging() && frame_.document) {
		std::vector<Edit> edits;
		mission_restore_edits(*frame_.document, grab_.pressed, gesture_.token(), edits);
		if (!edits.empty()) {
			out.request(request::edit_record(gesture_.path(), std::move(edits)));
			gesture_.sent();
		}
	}
	gesture_.end(out);
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

OverlayList MissionCanvas::shapes(const ViewportContext &context, const CanvasInput &in) const {
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
	const bool busy = looking_ || gesture_.dragging();
	overlay.hover = busy || tool() != MissionTool::Select ? -1 : mission_canvas_under(frame_, in, MissionPick::Click);
	overlay.primary = frame_.primary;
	overlay.selected = &frame_.selected;
	std::vector<NodeId> rows;
	for (const NodeAddress &record : frame_.records) rows.push_back(record.row);
	overlay.selected_rows = &rows;
	overlay.handle_reach = frame_.editable && frame_.current ? viewport.handle_reach() : 0.0f;
	overlay.device = frame_.device;
	// The labels by the project's names (the display names, kept while the document and the graph
	// stand: a label a mark a frame).
	const AssetGraph *graph = context.input.view.findings.graph.get();
	std::optional<GraphNameSource> names;
	if (graph) names.emplace(*graph);
	if (const Document *document = frame_.document)
		overlay.title = [this, document, &names](const NodeAddress &record) {
			return titles_.record(*document, record, names ? &*names : nullptr);
		};
	overlay.layout = &labels_;
	overlay.marquee = grab_.what == MissionGrab::What::Marquee && gesture_.dragging();
	overlay.marquee_from = grab_.from;
	overlay.marquee_to = grab_.to;
	overlay.pointer = in.mouse;
	// The handle the pointer is on (its words and step beside it), or the one a drag holds.
	MissionHandle handle = MissionHandle::Move;
	if (gesture_.dragging() && grab_.what == MissionGrab::What::Handle && !grab_.copy) {
		overlay.active_handle = true;
		overlay.handle = grab_.handle;
	} else if (!busy && tool() == MissionTool::Select && mission_canvas_handle_under(frame_, in, handle)) {
		overlay.active_handle = true;
		overlay.handle = handle;
	}
	overlay.snap = in.keys.ctrl ? 0.0f : frame_.snap;
	overlay.turn = in.keys.ctrl ? 0.0f : frame_.turn_snap;
	overlay.dragging = gesture_.dragging() && grab_.what == MissionGrab::What::Handle && !grab_.copy;
	// A group turn's pivot, while it goes.
	if (gesture_.dragging() && grab_.what == MissionGrab::What::Handle && grab_.handle == MissionHandle::Yaw &&
			!grab_.pressed.empty()) {
		double pivot[2] = { 0.0, 0.0 };
		if (mission_turn_centre(grab_.pressed, pivot)) {
			overlay.pivot = true;
			overlay.pivot_at[0] = pivot[0];
			overlay.pivot_at[1] = pivot[1];
			overlay.pivot_at[2] = grab_.pressed[grab_.grabbed].z;
		}
	}
	// An Alt-drag's copies where they go.
	if (gesture_.dragging() && grab_.what == MissionGrab::What::Handle && grab_.copy) {
		overlay.copies = &grab_.pressed;
		overlay.copy_by[0] = grab_.copy_by[0];
		overlay.copy_by[1] = grab_.copy_by[1];
	}
	// The Area tool's box on the ground.
	if (grab_.what == MissionGrab::What::Area && gesture_.dragging()) {
		double target[3];
		preview_to_mission(viewport.camera().target, target);
		if (mission_ground_point(context, viewport.camera(), grab_.from.x, grab_.from.y, target[2], overlay.box_from) &&
				mission_ground_point(context, viewport.camera(), grab_.to.x, grab_.to.y, target[2], overlay.box_to))
			overlay.box = true;
		overlay.box_snap = frame_.snap;
	}
	return mission_overlay_shapes(overlay);
}

CanvasCursor MissionCanvas::cursor(const ViewportContext &, const CanvasInput &in) const {
	if (grab_.what == MissionGrab::What::Handle && gesture_.dragging()) return CanvasCursor::Move;
	MissionHandle handle = MissionHandle::Move;
	if (!looking_ && !gesture_.pressed() && tool() == MissionTool::Select && mission_canvas_handle_under(frame_, in, handle))
		return CanvasCursor::Move;
	return CanvasCursor::Default;
}

std::string MissionCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	// The label beside the hovered mark says it (mission_overlay): no tip beside it.
	(void)in;
	return std::string();
}

std::string MissionCanvas::hint(const ViewportContext &context, const CanvasInput &in) const {
	MissionHintInput hint;
	hint.tool = tool();
	hint.item = item_name_.empty() && item_ ? "item " + std::to_string(item_) : item_name_;
	hint.path = path_;
	hint.editable = frame_.editable;
	hint.not_editable = frame_.not_editable;
	hint.current = frame_.current || !frame_.document;
	hint.snap = in.keys.ctrl ? 0.0f : frame_.snap;
	hint.grid = frame_.snap;
	hint.turn = in.keys.ctrl ? 0.0f : frame_.turn_snap;
	hint.dragging = gesture_.dragging() && (grab_.what == MissionGrab::What::Handle || grab_.what == MissionGrab::What::Marquee ||
													grab_.what == MissionGrab::What::Area);
	hint.handle = grab_.what == MissionGrab::What::Handle;
	hint.which = grab_.handle;
	hint.copying = grab_.copy;
	if (!gesture_.pressed() && !looking_) {
		MissionHandle handle = MissionHandle::Move;
		if (tool() == MissionTool::Select && mission_canvas_handle_under(frame_, in, handle)) {
			hint.handle = true;
			hint.which = handle;
		} else {
			const int under = mission_canvas_under(frame_, in, MissionPick::Click);
			if (under >= 0 && frame_.document) {
				const MissionMark &mark = frame_.marks[size_t(under)];
				const AssetGraph *graph = context.input.view.findings.graph.get();
				std::optional<GraphNameSource> names;
				if (graph) names.emplace(*graph);
				hint.hovered = titles_.record(*frame_.document, mark.record, names ? &*names : nullptr);
				hint.hovered_area = mark.area >= 0;
				hint.hovered_selected = is_selected(frame_, under);
				hint.hovered_drags = mission_canvas_under(frame_, in, MissionPick::Press) == under;
			}
		}
	}
	hint.selected = frame_.records.size();
	if (frame_.viewport)
		hint.empty_mission = frame_.viewport->scene().entities().empty() && frame_.viewport->scene().areas().empty();
	// While a drag writes, what it has come to.
	std::string line = mission_canvas_hint(hint);
	if (gesture_.dragging() && grab_.what == MissionGrab::What::Handle && !grab_.pressed.empty() && frame_.viewport) {
		const MissionPressed &held = grab_.pressed[grab_.grabbed];
		std::string now;
		if (grab_.copy) {
			now = "copies go " + metres(grab_.copy_by[0]) + " east, " + metres(grab_.copy_by[1]) + " north";
		} else if (const MissionEntityMark *entity = frame_.viewport->scene().entity(held.record.row)) {
			char text[96];
			if (grab_.handle == MissionHandle::Yaw)
				std::snprintf(text, sizeof(text), "heading %d deg", entity->yaw);
			else if (grab_.handle == MissionHandle::Height)
				std::snprintf(text, sizeof(text), "height %.2f m (%+.2f)", entity->z, entity->z - held.z);
			else
				std::snprintf(text, sizeof(text), "at %.2f m east, %.2f m north (moved %+.2f, %+.2f)", entity->x, entity->y,
						entity->x - held.x, entity->y - held.y);
			now = text;
		}
		if (!now.empty()) line = now + ". " + line;
	}
	return line;
}

} // namespace opennova::editor
