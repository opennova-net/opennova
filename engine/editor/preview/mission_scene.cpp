#include <editor/preview/mission_scene.h>

#include <algorithm>
#include <cmath>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/viewport_device.h>

namespace opennova::editor {

namespace {

constexpr const char *kPoolTokens[] = { "item", "building", "marker", "organic" };

bool same_transform(const MissionEntityMark &a, const MissionEntityMark &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z && a.pitch == b.pitch && a.yaw == b.yaw && a.roll == b.roll;
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
	return a.terrain == b.terrain && a.terrain_tile == b.terrain_tile && a.environment == b.environment &&
			a.start_time == b.start_time && a.minutes_per_day == b.minutes_per_day &&
			a.water_override == b.water_override && a.fog_override == b.fog_override;
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
			if (was.item != mark.item) delta.reshaped = true;
			else if (!same_transform(was, mark)) delta.moved = true;
			else mark.stamp = was.stamp; // as it was: its stamp stands
			if (was.team != mark.team) delta.overlays = true;
		}
		if (areas_.size() != areas_were.size() || paths_.size() != paths_were.size() || changes.reordered)
			delta.overlays = true;
		for (const MissionAreaMark &mark : areas_) {
			const MissionAreaMark *was = nullptr;
			for (const MissionAreaMark &each : areas_were)
				if (each.row == mark.row) was = &each;
			if (!was || was->zone != mark.zone || was->flags != mark.flags ||
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
			const bool item = held.item != read.item, moved = !same_transform(held, read), team = held.team != read.team;
			read.at = mission_scene_point(read.x, read.y, read.z);
			read.stamp = item || moved ? uint32_t(serial_ + 1) : held.stamp;
			held = read;
			delta.reshaped = delta.reshaped || item;
			delta.moved = delta.moved || moved;
			delta.overlays = delta.overlays || team;
			touched = true;
		} else if (const auto area_found = area_rows_.find(row); area_found != area_rows_.end()) {
			MissionAreaMark read;
			if (!source.area(row, read)) continue;
			++rows_read_;
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

size_t MissionScene::count(MissionPool pool) const {
	return size_t(std::count_if(entities_.begin(), entities_.end(),
			[pool](const MissionEntityMark &mark) { return mark.pool == pool; }));
}

// --- marks ---------------------------------------------------------------------------------------

std::vector<MissionMark> mission_marks(const MissionScene &scene, const MissionViewportOptions &options,
		const OrbitCamera &camera, int width, int height, const ViewportDevice *device) {
	std::vector<MissionMark> marks;
	marks.reserve(scene.entities().size() + scene.areas().size());
	const auto place = [&](MissionMark &mark, bool wanted) {
		const bool projected = camera.project(mark.at, width, height, mark.x, mark.y, &mark.depth);
		mark.shown = wanted && projected && mark.x >= 0.0f && mark.y >= 0.0f && mark.x <= float(width) &&
				mark.y <= float(height) && (options.mark_range <= 0.0f || mark.depth <= options.mark_range);
	};
	for (size_t i = 0; i < scene.entities().size(); ++i) {
		const MissionEntityMark &entity = scene.entities()[i];
		MissionMark mark;
		mark.record = NodeAddress{ entity.row, entity.kind, 0 };
		mark.kind = mission_pool_token(entity.pool);
		mark.at = entity.at;
		mark.entity = int(i);
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

int pick_mission_mark(const std::vector<MissionMark> &marks, float x, float y, float slop) {
	int best = -1;
	for (size_t i = 0; i < marks.size(); ++i) {
		const MissionMark &mark = marks[i];
		if (!mark.shown || std::fabs(mark.x - x) > slop || std::fabs(mark.y - y) > slop) continue;
		if (best < 0 || mark.depth < marks[size_t(best)].depth) best = int(i);
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
