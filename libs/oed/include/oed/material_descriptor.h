#pragma once

#include "oed/types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oed {

enum class MaterialDescriptorBlend : uint8_t {
  Opaque,
  AlphaBlend,
  Additive,
  Multiplicative,
};

enum class MaterialDescriptorFamily : uint8_t {
  Unknown,
  FixedFunction,
  Phong,
  Flag,
  Dot3,
  Environment,
  Glass,
};

enum class MaterialDescriptorNormalSpace : uint8_t {
  None,
  Tangent,
  Object,
};

inline constexpr uint32_t MATERIAL_DESCRIPTOR_SKINNED = 0x00000001u;
inline constexpr uint32_t MATERIAL_DESCRIPTOR_SPECULAR = 0x00000002u;
inline constexpr uint32_t MATERIAL_DESCRIPTOR_ENVIRONMENT = 0x00000004u;
inline constexpr uint32_t MATERIAL_DESCRIPTOR_FLAG_ANIMATION = 0x00000008u;
inline constexpr uint32_t MATERIAL_DESCRIPTOR_UV_TRANSFORM = 0x00000010u;

struct MaterialDescriptorRecord {
  const char name[24];
  MaterialInfoTypeFlags shader_flags;
  MaterialDescriptorFamily family;
  MaterialDescriptorBlend blend;
  MaterialDescriptorNormalSpace normal_space;
  uint32_t descriptor_flags;
};

constexpr MaterialInfoTypeFlags material_info_flags(uint32_t flags) {
  return static_cast<MaterialInfoTypeFlags>(flags);
}

// Static replacement for OED's runtime material records:
// - FindMaterialIndexByName resolves shader tags by exact name.
// - RegisterFixedFunctionMaterials derives FF_* rows from _FFP.fx.
// - LoadShaderEffect fills VS/FFP rows from one .fx file per shader tag.
//
// This table is deliberately exact-tag only. Non-OED tags are classified as
// unknown by ONED instead of being guessed from string fragments.
inline constexpr MaterialDescriptorRecord kMaterialDescriptorTable[] = {
    {"FF_ST_OP", material_info_flags(MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_OP#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AB", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AB#UV", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AD", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AD#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_OP_LUM", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_OP_LUM#UV", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AB_LUM", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AB_LUM#UV", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AD_LUM", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AD_LUM#UV", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_OP", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_OP#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AB", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AB#UV", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AD", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AD#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_OP_LUM", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_OP_LUM#UV", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AB_LUM", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AB_LUM#UV", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AD_LUM", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AD_LUM#UV", material_info_flags(MATERIAL_FLAG_LUMINANCE | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_SPECIAL | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FFP_GLASS", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::Glass, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_DOT3DIFFOBJ", material_info_flags(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, 0},
    {"VS_PHONGO", material_info_flags(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_DOT3DIFF", material_info_flags(MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, 0},
    {"VS_DOT3DIFF#UV", material_info_flags(MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"VS_PHONGT", material_info_flags(MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_PHONGT#UV", material_info_flags(MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SPECULAR | MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"VS_DOT3DIFF2", material_info_flags(MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::Dot3, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, 0},
    {"VS_BMTXMIRRT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Environment, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_BUMPMIRRT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Environment, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_ENVPHONGT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SMOOTH | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Environment, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_ENVIRONMENT | MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_SKBASIC", material_info_flags(MATERIAL_FLAG_FILTER | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBASIC#UV", material_info_flags(MATERIAL_FLAG_FILTER | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UI_TOGGLE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"VS_SKGLASS", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_FILTER | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SPECIAL), MaterialDescriptorFamily::Glass, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_SKBUMPDIFFOBJ", material_info_flags(MATERIAL_FLAG_FILTER | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBUMPPHONGOBJ", material_info_flags(MATERIAL_FLAG_FILTER | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_SKBUMPDIFFOBJ2", material_info_flags(MATERIAL_FLAG_FILTER | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::Dot3, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBUMPDIFFT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_FILTER | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBUMPPHONGT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_FILTER | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_SKBUMPDIFFT2", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_FILTER | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::Dot3, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_FLAG", material_info_flags(MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Flag, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_FLAG_ANIMATION},
};

inline constexpr size_t kMaterialDescriptorTableCount =
    sizeof(kMaterialDescriptorTable) / sizeof(kMaterialDescriptorTable[0]);
static_assert(kMaterialDescriptorTableCount == kMaterialInfoTableCount,
              "Material descriptor table must mirror kMaterialInfoTable");

// ASCII case-insensitive full-string equality, mirroring the engine's registry
// match: HLSLEffect_FindByName @ 0x5ade70 resolves a .3di material's shader tag
// against the loaded effect names with stricmp, not a case-sensitive compare.
// Equality requires both strings to terminate together (a prefix never matches),
// exactly like stricmp. Shader tags are ASCII, so a byte-wise tolower suffices.
inline bool material_tag_iequals(std::string_view tag, const char* name) {
  std::size_t i = 0;
  for (; i < tag.size(); ++i) {
    const char raw = name[i];
    if (raw == '\0') {
      return false;  // name shorter than tag
    }
    char a = tag[i];
    char b = raw;
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
    if (a != b) {
      return false;
    }
  }
  return name[i] == '\0';  // both ended together
}

inline const MaterialDescriptorRecord* find_material_descriptor(std::string_view shader_tag) {
  for (size_t i = 0; i < kMaterialDescriptorTableCount; ++i) {
    if (material_tag_iequals(shader_tag, kMaterialDescriptorTable[i].name)) {
      return &kMaterialDescriptorTable[i];
    }
  }
  return nullptr;
}

}  // namespace oed
