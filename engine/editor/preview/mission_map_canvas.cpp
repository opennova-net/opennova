#include <editor/preview/mission_map_canvas.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/model/document.h>
#include <editor/preview/mission_place.h>
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
	links_ = MissionLinkScene();
	badges_.clear();
	pick_ = MissionPickFrame();
	const Document *document = document_of(context);
	const bool current = document && viewport.current(context.input);
	// S24 pick mode: the slot the workspace's pick names, each pin's value for it; held (shown, no pin filling it)
	// while the context takes no edits.
	if (const MissionDocument *picked = dynamic_cast<const MissionDocument *>(document); picked && current) {
		const auto shown = context.input.view.workspace.documents.find(document->path());
		if (shown != context.input.view.workspace.documents.end() && shown->second.pick.active()) {
			std::vector<NodeAddress> records;
			records.reserve(marks_.size());
			for (const MissionMapMark &mark : marks_) records.push_back(mark.record);
			pick_ = mission_pick_frame(*picked, shown->second.pick.record, shown->second.pick.slot, records);
			if (!context.editable()) pick_.hold(context.not_editable());
		}
	}
	// S24: how many events name each mark.
	const MissionDocument *mission = dynamic_cast<const MissionDocument *>(document);
	if (mission && current && viewport_->options().links) {
		counts_.follow(*mission);
		if (counts_.named()) {
			badges_.resize(marks_.size());
			for (size_t i = 0; i < marks_.size(); ++i) badges_[i] = counts_.events(marks_[i].record.row);
		}
	}
	const Selection *selection = current ? selection_of(context, *document) : nullptr;
	if (!selection) return;
	if (mission && viewport_->options().links && mission_logic_record(selection->primary))
		links_ = mission_link_scene(*mission, viewport_->scene(), selection->primary);
	for (const NodeAddress &record : selection->records) {
		const int index = viewport_->scene().mark_index(record.row);
		if (index < 0 || size_t(index) >= marks_.size()) continue;
		if (record == selection->primary) primary_ = index;
		else selected_.push_back(index);
	}
}

MissionHandle MissionMapCanvas::handle_under_(const ViewportContext &context, const MissionMapView &view, float x, float y,
		MissionPressed &area) const {
	if (!viewport_ || primary_ < 0 || size_t(primary_) >= marks_.size() || marks_[size_t(primary_)].area < 0 ||
			!context.editable())
		return MissionHandle::Move;
	if (!viewport_->pressed(marks_[size_t(primary_)].record, area)) return MissionHandle::Move;
	for (const MissionHandle handle : kMissionMapAreaHandles) {
		double hx = 0.0, hy = 0.0;
		float px = 0.0f, py = 0.0f;
		if (!mission_map_handle_point(area, handle, hx, hy)) continue;
		view.project(hx, hy, px, py);
		if (std::fabs(px - x) <= kMissionPickSlop && std::fabs(py - y) <= kMissionPickSlop) return handle;
	}
	return MissionHandle::Move;
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
	// S24: Esc while a pick waits ends it.
	if (pick_.active() && in.keyboard.focused && in.keyboard.escape && !gesture_.pressed()) {
		mission_pick_end(path, out);
		return;
	}
	// Esc while a press is down cancels it, as in the 3D view: what its release would raise (a click's selection, a
	// box's) is not raised, and what a drag wrote (pins moved, an area's edge or corner) goes back where the press
	// found it, under its gesture (its one undo step then changes nothing).
	if (gesture_.pressed() && in.keyboard.focused && in.keyboard.escape) {
		if (gesture_.dragging() && document && !pick_.active()) {
			std::vector<MissionPressed> pressed = press_.taken;
			if (press_.handle != MissionHandle::Move) pressed.assign(1, press_.area);
			std::vector<Edit> edits;
			mission_restore_edits(*document, pressed, gesture_.token(), edits);
			if (!edits.empty()) {
				out.request(request::edit_record(path, std::move(edits)));
				gesture_.sent();
			}
		}
		gesture_.end(out);
		press_ = Press();
		return;
	}
	// A nudge lasts while an arrow is held on the canvas that has the keyboard.
	if (gesture_.nudging() && (!in.keyboard.arrow_held || !in.keyboard.focused)) {
		gesture_.end(out);
		nudge_.clear();
	}
	keys_(context, in, out);

	if (in.pressed && !in.middle && !in.panning && in.hovered) {
		gesture_.press(subject, in.screen, out);
		press_ = Press();
		press_.box_from = in.mouse;
		view.unproject(in.mouse.x, in.mouse.y, press_.from[0], press_.from[1]);
		// S24: the primary area's edge or corner first (its own handles stand over its pin and its box); none while a
		// pick waits.
		if (current && !pick_.active()) press_.handle = handle_under_(context, view, in.mouse.x, in.mouse.y, press_.area);
		if (press_.handle != MissionHandle::Move)
			mission_map_handle_point(press_.area, press_.handle, press_.handle_at[0], press_.handle_at[1]);
		else press_.mark = pick_mission_map_mark(marks_, view, in.mouse.x, in.mouse.y);
		if (press_.mark >= 0 && current && !pick_.active()) {
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
		if (pick_.active()) {
			// S24 pick mode: a click on a pin that fills the slot sets it and ends the pick; on nothing, ends it.
			if (current && !gesture_.dragging()) {
				if (pick_.fills_mark(press_.mark)) mission_pick_requests(path, pick_, press_.mark, out);
				else if (press_.mark < 0) mission_pick_end(path, out);
			}
		} else if (press_.handle != MissionHandle::Move) {
			// A tap or a drag of the primary area's handle: the selection stands.
		} else if (current && !gesture_.dragging()) {
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
	if (pick_.active()) return; // a pick drags nothing
	if (gesture_.dragging() && press_.handle != MissionHandle::Move && current && context.editable()) {
		// S24: the area's edge or corner as far as the pointer went, the area alone.
		double at[2];
		view.unproject(in.mouse.x, in.mouse.y, at[0], at[1]);
		const double to[3] = { press_.handle_at[0] + (at[0] - press_.from[0]), press_.handle_at[1] + (at[1] - press_.from[1]),
			0.0 };
		std::vector<Edit> edits;
		mission_area_handle_edits(*document, press_.area, press_.handle, to, in.keys.ctrl ? 0.0f : viewport.options().snap,
				gesture_.token(), edits);
		if (!edits.empty()) {
			out.request(request::edit_record(path, std::move(edits)));
			gesture_.sent();
		}
		return;
	}
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

void MissionMapCanvas::keys_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	const CanvasKeyboard &keyboard = in.keyboard;
	if (!viewport_ || !keyboard.focused || gesture_.pressed() || pick_.active()) return;
	const Document *document = document_of(context);
	const MissionDocument *mission = dynamic_cast<const MissionDocument *>(document);
	if (!mission || !viewport_->current(context.input) || !context.editable()) return;
	const Selection *selection = selection_of(context, *document);
	if (!selection || selection->records.empty()) return;
	const std::string &path = viewport_->path();
	const float snap = viewport_->options().snap > 0.0f ? viewport_->options().snap : 1.0f;
	if (keyboard.remove) {
		std::vector<Edit> edits;
		for (const NodeAddress &record : mission_view_removals(*mission, selection->records)) {
			Edit edit;
			edit.operation = EditOperation::Remove;
			edit.address = record;
			edits.push_back(std::move(edit));
		}
		out.request(request::edit_record(path, std::move(edits)));
		return;
	}
	if (keyboard.duplicate) {
		ViewportCommand command;
		command.name = "duplicate";
		command.kind = ViewportKind::Mission;
		command.by = { double(snap), 0.0 };
		out.request(request::edit_in_viewport(path, std::move(command)));
		return;
	}
	if (!keyboard.arrow_x && !keyboard.arrow_y) return;
	// A step on the map's axes as the picture shows them: right east, up north (a RotateMap180 mission's the other
	// way), Shift a tenth. arrow_y is +1 for Down, which is south on the picture.
	const double step = double(in.keys.shift ? snap * 0.1f : snap);
	const double turn = viewport_->ground().flip_180 ? -1.0 : 1.0;
	const double east = turn * double(keyboard.arrow_x) * step, north = -turn * double(keyboard.arrow_y) * step;
	if (!gesture_.nudging()) {
		nudge_.clear();
		MissionPressed held;
		if (viewport_->pressed(selection->primary, held)) nudge_.push_back(held);
		for (const NodeAddress &record : selection->records)
			if (record != selection->primary && viewport_->pressed(record, held)) nudge_.push_back(held);
		if (nudge_.empty()) return;
		CanvasSubject subject;
		subject.path = path;
		subject.identity = document->identity();
		gesture_.nudge(subject, out);
		nudged_ = selection->records;
		nudge_by_[0] = nudge_by_[1] = 0.0;
	}
	nudge_by_[0] += east;
	nudge_by_[1] += north;
	const double to[2] = { nudge_.front().x + nudge_by_[0], nudge_.front().y + nudge_by_[1] };
	std::vector<Edit> edits;
	if (mission_move_edits(*document, nudge_, 0, to, 0.0f, viewport_->options().stick, context.device, gesture_.token(),
				edits) &&
			!edits.empty()) {
		out.request(request::edit_record(path, std::move(edits)));
		gesture_.sent();
	}
}

void MissionMapCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	press_ = Press();
	panning_ = false;
	nudge_.clear();
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
	// The paths first, under the pins: a line through each one's stops (a looping path of two stops or more closed, as
	// the 3D view closes it), the player's route numbered from 0, each stop by its place as the 3D view and an event's
	// stop name it.
	if (options.paths) {
		const int route = mission_map_route(scene);
		for (size_t p = 0; p < scene.paths().size(); ++p) {
			const MissionPathMark &path = scene.paths()[p];
			std::vector<CanvasPoint> stops;
			std::vector<size_t> places;
			for (size_t s = 0; s < path.stops.size(); ++s)
				if (const MissionEntityMark *marker = path.stops[s] ? scene.entity(path.stops[s]) : nullptr) {
					stops.push_back(at(marker->x, marker->y));
					places.push_back(s);
				}
			if (stops.size() < 1) continue;
			const uint32_t rgb = (path.flags & uint32_t(bms::WaypointFlags::PlayerRoute)) ? kMissionBlueRgb
					: (path.flags & uint32_t(bms::WaypointFlags::RedTeam))             ? kMissionRedRgb
					                                                                    : kMissionPathRgb;
			const float thickness = int(p) == route ? 2.0f : 1.0f;
			for (size_t i = 0; i + 1 < stops.size(); ++i) list.line(stops[i], stops[i + 1], rgb, thickness);
			if (stops.size() > 1 && !(path.flags & uint32_t(bms::WaypointFlags::DoesNotLoop)))
				list.line(stops.back(), stops.front(), rgb, thickness);
			if (int(p) == route)
				for (size_t i = 0; i < stops.size(); ++i)
					list.text(CanvasPoint{ stops[i].x + 5.0f, stops[i].y - 7.0f }, std::to_string(places[i]), kMissionBlueRgb);
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
	// S24: the selected logic record's links, over the boxes and under the pins.
	if (options.links && links_.any()) {
		MissionLinkView links;
		links.point = [&](const std::array<double, 3> &point, CanvasPoint &out) {
			out = at(point[0], point[1]);
			return true;
		};
		links.segment = [&](const std::array<double, 3> &a, const std::array<double, 3> &b, CanvasPoint &from,
		                    CanvasPoint &to) {
			from = at(a[0], a[1]);
			to = at(b[0], b[1]);
			return true;
		};
		mission_link_shapes(links_, links, list);
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
		// Its team's ring, or (S24) its group's.
		const int group = mark.entity >= 0 ? scene.entities()[size_t(mark.entity)].group : 0;
		uint32_t ring = 0;
		if (mission_mark_ring_rgb(options.color_by, mark.team, group, ring))
			list.ring(point, 6.0f, OverlayRole::Normal, options.color_by == MissionColorBy::Group ? 1.75f : 1.0f, ring);
	}
	// S24: the groups' legend, coloured by group: each group a shown mark carries, by the mission's words for it.
	if (options.color_by == MissionColorBy::Group) {
		std::vector<size_t> grouped(64, 0);
		for (const MissionMapMark &mark : marks_)
			if (mark.shown && mark.entity >= 0) {
				const int group = scene.entities()[size_t(mark.entity)].group;
				if (group > 0 && group < 64) ++grouped[size_t(group)];
			}
		std::vector<std::pair<int, size_t>> groups;
		for (int group = 1; group < 64; ++group)
			if (grouped[size_t(group)]) groups.emplace_back(group, grouped[size_t(group)]);
		const MissionDocument *mission = dynamic_cast<const MissionDocument *>(document_of(context));
		const AssetGraph *graph = context.input.view.findings.graph.get();
		std::optional<GraphNameSource> source;
		if (graph) source.emplace(*graph);
		const std::unique_ptr<MissionNames> names = mission ? mission_label_names(*mission, source ? &*source : nullptr) : nullptr;
		mission_group_legend(groups, [&names](int group) {
			std::string words = names ? names->group(group) : "group " + std::to_string(group);
			if (!words.empty() && words[0] >= 'a' && words[0] <= 'z') words[0] = char(words[0] - 'a' + 'A');
			return words;
		}, in.width, list);
	}
	// S24: how many events name each shown mark.
	if (options.links)
		for (size_t i = 0; i < marks_.size() && i < badges_.size(); ++i)
			if (marks_[i].shown) mission_link_badge(CanvasPoint{ marks_[i].px, marks_[i].py }, badges_[i], list);
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
	// S24: the primary area's edge and corner handles.
	MissionPressed area;
	if (primary_ >= 0 && size_t(primary_) < marks_.size() && marks_[size_t(primary_)].area >= 0 && context.editable() &&
			viewport.pressed(marks_[size_t(primary_)].record, area))
		for (const MissionHandle handle : kMissionMapAreaHandles) {
			double hx = 0.0, hy = 0.0;
			if (mission_map_handle_point(area, handle, hx, hy))
				list.marker(at(hx, hy), OverlayGlyph::Square, mission_handle_is_corner(handle) ? 3.5f : 4.0f, OverlayRole::Selected);
		}
	int hovered = in.hovered && !gesture_.dragging() ? pick_mission_map_mark(marks_, view, in.mouse.x, in.mouse.y) : -1;
	// S24 pick mode: each candidate rung; only a candidate hovers.
	if (pick_.active()) {
		for (size_t i = 0; i < marks_.size(); ++i)
			if (marks_[i].shown && pick_.fills_mark(int(i))) mission_pick_candidate(CanvasPoint{ marks_[i].px, marks_[i].py }, list);
		if (!pick_.fills_mark(hovered)) hovered = -1;
	}
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

CanvasCursor MissionMapCanvas::cursor(const ViewportContext &context, const CanvasInput &in) const {
	if (!viewport_ || in.panning || panning_) return CanvasCursor::Default;
	// S24: an area's edge or corner: a resize along its axis or its diagonal.
	const auto resize = [](MissionHandle handle) {
		switch (handle) {
		case MissionHandle::XMin:
		case MissionHandle::XMax: return CanvasCursor::ResizeEW;
		case MissionHandle::YMin:
		case MissionHandle::YMax: return CanvasCursor::ResizeNS;
		case MissionHandle::XMinYMax:
		case MissionHandle::XMaxYMin: return CanvasCursor::ResizeNWSE;
		default: return CanvasCursor::ResizeNESW;
		}
	};
	if (gesture_.pressed() && press_.handle != MissionHandle::Move) return resize(press_.handle);
	if (gesture_.pressed()) return press_.mark >= 0 && gesture_.dragging() ? CanvasCursor::Move : CanvasCursor::Default;
	const MissionMapView view = viewport_->view(in.width, in.height);
	MissionPressed area;
	if (in.hovered) {
		const MissionHandle handle = handle_under_(context, view, in.mouse.x, in.mouse.y, area);
		if (handle != MissionHandle::Move) return resize(handle);
	}
	return in.hovered && pick_mission_map_mark(marks_, view, in.mouse.x, in.mouse.y) >= 0 ? CanvasCursor::Move
	                                                                                        : CanvasCursor::Default;
}

std::string MissionMapCanvas::hover_tip(const ViewportContext &context, const CanvasInput &in) const {
	if (!viewport_ || gesture_.dragging() || !in.hovered) return std::string();
	const int index = pick_mission_map_mark(marks_, viewport_->view(in.width, in.height), in.mouse.x, in.mouse.y);
	// S24: while a pick waits, what to click (over a pin that names another record, why it gives nothing).
	if (pick_.active()) return mission_pick_hint(pick_, index);
	if (index < 0) return std::string();
	const MissionMapMark &mark = marks_[size_t(index)];
	char at[64];
	std::snprintf(at, sizeof(at), " at %.1f east, %.1f north", mark.x, mark.y);
	std::string tip = viewport_->title(context, mark.record) + at +
	                  "\nClick to select it (Shift adds, Ctrl toggles); drag to move it. Right-drag pans, the wheel zooms.";
	// S24: what the selected logic's reach cannot draw about it (the player's place unknown).
	const std::string noted = links_.note(mark.record.row);
	if (!noted.empty()) tip += "\n\n" + noted;
	// S24: the events that name it, its badge's words.
	const MissionDocument *mission = dynamic_cast<const MissionDocument *>(document_of(context));
	if (mission && size_t(index) < badges_.size() && badges_[size_t(index)] > 0) {
		const AssetGraph *graph = context.input.view.findings.graph.get();
		// The names read the graph's source, which outlives them (made only when the words are made again).
		std::optional<GraphNameSource> source;
		const auto names = [&]() {
			if (graph && !source) source.emplace(*graph);
			return mission_label_names(*mission, source ? &*source : nullptr);
		};
		const std::string &named = hover_.words(*mission, counts_, mark.record.row, names, graph != nullptr,
				graph ? graph->generation() : 0);
		if (!named.empty()) tip += "\n\n" + named;
	}
	return tip;
}

} // namespace opennova::editor
