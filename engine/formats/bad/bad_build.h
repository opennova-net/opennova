// BAD clip construction seam: the one code path that builds a `.bad` clip and
// the `.adm` table naming it (ADR 0047). `bad_write.h` serializes what this
// assembles; `bad.h` reads the result back. The shape mirrors
// `formats/threedi/threedi_build.h`: plain doubles, no pointers, and every
// derivation and frame conversion owned here so a front end (the `.o3a` scene
// text, the Blender add-on through it) carries only what an author chose.
//
// This is a construction seam, not an intermediate representation: nothing at
// runtime walks a BadBuildClip; `BadFile` stays the one clip every consumer
// reads.
//
// What the builder derives, never the author (witnessed over the 477 retail
// `.bad` clips under OPENNOVA_JO_ASSETS, 2026-09-24):
//   * `BadBone.rotation[9]` is the transpose of the bone's first key as a
//     matrix, in 13517 of 13517 bones (worst deviation 5.0e-7). The runtime
//     reads it as the bind of the rig's RESET clip, which every clip of the rig
//     composes against, and of a clip that plays with no reset pinned
//     [orig: AnimChannel_ComputeBoneMatrices @0x410da0, the bind from
//     channel+44 @0x410dd8 else the playing clip @0x410de3].
//   * `BadBone.position[3]` follows the paired model's pivots through the
//     parent's bind in the SET's reset clip, the bind the runtime composes the
//     clip against: `position[i] = bind[parent(i)] . clip(pivot[i] -
//     pivot[parent(i)])`, in 30358 of the 32011 non-junk bones of the retail
//     tables' other clips (365 fit the clip's own rotation instead). The
//     runtime never reads it (6720 of 13517 retail bones triplicate X, 477 are
//     zero) [orig: BoneAnim_BuildWorldMatrices @0x40c400].
//   * `num_children`, `child_offset` and `parent_offset` from `parent`, and the
//     translation block's pad row past row frame_count (bad_write.cpp).
//   * A capsule `bottom`/`top` pair per event when the author gives neither an
//     explicit pair nor a constant one, by OUR rule (bad_clip_extents): what
//     retail measured them with is unwitnessed.
// What the author supplies and the seam only counts: frame_count + 1 keys per
// bone (or a duration table), events and translation rows, the fence-post
// count every retail clip carries (`event_count == frame_count + 1`).
// What the builder keeps verbatim: the key quaternions. Retail stores
// opposite-hemisphere neighbours (4573 of 657788 key pairs, 218 of 477 files)
// and the slerp short-arcs anyway [orig: Math_QuaternionSlerp @0x615e20], so
// nothing re-signs or renormalizes them.
#pragma once

#include <formats/bad/bad.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::bad {

// The header's flag word. Bit 3 rides 73 of the 477 retail clips (viewmodel
// draw clips, CycDr*, avenger_025) and is unwitnessed: it is carried, not read.
inline constexpr uint32_t BAD_FLAG_LOOP = 0x1u;
inline constexpr uint32_t BAD_FLAG_TRANSLATION = 0x2u;
inline constexpr uint32_t BAD_FLAG_BIT3 = 0x8u;

struct BadBuildVec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

// Authored as (x, y, z, w), the order the file stores.
struct BadBuildQuat {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

// Mission axes (x forward, y left, z up) <-> the clip frame (x side, y up,
// z forward). This is threedi_build's mission -> presentation permutation: a
// clip's rotations apply to the skeleton the model's pivots build, and that
// skeleton is the model frame mirrored on x (threedi_model_to_presentation).
BadBuildVec3 bad_clip_from_mission(const BadBuildVec3 &m);
BadBuildVec3 bad_mission_from_clip(const BadBuildVec3 &c);
BadBuildQuat bad_clip_from_mission(const BadBuildQuat &m);
BadBuildQuat bad_mission_from_clip(const BadBuildQuat &c);

// Row-major 3x3 <-> quaternion, the layouts BadBone.rotation and a channel key
// carry.
void bad_quat_to_rows(const BadBuildQuat &q, float rows[9]);
BadBuildQuat bad_rows_to_quat(const float rows[9]);
void bad_rows_transpose(const float in[9], float out[9]);

// The bone-position relation and its inverse (`anim scene` recovers the pivot
// a clip was exported from). `rel` is parent-relative, mission axes.
BadBuildVec3 bad_bone_position(const float parent_bind_rows[9], const BadBuildVec3 &rel_mission);
BadBuildVec3 bad_bone_rel(const float parent_bind_rows[9], const float position[3]);

struct BadBuildBone {
    std::string name;   // at most 31 characters
    int parent = -1;    // -1 for the root; always a lower index
    BadBuildVec3 pivot; // absolute, mission axes: the paired model part's pivot
    double length = 0.0;
    // frame_count + 1 keys, mission axes, model space (a channel row is a
    // world rotation, not a parent-relative one). A bone that carries its own
    // duration table may key sparsely instead: the channel walks that table and
    // the header's frame count alone sets the clip's length.
    std::vector<BadBuildQuat> keys;
    // Empty: every key lasts one frame, as 476 of 477 retail clips do; else one
    // duration per key.
    std::vector<uint16_t> durations;
    // frame_count + 1 entries under BAD_FLAG_TRANSLATION (rows 0..frame_count),
    // mission axes, as the keys and events carry: the runtime reads row
    // trunc(frame_count * t) and lerps it with the next one, so the last
    // interval of every cycle reaches row frame_count [orig: sub_4102D0
    // @0x4102d0 via BoneAnim_TransformBones @0x410360]. The writer appends the
    // pad row retail's clips carry past it (bad_write.cpp); no author gives it.
    std::vector<BadBuildVec3> translations;
    // The stored `position[3]`, as given, for a bone whose pivot cannot
    // re-derive it: retail's own exporter left junk in this dead field (6720 of
    // 13517 bones triplicate X), and the pivot relation is not exactly
    // invertible in float. `anim scene` writes it only when the derivation
    // misses, the way the `.o3d` scene writes a collision normal it cannot
    // rebuild; an authoring front end never does.
    bool position_given = false;
    BadBuildVec3 position_stored; // clip frame, as the file holds it
};

struct BadBuildEvent {
    BadBuildVec3 velocity; // mission axes, metres per frame
    int32_t trigger = 0;   // the event bit word (footsteps, fire, foley)
    bool extents_given = false;
    double bottom = 0.0, top = 0.0;
};

struct BadBuildClip {
    std::string name; // the `.bad` file stem
    uint32_t version = 1;
    uint32_t fps = 30; // 30 in every retail clip
    uint32_t flags = 0;
    uint32_t frame_count = 0; // intervals: every key list holds one more
    // A constant capsule pair for every event, the shape retail's viewmodel
    // clips carry (0.0/0.6 or 1.07/1.07 across the JO `_1st` sets).
    bool capsule_given = false;
    double capsule_bottom = 0.0, capsule_top = 0.0;
    std::vector<BadBuildBone> bones;
    std::vector<BadBuildEvent> events; // empty, or frame_count + 1
};

// One `.adm` row: the anim slot and its clip ring. The engine serves a row from
// its LAST variant back [orig: AnimMap_RegisterBoneNode @0x40C2D0]; the order
// here is the order the file stores.
struct BadBuildRow {
    std::string key;
    std::vector<std::string> variants;
};

// A clip set: one rig's table and every clip it names.
struct BadBuildSet {
    std::string adm_name;
    std::vector<BadBuildRow> rows;
    std::vector<BadBuildClip> clips;
};

// The contiguous BadFile the writer serializes; it owns every array `file`
// points at, so it must outlive the write.
struct BadAssembled {
    BadFile file{};
    std::vector<BadBone> bones;
    std::vector<BadChannel> channels;
    std::vector<std::vector<uint16_t>> durations;
    std::vector<std::vector<BadQuaternion>> rotations;
    std::vector<BadEvent> events;
    std::vector<std::array<float, 3>> translations;
};

// A row variant as a clip stem: the trailing `.bad` a table may or may not
// carry (4706 of 5146 retail variants do) is dropped.
std::string bad_build_clip_stem(const std::string &variant);

// The clip every clip of a table composes against: the reset row's (a key
// naming slot 0, `reset`, past its first five characters; the last such row)
// LAST variant, because each reset variant replaces the slot's head instead of
// joining a ring [orig: AnimMap_FindSlotByName @0x40cfa0, stricmp on the key
// + 5; AnimMap_RegisterBoneNode @0x40C2D0, slot 0 self-rings @0x40c38b;
// AnimMap_RegisterEntity @0x40bb60 pins its clip @0x40bbe3]. The stem is empty
// for a table with no reset row; the clip is null when the set lacks it too.
std::string bad_build_reset_stem(const std::vector<BadBuildRow> &rows);
const BadBuildClip *bad_build_reset_clip(const BadBuildSet &set);

// The rotation rows a bone's children turn through: the transpose of the
// bone's stored first key in `reset`, or in `clip` for a null `reset` or a
// bone past its bones (the runtime's own fallback when no reset is pinned).
void bad_derive_bind_rows(const BadBuildClip &clip, const BadBuildClip *reset, size_t bone,
                          float rows[9]);

// The bone table the seam derives for `clip`, exactly as bad_build_assemble
// writes it: name, parent and length; `rotation[9]`, the transpose of the
// bone's own stored first key; `position[3]` through the parent's bind rows
// (bad_derive_bind_rows), or `position_stored` where the bone gives it. A
// parent that is not a lower index derives as a root. `anim scene` calls it to
// find the bones whose stored position the pivots cannot re-derive.
void bad_derive_bone_table(const BadBuildClip &clip, const BadBuildClip *reset,
                           std::vector<BadBone> &rows);

// The per-frame capsule extents an event carries when the author gives none:
// the drop below bone 0 and the total height of the bone origins, over the
// pose the runtime draws, each bone's key composed against `reset`'s bind as
// `key * bind^-1` (the clip's own first key for a null `reset` or a bone past
// its bones). The rule is OURS, not retail's: what retail's exporter measured
// is unwitnessed (docs/anim/adm-bad-format-re.md). Both vectors come back with
// frame_count + 1 entries.
void bad_clip_extents(const BadBuildClip &clip, const BadBuildClip *reset,
                      std::vector<double> &bottom, std::vector<double> &top);

// Assemble `clip` into the document the writer serializes; `reset` is the
// set's reset clip (bad_build_reset_clip), null for a lone clip. False with
// `error` set for a clip the format cannot hold (no bones, a parent that is
// not a lower index, a key list that is not frame_count + 1 long, a
// translation block a flag promises and the clip lacks, a name over 31
// characters).
bool bad_build_assemble(const BadBuildClip &clip, const BadBuildClip *reset, BadAssembled &out,
                        std::string *error);

// Assemble and serialize in one step.
bool bad_build_mint(const BadBuildClip &clip, const BadBuildClip *reset, std::vector<uint8_t> &out,
                    std::string *error);

// Serialize the set's table through the canonical-form `.adm` writer. False
// with `error` set for a row the parser could not read back.
bool bad_build_mint_table(const BadBuildSet &set, std::string &out, std::string *error);

} // namespace opennova::bad
