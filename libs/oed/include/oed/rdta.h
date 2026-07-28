#pragma once

#include "oed/types.h"
#include "threedi_model.h"
#include <vector>
#include <string>

namespace oed {

void normalize_vec3(float (&v)[3]);

// StripifyResult defines the output of the stripifier.

struct RenderVertex;
struct SmoothedVertexVectors;
struct SmoothedFace;
struct StripBucket;
struct StripBuild;
struct StripifyContext;
struct TraceResult;
struct StripifyResult;
struct StripBuildIteration;
struct SkinnedGroup;

void sort_bone_weights(RenderVertex &rv);
StripifyResult stripify_triangles(const std::vector<uint16_t> &tris,
                                  int subobject_index,
                                  int material_index);
std::vector<SmoothedFace> compute_smoothed_vectors(const SubObject &subobj);
std::vector<SmoothedFace> compute_smoothed_vectors_float(
    const SubObject &subobj);
bool build_render_geometry_skinned(const LodHeader &lod,
                                   const MaterialTable *materials,
                                   ThreediLod &out_lod,
                                   std::string &error);
bool build_render_geometry(const LodHeader &lod,
                           const MaterialTable *materials,
                           bool force_skinned_path,
                           ThreediLod &out_lod, std::string &error);

void normalize_vec3(float (&v)[3]);
void normalize_vec3(Vec3 &v);
void cross3(const float (&a)[3], const float (&b)[3], float (&out)[3]);

MaterialInfoTypeFlags lookup_material_info_flags(const char *shader);

} // namespace oed
