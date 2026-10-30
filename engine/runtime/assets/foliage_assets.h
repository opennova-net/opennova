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
// model; and the texture its :fd texture bakes from: the first texture row of
// the first surface's material, whatever that row's slot. The load keeps the
// first primitive's material [orig: Foliage_LoadDefAssets @ 0x601320] and reads
// the name at its +0x20 for the "%s:fd" load and the bake's source image
// [@ 0x60161b..0x601664]; the runtime material holds its texture rows from
// +0x20 at 20 bytes each, so +0x20 is row 0's name [orig:
// Material_ConvertDefinition @ 0x5b03f2..0x5b041a]. There is no fallback to
// another row: a material with no row names nothing and the slot bakes no :fd
// texture. `fd_material_array_index` is the MTRL array index (-1 for a model
// with no surface), `fd_texture_row` the row (-1 for none).
struct FoliageModelAsset {
	renderer::FoliageSlotGeometry geometry;
	int fd_material_array_index = -1;
	int fd_texture_row = -1;
};
FoliageModelAsset foliage_model_asset(const threedi::Threedi3di3 &model);

// Whether a foliage definition's slot draws. Retail loads every definition's
// model and draws the slots whose model loaded: a failed load zeroes the slot's
// model [orig: Foliage_LoadDefAssets @ 0x6012cb..0x6012d0, @ 0x601a0a] and only
// a slot with a model gets its tile buffers [@ 0x601a61..0x601a71]; an unused
// definition row names no model, so it loads none. A model whose LOD0 makes no
// valid slot (foliage_slot_bounds) draws nothing either, and its slot stays off.
enum class FoliageSlotStatus {
	MissingDefinition,  // no definition in the slot
	MissingModel,       // the definition's model did not load
	InvalidGeometry,    // the model loaded with no valid slot geometry
	Enabled,
};
FoliageSlotStatus foliage_slot_status(bool has_definition, bool model_loaded,
		const renderer::FoliageSlotGeometry &geometry);

// The key of the model a foliage definition's graphic names: the graphic's
// file name, lower-cased, without its extension ("Mveg6.3di" and "mveg6" name
// "mveg6"); the embedder opens "<key>.3di" through the case-blind VFS, as the
// load opens the definition's name [orig: Foliage_LoadDefAssets @ 0x6012cb,
// ThreediGp_LoadModel(definition name)]. Empty for an empty name.
std::string foliage_model_basename(const std::string &graphic);

} // namespace opennova::assets
