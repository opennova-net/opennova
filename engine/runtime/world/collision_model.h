// The collision model's records and the frames that place them: the query-side COBJ / BVOL /
// BPLN / CFAC layout (CollisionModel and its parts), the Q22 placement matrix
// (CollisionMatrix and its builders) and the terrain clip and settle helpers the queries share.
// Split out of collision.h by responsibility (the oversize-header ratchet, A2-43); collision.h
// includes it. Witness record: docs/world/world-wac-ai-re.md §15.
#pragma once

#include <cstdint>
#include <vector>

#include <runtime/world/entity.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

// ----------------------------------------------------------------------------
// Runtime collision model (per graphic).
// ----------------------------------------------------------------------------

// [orig: runtime BPLN record, 12 B — int16 flags @+0 + Q14 normal xyz @+2/+4/+6 +
// 16.16 dist @+8; dot scales witnessed as >>14 (ray clip) and >>9 vs 32*dist
// (contact force). The sound-occlusion clip is the witnessed flags consumer:
// a nonzero flags BYTE (the word's low byte, @ 0x538d00) clamps that plane's
// clip radius at 0 (D-SND-9).]
struct CollisionPlane {
    int16_t flags = 0;              // authored BPLN flags word
    int16_t nx = 0, ny = 0, nz = 0; // Q14 unit normal
    int32_t dist = 0;               // 16.16 plane distance
};

// [orig: runtime BVOL record, 40 B — type @+0, AABB minX,maxX,minY,maxY,minZ,maxZ
// @+4..+24, plane_count @+28, plane_ptr @+32, flags @+36.]
struct CollisionVolume {
    int32_t type = 0;
    int32_t min_x = 0, max_x = 0;   // section-local 16.16
    int32_t min_y = 0, max_y = 0;
    int32_t min_z = 0, max_z = 0;
    int32_t plane_start = 0;        // run into CollisionModel::planes
    int32_t plane_count = 0;
    uint32_t flags = 0;             // authored bvol flags (blink letters)
};

// Poly Collision LOD records (CVRT/CNRM/CFAC). These preserve the authored
// fixed-point query data and source order and remain deliberately distinct
// from every BVOL gameplay family: CB (generic collision), CL (ladder), CA
// (armory), VC (vehicle collision), BB (blink box), CD (door), CT (change team),
// CF (flag/special function), DH/DM/DL (contact damage), CP (player collision),
// and the other trigger volumes. Ordinary projectile narrow phase uses CFAC,
// never BVOL substitutes.
struct CollisionVertex {
    int32_t p[3] = {};              // section-local 16.16
};

struct CollisionNormal {
    int16_t n[3] = {};              // exact signed Q14
    int16_t dominant_axis = 0;      // projection: 1=XY, 2=XZ, 4=YZ
};

struct CollisionFace {
    // Exact indexed representation.
    int16_t vertex_index[3] = {};
    int16_t normal_index = -1;
    uint32_t material_flags = 0;
    uint8_t poly_type = 0;
    // Runtime Q8/embedded-normal representation.
    int16_t v[3] = {};
    int16_t normal[3] = {};
    int16_t axis = 0;
    int32_t plane_dist = 0;
    int32_t min[3] = {}, max[3] = {};
    uint32_t flags = 0;
    uint8_t material = 0;
};

// [orig: runtime COBJ record, 108 B — volume count @+28, volume ptr @+36,
// first type-7 vehicle-pass start @+32, local AABB minX,maxX,minY,maxY,minZ,maxZ
// @+68..+88, bound-sphere
// center @+92..+100 + radius @+104.]
// The COBJ parent is not carried: section matrix i pairs with COBJ i strictly
// by ordinal, even when several rows share one parent. [orig: runtime COBJ +40
// = disk parent @0x5B3FCD; read only by Bone_BuildWorldMatrices @0x40C770
// behind the 'bfst' bone callback (@0x4E33D0, table row @0x82CF90), which no
// JO/revx02 RMDL selects]
// The COBJ flags bit a blast breaks the section off by: a blast whose box holds
// the section box's middle takes a section whose flags carry it [orig:
// Entity_ApplyWeaponDamage @0x4E6C5E..0x4E6E6B, the bit @0x4E6CD0].
inline constexpr uint32_t kCollisionSectionBreaks = 0x2u;
inline constexpr bool collision_section_breaks(uint32_t flags) {
    return (flags & kCollisionSectionBreaks) != 0;
}

struct CollisionSection {
    uint32_t flags = 0; // COBJ+0, kCollisionSectionBreaks selects blast breakage [orig: @0x4E6CD0]
    int32_t vertex_start = 0;
    int32_t vertex_count = 0;
    int32_t normal_start = 0;
    int32_t normal_count = 0;
    int32_t face_start = 0;
    int32_t face_count = 0;
    int32_t volume_start = 0;       // run into CollisionModel::volumes
    int32_t volume_count = 0;
    int32_t face_vertex_start = 0;  // run into CollisionModel::face_vertices [orig: COBJ+8]
    int32_t face_vertex_count = 0;  // [orig: COBJ+4]
    int32_t vehicle_volume_start = -1; // first type-7 volume (-1 = none)
                                       // [orig: COBJ+32]
    int32_t min_x = 0, max_x = 0;   // section-local 16.16 AABB
    int32_t min_y = 0, max_y = 0;
    int32_t min_z = 0, max_z = 0;
    int32_t offset[3] = {};         // exact COBJ section offset (16.16)
    int32_t center[3] = {};         // bound-sphere center (section-local 16.16)
    int32_t radius = 0;             // bound-sphere radius (16.16); negative = absent synthetic row
    bool authored_bounds = false;   // exact COBJ bounds/radius were retained
};

// ----------------------------------------------------------------------------
// The round-raycast face mesh (the "bullet LOD"): per-section runs of Q8 int16
// vertices, Q14 face normals with the projection-axis flag, and 44-B-equivalent
// face records. [orig: the runtime CVRT/CNRM/CFAC arrays hung off each COBJ by
// the collision builder @ 0x5b3bf0; walked ONLY by the projectile ray
// Physics_RaycastAgainstBoneCollision @ 0x4e4cb0 — movement/LOS queries walk
// the BVOL volumes instead.]
// ----------------------------------------------------------------------------
struct CollisionFaceVertex {
    int16_t x = 0, y = 0, z = 0; // section-local Q8 (16.16 >> 8); <<8 to 16.16
};

struct CollisionModel {
    std::vector<CollisionSection> sections;
    std::vector<CollisionVertex> vertices;
    std::vector<CollisionNormal> normals;
    std::vector<CollisionFaceVertex> face_vertices; // Q8 per-section runs
    std::vector<CollisionFace> faces;               // shared per-section runs
    std::vector<CollisionVolume> volumes;
    std::vector<CollisionPlane> planes;
    // Model-level AABB (union of the section AABBs, mission axes 16.16) — the
    // runtime collision-header bounds the render occlusion reads. [orig: the
    // collision block +24..+44 min/max fields, consumed by render_TOC's corner
    // refinement @ 0x5c4920, the water-straddle checks @ 0x5c8922, and
    // Entity_ComputeBoundingSphere @ 0x5c69a0]
    int32_t min[3] = {}, max[3] = {};
    // Conservative model-origin sphere used only when Entity::bound_radius was
    // not host-stamped. Cached at finalize time; u64 retains the full unscaled
    // Q16 diagonal before per-entity scale and signed-runtime clamping.
    uint64_t fallback_bound_radius_q16 = 0x10000u;
    // Precomputed membership for the TYPE-1-only LOS/ground segment walkers.
    // Models containing only triggers, blink volumes, ladders, or vehicle
    // volumes can never clip those rays and are rejected before entity/matrix
    // work without changing contact-query behavior.
    bool has_solid_volume = false;
    bool valid() const { return !sections.empty(); }

    // Derive section AABB/bound-sphere from its volumes (the loader precomputes
    // these on the runtime records; we rebuild them at model build time).
    void finalize_sections();
};

// One fully resolved section-debris particle sampled from the intact CFAC
// collision mesh. The death chain chooses the effect name from `material`.
struct SectionDebrisSample {
    Vec3 pos;
    Vec3 dir;
    uint8_t material = 0;
};

// ----------------------------------------------------------------------------
// Section transform: 16-dword fixed matrix, Q22 rotation rows + 16.16 translation,
// m[15] low bit = section disabled. [orig: the per-section matrix array returned
// by the model's transform callback (model+168); consumed via
// Math_TransformPointWithTranslation22 @ 0x412f60 (rotate>>22 + translate),
// Math_TransformPointFixedPoint22 @ 0x412e90 (rotate only), Math_FixedPointTransformPoint22
// @ 0x615810 (each row's dot plus 0x200000, >> 22, plus the row's translation word
// @0x61583B..0x61584E), inverted by Matrix_Transpose3x3WithNegateCol3
// @ 0x6136d0 (orthonormal transpose + negated rotated translation). Row/column
// convention is self-consistent here: we both build and invert the matrices.]
// ----------------------------------------------------------------------------
struct CollisionMatrix {
    int32_t m[16] = {};

    bool disabled() const { return (m[15] & 1) != 0; }
    // Projectile polygon traversal rejects either of the two low state bits.
    // [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0, matrix+60 & 3]
    bool polygon_disabled() const { return (m[15] & 3) != 0; }

    // world = R * local >> 22 + t
    void transform_point(const int32_t in[3], int32_t out[3]) const;
    // world dir = R * local >> 22 (no translation)
    void rotate_point(const int32_t in[3], int32_t out[3]) const;
    // out = inverse(this) for orthonormal rotations: R' = transpose(R),
    // t' = -(R' * t) >> 22. [orig: Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0]
    void invert_into(CollisionMatrix &out) const;
    // Safe port of Math_BuildInverseFixedPointMatrix3x3 @ 0x613e10. `this`
    // must contain a uniformly scaled orthogonal basis and scale_q16 must be
    // the same effective scale used to build it. False rejects retail-crashing
    // zero/overflow divisors.
    bool invert_uniform_scale_into(CollisionMatrix &out, int32_t scale_q16) const;
};

// Build the quantized yaw-only matrix (heading in BAM32 through the direction
// table, translation 16.16): the test fixtures', the editor range preview's and
// the minimap outline's matrix.
// No entity placement uses it: target_view places through
// collision_matrix_from_placement (D-NET-NEW-L6-10).
CollisionMatrix collision_matrix_from_heading(int32_t heading_bam, const int32_t pos[3]);

// The FULL placement matrix Rz(heading)·Ry(-pitch)·Rx(roll) for statics authored
// with pitch/roll (rocks rolled onto slopes, tilted wrecks) — the collision
// shell must lean WITH the visual or rounds thread past its edge where the
// model still looks solid. [orig: Math_BuildFixedPointMatrixFromEulerAngles
// @ 0x613f40; the spawn euler pack @ 0x40eb66.]
CollisionMatrix collision_matrix_from_euler(int32_t heading_bam, int32_t pitch_bam,
                                            int32_t roll_bam, const int32_t pos[3]);

// Apply one row-major, row-vector render/PANM pose to an entity collision
// matrix, returning the final fixed section matrix. This reproduces the exact
// retail callback sandwich:
//   fixed entity --0x611080 swizzle--> render float
//   pose * entity                         [row-vector order]
//   render float --0x611140 inverse--> final Q22/16.16
// Conversion rejects non-finite/out-of-range input instead of invoking an
// undefined host float-to-int cast. The affine pose's m[15] is never treated
// as the collision section-disabled bit.
// [orig: BoneCallback_Generic @ 0x4e26d0;
// Math_FixedPointToFloatMatrix4x4_Swizzled @ 0x611080;
// Math_FloatMatrixToFixedPoint22 @ 0x611140.]
// The entity placement matrix retail rebuilds at entity+0xB4 before it
// transforms a userpoint or the bbox center: the euler rotation from the live
// heading (the vehicle motor's BAM mirror when seeded, else the whole-degree
// mission yaw), pitch and roll about the 16.16 position, the uniform item
// scale riding the rotation diagonal [orig: the builder select
// @0x43b56c..0x43b5bd; Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale
// @0x614210; Math_BuildFixedPointMatrixFromEulerAngles @0x613f40].
CollisionMatrix entity_placement_matrix(const Entity &e);
// The scaled placement matrix: the same three stages as collision_matrix_from_euler over the
// diagonal scale << 6 (Q16 -> Q22), so the scale nests inside every stage's truncations; a 0
// scale is the unscaled builder. Retail's placement and pose matrices carry it, so the polygon
// walker selects the matching scaled inverse.
// [orig: Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale @0x614210]
CollisionMatrix collision_matrix_from_euler_scaled(int32_t heading_bam, int32_t pitch_bam,
		int32_t roll_bam, const int32_t pos[3], int32_t scale_q16);
// The collision placement of an entity from its Yaw word (Entity::heading,
// D-NET-NEW-L6-2), its pitch / roll degrees, its 16.16 position and its item
// scale (collision_matrix_from_euler_scaled): the full Euler matrix for every
// placement, a pure-yaw one included, with its exact x87 sine and cosine (the
// quantized direction table is no part of it): a yaw off the table's 2^22 BAM
// grid turns the shell with the visual, not up to 0.18 degrees off it
// (D-NET-NEW-L6-10). [orig: BoneCallback_Simple @0x4E2600 (the build @0x4E2662)
// and BoneCallback_Generic @0x4E26D0 (@0x4E2745) call
// Math_BuildFixedPointMatrixFromEulerAngles @0x613F40 on every unscaled
// placement, whose yaw stage @0x6140B2..0x6141DA runs fsin / fcos; a building's,
// decoration's or powerup's is built once by Entity_InitFromModel @0x40DC30 (the
// build @0x40E0CC), the same builder, which then sets Flags 0x20000 @0x40E105 so the
// callbacks keep it]
CollisionMatrix collision_matrix_from_placement(int32_t heading, int32_t pitch_bam,
                                                int32_t roll_bam, const int32_t pos[3],
                                                int32_t scale_q16);
// A local box (min x, y, z, then max x, y, z; 16.16) as `matrix` places it:
// the world box its eight corners span.
void collision_matrix_box_bounds(const CollisionMatrix &matrix, const int32_t local_box[6],
                                 int32_t min_out[3], int32_t max_out[3]);
// That matrix's live Euler triple (BAM32 heading, pitch, roll), shared with
// the render-slot march start (renderer::slot_march_start_offset), which
// builds the same rotation without the scale.
void entity_live_euler_bam(const Entity &e, int32_t out[3]);

bool collision_matrix_apply_render_pose(const CollisionMatrix &entity_world,
                                        const float pose_row_major[16],
                                        CollisionMatrix &out);

// The two halves of that sandwich on their own, for a caller that runs the bone pass
// over the entity's render float itself: the fixed matrix as the row-major render
// float (the (-y, z, x) swizzle; rotation / 2^22, translation / 2^16), and a render
// float back to Q22 / 16.16, each entry truncated toward zero (false on a non-finite
// or out-of-range entry). [orig: Math_FixedPointToFloatMatrix4x4_Swizzled @0x611080;
// Math_FloatMatrixToFixedPoint22 @0x611140]
void collision_matrix_to_render_float(const CollisionMatrix &fixed, float out_row_major[16]);
bool collision_matrix_from_render_float(const float in_row_major[16], CollisionMatrix &out);

// An addeweap child's fixed matrix on a userpoint of its carrier's root part:
// the carrier matrix as render floats, the userpoint's 16.16 position words
// (x, y, z) through it, its direction words (x, y, z) as a look-at frame
// times it, back to Q22 / 16.16. A root part with no animation leaves the
// bone matrix the carrier's own. False on a non-finite or out-of-range float.
// [orig: Bone_BuildAttachmentMatrix @0x56C630 (from Entity_UpdateTransformAndTurret
//  @0x44109D); Math_FixedPointToFloatMatrix4x4_Swizzled @0x611080;
//  Math_TransformPointByMatrix4x4 @0x40CF20; Math_BuildDirectionLookAtMatrix
//  @0x612C90; Math_MultiplyMatrix4x4_Float @0x611750;
//  Math_FloatMatrixToFixedPoint22 @0x611140]
bool collision_matrix_attach_userpoint(const CollisionMatrix &carrier, const int32_t point[3],
                                       const int32_t look[3], CollisionMatrix &out);

// The inverse of the euler builder: yaw/pitch/roll (BAM32) back out of a Q22
// rotation — yaw from row 1 col 0 over row 0 col 0, then the yaw-unrotated
// pitch from row 2 col 0, then roll. Every product is a 64-bit `>> 22` with no
// rounding bias and every angle truncates toward zero, exactly as the x87 code.
// The userpoint fire transform reports the posed bone's euler through it.
// [orig: Math_FixedPointMatrixToEulerAngles @ 0x613310 (yaw @ 0x61332c..
//  0x61335d, the A/B/C terms @ 0x61339a..0x613400, pitch @ 0x6133fa..0x613421,
//  D + roll @ 0x61344a..0x61347b)]
void collision_matrix_to_euler(const CollisionMatrix &m, int32_t out_yaw_pitch_roll[3]);

// The terrain leg of the LOS segment query, TRUE = the segment hits terrain
// (blocked). The ported heightmap raycast over the runtime height field; shared
// by CollisionWorld::raycast_clear, the sound-occlusion LOS, and the
// collision-less AiSystem fallback.
// [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760, faithfully ported as
// terrain_raycast_los_clear (the occlusion slice closed the sibling-internals
// open item): endpoint prechecks + the 4u-texel point-sample march + the
// height-0 floor; nonzero = clear.]
bool los_terrain_blocked(const terrain::TerrainHeightField &field, const int32_t a[3],
                         const int32_t b[3]);

// A LOS endpoint's parent slot in the pool walkers' exclusion set: the seat
// mount (+0x16C parentEntity) wins over the carried object (+0x268
// mountedChild); `parent_cleared` models a caller that nulls +0x16C around
// its query, leaving only the carried object [orig:
// Physics_RaycastTerrainAndSectors @0x5399C6..0x539A12;
// Physics_RaycastFindCollisionEntity @0x539AB8..0x539B10].
EntityHandle los_walker_parent(const Entity *e, bool parent_cleared = false);

// Terrain clip of a segment: on a hit writes the refined hit point to out_hit
// and returns true; on clear out_hit is untouched. The iris camera-ray clip's
// terrain leg [orig: Entity_RaycastCollision @ 0x413760 ->
// Terrain_RaycastHeightmapHiRes_0 @ 0x60e710 — called (start, end, end), the
// ray end clipping in place].
bool terrain_clip_segment(const terrain::TerrainHeightField &field, const int32_t a[3],
                          const int32_t b[3], int32_t out_hit[3]);

// The movement resolver's ground-settle tail over the terrain alone (an empty candidate set): Z
// raised to the 6144 grid, a 2.0 u column clipped by the heightfield (an indoors body, or none,
// skips it: the column's foot is the ground), then clearance = feet (pos z less the capsule
// bottom) - ground. The motor runs it with no collision world wired; the editor's mission view
// stands a placed person's spawn on the terrain with it.
// [orig: Entity_MovementCollisionResolver ground-settle tail @0x4B3D6E..0x4B3DA9 ->
//  Entity_RaycastGroundHeightAndObject(e, 0, 0, 0, 0x20000) @0x4B3D95 -> Entity_RaycastCollision
//  terrain leg @0x413760 (no candidates)]
int32_t terrain_settle_clearance(const terrain::TerrainHeightField *field, const int32_t pos[3],
                                 int32_t capsule_bottom, bool indoors);

// The ground-settle tail's probe origin: Z raised to the 6144 grid,
// (z + 6143) & ~0x17FF in 32-bit two's complement. Every port of the tail
// starts its 2.0 u ground column here.
// [orig: Entity_MovementCollisionResolver @0x4B3D6E..0x4B3DA9]
inline int32_t ground_probe_origin_z(int32_t z) {
    return static_cast<int32_t>((static_cast<uint32_t>(z) + 0x17FFu) & ~0x17FFu);
}

// The terrain's height under (x, y), 16.16: a vertical column through the
// whole span, which the heightfield clip writes the terrain height into
// whatever the ray's reach; false with no field or no terrain under it.
// [orig: Terrain_RaycastHeightmapLoRes @0x60cc12..0x60cc2d]
bool terrain_column_height(const terrain::TerrainHeightField *field, int32_t x, int32_t y,
                           int32_t &height);

} // namespace opennova::world
