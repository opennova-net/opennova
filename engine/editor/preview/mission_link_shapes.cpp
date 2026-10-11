#include <editor/preview/mission_link_shapes.h>

#include <algorithm>
#include <cmath>
#include <set>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_uses.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/view/session_view.h>
#include <base/io/json.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// How many segments a reach's circle is drawn in, and the dark under a line (it reads on any ground).
constexpr int kRingSegments = 64;
constexpr uint8_t kShadowAlpha = 150;
constexpr double kPi = 3.14159265358979323846;

std::array<double, 3> position_of(const MissionEntityMark &entity) { return { entity.x, entity.y, entity.z }; }

// The spot of a record among `spots`, made where none is yet (one a record and a role: a record a trigger
// watches and an action acts on is two spots).
MissionLinkSpot &spot_of(std::vector<MissionLinkSpot> &spots, const NodeAddress &record, bool watched) {
	for (MissionLinkSpot &spot : spots)
		if (spot.record == record && spot.watched == watched) return spot;
	spots.emplace_back();
	spots.back().record = record;
	spots.back().watched = watched;
	return spots.back();
}

// A point `metres` from `at` along a compass bearing (degrees: 0 north, 90 east), at its height.
std::array<double, 3> along(const std::array<double, 3> &at, double bearing, double metres) {
	const double r = bearing * kPi / 180.0;
	return { at[0] + std::sin(r) * metres, at[1] + std::cos(r) * metres, at[2] };
}

} // namespace

MissionLinkScene mission_link_scene(const MissionDocument &document, const MissionScene &scene, const NodeAddress &selected) {
	MissionLinkScene out;
	if (!mission_logic_record(selected)) return out;
	// The whole event's links, the selected trigger's or action's own drawn bolder.
	const MissionLinks links = mission_links(document, NodeAddress{selected.row, k(MissionKind::Event), 0});
	const auto emphasized = [&](const NodeAddress &from) { return !selected.child || from == selected; };
	for (const MissionLink &link : links.links) {
		std::vector<const MissionEntityMark *> marks;
		for (const NodeId row : link.rows)
			if (const MissionEntityMark *mark = scene.entity(row)) marks.push_back(mark);
		const bool watched = !link.action;
		if (link.kind == mission::ParamKind::Zone) {
			const MissionAreaMark *area = link.record.row ? scene.area(link.record.row) : nullptr;
			if (!area) continue;
			MissionLinkSpot &spot = spot_of(out.spots, link.record, watched);
			spot.selected = spot.selected || emphasized(link.from);
			spot.area = true;
			spot.area_index = int(area - scene.areas().data());
			spot.at = { (area->min[0] + area->max[0]) * 0.5, (area->min[1] + area->max[1]) * 0.5, area->min[2] };
			continue;
		}
		if (marks.empty()) continue;
		// The record by itself where it is one entity; a group, a path or a marker number by what it reaches.
		const NodeAddress record = link.record.row ? link.record : NodeAddress{marks.front()->row, marks.front()->kind, 0};
		MissionLinkSpot &spot = spot_of(out.spots, record, watched);
		spot.selected = spot.selected || emphasized(link.from);
		if (link.kind == mission::ParamKind::Path) {
			spot.path = true;
			spot.at = position_of(*marks.front());
		} else if (link.kind == mission::ParamKind::Group) {
			double sum[3] = { 0.0, 0.0, 0.0 };
			for (const MissionEntityMark *mark : marks)
				for (int i = 0; i < 3; ++i) sum[i] += i == 0 ? mark->x : i == 1 ? mark->y : mark->z;
			spot.at = { sum[0] / double(marks.size()), sum[1] / double(marks.size()), sum[2] / double(marks.size()) };
		} else {
			spot.at = position_of(*marks.front());
		}
		spot.members.clear();
		if (marks.size() > 1 || spot.path || link.kind == mission::ParamKind::Group)
			for (const MissionEntityMark *mark : marks) spot.members.push_back(position_of(*mark));
	}
	// A line from each thing watched to each thing acted on (none to itself).
	for (size_t w = 0; w < out.spots.size(); ++w) {
		if (!out.spots[w].watched) continue;
		for (size_t a = 0; a < out.spots.size(); ++a)
			if (!out.spots[a].watched && out.spots[a].record != out.spots[w].record) out.lines.emplace_back(w, a);
	}
	for (const MissionReach &reach : links.reaches) {
		const MissionEntityMark *subject = reach.subject ? scene.entity(reach.subject) : nullptr;
		if (!subject || reach.metres <= 0) continue;
		MissionLinkRing ring;
		ring.at = position_of(*subject);
		ring.metres = double(reach.metres);
		ring.facing = reach.facing;
		ring.yaw = double(subject->yaw);
		ring.selected = emphasized(reach.from);
		out.rings.push_back(ring);
	}
	return out;
}

void mission_link_shapes(const MissionLinkScene &links, const MissionLinkView &view, OverlayList &out) {
	if (!view.point || !view.segment) return;
	const auto segment = [&](const std::array<double, 3> &a, const std::array<double, 3> &b, uint32_t rgb, float thickness,
	                         uint8_t alpha, bool shadow) {
		CanvasPoint from, to;
		if (!view.segment(a, b, from, to)) return false;
		if (shadow) out.line(from, to, 0x000000, thickness + 2.0f, OverlayRole::Normal, uint8_t(kShadowAlpha * alpha / 255));
		out.line(from, to, rgb, thickness, OverlayRole::Normal, alpha);
		return true;
	};
	const auto anchor = [&](const MissionLinkSpot &spot) {
		std::array<double, 3> at = spot.at;
		if (spot.area && view.area_height) at[2] = view.area_height(spot);
		return at;
	};
	// The reaches first, under the rest: the circle at the subject's height, in 3D the sphere's outline,
	// a sees trigger's wedge.
	for (const MissionLinkRing &ring : links.rings) {
		const uint8_t alpha = ring.selected ? 230 : 120;
		const float thickness = ring.selected ? 1.75f : 1.0f;
		for (int i = 0; i < kRingSegments; ++i) {
			const double a = 360.0 * i / kRingSegments, b = 360.0 * (i + 1) / kRingSegments;
			// A sees trigger's arc within its 30 degrees bolder; the rest of its circle as any other.
			const bool inside = ring.facing && std::fabs(std::remainder((a + b) * 0.5 - ring.yaw, 360.0)) <= kMissionSeesHalfAngle;
			segment(along(ring.at, a, ring.metres), along(ring.at, b, ring.metres), kMissionWatchedRgb,
			        inside ? thickness + 1.0f : thickness, alpha, false);
		}
		if (ring.facing)
			for (const double side : { -kMissionSeesHalfAngle, kMissionSeesHalfAngle })
				segment(ring.at, along(ring.at, ring.yaw + side, ring.metres), kMissionWatchedRgb, thickness + 0.5f, alpha, true);
		CanvasPoint centre;
		if (view.outline && view.point(ring.at, centre)) {
			const float radius = view.outline(ring.at, ring.metres);
			if (radius > 2.0f) out.ring(centre, radius, OverlayRole::Normal, 1.0f, kMissionWatchedRgb, uint8_t(alpha / 2));
		}
	}
	// What a group and a marker number reach: a spoke to each from their middle; a path's line through its
	// stops.
	for (const MissionLinkSpot &spot : links.spots) {
		const uint32_t rgb = spot.watched ? kMissionWatchedRgb : kMissionActedRgb;
		const uint8_t alpha = spot.selected ? 255 : 140;
		if (spot.path) {
			for (size_t i = 0; i + 1 < spot.members.size(); ++i)
				segment(spot.members[i], spot.members[i + 1], rgb, spot.selected ? 2.5f : 1.5f, alpha, true);
		} else {
			for (const std::array<double, 3> &member : spot.members) segment(spot.at, member, rgb, 1.0f, uint8_t(alpha * 3 / 4), false);
		}
		for (const std::array<double, 3> &member : spot.members) {
			CanvasPoint at;
			if (view.point(member, at)) out.ring(at, 7.0f, OverlayRole::Normal, 1.5f, rgb, alpha);
		}
	}
	// The lines from what is watched to what is acted on, an arrowhead short of the target's ring.
	for (const auto &[w, a] : links.lines) {
		const MissionLinkSpot &from = links.spots[w], &to = links.spots[a];
		const bool bold = from.selected || to.selected;
		CanvasPoint p, q;
		if (!view.segment(anchor(from), anchor(to), p, q)) continue;
		const uint8_t alpha = bold ? 255 : 130;
		const float thickness = bold ? 2.0f : 1.25f;
		out.line(p, q, 0x000000, thickness + 2.0f, OverlayRole::Normal, uint8_t(kShadowAlpha * alpha / 255));
		out.line(p, q, kMissionWatchedRgb, thickness, OverlayRole::Normal, alpha);
		const float dx = q.x - p.x, dy = q.y - p.y, length = std::sqrt(dx * dx + dy * dy);
		if (length < 30.0f) continue;
		const float ux = dx / length, uy = dy / length;
		const CanvasPoint tip{ q.x - ux * 15.0f, q.y - uy * 15.0f };
		const CanvasPoint left{ tip.x - ux * 9.0f - uy * 5.0f, tip.y - uy * 9.0f + ux * 5.0f };
		const CanvasPoint right{ tip.x - ux * 9.0f + uy * 5.0f, tip.y - uy * 9.0f - ux * 5.0f };
		out.quad(tip, left, right, right, kMissionActedRgb, alpha);
	}
	// Each record named, rung where it stands (over a dark ring).
	for (const MissionLinkSpot &spot : links.spots) {
		CanvasPoint at;
		if (!view.point(anchor(spot), at)) continue;
		const uint32_t rgb = spot.watched ? kMissionWatchedRgb : kMissionActedRgb;
		const float radius = spot.watched ? 14.0f : 17.0f;
		const float thickness = spot.selected ? 2.5f : 1.5f;
		out.ring(at, radius, OverlayRole::Normal, thickness + 2.0f, 0x000000, kShadowAlpha);
		out.ring(at, radius, OverlayRole::Normal, thickness, rgb, spot.selected ? 255 : 160);
	}
}

bool mission_select_group(const MissionDocument &document, const std::vector<NodeAddress> &records,
		const NodeAddress &primary, CanvasRequests &out, std::string &error) {
	std::vector<NodeAddress> members = mission_group_members(document, records);
	if (members.empty()) {
		error = "Select an entity in a group (or a group) first: the command selects every entity of its group.";
		return false;
	}
	// The primary last (SelectRecord's records name it last).
	NodeAddress last = primary;
	if (std::find(members.begin(), members.end(), last) == members.end()) last = members.front();
	members.erase(std::remove(members.begin(), members.end(), last), members.end());
	members.push_back(last);
	out.request(request::select_record(document.path(), last, SelectMode::Replace, members));
	return true;
}

MissionPickFrame mission_pick_frame(const MissionDocument &document, const NodeAddress &record, int slot,
		const std::vector<NodeAddress> &marks) {
	MissionPickFrame out;
	if (!record.row || !mission_pick_slot(document, record, slot, out.takes)) return out;
	out.record = record;
	out.slot = slot;
	out.fills.assign(marks.size(), 0);
	out.values.assign(marks.size(), 0);
	if (!out.takes.marks) return out;
	for (size_t i = 0; i < marks.size(); ++i) out.fills[i] = mission_pick_value(document, out.takes, marks[i], out.values[i]);
	return out;
}

std::string mission_pick_hint(const MissionPickFrame &pick) {
	if (!pick.active()) return std::string();
	if (!pick.takes.marks)
		return pick.takes.label + " takes " + pick.takes.what + ": pick it in its list instead (Esc cancels).";
	return "Click " + pick.takes.what + " for " + pick.takes.label + " (Esc cancels).";
}

void mission_pick_requests(const std::string &path, const MissionPickFrame &pick, int mark, CanvasRequests &out) {
	if (!pick.fills_mark(mark)) return;
	Edit edit;
	edit.address = pick.record;
	edit.field = mission_param_field(pick.slot);
	edit.value = pick.values[size_t(mark)];
	out.request(request::edit_record(path, std::vector<Edit>{ edit }));
	mission_pick_end(path, out);
}

void mission_pick_end(const std::string &path, CanvasRequests &out) {
	io::JsonValue members = io::JsonValue::make_object();
	members.set("path", io::JsonValue::make_string(path));
	members.set("pick", io::JsonValue::make_object());
	io::JsonValue change = io::JsonValue::make_object();
	change.set("document", std::move(members));
	out.request(request::set_workspace(io::json_write(change)));
}

bool mission_pick_command(const ViewportContext &context, const Document &document, const std::vector<NodeId> &ids,
		CanvasRequests &out, std::string &error) {
	const MissionDocument *mission = dynamic_cast<const MissionDocument *>(&document);
	const auto shown = context.input.view.workspace.documents.find(document.path());
	if (!mission || shown == context.input.view.workspace.documents.end() || !shown->second.pick.active()) {
		error = "No pick waits: a chip's Pick in view asks one (the workspace's document pick).";
		return false;
	}
	if (!context.editable()) {
		error = context.not_editable();
		return false;
	}
	if (ids.size() != 1) {
		error = "pick takes the one record clicked (ids: [its identity]).";
		return false;
	}
	const MissionPickFrame pick = mission_pick_frame(*mission, shown->second.pick.record, shown->second.pick.slot,
			{ document.address_of(ids.front()) });
	if (!pick.fills_mark(0)) {
		error = "Record " + std::to_string(ids.front()) + " gives " + (pick.takes.label.empty() ? std::string("the slot") : pick.takes.label) +
				" nothing: it takes " + (pick.takes.what.empty() ? std::string("another record") : pick.takes.what) + ".";
		return false;
	}
	mission_pick_requests(document.path(), pick, 0, out);
	return true;
}

void mission_pick_candidate(CanvasPoint at, OverlayList &out) {
	out.ring(at, 13.0f, OverlayRole::Normal, 3.0f, 0x000000, 110);
	out.ring(at, 13.0f, OverlayRole::Normal, 1.5f, kMissionWatchedRgb, 200);
}

void mission_link_badge(CanvasPoint at, size_t count, OverlayList &out) {
	if (count == 0) return;
	// Above the mark and to its left, clear of its label (right of it, mission_label_picks).
	const std::string number = std::to_string(count);
	out.text(CanvasPoint{ at.x - 6.0f - 7.0f * float(number.size()), at.y - 21.0f }, number, kMissionBadgeRgb);
}

std::string mission_link_hover(const MissionUses &uses, size_t most) {
	std::set<size_t> seen;
	std::string lines;
	size_t listed = 0;
	for (const MissionUse &use : uses.uses) {
		if (!seen.insert(use.event_index).second) continue;
		if (listed++ < most) lines += "\nEvent " + std::to_string(use.event_index + 1) + ": " + use.sentence;
	}
	if (seen.empty()) return std::string();
	std::string out = "Named by " + std::to_string(seen.size()) + (seen.size() == 1 ? " event:" : " events:") + lines;
	if (seen.size() > most) out += "\n... and " + std::to_string(seen.size() - most) + " more (the Inspector lists them).";
	return out;
}

} // namespace opennova::editor
