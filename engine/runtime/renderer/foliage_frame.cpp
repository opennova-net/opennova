// Foliage frame compile (ADR 0033 R2): the vertex expansion, submission
// grouping, wind clocks, and anchor gating moved down from the shell
// dispatcher as a structural translation of the same decisions.

#include <runtime/renderer/foliage_frame.h>


#include <algorithm>
#include <cmath>
#include <limits>

namespace opennova::renderer {

namespace {

constexpr float kInvalidHeightThreshold = -1.0e6f;
constexpr float kDetailHeightScale = 0.5f;

bool valid_height(float height) {
	return std::isfinite(height) && height > kInvalidHeightThreshold;
}

bool finite3(float x, float y, float z) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

int32_t sign_extend_15(uint32_t value) {
	const int32_t low = static_cast<int32_t>(value & 0x7FFFu);
	return (low & 0x4000) != 0 ? low - 0x8000 : low;
}

} // namespace

// [orig: Foliage_SetupVertexShaderConstants @ 0x60074a..0x60079d — see the
// header note for the constant map and the 2-pi fold rationale]
float foliage_detail_wind_phase(uint32_t time_ms, int32_t wind_osc_ring0) {
	constexpr double kTwoPi = 6.283185307179586;
	// flt_7DE9D0 = 0x35CCCCCD, the float nearest 1/655360 (@ 0x600767).
	constexpr float kRingScale = 0x1.99999Ap-20f;
	const double clock_term =
			std::fmod(static_cast<double>(time_ms) * 0.003, kTwoPi);
	return static_cast<float>(clock_term +
			static_cast<double>(wind_osc_ring0) * static_cast<double>(kRingScale));
}

// [orig: Terrain_CollectNearFoliagePatches @ 0x603fc1 (the patch's sector
// origin FB20 << 9 stored for the draw); the header carries the rest]
float foliage_detail_wind_sector_origin_z(uint32_t cell_key) {
	int32_t z_min = static_cast<int32_t>(cell_key & 0x7FFFu);
	if ((z_min & 0x4000) != 0) {
		z_min -= 0x8000;
	}
	// Arithmetic floor to the 512-unit sector (a negative Z-min floors down).
	const int32_t sector = z_min >= 0 ? z_min / 512 : -((-z_min + 511) / 512);
	return static_cast<float>(sector * 512);
}

float foliage_model_wind_offset(double angle) {
	return static_cast<float>(std::sin(angle) * 0.079999998);
}

bool foliage_camera_above_water(float camera_y, float water_height) {
	return camera_y >= water_height;
}

bool foliage_entity_far_side(float entity_y, float camera_y, float water_height) {
	const bool entity_below_water = entity_y - 1.0f < water_height;
	return entity_below_water == foliage_camera_above_water(camera_y, water_height);
}

void FoliageFrameCompiler::configure_slots(
		const std::array<opennova::foliage::RuntimeSlot,
				opennova::FOLIAGE_MAX_DEFS> &slots,
		const std::array<FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS>
				&geometry) {
	slots_ = slots;
	geometry_ = geometry;
	// The MODEL tier's instanced VB source: retail maps the model's x/z over
	// its bound square ((v - centre) * 0.5 / BoundRadius + 0.5, 0.5 when the
	// radius is zero) and halves y; the 3DI import negated source x, so the
	// retail x is the imported x mirrored about the centre.
	// [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa2a..0x5ffac6]
	++model_mesh_generation_;
	for (size_t slot = 0; slot < model_meshes_.size(); ++slot) {
		FoliageSlotModelMesh &mesh = model_meshes_[slot];
		mesh = FoliageSlotModelMesh{};
		const FoliageSlotGeometry &source = geometry_[slot];
		if (!source.valid || source.vertices.empty() || source.indices.empty()) {
			continue;
		}
		const float scale = source.radius != 0.0f ? 0.5f / source.radius : 0.5f;
		mesh.vertices.reserve(source.vertices.size());
		mesh.max_half_height = -std::numeric_limits<float>::infinity();
		mesh.min_half_height = std::numeric_limits<float>::infinity();
		for (const FoliageSourceVertex &vertex : source.vertices) {
			FoliageModelVertex out;
			out.x = (source.center_x - vertex.x) * scale + 0.5f;
			out.y = vertex.y * kDetailHeightScale;
			out.z = (vertex.z - source.center_z) * scale + 0.5f;
			out.u = vertex.u;
			out.v = vertex.v;
			mesh.max_half_height = std::max(mesh.max_half_height, out.y);
			mesh.min_half_height = std::min(mesh.min_half_height, out.y);
			mesh.vertices.push_back(out);
		}
		mesh.indices.reserve(source.indices.size());
		for (const int32_t index : source.indices) {
			mesh.indices.push_back(static_cast<uint32_t>(index));
		}
		mesh.valid = true;
	}
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

	// Retail writes render x = keyLo + B + sx*sin + sz*cos (the Godot-Z
	// axis) and render z = keyHi + A + sx*cos - sz*sin (Godot X) for source
	// (sx, sz); the 3DI import negates source X (vertex.x = -sx).
	// [orig: Foliage_GenerateInstances_0 @ 0x600112..0x60014d]
	const float cos_a = std::cos(instance.yaw_radians);
	const float sin_a = std::sin(instance.yaw_radians);

	for (const FoliageSourceVertex &vertex : source.vertices) {
		const float planar_x = -(vertex.x * cos_a + vertex.z * sin_a);
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
			draw_list_.vertices.resize(vertex_base);
			return false;
		}

		const float world_y = ground + vertex.y * kDetailHeightScale;
		if (!finite3(world_x, world_y, world_z)) {
			draw_list_.vertices.resize(vertex_base);
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
		draw_list_.vertices.push_back(out);
	}
	return true;
}

const FoliageDrawList &FoliageFrameCompiler::compile(
		const FoliageViewInput &view,
		const opennova::foliage::WorldSamplers &world,
		const FoliageExpansionSamplers &expansion) {
	++compile_index_;
	draw_list_.frame_id = compile_index_;
	draw_list_.vertices.clear();
	draw_list_.indices.clear();
	draw_list_.mesh_builds.clear();
	draw_list_.commands.clear();
	draw_list_.model_instances.clear();
	draw_list_.detail_evicted.clear();
	draw_list_.debug = FoliageFrameDebugCounters{};
	draw_list_.debug.compile_index = compile_index_;

	// --- The frame request: cells straight through, anchors gated ----------
	opennova::foliage::FrameRequest request;
	request.slots = slots_;
	request.detail_cells = view.detail_cells;
	request.thermal_view = view.thermal_view;
	const bool camera_above_water =
			foliage_camera_above_water(view.cam_y, view.water_height);
	request.water_height = view.water_height;
	request.camera_below_water = !camera_above_water;
	draw_list_.debug.detail_cells =
			static_cast<int64_t>(request.detail_cells.size());
	draw_list_.debug.silhouette_anchors_input =
			static_cast<int64_t>(view.silhouette_anchors.size());

	// The anchors arrive already admitted by the visible-entity walk; the
	// MODEL walk adds its own view-depth floor, and each anchor rides its
	// BySide wave (foliage_entity_far_side).
	// [orig: Foliage_UpdateModelTiles @ 0x601f99..0x601fab (view z >= 38)]
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
		runtime_anchor.far_side =
				foliage_entity_far_side(anchor[1], view.cam_y, view.water_height);
		request.silhouette_anchors.push_back(runtime_anchor);
	}
	draw_list_.debug.silhouette_anchors_visible =
			static_cast<int64_t>(request.silhouette_anchors.size());

	// --- The placement runtime -------------------------------------------
	const opennova::foliage::FrameOutput output =
			runtime_.render_frame(request, world);
	const opennova::foliage::RuntimeStats &runtime_stats = runtime_.get_stats();
	draw_list_.debug.runtime = runtime_stats;
	draw_list_.debug.runtime_detail_intents =
			static_cast<int64_t>(output.detail.size());
	draw_list_.debug.runtime_silhouette_intents =
			static_cast<int64_t>(output.silhouettes.size());

	// The detail tier's c24.x (foliage_detail_wind_phase in the header —
	// [orig: Foliage_SetupVertexShaderConstants @ 0x60074a..0x60079d]).
	const float detail_wind_phase =
			foliage_detail_wind_phase(view.time_ms, view.wind_osc_ring0);

	// --- Detail submissions ------------------------------------------------
	// Skipped whole under the indoors letter: neither detail pass runs, so
	// no identity is built or drawn (view.detail_passes carries the witness).
	const size_t detail_submissions =
			view.detail_passes ? output.detail.size() : 0u;
	for (size_t begin = 0; begin < detail_submissions;) {
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
					static_cast<uint32_t>(draw_list_.vertices.size());
			const uint32_t first_index =
					static_cast<uint32_t>(draw_list_.indices.size());
			int64_t instance_count = 0;
			for (size_t i = begin; i < end; ++i) {
				const size_t base = draw_list_.vertices.size();
				if (expand_detail_instance(output.detail[i], world, expansion,
							base)) {
					const uint32_t local =
							static_cast<uint32_t>(base) - first_vertex;
					for (const int32_t idx : geometry_[slot].indices) {
						draw_list_.indices.push_back(local +
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
					static_cast<uint32_t>(draw_list_.vertices.size()) - first_vertex;
			build.first_index = first_index;
			build.index_count =
					static_cast<uint32_t>(draw_list_.indices.size()) - first_index;
			build.instance_count = static_cast<int32_t>(instance_count);
			draw_list_.mesh_builds.push_back(build);
			if (build.vertex_count > 0 && build.index_count > 0) {
				++draw_list_.debug.detail_mesh_uploads;
			}
			ResidentMesh entry;
			entry.empty = build.vertex_count == 0 || build.index_count == 0;
			entry.instances = instance_count;
			entry.vertices = static_cast<int64_t>(build.vertex_count);
			found = resident_.emplace(key, entry).first;
		} else {
			++draw_list_.debug.detail_mesh_hits;
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
			// pass accepted. [orig: Foliage_SetupDetailSlotDraw @ 0x6008fc..0x600912]
			command.high_pass_cutoff =
					first.near_secondary ? 180.0f / 255.0f : 0.0f;
			command.wind_phase = detail_wind_phase;
			command.wind_sector_origin_z =
					foliage_detail_wind_sector_origin_z(first.cell_key);
			command.far_side =
					first.water_pass == opennova::foliage::DetailWaterPass::FarSide;
			command.render_rung =
					command.far_side ? kRungFoliageFarSide : kRungFoliageCameraSide;
			command.sorting_offset =
					first.near_secondary ? kFoliageSecondaryLowSortingOffset : 0.0f;
			draw_list_.commands.push_back(command);

			if (high) {
				draw_list_.debug.detail_high_instances += resident.instances;
			} else {
				draw_list_.debug.detail_low_instances += resident.instances;
			}
			draw_list_.debug.detail_vertices += resident.vertices;
			++draw_list_.debug.render_batches;
		}
		begin = end;
	}

	// --- MODEL submissions: GridPlacementVS instance blocks -----------------
	// Each submission uploads its instance blocks and draws the slot's
	// normalized mesh once per instance; the block rows are the retail
	// constants: render x = local B + (-keyLo) (the Godot Z), the heights,
	// render z = local A + keyHi (Godot X), and the fold.
	// [orig: Foliage_UploadModelTileVSConstants @ 0x6010e6..0x601209;
	// Foliage_DrawModelTileSlot @ 0x601d90..0x601ec6]
	for (size_t begin = 0; begin < output.silhouettes.size();) {
		const opennova::foliage::SilhouetteInstance &first =
				output.silhouettes[begin];
		size_t end = begin + 1;
		while (end < output.silhouettes.size() &&
				output.silhouettes[end].submission_id == first.submission_id) {
			++end;
		}
		const int slot = first.slot;
		if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS ||
				!model_meshes_[static_cast<size_t>(slot)].valid) {
			begin = end;
			continue;
		}
		const FoliageSlotModelMesh &mesh = model_meshes_[static_cast<size_t>(slot)];
		const float key_hi = static_cast<float>(sign_extend_15(first.cell_key >> 16u));
		const float neg_key_lo = static_cast<float>(-sign_extend_15(first.cell_key));
		const uint32_t first_instance =
				static_cast<uint32_t>(draw_list_.model_instances.size());
		float aabb_min[3] = {std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity()};
		float aabb_max[3] = {-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity()};
		for (size_t i = begin; i < end; ++i) {
			const opennova::foliage::SilhouetteInstance &instance =
					output.silhouettes[i];
			bool heights_valid = true;
			for (const opennova::foliage::GroundCorner &corner : instance.corners) {
				heights_valid = heights_valid && valid_height(corner.height);
			}
			if (!heights_valid) {
				continue;
			}
			FoliageModelInstance block;
			float fold_extent = 0.0f;
			for (int k = 0; k < 4; ++k) {
				block.rows[k] = instance.corner_local_b[k] + neg_key_lo;
				block.rows[4 + k] = instance.corners[k].height;
				block.rows[8 + k] = instance.corner_local_a[k] + key_hi;
				block.rows[12 + k] = instance.fold[k];
				fold_extent += std::fabs(instance.fold[k]);
				aabb_min[0] = std::min(aabb_min[0], block.rows[8 + k]);
				aabb_max[0] = std::max(aabb_max[0], block.rows[8 + k]);
				aabb_min[1] = std::min(aabb_min[1], block.rows[4 + k]);
				aabb_max[1] = std::max(aabb_max[1], block.rows[4 + k]);
				aabb_min[2] = std::min(aabb_min[2], block.rows[k]);
				aabb_max[2] = std::max(aabb_max[2], block.rows[k]);
			}
			aabb_min[1] -= fold_extent;
			aabb_max[1] += fold_extent;
			draw_list_.model_instances.push_back(block);
		}
		const uint32_t instance_count =
				static_cast<uint32_t>(draw_list_.model_instances.size()) - first_instance;
		if (instance_count == 0) {
			begin = end;
			continue;
		}
		FoliageDrawCommand command;
		command.tier = FoliageTier::Silhouette;
		command.slot = first.slot;
		command.cell_key = first.cell_key;
		command.revision = first.cache_revision;
		command.submission_id = first.submission_id;
		command.alpha_reference =
				static_cast<float>(first.alpha_reference) / 255.0f;
		command.far_side = first.far_side;
		command.render_rung = first.far_side ? kFoliageMaskFarSideRung
		                                     : kFoliageMaskCameraSideRung;
		command.sorting_offset = kFoliageMaskSortingOffset;
		command.first_instance = first_instance;
		command.instance_count = instance_count;
		// c9 = (sin(++counter * 0.001) * 0.08, 1, 0, 0): the counter
		// pre-increments once per actual model draw, repeated submissions of
		// one resident entry included. [orig: Foliage_UploadModelTileVSConstants
		// @ 0x60108e..0x6010cf]
		command.wind_offset = foliage_model_wind_offset(
				static_cast<double>(++model_wind_counter_) * 0.001);
		// The half-height column (and its sway on render x) lifts the fitted
		// ground.
		const float sway = std::fabs(command.wind_offset) *
				std::max(std::fabs(mesh.max_half_height),
						std::fabs(mesh.min_half_height));
		command.aabb_min[0] = aabb_min[0];
		command.aabb_max[0] = aabb_max[0];
		command.aabb_min[1] = aabb_min[1] + std::min(0.0f, mesh.min_half_height);
		command.aabb_max[1] = aabb_max[1] + std::max(0.0f, mesh.max_half_height);
		command.aabb_min[2] = aabb_min[2] - sway;
		command.aabb_max[2] = aabb_max[2] + sway;
		draw_list_.commands.push_back(command);

		draw_list_.debug.silhouette_instances += instance_count;
		draw_list_.debug.silhouette_vertices +=
				static_cast<int64_t>(instance_count) *
				static_cast<int64_t>(mesh.vertices.size());
		++draw_list_.debug.render_batches;
		begin = end;
	}

	// --- Eviction lifecycle -----------------------------------------------
	draw_list_.detail_evicted = output.detail_evicted;
	for (const opennova::foliage::CacheIdentity &identity :
			output.detail_evicted) {
		resident_.erase(MeshKey{static_cast<uint8_t>(FoliageTier::Detail),
				identity.slot, identity.key, identity.revision});
	}

	return draw_list_;
}

}  // namespace opennova::renderer