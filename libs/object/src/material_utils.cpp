#include "object/rdta.h" // For the declaration of lookup_material_info_flags
#include "object/types.h" // For MaterialInfoTypeFlags and kMaterialInfoTable

#include <cstring> // For std::strcmp

namespace opennova::object {

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

} // namespace opennova::object
