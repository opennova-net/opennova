// The collision queries' records: the blink accumulation, the volume and touch codes, the
// target view, the ray, face, person and contact query records, the decoded rows' wire proxies
// and the projectile trace and its hit. Split out of collision.h by responsibility (the
// oversize-header ratchet, A2-43); collision.h includes it. Witness record:
// docs/world/world-wac-ai-re.md §15.
#pragma once

#include <cstdint>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <runtime/world/collision_model.h>
#include <runtime/world/entity.h>

namespace opennova::world {

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
    // The pool-2 building a packed hit names; invalid for no hit (0)
    // [orig: Pool_GetEntryUnchecked(2, hit >> 20) @0x56c8c9].
    static constexpr EntityHandle hit_owner(uint32_t key) {
        return key == 0 ? EntityHandle{} : EntityHandle::make(2, hit_pool_entity_index(key));
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
    kSolidCB = 1,         // the solid lines of sight stop at [orig: @0x413298]
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
    kPlayerCP = 19,       // solid for players, not AI (mask 0x2) [orig: @0x4ae543]
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
// Every ray takes a face from either side but a 0x800 face, which it takes
// only entering from its front; flag 1 takes even that one from either side
// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e5115..0x4e5139, every caller
// passing the back-face argument 1].
inline constexpr uint32_t kFaceFlagBothSides = 0x1;
inline constexpr uint32_t kFaceFlagNeverHit = 0x100;
inline constexpr uint32_t kFaceFlagFrontOnly = 0x800;

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
    // The face's own normal (the model's int16 x 4), unrotated by the section
    // matrix: what Physics_ComputeReflectionForce hands its caller.
    int32_t local_normal_q16[3] = {};
    int32_t section_index = -1;
    int32_t face_index = -1;
    int32_t section_face_index = -1;
    uint32_t material_flags = 0;
    int32_t poly_type = -1;
};

// `back_face`: the walk's back-face argument (ProjectileTrace::back_face_arg).
bool collision_raycast_polygons(const CollisionTargetView &target,
                                const CollisionRay &ray,
                                int32_t segment_length_q16,
                                uint32_t ammo_flags,
                                CollisionPolygonHit &out_hit,
                                bool back_face = true);

// The reflection's face lookup on a struck entity's model: the face at a
// section-local index of a section, within the section count and that
// section's face count, its normal x 4 (`local_out`) and that rotated by the
// section's matrix (`world_out`). False past either bound, where the
// reflection keeps its negated-velocity normal.
// [orig: Physics_ComputeReflectionForce @0x4E4310 -- the section bound
//  @0x4E4416..0x4E442D, the face bound @0x4E4440..0x4E444D, the normal x 4
//  @0x4E445E..0x4E4486, the rotation @0x4E449D]
bool collision_section_face_normal(const CollisionTargetView &target, int32_t section,
                                   int32_t section_face, int32_t local_out[3],
                                   int32_t world_out[3]);

// ----------------------------------------------------------------------------
// Segment-vs-face-mesh query — the PROJECTILE hit test. Closest accepted face
// across every enabled section; the sphere broad phase is the caller's.
// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0 — per-bone inverse
// transform of the segment, face AABB reject, flags & 0x100 skip, the
// material-17 foliage skip when the ammo carries flag 0x4000000, the
// plane-straddle test ((v.n >> 14) + dist on both endpoints), the direction
// rule (flag 1 either side; else, with the back-face argument the projectile
// callers push, 1, either side unless 0x800, which takes a front entry
// d0>0 && d1<=0 alone, and with the clamped ground query's 0 a front entry
// alone), the distance
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
// `ray_dir` / `ray_length` (both or neither): the caller's ray context
// direction and length (rayState[6..8] / [9]) where they differ from the
// segment's; the hit's distance is then |d0| * ray_length / (|d0| + |d1|) and
// its test point start + ray_dir * that distance [orig: @0x4E51B8;
// @0x4E51E4..0x4E522A]. Null = the segment's own unit direction and length.
bool collision_raycast_faces(const CollisionTargetView &target, const int32_t start[3],
                             const int32_t end[3], uint32_t ammo_flags, RayFaceHit &out,
                             const int32_t *ray_dir = nullptr, int32_t ray_length = 0,
                             bool back_face = true);
// The nearest-entity walker's face test over one entity: each enabled section's CFAC faces whose
// box meets the section-local segment's, bit 2 of the flags clear and the material not 16, that
// the segment crosses (either way) at |d0| * length / (|d0| + |d1|) along `dir` inside the
// triangle. `best` (the ray length at the walk's start) takes each strictly nearer hit's
// distance; true when one did.
// [orig: Physics_RaycastEntityPool @0x4143E0 - the section walk @0x414620..0x41495D (the disabled
//  bit @0x414620, Matrix_Transpose3x3WithNegateCol3 @0x414633, the local start / end / direction
//  @0x414646 / @0x41465C / @0x414675), the face box @0x4146F0..0x414738, bit 2 of +0x24
//  @0x41473E, the material byte +0x28 == 16 under the context's zero +0x48 @0x414748..0x414756,
//  the crossing @0x4147E2..0x4147F2, the distance @0x4147F8..0x41485A, the test point
//  @0x41485C..0x4148CD, Math_PointInTriangle2D @0x4148E6, the strict `<` @0x4148F2..0x4148F5]
bool collision_raycast_entity_faces(const CollisionTargetView &target, const int32_t start[3],
                                    const int32_t end[3], const int32_t dir[3], int32_t length,
                                    int32_t &best);

// A person section's hit radius (Q16) about its bone: 45 percent of the
// authored COBJ radius (65 for section 14, the head) plus the fixed 0xCCC floor
// and the round's own extra radius, sections 15 and 16 capped at 0x3000.
// [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670]
inline int32_t person_effective_radius(int32_t section, int32_t authored_radius,
                                int32_t extra_radius) {
    const int32_t scale = section == 14 ? 65 : 45;
    int32_t effective =
            extra_radius + 0xCCC +
            static_cast<int32_t>(static_cast<int64_t>(scale) * authored_radius / 100);
    if ((section == 15 || section == 16) && effective > 0x3000)
        effective = 0x3000;
    return effective;
}

// The extra radius a round's bone walk gives one person: the round's own,
// which dfx2.exe narrows for a player (Flags 0x100) that rides something
// (+0x16C) or holds still (MoveOrder bit 3 clear): past 0x1000 it keeps half of
// the rest. Jointops.exe has no such step.
// [orig: Physics_RaycastAgainstBoneSections @0x4e4670] [orig: @0x4e17af..0x4e17e0
//  (dfx2.exe)]
inline int32_t person_extra_radius(int title, bool player, bool mounted, bool moving,
                                   int32_t extra_radius) {
    if (title != gameprofile::GAME_DFX2 || !player) return extra_radius;
    if (!mounted && moving) return extra_radius;
    return extra_radius > 0x1000 ? ((extra_radius - 0x1000) >> 1) + 0x1000 : extra_radius;
}
inline int32_t person_extra_radius(int title, const Entity &target, int32_t extra_radius) {
    return person_extra_radius(title,
            ((target.flags | target.engine_flags) & kEntityFlagPlayer) != 0,
            target.mount_target.valid(),
            (target.net_move_input & Entity::kMoveOrderMoving) != 0, extra_radius);
}

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
    // The query reached a building (def type 5) past its broad phase: its
    // door-touch mask (+0x2B4) starts this query at 0, then takes the touched
    // door bits; the caller writes it to the target (D-NET-133).
    // [orig: Entity_ComputeBoneCollisionForce @0x4AE150 - the reset @0x4AE281,
    //  the OR @0x4AEB22]
    bool door_mask_reset = false;
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
    // The table's copy of the person's Position, taken when the table is built
    // ahead of the tick's pool-0 walk: the coarse gates read it, the movement
    // resolver's reach box and planar length and the person trace's broad phase.
    // [orig: Entity_BuildProximityLists_Pool01 @0x4B9340 (the copies
    //  @0x4B93F5..0x4B9409); Entity_MovementCollisionResolver @0x4B2BD0 (the
    //  reach gate @0x4B3AF6..0x4B3B74); Physics_RaycastAgainstProximityList
    //  @0x4E4A30 (the coarse gate @0x4E4AB4..0x4E4C26)]
    FixedVec3 position_q16;
    int32_t bound_radius_q16 = 0;
    int32_t uniform_scale_q16 = 0;
    FixedVec3 bbox_center_q16;
    // The person's collision model (CollisionWorld model id; -1 unresolved):
    // a resolved model walks its posed bone spheres through the pose
    // provider's build_wire_person_section_matrices, else the torso stand-in.
    int32_t model_id = -1;
    // The entity fields person_extra_radius reads: the Player bit of Flags,
    // a mount, and MoveOrder's moving bit.
    bool player = false;
    bool mounted = false;
    bool moving = false;
    // The person's team (entity+354) as the world stream bears it, 0 before a record that carries it:
    // the sun ray's person rule passes over a row of the local player's team near the eye
    // (sun_ray_clear_of_entities).
    uint8_t team = 0;
    // The person's LIVE Flags dead bit (0x02): the table keeps no copy, so the
    // movement resolver re-reads the entity's own word past the reach gate and
    // passes over a dead peer; a trace does not test it. A body below the own
    // slot that dies in its own update this tick is skipped by the own body's
    // resolve after it (set_wire_person_proxy_live).
    // [orig: Entity_MovementCollisionResolver @0x4B2BD0 (g_PersonProxEntity
    //  @0x4B3B7A, the +0x24 & 2 skip @0x4B3B89..0x4B3B8D);
    //  Entity_UpdateInfantryPlayerBody @0x4B40E0 (the death gate @0x4B4BF1)]
    bool dead = false;
    // The LIVE Flags bits both proximity traces pass a row over on, past their coarse
    // gate: 0x1 (carried / hidden) and 0x2000000 (a minefield: the lndm class init marks
    // a row with a graphic). The build passes over a bit-0 row already; one that gains a
    // bit after the build is passed over here (D-NET-196).
    // [orig: Physics_RaycastAgainstProximityList @0x4E4A30 (g_PersonProxEntity @0x4E4C2C,
    //  `test [eax+24h], 2000001h` @0x4E4C33); Projectile_RaycastProximitySlots @0x4E5340
    //  (g_DynProxEntity @0x4E5760, the test @0x4E5767); Entity_BuildProximityLists_Pool01
    //  @0x4B9340 (the bit-0 skips @0x4B9382 / @0x4B93E4); Minefield_InitEntity @0x4417D0,
    //  the lndm row's init (g_EntityClassEventCallbackTable @0x813000, the row @0x813198) -
    //  `or [esi+24h], 2000000h` @0x441A23]
    uint32_t live_flags = 0;
    // The person's LIVE Position: the movement resolver's push and the trace's
    // bone walk read the entity, not the table's copy. The table build seeds it
    // from position_q16 (replace_wire_collision_proxies); a person's update
    // after the build moves it.
    // [orig: Entity_MovementCollisionResolver @0x4B2BD0 (the live reads
    //  @0x4B3B93..0x4B3B99); Physics_RaycastAgainstProximityList @0x4E4A30
    //  (Physics_RaycastAgainstBoneSections on g_PersonProxEntity, the call
    //  @0x4E4C59)]
    FixedVec3 live_position_q16;
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
    // The table's copy of the mover's Position, taken at the tick's proximity
    // build: the coarse gates read it, the candidate slices' build and the
    // trace's slot gate.
    // [orig: Entity_BuildProximityLists_Pool01 @0x4B9340 (the copies
    //  @0x4B9393..0x4B93A7, the entity pointer @0x4B93B3);
    //  Projectile_RaycastProximitySlots @0x4E5340 (the slot gate on
    //  g_DynProxY/X/Z @0x4E55E6..0x4E575A)]
    FixedVec3 position_q16;
    // The mover's LIVE Position, heading and pitch / roll samples, which the
    // narrow phase reads through the table's entity pointer: the bound-sphere
    // gate and the posed collision faces. The table build seeds the position
    // from position_q16 (replace_wire_collision_proxies); a move after the
    // build writes all four (set_wire_dynamic_proxy_live).
    // [orig: Projectile_RaycastProximitySlots @0x4E5340 (g_DynProxEntity
    //  @0x4E5760, the Physics_RaycastAgainstBoneCollision call @0x4E578D);
    //  Physics_RaycastAgainstBoneCollision @0x4E4CB0 (the live +0 / +4..+12
    //  reads @0x4E4CD4..0x4E4D23)]
    FixedVec3 live_position_q16;
    int32_t heading_bam = 0;        // reconstructed entity+16 (yaw_byte << 24)
    int32_t pitch_bam = 0;          // retained entity+20 spawn/dead sample
    int32_t roll_bam = 0;           // retained entity+24 spawn/dead sample
    // The LIVE Flags bits both proximity traces pass a row over on, past their coarse
    // gate: 0x1 (carried / hidden) and 0x2000000 (a minefield: the lndm class init marks
    // a row with a graphic). The build passes over a bit-0 row already; one that gains a
    // bit after the build is passed over here (D-NET-196).
    // [orig: Physics_RaycastAgainstProximityList @0x4E4A30 (g_PersonProxEntity @0x4E4C2C,
    //  `test [eax+24h], 2000001h` @0x4E4C33); Projectile_RaycastProximitySlots @0x4E5340
    //  (g_DynProxEntity @0x4E5760, the test @0x4E5767); Entity_BuildProximityLists_Pool01
    //  @0x4B9340 (the bit-0 skips @0x4B9382 / @0x4B93E4); Minefield_InitEntity @0x4417D0,
    //  the lndm row's init (g_EntityClassEventCallbackTable @0x813000, the row @0x813198) -
    //  `or [esi+24h], 2000000h` @0x441A23]
    uint32_t live_flags = 0;
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
    // The ray context's own direction and length (rayState[6..8] and [9]) where
    // a caller's differ from the segment's: a throwable motor's ray runs along
    // its velocity over its speed. A pools-2/1 walk (the terrain, water and
    // person domains off) then gates each slot on the line through the start
    // along that direction, reads a face hit at start + dir * dist with
    // dist = |d0| * length / (|d0| + |d1|), and reports that dist as the hit's
    // distance (its t a fraction of the length). Zero length = the segment's
    // own unit direction and length, as every bullet's.
    // [orig: Projectile_RaycastProximitySlots @0x4E5340 -- the seed
    //  rayState[29] = rayState[9] @0x4E534F, the slot gate's projection onto
    //  rayState[6..8]; Physics_RaycastAgainstBoneCollision @0x4E4CB0 -- the
    //  parameter @0x4E51B8, the test point @0x4E51E4..0x4E522A]
    FixedVec3 ray_dir_q16;
    int32_t ray_length_q16 = 0;
    // The face walk's back-face argument: set, a face without flag 1 is taken from
    // either side unless 0x800, which takes it entering its front alone (every
    // projectile walk pushes 1); clear, a face without flag 1 is taken entering its
    // front alone (the clamped ground query, D-COL-17).
    // [orig: Physics_RaycastAgainstBoneCollision @0x4E4CB0 - the argument's test
    //  @0x4E511D, the 0x800 leg @0x4E5127..0x4E513B, the front leg @0x4E5140..0x4E514A]
    bool back_face_arg = true;
    // One entity alone, through its own gate and faces and no slot gate: the
    // walk's other rows are misses, and so is the entity itself without a
    // collision model (Physics_RaycastAgainstBoneCollision's model test).
    // [orig: Weapon_ValidateHitTarget @0x4E52E0 -- its ItemTypeIndex, flag
    // and exclusion tests @0x4E52F1..0x4E531E, the call
    // Physics_RaycastAgainstBoneCollision @0x4E5327]
    EntityHandle only_entity;
    // The owner's mount (and a gunner's carrier) as the ray's ray[18..20]
    // exclusions, a bullet's; the throwable motors' ray contexts leave them
    // zero (their memsets), excluding the owner alone.
    // [orig: Entity_UpdateGrenadePhysics @0x444522, Entity_UpdateSatchelPhysics
    //  @0x4486B2, Entity_UpdateClaymorePhysics @0x447664]
    bool owner_mount_exclusion = true;
    // The throwable motors' two walks share one ray record. Each walk resets
    // the kept distance to the ray's length, and its best face writes the
    // face flags, the face byte, the section and the face (ray[21], [22],
    // [31], [33]) whether or not the motor keeps it; the motor copies only the
    // distance and the entity. So when pool 1's walk, the second, also hits,
    // a kept pool-2 hit carries pool 1's record, and the hit's normals read
    // that section and face on the KEPT entity's model (none past its
    // bounds).
    // [orig: Projectile_RaycastProximitySlots @0x4E5340, the reset
    //  @0x4E534F; Physics_RaycastAgainstBoneCollision, the record
    //  @0x4E5254..0x4E5276; the keeps: Entity_UpdateGrenadePhysics
    //  @0x44461D..0x44467A, Entity_UpdateSatchelPhysics @0x4487B6..0x448808,
    //  Entity_UpdateClaymorePhysics @0x447768..0x4477BA]
    bool shared_ray_record = false;
};

struct ProjectileHit {
    int32_t distance_q16 = 0;
    ProjectileHitClass hit_class = ProjectileHitClass::None;
    EntityHandle geometry_entity;
    int32_t t_q16 = 0x10000;
    FixedVec3 position_q16;
    FixedVec3 normal_q16;
    // A face hit's normal in the model's own frame (the int16 normal x 4,
    // before the section matrix): the out-normal Physics_ComputeReflectionForce
    // returns, which the throwable motors' stick test, stick pose and slide
    // read, where the reflection itself uses the rotated one. Zero for a hit
    // without a face. [orig: Physics_ComputeReflectionForce @0x4E4310 -- the
    // out-normal @0x4E4465..0x4E4491, ahead of the rotation of its own copy
    // @0x4E449D]
    FixedVec3 local_normal_q16;
    // Whether the reflection's face lookup finds the hit's face: its normals
    // are then the face's, a stored zero normal included, where a hit without
    // one keeps the reflection's negated unit velocity.
    // [orig: Physics_ComputeReflectionForce @0x4E4416..0x4E449D]
    bool face_normal_found = false;
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

// The static prox list's membership and bound: a pool-2 row with an item type
// (+0x1C) and a def (+0x20) is written, and the count advances only while it is
// under 1199, so the 1200th row's write lands but is never counted and no
// later row is. [orig: Entity_BuildProximityLists_Pool2 @ 0x4b9430 - pass 1's
// tests @ 0x4b945d / @ 0x4b9466, `cmp eax, 4AFh` @ 0x4b94b7 and its `jge`
// @ 0x4b94cb; pass 2's @ 0x4b94f1 / @ 0x4b94fa, `cmp eax, 4AFh` @ 0x4b954b and
// its `jge` @ 0x4b955f]
inline constexpr int32_t kStaticProxCountBound = 1199;
inline bool static_prox_listed(const Entity &e) {
    return e.item_type_index != 0 && e.has_item_def;
}

} // namespace opennova::world
