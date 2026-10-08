#include <runtime/terrain/terrain_static_shadow_planner.h>
#include <base/io/fixed.h>
#include <base/io/hash.h>

#include <runtime/mission/placement_traits.h>
#include <runtime/terrain/row_stripes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace opennova::terrain {
namespace {

constexpr std::size_t kMaxUnsupportedAttribution = 128;
// Bounded caches: page requests per frame are bounded by the composition
// cache capacity; twice that comfortably covers epoch transitions.
constexpr std::size_t kMaxCachedPlans =
		2 * TerrainTileCompositionCache::kCapacity;

std::array<float, 3> transform_point(const std::array<float, 12> &transform,
		const std::array<float, 3> &point) {
	std::array<float, 3> out{};
	for (int row = 0; row < 3; ++row) {
		out[row] = transform[static_cast<std::size_t>(row) * 3] * point[0] +
				transform[static_cast<std::size_t>(row) * 3 + 1] * point[1] +
				transform[static_cast<std::size_t>(row) * 3 + 2] * point[2] +
				transform[9 + static_cast<std::size_t>(row)];
	}
	return out;
}

// The binding hashed the Godot basis rows then origin; the record carries the
// same value order.
uint64_t hash_transform(uint64_t hash,
		const std::array<float, 12> &transform) {
	for (const float component : transform) {
		hash = io::fnv1a64_value(hash, component);
	}
	return hash;
}

uint64_t hash_control_values(uint64_t hash,
		const opennova::renderer::ControlRegisterValues &values) {
	for (const int32_t value : values) {
		hash = io::fnv1a64_value(hash, value);
	}
	return hash;
}

uint64_t mix_content(uint64_t content, uint64_t config) {
	uint64_t hash = content;
	const uint8_t domain[] = {'s', 't', 'a', 't', 'i', 'c', '-', 'p', 'a',
			'g', 'e'};
	hash = io::fnv1a64_bytes(hash, domain, sizeof(domain));
	return io::fnv1a64_value(hash, config);
}

// Order-sensitive stamp over every raster-affecting field of the incoming
// snapshot. Records without geometry are skipped exactly as replace_casters
// drops them; geometry participates by content key, never by pointer (callers
// legitimately rebuild identical geometry objects per marshal).
uint64_t caster_set_stamp(
		const std::vector<TerrainStaticShadowPlannerCaster> &casters,
		bool admitted_geometry_missing) {
	uint64_t hash = io::fnv1a64_value(io::kFnv1a64Offset, admitted_geometry_missing);
	for (const TerrainStaticShadowPlannerCaster &record : casters) {
		if (record.geometry == nullptr) {
			continue;
		}
		hash = io::fnv1a64_value(hash, record.bms_id);
		hash = io::fnv1a64_value(hash, record.entity_kind);
		hash = io::fnv1a64_value(hash, record.entity_index);
		hash = io::fnv1a64_value(hash, record.team);
		hash = hash_control_values(hash, record.control_values);
		hash = io::fnv1a64_value(hash, record.entity_attrib);
		hash = io::fnv1a64_value(hash, record.item_attrib);
		hash = io::fnv1a64_value(hash, record.item_attrib2);
		hash = io::fnv1a64_value(hash, record.active);
		hash = io::fnv1a64_value(hash, record.graphic.size());
		hash = io::fnv1a64_bytes(hash, record.graphic.data(), record.graphic.size());
		hash = hash_transform(hash, record.world_transform);
		hash = io::fnv1a64_value(hash, record.geometry->key);
		hash = io::fnv1a64_value(hash, record.ground_y);
		hash = io::fnv1a64_value(hash, record.caster_identity);
	}
	return hash == 0 ? 1 : hash;
}

// Whether two admissions of one caster draw the same silhouettes into the same
// pages: what the tile test and the draws read (TerrainStaticShadowCollector::
// compile), the transform revision standing for the pose, the model, the team
// and the ground under it.
bool draws_alike(const TerrainStaticShadowCandidate &a,
		const TerrainStaticShadowCandidate &b) noexcept {
	return a.position_fixed == b.position_fixed &&
			a.model_radius_fixed == b.model_radius_fixed &&
			a.transform_revision == b.transform_revision &&
			a.geometry.geometry_key == b.geometry.geometry_key &&
			a.geometry.render_object_counts == b.geometry.render_object_counts;
}

// The reaches of the admitted casters one snapshot changes: one gone or no
// longer admitted names where it was, one new or newly admitted where it is,
// one that draws otherwise both.
void name_changed_reaches(const std::vector<TerrainStaticShadowCandidate> &before,
		const std::vector<TerrainStaticShadowCandidate> &after,
		std::vector<TerrainStaticShadowReach> &out) {
	std::unordered_map<uint64_t, const TerrainStaticShadowCandidate *> was;
	was.reserve(before.size());
	for (const TerrainStaticShadowCandidate &candidate : before)
		was.emplace(candidate.caster_key, &candidate);
	std::unordered_map<uint64_t, bool> kept;
	kept.reserve(after.size());
	for (const TerrainStaticShadowCandidate &candidate : after) {
		const auto found = was.find(candidate.caster_key);
		kept[candidate.caster_key] = true;
		if (found != was.end() && draws_alike(*found->second, candidate)) continue;
		if (found != was.end())
			out.push_back(terrain_static_shadow_caster_reach(*found->second));
		out.push_back(terrain_static_shadow_caster_reach(candidate));
	}
	for (const TerrainStaticShadowCandidate &candidate : before)
		if (kept.find(candidate.caster_key) == kept.end())
			out.push_back(terrain_static_shadow_caster_reach(candidate));
}

} // namespace

bool TerrainStaticShadowPlanner::PageKeyEq::operator()(
		const TerrainTilePageKey &a, const TerrainTilePageKey &b)
		const noexcept {
	return same_page(a, b);
}

std::size_t TerrainStaticShadowPlanner::PageKeyHash::operator()(
		const TerrainTilePageKey &key) const noexcept {
	uint64_t hash = io::fnv1a64_value(io::kFnv1a64Offset, key.sector_origin_x);
	hash = io::fnv1a64_value(hash, key.sector_origin_z);
	hash = io::fnv1a64_value(hash, key.page_local_x);
	hash = io::fnv1a64_value(hash, key.page_local_z);
	hash = io::fnv1a64_value(hash, key.page_lod_level);
	return static_cast<std::size_t>(hash);
}

uint64_t terrain_static_shadow_caster_key(int32_t entity_kind,
		int32_t entity_index, int32_t bms_id) {
	uint64_t hash = io::kFnv1a64Offset;
	hash = io::fnv1a64_value(hash, entity_kind);
	hash = io::fnv1a64_value(hash, entity_index);
	hash = io::fnv1a64_value(hash, bms_id);
	return hash == 0 ? 1 : hash;
}

TerrainStaticShadowPlanner::TerrainStaticShadowPlanner() :
		casters_(std::make_shared<CasterSet>()) {
	update_config_stamp();
}

void TerrainStaticShadowPlanner::bump_epoch() {
	plan_cache_.clear();
	++state_revision_;
	if (state_revision_ == 0) ++state_revision_;
}

void TerrainStaticShadowPlanner::update_config_stamp() {
	uint64_t hash = io::fnv1a64_value(io::kFnv1a64Offset, enabled_);
	for (const int32_t id : suppressed_ids_) {
		hash = io::fnv1a64_value(hash, id);
	}
	config_stamp_ = hash;
}

void TerrainStaticShadowPlanner::set_enabled(bool enabled) {
	if (enabled_ == enabled) return;
	enabled_ = enabled;
	update_config_stamp();
	bump_epoch();
}

void TerrainStaticShadowPlanner::set_suppressed_bms_ids(
		std::vector<int32_t> ids) {
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	if (ids == suppressed_ids_) return;
	suppressed_ids_ = std::move(ids);
	update_config_stamp();
	bump_epoch();
}

void TerrainStaticShadowPlanner::set_light(
		const TerrainStaticShadowLightDirection &world_light,
		const TerrainTileLightEpoch &light_epoch) {
	// Raw light feeds projection; the QUANTIZED epoch is the cache identity.
	// Sub-quantum time-of-day motion neither recomposes resident pages (the
	// content stamp mixes the epoch bytes, not the raw tuple) nor rebuilds
	// cached plans — the same approximation the page cache already rides.
	const bool epoch_changed = light_epoch_ != light_epoch;
	world_light_ = world_light;
	light_epoch_ = light_epoch;
	if (epoch_changed) bump_epoch();
}

void TerrainStaticShadowPlanner::replace_casters(
		std::vector<TerrainStaticShadowPlannerCaster> casters,
		bool admitted_geometry_missing) {
	// The rasterizer rebuilds its snapshot on any ObjectData global-counter
	// bump, and most rebuilds carry byte-identical casters — an identical
	// snapshot must keep the plan cache (and the composed pages riding its
	// stamps) intact.
	const uint64_t incoming_stamp = caster_set_stamp(casters,
			admitted_geometry_missing);
	if (incoming_stamp == caster_set_stamp_) {
		return;
	}
	caster_set_stamp_ = incoming_stamp;
	// Copy-on-write: the previous set stays alive for every worker snapshot
	// still holding it; this planner (and later copies) adopt the new one.
	auto next = std::make_shared<CasterSet>();
	next->exact = !admitted_geometry_missing;
	std::vector<TerrainStaticShadowCandidate> candidates;
	candidates.reserve(casters.size());
	for (TerrainStaticShadowPlannerCaster &record : casters) {
		if (record.geometry == nullptr) {
			// The marshaller already declared admitted casters whose
			// resolution failed; a record without geometry carries nothing.
			continue;
		}
		const uint64_t key = terrain_static_shadow_caster_key(
				record.entity_kind, record.entity_index, record.bms_id);

		TerrainStaticShadowCandidate candidate;
		candidate.bms_id = record.bms_id;
		candidate.caster_key = key;
		candidate.collector_order = static_cast<uint32_t>(
				std::max(record.entity_index, 0));
		candidate.entity_kind = record.entity_kind;
		candidate.entity_attrib = record.entity_attrib;
		candidate.item_attrib = record.item_attrib;
		candidate.item_attrib2 = record.item_attrib2;
		candidate.active = record.active;
		// Mission axes: x east = Godot x, y north = -Godot z (the origin
		// sits at rows-major [9..11]).
		candidate.position_fixed = {
			io::float_to_fp16_16_round_sat(record.world_transform[9]),
			io::float_to_fp16_16_round_sat(-record.world_transform[11])};
		candidate.model_radius_fixed = record.geometry->model_radius_fixed;
		candidate.geometry.geometry_key = record.geometry->key;
		candidate.geometry.render_object_counts =
				record.geometry->render_object_counts;
		uint64_t revision = hash_transform(io::kFnv1a64Offset,
				record.world_transform);
		revision = io::fnv1a64_value(revision, record.geometry->key);
		revision = io::fnv1a64_value(revision, record.caster_identity);
		// Team participates: TEX_TEAM flipbooks select alpha frames by team,
		// so a team change must invalidate the caster's resident pages.
		revision = io::fnv1a64_value(revision, record.team);
		revision = hash_control_values(revision, record.control_values);
		// Ground height participates: the raster subtracts caster_ground_y, so
		// a terrain edit under the caster must recompose its resident pages.
		revision = io::fnv1a64_value(revision, record.ground_y);
		candidate.transform_revision = revision == 0 ? 1 : revision;
		candidates.push_back(candidate);
		next->records[key] = std::move(record);
	}
	next->collector.replace(std::move(candidates));
	if (reports_caster_changes_)
		name_changed_reaches(casters_->collector.admitted(), next->collector.admitted(),
				changed_reaches_);
	casters_ = std::move(next);
	diagnostics_.snapshot_exact = casters_->exact;
	diagnostics_.caster_count = casters_->records.size();
	bump_epoch();
}

void TerrainStaticShadowPlanner::set_reports_caster_changes(bool on) {
	reports_caster_changes_ = on;
	if (!on) changed_reaches_.clear();
}

std::vector<TerrainStaticShadowReach>
TerrainStaticShadowPlanner::take_changed_reaches() {
	std::vector<TerrainStaticShadowReach> out;
	out.swap(changed_reaches_);
	return out;
}

void TerrainStaticShadowPlanner::reset_frame_diagnostics() {
	const bool exact = diagnostics_.snapshot_exact;
	const std::size_t count = diagnostics_.caster_count;
	diagnostics_ = TerrainStaticShadowPlannerDiagnostics{};
	diagnostics_.snapshot_exact = exact;
	diagnostics_.caster_count = count;
}

// Per-page compilation mirrors retail's per-tile collect-and-render walk:
// admitted casters whose model sphere reaches the sun-extended tile become
// draws of the selected shadow LOD's ROBJs in collector order
// [orig: Terrain_CollectAndRenderTileModels @ 0x60D250 — admission
// @ 0x60D421..0x60D450, sphere/tile test @ 0x60D465..0x60D54F,
// submit @ 0x60D881..0x60D971].
TerrainStaticShadowPageJob TerrainStaticShadowPlanner::compile(
		const TerrainTilePageKey &page) const {
	TerrainStaticShadowPageInput input;
	input.page = page;
	input.surface_to_light = world_light_;
	input.light_epoch = light_epoch_;
	TerrainStaticShadowPageJob job = casters_->collector.compile(input);
	job.content.value = mix_content(job.content.value, config_stamp_);
	return job;
}

// Retail evaluates a tile model's materials (AlphaGen, the complete UV
// transform, the diffuse flipbook frame) only inside the tile render: each
// visible model is submitted and flushed on the spot, and that flush samples
// the frame-shared tick. Resident tiles are never re-evaluated. The same
// evaluator runs here once per caster per classification or raster, from the
// tick the binding stamped on this planner for the job.
// [orig: Terrain_CollectAndRenderTileModels — Render_SubmitEntity @0x60D971
// then CRenderBatchQueue_SortAndFlush @0x60D97D per visible model;
// Material_ApplyShaderParameters @0x58DB80 inside that flush]
const TerrainStaticShadowPlanner::CasterMaterialStates &
TerrainStaticShadowPlanner::caster_material_states(MaterialStateTable &table,
		uint64_t caster_key,
		const TerrainStaticShadowPlannerCaster &caster) const {
	const auto cached = table.find(caster_key);
	if (cached != table.end()) return cached->second;
	const TerrainStaticShadowResolvedGeometry &geometry = *caster.geometry;
	CasterMaterialStates states;
	states.reserve(geometry.materials.size());
	for (const TerrainStaticShadowResolvedMaterial &material :
			geometry.materials) {
		states.push_back(terrain_static_shadow_evaluate_material(geometry,
				material, material_time_ms_, caster.control_values));
	}
	return table.emplace(caster_key, std::move(states)).first->second;
}

TerrainStaticShadowPlanner::DrawSupport
TerrainStaticShadowPlanner::classify_draw(
		const TerrainStaticShadowProjectionDraw &draw,
		MaterialStateTable &material_states) const {
	DrawSupport support;
	const auto caster_it = casters_->records.find(draw.caster_key);
	// lod_index caps at 1: retail models carry exactly two shadow-mesh slots
	// [orig: modelData[8]/modelData[9] @ 0x60d889..0x60d894].
	if (caster_it == casters_->records.end() ||
			caster_it->second.geometry == nullptr ||
			draw.geometry.lod_index > 1) {
		support.structurally_valid = false;
		support.supported = false;
		return support;
	}
	const TerrainStaticShadowResolvedGeometry &geometry =
			*caster_it->second.geometry;
	if (geometry.key != draw.geometry.geometry_key) {
		support.structurally_valid = false;
		support.supported = false;
		return support;
	}
	support.issues |= geometry.lod_unsupported_issues[
			draw.geometry.lod_index];
	const auto &coverage = geometry.coverage[draw.geometry.lod_index];
	if (draw.geometry.render_object_index >= coverage.size()) {
		support.structurally_valid = false;
		support.supported = false;
		return support;
	}
	const TerrainStaticShadowRenderObjectCoverage &state =
			coverage[draw.geometry.render_object_index];
	if (state.authored_surface_count != state.valid_surface_count) {
		support.issues |= kTerrainStaticShadowUnsupportedIncompleteGeometry;
	}
	if (state.malformed_indices) {
		support.issues |= kTerrainStaticShadowUnsupportedMalformedIndices;
	}
	if (state.missing_required_uvs) {
		support.issues |= kTerrainStaticShadowUnsupportedMissingRequiredUvs;
	}
	// One evaluation per caster per classification; every material index the
	// geometry resolver validated has a state (sizes match by construction).
	const CasterMaterialStates &states = caster_material_states(
			material_states, draw.caster_key, caster_it->second);
	for (const TerrainStaticShadowResolvedSurface &surface :
			geometry.surfaces[draw.geometry.lod_index]) {
		if (surface.render_object != draw.geometry.render_object_index) {
			continue;
		}
		if (surface.material_index < 0 ||
				surface.material_index >=
						static_cast<int>(geometry.materials.size())) {
			support.issues |= kTerrainStaticShadowUnsupportedInvalidMaterial;
			if (support.material_index < 0) {
				support.material_index = surface.material_index;
			}
			continue;
		}
		const TerrainStaticShadowResolvedMaterial &material =
				geometry.materials[static_cast<std::size_t>(
						surface.material_index)];
		if (!material.casts_projected_shadow) {
			continue;
		}
		const uint32_t material_issues = states[
				static_cast<std::size_t>(surface.material_index)].issues;
		if (material_issues != kTerrainStaticShadowUnsupportedNone &&
				support.material_index < 0) {
			support.material_index = surface.material_index;
		}
		support.issues |= material_issues;
	}
	support.supported =
			support.issues == kTerrainStaticShadowUnsupportedNone;
	return support;
}

void TerrainStaticShadowPlanner::record_unsupported_draw(
		const TerrainTilePageKey &page,
		const TerrainStaticShadowProjectionDraw &draw,
		const DrawSupport &support) {
	++diagnostics_.frame_unsupported_draw_count;
	if (diagnostics_.frame_unsupported_attribution.size() >=
			kMaxUnsupportedAttribution) {
		++diagnostics_.frame_unsupported_attribution_truncated;
		return;
	}
	TerrainStaticShadowUnsupportedAttribution row;
	row.page = page;
	row.bms_id = draw.bms_id;
	row.caster_key = draw.caster_key;
	row.lod_index = draw.geometry.lod_index;
	row.render_object_index = draw.geometry.render_object_index;
	row.material_index = support.material_index;
	row.issues = support.issues;
	const auto caster_it = casters_->records.find(draw.caster_key);
	if (caster_it != casters_->records.end()) {
		row.graphic = caster_it->second.graphic;
	}
	diagnostics_.frame_unsupported_attribution.push_back(std::move(row));
}

bool TerrainStaticShadowPlanner::classify_page_job(
		const TerrainStaticShadowPageJob &job,
		std::vector<uint8_t> *r_supported, bool record_unsupported) {
	if (!casters_->exact) return false;
	if (r_supported != nullptr) {
		r_supported->assign(job.draws.size(), 0);
	}
	MaterialStateTable material_states;
	for (std::size_t index = 0; index < job.draws.size(); ++index) {
		const auto &draw = job.draws[index];
		const DrawSupport support = classify_draw(draw, material_states);
		if (!support.structurally_valid) return false;
		if (support.supported) {
			if (r_supported != nullptr) (*r_supported)[index] = 1;
			continue;
		}
		if (record_unsupported) {
			record_unsupported_draw(job.page, draw, support);
		}
	}
	return true;
}

const TerrainStaticShadowPlanner::CachedPlan *
TerrainStaticShadowPlanner::plan_for(const TerrainTilePageKey &page) {
	const auto cached = plan_cache_.find(page);
	if (cached != plan_cache_.end()) {
		return &cached->second;
	}
	++diagnostics_.frame_plan_compiles;
	CachedPlan plan;
	plan.job = compile(page);
	if (classify_page_job(plan.job, &plan.supported, true)) {
		plan.valid = true;
		plan.supported_draws = static_cast<uint64_t>(std::count(
				plan.supported.begin(), plan.supported.end(),
				static_cast<uint8_t>(1)));
	}
	if (plan_cache_.size() >= kMaxCachedPlans) {
		plan_cache_.clear();
	}
	const auto emplaced = plan_cache_.emplace(page, std::move(plan));
	return &emplaced.first->second;
}

TerrainStaticShadowPagePlanResult TerrainStaticShadowPlanner::plan(
		const TerrainTilePageKey &page) {
	TerrainStaticShadowPagePlanResult result;
	if (!enabled_) {
		result.valid = true;
		return result;
	}
	++diagnostics_.frame_plan_count;
	try {
		const CachedPlan *cached = plan_for(page);
		if (cached == nullptr || !cached->valid) {
			++diagnostics_.frame_plan_failures;
			return result;
		}
		diagnostics_.frame_projection_draws += cached->supported_draws;
		if (cached->supported_draws != 0) {
			++diagnostics_.frame_pages_with_draws;
		}
		// Empty current coverage still owns an A-only no-op page. That makes
		// a moved/replaced/suppressed caster invalidate its old page instead
		// of reusing stale silhouette alpha.
		result.valid = true;
		result.raster_required = true;
		result.content = cached->job.content;
	} catch (const std::bad_alloc &) {
		++diagnostics_.frame_plan_failures;
		return {};
	}
	return result;
}

bool TerrainStaticShadowPlanner::rasterize(const TerrainTilePageKey &page,
		TerrainStaticShadowAlphaPage &page_alpha) {
	if (!enabled_ || !page_alpha.is_valid() ||
			!same_page(page_alpha.page, page)) {
		return false;
	}
	try {
		const CachedPlan *cached = plan_for(page);
		if (cached == nullptr || !cached->valid) return false;
		const TerrainStaticShadowPageJob &page_job = cached->job;
		const std::vector<uint8_t> &supported = cached->supported;
		if (page_job.content.value != page_alpha.content.value) return false;
		++diagnostics_.frame_raster_count;
		if (page_job.draws.empty()) return true;

		const auto build_started = std::chrono::steady_clock::now();
		TerrainStaticShadowRasterInput input;
		input.threads = raster_threads_;
		std::unordered_map<const TerrainStaticShadowAlphaPyramid *, int32_t>
				texture_indices;
		MaterialStateTable material_states;
		// Lay the page's triangles out in collector/ROBJ/strip order first
		// (the material states and texture slots resolve serially, in draw
		// order), then project disjoint runs of that same order on the lane
		// pool: the raster input is identical to a serial build.
		struct SurfaceRun {
			const TerrainStaticShadowPlannerCaster *caster = nullptr;
			const TerrainStaticShadowResolvedSurface *surface = nullptr;
			const TerrainStaticShadowResolvedMaterial *material = nullptr;
			const TerrainStaticShadowMaterialState *material_state = nullptr;
			int32_t texture_index = -1;
			std::size_t first_triangle = 0;
		};
		std::vector<SurfaceRun> runs;
		std::size_t triangle_total = 0;
		for (std::size_t draw_index = 0; draw_index < page_job.draws.size();
				++draw_index) {
			if (supported[draw_index] == 0) continue;
			const TerrainStaticShadowProjectionDraw &draw =
					page_job.draws[draw_index];
			const auto caster_it = casters_->records.find(draw.caster_key);
			if (caster_it == casters_->records.end() ||
					caster_it->second.geometry == nullptr ||
					caster_it->second.geometry->key !=
							draw.geometry.geometry_key ||
					// Same two-slot cap as classify_draw
					// [orig: @ 0x60d889..0x60d894].
					draw.geometry.lod_index > 1) {
				return false;
			}
			const TerrainStaticShadowPlannerCaster &caster = caster_it->second;
			const TerrainStaticShadowResolvedGeometry &geometry =
					*caster.geometry;
			const CasterMaterialStates &states = caster_material_states(
					material_states, draw.caster_key, caster);
			const std::vector<TerrainStaticShadowResolvedSurface> &surfaces =
					geometry.surfaces[draw.geometry.lod_index];
			for (const TerrainStaticShadowResolvedSurface &surface :
					surfaces) {
				if (surface.render_object !=
						draw.geometry.render_object_index) {
					continue;
				}
				if (surface.material_index < 0 ||
						surface.material_index >=
								static_cast<int>(geometry.materials.size())) {
					return false;
				}
				const TerrainStaticShadowResolvedMaterial &material =
						geometry.materials[static_cast<std::size_t>(
								surface.material_index)];
				if (!material.casts_projected_shadow) {
					continue;
				}
				const TerrainStaticShadowMaterialState &material_state =
						states[static_cast<std::size_t>(surface.material_index)];
				if (material_state.issues !=
						kTerrainStaticShadowUnsupportedNone) {
					return false;
				}
				int32_t texture_index = -1;
				const TerrainStaticShadowAlphaPyramid *identity =
						material_state.alpha_texture;
				if (identity != nullptr) {
					const auto existing = texture_indices.find(identity);
					if (existing != texture_indices.end()) {
						texture_index = existing->second;
					} else {
						texture_index = static_cast<int32_t>(
								input.alpha_textures.size());
						TerrainStaticShadowAlphaTextureView view;
						view.mips = identity->mips.data();
						view.mip_count = identity->mips.size();
						view.mip_filter = TerrainStaticShadowMipFilter::Linear;
						input.alpha_textures.push_back(view);
						texture_indices[identity] = texture_index;
					}
				}
				const std::size_t count = surface.indices.size() / 3;
				if (count == 0) continue;
				diagnostics_.frame_triangles += count;
				if (material.alpha_test_enabled) {
					diagnostics_.frame_alpha_test_triangles += count;
				}
				diagnostics_.frame_blend_triangles[
						static_cast<std::size_t>(material.blend)] += count;
				runs.push_back({ &caster, &surface, &material, &material_state,
						texture_index, triangle_total });
				triangle_total += count;
			}
		}
		input.triangles.resize(triangle_total);

		struct LaneResult {
			bool failed = false;
			bool has_bounds = false;
			float min_u = 0.0f, min_v = 0.0f, max_u = 0.0f, max_v = 0.0f;
		};
		// A lane per ~4k triangles, up to the raster's width.
		const std::size_t lanes = std::clamp<std::size_t>(
				triangle_total / 4096, 1, std::max<std::size_t>(raster_threads_, 1));
		std::vector<LaneResult> lane_results(lanes);
		run_row_stripe_lanes(lanes, [&](std::size_t lane) noexcept {
			LaneResult &result = lane_results[lane];
			const std::size_t begin = triangle_total * lane / lanes;
			const std::size_t end = triangle_total * (lane + 1) / lanes;
			if (begin >= end) return;
			// The run holding `begin`: the last run starting at or before it.
			std::size_t run_index = static_cast<std::size_t>(std::upper_bound(
					runs.begin(), runs.end(), begin,
					[](std::size_t value, const SurfaceRun &run) {
						return value < run.first_triangle;
					}) - runs.begin()) - 1;
			TerrainStaticShadowProjectionInput projection;
			projection.page = page_job.page;
			projection.surface_to_light = world_light_;
			for (std::size_t slot = begin; slot < end; ++slot) {
				while (run_index + 1 < runs.size() &&
						runs[run_index + 1].first_triangle <= slot) {
					++run_index;
				}
				const SurfaceRun &run = runs[run_index];
				const TerrainStaticShadowResolvedSurface &surface = *run.surface;
				const TerrainStaticShadowResolvedMaterial &material = *run.material;
				const TerrainStaticShadowMaterialState &material_state =
						*run.material_state;
				const TerrainStaticShadowPlannerCaster &caster = *run.caster;
				projection.caster_ground_y = caster.ground_y;
				const std::size_t index = (slot - run.first_triangle) * 3;
				TerrainStaticShadowRasterTriangle &triangle = input.triangles[slot];
				triangle.blend = material.blend;
				triangle.two_sided = material.two_sided;
				triangle.alpha_texture_index = run.texture_index;
				triangle.alpha_test_enabled = material.alpha_test_enabled;
				triangle.alpha_test_inverted =
						material.alpha_test_inverted;
				triangle.alpha_ref = material.alpha_ref;
				triangle.alpha_scale = material_state.alpha_scale;
				for (int corner = 0; corner < 3; ++corner) {
					const int32_t vertex_index = surface.indices[
							index + static_cast<std::size_t>(corner)];
					const std::array<float, 3> &local_base =
							surface.vertices[static_cast<std::size_t>(
									vertex_index)];
					const std::array<float, 3> local{
						local_base[0] + surface.render_object_offset[0],
						local_base[1] + surface.render_object_offset[1],
						local_base[2] + surface.render_object_offset[2],
					};
					const std::array<float, 3> world = transform_point(
							caster.world_transform, local);
					const std::array<float, 2> &uv = surface.uvs[
							static_cast<std::size_t>(vertex_index)];
					TerrainStaticShadowWorldVertex source;
					source.x = world[0];
					source.y = world[1];
					source.z = world[2];
					if (material.samples_diffuse_alpha) {
						source.texture_u =
								uv[0] * material_state.uv.m00 +
								uv[1] * material_state.uv.m10 +
								material_state.uv.m20;
						source.texture_v =
								uv[0] * material_state.uv.m01 +
								uv[1] * material_state.uv.m11 +
								material_state.uv.m21;
					} else {
						source.texture_u = uv[0];
						source.texture_v = uv[1];
					}
					if (!project_terrain_static_shadow_vertex(projection,
							source, triangle.vertices[corner])) {
						result.failed = true;
						return;
					}
					const TerrainStaticShadowRasterVertex &projected =
							triangle.vertices[corner];
					if (!result.has_bounds) {
						result.min_u = result.max_u = projected.page_u;
						result.min_v = result.max_v = projected.page_v;
						result.has_bounds = true;
					} else {
						result.min_u = std::min(result.min_u, projected.page_u);
						result.min_v = std::min(result.min_v, projected.page_v);
						result.max_u = std::max(result.max_u, projected.page_u);
						result.max_v = std::max(result.max_v, projected.page_v);
					}
				}
			}
		});
		for (const LaneResult &result : lane_results) {
			if (result.failed) return false;
			if (!result.has_bounds) continue;
			if (!diagnostics_.frame_has_projected_bounds) {
				diagnostics_.frame_min_u = result.min_u;
				diagnostics_.frame_min_v = result.min_v;
				diagnostics_.frame_max_u = result.max_u;
				diagnostics_.frame_max_v = result.max_v;
				diagnostics_.frame_has_projected_bounds = true;
			} else {
				diagnostics_.frame_min_u = std::min(diagnostics_.frame_min_u, result.min_u);
				diagnostics_.frame_min_v = std::min(diagnostics_.frame_min_v, result.min_v);
				diagnostics_.frame_max_u = std::max(diagnostics_.frame_max_u, result.max_u);
				diagnostics_.frame_max_v = std::max(diagnostics_.frame_max_v, result.max_v);
			}
		}
		diagnostics_.frame_triangle_build_us += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - build_started)
						.count());
		return rasterize_terrain_static_shadow_alpha(input, page_alpha);
	} catch (const std::bad_alloc &) {
		return false;
	}
}

} // namespace opennova::terrain
