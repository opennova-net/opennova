// The target a model is built for (formats/threedi/threedi_o3d_lower.h): the
// retail limits with the renderer's shader table (which shaders read the
// TANGENT semantic) and its texture loader rule (which rows load only as a
// .dds), the tables a format library cannot include. opennova-3di `build` and
// the editor's `.o3d` import build through it, so both hold a model to the
// same game.
#pragma once

#include <formats/threedi/threedi_o3d_lower.h>
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::renderer {

inline threedi::ThreediTarget retail_model_target() {
	threedi::ThreediTarget target;
	target.limits = threedi::threedi_retail_limits();
	target.shaders = material_descriptor_tangent_lookup;
	target.textures = material_texture_dds_only;
	return target;
}

} // namespace opennova::renderer
