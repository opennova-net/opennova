// World-object collision: the runtime queries the original engine runs against a
// model's collision block (the .3di CDTA volumes/planes), plus the per-tick
// proximity tables that feed them. Witness record: docs/world/world-wac-ai-re.md
// §15 (collision + blink boxes), grilled 2026-07-09 against Jointops.exe.
//
// Runtime data (the query-side layout, witnessed in the three query functions):
//   COBJ section records — volume list + local AABB + bound sphere
//   BVOL volume records  — collidable type + local AABB + plane run + flags
//   BPLN plane records   — int16 Q14 normal + 16.16 distance
// The host builds these PODs from the parsed .3di collision IR (the same
// leaf-consumer seam as terrain_query's TerrainHeightField, ADR 0020): libs/world
// never touches the format stack.
//
// Collidable types (runtime dispatch, witnessed in
// Entity_ComputeBoneCollisionForce @ 0x4ae150):
//   1  generic collision box ("CB") -> ordinary solid contact and generic rays
//   4  ladder volume ("CL")  -> contact flag 0x1 + ladder alignment frame
//   6  armory volume ("CA")  -> contact flag 0x4 -> entity Flags |= 0x400000
//      (gates the in-game armory screen: input action 218 opens weapon.mnu WEAPON
//       only while this flag is set [orig: Input_HandleActionBinding @ 0x49b848])
//   7  vehicle collision ("VC") -> solid on the vehicle-contact mask 0x8 path
//   8  blink box ("BB")      -> contact flag 0x10 + blink accumulation (buildings)
//   9  CD door touch         -> contact flag 0x20 + door-section bit on target
//   10 CT change-team box    -> contact flag 0x200
//   11 vehicle-loadout volume -> contact flag 0x400 -> entity Flags |= 0x800
//      (gates vehicle.mnu VEHICLE on the same key [orig: @ 0x49b858])
//   12 masked volume (mask 0x10 path)
//   13 CF flag volume        -> contact flag 0x800 (source stands on target;
//      the authoring manual describes the family as special-function/FARP activation)
//   16 DH damage high        -> contact flag 0x100 (-50 HP)
//   17 DM damage medium      -> contact flag 0x80  (-6 HP)
//   18 DL damage low         -> contact flag 0x40  (-1 HP)
//   19 CP player collision   -> solid only on the player mask 0x2 (not AI)
//   1 (and other unlisted types) solid -> SAT push-out force
//   5  contact-no-force marker
// Raycasts test ONLY type-1 volumes [orig: Entity_RaycastCollisionModel @ 0x413060];
// the blink point query tests ONLY type-8 volumes of building-kind entities
// [orig: Entity_TestCollisionSections @ 0x4aef90].
#ifndef OPENNOVA_WORLD_COLLISION_H
#define OPENNOVA_WORLD_COLLISION_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "world/entity.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;

// Static proximity coordinates are signed whole mission units stored in u16
// table words. Decode explicitly instead of relying on signed left-shift wrap.
inline int32_t static_slot_coord_units(uint16_t word) {
    const int32_t raw = static_cast<int32_t>(word);
    return raw >= 0x8000 ? raw - 0x10000 : raw;
}

inline int32_t static_slot_coord_q16(uint16_t word) {
    return static_slot_coord_units(word) * 0x10000;
}

// ----------------------------------------------------------------------------
// Runtime collision model (per graphic).
// ----------------------------------------------------------------------------

// [orig: runtime BPLN record, 12 B — int16 flags? + Q14 normal xyz @+2/+4/+6 +
// 16.16 dist @+8; dot scales witnessed as >>14 (ray clip) and >>9 vs 32*dist
// (contact force).]
struct CollisionPlane {
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
// type-7/12 vehicle-pass start @+32, local AABB minX,maxX,minY,maxY,minZ,maxZ
// @+68..+88, bound-sphere
// center @+92..+100 + radius @+104.]
struct CollisionSection {
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
    int32_t vehicle_volume_start = -1; // first type-7/12 vehicle-pass volume (-1 = none)
                                       // [orig: COBJ+32]
    int32_t min_x = 0, max_x = 0;   // section-local 16.16 AABB
    int32_t min_y = 0, max_y = 0;
    int32_t min_z = 0, max_z = 0;
    int32_t offset[3] = {};         // exact COBJ section offset (16.16)
    int32_t center[3] = {};         // bound-sphere center (section-local 16.16)
    int32_t radius = 0;             // bound-sphere radius (16.16); negative = absent synthetic row
    // Hierarchy metadata copied from COBJ::parent_subobject_index. This is NOT
    // the section-matrix selector: retail pairs callback matrix i with COBJ i
    // strictly by ordinal, even when several COBJ rows share one parent.
    int32_t parent_part_index = -1;
    int32_t part_index = -1;        // source render part
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
    bool valid() const { return !sections.empty(); }

    // Derive section AABB/bound-sphere from its volumes (the loader precomputes
    // these on the runtime records; we rebuild them at model build time).
    void finalize_sections();
};

// ----------------------------------------------------------------------------
// Section transform: 16-dword fixed matrix, Q22 rotation rows + 16.16 translation,
// m[15] low bit = section disabled. [orig: the per-section matrix array returned
// by the model's transform callback (model+168); consumed via
// Math_TransformPointWithTranslation22 @ 0x412f60 (rotate>>22 + translate),
// Math_TransformPointFixedPoint22 @ 0x412e90 / Math_FixedPointTransformPoint22
// @ 0x615810 (rotate only), inverted by Matrix_Transpose3x3WithNegateCol3
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

// Build the quantized yaw-only entity matrix (heading in BAM32, translation
// 16.16). Pure-yaw placements retain this exact table path; target_view uses
// the full-Euler builder when needed and layers callback section poses above it.
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
bool collision_matrix_apply_render_pose(const CollisionMatrix &entity_world,
                                        const float pose_row_major[16],
                                        CollisionMatrix &out);

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

// Terrain clip of a segment: on a hit writes the refined hit point to out_hit
// and returns true; on clear out_hit is untouched. The iris camera-ray clip's
// terrain leg [orig: raycast_entity_collision @ 0x413760 ->
// Terrain_RaycastHeightmapHiRes_0 @ 0x60e710 — called (start, end, end), the
// ray end clipping in place].
bool terrain_clip_segment(const terrain::TerrainHeightField &field, const int32_t a[3],
                          const int32_t b[3], int32_t out_hit[3]);

// ----------------------------------------------------------------------------
// Per-query blink accumulation. [orig: g_BlinkFlagsAccum @ 0xB57C70,
// g_BlinkHitSlot0..3 @ 0xB57C74, g_BlinkHitCount @ 0x82AE20 — cleared per query
// scope; hits pack ((section & 0x1F) | (pool_index << 8)) << 12; on hit the
// accumulator ORs volume.flags ^ 6, and bit 2 drives entity Flags 0x800000
// ("indoors").]
// ----------------------------------------------------------------------------
struct BlinkAccum {
    uint32_t flags = 0;
    int32_t hit_count = 0;
    uint32_t hits[4] = {};

    void reset() { flags = 0; hit_count = 0; hits[0] = hits[1] = hits[2] = hits[3] = 0; }
    void add_hit(int32_t section_index, int32_t pool_entity_index) {
        if (hit_count != -1 && hit_count < 4)
            hits[hit_count++] =
                static_cast<uint32_t>(((section_index & 0x1F) + (pool_entity_index << 8)) << 12);
    }
    // Decoders for the packed key add_hit writes (consumed by the occlusion
    // camera-containment walk [orig: @0x5c88c9-0x5c8938]).
    static constexpr int32_t hit_section(uint32_t key) {
        return static_cast<int32_t>((key >> 12) & 0x1F);
    }
    static constexpr int32_t hit_pool_entity_index(uint32_t key) {
        return static_cast<int32_t>(key >> 20);
    }
};

inline constexpr uint32_t kBlinkIndoorsBit = 0x2;  // accum bit -> kEntityFlagIndoors
inline constexpr uint32_t kBlinkWaterOffBit = 0x8; // authored water letter — both water passes
                                                   // skipped [orig: Terrain_RenderSceneWithReflection
                                                   // @0x5c93cb; docs/render/render-occlusion-re.md §4]
// The entity Flags bit constants the touch dispatch writes (kEntityFlagIndoors/
// LadderContact/ArmoryZone/VehicleLoadoutZone) live in world/entity.h — the one
// home beside the field they describe.

// Bounding-volume type codes — the contained-point dispatch [orig: the switch
// @0x4ae887; the letter names are the Super OED manual's volume suffixes,
// docs/world/world-wac-ai-re.md §15.4].
namespace bvol_type {
enum : int32_t {
    kContactMarker = 5,   // contact, no force
    kLadderCL = 4,
    kArmoryCA = 6,        // gates weapon.mnu on action 218
    kVehicleVC = 7,       // vehicle-collision solid (mask 0x8 pass)
    kBlinkBB = 8,
    kDoorCD = 9,
    kChangeTeamCT = 10,
    kVehicleLoadout = 11, // gates vehicle.mnu
    kVehicleExt = 12,     // optional extension of the VC pass
    kFlagCF = 13,         // grounded-touch special function
    kDamageHighDH = 16,
    kDamageMediumDM = 17,
    kDamageLowDL = 18,
};
} // namespace bvol_type

// Touch-accum bits the dispatch ORs into CollisionQueryResult::flags — one bit
// per volume family [orig: the |= writes @0x4ae894..0x4aebb3].
inline constexpr uint32_t kTouchLadder = 0x1;
inline constexpr uint32_t kTouchArmory = 0x4;
inline constexpr uint32_t kTouchBlink = 0x10;
inline constexpr uint32_t kTouchDoor = 0x20;
inline constexpr uint32_t kTouchDamageLow = 0x40;
inline constexpr uint32_t kTouchDamageMedium = 0x80;
inline constexpr uint32_t kTouchDamageHigh = 0x100;
inline constexpr uint32_t kTouchChangeTeam = 0x200;
inline constexpr uint32_t kTouchVehicleLoadout = 0x400;
inline constexpr uint32_t kTouchFlagGrounded = 0x800;

// Collision-face flag bits [orig: face tests @0x4e5073-family; the GDScript
// debug view mirrors these in hitbox_debug_view.gd].
inline constexpr uint32_t kFaceFlagBothSides = 0x1;
inline constexpr uint32_t kFaceFlagNeverHit = 0x100;
inline constexpr uint32_t kFaceFlagDoubleSided = 0x800;

// A test point (stride-4 record, xyz + spare — faithful to the caller layout).
struct CollisionPoint {
    int32_t x = 0, y = 0, z = 0, w = 0;
};

// The target side of a query: the placed entity's collision instance + the entity
// fields the original reads off GamePlayerEntity.
struct CollisionTargetView {
    const CollisionModel *model = nullptr;
    const CollisionMatrix *matrices = nullptr; // one per section
    int32_t pos[3] = {};                       // entity+4/+8/+12 (16.16)
    int32_t yaw_bam = 0;                       // entity+16 Yaw (BAM32) — ladder-contact leg
    int32_t pitch_bam = 0;                     // entity+20 Pitch (BAM32) — ladder-contact leg
    uint32_t entity_flags = 0;                 // entity+36 Flags (contact rejects flags & 1)
    int32_t bound_radius = 0;                  // entity+0 boundRadius (16.16)
    bool is_building = false;                  // itemDef type == 5 (blink gate)
    int32_t pool_index = 0;                    // pool index for the packed blink hit
    bool is_ground_of_source = false;          // source->groundEntity == target (type 13)
    // Effective entity+0x158/itemDef+0x1B8 uniform scale metadata. The pose
    // matrix already contains this scale; it only selects the matching inverse.
    int32_t uniform_scale_q16 = 0;
    // True only when the pose owner published one live matrix per COBJ section.
    // A yaw-only rigid fallback deliberately does not claim animated-bone fidelity.
    bool live_section_pose = false;
};

// ----------------------------------------------------------------------------
// Point-vs-blink query. Tests points (with per-point radii) against the TYPE-8
// volumes of a building-kind target; accumulates blink flags/hits into `blink`.
// Returns true when any point sits inside a blink volume.
// [orig: Entity_TestCollisionSections @ 0x4aef90]
// ----------------------------------------------------------------------------
bool collision_test_blink(const CollisionTargetView &target, const CollisionPoint *points,
                          const int32_t *radii, int32_t num_points, BlinkAccum &blink);

// ----------------------------------------------------------------------------
// Segment-vs-solid query. Clips ray.end to the nearest entry point into any
// TYPE-1 volume; returns true on hit. [orig: Entity_RaycastCollisionModel
// @ 0x413060 — per-section bound-sphere reject, then convex clip of the segment
// against the volume's plane run (dot >> 14 + dist), entry point written back.]
// ----------------------------------------------------------------------------
struct CollisionRay {
    int32_t start[3] = {};
    int32_t end[3] = {};   // clipped in place on hit
    int32_t mid[3] = {};
    int32_t half[3] = {};  // abs half extents
    int32_t dir[3] = {};   // normalized 16.16 direction (float-normalized like the orig)

    // Build mid/half/dir from start/end. [orig: prologue of raycast_entity_collision
    // @ 0x413760 / Entity_FindNearestByRay @ 0x413af0 (float normalize, ftol)]
    void refresh();
    // Recompute mid/half only — dir is built ONCE at ray construction and never
    // re-derived from a clipped segment. [orig: the hit tail @ 0x41370c-0x41374e
    // refreshes point[6..11] and leaves dir untouched]
    void refresh_bounds();
};

// Metadata for the solid volume that most recently clipped `ray` to its nearest
// entry point. The legacy callers only need the bool and leave this null; the
// projectile query retains section identity for downstream impact policy.
struct CollisionModelHit {
    int32_t section_index = -1;
    int32_t volume_index = -1;
    int32_t normal_q16[3] = {};
};

bool collision_raycast_model(const CollisionTargetView &target, CollisionRay &ray,
                             CollisionModelHit *out_hit = nullptr);

// Segment-vs-authored collision polygons. This is deliberately separate from
// collision_raycast_model: retail projectile geometry is CVRT/CNRM/CFAC, while
// the older query above is the TYPE-1 CB/BVOL solid path used by LOS/contact.
// distance_q16 is distance along the segment in world 16.16 units.
// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]
struct CollisionPolygonHit {
    int32_t distance_q16 = 0x7FFFFFFF;
    int32_t position_q16[3] = {};
    int32_t normal_q16[3] = {};
    int32_t section_index = -1;
    int32_t face_index = -1;
    int32_t section_face_index = -1;
    uint32_t material_flags = 0;
    int32_t poly_type = -1;
};

bool collision_raycast_polygons(const CollisionTargetView &target,
                                const CollisionRay &ray,
                                int32_t segment_length_q16,
                                uint32_t ammo_flags,
                                CollisionPolygonHit &out_hit);

// ----------------------------------------------------------------------------
// Segment-vs-face-mesh query — the PROJECTILE hit test. Closest accepted face
// across every enabled section; the sphere broad phase is the caller's.
// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0 — per-bone inverse
// transform of the segment, face AABB reject, flags & 0x100 skip, the
// material-17 foliage skip when the ammo carries flag 0x4000000, the
// plane-straddle test ((v.n >> 14) + dist on both endpoints), the direction
// rule (face flag 1 = both sides; 0x800 = double-sided via the witnessed
// nonzero stack-residue arg; else enter-front d0>0 && d1<=0), the distance
// split |d0| * len / (|d0| + |d1|) with the "Rounds Divide Error"
// 0x40000000 clamp, accept at <= best, and the odd-even point-in-triangle on
// the normal's projection plane (Math_PointInTriangle2D @ 0x414050, Q8
// vertices << 8).]
// ----------------------------------------------------------------------------
struct RayFaceHit {
    int32_t dist = 0;       // 16.16 distance along the segment at the hit
    uint32_t face_flags = 0;
    uint8_t material = 0;   // -> the impact effect tag (material + 4)
    int32_t section = -1;
    int32_t face = -1;
};
bool collision_raycast_faces(const CollisionTargetView &target, const int32_t start[3],
                             const int32_t end[3], uint32_t ammo_flags, RayFaceHit &out);

// Person/organic projectile narrow phase: one authored COBJ bound sphere per
// skeletal section (radius zero still receives retail's fixed 0xCCC floor),
// transformed by the callback matrix with the same strict
// ordinal pairing as the face walker. primary_section is the first accepted
// section in retail's reverse scan (the reaction/death-animation bone), while
// secondary_section is the final overlap and normal-infantry damage zone.
// [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670]
struct PersonSectionHit {
    int32_t dist = 0;               // ray[29]: projected distance - authored radius / 2
    int32_t projected_dist = 0;     // ray-line projection for the primary section
    int32_t primary_section = -1;   // ray[31]: highest accepted section ordinal
    int32_t secondary_section = -1; // ray[32]: lowest accepted section ordinal
    uint8_t material = 19;          // fixed retail organic material
};
bool collision_raycast_person_sections(const CollisionTargetView &target,
                                       const int32_t start[3], const int32_t end[3],
                                       int32_t extra_radius, uint32_t section_mask,
                                       PersonSectionHit &out);

// ----------------------------------------------------------------------------
// Contact force query: capsule test points vs every volume of the target model.
// Faithful port of Entity_ComputeBoneCollisionForce @ 0x4ae150 (the SAT push-out
// over the plane run with prev-position gating, second-plane assist, per-type
// flag dispatch, blink accumulation, and the bound-radius force clamp).
//
// mask bits: 0x1 = ladder recontact (inflated CL/type-4 test), 0x2 = player
// (test CP/type-19), 0x8 = vehicle collision query (use a section's type-7/12 run
// when present; otherwise fall back to its CB/default solids), 0x10 = type-12 pass.
// out_force is the world-space push (16.16, already >>5-scaled); out_flags is
// the contact-flag word listed in the type table above.
// ----------------------------------------------------------------------------
struct LadderContact {
    // [orig: globals @ 0xB5AB70..80 — written by the CL/type-4 hit. Plane 0
    // supplies the authored ladder facing; the frame is consumed by retail's
    // climb alignment/motion, which is not ported yet.]
    int32_t anchor[3] = {};
    int32_t yaw = 0;
    int32_t pitch = 0;
    bool valid = false;
};

struct ContactQuery {
    const CollisionPoint *points = nullptr;
    const int32_t *radii = nullptr;
    int32_t num_points = 0;
    int32_t prev_pos[3] = {};      // source savedLivePose (plane gating)
    int32_t source_bound_radius = 0;
    uint8_t mask = 0;
    bool query_is_player = false;  // [orig: g_CollisionQueryIsPlayer @ 0xB5AB84]
};

struct ContactResult {
    int32_t force[4] = {};         // world push force (16.16); [3] spare like the orig
    uint32_t flags = 0;            // contact flags (see the type table)
    uint32_t door_sections = 0;    // CD/type-9 bit-per-section mask [orig: target+692]
};

bool collision_contact_force(const CollisionTargetView &target, const ContactQuery &q,
                             BlinkAccum &blink, LadderContact &ladder, ContactResult &out);

// Host/model callback for the final world-space matrix array consumed by every
// collision walk. Matrix slot i corresponds to COBJ/collision section i by
// ordinal; COBJ::parent_subobject_index is hierarchy metadata, not a selector.
// [orig: model+168 callback -> one 16-dword matrix per COBJ, consumed in lockstep
// by Physics_RaycastAgainstBoneCollision @ 0x4e4cb0.]
class ICollisionSectionMatrixProvider {
public:
    virtual ~ICollisionSectionMatrixProvider() = default;
    // A host may learn about dynamic entities after its mission-start model
    // sweep (notably the local player deploy). Give query callers one shared,
    // idempotent way to attach that entity before choosing an unresolved
    // fallback. Returning true means the provider attached a usable instance.
    virtual bool ensure_collision_instance(World &world, EntityHandle entity) {
        (void)world;
        (void)entity;
        return false;
    }
    // View construction may cache this result. Implementations must treat the
    // queried CollisionWorld's model/instance/pose state as read-only here;
    // late attachment belongs in ensure_collision_instance().
    virtual bool build_section_matrices(World &world, EntityHandle entity,
                                        int32_t model_id,
                                        const CollisionMatrix &entity_world,
                                        const CollisionModel &model,
                                        std::vector<CollisionMatrix> &out) = 0;
};

// ---------------------------------------------------------------------------
// Projectile trace shared by authoritative and explicitly visual-only rounds.
// Fixed-point terrain/entity arbitration lives here; arming, armor, health,
// scoring, and effects remain consequences owned by RoundSim.
// ---------------------------------------------------------------------------
struct FixedVec3 {
    int32_t x = 0, y = 0, z = 0;

    constexpr int32_t operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

// One decoded remote person (player or non-player infantry) exposed only to
// visual projectile collision. It is deliberately not an Entity: the wire
// handle belongs to the server's address space and must never alias a
// client-local registry handle or participate in movement, AI, explosions, or
// authoritative damage.
struct ProjectilePersonProxy {
    uint16_t wire_handle = 0xFFFF;
    FixedVec3 position_q16;
};

// One decoded pool-1 mover (vehicle, emplacement, runtime item) projected into
// visual projectile collision with its AUTHORED collision geometry at the
// decoded wire pose. Same wire-keyed rule as the person proxy: never an
// Entity, never a registry handle. The pose mirrors what the retail client
// holds on its own pool entity — the live compact updates the coarse heading
// byte (entity+16 high byte) while entity+20/+24 retain the last spawn/dead
// Euler sample [orig: the 0x0D spawn angle landings + the compact fold; the
// collision placement matrix @ 0x613f40 reads those same three fields].
struct ProjectileDynamicProxy {
    uint16_t wire_handle = 0xFFFF;
    int32_t model_id = -1;          // CollisionWorld model registry id; -1 = unresolved
    FixedVec3 position_q16;
    int32_t heading_bam = 0;        // reconstructed entity+16 (yaw_byte << 24)
    int32_t pitch_bam = 0;          // retained entity+20 spawn/dead sample
    int32_t roll_bam = 0;           // retained entity+24 spawn/dead sample
    int32_t bound_radius_q16 = 0;   // broad-phase sphere (entity+0 boundRadius stand-in)
};

enum class ProjectileHitClass : uint8_t {
    Terrain = 0,
    StaticEntity = 1,
    DynamicEntity = 2,
    Person = 3,
    Water = 4,
    None = 0xFF,
};

inline constexpr int32_t kProjectileAuthorityMinRadiusQ16 = 6553; // 0.1u

struct ProjectileTrace {
    FixedVec3 start;
    FixedVec3 end;
    EntityHandle owner;
    int32_t radius_q16 = 0;
    uint32_t ammo_flags = 0;
    // The additional per-projectile exclusion carried by the retail ray context.
    EntityHandle extra_ignore;
    // Remote decoded proxies (person + dynamic) are a client-presentation input
    // only. The caller opts in explicitly for a VisualOnly round and supplies
    // the wire shooter identity for the normal flag-4-aware self-collision rule.
    bool include_wire_proxies = false;
    uint16_t shooter_wire_handle = 0xFFFF;
    // The decoded shooter's carrier at fire time — the wire-side analog of the
    // retail ray[18] mount exclusion (a mounted shooter's round never clips its
    // own vehicle). Retail resolves it off the live shooter entity's mount
    // pointers; a visual client resolves it off the shooter's decoded
    // carrier_handle instead. [orig: the mount exclusion setup feeding
    // Physics_RaycastAgainstBoneCollision @ 0x4e4cb0 via ray[18]]
    uint16_t shooter_carrier_wire_handle = 0xFFFF;
    // The throwable/useownmove motor sweep walks pools 2/1 only — a flying
    // grenade passes through people, and a person in front of a vehicle must
    // not mask the vehicle hit [orig: the two
    // Projectile_RaycastProximitySlots(2/1, ...) calls @ 0x444619..0x444667].
    // Bullets keep the default full walk.
    bool walk_persons = true;
};

struct ProjectileHit {
    ProjectileHitClass hit_class = ProjectileHitClass::None;
    EntityHandle geometry_entity;
    int32_t t_q16 = 0x10000;
    FixedVec3 position_q16;
    FixedVec3 normal_q16;
    int32_t section_index = -1;
    int32_t face_index = -1;
    int32_t bone_index = -1;
    int32_t hit_zone = -1;
    int32_t surface_type = -1;
    uint32_t material_flags = 0;

    constexpr bool hit() const { return hit_class != ProjectileHitClass::None; }
};

// ----------------------------------------------------------------------------
// CollisionWorld: the per-tick proximity tables + per-entity instances, and the
// world-level blink state. [orig: the g_StaticProx*/g_DynProx*/g_PersonProx*
// tables + g_ProxCandidateArena rebuilt each tick by
// Entity_BuildProximityLists_Pool2 @ 0x4b9430, _Pool01 @ 0x4b9340 and
// Entity_BuildProximityListsFromPools @ 0x4b8eb0.]
// ----------------------------------------------------------------------------
class CollisionWorld {
public:
    // Opt-in, per-tick projectile-trace profile: the probe/F3 attribution
    // surface for sustained-fire cost. Times are microseconds; *_survivors
    // count geometric broad-phase passes and *_faces count CFAC face-set sizes
    // admitted to the static/dynamic narrow phases.
    struct TraceProfile {
        int64_t calls = 0;
        int64_t terrain_us = 0;
        int64_t static_us = 0;
        int64_t dynamic_us = 0;
        int64_t person_us = 0;
        int64_t static_survivors = 0;
        int64_t dynamic_survivors = 0;
        int64_t person_survivors = 0;
        int64_t static_faces = 0;
        int64_t dynamic_faces = 0;
    };
    const TraceProfile &trace_profile() const { return trace_profile_; }
    bool trace_profile_enabled() const { return trace_profile_enabled_; }
    // Profiling is disabled by default. Each enable/disable edge clears the
    // snapshot so a new consumer never inherits another capture's counters.
    void set_trace_profile_enabled(bool enabled);
    // --- model registry (host-fed, keyed by an opaque graphic id) ---
    int32_t add_model(CollisionModel model); // returns model id
    const CollisionModel *model(int32_t id) const;
    // Attach a model instance to a live entity (net_id keyed like the traits sweep).
    void assign_entity(EntityHandle h, int32_t model_id,
                       uint64_t registry_spawn_id = 0);
    // Drop both intact and husk bindings for one packed slot. Hosts use this
    // when the registry serial proves the slot now belongs to another entity.
    void remove_entity_instance(EntityHandle h);
    // Attach the husk-stage collision model (swapped in while Flags & 4).
    void assign_entity_husk(EntityHandle h, int32_t husk_model_id);
    // Install the model-animation callback that supplies final per-section
    // matrices. Null restores the static shared-entity-matrix fallback.
    void set_section_matrix_provider(ICollisionSectionMatrixProvider *provider);
    // Resolve a host-owned late-spawn instance on demand. Existing instances
    // never call the provider, so repeated round/F3 queries are idempotent.
    bool ensure_entity_instance(World &world, EntityHandle h);
    // Publish the pose owner's current world-space Q22 section matrices. Retail
    // collision consumes the model callback's live matrices rather than deriving
    // animation itself. Count must exactly match the assigned model's COBJ count.
    bool publish_entity_section_matrices(EntityHandle h,
                                         std::vector<CollisionMatrix> matrices);
    void clear_entity_section_matrices(EntityHandle h);
    bool has_instance(const World &world, EntityHandle h) const;
    bool has_instance(EntityHandle h) const;
    size_t instance_count() const { return instances_.size(); }

    // --- per-tick snapshots ---
    // [orig: Entity_BuildProximityLists_Pool2 @ 0x4b9430] statics (pool-2 style):
    // buildings first [0..static_building_count), all statics [0..static_count),
    // positions quantized (p + 0x8000) >> 16 as u16, radius padded +111876, cap 1200.
    // [orig: Entity_BuildProximityLists_Pool01 @ 0x4b9340] persons (pool 0) into the
    // full-precision repulsion table; dynamics (pool 1) into the dyn table.
    // [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0] per-entity candidate
    // slices (dyn radius+4.0u / statics; pool-1 radius+6.0u) into a 3000-entry arena.
    void build_tick_tables(World &world);
    // Mission-init pool snapshot for pre-logic consumers such as portal setup.
    // Deliberately does not advance the 17-tick candidate-slice cadence.
    void build_initial_tables(World &world);
    // A spawn or registry rewind keeps an existing pool snapshot coherent. After
    // the first candidate-slice epoch it also replaces slices immediately; before
    // that epoch it preserves retail's initial 16 sliceless logic ticks.
    void refresh_after_registry_change(World &world);

    // Replace the persistent decoded-person collision projection. Sorting by
    // wire handle gives the proxy-only subset deterministic order; no wire
    // handle is ever converted to EntityHandle.
    void replace_projectile_person_proxies(
            std::vector<ProjectilePersonProxy> proxies,
            uint16_t local_player_wire_handle = 0xFFFF);

    // Replace the decoded pool-1 mover projection (authored geometry at the
    // decoded pose). Same ordering/aliasing rules as the person replace.
    void replace_projectile_dynamic_proxies(
            std::vector<ProjectileDynamicProxy> proxies);

    // Segment arbitration shared by authoritative and visual-only projectile
    // loops. The query is read-only: callers must publish/build collision
    // snapshots at the normal tick seam before tracing.
    ProjectileHit trace_projectile(const World &world,
                                   const ProjectileTrace &trace) const;

    // Per-entity blink refresh: test the entity position (one point, radius 0.5u)
    // against nearby buildings' blink volumes; stamp Entity.blink_hits + the
    // indoors flag; accumulate the local player's flags word.
    // [orig: Entity_BuildProximityList @ 0x4b3dc0]
    void refresh_blink(World &world, Entity &ent);

    // Point blink query at an arbitrary position (the camera-side analog of
    // refresh_blink): walk the building prefix with per-axis + euclid broad
    // phase at radius + 0x8000, run the point query (one point, radius 0x8000),
    // return the packed hit set in `accum`. Clears `accum` first.
    // [orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]
    void query_blink_boxes_at_point(World &world, const int32_t pos[3], BlinkAccum &accum);

    // Projectile face raycast against ONE entity's collision instance (the
    // husk-aware target view). kNoFaceMesh = no instance or the model carries
    // no face mesh — the caller's bound-sphere stand-in applies (the D-ITEM-1
    // bounded fallback); kMiss = a face mesh exists and the segment misses it
    // (the round flies on); kHit fills `out`. [orig: each pool-walk candidate
    // runs Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]
    enum class FaceRaycast { kNoFaceMesh, kMiss, kHit };
    FaceRaycast raycast_entity_faces(World &world, EntityHandle h, const int32_t start[3],
                                     const int32_t end[3], uint32_t ammo_flags,
                                     RayFaceHit &out);

    // Retail organic/person narrow phase over the entity's posed COBJ spheres.
    // A set section-mask bit removes that bone from collision.
    bool raycast_person_sections(World &world, EntityHandle h, const int32_t start[3],
                                 const int32_t end[3], int32_t extra_radius,
                                 PersonSectionHit &out);

    // Entity-only radiused segment test over the static collision prefix:
    // TRUE = some static's type-1 solid clips the segment at `radius`
    // (negative radius reads the planes thinner). The iris sun-occlusion ray
    // primitive — the caller passes allowAllTypes = 1, so no building-kind
    // gate. [orig: raycast_find_collision_entity @ 0x539a70 (the iris caller
    // @ 0x5c7784 pushes allowAllTypes 1) -> raycast_against_entity_pool
    // @ 0x538720; pool-1 dynamics are a tracked D-RLIT-2 residual.]
    bool segment_hits_static(World &world, const int32_t a[3], const int32_t b[3],
                             int32_t radius);

    // Ground-column probe through terrain + the entity's candidate models.
    // Builds the ray {x+dx, y+dy, z+z_up} down z_drop, clamps to the terrain
    // column, clips against candidate solids; returns the resolved ground Z and
    // (optionally) the hit entity. [orig: Entity_RaycastGroundHeight @ 0x4142c0 /
    // Entity_RaycastGroundHeightAndObject @ 0x414320 -> raycast_entity_collision
    // @ 0x413760]
    int32_t raycast_ground(World &world, EntityHandle source, const int32_t pos[3],
                           int32_t dx, int32_t dy, int32_t z_up, int32_t z_drop,
                           EntityHandle *out_hit_entity);

    // Segment LOS query, TRUE = CLEAR of terrain + sector solids — the AI mutual-LOS
    // seam. [orig: Physics_RaycastTerrainAndSectors @ 0x539910: terrain leg via
    // Terrain_RaycastHeightmapHiRes @ 0x60c760 (skipped when BOTH excluded entities
    // carry Flags & 0x800000 INDOORS — the heightmap has no interiors), then the
    // sector walk (raycast_against_entity_pool @ 0x538720) over pool 2 statics, then
    // pool 1 dynamics, excluding both entities; LOS callers pass ray radius 0.]
    // Tracked D-AI-7 residuals: the +0x28 owner-link exclusion, the Flags&4
    // destroyed-husk model swap, and the itemDef type-3 person sphere-block
    // (same-team within 3.0 u exempt) — person-kind residents of the walked pools
    // don't exist in our world yet (organics are pool 0, unwalked, like retail).
    bool raycast_clear(World &world, const int32_t a[3], const int32_t b[3],
                       EntityHandle exclude_a, EntityHandle exclude_b);

    // Sound-occlusion distance inflation [orig: Sound_ApplyOcclusionDistance
    // @ 0x529970]: base = min(d/8, 10u); two LOS rays listener -> source
    // (source z lifted +0x2000 for both; ray 2's segment raised 0.5u); a
    // blocked ray 1 compounds base = 2*base + 5u; the single final add is
    // base (ray 2 clear) or 2*base + 5u (ray 2 blocked). LOS per ray =
    // terrain leg [orig: Physics_CheckTerrainLineOfSight @ 0x53b080 —
    // skipped as clear when both entities are indoors; either-entity-null
    // paths precheck both endpoints above the bilinear surface, an
    // under-surface endpoint reading as clear] then entity leg [orig:
    // Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130 ->
    // raycast_find_collision_entity @ 0x539a70 with allowAllTypes = 0 —
    // only building-kind candidates from the LISTENER's slice block, via the
    // type-1 solid clip]. `source` may be invalid when the host has no emitter
    // identity; identified ambient/fire sources preserve exclusion and the
    // both-indoors terrain bypass. listener_pos is the AUDIO listener [orig:
    // listener_pos @ 0x24D6630], not the entity position.
    // Returns the inflated effective distance (16.16). Ray 2's -0x8000
    // height offset doubles as the entity-leg clip radius (the witnessed
    // arg-slot reuse): its planes read 0.5u thinner, which is what lets the
    // second ray clear walls the first grazes — ported; the per-plane
    // flag-byte branch (flagged planes clamp the radius at 0) is the D-SND-9
    // residue (docs/audio/lwf-dbf-sound-re.md).
    int32_t sound_occlusion_inflate(World &world, EntityHandle listener,
                                    EntityHandle source,
                                    const int32_t listener_pos[3],
                                    const int32_t source_pos[3],
                                    int32_t distance_q16);

    // The movement resolver: candidate contact forces + damage/flag dispatch +
    // repulsion + the ground-settle tail. Returns the foot clearance (feet Z -
    // resolved ground Z): <= 0 grounded (caller lifts by the return), > 0xF000
    // airborne. [orig: movement collision resolver @ 0x4b2bd0]
    // capsule_bottom/top are the anim frame's 16.16 capsule extents (out[3]/out[4]).
    struct ResolveState {
        int32_t prev_pos[3] = {};   // savedLivePose stand-in (updated per resolve)
        bool prev_valid = false;
        uint8_t skip_counter = 0;   // [orig: entity pad_370[3] idle throttle]
    };
    // anim_state_flags = the state's g_animStateFlagsTable word (bit 0 forces a
    // full update; the id itself picks the repulsion-exempt states).
    int32_t resolve_entity(World &world, EntityHandle source, ResolveState &state,
                           int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                           int32_t capsule_bottom, int32_t capsule_top,
                           int32_t heading, int32_t body_pitch, bool is_player,
                           bool is_authority, uint32_t tick, int32_t anim_state_id,
                           uint32_t anim_state_flags, int16_t &health);

    // Hull-vs-world contact for the vehicle motor [orig: Entity_CheckCollisionState
    // @ 0x462a30, called per tick from the vehicle physics @ 0x47cb8c/0x47d213 —
    // walks the source's proximity candidates and runs the contact-force query
    // (Entity_ComputeBoneCollisionForce @ 0x4ae150 = collision_contact_force) per
    // wheel point; a horizontal-dominant push (|fz|<<22/|f| under the slope
    // thresholds) applies in FULL at severity 3, vertical-dominant contacts take
    // the graded bands]. Our wheel-less stand-in queries ONE hull-center point
    // (radius 1.5 u, +0.5 u lift — the wheel array + per-wheel radii + the
    // v84/v85 slope-threshold grading ride the unported wheel solver, D-NET-161)
    // and keeps only the wall-like full-force class: vertical-dominant force is
    // dropped (the motor's terrain column owns the vertical). Returns severity
    // (0 or 3) and the XY push in out_force (16.16).
    int32_t resolve_vehicle_hull(World &world, EntityHandle source, const int32_t pos[3],
                                 const int32_t prev_pos[3], int32_t out_force[2]);

    // World-level blink state for the local player.
    // [orig: g_LocalPlayerBlinkFlags @ 0x24C1934]
    uint32_t local_player_blink_flags = 0;
    EntityHandle local_player;

    // Read-only capture of the local player's last FULL resolve (skip-throttled
    // ticks keep the previous capture): the capsule test points/radii the
    // resolver actually queried, the anim-frame capsule extents, and the
    // returned foot clearance. Debug-view seam only — never consumed by the
    // resolver itself. All values 16.16 mission space.
    struct LocalResolveDebug {
        bool valid = false;
        int32_t pos[3] = {};        // resolved position (post push-out)
        int32_t points[3][3] = {};  // the 3 capsule test points (pre pass-shift)
        int32_t radii[3] = {};
        int32_t capsule_bottom = 0; // anim-frame extents (resolve_entity inputs)
        int32_t capsule_top = 0;
        int32_t foot_clearance = 0; // resolve_entity's return (feet Z - ground Z)
    };
    LocalResolveDebug local_resolve_debug;

    // Host-wired terrain (shared with the AI system's field).
    const terrain::TerrainHeightField *terrain = nullptr;

    // --- introspection for tests/host ---
    int32_t static_count() const { return static_count_; }
    int32_t static_building_count() const { return static_building_count_; }
    int32_t candidate_count(EntityHandle h) const;
    // The entity's proximity slice [orig: entity+444/448 — the g_ProxCandidateArena
    // window Entity_BuildProximityListsFromPools fills @ 0x4b8eb0]. Pointer into
    // the arena, valid until the next 17th-tick rebuild; null when the entity has
    // no slice (retail's BSS-zero start: the first 16 ticks scan nothing).
    const EntityHandle *candidate_slice(EntityHandle h, int32_t &count_out) const;
    // True once the per-tick pool tables have been built — the discriminator
    // between "no candidates near" and "this world never ran the table build"
    // (headless callers), which keep whole-registry fallbacks.
    bool tick_tables_ready() const { return tick_tables_built_; }
    // Attach scans switch from their compatibility registry walk only after a
    // real candidate-slice epoch exists. Initial table snapshots deliberately
    // precede the retail 17-tick slice cadence, so treating those as an empty
    // authoritative slice would make nearby seats temporarily disappear.
    bool attach_candidate_slices_authoritative() const {
        return candidate_slices_built_;
    }

    // Static prox-table slot view (quantized u16 coords/radius like the retail
    // tables) — the render-occlusion engine walks the building prefix through
    // this. [orig: g_StaticProx{X,Y,Z,Radius,Entity} @ 0xB55558/0xB54BF8/
    // 0xB54298/0xB55EB8/0xB52FD8]
    struct StaticSlotView {
        uint16_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    StaticSlotView static_slot(int32_t i) const;
    // The collision model attached to this exact live registry identity
    // (nullptr for an absent or recycled packed slot).
    const CollisionModel *model_for(const World &world, EntityHandle h) const;
    // Handle-only compatibility lookup for callers without a World identity
    // source. Prefer the overload above when registry slot reuse matters.
    const CollisionModel *model_for(EntityHandle h) const;

    // Read-only world-space geometry snapshot for a host collision debug view.
    // Each instance's volumes are transformed through the SAME target_view /
    // collision_matrix_from_heading path every query uses, so what the host
    // draws is exactly what the resolver tests. Corner order: index bit 0 = max
    // x, bit 1 = max y, bit 2 = max z (mission space, 16.16). `range` > 0 keeps
    // only instances whose entity position is within it (per-axis box) of
    // `anchor`; `max_instances` caps the sweep either way.
    struct DebugVolume {
        int32_t type = 0;
        uint32_t flags = 0;
        int32_t min[3] = {}, max[3] = {}; // section-local AABB
        int32_t corners[8][3] = {};       // world-space transformed corners
    };
    struct DebugInstance {
        EntityHandle handle;
        int32_t pos[3] = {};
        int32_t heading_bam = 0; // the exact heading the world matrix was built from
        std::vector<DebugVolume> volumes;
    };
    std::vector<DebugInstance> debug_instances(World &world, const int32_t anchor[3],
                                               int32_t range, int32_t max_instances) const;

    // The round hit-detection reality for a host hitbox view: per entity, the
    // CFAC bullet-mesh triangles in WORLD space, transformed through the SAME
    // husk-aware target_view + full-euler placement path the projectile
    // raycast walks (what is drawn IS what rounds test), each face carrying
    // its material byte + flags; plus the broad-phase bound sphere and
    // whether the entity has a face mesh at all (none = the bound-sphere
    // stand-in decides hits, D-ITEM-1). `max_faces` is a total triangle
    // budget; face_total still reports each entity's authored count so a
    // truncated draw is visible as such.
    struct DebugHitboxFace {
        int32_t v[3][3] = {}; // world-space triangle corners (mission 16.16)
        uint8_t material = 0;
        uint32_t flags = 0;
    };
    struct DebugHitboxEntity {
        EntityHandle handle;
        int32_t pos[3] = {};
        int32_t bound_radius = 0; // 16.16 (the round broad-phase sphere)
        bool husk = false;        // the shell is the husk-swapped model
        bool has_faces = false;   // false = sphere stand-in resolves hits
        int32_t face_total = 0;   // authored faces (before the budget cap)
        std::vector<DebugHitboxFace> faces;
    };
    std::vector<DebugHitboxEntity> debug_hitboxes(World &world, const int32_t anchor[3],
                                                  int32_t range, int32_t max_entities,
                                                  int32_t max_faces) const;

    // The organic/person narrow-phase reality for F3: every authored COBJ
    // sphere after the SAME posed target_view matrix used by
    // raycast_person_sections. Radius is the retail effective projectile-zero
    // radius (+0xCCC padding and per-bone scale/cap), not the authored radius.
    struct DebugPersonSection {
        EntityHandle handle;
        int32_t section = -1;
        int32_t center[3] = {};
        int32_t radius = 0;
        int32_t authored_radius = 0;
        bool masked = false;
    };
    std::vector<DebugPersonSection> debug_person_sections(
            World &world, const int32_t anchor[3], int32_t range,
            int32_t max_entities);

private:
    void build_tables(World &world, bool advance_candidate_slices);
    // Contact-flag side effects shared by both resolver passes (DH/DM/DL damage +
    // the CA/CM entity flags). [orig: the dispatch @ 0x4b30b7-0x4b351e]
    void apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health, bool is_authority);

    struct Instance {
        int32_t model_id = -1;
        // The husk-stage collision model — substituted for every query once the
        // entity carries the destroyed flag (Flags & 4): rays, contacts, and
        // ground probes collide with the wreck, not the intact model. -1 = the
        // def authors no husk (the intact model keeps serving, the witnessed
        // fallback). [orig: the +52 huskModel substitution in the pool walk
        // raycast_against_entity_pool @ 0x538720 and the ray/contact picks
        // @ 0x413086 / @ 0x4ae233; D-AI-7 residual closed §24]
        int32_t husk_model_id = -1;
        // Entity::registry_spawn_id at assignment. Zero preserves the legacy
        // handle-only behavior for portable callers/tests that do not stamp it.
        uint64_t registry_spawn_id = 0;
        std::vector<CollisionMatrix> section_matrices;
    };
    const Instance *live_instance(const World &world, EntityHandle h) const;
    struct StaticSlot { // [orig: g_StaticProx* u16 tables + entity ptr array]
        uint16_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct DynSlot {    // [orig: g_DynProx* dword tables]
        int32_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct PersonSlot { // [orig: g_PersonProx* dword tables]
        int32_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct CandidateSlice {
        int32_t start = 0;
        int32_t count = 0;
    };

    const CollisionTargetView *target_view(const World &world, EntityHandle h,
                                           CollisionTargetView &scratch,
                                           std::vector<CollisionMatrix> &mat_scratch) const;
    void invalidate_trace_view(EntityHandle h);
    void invalidate_trace_views();
    // Per-logic-tick cache of projectile target views. Sustained automatic
    // fire re-traced the same structures per ROUND per tick, and every
    // broad-phase survivor rebuilt its per-section matrix vector (heap
    // allocation included) — ~15 ms/tick with ~90 rounds in flight at a
    // firing range. Retail keeps section matrices RESIDENT on the entity
    // [orig: the entity matrix array consumed by
    // Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]; this cache is the
    // host-side equivalent scoped to one tick. A cache hit revalidates the
    // husk bit because retail's model pick runs per query [orig: Flags & 4
    // pick @ 0x413086] and a round earlier in the SAME tick can husk the
    // target.
    struct TraceViewCacheEntry {
        CollisionTargetView view;
        std::vector<CollisionMatrix> matrices;
        bool valid = false;
        bool husk_bit = false;
        uint32_t piece_mask = 0;
        uint64_t registry_spawn_id = 0;
        int32_t effective_model_id = -1;
    };
    // Cleared by every table epoch and collision model/instance/pose mutation,
    // so entries never outlive either their tick or their source identity.
    mutable std::unordered_map<uint16_t, TraceViewCacheEntry> trace_view_cache_;
    const CollisionTargetView *trace_target_view(const World &world, EntityHandle h) const;

    bool trace_profile_enabled_ = false;
    mutable TraceProfile trace_profile_;
    // target_view's model selection without the section-matrix build: fills the
    // entity position and bound radius for the witnessed gate-before-view order.
    // False exactly when target_view would return null.
    bool target_bound(const World &world, EntityHandle h, int32_t pos_out[3],
                      int32_t &radius_out) const;

    // One sound-occlusion LOS ray (terrain + building legs); true = clear.
    // [orig: Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130]
    bool sound_los_clear(World &world, EntityHandle listener, EntityHandle source,
                         const int32_t start[3], const int32_t end[3],
                         int32_t height_offset);

    std::vector<CollisionModel> models_;
    std::unordered_map<uint16_t, Instance> instances_; // key: EntityHandle.packed
    ICollisionSectionMatrixProvider *section_matrix_provider_ = nullptr; // non-owning host seam

    std::vector<StaticSlot> statics_;   // cap 1199 counted [orig: g_StaticProx*]
    int32_t static_count_ = 0;
    int32_t static_building_count_ = 0;
    // Build state is independent of table contents: an empty-but-built table is
    // still authoritative and must not fall back to whole-registry scans.
    bool tick_tables_built_ = false;
    bool candidate_slices_built_ = false;
    // Candidate slices rebuild only every 17th tick — the counter increments per
    // tick and the rebuild fires (and resets it) once it reaches 16; pool tables
    // rebuild every tick. BSS-zero start: retail's first slice build lands on
    // tick 17 (mission starts run 16 sliceless ticks). [orig: dword_B57C84 vs
    // 0x10 @ 0x4c240f, zeroed by Entity_BuildProximityListsFromPools @ 0x4b8ed0]
    uint32_t slice_refresh_counter_ = 0;

    std::vector<DynSlot> dynamics_;     // [orig: g_DynProx*]
    std::vector<PersonSlot> persons_;   // [orig: g_PersonProx*]
    std::vector<ProjectilePersonProxy> projectile_person_proxies_;
    std::vector<ProjectileDynamicProxy> projectile_dynamic_proxies_;
    // Server H used solely as local L's ordering key during proxy-enabled
    // person walks. Geometry and ignore logic continue to use L.
    uint16_t projectile_local_player_wire_handle_ = 0xFFFF;
    std::vector<EntityHandle> arena_;   // cap 3000 [orig: g_ProxCandidateArena]
    std::unordered_map<uint16_t, CandidateSlice> candidates_;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_COLLISION_H
