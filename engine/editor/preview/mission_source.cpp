#include <editor/preview/mission_source.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_reads.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

namespace opennova::editor {

namespace {

MissionPool pool_of(MissionKind kind) {
	switch (kind) {
	case MissionKind::Building: return MissionPool::Building;
	case MissionKind::Marker: return MissionPool::Marker;
	case MissionKind::Organic: return MissionPool::Organic;
	default: return MissionPool::Item;
	}
}

MissionEntityMark mark_of(const MissionEntityRead &read) {
	MissionEntityMark mark;
	mark.row = read.address.row;
	mark.kind = node_kind(read.pool);
	mark.pool = pool_of(read.pool);
	mark.item = read.item_id;
	mark.x = read.x;
	mark.y = read.y;
	mark.z = read.z;
	mark.pitch = read.pitch;
	mark.yaw = read.yaw;
	mark.roll = read.roll;
	mark.team = read.team;
	mark.group = read.group;
	mark.attributes = read.attributes;
	mark.route = read.waypoint_id;
	mark.ssn = read.ssn;
	return mark;
}

MissionAreaMark mark_of(const MissionAreaRead &read) {
	MissionAreaMark mark;
	mark.row = read.address.row;
	mark.kind = read.address.kind;
	mark.zone = read.id;
	mark.min[0] = read.x_min;
	mark.max[0] = read.x_max;
	mark.min[1] = read.y_min;
	mark.max[1] = read.y_max;
	mark.min[2] = read.z_min;
	mark.max[2] = read.z_max;
	mark.constrains_z = read.constrains_z;
	return mark;
}

MissionPathMark mark_of(const MissionPathRead &read) {
	MissionPathMark mark;
	mark.row = read.address.row;
	mark.kind = read.address.kind;
	mark.index = read.number;
	mark.flags = read.flags;
	// Each stop's marker row (0: a stop naming none).
	for (const NodeAddress &marker : read.markers) mark.stops.push_back(marker.row);
	return mark;
}

// The scene's reads over the mission document's typed reads (documents/mission_reads.h).
class DocumentSource final : public MissionSceneSource {
public:
	explicit DocumentSource(const MissionDocument &document) : document_(document) {}

	bool header(MissionSceneHeader &out) const override {
		const MissionRow *row = document_.mission_row();
		if (!row) return false;
		const mission::MissionInfo info = mission::mission_info(row->native);
		out = MissionSceneHeader();
		out.terrain = info.terrain;
		out.tile_set = info.tile_set;
		out.environment = info.environment;
		out.start_time = info.start_time;
		out.minutes_per_day = info.minutes_per_day;
		out.attrib_flags = static_cast<uint32_t>(info.attrib_flags);
		out.water_override = info.water_override;
		out.fog_override = info.fog_override;
		out.water_murk = info.water_murk;
		out.wind_speed = info.wind_speed;
		out.wind_direction = info.wind_direction;
		for (int i = 0; i < 3; ++i) {
			out.fog_color[i] = info.fog_color[i];
			out.water_color[i] = info.water_color[i];
		}
		return true;
	}
	bool header_row(NodeId row) const override {
		const MissionRow *mission = document_.mission_row();
		return mission && mission->id == row;
	}
	void entities(std::vector<MissionEntityMark> &out) const override {
		out.clear();
		// Each entity's place in its pool: the rows come pool by pool in the writer's order.
		int indexes[4] = { 0, 0, 0, 0 };
		for (const MissionEntityRead &read : mission_entities(document_)) {
			MissionEntityMark mark = mark_of(read);
			mark.index = indexes[static_cast<size_t>(mark.pool)]++;
			out.push_back(std::move(mark));
		}
	}
	void areas(std::vector<MissionAreaMark> &out) const override {
		out.clear();
		int index = 0;
		for (const MissionAreaRead &read : mission_areas(document_)) {
			MissionAreaMark mark = mark_of(read);
			mark.index = index++;
			out.push_back(std::move(mark));
		}
	}
	void paths(std::vector<MissionPathMark> &out) const override {
		out.clear();
		for (const MissionPathRead &read : mission_paths(document_)) out.push_back(mark_of(read));
	}
	bool entity(NodeId row, MissionEntityMark &out) const override {
		MissionEntityRead read;
		if (!mission_entity(document_, document_.address_of(row), read)) return false;
		out = mark_of(read);
		return true;
	}
	bool area(NodeId row, MissionAreaMark &out) const override {
		MissionAreaRead read;
		if (!mission_area(document_, document_.address_of(row), read)) return false;
		out = mark_of(read);
		return true;
	}
	bool path(NodeId row, MissionPathMark &out) const override {
		MissionPathRead read;
		if (!mission_path(document_, document_.address_of(row), read) || read.stops.empty()) return false;
		out = mark_of(read);
		return true;
	}

private:
	const MissionDocument &document_;
};

} // namespace

std::unique_ptr<MissionSceneSource> mission_scene_source(const Document &document) {
	const auto *mission = dynamic_cast<const MissionDocument *>(&document);
	if (!mission) return nullptr;
	return std::make_unique<DocumentSource>(*mission);
}

} // namespace opennova::editor
