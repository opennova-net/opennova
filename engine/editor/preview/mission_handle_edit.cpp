#include <editor/preview/mission_handle_edit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <variant>

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

bool mission_yaw_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		double delta, float snap, uint64_t gesture, std::vector<Edit> &out) {
	out.clear();
	if (grabbed >= pressed.size() || pressed[grabbed].area) return false;
	// The grabbed one's heading snapped (whole degrees when free); the others turn as far.
	const double turned = snapped(double(pressed[grabbed].yaw) + delta, snap >= 1.0f ? snap : 1.0f);
	const double by = turned - double(pressed[grabbed].yaw);
	for (const MissionPressed &each : pressed)
		if (!each.area) set_yaw(document, each.record, wrapped(double(each.yaw) + by), gesture, out);
	return true;
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
