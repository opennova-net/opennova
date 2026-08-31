#include <runtime/particle/graphic_frames.h>

#include <cstdio>

#include <base/io/strutil.h>

namespace opennova::particle {

// [orig: CParticleDef_ReloadGraphicFrameTextures @ 0x5e4bb0]
std::string retail_particle_frame_name(std::string_view authored,
		int frame_count, int one_based_frame) {
	if (frame_count <= 1)
		return std::string(authored);
	std::string base = opennova::strutil::to_lower(authored);
	const std::size_t extension = base.find(".tga");
	if (extension != std::string::npos)
		base.resize(extension);
	char suffix[24]{};
	if (one_based_frame < 10)
		std::snprintf(suffix, sizeof(suffix), "_0%d.tga", one_based_frame);
	else
		std::snprintf(suffix, sizeof(suffix), "_%d.tga", one_based_frame);
	return base + suffix;
}

} // namespace opennova::particle
