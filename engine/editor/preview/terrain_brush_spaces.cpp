#include <editor/preview/terrain_brush_spaces.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/viewport_device.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/terrain_raycast.h>
#include <runtime/world/collision.h>

namespace opennova::editor {

namespace {

int32_t to_1616(double metres) {
	const double fixed = std::llround(metres * 65536.0);
	return fixed > 2147483647.0 ? INT32_MAX : fixed < -2147483648.0 ? INT32_MIN : static_cast<int32_t>(fixed);
}

// A sample of the height field for the terrain raycast, as the Shell's device takes one over its baked heights
// (godot/src/terrain TerrainData::raycast_terrain's substrate): at (x, z) of the presentation's plane (the mission's x and
// -y), through the editor's coords (outside the authored grid no terrain, an empty sector the height-0 floor), the point
// sample the height there, the bilinear the shared core's.
terrain::TerrainRaycastSample editor_ray_sample(const terrain::TerrainHeightField &field, int32_t x, int32_t z, bool bilinear) {
	const double wx = x / 65536.0, wz = z / 65536.0;
	terrain::TerrainRaycastSample out;
	const terrain::CoordsResult<double> r =
			terrain::coords_world_to_source<double>(field.layout, wx, wz, terrain::coords_editor_options());
	if (!r.valid) {
		const int grid_x = r.sector_sx - field.layout.origin_x, grid_z = r.sector_sz - field.layout.origin_y;
		const bool within = grid_z >= 0 && grid_z < field.layout.sector_rows && grid_x >= 0 && grid_x < field.layout.sector_count;
		out.kind = within ? terrain::TerrainRaycastSample::kEmpty : terrain::TerrainRaycastSample::kOutOfExtent;
		return out;
	}
	const float height = bilinear ? terrain::height_field_height_world_bilinear(field, float(wx), float(wz))
	                              : terrain::height_field_height_world(field, float(wx), float(wz));
	out.height_1616 = static_cast<int32_t>(std::llround(double(height) * 65536.0));
	return out;
}
terrain::TerrainRaycastSample editor_ray_point(void *ctx, int32_t x, int32_t z) {
	return editor_ray_sample(*static_cast<const terrain::TerrainHeightField *>(ctx), x, z, false);
}
terrain::TerrainRaycastSample editor_ray_bilinear(void *ctx, int32_t x, int32_t z) {
	return editor_ray_sample(*static_cast<const terrain::TerrainHeightField *>(ctx), x, z, true);
}

// Where the segment from `from` to `to` (mission metres) first meets the height field, as the Shell's device meets its
// terrain: the retail segment raycast [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80; Terrain_RaycastHeightmapHiRes_0 @
// 0x60e710; runtime/terrain_query/terrain_raycast.h] over the samples above, the segment clipped to the authored grid
// and a height band first so its 16.16 words hold. False where it meets none.
bool editor_terrain_ray(const terrain::TerrainHeightField &field, const double from[3], const double to[3], double hit[3]) {
	if (!field.layout.sector_grid) return false;
	// The presentation's axes: x, the height, the mission's -y.
	const double a[3] = {from[0], from[2], -from[1]}, b[3] = {to[0], to[2], -to[1]};
	const double size = terrain::COORDS_SECTOR_SIZE;
	const double min_x = double(field.layout.origin_x) * size, max_x = min_x + double(field.layout.sector_count) * size;
	const double min_z = double(field.layout.origin_y) * size, max_z = min_z + double(field.layout.sector_rows) * size;
	constexpr double kHeightBand = 30000.0;
	const double d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	double t0 = 0.0, t1 = 1.0;
	const auto clip = [&t0, &t1](double origin, double delta, double low, double high) {
		if (delta == 0.0) return origin >= low && origin <= high;
		const double ta = (low - origin) / delta, tb = (high - origin) / delta;
		t0 = std::max(t0, std::min(ta, tb));
		t1 = std::min(t1, std::max(ta, tb));
		return true;
	};
	if (!clip(a[0], d[0], min_x, max_x) || !clip(a[2], d[2], min_z, max_z) || !clip(a[1], d[1], -kHeightBand, kHeightBand) ||
	    t1 < t0)
		return false;
	const int32_t start[3] = {to_1616(a[0] + d[0] * t0), to_1616(a[2] + d[2] * t0), to_1616(a[1] + d[1] * t0)};
	const int32_t end[3] = {to_1616(a[0] + d[0] * t1), to_1616(a[2] + d[2] * t1), to_1616(a[1] + d[1] * t1)};
	terrain::TerrainRaycastSampler sampler;
	sampler.point = &editor_ray_point;
	sampler.bilinear = &editor_ray_bilinear;
	sampler.ctx = const_cast<terrain::TerrainHeightField *>(&field);
	int32_t met[3] = {0, 0, 0};
	if (!terrain::terrain_raycast_refined(sampler, start, end, met)) return false;
	hit[0] = met[0] / 65536.0;
	hit[1] = -met[1] / 65536.0;
	hit[2] = met[2] / 65536.0;
	return true;
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
	double hit[3] = {0.0, 0.0, 0.0};
	if (!editor_terrain_ray(*field, start, end, hit)) return false;
	at = TerrainPoint{hit[0], hit[1]};
	if (!mission_ground_height(*ground_, at, height)) height = hit[2];
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
