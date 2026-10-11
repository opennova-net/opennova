#include <editor/preview/mission_place.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_reads.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_device.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
#include <runtime/mission/player_start.h>

namespace opennova::editor {

namespace {

Edit set_of(const NodeAddress &record, const char *field, Value value) {
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

double snapped(double value, float snap) {
	return snap > 0.0f ? std::round(value / double(snap)) * double(snap) : value;
}

} // namespace

bool mission_duplicate_edits(const Document &document, const MissionScene &scene, const std::vector<NodeAddress> &records,
		const NodeAddress &primary, double east, double north, bool stick, const ViewportDevice *device,
		std::vector<Edit> &out, std::string &error) {
	out.clear();
	// The primary first: the batch selects what it made, its first the primary. Each record once, however
	// often it is named.
	std::vector<NodeAddress> ordered;
	if (std::find(records.begin(), records.end(), primary) != records.end()) ordered.push_back(primary);
	for (const NodeAddress &record : records)
		if (std::find(ordered.begin(), ordered.end(), record) == ordered.end()) ordered.push_back(record);
	if (ordered.empty()) {
		error = "Nothing to duplicate: select an entity or an area first.";
		return false;
	}
	const bool moves = east != 0.0 || north != 0.0;
	// A copy that would go past what the file's positions hold is refused, never clamped or wrapped.
	const auto holds = [](double metres) { return metres >= bms::kFixed16Min && metres <= bms::kFixed16Max; };
	for (const NodeAddress &record : ordered) {
		const MissionEntityMark *entity = scene.entity(record.row);
		const MissionAreaMark *area = entity ? nullptr : scene.area(record.row);
		if (!entity && !area) {
			error = document.record_title(record) + " is no entity or area: the picture copies those (the outline "
													"duplicates the rest).";
			out.clear();
			return false;
		}
		const bool inside = entity ? holds(entity->x + east) && holds(entity->y + north)
								   : holds(area->min[0] + east) && holds(area->max[0] + east) && holds(area->min[1] + north) &&
											 holds(area->max[1] + north);
		if (!inside) {
			error = "A copy of " + document.record_title(record) +
					" would go past what the mission's positions hold (32,768 m from its origin).";
			out.clear();
			return false;
		}
		Edit copy;
		copy.operation = EditOperation::Duplicate;
		copy.address = NodeAddress{ record.row, record.kind, 0 };
		const size_t made = out.size();
		out.push_back(std::move(copy));
		if (!moves) continue;
		const NodeAddress at{ batch_made(made), record.kind, 0 };
		if (entity) {
			const double x = entity->x + east, y = entity->y + north;
			out.push_back(set_of(at, mission_field_ids::kX, x));
			out.push_back(set_of(at, mission_field_ids::kY, y));
			// Its height over the ground kept, as a move keeps it.
			double was = 0.0, now = 0.0;
			if (stick && device && device->ground_at(entity->x, entity->y, was) && device->ground_at(x, y, now))
				out.push_back(set_of(at, mission_field_ids::kZ, now + (entity->z - was)));
		} else {
			out.push_back(set_of(at, mission_field_ids::kXMin, area->min[0] + east));
			out.push_back(set_of(at, mission_field_ids::kXMax, area->max[0] + east));
			out.push_back(set_of(at, mission_field_ids::kYMin, area->min[1] + north));
			out.push_back(set_of(at, mission_field_ids::kYMax, area->max[1] + north));
		}
	}
	return true;
}

bool mission_area_edits(const double a[2], const double b[2], float snap, std::vector<Edit> &out, std::string &error) {
	out.clear();
	const double x_min = snapped(std::min(a[0], b[0]), snap), x_max = snapped(std::max(a[0], b[0]), snap);
	const double y_min = snapped(std::min(a[1], b[1]), snap), y_max = snapped(std::max(a[1], b[1]), snap);
	if (!(x_max > x_min) || !(y_max > y_min)) {
		error = "An area needs some width and depth: drag a box on the ground.";
		return false;
	}
	Edit add;
	add.operation = EditOperation::Add;
	add.address = NodeAddress{ 0, node_kind(MissionKind::Area), 0 };
	out.push_back(std::move(add));
	const NodeAddress made{ batch_made(0), node_kind(MissionKind::Area), 0 };
	out.push_back(set_of(made, mission_field_ids::kXMin, x_min));
	out.push_back(set_of(made, mission_field_ids::kXMax, x_max));
	out.push_back(set_of(made, mission_field_ids::kYMin, y_min));
	out.push_back(set_of(made, mission_field_ids::kYMax, y_max));
	return true;
}

bool mission_stop_edits(const MissionDocument &document, int path, int64_t item, MissionKind pool, const double at[3],
		int yaw, std::vector<Edit> &out, std::string &error, size_t position) {
	out.clear();
	if (path <= 0 || path >= mission::kFirstPathCommand) {
		error = path <= 0 ? std::string("Pick a path to add stops to (path 0 is none).")
						  : "Path " + std::to_string(path) + " is a command (123 to 127), not a route: pick another.";
		return false;
	}
	const Node *row = nullptr;
	for (const Node *each : document.rows_of(MissionKind::WaypointPath))
		if (static_cast<const PathRow &>(*each).native.number == path) row = each;
	if (!row) {
		error = "The mission has no path " + std::to_string(path) + ".";
		return false;
	}
	if (item == 0) {
		error = "Pick the marker a stop places (the Place tool's Markers): no stop of the mission names one yet.";
		return false;
	}
	if (pool != MissionKind::Marker) {
		error = "Item " + std::to_string(item) + " is placed among the " +
				(pool == MissionKind::Organic ? std::string("organics") : pool == MissionKind::Building ? "buildings" : "items") +
				": a stop names a marker (pick one of the Markers).";
		return false;
	}
	// The marker: added at the end of its band, so its index among the markers is their count now.
	const size_t index = document.rows_of(MissionKind::Marker).size();
	Edit add;
	add.operation = EditOperation::Add;
	add.address = NodeAddress{ 0, node_kind(MissionKind::Marker), 0 };
	add.field = "item";
	add.value = item;
	out.push_back(std::move(add));
	const NodeAddress made{ batch_made(0), node_kind(MissionKind::Marker), 0 };
	out.push_back(set_of(made, mission_field_ids::kX, at[0]));
	out.push_back(set_of(made, mission_field_ids::kY, at[1]));
	out.push_back(set_of(made, mission_field_ids::kZ, at[2]));
	out.push_back(set_of(made, mission_field_ids::kYaw, int64_t(mission::wrapped_yaw(double(yaw)))));
	// The stop, after the path's last, or at its place among them.
	Edit stop;
	stop.operation = EditOperation::Add;
	stop.address = NodeAddress{ row->id, node_kind(MissionKind::Stop), 0 };
	stop.field = "marker";
	stop.value = int64_t(index);
	stop.position = position < static_cast<const PathRow &>(*row).native.stops.size() ? position : SIZE_MAX;
	out.push_back(std::move(stop));
	return true;
}

size_t mission_stop_place(const MissionScene &scene, int path, float x, float y, float slop,
		const std::function<bool(const MissionEntityMark &marker, float &px, float &py)> &project) {
	const MissionPathMark *found = nullptr;
	for (const MissionPathMark &each : scene.paths())
		if (each.index == path) found = &each;
	if (!found) return SIZE_MAX;
	// Each stop that names a marker on the picture, by its place among the path's stops.
	std::vector<std::pair<size_t, CanvasPoint>> stops;
	for (size_t i = 0; i < found->stops.size(); ++i) {
		const MissionEntityMark *marker = found->stops[i] ? scene.entity(found->stops[i]) : nullptr;
		CanvasPoint at;
		if (marker && project(*marker, at.x, at.y)) stops.emplace_back(i, at);
	}
	size_t best = SIZE_MAX;
	float nearest = slop;
	for (size_t i = 0; i + 1 < stops.size(); ++i) {
		const CanvasPoint a = stops[i].second, b = stops[i + 1].second;
		const float dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
		const float t = length2 > 0.0f ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / length2, 0.0f, 1.0f) : 0.0f;
		const float distance = std::hypot(a.x + dx * t - x, a.y + dy * t - y);
		// Strictly between its ends: a click on a stop itself adds after the last, as before.
		if (distance <= nearest && t > 0.0f && t < 1.0f) {
			nearest = distance;
			best = stops[i + 1].first;
		}
	}
	return best;
}

std::vector<NodeAddress> mission_view_removals(const MissionDocument &document, const std::vector<NodeAddress> &records) {
	std::vector<NodeAddress> out;
	const auto add = [&](const NodeAddress &record) {
		if (std::find(out.begin(), out.end(), record) == out.end()) out.push_back(record);
	};
	for (const NodeAddress &record : records) {
		if (record.child && record.kind == node_kind(MissionKind::Stop)) {
			// The marker the stop names (its index among the markers).
			const Node *row = document.row(record.row);
			Document::Placement at;
			if (!row || row->kind != node_kind(MissionKind::WaypointPath) || !document.placement(record, at)) continue;
			const std::vector<uint32_t> &stops = static_cast<const PathRow &>(*row).native.stops;
			if (at.index < stops.size())
				if (const Node *marker = document.row_of(MissionKind::Marker, stops[at.index]))
					add(NodeAddress{ marker->id, marker->kind, 0 });
			continue;
		}
		add(record);
	}
	return out;
}

int64_t mission_stop_item(const MissionScene &scene, int path) {
	// The scene's paths come by number: the last one met that has a marker is the last path's.
	int64_t own = 0, last = 0;
	for (const MissionPathMark &each : scene.paths()) {
		for (auto stop = each.stops.rbegin(); stop != each.stops.rend(); ++stop) {
			const MissionEntityMark *marker = *stop ? scene.entity(*stop) : nullptr;
			if (!marker) continue;
			if (each.index == path) own = marker->item;
			last = marker->item;
			break;
		}
	}
	return own ? own : last;
}

std::vector<NodeAddress> mission_same_item(const MissionScene &scene, const std::vector<NodeAddress> &records) {
	std::unordered_set<int64_t> items;
	for (const NodeAddress &record : records)
		if (const MissionEntityMark *entity = scene.entity(record.row)) items.insert(entity->item);
	std::vector<NodeAddress> out;
	for (const MissionEntityMark &entity : scene.entities())
		if (items.count(entity.item)) out.push_back(NodeAddress{ entity.row, entity.kind, 0 });
	return out;
}

} // namespace opennova::editor
