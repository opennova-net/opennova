// A foliage definition's model asset as the foliage frame reads it: the LOD0
// geometry its slot expands and the slot's centre and bound radius. Retail
// builds both at the terrain load, once per definition, from the model's own
// vertex buffer [orig: Foliage_LoadDefAssets @ 0x601260]; the embedder loads
// the model and its :fd texture, the engine makes the slot. RE record:
// docs/foliage/foliage-re.md ("Normalization and eight-sample ground fit").
#pragma once

#include <runtime/renderer/foliage_frame.h>

#include <string>

namespace opennova::threedi {
struct Threedi3di3;
}

namespace opennova::assets {

// The slot's centre and bound radius over its vertices: the planar box of the
// vertices' x and z, seeded at +-1000 as the load seeds it, its centre (y 0)
// and the larger of its two half extents [orig: Foliage_LoadDefAssets
// @ 0x60134d..0x6013eb, the bound radius stored @ 0x6013eb; the 0.75 footprint
// beside it @ 0x6013ff is foliage::Runtime's]. A slot with no triangle, a
// non-finite vertex, or a zero radius stays invalid: the dispatcher disables it.
void foliage_slot_bounds(renderer::FoliageSlotGeometry &geometry);

// A foliage model's asset: its slot geometry, every LOD0 surface the model
// prepares (renderer::prepare_model_mesh, the ordinary render frame) in surface
// order, each surface's indices offset past the vertices before it, then the
// slot's bounds (foliage_slot_bounds), as retail expands the complete LOD0
// model; and the material its :fd texture bakes from, the first surface's
// [orig: Foliage_LoadDefAssets @ 0x601320 (the first primitive's material),
// its texture name read for the "%s:fd" load @ 0x60161b..0x601650]
// (-1 for a model with no surface).
struct FoliageModelAsset {
	renderer::FoliageSlotGeometry geometry;
	int fd_material_array_index = -1;
};
FoliageModelAsset foliage_model_asset(const threedi::Threedi3di3 &model);

// The key of the model a foliage definition's graphic names: the graphic's
// file name, lower-cased, without its extension ("Mveg6.3di" and "mveg6" name
// "mveg6"); the embedder opens "<key>.3di" through the case-blind VFS, as the
// load opens the definition's name [orig: Foliage_LoadDefAssets @ 0x6012cb,
// ThreediGp_LoadModel(definition name)]. Empty for an empty name.
std::string foliage_model_basename(const std::string &graphic);

} // namespace opennova::assets
