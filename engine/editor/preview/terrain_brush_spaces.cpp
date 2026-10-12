#include <editor/preview/terrain_brush_spaces.h>

#include <cmath>
#include <cstdint>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/viewport_device.h>
#include <runtime/world/collision.h>

namespace opennova::editor {

namespace {

int32_t to_1616(double metres) {
	const double fixed = std::llround(metres * 65536.0);
	return fixed > 2147483647.0 ? INT32_MAX : fixed < -2147483648.0 ? INT32_MIN : static_cast<int32_t>(fixed);
}

} // namespace

bool mission_ground_height(const MissionGround &ground, TerrainPoint at, double &height) {
	const terrain::TerrainHeightField *field = ground.height_field();
	if (!field) return false;
	int32_t fixed = 0;
	if (!world::terrain_column_height(field, to_1616(at.x), to_1616(at.y), fixed)) return false;
	height = double(fixed) / 65536.0;
	return true;
}

bool CameraBrushSpace::pointer_ground(float x, float y, TerrainPoint &at, double &height) const {
	if (context_.device) {
		double out[3] = {0.0, 0.0, 0.0};
		bool terrain = false;
		if (!mission_ground_point(context_, camera_, x, y, 0.0, out, &terrain) || !terrain) return false;
		at = TerrainPoint{out[0], out[1]};
		height = out[2];
		return true;
	}
	const terrain::TerrainHeightField *field = ground_ ? ground_->height_field() : nullptr;
	if (!field) return false;
	PreviewVec3 from, along;
	if (!camera_.ray(x, y, context_.width, context_.height, from, along)) return false;
	const double length = std::sqrt(double(along.x) * along.x + double(along.y) * along.y + double(along.z) * along.z);
	if (!(length > 0.0)) return false;
	const double reach = kMissionPickReach / length;
	const PreviewVec3 far{float(double(from.x) + double(along.x) * reach), float(double(from.y) + double(along.y) * reach),
	                      float(double(from.z) + double(along.z) * reach)};
	double start[3], end[3];
	preview_to_mission(from, start);
	preview_to_mission(far, end);
	const int32_t a[3] = {to_1616(start[0]), to_1616(start[1]), to_1616(start[2])};
	const int32_t b[3] = {to_1616(end[0]), to_1616(end[1]), to_1616(end[2])};
	int32_t hit[3] = {0, 0, 0};
	if (!world::terrain_clip_segment(*field, a, b, hit)) return false;
	at = TerrainPoint{double(hit[0]) / 65536.0, double(hit[1]) / 65536.0};
	if (!mission_ground_height(*ground_, at, height)) height = double(hit[2]) / 65536.0;
	return true;
}

bool CameraBrushSpace::to_picture(TerrainPoint at, double height, float &x, float &y) const {
	const double point[3] = {at.x, at.y, height};
	return camera_.project(mission_to_preview(point), context_.width, context_.height, x, y);
}

double CameraBrushSpace::height(TerrainPoint at) const {
	double height = 0.0;
	if (context_.device && context_.device->ground_at(at.x, at.y, height)) return height;
	if (ground_ && mission_ground_height(*ground_, at, height)) return height;
	return 0.0;
}

bool MapBrushSpace::pointer_ground(float x, float y, TerrainPoint &at, double &height) const {
	view_.unproject(x, y, at.x, at.y);
	height = this->height(at);
	return true;
}

bool MapBrushSpace::to_picture(TerrainPoint at, double height, float &x, float &y) const {
	(void)height;
	view_.project(at.x, at.y, x, y);
	return true;
}

double MapBrushSpace::height(TerrainPoint at) const {
	double height = 0.0;
	return ground_ && mission_ground_height(*ground_, at, height) ? height : 0.0;
}

} // namespace opennova::editor
