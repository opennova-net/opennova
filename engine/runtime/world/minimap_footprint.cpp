#include "world/minimap_footprint.h"

#include "world/angle.h"
#include "world/collision.h"
#include "world/entity.h"
#include "world/occlusion.h"

#include <cmath>

namespace opennova::world {

namespace {

inline int32_t q16_from_float(float value) {
	return static_cast<int32_t>(
			std::lround(static_cast<double>(value) * 65536.0));
}

} // namespace

MinimapFootprintMesh minimap_footprint_from_occlusion(
		const OcclusionModel &model) {
	MinimapFootprintMesh out;
	// Each 60-byte runtime OOBJ owns its parity list. An edge shared by two
	// kept faces cancels by the low 15 bits; bit 15 carries winding only. The
	// removal swaps in the last survivor, matching retail's little stack.
	// [orig: @0x596afe..0x596b30]
	for (const OcclusionPortalFace &record : model.records) {
		if (record.type > kOccRecOpen) continue;
		std::vector<uint16_t> boundary;
		for (int32_t f = 0; f < record.face_count; ++f) {
			const size_t face_row =
					static_cast<size_t>(record.face_start + f);
			if (face_row >= model.faces.size()) break;
			const OcclusionFaceRec &face = model.faces[face_row];
			const size_t plane_row = static_cast<size_t>(
					record.plane_start + face.plane);
			if (face.plane >= record.plane_count ||
					plane_row >= model.planes.size() ||
					model.planes[plane_row].normal[1] <= 0.5f)
				continue;
			int32_t xy[6];
			bool valid = true;
			for (int corner = 0; corner < 3 && valid; ++corner) {
				const size_t vertex_row = static_cast<size_t>(
						record.vert_start + face.v[corner]);
				if (face.v[corner] >= record.vert_count ||
						vertex_row >= model.vertices.size()) {
					valid = false;
					break;
				}
				const OcclusionVertex &v = model.vertices[vertex_row];
				xy[corner * 2 + 0] = q16_from_float(v.p[0]);
				xy[corner * 2 + 1] = q16_from_float(v.p[2]);
			}
			if (!valid) continue;
			for (int32_t value : xy) out.fill_xy_q16.push_back(value);
			for (int corner = 0; corner < 3; ++corner) {
				const uint16_t edge = face.edge[corner];
				const uint16_t key = edge & 0x7FFFu;
				size_t match = boundary.size();
				for (size_t i = 0; i < boundary.size(); ++i) {
					if ((boundary[i] & 0x7FFFu) == key) {
						match = i;
						break;
					}
				}
				if (match < boundary.size()) {
					boundary[match] = boundary.back();
					boundary.pop_back();
				} else {
					boundary.push_back(edge);
				}
			}
		}
		// Degenerate records (under 4 surviving edges) draw no outline at all.
		// [orig: @0x596b3f — the edge-emit loop runs only when the survivor
		//  count is >= 4]
		if (boundary.size() >= 4) {
			for (const uint16_t edge : boundary) {
				const uint8_t a = static_cast<uint8_t>(edge & 0xFFu);
				const uint8_t b = static_cast<uint8_t>((edge >> 8) & 0x7Fu);
				if (a >= record.vert_count || b >= record.vert_count) continue;
				const size_t row_a = static_cast<size_t>(record.vert_start + a);
				const size_t row_b = static_cast<size_t>(record.vert_start + b);
				if (row_a >= model.vertices.size() || row_b >= model.vertices.size())
					continue;
				const OcclusionVertex &va = model.vertices[row_a];
				const OcclusionVertex &vb = model.vertices[row_b];
				out.edge_xy_q16.push_back(q16_from_float(va.p[0]));
				out.edge_xy_q16.push_back(q16_from_float(va.p[2]));
				out.edge_xy_q16.push_back(q16_from_float(vb.p[0]));
				out.edge_xy_q16.push_back(q16_from_float(vb.p[2]));
			}
		}
	}
	return out;
}

uint32_t minimap_footprint_fill_argb(const Entity &entity) {
	// [orig: render_collision_wireframe @0x596848..0x596880]
	if (entity.team == 1) return 0xFF4050A0u;
	if (entity.team == 2) return 0xFFA05040u;
	if ((entity.item_attrib & kItemAttribChangeTeam) != 0) return 0xFF609F60u;
	return 0xFFA0A0A0u;
}

void minimap_footprint_place(const MinimapFootprintMesh &mesh,
		const Entity &entity, std::vector<int32_t> &out_fill_xy_q16,
		std::vector<int32_t> &out_edge_xy_q16) {
	// Entity::yaw holds mission DEGREES; the placement heading is the same
	// BAM conversion the collision instance runs, so the footprint rotates
	// exactly with the collision shell and the render model.
	// [orig: bam_heading_from_mission_yaw_deg — the collision placement's
	//  heading source; the wireframe's sin/cos ride the same entity heading]
	const int32_t pos_q16[3] = {
		static_cast<int32_t>(entity.position.x * 65536.0f),
		static_cast<int32_t>(entity.position.y * 65536.0f),
		static_cast<int32_t>(entity.position.z * 65536.0f),
	};
	const CollisionMatrix matrix = collision_matrix_from_heading(
			bam_heading_from_mission_yaw_deg(
					static_cast<double>(entity.yaw)), pos_q16);
	const auto place_pairs = [&](const std::vector<int32_t> &local,
			std::vector<int32_t> &out) {
		out.reserve(out.size() + local.size());
		for (size_t i = 0; i + 1 < local.size(); i += 2) {
			const int32_t in[3] = {local[i], local[i + 1], 0};
			int32_t world[3] = {};
			matrix.transform_point(in, world);
			out.push_back(world[0]);
			out.push_back(world[1]);
		}
	};
	place_pairs(mesh.fill_xy_q16, out_fill_xy_q16);
	place_pairs(mesh.edge_xy_q16, out_edge_xy_q16);
}

} // namespace opennova::world
