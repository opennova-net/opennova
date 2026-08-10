#pragma once

#include <string>

#include "ase/types.h"
#include "oed/types.h"

namespace oed {
namespace project { struct Project; }

struct ConvertOptions {
  float scale = 1.0f;
  bool verbose = false;
  const project::Project* project = nullptr;
  int lod_index = 0;
};

// Non-skinned conversion path mirroring ModSuperOed's ConvertToInternal.
// Outputs populate the provided material table and workspace (lod + buckets).
bool convert_to_internal(const ase::Document& doc,
                         MaterialTable& table,
                         LodBucketWorkspace& work,
                         const ConvertOptions& opts = ConvertOptions{},
                         std::string* err = nullptr);

// Populate part animation data in the workspace from a parsed 3dp project.
void populate_part_anim_from_project(const project::Project* project,
                                     LodBucketWorkspace& work,
                                     int lod_index = 0);

// Refresh only the material table from project material definitions.
void apply_material_table_from_project(const project::Project* project,
                                       MaterialTable& table);

// Copy the render_function field from project LOD N into the LOD header.
void apply_render_function_from_project(const project::Project* project,
                                        LodHeader& lod,
                                        int lod_index = 0);

// Apply project light data (colorgen, disable flags) from project LOD 0.
// LGHT is global in 3DI and sourced from LOD 0 in the original tool.
void apply_lights_from_project(const project::Project* project,
                               LodHeader& lod,
                               int lod_index = 0);

inline bool convert_to_internal(const ase::Document& doc,
                                InternalState& state,
                                const ConvertOptions& opts = ConvertOptions{},
                                std::string* err = nullptr) {
  return convert_to_internal(doc, state.material_table, state.workspace, opts, err);
}

}  // namespace oed
