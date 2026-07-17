// Rendering occlusion: the blink-box-driven building-section visibility engine —
// per-frame section masks built by the portal traversal (render_VPT), the occluder
// culling pass (render_TOC), the mission-start portal weld, and the per-entity
// render gates (blink-hits + the outdoors three-ray latch). Witness record:
// docs/render/render-occlusion-re.md (grilled 2026-07-16 against Jointops.exe).
//
// Coordinate spaces: the original runs every float test in the RENDER FLOAT world
//   (X, Y, Z) = (-mission_y, mission_z, mission_x) / 65536
// [orig: Math_FixedPointToFloat3_YNegated @ 0x611210]. That mapping has det = -1
// versus mission axes, so cross-product handedness differs from mission space —
// the authored OFAC winding bits and every wedge cross order are calibrated to
// this frame. The port therefore works natively in it; hosts convert at the
// boundary. Model-space data (OVRT verts, OPLN planes, record positions) is the
// 3DI Y-up float model space the entity matrix maps from, fed through unchanged.
//
// The occlusion model data is host-fed PODs from the parsed .3di occlusion IR
// (the same leaf-consumer seam as CollisionModel, ADR 0020): libs/world never
// touches the format stack.
#ifndef OPENNOVA_WORLD_OCCLUSION_H
#define OPENNOVA_WORLD_OCCLUSION_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "world/collision.h"
#include "world/entity.h"

namespace opennova::world {

class World;

// ----------------------------------------------------------------------------
// Occlusion model (per graphic) — the "GPM Occ" arena records.
// [orig: load_occlusion_model_data @ 0x5b4a00 — OVRT/OPLN copied verbatim, OFAC
// keeps disk field order, OOBJ 36 B disk -> 60 B runtime with per-object slice
// pointers advancing sequentially. Runtime model +0xDC = count, +0xE0 = array.]
// ----------------------------------------------------------------------------

struct OcclusionVertex {
    float p[3] = {}; // 3DI float model space (Y-up)
};

struct OcclusionPlane {
    float normal[3] = {};
    float d = 0.0f;
};

// [orig: 12 B OFAC record — vertex index bytes @+0..2, plane index byte @+3,
// 3 edge words @+4..9. Edge word: bits 0..7 = vertex A, 8..14 = vertex B,
// bit 15 = winding; shared-edge cancellation identity = the low 15 bits.]
struct OcclusionFaceRec {
    uint8_t v[3] = {};
    uint8_t plane = 0;
    uint16_t edge[3] = {};
};

// Portal-face record types. [orig: the 60 B record type byte]
inline constexpr uint8_t kOccRecOccluder = 0;   // occluder + window-glow slot
inline constexpr uint8_t kOccRecOpen = 1;       // "open" occluder slot (doors)
inline constexpr uint8_t kOccRecWindow = 2;     // exterior window portal
inline constexpr uint8_t kOccRecPortal = 3;     // interior portal (room-to-room)
inline constexpr uint8_t kOccRecWeldedLink = 5; // cross-building link (weld-made)

// The 60 B runtime portal-face record; slice pointers become run indices into
// the model arrays. [orig: +0 type, +1/+2 section A/B (COBJ section ordinals;
// 0 = exterior), +4 pos f3, +0x10 radius, +0x14/+0x18 vert count/slice,
// +0x1C/+0x20 plane count/slice, +0x24/+0x28 face count/slice, +0x2C glow.]
struct OcclusionPortalFace {
    uint8_t type = 0;
    uint8_t section_a = 0;
    uint8_t section_b = 0;
    float pos[3] = {};   // model space
    float radius = 0.0f; // model space
    int32_t vert_start = 0, vert_count = 0;
    int32_t plane_start = 0, plane_count = 0;
    int32_t face_start = 0, face_count = 0;
    float glow_scale = 0.0f; // [orig: disk +20 -> runtime +0x2C]
};

struct OcclusionModel {
    std::vector<OcclusionPortalFace> records;
    std::vector<OcclusionVertex> vertices;
    std::vector<OcclusionPlane> planes;
    std::vector<OcclusionFaceRec> faces;
    bool valid() const { return !records.empty(); }
};

// ----------------------------------------------------------------------------
// Render-float math — the fixed->float pipeline every occlusion test runs on.
// Row-vector convention: p' = p * M, translation in m[12..14].
// ----------------------------------------------------------------------------

struct RenderMatrix {
    float m[16] = {}; // row-major storage, row-vector application

    // out = p * M (with translation). [orig: Math_TransformPointByMatrix4x4 @ 0x40cf20]
    void transform_point(const float p[3], float out[3]) const;
    // out = v * R (rotation rows only). [orig: Math_TransformVectorByMatrix3x3 @ 0x410f40]
    void rotate_vector(const float v[3], float out[3]) const;
};

// out = a * b (row-major 4x4). [orig: Math_MultiplyMatrix4x4_Float @ 0x611750]
RenderMatrix render_matrix_multiply(const RenderMatrix &a, const RenderMatrix &b);

// Mission fixed (x, y, z-up 16.16) -> render float world.
// [orig: Math_FixedPointToFloat3_YNegated @ 0x611210: (-y, z, x) / 65536]
void render_float_from_fixed(const int32_t p[3], float out[3]);

// Entity pose (fixed pos + BAM32 yaw/pitch/roll) -> render float 4x4, per-angle
// Q22-quantized trig, roll * pitch * yaw composition, (-y, z, x)/65536 translation.
// Angles of exactly 0 skip their factor like the original.
// [orig: Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200]
RenderMatrix render_matrix_from_pose(const int32_t pos[3], int32_t yaw_bam, int32_t pitch_bam,
                                     int32_t roll_bam);

// Live entity -> the same full retail render pose. Mission yaw uses the
// canonical heading conversion; authored pitch/roll are direct wrapped BAMs.
// [orig: the entity+4 call sites of Math_BuildFixedPointToFloatMatrix4x4]
RenderMatrix render_matrix_from_entity_pose(const Entity &entity);

// ----------------------------------------------------------------------------
// Per-frame camera input (host-fed). The original reads these from the render
// globals; the host derives them from its camera each frame.
// ----------------------------------------------------------------------------
struct OcclusionFrameCamera {
    int32_t pos_fixed[3] = {}; // mission fixed [orig: position @ 0xA78364]
    float pos_float[3] = {};   // render float world [orig: flt_27219C0]
    // View frustum planes in render float world, inward-facing normals
    // ({nx,ny,nz,d}: inside = dot + d >= 0), near + 4 sides.
    // [orig: g_CameraFrustumPlanes5 @ 0xA7849C — host-built from its camera;
    // D-OCC-12 host mapping]
    float frustum[5][4] = {};
    int32_t frustum_count = 5;
    // World->view rotation rows, Q22 fixed. Row 0 = view forward (the depth
    // axis the batch cull tests), rows 1/2 = the lateral axes the three-ray
    // probe offsets along. [orig: the fixed view matrix @ 0xA7841C]
    int32_t view_rows_q22[3][3] = {};
    int32_t fog_dist = 0;         // 16.16 [orig: Env_FogDistCurrent @ 0x26C681C]
    int32_t water_z = 0;          // 16.16 [orig: Env_WaterHeightFixed @ 0x26C6454]
    uint32_t local_blink_flags = 0; // [orig: g_LocalPlayerBlinkFlags @ 0x24C1934]
};

// ----------------------------------------------------------------------------
// OcclusionWorld: the per-mission portal state + the per-frame visibility engine.
// ----------------------------------------------------------------------------
class OcclusionWorld {
public:
    // Def-derived bits the engine reads off the entity's itemDef.
    struct EntityDefBits {
        bool weldable = false;        // [orig: itemDef attrib2 (+0x58) bit 6]
        bool recurse_windows = false; // [orig: itemDef attrib (+0x54) bit 27]
        // Destruction bone-map bases; bits >= these are forced visible in the
        // draw mask. 0 = none (the destruction system is unmodeled, D-COL-2).
        // [orig: itemDef bytes +2193/+2194]
        uint8_t forced_lo = 0;
        uint8_t forced_hi = 0;
    };

    // --- model registry (host-fed, keyed like CollisionWorld) ---
    int32_t add_model(OcclusionModel model);
    const OcclusionModel *model(int32_t id) const;
    void assign_entity(EntityHandle h, int32_t model_id, const EntityDefBits &bits);
    bool has_instance(EntityHandle h) const;

    // --- mission-start portal init ---
    // Register every building's type-2 faces, weld coincident opposite pairs of
    // DIFFERENT buildings into type-5 cross-links, stamp the per-building flag
    // bytes. The register+weld half runs when do_register_weld (the original's
    // arg, structurally nonzero at mission start — D-OCC-4).
    // [orig: Terrain_InitBuildingPortals @ 0x5c7480, tail @ 0x5c5860, from
    // Game_StartMission @ 0x525e11]
    void init_mission(World &world, CollisionWorld &collision, bool do_register_weld = true);

    // --- per-frame ---
    // The collect + occluder-planes + section-mask pipeline, in the original's
    // order (the distance sort between collect and the occluder pass orders
    // retail draw calls only and is not ported — D-OCC-10).
    // [orig: Terrain_CollectVisibleEntities @ 0x5c9160 steps 1-4]
    void build_frame(World &world, CollisionWorld &collision, const OcclusionFrameCamera &cam);

    // Per-entity render gate for non-building entities (the entity collectors'
    // occlusion rules): the blink-hits gate + the outdoors-only three-ray
    // terrain latch (mutates Entity::occlusion_latch). TRUE = render.
    // [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 (statics) /
    // collect_visible_entities_for_terrain @ 0x5c8c60 (pools 0/1) — the gates
    // @ 0x5c7022-0x5c708a and the latch @ 0x5c7125-0x5c7162]
    bool entity_render_visible(World &world, CollisionWorld &collision, Entity &ent,
                               const OcclusionFrameCamera &cam);

    // --- frame results ---
    // Whether the building entered the visible batch this frame (distance +
    // frustum + TOC). Non-batched or TOC-culled buildings do not render.
    bool building_visible(EntityHandle h) const;
    // The building's section mask with the def forced-visible bits applied:
    // bit N = COBJ section (render part) N draws; bit 0 = exterior.
    // [orig: g_BuildingSectionVisMask @ 0x297F250 + the forced-bit merge in
    // Terrain_RenderSectorModels @ 0x5c5d7c-0x5c5da8]
    uint32_t section_mask(EntityHandle h) const;
    // The building carries an open (type-1) portal record in a live slot — the
    // two-pass draw marker. [orig: batch +16 flag consumption @ 0x5c5e17]
    bool building_open_flagged(EntityHandle h) const;

    bool water_visible() const { return water_visible_; }        // [orig: g_BlinkWaterVisible @ 0x29ACE40]
    bool exterior_visible() const { return exterior_visible_; }  // [orig: g_PortalExteriorVisible @ 0x29ACE20]
    bool camera_indoors() const { return camera_indoors_; }

    // --- introspection (tests + debug hosts) ---
    struct WeldRecord { // [orig: the 24 B g_PortalWeldRecords @ 0x2967250 rows]
        EntityHandle own_entity;
        int32_t own_section = 0;
        int32_t own_face = 0;
        EntityHandle other_entity;
        int32_t other_face = 0;
        int32_t other_section = 0;
    };
    const std::vector<WeldRecord> &weld_records() const { return welds_; }
    // The per-building flag bytes stamped at init. [orig: entity +0x2CC..+0x2CE]
    struct BuildingFlags {
        bool has_open = false;    // +0x2CC (type-1 present; unread in retail, D-OCC-2)
        bool has_windows = false; // +0x2CD (type-2 present; the outdoor mask rule input)
        bool has_links = false;   // +0x2CE (type-5 present; unread in retail, D-OCC-2)
    };
    BuildingFlags building_flags(EntityHandle h) const;
    // The instance's occlusion model id (-1 when the entity carries none), and
    // the batch-membership split building_visible() collapses: batched = entered
    // the frame's visible batch BEFORE render_TOC, so batched && !visible =
    // TOC-culled. Debug-host reads (the F3 occlusion view); no engine consumer.
    int32_t instance_model_id(EntityHandle h) const;
    bool building_batched(EntityHandle h) const;
    int32_t instance_count() const { return static_cast<int32_t>(instances_.size()); }
    int32_t batch_count() const { return static_cast<int32_t>(batch_.size()); }
    int32_t slot_count() const { return static_cast<int32_t>(slots_.size()); }
    int32_t window_frustum_group_count() const { return static_cast<int32_t>(window_groups_.size()); }
    int32_t viewthru_group_count() const { return static_cast<int32_t>(viewthru_groups_.size()); }

private:
    struct Instance {
        int32_t model_id = -1;
        EntityDefBits def_bits;
        // Mission-init flag bytes [orig: entity +0x2CC/+0x2CD/+0x2CE]
        bool has_open = false, has_windows = false, has_links = false;
    };

    // NOTE: the weld pass rewrites record TYPE bytes in the model's record array,
    // which is SHARED by every entity of the same graphic — welding one instance
    // retypes its twins' faces too (whose weld-record lookups then miss, making
    // those faces dead portals). Faithful to the original's shared "GPM Occ"
    // arena mutation [orig: @ 0x5c5aab/0x5c5abb]; models are host-re-added per
    // mission like the retail per-mission model reload.
    struct RegistryEntry { // [orig: the 44 B g_PortalFaceRegistry @ 0x29BEED8 rows]
        EntityHandle entity;
        int32_t model_id = -1;
        int32_t face_index = 0;
        float world_pos[3] = {};
        float world_normal[3] = {};
        bool weldable = false; // bit 0 of +40
    };

    struct Slot { // [orig: the 20 B g_PortalSlots @ 0x2983E88 rows]
        EntityHandle entity;
        int32_t record_index = 0;
        int32_t glow = 0;          // [orig: slot +8 — window-glow renderer input, unconsumed here]
        int32_t viewthru_start = 0; // run into viewthru_groups_ [orig: slot +12 ref]
        int32_t viewthru_count = 0; // [orig: slot +16]
    };

    struct BatchEntry { // [orig: the 20 B g_VisibleBuildingBatch @ 0x2985C90 rows]
        EntityHandle entity;
        bool open_flag = false; // +16: ANY record with type >= 1 (not only type 1)
        bool culled = false;    // render_TOC zeroed the entry
        // Set at batch push and NEVER zeroed: the introspection key that keeps
        // identifying the row after render_TOC faithfully zeroes `entity`.
        // Engine legs read only `entity`; building_batched() reads this.
        EntityHandle debug_entity;
    };

    struct PlaneGroup {
        int32_t start = 0; // run into the owning wedge-plane bank
        int32_t count = 0;
    };

    // The per-seed traversal context [orig: the per-seed g_PortalCtx* fields
    // @ 0x29ACDEC-0x29ACE00; the cross-seed camera-inside / bank-viewthru modes
    // live on the class like their globals @ 0x29ACE10/0x29ACE18].
    struct TraverseCtx {
        const OcclusionModel *model = nullptr;
        RenderMatrix entity_matrix;
        float camera[3] = {};
        int32_t depth = 0;
        uint32_t section_mask = 0;
        int32_t camera_section = 0;
        bool exterior_seed = false;
        bool recurse_windows = false;
        EntityHandle entity;
    };

    const Instance *instance(EntityHandle h) const;
    Instance *instance(EntityHandle h);

    // [orig: Terrain_RegisterExteriorPortalFaces @ 0x5c5b80]
    void register_exterior_faces(World &world, CollisionWorld &collision);
    // [orig: Terrain_WeldOppositePortalFaces @ 0x5c5910]
    void weld_opposite_faces();
    // [orig: the flag-stamp tail @ 0x5c5860]
    void stamp_building_flags(World &world, CollisionWorld &collision);

    // [orig: collect_visible_sector_userpoints @ 0x5c6b60] — the building batch
    // + portal-slot collection (main scene: no def/entity flag filters).
    void collect_buildings(World &world, CollisionWorld &collision,
                           const OcclusionFrameCamera &cam);
    // [orig: Terrain_BuildPortalOccluderPlanes @ 0x5c44c0 ->
    // build_clip_planes_from_collision @ 0x5b34e0]
    void build_occluder_planes(World &world, const OcclusionFrameCamera &cam);
    // [orig: build_sector_visibility_masks @ 0x5c8610]
    void build_section_masks(World &world, CollisionWorld &collision,
                             const OcclusionFrameCamera &cam);
    // [orig: render_visibility_portal_traversal @ 0x5c4ae0 — "render_VPT()"]
    void traverse(TraverseCtx &ctx, int32_t current_section, const float (*planes)[4],
                  int32_t plane_count);
    // The two seeders. [orig: Terrain_TraversePortalsFromSection @ 0x5c73d0 /
    // Terrain_TraversePortalsFromExterior @ 0x5c7330]
    uint32_t traverse_from_section(World &world, EntityHandle h, int32_t section,
                                   const OcclusionFrameCamera &cam);
    uint32_t traverse_from_exterior(World &world, EntityHandle h,
                                    const OcclusionFrameCamera &cam);
    // [orig: test_sector_entity_occlusion @ 0x5c4610 — "render_TOC()"; TRUE = occluded]
    bool toc_occluded(World &world, CollisionWorld &collision, BatchEntry &entry,
                      const OcclusionFrameCamera &cam);
    // [orig: Terrain_TestSphereInPlaneGroups @ 0x5c4580]
    bool sphere_in_plane_groups(const float pos[3], float radius,
                                const std::vector<float> &plane_bank,
                                const PlaneGroup *groups, int32_t group_count) const;
    // [orig: terrain_occlusion_check_three_rays @ 0x610ed0; TRUE = some ray clear]
    bool three_rays_clear(const CollisionWorld &collision, const OcclusionFrameCamera &cam,
                          const int32_t target[3], int32_t radius) const;
    // Bound-sphere derivation from the collision model bounds.
    // [orig: Entity_ComputeBoundingSphere @ 0x5c69a0 — center = AABB mid,
    // radius = min(|half|, 0x7FFF0000 as float); the def scale leg is unported
    // (statics carry no live scale)]
    static void bound_sphere_fixed(const CollisionModel &m, int32_t center_local[3],
                                   int32_t &radius);
    // Batch view cull: forward-depth + sphere-vs-frustum stand-in for the
    // original viewport projector (D-OCC-12).
    bool sphere_in_view(const OcclusionFrameCamera &cam, const int32_t center_fixed[3],
                        int32_t radius_fixed) const;

    // [orig: PRNG_Next16_C @ 0x6131b0 — rol4(s + rol11(s)) ^ 1, own stream]
    uint16_t latch_rand16();

    std::vector<OcclusionModel> models_;
    std::unordered_map<uint16_t, Instance> instances_; // key: EntityHandle.packed

    std::vector<RegistryEntry> registry_; // mission-init scratch
    std::vector<WeldRecord> welds_;       // [orig: g_PortalWeldRecords @ 0x2967250]

    // Frame state
    std::vector<BatchEntry> batch_;
    std::vector<Slot> slots_;
    // Per-slot occluder planes [orig: g_PortalOccluder{Edge,Face}Planes/Counts @ 0x2983D74..80]
    std::vector<OcclusionPlane> occluder_edge_planes_;
    std::vector<OcclusionPlane> occluder_face_planes_;
    std::vector<PlaneGroup> occluder_edge_runs_;
    std::vector<PlaneGroup> occluder_face_runs_;
    // Window-frustum / viewthru wedge banks [orig: g_WindowFrustum* / g_Viewthru*]
    std::vector<PlaneGroup> window_groups_;
    std::vector<PlaneGroup> viewthru_groups_;
    std::vector<float> window_group_planes_;  // 4 floats per plane
    std::vector<float> viewthru_group_planes_;  // separate retail arena
    // The weld-link track list [orig: g_PortalLinkTrackList @ 0x29BEE54, cap 32]
    std::vector<int32_t> link_track_;

    // Per-building section masks [orig: g_BuildingSectionVisMask @ 0x297F250,
    // 1200 dwords, indexed by the pool-2 entity index]. Keyed here by the
    // pool-2 handle slot masked into the array size.
    std::vector<uint32_t> masks_;

    // The exterior latch [orig: g_PortalExteriorPlaneSet @ 0x29ACE24 + the plane
    // point/normal @ 0x29ACE28..3C]
    bool exterior_plane_set_ = false;
    float exterior_plane_point_[3] = {};
    float exterior_plane_normal_[3] = {};

    // Cross-seed traversal modes [orig: g_PortalCtxCameraInsideMode @ 0x29ACE10 /
    // g_PortalCtxBankViewthru @ 0x29ACE18 — set by the seeders/mask builder,
    // read by the traversal dispatch and render_TOC's early rule]
    bool camera_inside_mode_ = false;
    bool bank_viewthru_ = false;

    bool exterior_visible_ = false;
    bool water_visible_ = false;
    bool camera_indoors_ = false;

    uint32_t latch_rng_ = 0; // [orig: dword_31BFBB4 — BSS-zero boot state]
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_OCCLUSION_H
