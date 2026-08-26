#include "oed/rdta.h" // For the declaration of lookup_material_info_flags
#include "oed/types.h" // For MaterialInfoTypeFlags and kMaterialInfoTable

#include <cstring> // For std::strcmp

// [orig: HLSLEffect_FindByName @0x5ade70 — the runtime name lookup over the effect registry]
// ModSuperOed's FindMaterialIndexByName is the tool-side twin (exact strcmp over
// gMaterialInfoTable, unknown -> row 0); its address is not pinned in the tracked records.

namespace oed {

MaterialInfoTypeFlags lookup_material_info_flags(const char *shader) {
  if (!shader) return kMaterialInfoTable[0].flags;
  for (size_t i = 0; i < kMaterialInfoTableCount; ++i) {
    if (std::strcmp(shader, kMaterialInfoTable[i].name) == 0) {
      return kMaterialInfoTable[i].flags;
    }
  }
  // Mirror FindMaterialIndexByName fallback: unknown shaders default to the
  // first table entry (FF_ST_OP).
  return kMaterialInfoTable[0].flags;
}

} // namespace oed
