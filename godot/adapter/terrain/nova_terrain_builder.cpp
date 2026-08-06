#include "nova_terrain_builder.h"

#include "nova_terrain_build_job.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace godot {

namespace {

std::vector<uint8_t> to_byte_vector(const PackedByteArray &bytes) {
	std::vector<uint8_t> output;
	output.resize(bytes.size());
	if (!output.empty()) {
		std::memcpy(output.data(), bytes.ptr(), output.size());
	}
	return output;
}

void push_error_message(const std::string &message) {
	if (!message.empty()) {
		UtilityFunctions::push_error(message.c_str());
	}
}

opennova::TerrainQuadrantLocks to_quadrant_locks(
		const PackedInt32Array &values) {
	opennova::TerrainQuadrantLocks locks{};
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		const int base = quadrant * 2;
		locks[quadrant].x = base < values.size() ? values[base] : 0;
		locks[quadrant].y = base + 1 < values.size() ? values[base + 1] : 0;
	}
	return locks;
}

} // namespace

NovaTerrainBuilder::NovaTerrainBuilder() = default;
NovaTerrainBuilder::~NovaTerrainBuilder() = default;

void NovaTerrainBuilder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build_from_data", "heightmap_raw16", "output_dir", "terrain_name", "creator", "depth_format", "quadrant_locks"),
	                     &NovaTerrainBuilder::build_from_data, DEFVAL(1), DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("begin_build_from_data", "heightmap_raw16", "output_dir", "terrain_name", "creator", "depth_format", "quadrant_locks"),
	                     &NovaTerrainBuilder::begin_build_from_data, DEFVAL(1), DEFVAL(PackedInt32Array()));
	ClassDB::bind_static_method("NovaTerrainBuilder",
	                            D_METHOD("save_image_tga", "image", "path"),
	                            &NovaTerrainBuilder::save_image_tga);
	ClassDB::bind_static_method("NovaTerrainBuilder",
	                            D_METHOD("save_image_tga24", "image", "path"),
	                            &NovaTerrainBuilder::save_image_tga24);
}

NovaTerrainBuilder::BuildExecutionResult NovaTerrainBuilder::_build_from_data_impl(
		const std::vector<uint8_t> &heightmap_raw16,
		const std::string &output_dir,
		const std::string &terrain_name,
		const std::string &creator,
		const opennova::TerrainQuadrantLocks &quadrant_locks,
		opennova::DepthFormat depth_format,
		const opennova::TerrainBuildProgressCallback &progress_callback) {
	constexpr int EXPECTED_SIZE = 1024 * 1024 * 2;
	if (static_cast<int>(heightmap_raw16.size()) != EXPECTED_SIZE) {
		std::ostringstream message;
		message << "NovaTerrainBuilder: heightmap must be " << EXPECTED_SIZE
		        << " bytes (1024x1024 uint16), got " << heightmap_raw16.size();
		return {ERR_INVALID_PARAMETER, message.str()};
	}

	fs::create_directories(output_dir);
	std::string depthmap_path = output_dir + "/_temp_depth.raw";

	FILE *f = std::fopen(depthmap_path.c_str(), "wb");
	if (!f) {
		return {ERR_FILE_CANT_WRITE,
		        "NovaTerrainBuilder: failed to write temp depthmap: " + depthmap_path};
	}
	std::fwrite(heightmap_raw16.data(), 1, heightmap_raw16.size(), f);
	std::fclose(f);

	opennova::TpjProject project;
	project.terrain_name = terrain_name;
	project.creator = creator;
	project.path = "";
	project.depthmap = depthmap_path;
	project.output = terrain_name.empty() ? "terrain" : terrain_name;
	project.lock_topleft = quadrant_locks[0];
	project.lock_topright = quadrant_locks[1];
	project.lock_bottomleft = quadrant_locks[2];
	project.lock_bottomright = quadrant_locks[3];

	try {
		opennova::TerrainBuildOptions options;
		options.depth_format = depth_format;
		options.smooth_depthmap = false;
		options.rasterize_depth = true;
		opennova::build_terrain(project, output_dir, options, progress_callback);
	} catch (const std::exception &e) {
		std::remove(depthmap_path.c_str());
		return {ERR_SCRIPT_FAILED, std::string("NovaTerrainBuilder: build failed: ") + e.what()};
	}

	std::remove(depthmap_path.c_str());
	_cleanup_intermediates(output_dir, project.output);
	return {OK, ""};
}

// Translate the int the GDScript side passes us into the lib's enum.
// 0 = DPTH (BHD-era raw uint16), 1 = CDEP (JO/DFX-era compressed). Anything else
// snaps to CDEP - keeping the build alive is preferable to refusing on a typo.
static opennova::DepthFormat _depth_format_from_int(int value) {
	if (value == 0) return opennova::DepthFormat::DPTH;
	return opennova::DepthFormat::CDEP;
}

Error NovaTerrainBuilder::build_from_data(const PackedByteArray &p_heightmap_raw16,
                                          const String &p_output_dir,
                                          const String &p_terrain_name,
                                          const String &p_creator,
                                          int p_depth_format,
                                          const PackedInt32Array &p_quadrant_locks) {
	auto result = _build_from_data_impl(
		to_byte_vector(p_heightmap_raw16),
		p_output_dir.utf8().get_data(),
		p_terrain_name.utf8().get_data(),
		p_creator.utf8().get_data(),
		to_quadrant_locks(p_quadrant_locks),
		_depth_format_from_int(p_depth_format));
	push_error_message(result.message);
	return result.error;
}

Ref<NovaTerrainBuildJob> NovaTerrainBuilder::begin_build_from_data(
		const PackedByteArray &p_heightmap_raw16,
		const String &p_output_dir,
		const String &p_terrain_name,
		const String &p_creator,
		int p_depth_format,
		const PackedInt32Array &p_quadrant_locks) {
	Ref<NovaTerrainBuildJob> job;
	job.instantiate();
	job->_start_data(
		to_byte_vector(p_heightmap_raw16),
		p_output_dir.utf8().get_data(),
		p_terrain_name.utf8().get_data(),
		p_creator.utf8().get_data(),
		_depth_format_from_int(p_depth_format),
		to_quadrant_locks(p_quadrant_locks));
	return job;
}

void NovaTerrainBuilder::_cleanup_intermediates(const std::string &output_dir,
                                                const std::string &output_prefix) {
	try {
		for (auto &entry : fs::directory_iterator(output_dir)) {
			auto path = entry.path().string();
			auto ext = entry.path().extension().string();
			if (ext == ".tml" || ext == ".tms" || ext == ".dep") {
				auto filename = entry.path().filename().string();
				if (filename.rfind(output_prefix, 0) == 0) {
					std::remove(path.c_str());
				}
			}
		}
	} catch (...) {
	}
}

Error NovaTerrainBuilder::save_image_tga(const Ref<Image> &p_image, const String &p_path) {
	if (p_image.is_null() || p_image->is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	Ref<Image> img = p_image;
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img = img->duplicate();
		img->convert(Image::FORMAT_RGBA8);
	}

	int w = img->get_width();
	int h = img->get_height();
	PackedByteArray data = img->get_data();

	std::string path = p_path.utf8().get_data();
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		return ERR_FILE_CANT_WRITE;
	}

	uint8_t header[18] = {};
	header[2] = 2;
	header[12] = w & 0xFF;
	header[13] = w >> 8;
	header[14] = h & 0xFF;
	header[15] = h >> 8;
	header[16] = 32;
	header[17] = 0x08;
	std::fwrite(header, 1, 18, f);

	const uint8_t *src = data.ptr();
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			int i = y * w + x;
			uint8_t bgra[4] = {src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0], src[i * 4 + 3]};
			std::fwrite(bgra, 1, 4, f);
		}
	}

	std::fclose(f);
	return OK;
}

Error NovaTerrainBuilder::save_image_tga24(const Ref<Image> &p_image, const String &p_path) {
	if (p_image.is_null() || p_image->is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	Ref<Image> img = p_image;
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img = img->duplicate();
		img->convert(Image::FORMAT_RGBA8);
	}

	int w = img->get_width();
	int h = img->get_height();
	PackedByteArray data = img->get_data();

	std::string path = p_path.utf8().get_data();
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		return ERR_FILE_CANT_WRITE;
	}

	uint8_t header[18] = {};
	header[2] = 2;
	header[12] = w & 0xFF;
	header[13] = w >> 8;
	header[14] = h & 0xFF;
	header[15] = h >> 8;
	header[16] = 24;
	header[17] = 0x00;
	std::fwrite(header, 1, 18, f);

	const uint8_t *src = data.ptr();
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			int i = y * w + x;
			uint8_t bgr[3] = {src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]};
			std::fwrite(bgr, 1, 3, f);
		}
	}

	std::fclose(f);
	return OK;
}

} // namespace godot
