// World-object collision: the runtime queries the original engine runs against a
// model's collision block (the .3di CDTA volumes/planes), plus the per-tick
// proximity tables that feed them. Witness record: docs/world/world-wac-ai-re.md
// §15 (collision + blink boxes), grilled 2026-07-09 against Jointops.exe.
//
// Runtime data (the query-side layout, witnessed in the three query functions):
//   COBJ section records — volume list + local AABB + bound sphere
//   BVOL volume records  — collidable type + local AABB + plane run + flags
//   BPLN plane records   — int16 Q14 normal + 16.16 distance
// The embedder builds these PODs from the parsed .3di collision IR (the same
// leaf-consumer seam as terrain_query's TerrainHeightField, ADR 0020): engine/runtime/world
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
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <runtime/devtools/tick_profile.h>
#include <runtime/world/entity.h>

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
struct CollisionSection {
    uint32_t flags = 0; // COBJ+0, bit 1 selects blast breakage [orig: @0x4E6CD0]
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
// The entity placement matrix retail rebuilds at entity+0xB4 before it
// transforms a userpoint or the bbox center: the euler rotation from the live
// heading (the vehicle motor's BAM mirror when seeded, else the whole-degree
// mission yaw), pitch and roll about the 16.16 position, the uniform item
// scale riding the rotation diagonal [orig: the builder select
// @0x43b56c..0x43b5bd; Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale
// @0x614210; Math_BuildFixedPointMatrixFromEulerAngles @0x613f40].
CollisionMatrix entity_placement_matrix(const Entity &e);
// That matrix's live Euler triple (BAM32 heading, pitch, roll), shared with
// the render-slot march start (renderer::slot_march_start_offset), which
// builds the same rotation without the scale.
void entity_live_euler_bam(const Entity &e, int32_t out[3]);

bool collision_matrix_apply_render_pose(const CollisionMatrix &entity_world,
                                        const float pose_row_major[16],
                                        CollisionMatrix &out);

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
inline constexpr uint32_t kBlinkSkyOffBit = 0x4;   // authored sky letter — the main frame's
                                                   // dome + sun/moon bracket skipped
                                                   // [orig: Render_ProcessMainSceneFrame
                                                   // @0x5ca1a3..0x5ca1ab -> @0x5ca7c4]
inline constexpr uint32_t kBlinkWaterOffBit = 0x8; // authored water letter — both water passes
                                                   // skipped [orig: Terrain_RenderWorldScene
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

// Collision-face flag bits [orig: face tests @0x4e5073-family]; the F3 hitbox
// report (godot/src/simulation/hitbox_debug_report.h) carries them per face.
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
    // Door map affects only non-player contact, never projectile/LOS rays.
    // [orig: Entity_ComputeBoneCollisionForce @0x4AE289..0x4AE335]
    uint64_t door_passable_sections = 0;
    int32_t door_first_bone = 0;
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

    // Build mid/half/dir from start/end. [orig: prologue of Entity_RaycastCollision
    // @ 0x413760 / Entity_FindNearestByRay @ 0x413af0 (float normalize, ftol)]
    void refresh();
    // Recompute mid/half only — dir is built ONCE at ray construction and never
    // re-derived from a clipped segment. [orig: the hit tail @ 0x41370c-0x41374e
    // refreshes point[6..11] and leaves dir untouched]
    void refresh_bounds();
};

// Metadata for the solid volume that most recently clipped `ray` to its nearest
// entry point. Callers that only need the bool leave this null; the
// projectile query retains section identity for downstream impact policy.
struct CollisionModelHit {
    int32_t section_index = -1;
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
    // supplies the authored ladder facing; the frame feeds the resolver's climb
    // alignment/entry legs and the motors' dismount/exit pushes.]
    int32_t anchor[3] = {};
    int32_t yaw = 0;
    int32_t pitch = 0;
    bool valid = false;
};

// The climb-motor channels the resolver's CL/type-4 legs read and write
// (the D-COL-5 port). Motor callers pass their InfantryState-backed fields;
// the replica seam and harness callers pass nullptr and keep the latch-only
// behavior (a remote row's climb pose is owned by its authority).
struct LadderResolveIO {
    // Position Z captured at the motor tick's head — the fresh-entry gate
    // measures the anchor against the tick-start pose, not the integrated one.
    // [orig: entity+0x88 savedLivePose.Z, captured @ 0x4b4190-0x4b419c; read
    //  @ 0x4b327d]
    int32_t tick_start_z = 0;
    bool prone = false;  // MoveOrder 0x100 [orig: entry-bump pick @ 0x4b3330]
    bool crouch = false; // MoveOrder 0x200 [orig: @ 0x4b3340]
    // The AI climb qualifier — the third fresh-entry arm besides previous
    // contact and the player class bit. make_ladder_resolve_io feeds it from
    // the slot's CLIMBER bit (ChangeAI sub 17); the AI move-order half of the
    // retail test is unported. [orig: var_60 @ 0x4b3257 || Flags & 0x100
    // @ 0x4b325f || aiRuntime+4 & 0x400 @ 0x4b326a]
    bool ai_wants_climb = false;
    bool is_local_player = false;
    // View + body pose channels (BAM32). view_yaw/view_pitch are entity
    // +0x10/+0x14; for the local player the embedder's mouse accumulator must
    // inherit any resolver write-back (retail drags g_LocalPlayerLookYaw / dword_B7900C
    // alongside the entity fields).
    int32_t *view_yaw = nullptr;
    int32_t *view_pitch = nullptr;
    int32_t *body_heading = nullptr; // entity+0x8C
    int32_t *body_pitch = nullptr;   // entity+0x90
    // The AI hard-set channels — the (Flags & 0x100)==0 leg only; players keep
    // these null (retail's +0x1A8 slot is the player jump cooldown reuse).
    // [orig: @ 0x4b33da-0x4b33fa]
    int32_t *ai_target_heading = nullptr;
    int32_t *ai_aim_heading = nullptr;
    // The local-player post-ladder pitch restore: the exit leg arms it, the
    // on-ladder chase disarms it, the per-resolve chase eases the view pitch
    // back. [orig: +0x2C bit 4 latch; the aimPitch slot reuse; dword_B7900C]
    bool *pitch_restore_active = nullptr;
    int32_t *pitch_restore_target = nullptr;
    int32_t *pitch_restore_prev = nullptr;
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

// Per-wheel/hull result of the vehicle contact query. Vertical support is
// retained even when the surface is climbable (severity zero).
// [orig: Entity_CheckCollisionState @0x462A30]
struct VehicleProbeForce {
	int32_t terrain_gap = 0; // optional contact output: positive separation from terrain
	int32_t fx = 0, fy = 0, fz = 0;
	bool wall_contact = false;
};

struct ContactResult {
    int32_t force[4] = {};         // world push force (16.16); [3] spare like the orig
    uint32_t flags = 0;            // contact flags (see the type table)
    uint32_t door_sections = 0;    // CD/type-9 bit-per-section mask [orig: target+692]
};

bool collision_contact_force(const CollisionTargetView &target, const ContactQuery &q,
                             BlinkAccum &blink, LadderContact &ladder, ContactResult &out);

class IPoseProvider; // runtime/world/pose_provider.h: the embedder's live pose seam

// ---------------------------------------------------------------------------
// Projectile trace shared by authoritative and explicitly visual-only rounds.
// Fixed-point terrain/entity arbitration lives here; arming, armor, health,
// scoring, and effects remain consequences owned by RoundSim.
// ---------------------------------------------------------------------------


// One items.def + model-header collision initialization result, shared by
// locally materialized entities and decoded wire rows. Values already include
// the retail collision-block gate, effective authored scale, first-husk max,
// and +0x1000 bound pad. Zero scale is the ordinary unscaled sentinel.
// [orig: Entity_InitFromModel @0x40dc30..0x40e078]
struct ResolvedCollisionShape {
    int32_t model_id = -1;
    int32_t bound_radius_q16 = 0;
    int32_t uniform_scale_q16 = 0;
    FixedVec3 bbox_center_q16;
    bool has_collision_block = false;
    // Pool-1 rows receive a +6u proximity-source slice only when the ItemDef
    // survives retail's source gates (EWeap is excluded unless raw type 1).
    // [orig: Entity_BuildProximityListsFromPools — the pool-1 loop head
    // @0x4b9127..0x4b9147 (in-use, ItemDef, Flags bit 0, attrib 0x20 unless
    // type 1); +6.0u @0x4b9152]
    bool pool1_candidate_source_eligible = false;
    // The raw ItemDef+0x5C type (1 vehicle, 3 person): the client-side blink
    // walk's branch key [orig: Entity_BuildProximityList @ 0x4b3e47..0x4b3e5f].
    uint8_t item_type = 0;
    // The render model the collectors' model leg reads when the graphic has
    // no usable collision geometry: its collision-block CMDL sphere, UNSCALED
    // (world::collision_projection_sphere_from_3di in the entity-init form;
    // radius 0 without the block). has_render_model false = the graphic did
    // not load, and the collectors never collect the row. [orig: the
    // entity+0x30 gate @ 0x5c8cf6..0x5c8cff; Entity_ComputeBoundingSphere
    // @ 0x5c69a0, the null-block early out @ 0x5c69be]
    bool has_render_model = false;
    FixedVec3 render_sphere_center_q16;
    int32_t render_sphere_radius_q16 = 0;
};

// One decoded remote pool-0 person (player or non-player infantry) projected
// into client-side collision consumers. ClientWorldMaterializer deliberately
// excludes pool 0, so this proxy must never alias the joiner's separate local
// Entity or participate in movement, AI, explosions, or authoritative damage.
// Projectile traces and per-draw sun visibility share this one typed row.
struct WirePersonCollisionProxy {
    uint16_t wire_handle = 0xFFFF;
    FixedVec3 position_q16;
    int32_t bound_radius_q16 = 0;
    int32_t uniform_scale_q16 = 0;
    FixedVec3 bbox_center_q16;
};

// One decoded pool-1 mover (vehicle, emplacement, runtime item) projected into
// visual projectile collision with its AUTHORED collision geometry at the
// decoded wire pose. Same wire-keyed rule as the person proxy: never an
// Entity, never a registry handle. The pose mirrors what the retail client
// holds on its own pool entity — the live compact updates the coarse heading
// byte (entity+16 high byte) while entity+20/+24 retain the last spawn/dead
// Euler sample [orig: the 0x0D spawn angle landings + the compact fold; the
// collision placement matrix @ 0x613f40 reads those same three fields].
struct WireDynamicCollisionProxy {
    uint16_t wire_handle = 0xFFFF;
    // Explicitly verified local materialization of this same decoded row.
    // Numeric handle equality alone is never treated as identity.
    EntityHandle registry_twin;
    int32_t model_id = -1;          // CollisionWorld model registry id; -1 = unresolved
    FixedVec3 position_q16;
    int32_t heading_bam = 0;        // reconstructed entity+16 (yaw_byte << 24)
    int32_t pitch_bam = 0;          // retained entity+20 spawn/dead sample
    int32_t roll_bam = 0;           // retained entity+24 spawn/dead sample
    int32_t bound_radius_q16 = 0;   // exact entity+0 boundRadius
    int32_t uniform_scale_q16 = 0;  // entity+0x158/itemDef+0x1B8 effective scale
    FixedVec3 bbox_center_q16;       // entity+0x1FC..+0x204 sun/LOS origin offset
    bool candidate_source_eligible = false;
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
    EntityHandle mount_ignore; // explicit ray[18], used by the lndm ground query
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
    // Query-domain switches. The throwable/useownmove motor sweep walks pools
    // 2/1 only, so terrain, water, or a person in front of an entity must not
    // win nearest-hit arbitration and mask that entity [orig: the two
    // Projectile_RaycastProximitySlots(2/1, ...) calls @0x444619..0x444667].
    // Bullets keep the default full walk.
    bool walk_terrain = true;
    bool walk_water = true;
    bool walk_persons = true;
};

struct ProjectileHit {
    int32_t distance_q16 = 0;
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
    // A person hit's victim entity+0 boundRadius (Q16): the person arm parks
    // a surviving round that far past the hit. [orig: Projectile_UpdatePhysics
    // @0x4EA7BE..0x4EA7D5]
    int32_t victim_bound_radius_q16 = 0;
    // A decoded pool-1 wire proxy's verified registry twin, when the hit is
    // that proxy (geometry_entity stays invalid on a proxy hit): the client's
    // own row of the entity the retail client's table walk would name.
    EntityHandle wire_registry_twin;
    // A decoded remote person proxy's wire handle, when the hit is that proxy
    // (geometry_entity stays invalid): the pool-0 row a retail client's own
    // table walk would name. 0xFFFF otherwise.
    uint16_t wire_person_handle = 0xFFFF;

    constexpr bool hit() const { return hit_class != ProjectileHitClass::None; }
};

// The raw ItemDef+0x5C type the statics table splits pool 2 on.
inline constexpr uint8_t kItemDefTypeBuilding = 5;

// The entity's items.def type is Building — the key retail splits pool 2 on.
// Every pool-2 row is a BMS "building" record (EntityKind::Building), but
// only the Building-type defs form the statics table's building prefix
// [0, static_building_count) that the building collector, the blink queries
// and the portal init walk, and only they skip the blink refresh; the other
// def types (decoration, foliage, ...) follow the prefix and are collected
// as ENTITIES — the blink-quad gate, the view cull and latch, then the render
// waves' render_TOC for an empty quad — each with its own blink quad from the
// mission-start refresh. An entity with no resolved def keeps its record
// family's answer (a synthetic world; retail tables no def-less row).
// [orig: Entity_BuildProximityLists_Pool2 @ 0x4b9430 — pass 1 `def->type ==
//  ItemType_Building` @ 0x4b946e, pass 2 `!=` @ 0x4b9502; the prefix reader
//  Terrain_CollectVisibleSectorUserpoints @ 0x5c6b97 and the tail reader
//  Terrain_CollectVisibleEntities_0 @ 0x5c6f48..0x5c721e; the blink refresh's
//  Building arm Entity_BuildProximityList @ 0x4b3e4d, its candidate walk's
//  `childModel->type == ItemType_Building` @ 0x4b3f73, and the mission-start
//  refresh's pool-2 filter @ 0x5240f3]
inline bool building_def_row(const Entity &e) {
    return e.has_item_def ? e.item_type == kItemDefTypeBuilding
                          : e.kind == EntityKind::Building;
}

// ----------------------------------------------------------------------------
// CollisionWorld: the proximity tables + per-entity instances, and the
// world-level blink state. [orig: the g_StaticProx* table is built at mission
// start and on the teleport paths by Entity_BuildAllProximityLists @ 0x4c20f0
// -> Entity_BuildProximityLists_Pool2 @ 0x4b9430; the g_DynProx*/g_PersonProx*
// tables are rebuilt every tick by Entity_BuildProximityLists_Pool01
// @ 0x4b9340 (Entity_UpdateAllEntities @ 0x4c240a); the g_ProxCandidateArena
// slices every 17th tick by Entity_BuildProximityListsFromPools @ 0x4b8eb0
// (gate @ 0x4c2416).]
// ----------------------------------------------------------------------------
class CollisionWorld {
public:
    // Debug-card tap: the last resolve_entity's strongest-contact source (the
    // candidate whose model produced push-out force) — the frozen-clump
    // instrument. Reset at each resolve; single-threaded like the resolver.
    EntityHandle dbg_last_contact{};
    int32_t dbg_last_contact_item = 0;
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
    // Opt-in ray-debug capture: every segment query records one event into a
    // per-category ring while enabled (dev tooling — the F3 ray view/window
    // feed, not a ported surface). Per-category rings keep recurring per-frame
    // categories (replication LOS, sun visibility) from evicting one-shot rays
    // (a bullet, a pick) inside the view's fade window; `total` counts every
    // recorded event so ring overwrite never hides true throughput.
    enum class RayDebugCategory : uint8_t {
        kUncategorized = 0,
        kProjectile,
        kKnife,
        kThrowable,
        kAiLos,
        kReplicationLos,
        kScriptLos,
        kExplosionLos,
        kGroundProbe,
        kCameraIris,
        kRenderOcclusion,
        kSunVisibility,
        kSoundOcclusion,
        kPrecipitation,
        kPick,
        kCount,
    };
    static const char *ray_debug_category_name(RayDebugCategory category);
    // result: 0 = clear/miss (ray ran its full length), 1 = hit with a resolved
    // point in `hit`, 2 = blocked (boolean query, no hit point resolved).
    enum : uint8_t {
        kRayDebugClear = 0,
        kRayDebugHit = 1,
        kRayDebugBlocked = 2,
    };
    struct RayDebugEvent {
        FixedVec3 start;
        FixedVec3 end; // the requested endpoint
        FixedVec3 hit; // the resolved stop/clip point (== end when none)
        uint32_t tick = 0;
        uint8_t category = 0; // RayDebugCategory
        uint8_t result = kRayDebugClear;
        uint16_t reserved = 0;
    };
    struct RayDebugRing {
        std::vector<RayDebugEvent> events; // cap once enabled, empty when off
        int32_t next = 0;
        int32_t count = 0;   // saturates at the cap
        uint64_t total = 0;  // lifetime recorded (the drop-honesty counter)
    };
    static constexpr int32_t kRayDebugCapPerCategory = 256;
    // The presentation filter the ray view/window share: bit i of the mask
    // draws category i; TTL is the fade window in 62 Hz ticks. Recording is
    // never filtered — rings capture everything, the mask gates drawing only.
    static constexpr uint32_t kRayDebugMaskAll = 0x7FFF;
    uint32_t ray_debug_mask() const { return ray_debug_mask_; }
    void set_ray_debug_mask(uint32_t mask) { ray_debug_mask_ = mask & kRayDebugMaskAll; }
    int32_t ray_debug_ttl_ticks() const { return ray_debug_ttl_ticks_; }
    void set_ray_debug_ttl_ticks(int32_t ticks) {
        ray_debug_ttl_ticks_ = ticks < 1 ? 1 : (ticks > 620 ? 620 : ticks);
    }
    bool ray_debug_enabled() const { return ray_debug_enabled_; }
    // Disabled by default; each enable/disable edge clears the rings (disable
    // also frees them — hosts/tests stack-allocate Worlds, keep pools off that
    // footprint) so a new consumer never inherits another capture's events.
    void set_ray_debug_enabled(bool enabled);
    const std::array<RayDebugRing,
                     static_cast<size_t>(RayDebugCategory::kCount)> &
    ray_debug_rings() const { return ray_debug_rings_; }
    // Hot-path recorder: a single cold branch when disabled. The category is
    // the active RayDebugScope override when set, else `fallback`.
    void ray_debug_record(RayDebugCategory fallback, uint32_t tick,
                          const int32_t a[3], const int32_t b[3],
                          const int32_t *hit_or_null, uint8_t result) const;
    // Outermost-wins RAII category tag: a scope takes effect only while no
    // outer scope is active, so a script-LOS caller keeps its tag through the
    // AI LOS leg it routes through. Cheap either way (one member write).
    class RayDebugScope {
    public:
        RayDebugScope(const CollisionWorld *cw, RayDebugCategory category)
                : cw_(cw) {
            if (cw_ != nullptr &&
                cw_->ray_debug_scope_ == RayDebugCategory::kUncategorized &&
                category != RayDebugCategory::kUncategorized) {
                cw_->ray_debug_scope_ = category;
                owns_ = true;
            }
        }
        RayDebugScope(const CollisionWorld &cw, RayDebugCategory category)
                : RayDebugScope(&cw, category) {}
        ~RayDebugScope() {
            if (owns_) cw_->ray_debug_scope_ = RayDebugCategory::kUncategorized;
        }
        RayDebugScope(const RayDebugScope &) = delete;
        RayDebugScope &operator=(const RayDebugScope &) = delete;

    private:
        const CollisionWorld *cw_;
        bool owns_ = false;
    };
    // Opt-in contact/hit capture (dev tooling — the F3 collision view's hit
    // flashes and the Physics window's counts, not a ported surface): resolved
    // per-body outcomes keyed by the touched entity, so the overlay can light
    // the box it already draws. One ring, not per-category — hits are sparse
    // next to rays — with per-kind lifetime totals for drop-honesty. The same
    // cold-branch/enable-edge contract as the ray capture.
    enum class ContactDebugKind : uint8_t {
        kProjectileHit = 0, // trace_projectile resolved on an entity/person
        kKnifeHit,          // trace_knife_impact resolved
        kMoveContact,       // resolve_entity solid push-out (pass 0)
        kVehicleHull,       // resolve_vehicle_probes wall-like push
        kTerrainHit,        // a trace resolved on terrain (marker only)
        kWaterHit,
        kCount,
    };
    static const char *contact_debug_kind_name(ContactDebugKind kind);
    struct ContactDebugEvent {
        FixedVec3 pos;            // world 16.16 hit/contact point
        uint32_t tick = 0;
        uint16_t target = 0xFFFF; // EntityHandle.packed of the flashed body (0xFFFF none)
        uint8_t kind = 0;         // ContactDebugKind
        uint8_t hit_class = 0xFF; // ProjectileHitClass for the trace kinds
    };
    struct ContactDebugRing {
        std::vector<ContactDebugEvent> events; // cap once enabled, empty when off
        int32_t next = 0;
        int32_t count = 0;   // saturates at the cap
        uint64_t total = 0;  // lifetime recorded (the drop-honesty counter)
        uint64_t kind_totals[static_cast<size_t>(ContactDebugKind::kCount)] = {};
    };
    static constexpr int32_t kContactDebugCap = 256;
    // The presentation filter (bit i of the mask flashes kind i) and the flash
    // window; recording is never filtered — the ring captures everything.
    static constexpr uint32_t kContactDebugMaskAll = 0x3F;
    static constexpr int32_t kContactDebugTtlTicks = 62; // ~1 s flash window
    uint32_t contact_debug_mask() const { return contact_debug_mask_; }
    void set_contact_debug_mask(uint32_t mask) {
        contact_debug_mask_ = mask & kContactDebugMaskAll;
    }
    bool contact_debug_enabled() const { return contact_debug_enabled_; }
    // Disabled by default; each enable/disable edge clears the ring (disable
    // also frees it — the ray-capture footprint rationale).
    void set_contact_debug_enabled(bool enabled);
    const ContactDebugRing &contact_debug_ring() const { return contact_debug_ring_; }
    // Hot-path recorder: a single cold branch when disabled.
    void contact_debug_record(ContactDebugKind kind, uint32_t tick, EntityHandle target,
                              const int32_t pos[3], uint8_t hit_class) const;
    const TraceProfile &trace_profile() const { return trace_profile_; }
    bool trace_profile_enabled() const { return trace_profile_enabled_; }
    // Profiling is disabled by default. Each enable/disable edge clears the
    // snapshot so a new consumer never inherits another capture's counters.
    void set_trace_profile_enabled(bool enabled);
    // --- model registry (embedder-fed, keyed by an opaque graphic id) ---
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
    // Install the embedder's pose seam that supplies final per-section
    // matrices. Null restores the static shared-entity-matrix fallback.
    void set_pose_provider(IPoseProvider *provider);
    // Resolve an embedder-owned late-spawn instance on demand. Existing instances
    // never call the provider, so repeated round/F3 queries are idempotent.
    bool ensure_entity_instance(World &world, EntityHandle h);
    // Publish the pose owner's current world-space Q22 section matrices. Retail
    // collision consumes the model callback's live matrices rather than deriving
    // animation itself. Count must exactly match the assigned model's COBJ count.
    bool publish_entity_section_matrices(EntityHandle h,
                                         std::vector<CollisionMatrix> matrices);
    void clear_entity_section_matrices(EntityHandle h);
    bool has_instance(const World &world, EntityHandle h) const;
    // The entity's current world-space section matrix (the same slot array the
    // traces consume: published pose, else the host callback, else the placement
    // matrix). False when the entity has no collision instance or the section
    // is out of range. The scar writer transposes it to store a bone-local slot
    // [orig: Scar_AddEntry @0x5ccc99..0x5ccca5].
    bool entity_section_matrix(const World &world, EntityHandle h, int section,
                               CollisionMatrix &out) const;
    bool has_instance(EntityHandle h) const;
    // The attached collision model id for a live entity (-1 = no instance);
    // the userpoint leg of the muzzle-pose provider keys its parsed model by it.
    int32_t entity_model_id(EntityHandle h) const;
    // The husk-stage model id attached beside it (-1 = none or no instance).
    int32_t entity_husk_model_id(EntityHandle h) const;
    size_t instance_count() const { return instances_.size(); }

    // --- the per-tick snapshot ---
    // [orig: Entity_BuildProximityLists_Pool01 @ 0x4b9340] persons (pool 0) into the
    // full-precision repulsion table; dynamics (pool 1) into the dyn table, every
    // tick. The statics table (pool 2, [orig: Entity_BuildProximityLists_Pool2
    // @ 0x4b9430]: buildings first [0..static_building_count), all statics
    // [0..static_count), positions quantized (p + 0x8000) >> 16 as u16, radius
    // padded +111876, cap 1200) is rebuilt only when a pool-2 instance or the
    // registry changed — retail builds it at mission start and on teleport
    // (Entity_BuildAllProximityLists @ 0x4c20f0), never per tick; the instance
    // and registry-lifetime edges below stand in for those paths.
    // [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0] per-entity candidate
    // slices (dyn radius+4.0u / statics; pool-1 radius+6.0u) into a 3000-entry arena.
    void build_tick_tables(World &world);
    // Mission-init pool snapshot and candidate slices for pre-logic consumers
    // such as portal setup and first-tick grounding. The 17-tick cadence only
    // governs steady-state rebuilds.
    void build_initial_tables(World &world);
    // A spawn or registry rewind immediately republishes the existing pool
    // snapshot and its candidate slices.
    void refresh_after_registry_change(World &world);
    // Direct candidate rebuild plus this entity's blink refresh. Pool position
    // snapshots stay at their published epoch; unrelated contacts are retained.
    // [orig: WacCmd_Tele @0x4F22D0 -> 0x4B8EB0, 0x4B3DC0]
    void refresh_entity_proximity(World &world, Entity &entity);

    // Atomically replace the persistent decoded collision projection. Sorting
    // by wire handle gives both domains deterministic order; the wire-keyed
    // candidate arena is separate from candidates_, so a numerically equal
    // packed wire and registry handle can never alias. There is deliberately
    // no projectile-only compatibility mutator: projectile traces and replica
    // sun lighting consume this same snapshot.
    void replace_wire_collision_proxies(
            std::vector<WirePersonCollisionProxy> persons,
            std::vector<WireDynamicCollisionProxy> dynamics,
            uint16_t local_player_wire_handle = 0xFFFF);
    // The decoded person's entity+0 boundRadius as its current proxy carries
    // it (0 when no proxy names the handle): a client's remote person is a
    // replica row, never a registry entity.
    int32_t wire_person_bound_radius_q16(uint16_t wire_handle) const;

    // Segment arbitration shared by authoritative and visual-only projectile
    // loops. The query is read-only: callers must publish/build collision
    // snapshots at the normal tick seam before tracing.
    // `out_ground` takes the entity whose geometry clamped the height (the
    // query's own out word; a client's wire-proxied mover names its registry
    // twin), kInvalid when none did, even where the terrain floor then wins.
    // [orig: Entity_ComputeClampedDisplacement @0x4ad6a0, the out store
    // @0x4ad80e..0x4ad810]
    int32_t minefield_ground(const World &world, EntityHandle source,
                             FixedVec3 position, bool indoors,
                             EntityHandle *out_ground = nullptr) const;

    ProjectileHit trace_projectile(const World &world,
                                   const ProjectileTrace &trace) const;

    // The instant Knife presenter walks terrain, water, then the PERSON prox
    // table (replacing on a distance tie), then buildings and items (strict),
    // and every entity pool uses authored CFAC geometry. In particular, pool-0
    // persons do not use the ballistic bone-sphere path and unresolved models
    // do not gain a sphere substitute. The hit's `surface_type` is the CFAC
    // material; the caller maps it to the effect row (person material 1 =
    // flesh). [orig: Weapon_RaycastAndSpawnImpact @0x4e8460 ->
    // Projectile_RaycastProximitySlots @0x4e5340 slot types 0/2/1]
    // [orig: Physics_RaycastEntityPoolsAndUpdate @0x539580]
    ProjectileHit trace_aim(const World &world, const ProjectileTrace &trace) const;
    // Squib uses the general TYPE-1 solid / whole-person sphere query.
    // [orig: Entity_ProcessProjectileTravel @0x448D50 -> sub_539530 @0x539530]
    ProjectileHit trace_squib(World &world, EntityHandle source,
            const FixedVec3 &start, const FixedVec3 &end);

    ProjectileHit trace_knife_impact(const World &world,
                                     const ProjectileTrace &trace) const;

    // Per-entity blink refresh: test the entity position (one point, radius 0.5u)
    // against nearby buildings' blink volumes; stamp Entity.blink_hits + the
    // indoors flag; accumulate the local player's flags word.
    // [orig: Entity_BuildProximityList @ 0x4b3dc0]
    void refresh_blink(World &world, Entity &ent);
    // The mission-start blink stamp over the static table just built: every
    // pool-1 row, then every pool-2 row that is not a building, takes one
    // refresh_blink before the portal init.
    // [orig: Entity_BuildProximityListsForPools12 @ 0x5240a0 — the pool-1 loop
    //  @ 0x5240b8..0x5240cd, the pool-2 loop with its def-type-5 skip
    //  @ 0x5240e4..0x524102; called from Game_StartMission @ 0x525898, after
    //  Entity_InitAllFromModels built the static table]
    void refresh_mission_start_blink(World &world);

    // Point blink query at an arbitrary position (the camera-side analog of
    // refresh_blink): walk the building prefix with per-axis + euclid broad
    // phase at radius + 0x8000, run the point query (one point, radius 0x8000),
    // return the packed hit set in `accum`. Clears `accum` first.
    // [orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]
    void query_blink_boxes_at_point(World &world, const int32_t pos[3], BlinkAccum &accum);

    // Iris/exposure point query: clear `accum`, then test only building-kind
    // entries in `source`'s fixed proximity-candidate slice. This is a
    // different retail function from query_blink_boxes_at_point: the latter
    // walks the global building prefix for camera/occlusion callers, while
    // Terrain_SectorComputeLighting reuses the local player's
    // entity+0x1BC/+0x1C0 slice for all three marched samples.
    // [orig: Terrain_SectorComputeLighting @ 0x5c7550, caller passes
    // g_LocalPlayerEntity from Environment_ApplyFogAndAmbient @ 0x57e51d]
    void query_candidate_blink_boxes_at_point(World &world, EntityHandle source,
                                              const int32_t pos[3], BlinkAccum &accum);

    // The wire-identity twin of refresh_blink for a decoded source without a
    // registry entity: the retail client runs Entity_BuildProximityList on its
    // own copy of the entity. A def type 1/3 source (`candidate_walk`) tests
    // the building-kind entries of its candidate slice — the source's
    // wire-keyed slice here, the one wire_sun_visibility_blocked_rays walks;
    // no slice, no hit [orig: @ 0x4b3e53..0x4b3e5f -> @ 0x4b3f3e..0x4b3f93].
    // Every other type walks the static building prefix [orig: @ 0x4b3e65..
    // 0x4b3f39]. Clears `accum` first.
    void query_wire_blink_boxes_at_point(World &world, uint16_t wire_handle,
                                         const int32_t pos[3], bool candidate_walk,
                                         BlinkAccum &accum);

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

    // Entity-only radiused segment test over `source`'s candidate slice:
    // TRUE = an eligible pool-1 dynamic or pool-2 static type-1 solid clips
    // the segment at `radius` (negative radius reads the planes thinner). The
    // iris/entity-sun callers use allowAllTypes=1, so there is no building-kind
    // gate. [orig: Physics_RaycastFindCollisionEntity @ 0x539a70 ->
    // Physics_RaycastAgainstEntityPool @ 0x538720]
    bool candidate_segment_hits_solid(World &world, EntityHandle source,
                                      const int32_t a[3], const int32_t b[3],
                                      int32_t radius);

    // The three sun-occlusion clip radii, most permissive first — the same
    // segment recast with progressively thinner plane reads; each blocked cast
    // steps the light quality down one. Shared by the camera iris march and the
    // per-entity sun-visibility factor. [orig: the -0x2000/-0x5000/-0x8000
    // pushes @ 0x5c687c/0x5c68a7/0x5c68c8 (Entity_ComputeSunVisibility) and
    // @ 0x5c7767/0x5c7792/0x5c77ac (the iris march).]
    static constexpr int32_t kSunOcclusionClipRadii[3] = {-0x2000, -0x5000,
                                                          -0x8000};

    // Per-entity sun-visibility blocked-ray count for the drawn-entity lighting
    // factor: one segment from the entity position + raw collision-bbox
    // midpoint, 200 u along the active light direction, recast at the three
    // clip radii — against every eligible entry in the ENTITY'S OWN candidate
    // slice (the +0x1BC arena block the ray walker iterates; only solids whose
    // inflated sphere overlaps the entity's bubble can block its sun). An
    // entity with no slice — statics, ineligible pool-1 rows, or the 16
    // sliceless mission-start ticks — returns 0, matching retail's +0x1C0 == 0 skip
    // (quality stays 4, factor 1.0). The caller maps the count through
    // renderer::sun_visibility_factor ((4 - blocked) * 0.25).
    // [orig: Entity_ComputeSunVisibility @ 0x5c6800 — the +0x1C0 gate
    // @ 0x5c6808, origin = position + (entity+0x1FC..+0x204)
    // @ 0x5c681f..0x5c6847, end = origin + 200*lightdir @ 0x5c6850..0x5c6876,
    // one decrement per blocked cast @ 0x5c689e..0x5c68ea;
    // Physics_RaycastFindCollisionEntity @ 0x539a70 walks entity_a's
    // +0x1BC/+0x1C0 slice]
    int sun_visibility_blocked_rays(World &world, const Entity &e,
                                    const int32_t sun_step_q16[3]);

    // Wire-identity twin of the entity query above. Membership comes from the
    // decoded source's own separately keyed 17-tick slice; blockers remain the
    // locally hosted pool-1/pool-2 collision instances. Position and bbox are
    // live decoded Q16 values, while membership is intentionally stale for up
    // to 16 ticks like retail's entity+0x1BC/+0x1C0 pointer/count.
    int wire_sun_visibility_blocked_rays(
            World &world, uint16_t wire_handle, const FixedVec3 &position_q16,
            const FixedVec3 &bbox_center_q16,
            const int32_t sun_step_q16[3]);

    // Clip an arbitrary segment in place to the nearest terrain or eligible
    // solid in `source`'s candidate slice. The returned handle identifies the
    // nearest entity hit; invalid means terrain-only or no hit. Terrain is
    // skipped for an indoors source. This is the single hosted form of
    // Entity_RaycastCollision used by both the ground-column wrappers and the
    // iris camera ray. [orig: Entity_RaycastCollision @ 0x413760]
    EntityHandle clip_segment_to_nearest_collision(World &world, EntityHandle source,
                                                   const int32_t start[3],
                                                   int32_t inout_end[3]);

    // Ground-column probe through terrain + the entity's candidate models.
    // Builds the ray {x+dx, y+dy, z+z_up} down z_drop, clamps to the terrain
    // column, clips against candidate solids; returns the resolved ground Z and
    // (optionally) the hit entity. [orig: Entity_RaycastGroundHeight @ 0x4142c0 /
    // Entity_RaycastGroundHeightAndObject @ 0x414320 -> Entity_RaycastCollision
    // @ 0x413760]
    int32_t raycast_ground(World &world, EntityHandle source, const int32_t pos[3],
                           int32_t dx, int32_t dy, int32_t z_up, int32_t z_drop,
                           EntityHandle *out_hit_entity);

    // Segment LOS query, TRUE = CLEAR of terrain + sector solids — the AI mutual-LOS
    // seam. [orig: Physics_RaycastTerrainAndSectors @ 0x539910: terrain leg via
    // Terrain_RaycastHeightmapHiRes @ 0x60c760 (skipped when BOTH endpoint entities
    // carry Flags & 0x800000 INDOORS — the heightmap has no interiors), then the
    // sector walk (Physics_RaycastAgainstEntityPool @ 0x538720) over pool 2 statics, then
    // pool 1 dynamics; LOS callers pass ray radius 0.] The walk skips the endpoint
    // entities A/B, their parent slots (los_walker_parent; the AI LOS passes both,
    // other callers may leave them null) and any candidate standing on A or B
    // (+0x28) [orig: @0x53882F..0x538877]. The itemDef type-3 person sphere-block
    // (same-team within 3.0 u exempt) has no resident: organics are pool 0,
    // unwalked, like retail.
    bool raycast_clear(World &world, const int32_t a[3], const int32_t b[3],
                       EntityHandle exclude_a, EntityHandle exclude_b,
                       EntityHandle parent_a = EntityHandle{},
                       EntityHandle parent_b = EntityHandle{});
    // Terrain plus the querying entity's building-candidate slice. A null
    // second entity permits a buried endpoint; height_offset raises/lowers the
    // terrain ray and shrinks/inflates the solid clip. Audio and blasts share
    // this distinct retail query [orig: Entity_CheckLineOfSightTerrainAndEntities
    // @ 0x53B130; Physics_CheckTerrainLineOfSight @ 0x53B080].
	// `query_parent_cleared` models a caller that NULLS the query entity's
	// parentEntity (+0x16C) around the call, so the walker's parent slot falls
	// to mountedChild and the occupant's own mount hull can block the ray
	// [orig: Projectile_ProcessExplosionQueue pool-0 leg @0x4eb142..0x4eb16c].
	bool entity_los_clear(World &world, EntityHandle query, EntityHandle endpoint,
			const int32_t start[3], const int32_t end[3], int32_t height_offset,
			bool all_types = false, bool query_parent_cleared = false);
	// A decoded remote person as the endpoint ENTITY, for a client that keeps
	// no registry entity for it: the two endpoint fields the query reads, its
	// Flags indoors bit and its parent slot. Retail's endpoint is the client's
	// own pool-0 row, so the no-entity leg's buried-endpoint pass never runs.
	// [orig: Physics_CheckTerrainLineOfSight `entityB+24h & 800000h` @0x53B0A0;
	//  Physics_RaycastFindCollisionEntity entity_b[154] / [91] @0x539AE8..0x539B10]
	struct LosWireEndpoint {
		bool indoors = false;
		EntityHandle parent;
	};
	bool wire_person_los_clear(World &world, EntityHandle query, const LosWireEndpoint &endpoint,
			const int32_t start[3], const int32_t end[3], int32_t height_offset,
			bool all_types = false);
	// Same exact query with per-target section matrices retained for a caller-
    // declared stable world phase. The server resets the cache after gameplay
    // movement and again before snapshot fan-out; every recipient LOS ray can
    // then reuse retail's entity-resident matrix equivalent without observing
    // a pre-movement pose.
    bool raycast_clear_cached(World &world, const int32_t a[3], const int32_t b[3],
                              EntityHandle exclude_a, EntityHandle exclude_b,
                              EntityHandle parent_a = EntityHandle{},
                              EntityHandle parent_b = EntityHandle{});
    // Sector candidates the most recent raycast_clear / raycast_clear_cached
    // visited (the broad-phase's exactness, pinned by the collision ctest).
    uint64_t last_los_sector_candidates() const { return last_los_sector_candidates_; }
    // Publish an exact broad-phase over the final live target bounds for a
    // caller-declared stable world phase. The replication fan prepares once
    // after movement/destruction, then every recipient ray queries only cells
    // intersecting its segment while retaining the original exact solid clip.
    // Its phases lap onto the SIM_REPLICATION_QUERY_* rows of world.profile.
    void prepare_cached_raycast_queries(World &world);
    void reset_query_view_cache();

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
    // Physics_RaycastFindCollisionEntity @ 0x539a70 with allowAllTypes = 0 —
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
    struct GameplayContact {
        EntityHandle source;
        EntityHandle target;
    };

    // Drain the exact type-10 Change Team Box touches produced by this tick's
    // authority movement resolves. The collision module owns shape/cadence;
    // gameplay owns the capture request transaction.
    // [orig: contact flag 0x200 @0x4AEB7B; resolver dispatch/callback
    // @0x4B31DD..0x4B3238]
    std::vector<GameplayContact> take_change_team_contacts();

    // Drain successful first-pass MoveCB/non-Powerup contacts. Retail routes
    // these through Entity_ProcessWaypointInteraction instead of treating the
    // target as solid geometry; gameplay interprets the contacted item ID.
    // [orig: resolver attrib branches/call @0x4B2F90..0x4B2FF5]
    std::vector<GameplayContact> take_movement_callback_contacts();

    // Drain successful first-pass Powerup contacts of a player-class source.
    // Retail invokes the target's pickup callback inline, on every peer whose
    // resolver ran the body, and treats the target as no solid geometry;
    // world/powerup.h runs the callback in this order.
    // [orig: the attrib&2 branch @0x4B2FB8, the source Flags&0x100 gate
    //  @0x4B2FBD, Entity_InvokeCollisionCallback @0x4B2FCC]
    std::vector<GameplayContact> take_powerup_contacts();

    // The same Powerup contacts a wire-replica resolve (resolve_replica) made:
    // the source is the decoded row's wire handle, which names no registry
    // entity, so the embedder runs the pickup for its transient copy of the
    // body. [orig: the same branch; a client's resolver moves its remote
    // bodies through the org2 tail call @0x4B7CF4]
    struct ReplicaPowerupContact {
        uint16_t wire_handle = 0xFFFF;
        EntityHandle target;
    };
    std::vector<ReplicaPowerupContact> take_replica_powerup_contacts();

    struct ResolveState {
        int32_t prev_pos[3] = {};   // savedLivePose stand-in (updated per resolve)
        bool prev_valid = false;
        uint8_t skip_counter = 0;   // [orig: entity pad_370[3] idle throttle]
    };
    // is_player_class = the entity's wire Player class bit (Flags & 0x100):
    // retail keys EVERY physics leg on it — the skip-path gravity undo
    // (@ 0x4b2cd9), the candidate mask (@ 0x4b2f7c / @ 0x4b35af), the ladder
    // entry/chase gates (@ 0x4b3271 / @ 0x4b32a5 / @ 0x4b33aa) and the exit
    // push — for local and remote bodies alike; only the local side-writes
    // (g_LocalPlayerLookYaw @ 0x4b33ca, the blink mirror @ 0x4b34ce, the pitch
    // restore @ 0x4b3cdc / @ 0x4b3cfe) add `entity == g_LocalPlayerEntity`,
    // and those ride LadderResolveIO::is_local_player.
    // anim_state_flags = the state's g_AnimStateFlagsTable word (bit 0 forces a
    // full update; the id itself picks the repulsion-exempt states).
    // out_ground (optional) receives the ground probe's hit entity — the same
    // value the resolver stores into a registered source's groundEntity.
    // ladder_io (optional) wires the D-COL-5 climb legs: the CL entry gate +
    // anchor snap, the per-tick alignment chase, and the exit push / pitch
    // restore. Without it only the raw latch/bookkeeping runs (replica rows,
    // harness callers).
    // eye_offset (optional, xyz 16.16) is the entity's CameraOffset: the second
    // capsule test point is pos + CameraOffset [orig: @0x4b2ee0-0x4b2ef8]. When
    // absent the resolver derives the org1 producer's value from the capsule,
    // max(top - bottom, 0x9000) with lean at rest [orig: Entity_UpdateInfantryAI
    // @0x4b9910, kong 155492-155521] -- harness rows and the replica seam.
    int32_t resolve_entity(World &world, EntityHandle source, ResolveState &state,
                           int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                           int32_t capsule_bottom, int32_t capsule_top,
                           int32_t heading, int32_t body_pitch, bool is_player_class,
                           bool is_authority, uint32_t tick, int32_t anim_state_id,
                           uint32_t anim_state_flags, int16_t &health,
                           EntityHandle *out_ground = nullptr,
                           const LadderResolveIO *ladder_io = nullptr,
                           const int32_t *eye_offset = nullptr,
                           devtools::TickProfile *profile = nullptr);

    // The REPLICA seam (net-re §5.38e, D-NET-196): the same resolver for a
    // decoded remote row that has NO world entity — retail runs remote
    // organics through the ordinary org movers whose shared tail calls the
    // resolver ungated [orig: Entity_UpdateInfantryPlayerBody call @0x4B7CF4;
    // Entity_UpdateInfantryAI @0x4BF7FA]. The candidate slice is built AD HOC
    // at the query position with the pool-0 rule (the exact caller-supplied
    // authored bound radius + 4.0 u pad), person repulsion runs the
    // world persons_ PLUS the caller-passed replica peer spheres through the
    // SAME witnessed loop (a ClientState peer's "live re-read" is its staged
    // snapshot — the row has no registry entity), and the Entity-side writes
    // (flag latches, blink, groundEntity) become the out_ground return.
    // is_authority is pinned false: a replica resolve never runs damage legs.
    struct ReplicaPeer {
        int32_t x = 0, y = 0, z = 0;
        int32_t radius = 0;
        uint16_t handle = 0xFFFF; // wire handle, for self-exclusion only
    };
    // entity_flags (optional) is the row's persistent retail-Flags mirror — the
    // caller-owned stand-in for the entity Flags word the resolver's latch
    // sites read and write on a registry row. When provided, the resolve runs
    // the SAME flag channel retail runs on a remote entity: the InAir bit
    // feeds the idle-skip full-update discriminant [orig: @ 0x4b2ca6], the
    // resolve-start clear drops Indoors/LadderContact/zone bits
    // [orig: @ 0x4b2d54 block], the CL touch latches LadderContact
    // [orig: @ 0x4b3291], the blink accum latches Indoors [orig: @ 0x4b34c2],
    // the dead/hidden and parachute bits gate/widen repulsion
    // [orig: @ 0x4b3aac/0x4b3aba], and the ground probe's terrain clamp is
    // skipped while Indoors [orig: the Flags & 0x800000 gate @ 0x413785].
    int32_t resolve_replica(World &world, ResolveState &state, int32_t pos[3],
                            int32_t vel_xy[2], int32_t &vel_z,
                            int32_t capsule_bottom, int32_t capsule_top,
                            int32_t source_bound_radius_q16,
                            bool is_player_class, uint32_t tick, int32_t anim_state_id,
                            uint32_t anim_state_flags, const ReplicaPeer *peers,
                            int32_t peer_count, uint16_t exclude_handle,
                            uint32_t *entity_flags, EntityHandle *out_ground);

	// Fold model contacts into the terrain forces, one query per authored
	// wheel/hull probe. Returns severity 0..3 and the last severe hit entity.
	// The hull position is the already-integrated pose; savedLivePose supplies
	// the previous-plane gate. Mounted children are excluded through three
	// carrier links. [orig: Entity_CheckCollisionState @0x462A30, entity half
	// @0x462DFB..0x4632CC; Entity_ComputeCollisionForces @0x462150 boat twin]
	int32_t resolve_vehicle_probes(World &world, Entity &source, const int32_t hull_pos[3],
			const int32_t (*probes)[3], const int32_t *radii, int count, int32_t soft, int32_t hard,
			VehicleProbeForce *forces, EntityHandle &hit_entity);

	// World-level blink state for the local player.
	// [orig: g_LocalPlayerBlinkFlags @ 0x24C1934]
	uint32_t local_player_blink_flags = 0;
    EntityHandle local_player;

    // The last CL latch's alignment frame. Retail keeps these as globals that
    // persist across resolves — every consumer (the motors' dismount pushes,
    // the bottom-exit compare, org1's 33/35 select) is Flags-0x100000-gated, so
    // one world-level frame is the faithful carrier.
    // [orig: g_LadderContact{Pitch,Yaw,X,Y,Z} @ 0xB5AB70..80]
    LadderContact last_ladder_frame;
    // Whether the last resolve APPLIED a nonzero net push — org1's on-ladder
    // facing press runs only while this is clear. (NOT the pass-2 contact
    // flag: retail re-zeroes that stack slot after both passes and re-sets it
    // only when the accumulated total force is nonzero, so the global latches
    // "the resolver moved the entity this resolve".)
    // [orig: dword_B57C8C — slot re-zero @ 0x4b3734, set on nonzero total
    //  @ 0x4b3767, stored @ 0x4b3a5c-0x4b3a62; read @ 0x4bfa3e-0x4bfa45]
    bool resolver_applied_push = false;

    // The witnessed on-ladder person probe: 1.25u ahead of the climber, a live
    // pool-0 person within 1.125u on both axes whose Z band overlaps holds the
    // climb (org1 stamps climb_idle 32 and skips the press/select).
    // [orig: the g_PoolList[0] scan @ 0x4bf9b7-0x4bfa2b]
    bool ladder_person_ahead(World &world, EntityHandle self, int32_t probe_x,
                             int32_t probe_y, int32_t self_z, int32_t self_bound);

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

    // Embedder-wired terrain (shared with the AI system's field).
    const terrain::TerrainHeightField *terrain = nullptr;

    // --- introspection for tests/embedders ---
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
    // The husk-stage collision model attached to that identity (nullptr when
    // the def authors no husk or the slot is absent/recycled). Retail keeps
    // the husk model pointer at entity+0x34 beside the graphic at +0x30 and
    // lets each consumer pick by Flags & 4 [orig: Entity_CalcSlopeForces
    // @ 0x4b0c10..0x4b0c1b] — this is the husk half of that pair.
    const CollisionModel *husk_model_for(const World &world, EntityHandle h) const;

    // Retail's per-section collision-triangle debris sampler. The transform
    // callback is resolved through the same target view as projectile traces;
    // an absent/invalid model produces no synthetic fallback.
    // [orig: Entity_SpawnSectionDebris @0x43f580]
    std::vector<SectionDebrisSample> sample_section_debris(
            const World &world, EntityHandle h,
            const Vec3 &blast_center) const;

    // Read-only world-space geometry snapshot for an embedder collision debug view.
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
    void build_candidate_slices(World &world);
    // entity_los_clear over an endpoint described by its read fields.
    bool entity_los_clear_impl(World &world, EntityHandle listener, EntityHandle source,
            bool endpoint_present, bool endpoint_indoors, EntityHandle parent_b,
            const int32_t start_in[3], const int32_t end_in[3], int32_t height_offset,
            bool all_types, bool query_parent_cleared);
    // Contact-flag side effects shared by both resolver passes (DH/DM/DL damage +
    // the CA/CM entity flags). [orig: the dispatch @ 0x4b30b7-0x4b351e]
    void apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health, bool is_authority);
    void record_change_team_contact(EntityHandle source, EntityHandle trigger);
    void record_movement_callback_contact(EntityHandle source, EntityHandle target);
    void record_powerup_contact(EntityHandle source, EntityHandle target);

    std::vector<GameplayContact> change_team_contacts_;
    std::vector<GameplayContact> movement_callback_contacts_;
    std::vector<GameplayContact> powerup_contacts_;
    std::vector<ReplicaPowerupContact> replica_powerup_contacts_;

    struct Instance {
        int32_t model_id = -1;
        // The husk-stage collision model — substituted for every query once the
        // entity carries the destroyed flag (Flags & 4): rays, contacts, and
        // ground probes collide with the wreck, not the intact model. -1 = the
        // def authors no husk (the intact model keeps serving, the witnessed
        // fallback). [orig: the +52 huskModel substitution in the pool walk
        // Physics_RaycastAgainstEntityPool @ 0x538720 and the ray/contact picks
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
        // The source position the slice was built at (wire slices only): a
        // row that has moved past its pad since then needs a fresh build.
        int32_t built_pos[3] = {0, 0, 0};
    };
    struct StableLosCandidate {
        EntityHandle h;
        int32_t pos[3] = {};
        int32_t radius = 0;
    };
    struct StableLosCell {
        std::vector<uint32_t> candidates;
        uint32_t epoch = 0;
    };
    struct StableLosCellSpan {
        int32_t min_x = 0;
        int32_t max_x = 0;
        int32_t min_y = 0;
        int32_t max_y = 0;
        uint32_t candidate = 0;
    };

    const CollisionTargetView *target_view(const World &world, EntityHandle h,
                                           CollisionTargetView &scratch,
                                           std::vector<CollisionMatrix> &mat_scratch) const;
    // One candidate entity vs one radiused segment: the cheap-metadata broad
    // phase, then live section matrices for survivors (shared by sound and the
    // slice-scoped iris/entity sun casts).
    bool entity_blocks_segment(World &world, EntityHandle candidate,
                               const CollisionRay &ray, int32_t radius,
                               int32_t broad_r);
    bool candidate_slice_segment_hits_solid(
            World &world, const EntityHandle *slice, int32_t slice_count,
            EntityHandle source_registry_twin, const int32_t a[3],
            const int32_t b[3], int32_t radius);
    int candidate_slice_sun_blocked_rays(
            World &world, const EntityHandle *slice, int32_t slice_count,
            EntityHandle source_registry_twin, const int32_t a[3],
            const int32_t b[3]);
    const EntityHandle *wire_candidate_slice(
            uint16_t wire_handle, int32_t &count_out) const;
    void invalidate_trace_view(EntityHandle h);
    void invalidate_trace_views();
    void invalidate_stable_los_index();
    const std::vector<uint32_t> &stable_los_candidates_for_ray(
            const CollisionRay &ray);
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
    // Exact stable-phase LOS broad phase. Candidates retain pool-2 then pool-1
    // order; a bounded dense grid handles ordinary mission extents and the
    // sparse hash grid handles pathological extents. Both bucket forms hold
    // candidate indices, and each query sorts its small deduplicated result
    // back into pool order before the exact clip.
    std::vector<StableLosCandidate> stable_los_candidates_;
    std::unordered_map<uint64_t, StableLosCell> stable_los_cells_;
    std::vector<StableLosCell> stable_los_dense_cells_;
    std::vector<StableLosCellSpan> stable_los_cell_spans_;
    std::vector<uint32_t> stable_los_large_candidates_;
    std::vector<uint32_t> stable_los_query_candidates_;
    std::vector<uint32_t> stable_los_query_marks_;
    uint32_t stable_los_query_generation_ = 0;
    uint32_t stable_los_index_epoch_ = 0;
    int32_t stable_los_dense_min_x_ = 0;
    int32_t stable_los_dense_max_x_ = -1;
    int32_t stable_los_dense_min_y_ = 0;
    int32_t stable_los_dense_max_y_ = -1;
    int32_t stable_los_dense_height_ = 0;
    bool stable_los_dense_enabled_ = false;
    bool stable_los_index_ready_ = false;
    const CollisionTargetView *trace_target_view(const World &world, EntityHandle h) const;
    bool raycast_clear_impl(World &world, const int32_t a[3], const int32_t b[3],
                            EntityHandle exclude_a, EntityHandle exclude_b,
                            EntityHandle parent_a, EntityHandle parent_b,
                            bool cache_target_views);
    uint64_t last_los_sector_candidates_ = 0;
    ProjectileHit trace_projectile_impl(const World &world,
                                        const ProjectileTrace &trace,
                                        bool person_faces_only, bool aim = false) const;
    void record_trace_debug(const World &world, const ProjectileTrace &trace,
                            const ProjectileHit &hit, RayDebugCategory category,
                            bool knife) const;

    bool trace_profile_enabled_ = false;
    mutable TraceProfile trace_profile_;
    bool ray_debug_enabled_ = false;
    uint32_t ray_debug_mask_ = kRayDebugMaskAll;
    int32_t ray_debug_ttl_ticks_ = 93; // ~1.5 s at 62 Hz
    mutable RayDebugCategory ray_debug_scope_ = RayDebugCategory::kUncategorized;
    mutable std::array<RayDebugRing,
                       static_cast<size_t>(RayDebugCategory::kCount)>
            ray_debug_rings_;
    bool contact_debug_enabled_ = false;
    uint32_t contact_debug_mask_ = kContactDebugMaskAll;
    mutable ContactDebugRing contact_debug_ring_;
    // target_view's model selection without the section-matrix build: fills the
    // entity position and bound radius for the witnessed gate-before-view order.
    // False exactly when target_view would return null; solid_only also
    // rejects models without a solid volume.
    bool target_bound(const World &world, EntityHandle h, int32_t pos_out[3],
                      int32_t &radius_out, bool solid_only) const;

    std::vector<CollisionModel> models_;
    std::unordered_map<uint16_t, Instance> instances_; // key: EntityHandle.packed
    IPoseProvider *pose_provider_ = nullptr; // non-owning embedder seam

    std::vector<StaticSlot> statics_;   // cap 1199 counted [orig: g_StaticProx*]
    int32_t static_count_ = 0;
    int32_t static_building_count_ = 0;
    // Build state is independent of table contents: an empty-but-built table is
    // still authoritative and must not fall back to whole-registry scans.
    bool tick_tables_built_ = false;
    bool candidate_slices_built_ = false;
    // A pool-2 instance/registry edge since the statics table was last built.
    bool statics_dirty_ = true;
    // Candidate slices rebuild only every 17th tick — the counter increments per
    // tick and the rebuild fires (and resets it) once it reaches 16; pool tables
    // rebuild every tick. BSS-zero start: retail's first slice build lands on
    // tick 17 (mission starts run 16 sliceless ticks). [orig: dword_B57C84 vs
    // 0x10 @ 0x4c240f, zeroed by Entity_BuildProximityListsFromPools @ 0x4b8ed0]
    uint32_t slice_refresh_counter_ = 0;

    std::vector<DynSlot> dynamics_;     // [orig: g_DynProx*]
    std::vector<PersonSlot> persons_;   // [orig: g_PersonProx*]
    std::vector<WirePersonCollisionProxy> wire_person_proxies_;
    std::vector<WireDynamicCollisionProxy> wire_dynamic_proxies_;
    // Server H used solely as local L's ordering key during proxy-enabled
    // person walks. Geometry and ignore logic continue to use L.
    uint16_t wire_local_player_handle_ = 0xFFFF;
    std::vector<EntityHandle> arena_;   // cap 3000 [orig: g_ProxCandidateArena]
    std::unordered_map<uint16_t, CandidateSlice> candidates_;
    // A distinct identity domain for decoded rows. Using a second arena/map is
    // structural: wire H=0 and a registry L=0 are both valid simultaneously.
    std::vector<EntityHandle> wire_arena_;
    std::unordered_map<uint16_t, CandidateSlice> wire_candidates_;
    // Staged only for the duration of one resolve_replica call: the replica
    // peer spheres the person-repulsion loop walks after persons_, and the
    // calling row's own wire handle (self-exclusion). Empty for every
    // ordinary resolve_entity call.
    const ReplicaPeer *replica_peers_ = nullptr;
    int32_t replica_peer_count_ = 0;
    uint16_t replica_exclude_handle_ = 0xFFFF;
    // A cell index over the staged peer table, rebuilt once per (table, pump
    // tick): the repulsion loop only ever moves a row for peers within 30% of
    // the summed radii, so each resolve gathers the peers of the cells that
    // reach can touch and walks them in table order — the same peers, the
    // same order, the same pushes as the full walk (see the exactness guard
    // beside the walk). Pure acceleration; no witnessed rule lives here.
    struct ReplicaPeerIndex {
        const ReplicaPeer *src = nullptr;
        int32_t count = 0;
        uint32_t tick = 0;
        int32_t max_radius = 0;
        std::unordered_map<uint64_t, std::vector<int32_t>> cells;
        std::vector<int32_t> gathered;
    };
    ReplicaPeerIndex replica_peer_index_;
    bool replica_peer_index_enabled_ = true;
    void stage_replica_peer_index(const ReplicaPeer *peers, int32_t count,
                                  uint32_t tick);

public:
    // Test seam: the full table walk stays reachable so the cell gather can
    // be proven equivalent against it.
    void set_replica_peer_index_enabled(bool enabled) {
        replica_peer_index_enabled_ = enabled;
    }

private:
    int32_t replica_source_bound_radius_q16_ = 0;
    // Staged like replica_peers_: the calling row's retail-Flags mirror. The
    // resolver's flag latch sites and the ground probe's indoors gate read and
    // write it exactly where they read and write ent->flags on a registry row.
    uint32_t *replica_flags_ = nullptr;
};

} // namespace opennova::world
