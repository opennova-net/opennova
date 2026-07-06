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
// View-angle fade: color x |dot(eye, normal)|^2 — the tracer soft-edge
// [orig: vsTracer in Tracer.fx (localres.pff): lum = abs(dot(eye_vec,
// worldnormal)), Out.Diff = lum*lum, TSS MODULATE(Texture, Diffuse)].
inline constexpr uint32_t MATERIAL_DESCRIPTOR_VIEW_FADE = 0x00000020u;

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
//
// shader_flags carries the RUNTIME-derived capability words where the OED
// dump drifted (D-RMAT-4, docs/render/render-material-re.md): retail derives
// the word per effect by probing EVERY technique at .fx load [orig:
// HLSLEffect_LoadFromFile @ 0x5ae690], and five rows of ModSuperOed's
// authored gMaterialInfoTable disagree with that derivation over the shipped
// localres.pff set — FFP_GLASS (no VS => no TANGENT; its GLOW technique uses
// TexCubeRotSpecular => GLOW), VS_SKBUMPDIFFT / VS_SKBUMPPHONGT /
// VS_SKBUMPDIFFT2 (read In.Tangent, never ReflectColor => TANGENT, not
// GLASS), and VS_SKGLASS (untextured => no DIFFUSE). kMaterialInfoTable in
// oed/types.h stays the byte-faithful OED dump; the corrections live here,
// on the renderer-facing rows.
inline constexpr MaterialDescriptorRecord kMaterialDescriptorTable[] = {
    {"FF_ST_OP", material_info_flags(MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_OP#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AB", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AB#UV", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AD", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AD#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_OP_LUM", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_OP_LUM#UV", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AB_LUM", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AB_LUM#UV", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_ST_AD_LUM", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_ST_AD_LUM#UV", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_OP", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_OP#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AB", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AB#UV", material_info_flags(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AD", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AD#UV", material_info_flags(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_OP_LUM", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_OP_LUM#UV", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AB_LUM", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AB_LUM#UV", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::AlphaBlend, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"FF_MT_AD_LUM", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, 0},
    {"FF_MT_AD_LUM#UV", material_info_flags(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    // FFP_GLASS runtime word [orig: probe @ 0x5ae690 over Glass.fx]: no vertex
    // shader => no TANGENT; the GLOW technique samples TexCubeRotSpecular =>
    // GLOW (the Q3 bloom-copy set). OED dump said GLASS|TANGENT|BLENDING.
    {"FFP_GLASS", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_GLOW), MaterialDescriptorFamily::Glass, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_DOT3DIFFOBJ", material_info_flags(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, 0},
    {"VS_PHONGO", material_info_flags(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_DOT3DIFF", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, 0},
    {"VS_DOT3DIFF#UV", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"VS_PHONGT", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_PHONGT#UV", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SPECULAR | MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    {"VS_DOT3DIFF2", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::Dot3, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, 0},
    {"VS_BMTXMIRRT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Environment, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_BUMPMIRRT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Environment, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_ENVPHONGT", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Environment, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_ENVIRONMENT | MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_SKBASIC", material_info_flags(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBASIC#UV", material_info_flags(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_UV_TRANSFORM},
    // VS_SKGLASS runtime word [orig: probe @ 0x5ae690 over SkGlass.fx]: untextured
    // (Out.Diff = ReflectColor; no TexDiffuse1 reference) => no DIFFUSE.
    {"VS_SKGLASS", material_info_flags(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_BLENDING), MaterialDescriptorFamily::Glass, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_ENVIRONMENT},
    {"VS_SKBUMPDIFFOBJ", material_info_flags(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBUMPPHONGOBJ", material_info_flags(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_SKBUMPDIFFOBJ2", material_info_flags(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::Dot3, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Object, MATERIAL_DESCRIPTOR_SKINNED},
    // VS_SKBUMPDIFFT / VS_SKBUMPPHONGT / VS_SKBUMPDIFFT2 runtime words [orig:
    // probe @ 0x5ae690 over SkBDiffT/SkBPhongT/SkBDiffT2 + _vsSkDfT.fx]: the
    // tangent-space skinned VS reads In.Tangent (TANGENT) and nothing
    // references ReflectColor (no GLASS). OED dump had GLASS instead.
    {"VS_SKBUMPDIFFT", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_SKBUMPPHONGT", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Phong, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SKINNED | MATERIAL_DESCRIPTOR_SPECULAR},
    {"VS_SKBUMPDIFFT2", material_info_flags(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY), MaterialDescriptorFamily::Dot3, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::Tangent, MATERIAL_DESCRIPTOR_SKINNED},
    {"VS_FLAG", material_info_flags(MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::Flag, MaterialDescriptorBlend::Opaque, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_FLAG_ANIMATION},
    // Tracer.fx: unlit additive diffuse (RSAlphaMode(TRUE, ONE, ONE),
    // ZMODE_NOWRITE, no lighting includes) with the vsTracer view-angle fade
    // (VIEW_FADE: color x |dot(eye, normal)|^2 — not a displacement; the
    // "soft edge" is the squared facing falloff). D-RMAT-2 ported at REN-4.
    {"VS_TRACER", material_info_flags(MATERIAL_FLAG_DIFFUSE), MaterialDescriptorFamily::FixedFunction, MaterialDescriptorBlend::Additive, MaterialDescriptorNormalSpace::None, MATERIAL_DESCRIPTOR_VIEW_FADE},
};

inline constexpr size_t kMaterialDescriptorTableCount =
    sizeof(kMaterialDescriptorTable) / sizeof(kMaterialDescriptorTable[0]);
static_assert(kMaterialDescriptorTableCount == kMaterialInfoTableCount,
              "Material descriptor table must mirror kMaterialInfoTable");

// Tag resolution is case-INSENSITIVE like the runtime registry lookup
// [orig: HLSLEffect_FindByName @ 0x5ade70 — stricmp over the entry names].
// Still deliberately exact-tag (no substring guessing).
inline const MaterialDescriptorRecord* find_material_descriptor(std::string_view shader_tag) {
  auto ascii_lower = [](char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  };
  for (size_t i = 0; i < kMaterialDescriptorTableCount; ++i) {
    std::string_view name = kMaterialDescriptorTable[i].name;
    if (name.size() != shader_tag.size()) {
      continue;
    }
    bool equal = true;
    for (size_t j = 0; j < name.size(); ++j) {
      if (ascii_lower(name[j]) != ascii_lower(shader_tag[j])) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return &kMaterialDescriptorTable[i];
    }
  }
  return nullptr;
}

}  // namespace oed
