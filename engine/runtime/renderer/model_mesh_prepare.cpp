#include "model_mesh_prepare.h"

#include <formats/threedi/threedi_strip_decode.h>

#include <algorithm>
#include <utility>

namespace opennova::renderer {
namespace {

using namespace threedi;

bool vertex_has_tangents(const ThreediVertex &v) {
    if ((v.flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0) return true;
    const float tangent_len = v.tangent[0] * v.tangent[0] +
            v.tangent[1] * v.tangent[1] + v.tangent[2] * v.tangent[2];
    const float bitangent_len = v.bitangent[0] * v.bitangent[0] +
            v.bitangent[1] * v.bitangent[1] + v.bitangent[2] * v.bitangent[2];
    return tangent_len > 0.000001f && bitangent_len > 0.000001f;
}

void append_vertex(PreparedMeshSurface &out, const ThreediVertex &v,
                   const ThreediTriangleStrip &strip, bool tangents) {
    const std::array<float, 3> normal{-v.normal[0], v.normal[1], v.normal[2]};
    out.vertices.push_back({-v.position[0], v.position[1], v.position[2]});
    out.normals.push_back(normal);
    out.uvs.push_back({v.uv0[0], v.uv0[1]});
    out.uvs2.push_back({v.uv1[0], v.uv1[1]});
    if (tangents) {
        const std::array<float, 3> tangent{-v.tangent[0], v.tangent[1], v.tangent[2]};
        const std::array<float, 3> bitangent{-v.bitangent[0], v.bitangent[1], v.bitangent[2]};
        const std::array<float, 3> cross{
            normal[1] * tangent[2] - normal[2] * tangent[1],
            normal[2] * tangent[0] - normal[0] * tangent[2],
            normal[0] * tangent[1] - normal[1] * tangent[0]};
        const float dot = cross[0] * bitangent[0] + cross[1] * bitangent[1] + cross[2] * bitangent[2];
        out.tangents.push_back({tangent[0], tangent[1], tangent[2], dot < 0.0f ? -1.0f : 1.0f});
    }
    // Retail's blend (threedi_skin_influences): the three stored weights as
    // they are and the fourth, 1 - (w0 + w1 + w2), on index byte 3, never
    // renormalized, each byte naming a part through the strip's bone table
    // (the palette entry it indexes). A byte past the table, a matrix the
    // palette does not hold in retail, rides bone 0.
    // [orig: ThreediGp_ConvertVerticesToGPUFormat @ 0x5B4C90 (the weights
    // copied verbatim @ 0x5B4E2C); D3DDevice_CreateVertexDeclarations
    // @ 0x5B0A00 (byte k is IndexArray[k]); CRenderBatchQueue_FlushBatches
    // @ 0x5DA170 (palette entry k is bone-table entry k); _BaseInc.fx
    // CalcSkinWorldPosAndNormal (NumBones 4: lastweight on IndexArray[3])]
    if (strip.bone_table_length > 0) {
        ThreediSkinInfluence influences[4];
        threedi_skin_influences(&v, strip.bone_table, strip.bone_table_length, influences);
        std::array<int32_t, 4> bones{};
        std::array<float, 4> weights{};
        for (size_t k = 0; k < bones.size(); ++k) {
            bones[k] = influences[k].part >= 0 ? influences[k].part : 0;
            weights[k] = influences[k].weight;
        }
        out.bones.push_back(bones);
        out.weights.push_back(weights);
        // SkinModelLightArray entry k is the light taken through the inverse
        // of palette entry k, filled in table order through one inverse
        // buffer that a singular matrix leaves as it was (D3DXMatrixInverse
        // writes nothing when the determinant is zero), so the entry keeps
        // the last inverse made. The lit effects read the entry of the
        // vertex's first index byte: a vertex whose first entry collapses is
        // lit through the nearest earlier entry that inverts, and before the
        // table's first entry the buffer holds stale stack.
        // [orig: CRenderBatchQueue_FlushBatches @ 0x5DA4F6..0x5DA5CE (the
        // directional fill, its inverse @ 0x5DA54B), @ 0x5DA950..0x5DA9A1 (the
        // point fill, its inverse @ 0x5DA967)]
        std::array<int32_t, 4> fallbacks{-1, -1, -1, -1};
        const int32_t first = v.bone_indices[0];
        if (first < strip.bone_table_length && first < 16) {
            for (int32_t n = 0; n < 4 && first - 1 - n >= 0; ++n)
                fallbacks[static_cast<size_t>(n)] = strip.bone_table[first - 1 - n];
        }
        out.light_fallback_bones.push_back(fallbacks);
    }
    out.indices.push_back(static_cast<int32_t>(out.vertices.size() - 1));
}

void finish_surface(PreparedMeshSurface &surface, MeshPreparationOptions options) {
    if (options.native_frame) {
        for (auto &v : surface.vertices) v[0] = -v[0];
        for (auto &n : surface.normals) n[0] = -n[0];
        for (auto &t : surface.tangents) {
            t[0] = -t[0];
            t[3] = -t[3];
        }
        for (size_t i = 0; i + 2 < surface.indices.size(); i += 3)
            std::swap(surface.indices[i + 1], surface.indices[i + 2]);
    }
    // [orig: rigid weapon parts ride a bone via fake skinning.]
    if (surface.bones.empty() && options.skeletal) {
        const int bone = options.bone_count > 0
                ? std::clamp(surface.part_index, 0, options.bone_count - 1)
                : std::max(surface.part_index, 0);
        surface.bones.assign(surface.vertices.size(), {bone, 0, 0, 0});
        surface.weights.assign(surface.vertices.size(), {1.0f, 0.0f, 0.0f, 0.0f});
        surface.light_fallback_bones.assign(surface.vertices.size(), {-1, -1, -1, -1});
    }
}

} // namespace

// Strips are sequential per render object: opaque first, then alpha.
// [orig: the RMDL/ROBJ walk every renderer pass performs; the STRP decode as
// the OED reader walks it, basic loop @0x474CAF / skinned @0x474B60
// (ModSuperOed.exe)]
std::vector<PreparedMeshSurface> prepare_model_mesh(
        const threedi::Threedi3di3 &model, int lod_index, MeshPreparationOptions options) {
    std::vector<PreparedMeshSurface> result;
    if (model.lods == nullptr || lod_index < 0 || static_cast<size_t>(lod_index) >= model.lod_count)
        return result;
    const auto &lod = model.lods[lod_index];
    if (lod.vertices.items == nullptr || lod.indices.indices == nullptr ||
            lod.strips == nullptr || lod.render_objects == nullptr) return result;

    size_t cursor = 0;
    for (size_t part_index = 0; part_index < lod.render_object_count; ++part_index) {
        const auto &part = lod.render_objects[part_index];
        const size_t count = static_cast<size_t>(part.num_strips + part.num_alpha_strips);
        for (size_t s = 0; s < count && cursor < lod.strip_count; ++s, ++cursor) {
            const auto &strip = lod.strips[cursor];
            std::vector<uint16_t> decoded;
            if (!threedi::threedi_decode_strip_indices(lod, strip, decoded)) continue;
            PreparedMeshSurface surface;
            surface.primitive_index = cursor;
            surface.material_index = strip.material_index;
            surface.material_array_index = threedi::threedi_material_array_index_for_id(model, strip.material_index);
            surface.part_index = static_cast<int>(part_index);
            surface.parent_index = part.parent_index;
            surface.abs = {-part.abs[0], part.abs[1], part.abs[2]};
            surface.is_alpha = s >= static_cast<size_t>(part.num_strips);
            surface.vertex_offset = static_cast<uint32_t>(strip.start_vertex);
            bool tangents = true;
            for (int i = 0; i < strip.num_vertices; ++i) {
                if (!vertex_has_tangents(lod.vertices.items[surface.vertex_offset + i])) {
                    tangents = false;
                    break;
                }
            }
            for (const auto index : decoded)
                append_vertex(surface, lod.vertices.items[surface.vertex_offset + index], strip, tangents);
            if (surface.vertices.empty()) continue;
            finish_surface(surface, options);
            result.push_back(std::move(surface));
        }
    }
    return result;
}

} // namespace opennova::renderer
