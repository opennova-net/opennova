// A heightmap image made into the depth the TrnGen bake takes (heightmap_depth.h). Tooling over the bake's own
// units (terrain_bake.h): TrnGen.exe read its depth map as a .raw alone.
#include "heightmap_depth.h"

#include <algorithm>
#include <cmath>

#include <base/io/strutil.h>
#include <formats/png/png_decode.h>

#include "terrain_bake.h"

namespace opennova::trngen {

bool decode_heightmap_depth(const std::string &name, const std::vector<uint8_t> &bytes, double top,
                            HeightmapDepth &out, std::string &why) {
	out = HeightmapDepth();
	const size_t texels = size_t(kDepthSide) * kDepthSide;
	const auto scaled16 = [&](const std::vector<uint16_t> &raw16) {
		// A raw16 height scaled by top / 127.5, rounded, kept within the format's 16 bits.
		const double scale = top / kDepth8Top;
		out.depth16.resize(raw16.size());
		for (size_t i = 0; i < raw16.size(); ++i)
			out.depth16[i] = static_cast<uint16_t>(std::min(65535.0, std::floor(raw16[i] * scale + 0.5)));
	};
	const auto from8 = [&](std::vector<uint8_t> depth8) {
		// TrnGen's input as it is at its own scale; else smoothed as TrnGen smooths it [orig: TrnGen.exe
		// build_terrain_thread @ 0x4013A0], then scaled.
		if (top == kDepth8Top) out.depth8 = std::move(depth8);
		else scaled16(smooth_depthmap(depth8));
	};
	if (strutil::ends_with_icase(name, ".raw")) {
		if (bytes.size() == texels) {
			from8(bytes);
			return true;
		}
		if (bytes.size() == texels * 2) {
			out.depth16.resize(texels);
			for (size_t i = 0; i < texels; ++i) out.depth16[i] = static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
			return true;
		}
		why = name + " is " + std::to_string(bytes.size()) + " bytes: a .raw heightmap is 1024 x 1024 texels, 1 MiB of 8-bit "
		      "heights or 2 MiB of 16-bit";
		return false;
	}
	png::GrayImage grey;
	if (!png::decode_png_gray(bytes, grey, why)) {
		why = name + ": " + why + " (a heightmap is a PNG or a .raw)";
		return false;
	}
	if (grey.width != kDepthSide || grey.height != kDepthSide) {
		why = name + " is " + std::to_string(grey.width) + " x " + std::to_string(grey.height) +
		      ": a heightmap is 1024 x 1024 texels, one a world unit";
		return false;
	}
	if (grey.max_value == 255) {
		std::vector<uint8_t> depth8(grey.samples.size());
		for (size_t i = 0; i < depth8.size(); ++i) depth8[i] = static_cast<uint8_t>(grey.samples[i]);
		from8(std::move(depth8));
		return true;
	}
	// 16 bits: 0 to 65535 over 0 to top world units, 256 raw a unit.
	out.depth16.resize(texels);
	const double scale = top * 256.0 / 65535.0;
	for (size_t i = 0; i < texels; ++i)
		out.depth16[i] = static_cast<uint16_t>(std::min(65535.0, std::floor(grey.samples[i] * scale + 0.5)));
	return true;
}

} // namespace opennova::trngen
