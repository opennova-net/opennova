#include <editor/preview/mission_handle_edit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <variant>

#include <base/io/bam.h>
#include <editor/model/document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_model.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

namespace {

struct HandleToken {
	MissionHandle handle;
	const char *token;
};
constexpr HandleToken kHandles[] = {
	{ MissionHandle::Move, "move" }, { MissionHandle::Height, "height" }, { MissionHandle::Yaw, "yaw" },
	{ MissionHandle::XMin, "x_min" }, { MissionHandle::XMax, "x_max" }, { MissionHandle::YMin, "y_min" },
	{ MissionHandle::YMax, "y_max" },
};

double snapped(double value, float snap) {
	return snap > 0.0f ? std::round(value / double(snap)) * double(snap) : value;
}

// A Set of the position field `field` to `value` where its 16.16 word is another than the word the
// row holds now (the file's own step: less moves nothing it writes).
void set_position(const Document &document, const NodeAddress &record, const char *field, double value,
		uint64_t gesture, std::vector<Edit> &out) {
	Value now;
	if (document.get(record, field, now) && std::holds_alternative<double>(now) &&
			bms::to_fixed_16_16(std::get<double>(now)) == bms::to_fixed_16_16(value))
		return;
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = value;
	edit.gesture = gesture;
	out.push_back(std::move(edit));
}

// A Set of `yaw` to `degrees` (0..359) where the row holds another.
void set_yaw(const Document &document, const NodeAddress &record, int degrees, uint64_t gesture,
		std::vector<Edit> &out) {
	Value now;
	if (document.get(record, "yaw", now) && std::holds_alternative<int64_t>(now) &&
			std::get<int64_t>(now) == int64_t(degrees))
		return;
	Edit edit;
	edit.address = record;
	edit.field = "yaw";
	edit.value = int64_t(degrees);
	edit.gesture = gesture;
	out.push_back(std::move(edit));
}

// Degrees in 0..359, as the file stores a yaw.
int wrapped(double degrees) {
	const long whole = std::lround(degrees);
	const long turn = whole % 360;
	return int(turn < 0 ? turn + 360 : turn);
}

} // namespace

bool mission_handle_from_token(const char *token, MissionHandle &out) {
	for (const HandleToken &row : kHandles)
		if (std::strcmp(token, row.token) == 0) {
			out = row.handle;
			return true;
		}
	return false;
}

const char *mission_handle_token(MissionHandle handle) {
	for (const HandleToken &row : kHandles)
		if (row.handle == handle) return row.token;
	return "";
}

bool mission_ground_point(const ViewportContext &context, const OrbitCamera &camera, float x, float y,
		double plane_z, double out[3], bool *on_terrain) {
	if (on_terrain) *on_terrain = false;
	PreviewVec3 from, along;
	if (!camera.ray(x, y, context.width, context.height, from, along)) return false;
	if (context.device) {
		// The ray as a segment of the mission, from the eye as far as a pick reaches.
		const double length = std::sqrt(double(along.x) * along.x + double(along.y) * along.y + double(along.z) * along.z);
		const double reach = length > 0.0 ? kMissionPickReach / length : 0.0;
		const PreviewVec3 far{ float(double(from.x) + double(along.x) * reach),
			float(double(from.y) + double(along.y) * reach), float(double(from.z) + double(along.z) * reach) };
		double start[3], end[3];
		preview_to_mission(from, start);
		preview_to_mission(far, end);
		if (context.device->surface_between(start, end, out)) {
			// The march's stop is the ray's point over the ground (the terrain raycast steps); its height
			// the ground's there, as a move's `stick` and the ground command read it, so what lands on
			// the point lies on the ground.
			double height = 0.0;
			if (context.device->ground_at(out[0], out[1], height)) out[2] = height;
			if (on_terrain) *on_terrain = true;
			return true;
		}
	}
	return mission_camera_on_height(camera, x, y, context.width, context.height, plane_z, out);
}

bool mission_move_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		const double to[2], float snap, bool stick, const ViewportDevice *device, uint64_t gesture,
		std::vector<Edit> &out) {
	out.clear();
	if (grabbed >= pressed.size()) return false;
	const MissionPressed &held = pressed[grabbed];
	const double dx = snapped(to[0], snap) - held.x, dy = snapped(to[1], snap) - held.y;
	for (const MissionPressed &each : pressed) {
		if (each.area) {
			set_position(document, each.record, "x_min", each.min[0] + dx, gesture, out);
			set_position(document, each.record, "x_max", each.max[0] + dx, gesture, out);
			set_position(document, each.record, "y_min", each.min[1] + dy, gesture, out);
			set_position(document, each.record, "y_max", each.max[1] + dy, gesture, out);
			continue;
		}
		const double x = each.x + dx, y = each.y + dy;
		set_position(document, each.record, "x", x, gesture, out);
		set_position(document, each.record, "y", y, gesture, out);
		// Its height over the ground kept: over where it stood as pressed, and over where it goes.
		double was = 0.0, now = 0.0;
		if (stick && device && device->ground_at(each.x, each.y, was) && device->ground_at(x, y, now))
			set_position(document, each.record, "z", now + (each.z - was), gesture, out);
		else
			set_position(document, each.record, "z", each.z, gesture, out);
	}
	return true;
}

bool mission_height_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		double dz, float snap, uint64_t gesture, std::vector<Edit> &out) {
	out.clear();
	if (grabbed >= pressed.size() || pressed[grabbed].area) return false;
	const double by = snapped(pressed[grabbed].z + dz, snap) - pressed[grabbed].z;
	for (const MissionPressed &each : pressed)
		if (!each.area) set_position(document, each.record, "z", each.z + by, gesture, out);
	return true;
}

bool mission_turn_centre(const std::vector<MissionPressed> &pressed, double out[2]) {
	size_t members = 0;
	double low[2] = { 0.0, 0.0 }, high[2] = { 0.0, 0.0 };
	for (const MissionPressed &each : pressed) {
		// An entity by its position, an area by its middle.
		const double x = each.area ? (each.min[0] + each.max[0]) * 0.5 : each.x;
		const double y = each.area ? (each.min[1] + each.max[1]) * 0.5 : each.y;
		if (!members++) {
			low[0] = high[0] = x;
			low[1] = high[1] = y;
		}
		low[0] = std::min(low[0], x);
		low[1] = std::min(low[1], y);
		high[0] = std::max(high[0], x);
		high[1] = std::max(high[1], y);
	}
	if (members < 2) return false;
	out[0] = (low[0] + high[0]) * 0.5;
	out[1] = (low[1] + high[1]) * 0.5;
	return true;
}

bool mission_yaw_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		double delta, float snap, uint64_t gesture, std::vector<Edit> &out, bool stick, const ViewportDevice *device) {
	out.clear();
	if (grabbed >= pressed.size() || pressed[grabbed].area) return false;
	// The grabbed one's heading snapped (whole degrees when free); the others turn as far.
	const double turned = snapped(double(pressed[grabbed].yaw) + delta, snap >= 1.0f ? snap : 1.0f);
	const double by = turned - double(pressed[grabbed].yaw);
	// Several (entities and areas) turn about their group's centre: the middle of their box.
	double centre[2] = { 0.0, 0.0 };
	const bool group = mission_turn_centre(pressed, centre);
	const double cx = centre[0], cy = centre[1];
	// A compass heading turns clockwise: (east, north) by `by` degrees is (e cos + n sin, n cos - e sin).
	const double radians = by * io::kRadiansPerDegree, c = std::cos(radians), s = std::sin(radians);
	// An area's box keeps the file's axes: an odd number of quarter turns swaps its extents.
	const long quarters = std::lround(by / 90.0);
	const bool swaps = std::fabs(by - double(quarters) * 90.0) < 1e-9 && quarters % 2 != 0;
	for (const MissionPressed &each : pressed) {
		if (each.area) {
			if (!group) continue;
			// Its middle carried round the centre, its extents as they stand (swapped by a quarter turn).
			const double mx = (each.min[0] + each.max[0]) * 0.5, my = (each.min[1] + each.max[1]) * 0.5;
			const double half_e = (each.max[0] - each.min[0]) * 0.5, half_n = (each.max[1] - each.min[1]) * 0.5;
			const double e = mx - cx, n = my - cy;
			const double x = cx + e * c + n * s, y = cy + n * c - e * s;
			const double he = swaps ? half_n : half_e, hn = swaps ? half_e : half_n;
			set_position(document, each.record, "x_min", x - he, gesture, out);
			set_position(document, each.record, "x_max", x + he, gesture, out);
			set_position(document, each.record, "y_min", y - hn, gesture, out);
			set_position(document, each.record, "y_max", y + hn, gesture, out);
			continue;
		}
		set_yaw(document, each.record, wrapped(double(each.yaw) + by), gesture, out);
		if (!group) continue;
		const double e = each.x - cx, n = each.y - cy;
		const double x = cx + e * c + n * s, y = cy + n * c - e * s;
		set_position(document, each.record, "x", x, gesture, out);
		set_position(document, each.record, "y", y, gesture, out);
		double was = 0.0, now = 0.0;
		if (stick && device && device->ground_at(each.x, each.y, was) && device->ground_at(x, y, now))
			set_position(document, each.record, "z", now + (each.z - was), gesture, out);
		else
			set_position(document, each.record, "z", each.z, gesture, out);
	}
	return true;
}

void mission_restore_edits(const Document &document, const std::vector<MissionPressed> &pressed, uint64_t gesture,
		std::vector<Edit> &out) {
	out.clear();
	for (const MissionPressed &each : pressed) {
		if (each.area) {
			set_position(document, each.record, "x_min", each.min[0], gesture, out);
			set_position(document, each.record, "x_max", each.max[0], gesture, out);
			set_position(document, each.record, "y_min", each.min[1], gesture, out);
			set_position(document, each.record, "y_max", each.max[1], gesture, out);
			continue;
		}
		set_position(document, each.record, "x", each.x, gesture, out);
		set_position(document, each.record, "y", each.y, gesture, out);
		set_position(document, each.record, "z", each.z, gesture, out);
		set_yaw(document, each.record, wrapped(double(each.yaw)), gesture, out);
	}
}

bool mission_area_edge_edits(const Document &document, const MissionPressed &area, MissionHandle edge, double to,
		float snap, uint64_t gesture, std::vector<Edit> &out) {
	out.clear();
	if (!area.area || !mission_handle_is_edge(edge)) return false;
	const double value = snapped(to, snap);
	switch (edge) {
	case MissionHandle::XMin: set_position(document, area.record, "x_min", std::min(value, area.max[0]), gesture, out); break;
	case MissionHandle::XMax: set_position(document, area.record, "x_max", std::max(value, area.min[0]), gesture, out); break;
	case MissionHandle::YMin: set_position(document, area.record, "y_min", std::min(value, area.max[1]), gesture, out); break;
	case MissionHandle::YMax: set_position(document, area.record, "y_max", std::max(value, area.min[1]), gesture, out); break;
	default: return false;
	}
	return true;
}

} // namespace opennova::editor
