#include <editor/preview/mission_overlay.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_hint.h>
#include <editor/preview/mission_labels.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/viewport_device.h>
#include <formats/mission/bms.h>
#include <runtime/world/presentation_frame.h>

namespace opennova::editor {

namespace {

// The dark ring under a selected or hovered mark's (it reads on bright ground and among glyphs).
constexpr uint32_t kMissionShadowRgb = 0x000000;
constexpr uint8_t kMissionShadowAlpha = 170;

std::string capitalized(std::string text) {
	if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = char(text[0] - 'a' + 'A');
	return text;
}

double snapped(double value, float snap) {
	return snap > 0.0f ? std::round(value / double(snap)) * double(snap) : value;
}

PreviewVec3 along(const PreviewVec3 &from, const PreviewVec3 &direction, float t) {
	return PreviewVec3{ from.x + direction.x * t, from.y + direction.y * t, from.z + direction.z * t };
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

} // namespace

uint32_t mission_team_rgb(int team) {
	// NEEDS-RE: the original editor's team colours are not witnessed; team 1 blue and team 2 red is
	// the game's HUD convention assumed here, and which team number is which side is to be
	// confirmed against the editor's own ring colours (ADR 0046 S14, the view design's open note).
	switch (team) {
	case 1: return kMissionBlueRgb;
	case 2: return kMissionRedRgb;
	default: return kMissionPathRgb;
	}
}

PreviewVec3 mission_entity_heading(const MissionEntityMark &entity) {
	float forward[3];
	world::presentation_forward_from_angles(float(entity.yaw), 0.0f, forward);
	return PreviewVec3{ forward[0], forward[1], forward[2] };
}

PreviewVec3 mission_yaw_handle(const MissionEntityMark &entity, float reach) {
	return along(entity.at, mission_entity_heading(entity), reach);
}

PreviewVec3 mission_height_handle(const MissionEntityMark &entity, float reach) {
	return PreviewVec3{ entity.at.x, entity.at.y + reach, entity.at.z };
}

PreviewVec3 mission_area_edge_middle(const MissionAreaMark &area, MissionHandle edge, double z) {
	const double mx = (area.min[0] + area.max[0]) * 0.5, my = (area.min[1] + area.max[1]) * 0.5;
	switch (edge) {
	case MissionHandle::XMin: return mission_scene_point(area.min[0], my, z);
	case MissionHandle::XMax: return mission_scene_point(area.max[0], my, z);
	case MissionHandle::YMin: return mission_scene_point(mx, area.min[1], z);
	case MissionHandle::YMax: return mission_scene_point(mx, area.max[1], z);
	default: return mission_scene_point(mx, my, z);
	}
}

double mission_area_anchor_z(const MissionAreaMark &area, const ViewportDevice *device) {
	double ground = 0.0;
	if (device && device->ground_at((area.min[0] + area.max[0]) * 0.5, (area.min[1] + area.max[1]) * 0.5, ground))
		return ground;
	return area.min[2];
}

bool mission_project_segment(const OrbitCamera &camera, int width, int height, const PreviewVec3 &a,
		const PreviewVec3 &b, CanvasPoint &from, CanvasPoint &to) {
	float depth_a = 0.0f, depth_b = 0.0f;
	const bool a_in = camera.project(a, width, height, from.x, from.y, &depth_a);
	const bool b_in = camera.project(b, width, height, to.x, to.y, &depth_b);
	if (a_in && b_in) return true;
	if (!a_in && !b_in) return false;
	// The end behind the near plane moved onto it along the segment (a hair in front, so it projects).
	const float plane = camera.near_plane * 1.001f;
	const float t = (plane - depth_a) / (depth_b - depth_a);
	const PreviewVec3 clipped = along(a, PreviewVec3{ b.x - a.x, b.y - a.y, b.z - a.z }, t);
	if (a_in) return camera.project(clipped, width, height, to.x, to.y);
	return camera.project(clipped, width, height, from.x, from.y);
}

OverlayList mission_overlay_shapes(const MissionOverlayInput &in) {
	OverlayList list;
	if (!in.scene || !in.options || !in.camera || !in.marks) return list;
	const OrbitCamera &camera = *in.camera;
	const MissionScene &scene = *in.scene;
	const std::vector<MissionMark> &marks = *in.marks;
	// Each mark's selection and each selected row, looked up once (a selection of thousands is drawn
	// every frame).
	std::vector<char> selected_marks(marks.size(), 0);
	if (in.primary >= 0 && size_t(in.primary) < marks.size()) selected_marks[size_t(in.primary)] = 1;
	if (in.selected)
		for (const int mark : *in.selected)
			if (mark >= 0 && size_t(mark) < marks.size()) selected_marks[size_t(mark)] = 1;
	const auto is_selected = [&](int mark) { return mark >= 0 && size_t(mark) < marks.size() && selected_marks[size_t(mark)]; };
	std::unordered_set<NodeId> selected_rows;
	if (in.selected_rows) selected_rows.insert(in.selected_rows->begin(), in.selected_rows->end());
	const auto selected_row = [&](NodeId row) { return selected_rows.count(row) != 0; };
	const auto line = [&](const PreviewVec3 &a, const PreviewVec3 &b, uint32_t rgb, float thickness) {
		CanvasPoint from, to;
		if (mission_project_segment(camera, in.width, in.height, a, b, from, to)) list.line(from, to, rgb, thickness);
	};

	// The paths first, under the marks: a line through each one's stops that name a marker.
	if (in.options->paths) {
		for (const MissionPathMark &path : scene.paths()) {
			std::vector<PreviewVec3> stops;
			bool selected = selected_row(path.row);
			for (const NodeId stop : path.stops) {
				const MissionEntityMark *marker = stop ? scene.entity(stop) : nullptr;
				if (!marker) continue;
				stops.push_back(marker->at);
				selected = selected || selected_row(marker->row);
			}
			if (stops.size() < 2) continue;
			const uint32_t rgb = (path.flags & uint32_t(bms::WaypointFlags::BlueTeam)) ? kMissionBlueRgb
					: (path.flags & uint32_t(bms::WaypointFlags::RedTeam))             ? kMissionRedRgb
																						 : kMissionPathRgb;
			const float thickness = selected ? 2.5f : 1.0f;
			for (size_t i = 0; i + 1 < stops.size(); ++i) line(stops[i], stops[i + 1], rgb, thickness);
			if (!(path.flags & uint32_t(bms::WaypointFlags::DoesNotLoop))) line(stops.back(), stops.front(), rgb, thickness);
		}
	}

	// The nearest kMissionMarksDrawn shown marks.
	std::vector<int> drawn;
	for (size_t i = 0; i < marks.size(); ++i)
		if (marks[i].shown) drawn.push_back(int(i));
	if (drawn.size() > kMissionMarksDrawn) {
		std::nth_element(drawn.begin(), drawn.begin() + std::ptrdiff_t(kMissionMarksDrawn), drawn.end(),
				[&](int a, int b) { return marks[size_t(a)].depth < marks[size_t(b)].depth; });
		drawn.resize(kMissionMarksDrawn);
		std::sort(drawn.begin(), drawn.end());
	}
	// The labels asked for, placed once every mark is drawn (over them).
	std::vector<MissionLabelCandidate> labels;
	std::vector<int> label_marks;
	for (const int index : drawn) {
		const MissionMark &mark = marks[size_t(index)];
		const CanvasPoint at{ mark.x, mark.y };
		if (mark.area >= 0) {
			// An area: its footprint at its anchor height, its top and bottom where its flags bound z.
			const MissionAreaMark &area = scene.areas()[size_t(mark.area)];
			const double z = mission_area_anchor_z(area, in.device);
			const PreviewVec3 corners[4] = { mission_scene_point(area.min[0], area.min[1], z),
				mission_scene_point(area.max[0], area.min[1], z), mission_scene_point(area.max[0], area.max[1], z),
				mission_scene_point(area.min[0], area.max[1], z) };
			for (int c = 0; c < 4; ++c) line(corners[c], corners[(c + 1) % 4], kMissionAreaRgb, 1.0f);
			if (area.constrains_z) {
				for (const double level : { area.min[2], area.max[2] }) {
					const PreviewVec3 ring[4] = { mission_scene_point(area.min[0], area.min[1], level),
						mission_scene_point(area.max[0], area.min[1], level),
						mission_scene_point(area.max[0], area.max[1], level),
						mission_scene_point(area.min[0], area.max[1], level) };
					for (int c = 0; c < 4; ++c) line(ring[c], ring[(c + 1) % 4], kMissionAreaRgb, 1.0f);
				}
				for (int c = 0; c < 4; ++c) {
					const double cx = c == 1 || c == 2 ? area.max[0] : area.min[0];
					const double cy = c >= 2 ? area.max[1] : area.min[1];
					line(mission_scene_point(cx, cy, area.min[2]), mission_scene_point(cx, cy, area.max[2]), kMissionAreaRgb, 1.0f);
				}
			}
			list.marker(at, OverlayGlyph::Square, 3.0f, OverlayRole::Normal, kMissionAreaRgb);
			if (index == in.primary) {
				// Its edge handles.
				for (const MissionHandle edge : { MissionHandle::XMin, MissionHandle::XMax, MissionHandle::YMin, MissionHandle::YMax }) {
					float hx = 0.0f, hy = 0.0f;
					if (camera.project(mission_area_edge_middle(area, edge, z), in.width, in.height, hx, hy))
						list.marker(CanvasPoint{ hx, hy }, OverlayGlyph::Square, 4.0f, OverlayRole::Selected);
				}
			}
		} else {
			const MissionEntityMark &entity = scene.entities()[size_t(mark.entity)];
			const uint32_t rgb = pool_rgb(entity.pool);
			switch (entity.pool) {
			case MissionPool::Item:
				list.quad(CanvasPoint{ at.x, at.y - 4.0f }, CanvasPoint{ at.x + 4.0f, at.y }, CanvasPoint{ at.x, at.y + 4.0f },
						CanvasPoint{ at.x - 4.0f, at.y }, rgb);
				break;
			case MissionPool::Building: list.marker(at, OverlayGlyph::Square, 4.0f, OverlayRole::Normal, rgb); break;
			case MissionPool::Marker: list.marker(at, OverlayGlyph::Cross, 5.0f, OverlayRole::Normal, rgb, 1.5f); break;
			case MissionPool::Organic: list.marker(at, OverlayGlyph::Dot, 4.0f, OverlayRole::Normal, rgb); break;
			}
			if (entity.team == 1 || entity.team == 2) list.ring(at, 6.0f, OverlayRole::Normal, 1.0f, mission_team_rgb(entity.team));
			if (index == in.primary && in.handle_reach > 0.0f) {
				// The heading line to the yaw handle, the height handle above.
				float hx = 0.0f, hy = 0.0f;
				const PreviewVec3 yaw = mission_yaw_handle(entity, in.handle_reach);
				line(entity.at, yaw, kMissionItemRgb, 1.5f);
				if (camera.project(yaw, in.width, in.height, hx, hy)) list.disc(CanvasPoint{ hx, hy }, 4.0f, OverlayRole::Selected);
				if (camera.project(mission_height_handle(entity, in.handle_reach), in.width, in.height, hx, hy))
					list.marker(CanvasPoint{ hx, hy }, OverlayGlyph::Square, 4.0f, OverlayRole::Selected);
			}
		}
		// The hovered and the selected rings over a dark one, so they read on any ground.
		if (index == in.hover) {
			list.ring(at, 11.0f, OverlayRole::Normal, 4.0f, kMissionShadowRgb, kMissionShadowAlpha);
			list.ring(at, 11.0f, OverlayRole::Hover, 2.0f);
		}
		if (is_selected(index)) {
			const bool primary = index == in.primary;
			list.ring(at, 9.0f, OverlayRole::Normal, primary ? 5.0f : 3.5f, kMissionShadowRgb, kMissionShadowAlpha);
			list.ring(at, 9.0f, OverlayRole::Selected, primary ? 2.5f : 1.5f);
		}
		if (in.title && (index == in.hover || is_selected(index) || in.options->labels)) {
			MissionLabelCandidate label;
			label.x = at.x;
			label.y = at.y;
			label.depth = mark.depth;
			label.always = index == in.hover || is_selected(index);
			labels.push_back(label);
			label_marks.push_back(index);
		}
	}
	// The labels, decluttered: the hovered and the selected first, then the nearest that overlap none.
	if (!labels.empty()) {
		std::vector<std::string> words(labels.size());
		for (size_t i = 0; i < labels.size(); ++i) {
			words[i] = in.title(marks[size_t(label_marks[i])].record);
			labels[i].length = words[i].size();
		}
		for (const size_t pick : mission_label_picks(labels))
			list.text(CanvasPoint{ labels[pick].x + kMissionLabelDx, labels[pick].y + kMissionLabelDy }, words[pick]);
	}
	// The handle the pointer is on, or a drag holds: what it does and its step, beside it.
	if (in.active_handle && in.primary >= 0 && size_t(in.primary) < marks.size()) {
		const MissionMark &primary = marks[size_t(in.primary)];
		PreviewVec3 at;
		bool has = false;
		if (primary.area >= 0 && mission_handle_is_edge(in.handle)) {
			const MissionAreaMark &area = scene.areas()[size_t(primary.area)];
			at = mission_area_edge_middle(area, in.handle, mission_area_anchor_z(area, in.device));
			has = true;
		} else if (primary.entity >= 0 && (in.handle == MissionHandle::Yaw || in.handle == MissionHandle::Height) &&
				in.handle_reach > 0.0f) {
			const MissionEntityMark &entity = scene.entities()[size_t(primary.entity)];
			at = in.handle == MissionHandle::Yaw ? mission_yaw_handle(entity, in.handle_reach)
												 : mission_height_handle(entity, in.handle_reach);
			has = true;
		}
		float hx = 0.0f, hy = 0.0f;
		if (has && camera.project(at, in.width, in.height, hx, hy)) {
			list.ring(CanvasPoint{ hx, hy }, 7.0f, OverlayRole::Hover, 2.0f);
			list.text(CanvasPoint{ hx + kMissionLabelDx, hy + kMissionLabelDy - kMissionLabelHeight },
					capitalized(mission_handle_words(in.handle)) + " (" + mission_handle_step(in.handle, in.snap, in.turn) + ")",
					0xFFFFFF, OverlayRole::Hover);
		}
	}
	// An Alt-drag's copies: a ring where each goes, a line from where it is.
	if (in.copies) {
		for (const MissionPressed &each : *in.copies) {
			PreviewVec3 from, to;
			if (each.area) {
				const double mx = (each.min[0] + each.max[0]) * 0.5, my = (each.min[1] + each.max[1]) * 0.5;
				from = mission_scene_point(mx, my, each.min[2]);
				to = mission_scene_point(mx + in.copy_by[0], my + in.copy_by[1], each.min[2]);
			} else {
				from = mission_scene_point(each.x, each.y, each.z);
				to = mission_scene_point(each.x + in.copy_by[0], each.y + in.copy_by[1], each.z);
			}
			line(from, to, 0xFFFFFF, 1.0f);
			float cx = 0.0f, cy = 0.0f;
			if (camera.project(to, in.width, in.height, cx, cy)) {
				list.ring(CanvasPoint{ cx, cy }, 9.0f, OverlayRole::Normal, 3.5f, kMissionShadowRgb, kMissionShadowAlpha);
				list.ring(CanvasPoint{ cx, cy }, 9.0f, OverlayRole::Hover, 2.0f);
			}
		}
	}
	// The Area tool's box on the ground, as its drop snaps it.
	if (in.box) {
		const double x0 = snapped(std::min(in.box_from[0], in.box_to[0]), in.box_snap);
		const double x1 = snapped(std::max(in.box_from[0], in.box_to[0]), in.box_snap);
		const double y0 = snapped(std::min(in.box_from[1], in.box_to[1]), in.box_snap);
		const double y1 = snapped(std::max(in.box_from[1], in.box_to[1]), in.box_snap);
		const double corners[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
		PreviewVec3 points[4];
		for (int c = 0; c < 4; ++c) {
			double z = (in.box_from[2] + in.box_to[2]) * 0.5;
			if (in.device) in.device->ground_at(corners[c][0], corners[c][1], z);
			points[c] = mission_scene_point(corners[c][0], corners[c][1], z);
		}
		for (int c = 0; c < 4; ++c) line(points[c], points[(c + 1) % 4], kMissionAreaRgb, 2.5f);
		char size[64];
		std::snprintf(size, sizeof(size), "%.1f m by %.1f m", x1 - x0, y1 - y0);
		list.text(CanvasPoint{ in.pointer.x + kMissionLabelDx, in.pointer.y + kMissionLabelDy }, size, 0xFFFFFF,
				OverlayRole::Hover);
	}
	if (in.marquee) {
		list.rect(CanvasPoint{ std::min(in.marquee_from.x, in.marquee_to.x), std::min(in.marquee_from.y, in.marquee_to.y) },
				CanvasPoint{ std::max(in.marquee_from.x, in.marquee_to.x), std::max(in.marquee_from.y, in.marquee_to.y) },
				OverlayRole::Marquee);
	}
	// An empty mission says how to start, in the middle of the picture.
	if (scene.entities().empty() && scene.areas().empty()) {
		const std::string start = "An empty mission: pick Place to put people, vehicles and buildings on the ground.";
		list.text(CanvasPoint{ float(in.width) * 0.5f - float(start.size()) * kMissionLabelCharWidth * 0.5f,
						  float(in.height) * 0.5f },
				start);
	}
	return list;
}

} // namespace opennova::editor
