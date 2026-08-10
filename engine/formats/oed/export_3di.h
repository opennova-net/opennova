#pragma once

#include <string>
#include <vector>

#include "oed/types.h"

#include "threedi_model.h"

namespace oed {

struct LodWorkSlot;  // from apps/oed/include/oed/types.h
struct LodBucketWorkspace;  // from apps/oed/include/oed/types.h
struct InternalState;  // from apps/oed/include/oed/types.h

struct Export3diOptions {
  std::string model_name;
};

// Build a ThreediModel in memory without writing it to disk. The caller owns
// the allocations in out_model and must free them with threedi_model_free().
bool build_3di_model(const LodBucketWorkspace &workspace,
                     const MaterialTable *materials,
                     const Export3diOptions &options,
                     ThreediModel &out_model,
                     std::string &error);
bool build_3di_model(const InternalState &state,
                     const Export3diOptions &options,
                     ThreediModel &out_model,
                     std::string &error);

// Convert a populated workspace (LodWorkSlot) into a 3DI model and write it to disk.
// Returns true on success, false and sets error on failure.
bool export_3di(const LodWorkSlot &workspace,
                const std::string &output_path,
                std::string &error);
bool export_3di(const LodBucketWorkspace &workspace,
                const std::string &output_path,
                std::string &error);
bool export_3di(const InternalState &state,
                const std::string &output_path,
                std::string &error);

// Multi-LOD export: each workspace provides geometry for one LOD.
// Materials come from the shared material table.  Thresholds are per-LOD.
// poly_collision_lod selects which LOD supplies collision vertices.
bool export_3di(const std::vector<const LodBucketWorkspace *> &workspaces,
                const MaterialTable *materials,
                const std::vector<float> &thresholds,
                int poly_collision_lod,
                const std::string &output_path,
                const Export3diOptions &options,
                std::string &error);
bool build_3di_model(const std::vector<const LodBucketWorkspace *> &workspaces,
                     const MaterialTable *materials,
                     const std::vector<float> &thresholds,
                     int poly_collision_lod,
                     const Export3diOptions &options,
                     ThreediModel &out_model,
                     std::string &error);
bool export_3di(const std::vector<const LodBucketWorkspace *> &workspaces,
                const MaterialTable *materials,
                const std::vector<float> &thresholds,
                const std::string &output_path,
                std::string &error);

namespace detail {
// Packs Light -> ThreediLight.flags from disable/type bits (0-3).
uint8_t compute_light_flags(const Light &src);

struct OcclusionBuffers {
  std::vector<ThreediOcclusionVertex> vertices;
  std::vector<ThreediOcclusionPlane> planes;
  std::vector<ThreediOcclusionFace> faces;
  std::vector<ThreediOcclusionObject> objects;
};

// Collect occlusion vertices/planes/faces/objects from collisions that match
// the occlusion collidable types (NodeType_OB through NodeType_CC), mirroring
// WriteOCCL behavior. Existing contents of out are cleared.
void build_occlusion_buffers(const LodHeader &lod, OcclusionBuffers &out);
}  // namespace detail

}  // namespace oed
