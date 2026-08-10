#include "mission/authoring.h"

#include "def/def.h"

#include <cmath>

namespace opennova::mission::authoring {

namespace {

constexpr double kDegToRad = 0.017453292519943295;

struct Mat3 {
	// Row-major 3x3.
	double m[3][3];

	static Mat3 identity() {
		return Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	}

	static Mat3 rot_x(double deg) {
		const double c = std::cos(deg * kDegToRad), s = std::sin(deg * kDegToRad);
		return Mat3{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
	}

	static Mat3 rot_y(double deg) {
		const double c = std::cos(deg * kDegToRad), s = std::sin(deg * kDegToRad);
		return Mat3{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
	}

	static Mat3 rot_z(double deg) {
		const double c = std::cos(deg * kDegToRad), s = std::sin(deg * kDegToRad);
		return Mat3{{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
	}

	Mat3 operator*(const Mat3 &o) const {
		Mat3 r{};
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
			}
		}
		return r;
	}

	void apply(const float v[3], double out[3]) const {
		for (int i = 0; i < 3; ++i) {
			out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
		}
	}
};

// The entity's render rotation in mission axes: the engine euler builder
// Rz(90 - yaw) * Ry(-pitch) * Rx(roll) [orig:
// Math_BuildFixedPointMatrixFromEulerAngles @ 0x613F40] times the .3di
// import's constant model-forward correction Rz(90). This is the exact
// conjugate of the Godot-side MissionObjectPlacer.bms_to_godot_basis under
// the (x, y, z)bms -> (x, z, -y)godot axis map, which the parity test in
// godot/tests/mission_object_placer_test.gd pins.
Mat3 entity_rotation_bms(int pitch_deg, int yaw_deg, int roll_deg) {
	return Mat3::rot_z(90.0 - static_cast<double>(yaw_deg)) *
	       Mat3::rot_y(-static_cast<double>(pitch_deg)) *
	       Mat3::rot_x(static_cast<double>(roll_deg)) *
	       Mat3::rot_z(90.0);
}

} // namespace

EntityKind entity_kind_for_item_type(int def_item_type) {
	// DefItemType carries the witnessed engine values [orig: ItemDef_ParseProperty
	// @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1]; foliage shares 2 with
	// decoration and object shares 6 with powerup, so one case label covers each pair.
	switch (def_item_type) {
		case DEF_ITEM_TYPE_PERSON: // 3
			return EntityKind::Organic;
		case DEF_ITEM_TYPE_BUILDING:   // 5
		case DEF_ITEM_TYPE_DECORATION: // 2 (= DEF_ITEM_TYPE_FOLIAGE)
			return EntityKind::Building;
		case DEF_ITEM_TYPE_MARKER: // 4
			return EntityKind::Marker;
		default: // vehicle / powerup=object / effect / unset
			return EntityKind::Item;
	}
}

int marker_item_id_for_path(const MissionDocument &doc, size_t path_index) {
	WaypointPath path;
	if (doc.get_waypoint_path(path_index, path) && !path.marker_indices.empty()) {
		EntityRecord existing;
		if (doc.get_entity(EntityKind::Marker, static_cast<size_t>(path.marker_indices.front()), existing) &&
				existing.item_id > 0) {
			return existing.item_id;
		}
	}
	return kWaypointMarkerItemId;
}

EntityTransform bake_ground_transform(const EntityTransform &ground_hit_with_rotation,
                                      const float ground_anchor_bms[3]) {
	const Mat3 rot = entity_rotation_bms(ground_hit_with_rotation.pitch,
	                                     ground_hit_with_rotation.yaw,
	                                     ground_hit_with_rotation.roll);
	double rotated[3];
	rot.apply(ground_anchor_bms, rotated);
	EntityTransform out = ground_hit_with_rotation;
	out.x = static_cast<float>(static_cast<double>(ground_hit_with_rotation.x) - rotated[0]);
	out.y = static_cast<float>(static_cast<double>(ground_hit_with_rotation.y) - rotated[1]);
	out.z = static_cast<float>(static_cast<double>(ground_hit_with_rotation.z) - rotated[2]);
	return out;
}

bool place_entity_grounded(MissionDocument &doc,
                           int item_id,
                           int def_item_type,
                           const float ground_hit_bms[3],
                           const float ground_anchor_bms[3],
                           EntityRecord *out) {
	const EntityKind kind = entity_kind_for_item_type(def_item_type);
	EntityTransform transform;
	transform.x = ground_hit_bms[0];
	transform.y = ground_hit_bms[1];
	transform.z = ground_hit_bms[2];
	// New records are rotation-zero, and the editor's place-time bake is the
	// UNROTATED anchor subtraction; markers are mesh-less and skip it.
	if (kind != EntityKind::Marker) {
		transform.x -= ground_anchor_bms[0];
		transform.y -= ground_anchor_bms[1];
		transform.z -= ground_anchor_bms[2];
	}
	return doc.add_entity(kind, item_id, transform, out);
}

bool move_entity_grounded(MissionDocument &doc,
                          EntityKind kind,
                          size_t index,
                          const float ground_hit_bms[3],
                          const float ground_anchor_bms[3]) {
	EntityRecord record;
	if (!doc.get_entity(kind, index, record)) {
		return false;
	}
	EntityTransform hit = record.transform; // keep pitch/yaw/roll
	hit.x = ground_hit_bms[0];
	hit.y = ground_hit_bms[1];
	hit.z = ground_hit_bms[2];
	if (kind == EntityKind::Marker) {
		return doc.set_entity_transform(kind, index, hit);
	}
	return doc.set_entity_transform(kind, index, bake_ground_transform(hit, ground_anchor_bms));
}

bool add_path_marker_grounded(MissionDocument &doc,
                              size_t path_index,
                              const float ground_hit_bms[3],
                              int insert_index,
                              EntityRecord *out_marker,
                              WaypointPath *out_path) {
	EntityTransform transform;
	transform.x = ground_hit_bms[0];
	transform.y = ground_hit_bms[1];
	transform.z = ground_hit_bms[2];
	return doc.add_waypoint_marker(path_index,
	                               marker_item_id_for_path(doc, path_index),
	                               transform,
	                               insert_index,
	                               out_marker,
	                               out_path);
}

size_t reground_entities(MissionDocument &doc,
                         const RegroundRequest *requests,
                         size_t count,
                         float epsilon,
                         bool apply,
                         std::vector<size_t> *out_moved_rows) {
	if (requests == nullptr) {
		return 0;
	}
	size_t moved = 0;
	for (size_t i = 0; i < count; ++i) {
		const RegroundRequest &request = requests[i];
		EntityRecord record;
		if (!doc.get_entity(request.kind, request.index, record)) {
			continue;
		}
		// The same target move_entity_grounded would write: markers store the
		// hit directly, everything else gets the rotated-anchor bake.
		EntityTransform hit = record.transform; // keep pitch/yaw/roll
		hit.x = request.ground_hit_bms[0];
		hit.y = request.ground_hit_bms[1];
		hit.z = request.ground_hit_bms[2];
		const EntityTransform target = request.kind == EntityKind::Marker
		                                       ? hit
		                                       : bake_ground_transform(hit, request.ground_anchor_bms);
		const float dx = target.x - record.transform.x;
		const float dy = target.y - record.transform.y;
		const float dz = target.z - record.transform.z;
		if (std::fabs(dx) <= epsilon && std::fabs(dy) <= epsilon && std::fabs(dz) <= epsilon) {
			continue;
		}
		if (apply && !doc.set_entity_transform(request.kind, request.index, target)) {
			continue;
		}
		if (out_moved_rows != nullptr) {
			out_moved_rows->push_back(i);
		}
		moved++;
	}
	return moved;
}

} // namespace opennova::mission::authoring
