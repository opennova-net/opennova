#pragma once

// The retail flipbook frame-name registrar. A multi-frame graphic layer names
// its frames as separate texture files derived from the authored name; a
// single-frame layer uses the authored name verbatim.

#include <string>
#include <string_view>

namespace opennova::particle {

// [orig: CParticleDef_ReloadGraphicFrameTextures @ 0x5e4bb0]. one_based_frame
// is in [1, frame_count] when frame_count is greater than one.
std::string retail_particle_frame_name(std::string_view authored,
		int frame_count, int one_based_frame);

} // namespace opennova::particle
