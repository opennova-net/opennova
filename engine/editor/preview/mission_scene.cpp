#include <editor/preview/mission_scene.h>

#include <algorithm>
#include <cmath>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/viewport_device.h>

namespace opennova::editor {

namespace {

constexpr const char *kPoolTokens[] = { "item", "building", "marker", "organic" };

bool same_transform(const MissionEntityMark &a, const MissionEntityMark &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z && a.pitch == b.pitch && a.yaw == b.yaw && a.roll == b.roll;
}

// What a person's spawn pose reads of its record beside its item and attributes.
bool same_pose_fields(const MissionEntityMark &a, const MissionEntityMark &b) {
	return a.route == b.route && a.ssn == b.ssn;
}

bool pool_shown(const MissionViewportOptions &options, MissionPool pool) {
	switch (pool) {
	case MissionPool::Item: return options.items;
	case MissionPool::Building: return options.buildings;
	case MissionPool::Marker: return options.markers;
	case MissionPool::Organic: return options.organics;
	}
	return true;
}

} // namespace

const char *mission_pool_token(MissionPool pool) {
	return kPoolTokens[static_cast<size_t>(pool) < 4 ? static_cast<size_t>(pool) : 0];
}

bool operator==(const MissionSceneHeader &a, const MissionSceneHeader &b) {
	return a.terrain == b.terrain && a.tile_set == b.tile_set && a.environment == b.environment &&
			a.start_time == b.start_time && a.minutes_per_day == b.minutes_per_day && a.attrib_flags == b.attrib_flags &&
			a.water_override == b.water_override && a.fog_override == b.fog_override && a.water_murk == b.water_murk &&
			a.wind_speed == b.wind_speed && a.wind_direction == b.wind_direction &&
			std::equal(std::begin(a.fog_color), std::end(a.fog_color), std::begin(b.fog_color)) &&
			std::equal(std::begin(a.water_color), std::end(a.water_color), std::begin(b.water_color));
}

PreviewVec3 mission_scene_point(double x, double y, double z) {
	const double mission[3] = { x, y, z };
	return mission_to_preview(mission);
}

void MissionScene::clear() {
	header_ = MissionSceneHeader();
	entities_.clear();
	areas_.clear();
	paths_.clear();
	index_();
	++serial_;
}

void MissionScene::index_() {
	entity_rows_.clear();
	area_rows_.clear();
	path_rows_.clear();
	for (size_t i = 0; i < entities_.size(); ++i) {
		MissionEntityMark &mark = entities_[i];
		mark.at = mission_scene_point(mark.x, mark.y, mark.z);
		entity_rows_[mark.row] = i;
	}
	for (size_t i = 0; i < areas_.size(); ++i) area_rows_[areas_[i].row] = i;
	for (size_t i = 0; i < paths_.size(); ++i) path_rows_[paths_[i].row] = i;
}

void MissionScene::read(const MissionSceneSource &source) {
	++serial_;
	header_ = MissionSceneHeader();
	rows_read_ += source.header(header_) ? 1 : 0;
	entities_.clear();
	areas_.clear();
	paths_.clear();
	source.entities(entities_);
	source.areas(areas_);
	source.paths(paths_);
	rows_read_ += entities_.size() + areas_.size() + paths_.size();
	for (MissionEntityMark &mark : entities_) mark.stamp = uint32_t(serial_);
	index_();
}

MissionSceneDelta MissionScene::patch(const RowChanges &changes, const MissionSceneSource &source) {
	MissionSceneDelta delta;
	if (changes.reshapes()) {
		// A row added or removed, or the rows in another order: everything read again, the delta
		// from what differs by row (the device keys by row: an order alone changes nothing it draws).
		const std::vector<MissionEntityMark> were = entities_;
		const std::vector<MissionAreaMark> areas_were = areas_;
		const std::vector<MissionPathMark> paths_were = paths_;
		const MissionSceneHeader header_was = header_;
		std::unordered_map<NodeId, const MissionEntityMark *> by_row;
		for (const MissionEntityMark &mark : were) by_row[mark.row] = &mark;
		read(source);
		delta.header = header_ != header_was;
		if (entities_.size() != were.size()) delta.reshaped = true;
		for (MissionEntityMark &mark : entities_) {
			const auto found = by_row.find(mark.row);
			if (found == by_row.end()) {
				delta.reshaped = true;
				continue;
			}
			const MissionEntityMark &was = *found->second;
			if (!mission_entity_places_alike(was, mark)) delta.reshaped = true;
			else if (!same_transform(was, mark)) delta.moved = true;
			else mark.stamp = was.stamp; // as it was: its stamp stands
			if (!same_pose_fields(was, mark)) delta.posed = true;
			if (was.team != mark.team) delta.overlays = true;
		}
		if (areas_.size() != areas_were.size() || paths_.size() != paths_were.size() || changes.reordered)
			delta.overlays = true;
		for (const MissionAreaMark &mark : areas_) {
			const MissionAreaMark *was = nullptr;
			for (const MissionAreaMark &each : areas_were)
				if (each.row == mark.row) was = &each;
			if (!was || was->zone != mark.zone || was->constrains_z != mark.constrains_z ||
					!std::equal(std::begin(was->min), std::end(was->min), std::begin(mark.min)) ||
					!std::equal(std::begin(was->max), std::end(was->max), std::begin(mark.max)))
				delta.overlays = true;
		}
		for (const MissionPathMark &mark : paths_) {
			const MissionPathMark *was = nullptr;
			for (const MissionPathMark &each : paths_were)
				if (each.row == mark.row) was = &each;
			if (!was || was->flags != mark.flags || was->stops != mark.stops) delta.overlays = true;
		}
		return delta;
	}
	bool touched = false;
	for (const NodeId row : changes.changed) {
		if (const auto found = entity_rows_.find(row); found != entity_rows_.end()) {
			MissionEntityMark read;
			if (!source.entity(row, read)) continue;
			++rows_read_;
			MissionEntityMark &held = entities_[found->second];
			const bool item = !mission_entity_places_alike(held, read), moved = !same_transform(held, read),
					   team = held.team != read.team, posed = !same_pose_fields(held, read);
			read.at = mission_scene_point(read.x, read.y, read.z);
			read.stamp = item || moved ? uint32_t(serial_ + 1) : held.stamp;
			// Its place in its pool is the whole read's (a row's read alone does not know it).
			read.index = held.index;
			held = read;
			delta.reshaped = delta.reshaped || item;
			delta.moved = delta.moved || moved;
			delta.posed = delta.posed || posed;
			delta.overlays = delta.overlays || team;
			touched = true;
		} else if (const auto area_found = area_rows_.find(row); area_found != area_rows_.end()) {
			MissionAreaMark read;
			if (!source.area(row, read)) continue;
			++rows_read_;
			read.index = areas_[area_found->second].index;
			areas_[area_found->second] = read;
			delta.overlays = true;
			touched = true;
		} else if (const auto path_found = path_rows_.find(row); path_found != path_rows_.end()) {
			MissionPathMark read;
			++rows_read_;
			if (source.path(row, read)) {
				paths_[path_found->second] = read;
			} else {
				// Its last stop gone: no longer a path the overlays draw.
				paths_.erase(paths_.begin() + std::ptrdiff_t(path_found->second));
				index_();
			}
			delta.overlays = true;
			touched = true;
		} else if (source.header_row(row)) {
			MissionSceneHeader read;
			++rows_read_;
			if (source.header(read) && read != header_) {
				header_ = read;
				delta.header = true;
				touched = true;
			}
		} else {
			// A path that gained its first stop is listed now; any other row (an event) the picture
			// does not read.
			MissionPathMark read;
			if (!source.path(row, read)) continue;
			++rows_read_;
			paths_.push_back(read);
			std::sort(paths_.begin(), paths_.end(),
					[](const MissionPathMark &a, const MissionPathMark &b) { return a.index < b.index; });
			index_();
			delta.overlays = true;
			touched = true;
		}
	}
	if (touched) ++serial_;
	return delta;
}

const MissionEntityMark *MissionScene::entity(NodeId row) const {
	const auto found = entity_rows_.find(row);
	return found == entity_rows_.end() ? nullptr : &entities_[found->second];
}

const MissionAreaMark *MissionScene::area(NodeId row) const {
	const auto found = area_rows_.find(row);
	return found == area_rows_.end() ? nullptr : &areas_[found->second];
}

const MissionPathMark *MissionScene::path(NodeId row) const {
	const auto found = path_rows_.find(row);
	return found == path_rows_.end() ? nullptr : &paths_[found->second];
}

int MissionScene::mark_index(NodeId row) const {
	if (const auto found = entity_rows_.find(row); found != entity_rows_.end()) return int(found->second);
	if (const auto found = area_rows_.find(row); found != area_rows_.end()) return int(entities_.size() + found->second);
	return -1;
}

size_t MissionScene::count(MissionPool pool) const {
	return size_t(std::count_if(entities_.begin(), entities_.end(),
			[pool](const MissionEntityMark &mark) { return mark.pool == pool; }));
}

// --- marks ---------------------------------------------------------------------------------------

std::vector<MissionMark> mission_marks(const MissionScene &scene, const MissionViewportOptions &options,
		const OrbitCamera &camera, int width, int height, const ViewportDevice *device, const MissionPickRadii *radii) {
	std::vector<MissionMark> marks;
	marks.reserve(scene.entities().size() + scene.areas().size());
	const auto place = [&](MissionMark &mark, bool wanted) {
		const bool projected = camera.project(mark.at, width, height, mark.x, mark.y, &mark.depth);
		mark.pickable = wanted && (options.mark_range <= 0.0f || mark.depth <= options.mark_range);
		mark.shown = mark.pickable && projected && mark.x >= 0.0f && mark.y >= 0.0f && mark.x <= float(width) &&
				mark.y <= float(height);
	};
	for (size_t i = 0; i < scene.entities().size(); ++i) {
		const MissionEntityMark &entity = scene.entities()[i];
		MissionMark mark;
		mark.record = NodeAddress{ entity.row, entity.kind, 0 };
		mark.kind = mission_pool_token(entity.pool);
		mark.at = entity.at;
		mark.entity = int(i);
		if (radii)
			if (const auto radius = radii->find(entity.item); radius != radii->end()) mark.radius = radius->second;
		place(mark, pool_shown(options, entity.pool));
		marks.push_back(mark);
	}
	for (size_t i = 0; i < scene.areas().size(); ++i) {
		const MissionAreaMark &area = scene.areas()[i];
		MissionMark mark;
		mark.record = NodeAddress{ area.row, area.kind, 0 };
		mark.kind = "area";
		const double x = (area.min[0] + area.max[0]) * 0.5, y = (area.min[1] + area.max[1]) * 0.5;
		double z = area.min[2];
		double ground = 0.0;
		if (device && device->ground_at(x, y, ground)) z = ground;
		mark.at = mission_scene_point(x, y, z);
		mark.area = int(i);
		place(mark, options.areas);
		marks.push_back(mark);
	}
	return marks;
}

int pick_mission_mark(const std::vector<MissionMark> &marks, const OrbitCamera &camera, int width, int height, float x,
		float y, const ViewportDevice *device, MissionPick by, float slop) {
	// A glyph: the front-most anchor within the slop.
	int best = -1;
	for (size_t i = 0; i < marks.size(); ++i) {
		const MissionMark &mark = marks[i];
		if (!mark.shown || std::fabs(mark.x - x) > slop || std::fabs(mark.y - y) > slop) continue;
		if (best < 0 || mark.depth < marks[size_t(best)].depth) best = int(i);
	}
	if (best >= 0) return best;
	PreviewVec3 from, direction;
	if (!camera.ray(x, y, width, height, from, direction)) return -1;
	const double a = double(direction.x) * direction.x + double(direction.y) * direction.y + double(direction.z) * direction.z;
	if (!(a > 0.0)) return -1;
	// What the pointer is over, as the device draws it: the first surface the ray meets (the game's own
	// choice among the entities a ray's broad phase passes is a face hit, world-wac-ai-re.md "Broad
	// phase" and the segment clip), the ray as a segment of the mission from the eye as far as a pick
	// reaches.
	if (device) {
		const double reach = kMissionPickReach / std::sqrt(a);
		const PreviewVec3 far{ float(double(from.x) + double(direction.x) * reach),
			float(double(from.y) + double(direction.y) * reach), float(double(from.z) + double(direction.z) * reach) };
		double start[3], end[3];
		preview_to_mission(from, start);
		preview_to_mission(far, end);
		const ViewportRayHit hit = device->ray_between(start, end);
		if (hit.met != ViewportRayHit::Met::Unknown) {
			if (hit.met != ViewportRayHit::Met::Record) return -1;
			for (size_t i = 0; i < marks.size(); ++i)
				if (marks[i].entity >= 0 && marks[i].record.row == hit.row) return marks[i].pickable ? int(i) : -1;
			return -1;
		}
	}
	// No device to say: a press takes nothing past the glyph (it would move what the pointer may not be
	// over); a click the smallest sphere the ray passes through in front of the eye, the eye inside it or
	// not (a small entity nested in a large one's sphere, a jeep beside a hangar, is the small one's), the
	// nearer of two alike.
	if (by == MissionPick::Press) return -1;
	double best_radius = 0.0, best_along = 0.0;
	for (size_t i = 0; i < marks.size(); ++i) {
		const MissionMark &mark = marks[i];
		if (!mark.pickable || !(mark.radius > 0.0f)) continue;
		const double ox = double(mark.at.x) - from.x, oy = double(mark.at.y) - from.y, oz = double(mark.at.z) - from.z;
		const double along = (ox * direction.x + oy * direction.y + oz * direction.z) / a;
		const double radius = double(mark.radius);
		double gap;
		if (along >= 0.0) {
			const double px = ox - along * direction.x, py = oy - along * direction.y, pz = oz - along * direction.z;
			gap = std::sqrt(px * px + py * py + pz * pz);
		} else {
			// Its middle behind the eye: taken only where the eye stands inside it.
			gap = std::sqrt(ox * ox + oy * oy + oz * oz);
		}
		if (!(gap < radius)) continue;
		if (best < 0 || radius < best_radius || (radius == best_radius && along < best_along)) {
			best = int(i);
			best_radius = radius;
			best_along = along;
		}
	}
	return best;
}

std::vector<NodeAddress> mission_box_records(const std::vector<MissionMark> &marks, CanvasPoint a, CanvasPoint b) {
	const float left = std::min(a.x, b.x), right = std::max(a.x, b.x);
	const float top = std::min(a.y, b.y), bottom = std::max(a.y, b.y);
	std::vector<const MissionMark *> inside;
	for (const MissionMark &mark : marks)
		if (mark.shown && mark.x >= left && mark.x <= right && mark.y >= top && mark.y <= bottom) inside.push_back(&mark);
	std::stable_sort(inside.begin(), inside.end(),
			[](const MissionMark *p, const MissionMark *q) { return p->depth < q->depth; });
	std::vector<NodeAddress> out;
	out.reserve(inside.size());
	for (const MissionMark *mark : inside) out.push_back(mark->record);
	return out;
}

} // namespace opennova::editor
