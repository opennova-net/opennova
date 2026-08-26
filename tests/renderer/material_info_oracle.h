// tests/renderer/material_info_oracle.h — TEST ORACLE, not runtime code.
//
// Byte-faithful relocation of the OED material-info dump (ModSuperOed's
// gMaterialInfoTable plus VS_TRACER) that lived in engine/formats/oed/types.h
// until the asset-pipeline cut (ADR 0038). With the OED library and the
// third_party/modsuperoed submodule gone, this is the only in-repo copy of
// that dump; renderer_material_classify_test pins
// renderer::kMaterialDescriptorTable (engine/runtime/renderer/
// material_descriptor.h) against it row by row, with the five D-RMAT-4
// runtime corrections enumerated in the test. It deliberately includes
// nothing from engine/runtime/renderer: an oracle that moved with the code
// under test would pin nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace material_oracle {

//
// Bit meanings are witnessed against retail Jointops.exe, where the runtime
// builds the same word per effect at .fx load: the FF_* rows are AUTHORED
// ([orig: HLSLEffect_InitFixedFunctionShaders @ 0x5af790 — _ST 0x4 / _MT 0xC,
// _AB 0x1002 / _AD 0x1000, _LUM 0x10000001, #UV |0x10000]) and the file-effect
// rows are PROBED per technique ([orig: HLSLEffect_LoadFromFile @ 0x5ae690 —
// TexDiffuse1 0x4, TexDiffuse2 0x8, TexNormal1 0x10, TexNormal2 0x20,
// TexHorizon 0x40, TexOcclusion 0x80, TexSpecularCtrl 0x100, DisplaceAmount
// 0x200, "blending" annotation 0x1000, ReflectColor 0x2000,
// SkinWorldMatrixArray 0x4000, TANGENT input semantic 0x8000, EffectAlt_UV
// variant 0x10000]). The 0x10000000 dialects RESOLVED at REN-4: the file-effect
// probe sets it for "uses TexCubeRotSpecular" [orig: @ 0x5af04b] and the FF
// path authors it on the _LUM rows [orig: selflum table @ 0x5afa36] — both mean
// the same runtime capability, "renders a glow/bloom copy": the batch queue's
// Q3 duplicate is gated on this bit [orig: collect_render_objects_for_batch
// @ 0x5d93b5], and the probe booleans are UNIONS over ALL techniques [orig:
// HLSLEffect_LoadFromFile @ 0x5ae690 technique loop], so at runtime FFP_GLASS
// (whose GLOW technique samples TexCubeRotSpecular — Glass.fx) carries it even
// though this OED-dump table does not. The self-lum LOOK is the EMISSIVE bit
// (0x1); GLOW is the bloom-copy capability. See
// docs/render/render-material-re.md D-RMAT-4.
enum MaterialInfoTypeFlags : uint32_t {
  MATERIAL_FLAG_EMISSIVE  = 0x0001,   // self-lum look (FF _LUM rows author 0x10000001)
  MATERIAL_FLAG_ALPHA     = 0x0002,   // alpha-blend (AB) variant (FF _AB rows author 0x1002)
  MATERIAL_FLAG_DIFFUSE   = 0x0004,   // uses TexDiffuse1
  MATERIAL_FLAG_SECONDARY = 0x0008,   // uses TexDiffuse2 (detail/multi-texture)
  MATERIAL_FLAG_NORMAL_A  = 0x0010,   // uses TexNormal1
  MATERIAL_FLAG_NORMAL_B  = 0x0020,   // uses TexNormal2
  MATERIAL_FLAG_BLENDING  = 0x1000,   // technique declares a "blending" annotation
  MATERIAL_FLAG_GLASS     = 0x2000,   // uses ReflectColor (glass/reflective)
  MATERIAL_FLAG_SKINNED   = 0x4000,   // uses SkinWorldMatrixArray
  MATERIAL_FLAG_TANGENT   = 0x8000,   // vertex shader consumes the TANGENT semantic
  MATERIAL_FLAG_UVGEN     = 0x10000,  // the TEX_UVXFORM (#UV / ", UVGen") variant
  MATERIAL_FLAG_GLOW      = 0x10000000,  // glow/bloom-copy capable (Q3 duplicate; dialect note above)
};

struct MaterialInfoRecord {
  const char name[24];
  MaterialInfoTypeFlags flags;
};

// The runtime shader-tag registry, as retail Jointops.exe builds it at boot:
// 24 FF_* built-ins compiled from _FFP.fx [orig: HLSLEffect_InitFixedFunctionShaders
// @ 0x5af790], the shipped localres.pff .fx effect tags (underscore-prefixed
// includes are skipped) [orig: HLSLEffect_LoadAllFromPFFArchive @ 0x5afed0 /
// HLSLEffect_LoadFromFile @ 0x5ae690], and a "#UV" twin for every effect that
// declares EffectAlt_UV [orig: @ 0x5aea03]. Identical to ModSuperOed's dumped
// gMaterialInfoTable plus VS_TRACER (present in the runtime registry; absent
// from OED's authorable table).
inline constexpr MaterialInfoRecord kMaterialInfoTable[] = {
    {"FF_ST_OP", MATERIAL_FLAG_DIFFUSE},
    {"FF_ST_OP#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AB", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AB#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AD", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AD#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_OP_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE)},
    {"FF_ST_OP_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AB_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AB_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AD_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AD_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_OP", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"FF_MT_OP#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AB", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AB#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AD", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AD#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_OP_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"FF_MT_OP_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AB_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AB_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AD_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AD_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FFP_GLASS", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_BLENDING)},
    {"VS_DOT3DIFFOBJ", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_PHONGO", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_DOT3DIFF", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_DOT3DIFF#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"VS_PHONGT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_PHONGT#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"VS_DOT3DIFF2", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"VS_BMTXMIRRT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_BUMPMIRRT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_ENVPHONGT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBASIC", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBASIC#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"VS_SKGLASS", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"VS_SKBUMPDIFFOBJ", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPPHONGOBJ", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPDIFFOBJ2", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"VS_SKBUMPDIFFT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPPHONGT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPDIFFT2", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"VS_FLAG", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE)},
    // Runtime-registry row absent from OED's table: Tracer.fx registers
    // VS_TRACER (EffectSpecial=true, TECHNIQUE_NORMAL with usevs/ZMODE_NOWRITE,
    // TexDiffuse1 only, RSAlphaMode(TRUE, ONE, ONE)). The probed flag word is
    // DIFFUSE only — Tracer.fx declares no "blending" annotation (it sorts via
    // EffectSpecial instead). docs/render/render-material-re.md D-RMAT-2.
    {"VS_TRACER", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE)},
};
inline constexpr size_t kMaterialInfoTableCount = std::size(kMaterialInfoTable);

} // namespace material_oracle
