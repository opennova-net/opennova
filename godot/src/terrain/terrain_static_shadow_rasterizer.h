#pragma once

// Godot resolver for the engine-owned terrain static-shadow collector/raster.
// MissionObjectPlacer supplies typed BMS/ObjectData sources; this adapter
// resolves the selected retail shadow LOD/ROBJ geometry and material alpha,
// then writes only the composed page-alpha carrier through the portable leaf.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250..0x60DA4F;
// PolyTrn_RenderTile @0x60E0C6..0x60E19D; see
// docs/terrain/terrain-re.md]

#include "terrain/terrain_tile_cache_device.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

namespace godot {

class MissionObjectPlacer;
class ObjectData;
class TerrainData;

class TerrainStaticShadowRasterizer final :
		public TerrainStaticShadowPageRasterizer {
public:
	TerrainStaticShadowRasterizer();
	~TerrainStaticShadowRasterizer() override;

	void set_terrain_data(const Ref<TerrainData> &p_data);
	void set_mission_object_placer(
			const Ref<MissionObjectPlacer> &p_placer);
	// A caster graphic's shadow geometry resolved and cached ahead of the
	// snapshot that will name it (the editor's mission device resolves each in
	// a unit of its own); true when it resolves. A cached geometry whose
	// document is freed is dropped at the next snapshot.
	bool prepare_caster_geometry(const String &p_graphic, const Ref<ObjectData> &p_data);
	void set_enabled(bool p_enabled);
	bool is_enabled() const noexcept;
	void set_suppressed_bms_ids(const PackedInt32Array &p_bms_ids);
	PackedInt32Array get_suppressed_bms_ids() const;
	Dictionary get_diagnostics() const;
	// Direct Environment_GetLightDirectionFloat tuple. The portable projector
	// owns its witnessed conversion into presentation-world axes.
	void begin_frame(const Vector3 &p_environment_light_tuple,
			uint32_t p_material_time_ms);

	std::shared_ptr<const opennova::terrain::TerrainStaticShadowCompilationSnapshot>
	compilation_snapshot() const override;
	uint32_t material_time_ms() const noexcept override;
	void merge_async_diagnostics(
			const opennova::terrain::TerrainStaticShadowPlannerDiagnostics
					&p_diagnostics) noexcept override;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace godot
