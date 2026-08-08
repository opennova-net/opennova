#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <terrain/builder.h>

#include <string>
#include <vector>

namespace godot {

class NovaTerrainBuildJob;

class NovaTerrainBuilder : public RefCounted {
	GDCLASS(NovaTerrainBuilder, RefCounted)

private:
	struct BuildExecutionResult {
		Error error = OK;
		std::string message;
	};

	static void _cleanup_intermediates(const std::string &output_dir,
	                                   const std::string &output_prefix);
	static BuildExecutionResult _build_from_data_impl(const std::vector<uint8_t> &heightmap_raw16,
	                                                  const std::string &output_dir,
	                                                  const std::string &terrain_name,
	                                                  const std::string &creator,
	                                                  const opennova::TerrainQuadrantLocks &quadrant_locks,
	                                                  opennova::DepthFormat depth_format = opennova::DepthFormat::CDEP,
	                                                  const opennova::TerrainBuildProgressCallback &progress_callback = {});

protected:
	static void _bind_methods();

public:
	NovaTerrainBuilder();
	~NovaTerrainBuilder();

	// Build CPT from raw uint16 heightmap data (1024x1024 = 2MB).
	// Uses no_smooth=true since the data is already 16-bit.
	// p_depth_format: 0 = DPTH (BHD-era, raw uint16), 1 = CDEP (JO/DFX-era, compressed).
	Error build_from_data(const PackedByteArray &p_heightmap_raw16,
	                      const String &p_output_dir,
	                      const String &p_terrain_name,
	                      const String &p_creator,
	                      int p_depth_format = 1,
	                      const PackedInt32Array &p_quadrant_locks = PackedInt32Array());

	Ref<NovaTerrainBuildJob> begin_build_from_data(const PackedByteArray &p_heightmap_raw16,
	                                               const String &p_output_dir,
	                                               const String &p_terrain_name,
	                                               const String &p_creator,
	                                               int p_depth_format = 1,
	                                               const PackedInt32Array &p_quadrant_locks = PackedInt32Array());

	// Save a Godot Image as an uncompressed 32-bit BGRA TGA (bottom-left origin).
	static Error save_image_tga(const Ref<Image> &p_image, const String &p_path);

	// Save a Godot Image as an uncompressed 24-bit BGR TGA (no alpha, bottom-left origin).
	static Error save_image_tga24(const Ref<Image> &p_image, const String &p_path);

	friend class NovaTerrainBuildJob;
};

} // namespace godot
