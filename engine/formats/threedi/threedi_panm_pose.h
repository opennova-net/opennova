// Model-level PANM pose evaluation over a parsed Threedi3di3 — the layer the
// retail model loader/renderer runs above the per-node matrix builder:
// LOD-effective node selection, per-track file-local -> global CTRL register
// resolution, liveness classification, and the part-indexed pose array with
// its base-transform fallback. Ported from the shell binding's
// ObjectData PANM evaluation so the simulation can pose collision
// sections without the render binding (ADR 0028).
// [orig: model CTRL loader @ 0x5B4640; CtrlName_ToOrdinal @ 0x57B290;
//  PANM_SampleTrack @ 0x5B2270; the node-matrix builder is
//  threedi_panm_runtime.h's threedi_panm_build_node_matrices]
#pragma once

#include <formats/threedi/threedi_3di3.h>

#include <cstdint>
#include <vector>

namespace opennova::threedi {

// A node is "live" when any of its enabled transform families samples a track
// with a non-zero control high nibble — plus the three rotation modes that
// evaluate without sampling a conventional track (spinner reinterprets raw
// PANM bytes as floats and may carry a zero high nibble; the two view-derived
// modes never sample).
bool threedi_panm_animation_is_live(const ThreediPartAnimation &anim);
// Noise-style tracks (control low nibble 6) re-randomize per evaluation, so a
// caching consumer must re-evaluate every frame while one is present.
bool threedi_panm_animation_uses_noise(const ThreediPartAnimation &anim);

// The effective PANM node set for a LOD: the LOD-local block wins outright;
// a LOD with none inherits the model-level block. False when the model/LOD is
// invalid or neither block has nodes.
bool threedi_panm_effective_for_lod(const Threedi3di3 &model, int lod_index,
                                    std::vector<ThreediPartAnimation> &r_nodes);

// True when the LOD has render objects and any effective node is live.
bool threedi_panm_lod_has_live(const Threedi3di3 &model, int lod_index);

// Rewrite each track's authored file-local CTRL index into the global
// catalog ordinal, exactly like the retail loader does before any track is
// sampled. Only style 113 later reads the register bus; styles 114..117 keep
// the resolved ordinal as their waveform phase. An absent or unknown name
// inherits CtrlName_ToOrdinal's zero result and therefore aliases LOD_FRAC.
// [orig: model CTRL loader @ 0x5B4640; CtrlName_ToOrdinal @ 0x57B290]
void threedi_panm_resolve_registers(const Threedi3di3 &model,
                                    std::vector<ThreediPartAnimation> &nodes);

// Retail's runtime clock is a GetTickCount DWORD; a negative embedder time
// clamps to zero rather than wrapping.
uint32_t threedi_panm_runtime_time_ms(int64_t time_ms);

// Evaluate the whole per-part pose for one LOD: identity base transforms
// carrying each render object's absolute pivot, register resolution, the
// node-matrix build, and the part->node mapping (last node targeting a part
// wins; unanimated parts fall back to their base transform). r_matrices is
// sized to the LOD's render-object count; r_animated (optional) marks parts
// driven by a PANM node. Returns false when the model/LOD has no render
// objects. ctrl_values is the 96-slot global register bus (may be null for
// all-zero).
bool threedi_panm_pose_parts(const Threedi3di3 &model, int lod_index,
                             uint32_t time_ms, const int32_t *ctrl_values,
                             std::vector<ThreediMatrix4x4> &r_matrices,
                             std::vector<uint8_t> *r_animated);

// Layer one LOD's PANM over POSED per-part frames: the clip-driven submit.
// Retail hands every submitted model's bone matrices to the one builder that
// poses a static model, on the rigid and the per-vertex skinned paths alike;
// the first-person gun goes in with the matrices its clip built, and its arms
// with the gun's same array, each composed through its own PANM
// [orig: Player_RenderFirstPersonViewModel @ 0x4DED60 (the gun's submit
//  @ 0x4DF043 / @ 0x4DEFCF, the arms' @ 0x4DF088); Render_SubmitEntity
//  @ 0x5DAD80; Render_CollectRenderObjectsForBatch @ 0x5D8F3B;
//  Render_CollectRenderBatchesForEntity @ 0x5D94F5;
//  Model_TransformBoneMatrices @ 0x58E390].
// inputs: one model-space frame per render object, the part's skinning matrix
// (its posed bone times its rest inverse), native row-vector frame; a part
// past the array is identity. r_matrices: one per render object, a part no
// node drives keeping its input. r_animated (optional) marks the node-driven
// parts. Returns false, r_matrices then equal to the inputs, when the LOD has
// no render objects or no effective node.
bool threedi_panm_pose_parts_over(const Threedi3di3 &model, int lod_index,
                                  uint32_t time_ms, const int32_t *ctrl_values,
                                  const std::vector<ThreediMatrix4x4> &inputs,
                                  std::vector<ThreediMatrix4x4> &r_matrices,
                                  std::vector<uint8_t> *r_animated);


} // namespace opennova::threedi
