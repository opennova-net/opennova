#include <editor/preview/model_overlay.h>

#include <algorithm>
#include <cmath>

#include <base/io/strutil.h>
#include <editor/documents/model_document.h>
#include <formats/threedi/threedi_panm_pose.h>

namespace opennova::editor {

namespace {

using threedi::ThreediMatrix4x4;

// p * M (the row-vector convention of the part matrices), a point or a direction.
void transform(const ThreediMatrix4x4 &m, const float in[3], bool point, float out[3]) {
	for (int c = 0; c < 3; ++c)
		out[c] = in[0] * m.m[c] + in[1] * m.m[4 + c] + in[2] * m.m[8 + c] + (point ? m.m[12 + c] : 0.0f);
}

PreviewVec3 normalized(PreviewVec3 v) {
	const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (length > 0.0f) {
		v.x /= length;
		v.y /= length;
		v.z /= length;
	}
	return v;
}

// A model-space point (and direction) through part `part`'s posed matrix when there is one,
// into the preview's space.
void place(const std::vector<ThreediMatrix4x4> &parts, int part, const float at[3], const float *direction,
           ModelOverlay &out) {
	float posed[3] = {at[0], at[1], at[2]};
	float axis[3] = {};
	if (direction) std::copy(direction, direction + 3, axis);
	if (part >= 0 && static_cast<size_t>(part) < parts.size()) {
		transform(parts[size_t(part)], at, true, posed);
		if (direction) transform(parts[size_t(part)], direction, false, axis);
		out.part = part;
	}
	out.at = preview_from_model(posed);
	if (direction) {
		out.has_direction = true;
		out.direction = normalized(preview_from_model(axis));
	}
}

} // namespace

const char *model_overlay_kind_token(ModelOverlayKind kind) {
	switch (kind) {
	case ModelOverlayKind::UserPoint: return "user_point";
	case ModelOverlayKind::Light: return "light";
	case ModelOverlayKind::Pivot: return "pivot";
	}
	return "user_point";
}

std::vector<ModelOverlay> model_overlays(const threedi::Threedi3di3 &model, int lod, uint32_t time_ms,
                                         const int32_t bus[96], const ModelOverlayOptions &options) {
	std::vector<ModelOverlay> out;
	// The first level's pose carries the attachments; the drawn level's, its lights and
	// pivots (the parts the device draws).
	std::vector<ThreediMatrix4x4> first, drawn;
	const bool first_live = threedi::threedi_panm_lod_has_live(model, 0) &&
	                        threedi::threedi_panm_pose_parts(model, 0, time_ms, bus, first, nullptr);
	if (lod >= 0) threedi::threedi_panm_pose_parts(model, lod, time_ms, bus, drawn, nullptr);
	if (options.user_points) {
		for (size_t i = 0; i < model.user_point_count; ++i) {
			const threedi::ThreediUserPoint &point = model.user_points[i];
			float at[3], axis[3];
			threedi::threedi_user_point_position(&point, at);
			threedi::threedi_user_point_direction(&point, axis);
			ModelOverlay row;
			row.kind = ModelOverlayKind::UserPoint;
			row.index = int(i);
			row.name = strutil::fixed_string(point.name, sizeof(point.name));
			place(first_live ? first : std::vector<ThreediMatrix4x4>(), point.subobject_index, at, axis, row);
			out.push_back(std::move(row));
		}
	}
	if (options.lights) {
		for (size_t i = 0; i < model.light_count; ++i) {
			const threedi::ThreediLight &light = model.lights[i];
			ModelOverlay row;
			row.kind = ModelOverlayKind::Light;
			row.index = int(i);
			row.name = "Light " + std::to_string(i);
			const bool spot = (light.flags & threedi::THREEDI_LIGHT_FLAG_TYPE_TARGET) != 0;
			const float axis[3] = {light.rotation[0], light.rotation[1], light.rotation[2]};
			place(light.subobj_index > 0 ? drawn : std::vector<ThreediMatrix4x4>(), light.subobj_index, light.offset,
			      spot ? axis : nullptr, row);
			row.radius = light.atten_end;
			row.cone = spot ? std::acos(std::clamp(light.rotation[3], -1.0f, 1.0f)) : 0.0f;
			row.color = (uint32_t(light.color_start[2]) << 16) | (uint32_t(light.color_start[1]) << 8) | light.color_start[0];
			out.push_back(std::move(row));
		}
	}
	if (options.pivots && model.lods && lod >= 0 && size_t(lod) < model.lod_count) {
		const threedi::ThreediLod &level = model.lods[lod];
		for (size_t p = 0; p < level.render_object_count; ++p) {
			ModelOverlay row;
			row.kind = ModelOverlayKind::Pivot;
			row.index = int(p);
			row.name = "Part " + std::to_string(p);
			place(drawn, int(p), level.render_objects[p].abs, nullptr, row);
			out.push_back(std::move(row));
		}
	}
	return out;
}

threedi::ThreediMatrix4x4 model_overlay_pose(const threedi::Threedi3di3 &model, const ModelOverlay &overlay, int lod,
                                             uint32_t time_ms, const int32_t bus[96]) {
	ThreediMatrix4x4 out;
	threedi::threedi_mat4_identity(&out);
	if (overlay.part < 0) return out;
	const int level = overlay.kind == ModelOverlayKind::UserPoint ? 0 : lod;
	std::vector<ThreediMatrix4x4> parts;
	if (level >= 0 && threedi::threedi_panm_pose_parts(model, level, time_ms, bus, parts, nullptr) &&
	    size_t(overlay.part) < parts.size())
		out = parts[size_t(overlay.part)];
	return out;
}

int pick_model_overlay(const std::vector<ModelOverlay> &overlays, const OrbitCamera &camera, int width, int height,
                       float x, float y, float slop) {
	int best = -1;
	float best_depth = 0.0f;
	for (size_t i = 0; i < overlays.size(); ++i) {
		float sx = 0.0f, sy = 0.0f, depth = 0.0f;
		if (!camera.project(overlays[i].at, width, height, sx, sy, &depth)) continue;
		if (std::fabs(sx - x) > slop || std::fabs(sy - y) > slop) continue;
		if (best < 0 || depth < best_depth) {
			best = int(i);
			best_depth = depth;
		}
	}
	return best;
}

namespace {

// The identity of the record at `index` of a list of identities (0 past its end).
NodeId identity_at(const std::vector<RecordIds> &ids, int index) {
	return index >= 0 && size_t(index) < ids.size() ? ids[size_t(index)].id : 0;
}

// The place of an identity in a list of identities (-1 when it is not there).
int place_of(const std::vector<RecordIds> &ids, NodeId id) {
	for (size_t i = 0; i < ids.size(); ++i)
		if (ids[i].id == id) return int(i);
	return -1;
}

} // namespace

NodeAddress model_overlay_record(const ModelDocument &document, const ModelOverlay &overlay, int lod) {
	const ModelRow *row = document.model_row();
	if (!row) return NodeAddress();
	const auto at = [&](size_t list, NodeKind kind) {
		const NodeId id = list < row->ids.lists.size() ? identity_at(row->ids.lists[list], overlay.index) : 0;
		return id ? NodeAddress{row->id, kind, id} : NodeAddress();
	};
	switch (overlay.kind) {
	case ModelOverlayKind::UserPoint: return at(kModelUserPoints, node_kind(ModelKind::UserPoint));
	case ModelOverlayKind::Light: return at(kModelLights, node_kind(ModelKind::Light));
	case ModelOverlayKind::Pivot: {
		// A LOD's one list is its part animations.
		if (lod < 0 || row->ids.lists.size() <= kModelLods || size_t(lod) >= row->ids.lists[kModelLods].size())
			return NodeAddress();
		const NodeId id = identity_at(row->ids.lists[kModelLods][size_t(lod)].lists[kModelOwnList], overlay.index);
		return id ? NodeAddress{row->id, node_kind(ModelKind::PartAnimation), id} : NodeAddress();
	}
	}
	return NodeAddress();
}

bool model_overlay_of(const ModelDocument &document, const NodeAddress &record, ModelOverlayKind &kind, int &index) {
	const ModelRow *row = document.model_row();
	if (!row || record.row != row->id || !record.child || row->ids.lists.size() <= kModelFrames) return false;
	const auto find = [&](size_t list, ModelOverlayKind as) {
		const int found = place_of(row->ids.lists[list], record.child);
		if (found < 0) return false;
		kind = as;
		index = found;
		return true;
	};
	if (record.kind == node_kind(ModelKind::UserPoint)) return find(kModelUserPoints, ModelOverlayKind::UserPoint);
	if (record.kind == node_kind(ModelKind::Light)) return find(kModelLights, ModelOverlayKind::Light);
	if (record.kind == node_kind(ModelKind::PartAnimation)) {
		for (const RecordIds &level : row->ids.lists[kModelLods]) {
			const int found = place_of(level.lists[kModelOwnList], record.child);
			if (found < 0) continue;
			kind = ModelOverlayKind::Pivot;
			index = found;
			return true;
		}
	}
	return false;
}

} // namespace opennova::editor
