// Device leg only: FNV geometry cache keys, Texture2D mip extraction into the
// alpha pyramid, Transform3D flattening, and CptFile/TrnConfig marshalling.
// Every witnessed rule — the scanline walk, edge admission, mip LOD pick, the
// planner's placement math — lives cited in engine/runtime/terrain/
// terrain_static_shadow_{raster,planner,alpha,geometry}.cpp (see the class
// header's witness block and docs/terrain/terrain-re.md).
#include "terrain/nova_terrain_static_shadow_rasterizer.h"

#include "mission/nova_mission_object_placer.h"
#include "object/nova_object_data.h"
#include "terrain/nova_terrain_data.h"

#include <runtime/terrain/terrain_static_shadow.h>
#include <runtime/terrain/terrain_static_shadow_geometry.h>
#include <runtime/terrain/terrain_static_shadow_planner.h>

#include <runtime/terrain_query/height_field.h>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace godot {
namespace {

constexpr uint64_t kFnvOffset = UINT64_C(1469598103934665603);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

uint64_t hash_bytes(uint64_t p_hash, const void *p_data,
		std::size_t p_size) noexcept {
	const auto *bytes = static_cast<const uint8_t *>(p_data);
	for (std::size_t i = 0; i < p_size; ++i) {
		p_hash = (p_hash ^ bytes[i]) * kFnvPrime;
	}
	return p_hash;
}

template <typename T>
uint64_t hash_value(uint64_t p_hash, const T &p_value) noexcept {
	return hash_bytes(p_hash, &p_value, sizeof(p_value));
}

uint64_t hash_string(uint64_t p_hash, const String &p_value) noexcept {
	const CharString bytes = p_value.to_lower().utf8();
	return hash_bytes(p_hash, bytes.get_data(),
			static_cast<std::size_t>(bytes.length()));
}

opennova::terrain::TerrainStaticShadowLightDirection world_light(
		const Vector3 &p_environment_tuple) noexcept {
	// Static projection consumes Environment_GetLightDirectionFloat directly;
	// the later DOT3 bake alone truncates that tuple to D3DCOLOR bytes.
	// (retail: collector getters @0x60D2F5/0x60D2FF and projector setup
	// @0x60D800; see docs/terrain/terrain-re.md).
	return opennova::terrain::
			terrain_static_shadow_world_light_from_environment_tuple(
					static_cast<float>(p_environment_tuple.x),
					static_cast<float>(p_environment_tuple.y),
					static_cast<float>(p_environment_tuple.z));
}

void merge_planner_diagnostics(
		opennova::terrain::TerrainStaticShadowPlannerDiagnostics &target,
		const opennova::terrain::TerrainStaticShadowPlannerDiagnostics &source) {
	target.frame_plan_count += source.frame_plan_count;
	target.frame_plan_failures += source.frame_plan_failures;
	target.frame_plan_compiles += source.frame_plan_compiles;
	target.frame_pages_with_draws += source.frame_pages_with_draws;
	target.frame_projection_draws += source.frame_projection_draws;
	target.frame_raster_count += source.frame_raster_count;
	target.frame_triangles += source.frame_triangles;
	target.frame_alpha_test_triangles += source.frame_alpha_test_triangles;
	for (std::size_t index = 0; index < target.frame_blend_triangles.size();
			++index) {
		target.frame_blend_triangles[index] +=
				source.frame_blend_triangles[index];
	}
	target.frame_unsupported_draw_count +=
			source.frame_unsupported_draw_count;
	target.frame_unsupported_attribution_truncated +=
			source.frame_unsupported_attribution_truncated;
	constexpr std::size_t kAttributionLimit = 128;
	for (const auto &row : source.frame_unsupported_attribution) {
		if (target.frame_unsupported_attribution.size() >= kAttributionLimit) {
			++target.frame_unsupported_attribution_truncated;
			continue;
		}
		target.frame_unsupported_attribution.push_back(row);
	}
	if (source.frame_has_projected_bounds) {
		if (!target.frame_has_projected_bounds) {
			target.frame_min_u = source.frame_min_u;
			target.frame_min_v = source.frame_min_v;
			target.frame_max_u = source.frame_max_u;
			target.frame_max_v = source.frame_max_v;
			target.frame_has_projected_bounds = true;
		} else {
			target.frame_min_u = std::min(target.frame_min_u,
					source.frame_min_u);
			target.frame_min_v = std::min(target.frame_min_v,
					source.frame_min_v);
			target.frame_max_u = std::max(target.frame_max_u,
					source.frame_max_u);
			target.frame_max_v = std::max(target.frame_max_v,
					source.frame_max_v);
		}
	}
	target.frame_receiver_cache_hits += source.frame_receiver_cache_hits;
	target.frame_receiver_cache_misses += source.frame_receiver_cache_misses;
	target.snapshot_exact = target.snapshot_exact && source.snapshot_exact;
	target.caster_count = std::max(target.caster_count, source.caster_count);
}

// Godot Image -> engine alpha pyramid. The one genuinely device-bound leg of
// caster resolution: texture pixels decode here, policy lives in
// engine/runtime/terrain/terrain_static_shadow_geometry.
std::shared_ptr<const opennova::terrain::TerrainStaticShadowAlphaPyramid>
extract_alpha_pyramid(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) return {};
	const Ref<Image> source = p_texture->get_image();
	if (source.is_null() || source->is_empty() || source->get_width() <= 0 ||
			source->get_height() <= 0) {
		return {};
	}
	Ref<Image> image = Image::create_from_data(source->get_width(),
			source->get_height(), source->has_mipmaps(), source->get_format(),
			source->get_data());
	if (image.is_null()) return {};
	image->convert(Image::FORMAT_RGBA8);
	const PackedByteArray bytes = image->get_data();
	if (bytes.is_empty()) return {};

	auto result = std::make_shared<
			opennova::terrain::TerrainStaticShadowAlphaPyramid>();
	const int level_count = 1 + image->get_mipmap_count();
	result->storage.reserve(static_cast<std::size_t>(level_count));
	result->mips.reserve(static_cast<std::size_t>(level_count));
	for (int level = 0; level < level_count; ++level) {
		const int width = std::max(1, image->get_width() >> level);
		const int height = std::max(1, image->get_height() >> level);
		const int64_t offset = image->get_mipmap_offset(level);
		const int64_t rgba_size = static_cast<int64_t>(width) * height * 4;
		if (offset < 0 || rgba_size <= 0 || offset > bytes.size() - rgba_size) {
			return {};
		}
		std::vector<uint8_t> alpha(
				static_cast<std::size_t>(width) * height);
		for (int64_t texel = 0;
				texel < static_cast<int64_t>(width) * height; ++texel) {
			alpha[static_cast<std::size_t>(texel)] =
					bytes[offset + texel * 4 + 3];
		}
		result->storage.push_back(std::move(alpha));
	}
	for (int level = 0; level < level_count; ++level) {
		const uint32_t width = static_cast<uint32_t>(
				std::max(1, image->get_width() >> level));
		const uint32_t height = static_cast<uint32_t>(
				std::max(1, image->get_height() >> level));
		result->mips.push_back({width, height, width,
				result->storage[static_cast<std::size_t>(level)].data()});
	}
	return result;
}

// Per-resolve texture provider over the source document's resolver.
class ObjectDataTextureProvider final :
		public opennova::terrain::TerrainStaticShadowTextureProvider {
public:
	explicit ObjectDataTextureProvider(const Ref<ObjectData> &p_data) :
			data_(p_data) {}

	std::shared_ptr<const opennova::terrain::TerrainStaticShadowAlphaPyramid>
	load_alpha(std::string_view p_texture_name) override {
		if (data_.is_null() || p_texture_name.empty()) return {};
		const String name = String::utf8(p_texture_name.data(),
				static_cast<int>(p_texture_name.size()));
		return extract_alpha_pyramid(data_->load_texture_name(name));
	}

private:
	Ref<ObjectData> data_;
};

std::array<float, 12> transform_rows(const Transform3D &p_transform) noexcept {
	std::array<float, 12> out{};
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column) {
			out[static_cast<std::size_t>(row) * 3 + column] =
					static_cast<float>(p_transform.basis[row][column]);
		}
	}
	for (int axis = 0; axis < 3; ++axis) {
		out[9 + static_cast<std::size_t>(axis)] =
				static_cast<float>(p_transform.origin[axis]);
	}
	return out;
}

} // namespace

class TerrainStaticShadowRasterizer::Impl {
public:
	Ref<TerrainData> terrain_data;
	Ref<MissionObjectPlacer> placer;
	bool enabled = true;
	PackedInt32Array suppressed_bms_ids;
	mutable opennova::terrain::TerrainStaticShadowPlanner planner;
	std::shared_ptr<const TerrainStaticShadowReceiverStorage> receiver_storage;
	mutable std::shared_ptr<const TerrainStaticShadowCompilationSnapshot>
			compilation_snapshot;
	mutable opennova::terrain::TerrainStaticShadowPlannerDiagnostics
			async_diagnostics;
	mutable opennova::terrain::TerrainStaticShadowPlannerDiagnostics
			async_epoch_diagnostics;
	std::unordered_map<uint64_t, std::shared_ptr<
			const opennova::terrain::TerrainStaticShadowResolvedGeometry>>
			geometry_cache;
	uint64_t observed_source_revision = 0;
	uint64_t observed_terrain_revision = 0;
	uint64_t observed_object_global_counter = 0;
	bool have_receiver = false;
	// Identity of the last refresh_receiver_terrain attempt. The outcome is a
	// pure function of (document, revision), so a repeat attempt is skipped —
	// success or failure — until the terrain document actually changes.
	uint64_t receiver_attempt_id = ~UINT64_C(0);
	uint64_t receiver_attempt_revision = ~UINT64_C(0);

	std::shared_ptr<const opennova::terrain::TerrainStaticShadowResolvedGeometry>
	geometry_for(const String &p_graphic, const Ref<ObjectData> &p_data) {
		if (p_data.is_null()) return {};
		uint64_t lookup = hash_string(kFnvOffset, p_graphic);
		lookup = hash_value(lookup,
				static_cast<uint64_t>(p_data->get_instance_id()));
		lookup = hash_value(lookup, p_data->get_change_revision());
		const auto cached = geometry_cache.find(lookup);
		if (cached != geometry_cache.end()) return cached->second;
		const CharString graphic_bytes = p_graphic.to_lower().utf8();
		ObjectDataTextureProvider provider(p_data);
		std::shared_ptr<const opennova::terrain::
				TerrainStaticShadowResolvedGeometry> resolved =
				opennova::terrain::resolve_terrain_static_shadow_geometry(
						p_data->native_model(),
						std::string_view(graphic_bytes.get_data(),
								static_cast<std::size_t>(
										graphic_bytes.length())),
						provider);
		if (resolved != nullptr) geometry_cache[lookup] = resolved;
		return resolved;
	}

	// Marshal the placer's typed sources into planner caster records.
	void rebuild_snapshot() {
		observed_object_global_counter =
				ObjectData::get_global_change_counter();
		std::vector<opennova::terrain::TerrainStaticShadowPlannerCaster>
				records;
		bool admitted_geometry_missing = false;
		observed_source_revision = placer.is_valid()
				? placer->get_static_terrain_shadow_source_revision()
				: 0;
		observed_terrain_revision = terrain_data.is_valid()
				? terrain_data->get_change_revision()
				: 0;
		std::vector<int32_t> suppressed;
		suppressed.reserve(static_cast<std::size_t>(
				suppressed_bms_ids.size()));
		for (int index = 0; index < suppressed_bms_ids.size(); ++index) {
			suppressed.push_back(suppressed_bms_ids[index]);
		}
		planner.set_enabled(enabled);
		planner.set_suppressed_bms_ids(std::move(suppressed));
		if (!enabled || placer.is_null()) {
			planner.replace_casters({}, false);
			return;
		}
		const Vector<MissionObjectPlacer::StaticTerrainShadowSource> sources =
				placer->get_static_terrain_shadow_sources();
		records.reserve(static_cast<std::size_t>(sources.size()));
		for (const MissionObjectPlacer::StaticTerrainShadowSource &source :
				sources) {
			// The planner canonicalizes and applies suppression on its own
			// list; keep resolving unsuppressed sources only, as before.
			if (planner.suppressed_bms_ids().end() != std::find(
					planner.suppressed_bms_ids().begin(),
					planner.suppressed_bms_ids().end(), source.bms_id)) {
				continue;
			}
			const bool admitted = source.active &&
					opennova::mission::item_casts_static_terrain_shadow(
							source.entity_kind, source.entity_attrib,
							source.item_attrib, source.item_attrib2);
			const std::shared_ptr<const opennova::terrain::
					TerrainStaticShadowResolvedGeometry> geometry =
					geometry_for(source.graphic, source.object_data);
			if (geometry == nullptr) {
				if (admitted) admitted_geometry_missing = true;
				continue;
			}
			opennova::terrain::TerrainStaticShadowPlannerCaster record;
			record.bms_id = source.bms_id;
			record.entity_kind = source.entity_kind;
			record.entity_index = source.entity_index;
			record.team = source.team;
			// The sector-model submit writes TEX_TEAM immediately before the
			// object enters the retail batch queue. This collector has no other
			// explicit CTRL writer; inherited process-global ordering remains
			// tracked by D-3DI-2 instead of being misrepresented as caster-local.
			// The submit-site witness lives with the present row contract in
			// engine/runtime/world/present_rows.h (TEX_TEAM control value).
			record.control_values[THREEDI_CTRL_TEX_TEAM] = source.team;
			record.entity_attrib = source.entity_attrib;
			record.item_attrib = source.item_attrib;
			record.item_attrib2 = source.item_attrib2;
			record.active = source.active;
			const CharString graphic_bytes = source.graphic.utf8();
			record.graphic.assign(graphic_bytes.get_data(),
					static_cast<std::size_t>(graphic_bytes.length()));
			record.world_transform = transform_rows(source.world_transform);
			record.geometry = geometry;
			// Point sampler, deliberately: retail grounds each caster at the
			// entity origin, not by bilinear interpolation.
			record.ground_y = terrain_data.is_valid()
					? terrain_data->get_height_world(source.world_transform.origin)
					: 0.0f;
			record.caster_identity = source.object_data.is_valid()
					? static_cast<uint64_t>(
							source.object_data->get_instance_id())
					: 0;
			records.push_back(std::move(record));
		}
		planner.replace_casters(std::move(records),
				admitted_geometry_missing);
	}

	void refresh_receiver_terrain() {
		const uint64_t source_id = terrain_data.is_valid()
				? static_cast<uint64_t>(terrain_data->get_instance_id())
				: 0;
		const uint64_t source_revision = terrain_data.is_valid()
				? terrain_data->get_change_revision()
				: 0;
		if (receiver_attempt_id == source_id &&
				receiver_attempt_revision == source_revision) {
			return;
		}
		receiver_attempt_id = source_id;
		receiver_attempt_revision = source_revision;
		if (terrain_data.is_null()) {
			planner.clear_receiver_terrain();
			receiver_storage.reset();
			have_receiver = false;
			return;
		}
		const opennova::CptFile &cpt = terrain_data->get_cpt();
		const int dimension = static_cast<int>(std::sqrt(
				static_cast<double>(cpt.depth_buffer.size())));
		if (dimension <= 0 ||
				static_cast<std::size_t>(dimension) * dimension !=
						cpt.depth_buffer.size()) {
			planner.clear_receiver_terrain();
			receiver_storage.reset();
			have_receiver = false;
			return;
		}
		auto mutable_receiver =
				std::make_shared<TerrainStaticShadowReceiverStorage>();
		mutable_receiver->heightmap = cpt.depth_buffer;
		const opennova::TrnConfig &trn = terrain_data->get_trn();
		std::copy_n(&trn.sector_grid[0][0],
				mutable_receiver->sector_grid.size(),
				mutable_receiver->sector_grid.begin());
		receiver_storage = std::move(mutable_receiver);
		opennova::terrain::TerrainHeightField field;
		field.heightmap = receiver_storage->heightmap.data();
		field.dim = dimension;
		field.layout.sector_grid = receiver_storage->sector_grid.data();
		height_field_apply_trn(field, trn);
		if (!field.valid()) {
			planner.clear_receiver_terrain();
			receiver_storage.reset();
			have_receiver = false;
			return;
		}
		planner.set_receiver_terrain(field, observed_terrain_revision);
		have_receiver = true;
	}
};

TerrainStaticShadowRasterizer::TerrainStaticShadowRasterizer() :
		impl_(std::make_unique<Impl>()) {}

TerrainStaticShadowRasterizer::~TerrainStaticShadowRasterizer() = default;

void TerrainStaticShadowRasterizer::set_terrain_data(
		const Ref<TerrainData> &p_data) {
	impl_->terrain_data = p_data;
	impl_->observed_terrain_revision = p_data.is_valid()
			? p_data->get_change_revision()
			: 0;
	impl_->refresh_receiver_terrain();
	impl_->rebuild_snapshot();
}

void TerrainStaticShadowRasterizer::set_mission_object_placer(
		const Ref<MissionObjectPlacer> &p_placer) {
	impl_->placer = p_placer;
	impl_->rebuild_snapshot();
}

void TerrainStaticShadowRasterizer::set_enabled(bool p_enabled) {
	if (impl_->enabled == p_enabled) return;
	impl_->enabled = p_enabled;
	impl_->rebuild_snapshot();
}

bool TerrainStaticShadowRasterizer::is_enabled() const noexcept {
	return impl_->enabled;
}

void TerrainStaticShadowRasterizer::set_suppressed_bms_ids(
		const PackedInt32Array &p_bms_ids) {
	// The planner canonicalizes (sorts + dedups); compare canonically so an
	// equivalent list stays a no-op.
	std::vector<int32_t> next;
	next.reserve(static_cast<std::size_t>(p_bms_ids.size()));
	for (int index = 0; index < p_bms_ids.size(); ++index) {
		next.push_back(p_bms_ids[index]);
	}
	std::sort(next.begin(), next.end());
	next.erase(std::unique(next.begin(), next.end()), next.end());
	if (next == impl_->planner.suppressed_bms_ids()) return;
	impl_->suppressed_bms_ids = p_bms_ids;
	impl_->rebuild_snapshot();
}

PackedInt32Array TerrainStaticShadowRasterizer::get_suppressed_bms_ids() const {
	const std::vector<int32_t> &ids = impl_->planner.suppressed_bms_ids();
	PackedInt32Array result;
	result.resize(static_cast<int64_t>(ids.size()));
	for (int index = 0; index < static_cast<int>(ids.size()); ++index) {
		result.set(index, ids[static_cast<std::size_t>(index)]);
	}
	return result;
}

Dictionary TerrainStaticShadowRasterizer::get_diagnostics() const {
	opennova::terrain::TerrainStaticShadowPlannerDiagnostics frame =
			impl_->planner.diagnostics();
	merge_planner_diagnostics(frame, impl_->async_diagnostics);
	Dictionary diagnostics;
	diagnostics["enabled"] = impl_->enabled;
	diagnostics["source_revision"] =
			static_cast<int64_t>(impl_->observed_source_revision);
	diagnostics["terrain_revision"] =
			static_cast<int64_t>(impl_->observed_terrain_revision);
	diagnostics["candidate_count"] =
			static_cast<int64_t>(impl_->planner.candidate_count());
	diagnostics["admitted_count"] =
			static_cast<int64_t>(impl_->planner.admitted_count());
	diagnostics["resolved_casters"] =
			static_cast<int64_t>(impl_->planner.caster_count());
	diagnostics["snapshot_exact"] = impl_->planner.snapshot_exact();
	diagnostics["frame_plan_count"] =
			static_cast<int64_t>(frame.frame_plan_count);
	diagnostics["frame_plan_failures"] =
			static_cast<int64_t>(frame.frame_plan_failures);
	diagnostics["frame_plan_compiles"] =
			static_cast<int64_t>(frame.frame_plan_compiles);
	diagnostics["frame_pages_with_draws"] =
			static_cast<int64_t>(frame.frame_pages_with_draws);
	diagnostics["frame_projection_draws"] =
			static_cast<int64_t>(frame.frame_projection_draws);
	diagnostics["frame_raster_count"] =
			static_cast<int64_t>(frame.frame_raster_count);
	diagnostics["frame_triangles"] =
			static_cast<int64_t>(frame.frame_triangles);
	diagnostics["frame_alpha_test_triangles"] =
			static_cast<int64_t>(frame.frame_alpha_test_triangles);
	diagnostics["frame_opaque_triangles"] =
			static_cast<int64_t>(frame.frame_blend_triangles[
					static_cast<std::size_t>(opennova::terrain::
							TerrainStaticShadowBlend::Opaque)]);
	diagnostics["frame_alpha_blend_triangles"] =
			static_cast<int64_t>(frame.frame_blend_triangles[
					static_cast<std::size_t>(opennova::terrain::
							TerrainStaticShadowBlend::Alpha)]);
	diagnostics["frame_additive_triangles"] =
			static_cast<int64_t>(frame.frame_blend_triangles[
					static_cast<std::size_t>(opennova::terrain::
							TerrainStaticShadowBlend::Additive)]);
	diagnostics["frame_multiply_triangles"] =
			static_cast<int64_t>(frame.frame_blend_triangles[
					static_cast<std::size_t>(opennova::terrain::
							TerrainStaticShadowBlend::Multiply)]);
	diagnostics["frame_unsupported_draw_count"] =
			static_cast<int64_t>(frame.frame_unsupported_draw_count);
	diagnostics["frame_unsupported_attribution_truncated"] =
			static_cast<int64_t>(frame.frame_unsupported_attribution_truncated);
	Array unsupported_rows;
	for (const opennova::terrain::TerrainStaticShadowUnsupportedAttribution
			&source : frame.frame_unsupported_attribution) {
		Dictionary row;
		row["sector_origin_x"] = source.page.sector_origin_x;
		row["sector_origin_z"] = source.page.sector_origin_z;
		row["page_local_x"] = source.page.page_local_x;
		row["page_local_z"] = source.page.page_local_z;
		row["page_lod_level"] = source.page.page_lod_level;
		row["bms_id"] = source.bms_id;
		row["caster_key"] = static_cast<int64_t>(source.caster_key);
		row["graphic"] = String::utf8(source.graphic.c_str(),
				static_cast<int>(source.graphic.size()));
		row["lod_index"] = source.lod_index;
		row["render_object_index"] = source.render_object_index;
		row["material_index"] = source.material_index;
		row["reason_flags"] = static_cast<int64_t>(source.issues);
		PackedStringArray reasons;
		for (const char *reason : opennova::terrain::
				terrain_static_shadow_unsupported_reason_names(
						source.issues)) {
			reasons.push_back(String(reason));
		}
		row["reasons"] = reasons;
		unsupported_rows.push_back(row);
	}
	diagnostics["frame_unsupported_attribution"] = unsupported_rows;
	diagnostics["frame_has_projected_bounds"] =
			frame.frame_has_projected_bounds;
	if (frame.frame_has_projected_bounds) {
		diagnostics["frame_projected_uv_min"] =
				Vector2(frame.frame_min_u, frame.frame_min_v);
		diagnostics["frame_projected_uv_max"] =
				Vector2(frame.frame_max_u, frame.frame_max_v);
	}
	diagnostics["frame_receiver_cache_hits"] =
			static_cast<int64_t>(frame.frame_receiver_cache_hits);
	diagnostics["frame_receiver_cache_misses"] =
			static_cast<int64_t>(frame.frame_receiver_cache_misses);
	const auto &epoch = impl_->async_epoch_diagnostics;
	diagnostics["epoch_plan_count"] =
			static_cast<int64_t>(epoch.frame_plan_count);
	diagnostics["epoch_plan_failures"] =
			static_cast<int64_t>(epoch.frame_plan_failures);
	diagnostics["epoch_plan_compiles"] =
			static_cast<int64_t>(epoch.frame_plan_compiles);
	diagnostics["epoch_pages_with_draws"] =
			static_cast<int64_t>(epoch.frame_pages_with_draws);
	diagnostics["epoch_projection_draws"] =
			static_cast<int64_t>(epoch.frame_projection_draws);
	diagnostics["epoch_raster_count"] =
			static_cast<int64_t>(epoch.frame_raster_count);
	diagnostics["epoch_triangles"] =
			static_cast<int64_t>(epoch.frame_triangles);
	diagnostics["epoch_alpha_test_triangles"] =
			static_cast<int64_t>(epoch.frame_alpha_test_triangles);
	diagnostics["epoch_unsupported_draw_count"] =
			static_cast<int64_t>(epoch.frame_unsupported_draw_count);
	diagnostics["epoch_unsupported_attribution_truncated"] =
			static_cast<int64_t>(
					epoch.frame_unsupported_attribution_truncated);
	Array epoch_unsupported_rows;
	for (const auto &source : epoch.frame_unsupported_attribution) {
		Dictionary row;
		row["sector_origin_x"] = source.page.sector_origin_x;
		row["sector_origin_z"] = source.page.sector_origin_z;
		row["page_local_x"] = source.page.page_local_x;
		row["page_local_z"] = source.page.page_local_z;
		row["page_lod_level"] = source.page.page_lod_level;
		row["bms_id"] = source.bms_id;
		row["caster_key"] = static_cast<int64_t>(source.caster_key);
		row["graphic"] = String::utf8(source.graphic.c_str(),
				static_cast<int>(source.graphic.size()));
		row["lod_index"] = source.lod_index;
		row["render_object_index"] = source.render_object_index;
		row["material_index"] = source.material_index;
		row["reason_flags"] = static_cast<int64_t>(source.issues);
		PackedStringArray reasons;
		for (const char *reason : opennova::terrain::
				terrain_static_shadow_unsupported_reason_names(source.issues)) {
			reasons.push_back(String(reason));
		}
		row["reasons"] = reasons;
		epoch_unsupported_rows.push_back(row);
	}
	diagnostics["epoch_unsupported_attribution"] = epoch_unsupported_rows;
	diagnostics["epoch_has_projected_bounds"] =
			epoch.frame_has_projected_bounds;
	if (epoch.frame_has_projected_bounds) {
		diagnostics["epoch_projected_uv_min"] =
				Vector2(epoch.frame_min_u, epoch.frame_min_v);
		diagnostics["epoch_projected_uv_max"] =
				Vector2(epoch.frame_max_u, epoch.frame_max_v);
	}
	diagnostics["epoch_receiver_cache_hits"] =
			static_cast<int64_t>(epoch.frame_receiver_cache_hits);
	diagnostics["epoch_receiver_cache_misses"] =
			static_cast<int64_t>(epoch.frame_receiver_cache_misses);
	return diagnostics;
}

void TerrainStaticShadowRasterizer::begin_frame(
		const Vector3 &p_environment_light_tuple,
		uint32_t p_material_time_ms) {
	impl_->planner.reset_frame_diagnostics();
	impl_->async_diagnostics = {};
	impl_->async_diagnostics.snapshot_exact =
			impl_->planner.snapshot_exact();
	impl_->async_diagnostics.caster_count = impl_->planner.caster_count();
	impl_->planner.set_light(world_light(p_environment_light_tuple),
			opennova::terrain::terrain_tile_light_epoch_from_environment_tuple(
					static_cast<float>(p_environment_light_tuple.x),
					static_cast<float>(p_environment_light_tuple.y),
					static_cast<float>(p_environment_light_tuple.z)));
	impl_->planner.set_material_time(p_material_time_ms);
	const uint64_t terrain_revision = impl_->terrain_data.is_valid()
			? impl_->terrain_data->get_change_revision()
			: 0;
	const uint64_t source_revision = impl_->placer.is_valid()
			? impl_->placer->get_static_terrain_shadow_source_revision()
			: 0;
	const bool terrain_changed =
			terrain_revision != impl_->observed_terrain_revision;
	// One global counter compare gates the geometry-revision concern: caster
	// documents only change through ObjectData mutation, which bumps it.
	const bool objects_changed = ObjectData::get_global_change_counter() !=
			impl_->observed_object_global_counter;
	if (terrain_changed || source_revision != impl_->observed_source_revision ||
			objects_changed) {
		impl_->observed_terrain_revision = terrain_revision;
		if (terrain_changed || !impl_->have_receiver) {
			// The attempt-identity gate inside makes a receiver-less retry
			// free until the terrain document actually changes.
			impl_->refresh_receiver_terrain();
		}
		impl_->rebuild_snapshot();
	}
}

opennova::terrain::TerrainStaticShadowPagePlanResult
TerrainStaticShadowRasterizer::plan_page(
		const opennova::TerrainTilePageKey &p_page) {
	return impl_->planner.plan(p_page);
}

std::shared_ptr<const TerrainStaticShadowCompilationSnapshot>
TerrainStaticShadowRasterizer::compilation_snapshot() const {
	// The revision moves only on a structural change (caster set, light
	// quantum, receiver terrain, config); material time is per job, so a
	// steady world shares one snapshot across frames. The planner copy below
	// shares the immutable caster set by pointer and duplicates only the
	// per-page memo caches.
	const uint64_t revision = impl_->planner.state_revision();
	if (impl_->compilation_snapshot == nullptr ||
			impl_->compilation_snapshot->revision != revision) {
		auto snapshot =
				std::make_shared<TerrainStaticShadowCompilationSnapshot>();
		snapshot->revision = revision;
		snapshot->receiver_storage = impl_->receiver_storage;
		snapshot->planner = impl_->planner;
		impl_->compilation_snapshot = std::move(snapshot);
		impl_->async_epoch_diagnostics = {};
		impl_->async_epoch_diagnostics.snapshot_exact =
				impl_->planner.snapshot_exact();
		impl_->async_epoch_diagnostics.caster_count =
				impl_->planner.caster_count();
	}
	return impl_->compilation_snapshot;
}

uint32_t TerrainStaticShadowRasterizer::material_time_ms() const noexcept {
	return impl_->planner.material_time_ms();
}

void TerrainStaticShadowRasterizer::merge_async_diagnostics(
		const opennova::terrain::TerrainStaticShadowPlannerDiagnostics
				&p_diagnostics) noexcept {
	merge_planner_diagnostics(impl_->async_diagnostics, p_diagnostics);
	merge_planner_diagnostics(impl_->async_epoch_diagnostics, p_diagnostics);
}

} // namespace godot
