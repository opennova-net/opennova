#include <editor/preview/mission_overlay.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_hint.h>
#include <editor/preview/mission_label_picks.h>
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
	// The game's map colours (mission_overlay.h): team 1 the HUD palette's palette[3], team 2 its palette[5].
	switch (team) {
	case 1: return kMissionBlueRgb;
	case 2: return kMissionRedRgb;
	default: return kMissionPathRgb;
	}
}

uint32_t mission_group_rgb(int group) {
	if (group <= 0 || group >= 64) return 0;
	// The golden angle round the hue circle a group, three tiers of saturation and value in turn (the editor's
	// palette: each reads on the terrain, neighbours apart).
	const double hue = std::fmod(double(group) * 0.3819660112501051, 1.0) * 6.0;
	const double saturation[3] = { 0.85, 0.55, 0.95 }, value[3] = { 1.0, 0.95, 0.8 };
	const double s = saturation[group % 3], v = value[(group / 3) % 3];
	const int sector = int(hue) % 6;
	const double f = hue - std::floor(hue), p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
	double r = v, g = t, b = p;
	switch (sector) {
	case 0: r = v, g = t, b = p; break;
	case 1: r = q, g = v, b = p; break;
	case 2: r = p, g = v, b = t; break;
	case 3: r = p, g = q, b = v; break;
	case 4: r = t, g = p, b = v; break;
	default: r = v, g = p, b = q; break;
	}
	const auto byte = [](double c) { return uint32_t(std::lround(std::clamp(c, 0.0, 1.0) * 255.0)); };
	return (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

bool mission_mark_ring_rgb(MissionColorBy by, int team, int group, uint32_t &rgb) {
	if (by == MissionColorBy::Group) {
		rgb = mission_group_rgb(group);
		return rgb != 0;
	}
	if (team != 1 && team != 2) return false;
	rgb = mission_team_rgb(team);
	return true;
}

void mission_group_legend(const std::vector<std::pair<int, size_t>> &groups, const std::function<std::string(int)> &title,
		int width, OverlayList &out, size_t most) {
	if (groups.empty()) return;
	std::vector<std::pair<std::string, uint32_t>> lines;
	lines.emplace_back("Groups in view", 0xFFFFFF);
	for (size_t i = 0; i < groups.size() && i < most; ++i) {
		const auto &[group, count] = groups[i];
		const std::string name = title ? title(group) : "Group " + std::to_string(group);
		lines.emplace_back(name + " (" + std::to_string(count) + ")", mission_group_rgb(group));
	}
	if (groups.size() > most) lines.emplace_back("and " + std::to_string(groups.size() - most) + " more", 0xC0C0C0);
	size_t longest = 0;
	for (const auto &line : lines) longest = std::max(longest, line.first.size());
	const float x = float(width) - 10.0f - float(longest) * kMissionLabelCharWidth;
	for (size_t i = 0; i < lines.size(); ++i)
		out.text(CanvasPoint{ x, 8.0f + float(i) * (kMissionLabelHeight + 2.0f) }, lines[i].first, lines[i].second);
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

PreviewVec3 mission_area_handle_point(const MissionAreaMark &area, MissionHandle handle, double z) {
	const double mx = (area.min[0] + area.max[0]) * 0.5, my = (area.min[1] + area.max[1]) * 0.5;
	switch (handle) {
	case MissionHandle::XMin: return mission_scene_point(area.min[0], my, z);
	case MissionHandle::XMax: return mission_scene_point(area.max[0], my, z);
	case MissionHandle::YMin: return mission_scene_point(mx, area.min[1], z);
	case MissionHandle::YMax: return mission_scene_point(mx, area.max[1], z);
	case MissionHandle::XMinYMin: return mission_scene_point(area.min[0], area.min[1], z);
	case MissionHandle::XMaxYMin: return mission_scene_point(area.max[0], area.min[1], z);
	case MissionHandle::XMaxYMax: return mission_scene_point(area.max[0], area.max[1], z);
	case MissionHandle::XMinYMax: return mission_scene_point(area.min[0], area.max[1], z);
	case MissionHandle::ZMin: return mission_scene_point(mx, my, area.min[2]);
	case MissionHandle::ZMax: return mission_scene_point(mx, my, area.max[2]);
	default: return mission_scene_point(mx, my, z);
	}
}

std::vector<MissionHandle> mission_area_handles(const MissionAreaMark &area) {
	std::vector<MissionHandle> out = { MissionHandle::XMin, MissionHandle::XMax, MissionHandle::YMin, MissionHandle::YMax,
		MissionHandle::XMinYMin, MissionHandle::XMaxYMin, MissionHandle::XMaxYMax, MissionHandle::XMinYMax };
	if (area.constrains_z) {
		out.push_back(MissionHandle::ZMin);
		out.push_back(MissionHandle::ZMax);
	}
	return out;
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

	// The paths first, under the marks: a line through each one's stops that name a marker; S24: an arrowhead along
	// each segment the way the path goes, each stop's number (its place among the path's stops, as the outline and an
	// event's stop name it) while the path or one of its markers is selected or with the labels option, and a path of
	// one stop ringed where it stands.
	if (in.options->paths) {
		for (const MissionPathMark &path : scene.paths()) {
			std::vector<PreviewVec3> stops;
			std::vector<size_t> numbers;
			bool selected = selected_row(path.row);
			for (size_t s = 0; s < path.stops.size(); ++s) {
				const MissionEntityMark *marker = path.stops[s] ? scene.entity(path.stops[s]) : nullptr;
				if (!marker) continue;
				stops.push_back(marker->at);
				numbers.push_back(s);
				selected = selected || selected_row(marker->row);
			}
			if (stops.empty()) continue;
			const uint32_t rgb = (path.flags & uint32_t(bms::WaypointFlags::PlayerRoute)) ? kMissionBlueRgb
					: (path.flags & uint32_t(bms::WaypointFlags::RedTeam))             ? kMissionRedRgb
																						 : kMissionPathRgb;
			const float thickness = selected ? 2.5f : 1.0f;
			// A segment and its arrowhead at its middle, pointing the way the path goes.
			const auto segment = [&](const PreviewVec3 &a, const PreviewVec3 &b) {
				CanvasPoint from, to;
				if (!mission_project_segment(camera, in.width, in.height, a, b, from, to)) return;
				list.line(from, to, rgb, thickness);
				const float dx = to.x - from.x, dy = to.y - from.y, length = std::sqrt(dx * dx + dy * dy);
				if (length < 28.0f) return;
				const float ux = dx / length, uy = dy / length, size = selected ? 7.0f : 5.0f;
				const CanvasPoint tip{ (from.x + to.x) * 0.5f + ux * size * 0.5f, (from.y + to.y) * 0.5f + uy * size * 0.5f };
				const CanvasPoint left{ tip.x - ux * size - uy * size * 0.6f, tip.y - uy * size + ux * size * 0.6f };
				const CanvasPoint right{ tip.x - ux * size + uy * size * 0.6f, tip.y - uy * size - ux * size * 0.6f };
				list.quad(tip, left, right, right, rgb);
			};
			for (size_t i = 0; i + 1 < stops.size(); ++i) segment(stops[i], stops[i + 1]);
			if (stops.size() > 1 && !(path.flags & uint32_t(bms::WaypointFlags::DoesNotLoop))) segment(stops.back(), stops.front());
			if (stops.size() == 1) {
				float sx = 0.0f, sy = 0.0f;
				if (camera.project(stops.front(), in.width, in.height, sx, sy))
					list.ring(CanvasPoint{ sx, sy }, 8.0f, OverlayRole::Normal, thickness + 0.5f, rgb);
			}
			if (selected || in.options->labels)
				for (size_t i = 0; i < stops.size(); ++i) {
					float sx = 0.0f, sy = 0.0f;
					if (camera.project(stops[i], in.width, in.height, sx, sy))
						list.text(CanvasPoint{ sx - 4.0f, sy + 8.0f }, std::to_string(numbers[i]), rgb);
				}
		}
	}

	// S24: the selected logic record's links, under the marks.
	if (in.options->links && in.links && in.links->any()) {
		MissionLinkView view;
		view.point = [&](const std::array<double, 3> &at, CanvasPoint &out) {
			return camera.project(mission_scene_point(at[0], at[1], at[2]), in.width, in.height, out.x, out.y);
		};
		view.segment = [&](const std::array<double, 3> &a, const std::array<double, 3> &b, CanvasPoint &from, CanvasPoint &to) {
			return mission_project_segment(camera, in.width, in.height, mission_scene_point(a[0], a[1], a[2]),
					mission_scene_point(b[0], b[1], b[2]), from, to);
		};
		// A sphere's outline seen from the eye: its apparent radius, f r / sqrt(d^2 - r^2) (none from inside it).
		const float focal = camera.focal(in.width);
		view.outline = [&](const std::array<double, 3> &at, double metres) {
			float x = 0.0f, y = 0.0f, depth = 0.0f;
			if (!camera.project(mission_scene_point(at[0], at[1], at[2]), in.width, in.height, x, y, &depth)) return 0.0f;
			const double d = double(depth), r = metres;
			return d > r ? float(double(focal) * r / std::sqrt(d * d - r * r)) : 0.0f;
		};
		view.area_height = [&](const MissionLinkSpot &spot) {
			return spot.area_index >= 0 && size_t(spot.area_index) < scene.areas().size()
					? mission_area_anchor_z(scene.areas()[size_t(spot.area_index)], in.device)
					: spot.at[2];
		};
		mission_link_shapes(*in.links, view, list);
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
	// S24: how many drawn marks carry each group (the legend's, coloured by group).
	std::vector<size_t> grouped(64, 0);
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
				// Its edge handles, its corners' (S24), and its bottom's and top's where its flags bound z (a line
				// between them).
				if (area.constrains_z)
					line(mission_area_handle_point(area, MissionHandle::ZMin, z), mission_area_handle_point(area, MissionHandle::ZMax, z),
							kMissionAreaRgb, 1.5f);
				for (const MissionHandle handle : mission_area_handles(area)) {
					float hx = 0.0f, hy = 0.0f;
					if (camera.project(mission_area_handle_point(area, handle, z), in.width, in.height, hx, hy))
						list.marker(CanvasPoint{ hx, hy }, OverlayGlyph::Square, mission_handle_is_corner(handle) ? 3.5f : 4.0f,
								OverlayRole::Selected);
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
			// Its team's ring, or (S24) its group's.
			uint32_t ring = 0;
			if (mission_mark_ring_rgb(in.options->color_by, entity.team, entity.group, ring))
				list.ring(at, 6.0f, OverlayRole::Normal, in.options->color_by == MissionColorBy::Group ? 1.75f : 1.0f, ring);
			if (in.options->color_by == MissionColorBy::Group && entity.group > 0 && entity.group < 64) ++grouped[entity.group];
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
		// S24: how many events name it.
		if (in.options->links && in.badges && size_t(index) < in.badges->size())
			mission_link_badge(at, (*in.badges)[size_t(index)], list);
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
			label.always = index == in.hover || index == in.primary;
			label.first = !label.always && is_selected(index);
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
		// The last layout where nothing it read moved (a frame with the selection, the camera, the hover
		// and the titles as they were), else laid out again.
		std::vector<size_t> made;
		const std::vector<size_t> &picks = in.layout ? in.layout->picks(labels, words) : (made = mission_label_picks(labels));
		for (const size_t pick : picks)
			list.text(CanvasPoint{ labels[pick].x + kMissionLabelDx, labels[pick].y + kMissionLabelDy }, words[pick]);
	}
	// The handle the pointer is on, or a drag holds: what it does and its step, beside it.
	if (in.active_handle && in.primary >= 0 && size_t(in.primary) < marks.size()) {
		const MissionMark &primary = marks[size_t(in.primary)];
		PreviewVec3 at;
		bool has = false;
		if (primary.area >= 0 && mission_handle_is_area(in.handle)) {
			const MissionAreaMark &area = scene.areas()[size_t(primary.area)];
			at = mission_area_handle_point(area, in.handle, mission_area_anchor_z(area, in.device));
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
	// A group turn's pivot: a cross over a dark ring where the records turn about.
	if (in.pivot) {
		float px = 0.0f, py = 0.0f;
		if (camera.project(mission_scene_point(in.pivot_at[0], in.pivot_at[1], in.pivot_at[2]), in.width, in.height, px, py)) {
			list.ring(CanvasPoint{ px, py }, 7.0f, OverlayRole::Normal, 3.5f, kMissionShadowRgb, kMissionShadowAlpha);
			list.marker(CanvasPoint{ px, py }, OverlayGlyph::Cross, 6.0f, OverlayRole::Selected, kMissionItemRgb, 2.0f);
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
	// S24: the groups' legend, coloured by group.
	if (in.options->color_by == MissionColorBy::Group) {
		std::vector<std::pair<int, size_t>> groups;
		for (int group = 1; group < 64; ++group)
			if (grouped[size_t(group)]) groups.emplace_back(group, grouped[size_t(group)]);
		mission_group_legend(groups, in.group_title, in.width, list);
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
