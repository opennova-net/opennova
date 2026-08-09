// Foliage frame compile (ADR 0033 R2): the vertex expansion, submission
// grouping, wind clocks, and anchor gating moved down from the shell
// dispatcher as a structural translation of the same decisions.

#include "renderer/foliage_frame.h"

#include <terrain/quadtree.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace renderer {

namespace {

constexpr float kInvalidHeightThreshold = -1.0e6f;
constexpr float kDetailHeightScale = 0.5f;
constexpr float kPiF = 3.14159265358979323846f;

bool valid_height(float height) {
	return std::isfinite(height) && height > kInvalidHeightThreshold;
}

bool finite3(float x, float y, float z) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool point_in_frustum(const opennova::Frustum &frustum, float x, float y,
		float z) {
	for (int p = 0; p < 6; ++p) {
		const float *plane = frustum.planes[p];
		if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0.0f) {
			return false;
		}
	}
	return true;
}

} // namespace

void FoliageFrameCompiler::configure_slots(
		const std::array<opennova::foliage::RuntimeSlot,
				opennova::FOLIAGE_MAX_DEFS> &slots,
		const std::array<FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS>
				&geometry) {
	slots_ = slots;
	geometry_ = geometry;
}

void FoliageFrameCompiler::reset() {
	runtime_.reset();
	resident_.clear();
	model_wind_counter_ = 0;
}

bool FoliageFrameCompiler::expand_detail_instance(
		const opennova::foliage::DetailInstance &instance,
		const opennova::foliage::WorldSamplers &world,
		const FoliageExpansionSamplers &expansion, size_t vertex_base) {
	const int slot = instance.slot;
	if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS ||
			!geometry_[slot].valid) {
		return false;
	}
	const FoliageSlotGeometry &source = geometry_[slot];

	// Rotation of the source's planar footprint by yaw + pi around +Y (the
	// shell's Basis(Vector3(0,1,0), yaw + pi) convention).
	const float angle = instance.yaw_radians + kPiF;
	const float cos_a = std::cos(angle);
	const float sin_a = std::sin(angle);

	for (const FoliageSourceVertex &vertex : source.vertices) {
		const float planar_x = vertex.x * cos_a + vertex.z * sin_a;
		const float planar_z = -vertex.x * sin_a + vertex.z * cos_a;
		const float world_x = instance.center.x + planar_x;
		const float world_z = instance.center.z + planar_z;
		const float ground = world.height_at(world_x, world_z);
		const float height_left = world.height_at(world_x - 1.0f, world_z);
		const float height_right = world.height_at(world_x + 1.0f, world_z);
		const float height_previous = world.height_at(world_x, world_z - 1.0f);
		const float height_next = world.height_at(world_x, world_z + 1.0f);
		if (!valid_height(ground) || !valid_height(height_left) ||
				!valid_height(height_right) || !valid_height(height_previous) ||
				!valid_height(height_next)) {
			packet_.vertices.resize(vertex_base);
			return false;
		}

		const float world_y = ground + vertex.y * kDetailHeightScale;
		if (!finite3(world_x, world_y, world_z)) {
			packet_.vertices.resize(vertex_base);
			return false;
		}

		const int bend_byte =
				std::clamp(static_cast<int>(vertex.y * 128.0f), 0, 255);

		FoliageVertex out;
		out.x = world_x;
		out.y = world_y;
		out.z = world_z;
		const float nx = height_left - height_right;
		const float nz = height_previous - height_next;
		const float nl = std::sqrt(nx * nx + 4.0f + nz * nz);
		out.nx = nx / nl;
		out.ny = 2.0f / nl;
		out.nz = nz / nl;
		out.u = vertex.u;
		out.v = vertex.v;
		if (!expansion.terrain_uv_at ||
				!expansion.terrain_uv_at(world_x, world_z, out.u2, out.v2)) {
			out.u2 = 0.0f;
			out.v2 = 0.0f;
		}
		out.bend = static_cast<float>(bend_byte) / 255.0f;
		packet_.vertices.push_back(out);
	}
	return true;
}

bool FoliageFrameCompiler::expand_silhouette_instance(
		const opennova::foliage::SilhouetteInstance &instance,
		size_t vertex_base) {
	const int slot = instance.slot;
	if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS ||
			!geometry_[slot].valid) {
		return false;
	}
	for (const opennova::foliage::GroundCorner &corner : instance.corners) {
		if (!valid_height(corner.height)) {
			return false;
		}
	}
	const FoliageSlotGeometry &source = geometry_[slot];
	if (source.radius <= 1.0e-6f) {
		return false;
	}

	const float inverse_span = 1.0f / (2.0f * source.radius);

	for (const FoliageSourceVertex &vertex : source.vertices) {
		// The 3DI import negates source X. Convert back to retail local A
		// while local B remains the terrain Z axis.
		const float x_normalized =
				0.5f - (vertex.x - source.center_x) * inverse_span;
		const float z_normalized =
				0.5f + (vertex.z - source.center_z) * inverse_span;
		const float one_minus_x = 1.0f - x_normalized;
		const float one_minus_z = 1.0f - z_normalized;
		const float weights[4] = {
			one_minus_x * one_minus_z,
			x_normalized * one_minus_z,
			one_minus_x * z_normalized,
			x_normalized * z_normalized,
		};

		float world_x = 0.0f;
		float world_z = 0.0f;
		float ground = 0.0f;
		for (int corner = 0; corner < 4; ++corner) {
			world_x += weights[corner] * instance.corners[corner].x;
			world_z += weights[corner] * instance.corners[corner].z;
			ground += weights[corner] * instance.corners[corner].height;
		}

		const float u = 2.0f * x_normalized - 1.0f;
		const float v = 2.0f * z_normalized - 1.0f;
		ground += (1.0f - v * v) * (instance.fold[0] + u * instance.fold[1]) +
				(1.0f - u * u) * (instance.fold[2] + v * instance.fold[3]);

		const float half_height = vertex.y * kDetailHeightScale;
		const float world_y = ground + half_height;
		if (!finite3(world_x, world_y, world_z)) {
			packet_.vertices.resize(vertex_base);
			return false;
		}

		FoliageVertex out;
		out.x = world_x;
		out.y = world_y;
		out.z = world_z;
		out.u = vertex.u;
		out.v = vertex.v;
		out.u2 = half_height;
		out.v2 = 0.0f;
		packet_.vertices.push_back(out);
	}
	return true;
}

const FoliageDrawPacket &FoliageFrameCompiler::compile(
		const FoliageViewInput &view,
		const opennova::foliage::WorldSamplers &world,
		const FoliageExpansionSamplers &expansion) {
	++compile_index_;
	packet_.frame_id = compile_index_;
	packet_.vertices.clear();
	packet_.indices.clear();
	packet_.mesh_builds.clear();
	packet_.commands.clear();
	packet_.detail_evicted.clear();
	packet_.model_evicted.clear();
	packet_.debug = FoliageFrameDebugCounters{};
	packet_.debug.compile_index = compile_index_;

	// --- The frame request: cells straight through, anchors gated ----------
	opennova::foliage::FrameRequest request;
	request.slots = slots_;
	request.detail_cells = view.detail_cells;
	packet_.debug.detail_cells =
			static_cast<int64_t>(request.detail_cells.size());
	packet_.debug.silhouette_anchors_input =
			static_cast<int64_t>(view.silhouette_anchors.size());

	// The MODEL-tier anchor gate: view depth >= the schedule floor, then
	// frustum membership (the shell used Camera3D::is_position_in_frustum;
	// the same six inside-positive planes come from the view/proj here).
	float mvp[16] = {};
	for (int row = 0; row < 4; ++row) {
		for (int col = 0; col < 4; ++col) {
			for (int k = 0; k < 4; ++k) {
				mvp[col * 4 + row] += view.proj[k * 4 + row] * view.view[col * 4 + k];
			}
		}
	}
	const opennova::Frustum frustum = opennova::extract_frustum(mvp);

	request.silhouette_anchors.reserve(view.silhouette_anchors.size());
	for (const std::array<float, 3> &anchor : view.silhouette_anchors) {
		if (!finite3(anchor[0], anchor[1], anchor[2])) {
			continue;
		}
		const float view_z = view.view[2] * anchor[0] + view.view[6] * anchor[1] +
				view.view[10] * anchor[2] + view.view[14];
		const float view_depth = -view_z;
		if (!std::isfinite(view_depth) || view_depth < kSilhouetteDepthGate) {
			continue;
		}
		if (!view.no_frustum &&
				!point_in_frustum(frustum, anchor[0], anchor[1], anchor[2])) {
			continue;
		}
		const float dx = anchor[0] - view.cam_x;
		const float dy = anchor[1] - view.cam_y;
		const float dz = anchor[2] - view.cam_z;
		const float camera_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (!std::isfinite(camera_distance)) {
			continue;
		}
		opennova::foliage::SilhouetteAnchor runtime_anchor;
		runtime_anchor.position = {anchor[0], anchor[2]};
		runtime_anchor.view_depth = view_depth;
		runtime_anchor.camera_distance = camera_distance;
		request.silhouette_anchors.push_back(runtime_anchor);
	}
	packet_.debug.silhouette_anchors_visible =
			static_cast<int64_t>(request.silhouette_anchors.size());

	// --- The placement runtime -------------------------------------------
	const opennova::foliage::FrameOutput output =
			runtime_.render_frame(request, world);
	const opennova::foliage::RuntimeStats &runtime_stats = runtime_.get_stats();
	packet_.debug.runtime = runtime_stats;
	packet_.debug.runtime_detail_intents =
			static_cast<int64_t>(output.detail.size());
	packet_.debug.runtime_silhouette_intents =
			static_cast<int64_t>(output.silhouettes.size());

	const float detail_wind_phase =
			static_cast<float>(runtime_stats.terrain_scene_counter) * 0.001f;

	// --- Detail submissions ------------------------------------------------
	for (size_t begin = 0; begin < output.detail.size();) {
		const opennova::foliage::DetailInstance &first = output.detail[begin];
		size_t end = begin + 1;
		while (end < output.detail.size() &&
				output.detail[end].submission_id == first.submission_id) {
			++end;
		}
		const int slot = first.slot;
		if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS) {
			begin = end;
			continue;
		}
		const MeshKey key{static_cast<uint8_t>(FoliageTier::Detail), first.slot,
				first.cell_key, first.cache_revision};
		auto found = resident_.find(key);
		if (found == resident_.end()) {
			const uint32_t first_vertex =
					static_cast<uint32_t>(packet_.vertices.size());
			const uint32_t first_index =
					static_cast<uint32_t>(packet_.indices.size());
			int64_t instance_count = 0;
			for (size_t i = begin; i < end; ++i) {
				const size_t base = packet_.vertices.size();
				if (expand_detail_instance(output.detail[i], world, expansion,
							base)) {
					const uint32_t local =
							static_cast<uint32_t>(base) - first_vertex;
					for (const int32_t idx : geometry_[slot].indices) {
						packet_.indices.push_back(local +
								static_cast<uint32_t>(idx));
					}
					++instance_count;
				}
			}
			FoliageMeshBuild build;
			build.tier = FoliageTier::Detail;
			build.slot = first.slot;
			build.cell_key = first.cell_key;
			build.revision = first.cache_revision;
			build.first_vertex = first_vertex;
			build.vertex_count =
					static_cast<uint32_t>(packet_.vertices.size()) - first_vertex;
			build.first_index = first_index;
			build.index_count =
					static_cast<uint32_t>(packet_.indices.size()) - first_index;
			build.instance_count = static_cast<int32_t>(instance_count);
			packet_.mesh_builds.push_back(build);
			if (build.vertex_count > 0 && build.index_count > 0) {
				++packet_.debug.detail_mesh_uploads;
			}
			ResidentMesh entry;
			entry.empty = build.vertex_count == 0 || build.index_count == 0;
			entry.instances = instance_count;
			entry.vertices = static_cast<int64_t>(build.vertex_count);
			found = resident_.emplace(key, entry).first;
		} else {
			++packet_.debug.detail_mesh_hits;
		}

		const ResidentMesh &resident = found->second;
		if (!resident.empty) {
			const bool high =
					first.pass == opennova::foliage::DetailPass::HighAlphaTest;
			FoliageDrawCommand command;
			command.tier = FoliageTier::Detail;
			command.slot = first.slot;
			command.cell_key = first.cell_key;
			command.revision = first.cache_revision;
			command.submission_id = first.submission_id;
			command.pass = first.pass;
			command.near_secondary = first.near_secondary;
			command.fade = first.alpha;
			command.alpha_reference =
					static_cast<float>(first.alpha_reference) / 255.0f;
			// The near secondary LOW draw runs under strict D3DCMP_LESS in
			// retail; the cutoff discard keeps it off every texel the HIGH
			// pass accepted. [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912]
			command.high_pass_cutoff =
					first.near_secondary ? 180.0f / 255.0f : 0.0f;
			command.wind_phase = detail_wind_phase;
			packet_.commands.push_back(command);

			if (high) {
				packet_.debug.detail_high_instances += resident.instances;
			} else {
				packet_.debug.detail_low_instances += resident.instances;
			}
			packet_.debug.detail_vertices += resident.vertices;
			++packet_.debug.render_batches;
		}
		begin = end;
	}

	// --- Silhouette submissions --------------------------------------------
	for (size_t begin = 0; begin < output.silhouettes.size();) {
		const opennova::foliage::SilhouetteInstance &first =
				output.silhouettes[begin];
		size_t end = begin + 1;
		while (end < output.silhouettes.size() &&
				output.silhouettes[end].submission_id == first.submission_id) {
			++end;
		}
		const int slot = first.slot;
		if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS) {
			begin = end;
			continue;
		}
		const MeshKey key{static_cast<uint8_t>(FoliageTier::Silhouette),
				first.slot, first.cell_key, first.cache_revision};
		auto found = resident_.find(key);
		if (found == resident_.end()) {
			const uint32_t first_vertex =
					static_cast<uint32_t>(packet_.vertices.size());
			const uint32_t first_index =
					static_cast<uint32_t>(packet_.indices.size());
			int64_t instance_count = 0;
			for (size_t i = begin; i < end; ++i) {
				const size_t base = packet_.vertices.size();
				if (expand_silhouette_instance(output.silhouettes[i], base)) {
					const uint32_t local =
							static_cast<uint32_t>(base) - first_vertex;
					for (const int32_t idx : geometry_[slot].indices) {
						packet_.indices.push_back(local +
								static_cast<uint32_t>(idx));
					}
					++instance_count;
				}
			}
			FoliageMeshBuild build;
			build.tier = FoliageTier::Silhouette;
			build.slot = first.slot;
			build.cell_key = first.cell_key;
			build.revision = first.cache_revision;
			build.first_vertex = first_vertex;
			build.vertex_count =
					static_cast<uint32_t>(packet_.vertices.size()) - first_vertex;
			build.first_index = first_index;
			build.index_count =
					static_cast<uint32_t>(packet_.indices.size()) - first_index;
			build.instance_count = static_cast<int32_t>(instance_count);
			packet_.mesh_builds.push_back(build);
			if (build.vertex_count > 0 && build.index_count > 0) {
				++packet_.debug.model_mesh_uploads;
			}
			ResidentMesh entry;
			entry.empty = build.vertex_count == 0 || build.index_count == 0;
			entry.instances = instance_count;
			entry.vertices = static_cast<int64_t>(build.vertex_count);
			found = resident_.emplace(key, entry).first;
		} else {
			++packet_.debug.model_mesh_hits;
		}

		const ResidentMesh &resident = found->second;
		if (!resident.empty) {
			FoliageDrawCommand command;
			command.tier = FoliageTier::Silhouette;
			command.slot = first.slot;
			command.cell_key = first.cell_key;
			command.revision = first.cache_revision;
			command.submission_id = first.submission_id;
			command.alpha_reference =
					static_cast<float>(first.alpha_reference) / 255.0f;
			// Retail pre-increments the wind counter once per actual nonempty
			// model draw, including repeated submissions of one resident
			// cache entry.
			command.wind_phase =
					static_cast<float>(++model_wind_counter_) * 0.001f;
			packet_.commands.push_back(command);

			packet_.debug.silhouette_instances += resident.instances;
			packet_.debug.silhouette_vertices += resident.vertices;
			++packet_.debug.render_batches;
		}
		begin = end;
	}

	// --- Eviction lifecycle -----------------------------------------------
	packet_.detail_evicted = output.detail_evicted;
	packet_.model_evicted = output.model_evicted;
	for (const opennova::foliage::CacheIdentity &identity :
			output.detail_evicted) {
		resident_.erase(MeshKey{static_cast<uint8_t>(FoliageTier::Detail),
				identity.slot, identity.key, identity.revision});
	}
	for (const opennova::foliage::CacheIdentity &identity :
			output.model_evicted) {
		resident_.erase(MeshKey{static_cast<uint8_t>(FoliageTier::Silhouette),
				identity.slot, identity.key, identity.revision});
	}

	return packet_;
}

} // namespace renderer
