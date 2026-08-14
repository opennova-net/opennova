#include "world/minimap_footprint.h"

#include "world/angle.h"
#include "world/collision.h"
#include "world/entity.h"

#include <unordered_set>

namespace opennova::world {

namespace {

// Undirected edge key over model-global vertex rows.
inline uint64_t edge_key(int32_t a, int32_t b) {
	const uint32_t lo = static_cast<uint32_t>(a < b ? a : b);
	const uint32_t hi = static_cast<uint32_t>(a < b ? b : a);
	return (static_cast<uint64_t>(hi) << 32) | lo;
}

} // namespace

MinimapFootprintMesh minimap_footprint_from_collision(
		const CollisionModel &model) {
	MinimapFootprintMesh out;
	// The parity toggle: an edge shared by two kept faces cancels; the
	// survivors are the footprint silhouette. Retail runs the same toggle
	// over the face records' edge words masked 0x7FFF (lo byte / hi 7 bits =
	// the vertex pair, bit15 a flag); the undirected vertex-pair form is the
	// structural equivalent over our indexed faces, per section like the
	// original's per-COBJ edge list. [orig: @0x596afe..0x596b30]
	std::unordered_set<uint64_t> boundary;
	for (const CollisionSection &section : model.sections) {
		// Only the ordinary hull sections slice into the footprint — the
		// special/interior families (COBJ type byte > 1) are skipped, which
		// is what keeps retail's building outlines single and clean.
		// [orig: @0x596803 — *(BYTE *)cobj <= 1 gate]
		if ((section.type_code & 0xFF) > 1) continue;
		for (int32_t f = 0; f < section.face_count; ++f) {
			const size_t face_row =
					static_cast<size_t>(section.face_start + f);
			if (face_row >= model.faces.size()) break;
			const CollisionFace &face = model.faces[face_row];
			// Up-facing filter: the plane normal's up component > 0.5.
			// [orig: the 0.5 compare against the plane table's +4 field]
			const size_t normal_row = static_cast<size_t>(
					section.normal_start + face.normal_index);
			if (face.normal_index < 0 || normal_row >= model.normals.size())
				continue;
			if (model.normals[normal_row].n[2] <= 8192) // 0.5 in Q14
				continue;
			int32_t xy[6];
			bool valid = true;
			for (int corner = 0; corner < 3 && valid; ++corner) {
				const size_t vertex_row = static_cast<size_t>(
						section.vertex_start + face.vertex_index[corner]);
				if (face.vertex_index[corner] < 0 ||
						vertex_row >= model.vertices.size()) {
					valid = false;
					break;
				}
				const CollisionVertex &v = model.vertices[vertex_row];
				xy[corner * 2 + 0] = v.p[0] + section.offset[0];
				xy[corner * 2 + 1] = v.p[1] + section.offset[1];
			}
			if (!valid) continue;
			for (int32_t value : xy) out.fill_xy_q16.push_back(value);
			for (int corner = 0; corner < 3; ++corner) {
				const int32_t a = section.vertex_start +
						face.vertex_index[corner];
				const int32_t b = section.vertex_start +
						face.vertex_index[(corner + 1) % 3];
				const uint64_t key = edge_key(a, b);
				if (!boundary.insert(key).second) boundary.erase(key);
			}
		}
	}
	// Degenerate slices (under 4 surviving edges) draw no outline at all.
	// [orig: @0x596b3f — the edge-emit loop runs only when the survivor
	//  count is >= 4]
	if (boundary.size() >= 4) {
		// Second pass: emit the surviving edges' endpoints. Vertex rows are
		// model-global; recover each section's offset by locating the run.
		for (const CollisionSection &section : model.sections) {
			if ((section.type_code & 0xFF) > 1) continue;
			for (int32_t f = 0; f < section.face_count; ++f) {
				const size_t face_row =
						static_cast<size_t>(section.face_start + f);
				if (face_row >= model.faces.size()) break;
				const CollisionFace &face = model.faces[face_row];
				const size_t normal_row = static_cast<size_t>(
						section.normal_start + face.normal_index);
				if (face.normal_index < 0 ||
						normal_row >= model.normals.size())
					continue;
				if (model.normals[normal_row].n[2] <= 8192) continue;
				for (int corner = 0; corner < 3; ++corner) {
					const int32_t a = section.vertex_start +
							face.vertex_index[corner];
					const int32_t b = section.vertex_start +
							face.vertex_index[(corner + 1) % 3];
					const auto it = boundary.find(edge_key(a, b));
					if (it == boundary.end()) continue;
					boundary.erase(it);
					const CollisionVertex &va =
							model.vertices[static_cast<size_t>(a)];
					const CollisionVertex &vb =
							model.vertices[static_cast<size_t>(b)];
					out.edge_xy_q16.push_back(va.p[0] + section.offset[0]);
					out.edge_xy_q16.push_back(va.p[1] + section.offset[1]);
					out.edge_xy_q16.push_back(vb.p[0] + section.offset[0]);
					out.edge_xy_q16.push_back(vb.p[1] + section.offset[1]);
				}
			}
		}
	}
	return out;
}

uint32_t minimap_footprint_fill_argb(const Entity &entity) {
	// [orig: render_collision_wireframe @0x596848..0x596880]
	if (entity.team == 1) return 0xD04050A0u;
	if (entity.team == 2) return 0xD0A05040u;
	if ((entity.item_attrib & kItemAttribChangeTeam) != 0) return 0xD0609F60u;
	return 0xD0A0A0A0u;
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
