// Rendering occlusion — faithful port of the blink-box-driven building-section
// visibility engine. Witness record: docs/render/render-occlusion-re.md.
// Every routine cites its original; the pipeline runs in the render float world
// (see occlusion.h header notes on the (-y, z, x)/65536 mapping).

#include <runtime/world/occlusion.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <runtime/renderer/object_lod.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {

namespace {

// The engine's angle scale — NOT exactly 2*pi/2^32: the retail double is
// 0x3E19222D9890E4A8 (~2.1 ppm above), and the quantized trig below feeds off
// it, so we carry the exact bits. [orig: dbl_7C3608]
inline double bam_angle_scale() {
    static const double v = [] {
        const uint64_t bits = 0x3E19222D9890E4A8ull;
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        return d;
    }();
    return v;
}

// _ftol2_sse truncates toward zero (C cast semantics).
inline int32_t ftol(double v) { return static_cast<int32_t>(v); }

inline int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

inline void entity_pos_fixed(const Entity &e, int32_t out[3]) {
    out[0] = to_fixed(e.position.x);
    out[1] = to_fixed(e.position.y);
    out[2] = to_fixed(e.position.z);
}

// The collectors' fixed entity matrix: the full Euler pack at entity+4 (the
// canonical heading plus the authored pitch/roll BAMs), which places the
// entity+0x1FC sphere center before the view transform.
// [orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40, called from the
//  building collector @ 0x5c6c6a, the statics collector @ 0x5c70b1 and the
//  model leg @ 0x5c8f25]
inline CollisionMatrix entity_euler_pose(const Entity &e, const int32_t pos[3]) {
    return collision_matrix_from_euler(
        bam_heading_from_mission_yaw_deg(static_cast<double>(e.yaw)),
        bam_from_degrees_wrapped(static_cast<double>(e.pitch)),
        bam_from_degrees_wrapped(static_cast<double>(e.roll)), pos);
}

// Per-axis Q22-quantized trig factor: c/s = float(ftol(trig(th) * 2^22)) / 2^22,
// with the sin quantization constant NEGATIVE (the sign is baked into it).
// [orig: Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200 — dbl_7C3600 = 2^22,
// dbl_7C57B0 = -2^22, flt_7C3610 = 1/2^22]
inline void quantized_trig(int32_t bam, float &c, float &s) {
    const double th = static_cast<double>(bam) * bam_angle_scale();
    c = static_cast<float>(ftol(std::cos(th) * io::kQ22One)) * (1.0f / 4194304.0f);
    s = static_cast<float>(ftol(std::sin(th) * -io::kQ22One)) * (1.0f / 4194304.0f);
}

constexpr int32_t kSlotCap = 128;        // [orig: the 128-cap slot arrays @ 0x5c6d7f]
constexpr int32_t kSortedSlotCap = 14;   // [orig: `cmp eax,0Eh` @ 0x5c449f]
constexpr int32_t kLinkTrackCap = 32;    // [orig: g_PortalLinkTrackList cap @ 0x5c54fb]
constexpr int32_t kWedgePlaneCap = 64;   // [orig: "render_VPT() : too many planes" @ 0x5c5477]
constexpr int32_t kGroupCap = 512;       // [orig: window/viewthru group caps]
constexpr int32_t kGroupPlaneCap = 2048; // [orig: each plane-arena cap]
constexpr int32_t kMaskSlots = 0x1000;   // pool-2 handle-slot space (retail array = 1200)
// [orig: the 0xFA0000 slot range @ 0x5c6d7f] — the exported world-unit radius
// lives in occlusion.h (kPortalSlotCollectRadius).
constexpr int32_t kSlotDistFixed = static_cast<int32_t>(kPortalSlotCollectRadius) << 16;

} // namespace

// ----------------------------------------------------------------------------
// Render-float math
// ----------------------------------------------------------------------------

// [orig: Math_TransformPointByMatrix4x4 @ 0x40cf20 — row-vector p' = p * M]
void RenderMatrix::transform_point(const float p[3], float out[3]) const {
    out[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    out[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    out[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
}

// [orig: Math_TransformVectorByMatrix3x3 @ 0x410f40]
void RenderMatrix::rotate_vector(const float v[3], float out[3]) const {
    out[0] = v[0] * m[0] + v[1] * m[4] + v[2] * m[8];
    out[1] = v[0] * m[1] + v[1] * m[5] + v[2] * m[9];
    out[2] = v[0] * m[2] + v[1] * m[6] + v[2] * m[10];
}

// [orig: Math_MultiplyMatrix4x4_Float @ 0x611750]
RenderMatrix render_matrix_multiply(const RenderMatrix &a, const RenderMatrix &b) {
    RenderMatrix r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[4 * i + j] = a.m[4 * i + 0] * b.m[0 + j] + a.m[4 * i + 1] * b.m[4 + j] +
                             a.m[4 * i + 2] * b.m[8 + j] + a.m[4 * i + 3] * b.m[12 + j];
    return r;
}

// [orig: Math_FixedPointToFloat3_YNegated @ 0x611210]
void render_float_from_fixed(const int32_t p[3], float out[3]) {
    out[0] = static_cast<float>(-p[1]) * (io::kInvFp16One);
    out[1] = static_cast<float>(p[2]) * (io::kInvFp16One);
    out[2] = static_cast<float>(p[0]) * (io::kInvFp16One);
}

// [orig: Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200 — roll (float Z), then
// pitch (float X), then yaw (float Y), each skipped when the BAM is exactly 0;
// translation (-y, z, x)/65536]
RenderMatrix render_matrix_from_pose(const int32_t pos[3], int32_t yaw_bam, int32_t pitch_bam,
                                     int32_t roll_bam) {
    RenderMatrix acc;
    acc.m[0] = acc.m[5] = acc.m[10] = acc.m[15] = 1.0f;
    if (roll_bam != 0) {
        float c, s;
        quantized_trig(roll_bam, c, s);
        RenderMatrix rz{};
        rz.m[0] = c;
        rz.m[1] = s;
        rz.m[4] = -s;
        rz.m[5] = c;
        rz.m[10] = 1.0f;
        rz.m[15] = 1.0f;
        acc = render_matrix_multiply(acc, rz);
    }
    if (pitch_bam != 0) {
        float c, s;
        quantized_trig(pitch_bam, c, s);
        RenderMatrix rx{};
        rx.m[0] = 1.0f;
        rx.m[5] = c;
        rx.m[6] = s;
        rx.m[9] = -s;
        rx.m[10] = c;
        rx.m[15] = 1.0f;
        acc = render_matrix_multiply(acc, rx);
    }
    if (yaw_bam != 0) {
        float c, s;
        quantized_trig(yaw_bam, c, s);
        RenderMatrix ry{};
        ry.m[0] = c;
        ry.m[2] = -s;
        ry.m[5] = 1.0f;
        ry.m[8] = s;
        ry.m[10] = c;
        ry.m[15] = 1.0f;
        acc = render_matrix_multiply(acc, ry);
    }
    RenderMatrix t{};
    t.m[0] = t.m[5] = t.m[10] = t.m[15] = 1.0f;
    t.m[12] = static_cast<float>(-pos[1]) * (io::kInvFp16One);
    t.m[13] = static_cast<float>(pos[2]) * (io::kInvFp16One);
    t.m[14] = static_cast<float>(pos[0]) * (io::kInvFp16One);
    return render_matrix_multiply(acc, t);
}

// The float-space occlusion consumers use the full authored entity pose. The
// collision path builds its fixed full-Euler entity matrix separately, then
// applies any live PANM section callbacks at query time.
// [orig: Math_BuildFixedPointToFloatMatrix4x4(entity+4)]
RenderMatrix render_matrix_from_entity_pose(const Entity &e) {
    int32_t pos[3];
    entity_pos_fixed(e, pos);
    const int32_t heading = bam_heading_from_mission_yaw_deg(static_cast<double>(e.yaw));
    const int32_t pitch = bam_from_degrees_wrapped(static_cast<double>(e.pitch));
    const int32_t roll = bam_from_degrees_wrapped(static_cast<double>(e.roll));
    return render_matrix_from_pose(pos, heading, pitch, roll);
}

// ----------------------------------------------------------------------------
// Registry
// ----------------------------------------------------------------------------

int32_t OcclusionWorld::add_model(OcclusionModel model) {
    models_.push_back(std::move(model));
    return static_cast<int32_t>(models_.size()) - 1;
}

const OcclusionModel *OcclusionWorld::model(int32_t id) const {
    if (id < 0 || id >= static_cast<int32_t>(models_.size())) return nullptr;
    return &models_[id];
}

void OcclusionWorld::assign_entity(EntityHandle h, int32_t model_id, const EntityDefBits &bits) {
    if (!h.valid() || model_id < 0 || model_id >= static_cast<int32_t>(models_.size())) return;
    Instance inst;
    inst.model_id = model_id;
    inst.def_bits = bits;
    instances_[h.packed] = inst;
}

void OcclusionWorld::remove_entity_instance(EntityHandle h) {
    if (!h.valid()) return;
    instances_.erase(h.packed);
    forced_sections_.erase(h.packed);
}

void OcclusionWorld::assign_forced_sections(EntityHandle h, uint8_t first_lo,
                                            uint8_t first_hi) {
    if (!h.valid()) return;
    if (first_lo == 0 && first_hi == 0) {
        forced_sections_.erase(h.packed);
        return;
    }
    forced_sections_[h.packed] = {first_lo, first_hi};
}

bool OcclusionWorld::has_instance(EntityHandle h) const {
    return instances_.count(h.packed) != 0;
}

const OcclusionWorld::Instance *OcclusionWorld::instance(EntityHandle h) const {
    auto it = instances_.find(h.packed);
    return it == instances_.end() ? nullptr : &it->second;
}

OcclusionWorld::Instance *OcclusionWorld::instance(EntityHandle h) {
    auto it = instances_.find(h.packed);
    return it == instances_.end() ? nullptr : &it->second;
}

// ----------------------------------------------------------------------------
// Mission-start portal init
// ----------------------------------------------------------------------------

// [orig: Terrain_InitBuildingPortals @ 0x5c7480 — arg != 0 runs register + weld
// (the sole call site @ 0x525e11 pushes Game_StartMission's argument == 0:
// nonzero on the first start, zero on Game_RestartRoundSP's round restart,
// D-OCC-4); the flag-stamp tail @ 0x5c5860 always runs.]
void OcclusionWorld::init_mission(World &world, CollisionWorld &collision,
                                  bool do_register_weld) {
    if (do_register_weld) {
        register_exterior_faces(world, collision);
        weld_opposite_faces();
    }
    stamp_building_flags(collision);
}

// [orig: Terrain_RegisterExteriorPortalFaces @ 0x5c5b80 — walks the static
// building prefix; per type-2 record appends a 44 B registry entry: entity,
// model, record array, face index, world position (entity 4x4 * record.pos),
// world plane normal (rot3x3 * OPLN[face0.planeIdx]), +40 bit 0 = itemDef
// attrib2 bit 6 "weldable".]
void OcclusionWorld::register_exterior_faces(World &world, CollisionWorld &collision) {
    registry_.clear();
    // Mission portal init: the pose memo's model pointers must never survive
    // into a reloaded mission's allocations.
    static_pose_memo_.clear();
    const int32_t buildings = collision.static_building_count();
    for (int32_t i = 0; i < buildings; ++i) {
        const CollisionWorld::StaticSlotView slot = collision.static_slot(i);
        const Entity *e = world.registry.get(slot.h);
        if (e == nullptr) continue;
        const Instance *inst = instance(slot.h);
        if (inst == nullptr) continue;
        const OcclusionModel *m = model(inst->model_id);
        if (m == nullptr || m->records.empty()) continue;
        const RenderMatrix world_mat = render_matrix_from_entity_pose(*e);
        for (int32_t r = 0; r < static_cast<int32_t>(m->records.size()); ++r) {
            const OcclusionPortalFace &rec = m->records[r];
            if (rec.type != kOccRecWindow) continue;
            if (rec.face_count <= 0) continue;
            RegistryEntry entry;
            entry.entity = slot.h;
            entry.model_id = inst->model_id;
            entry.face_index = r;
            world_mat.transform_point(rec.pos, entry.world_pos);
            const OcclusionFaceRec &f0 = m->faces[rec.face_start];
            const OcclusionPlane &pl = m->planes[rec.plane_start + f0.plane];
            world_mat.rotate_vector(pl.normal, entry.world_normal);
            entry.weldable = inst->def_bits.weldable;
            registry_.push_back(entry);
        }
    }
}

// [orig: Terrain_WeldOppositePortalFaces @ 0x5c5910 — pairs entries of
// DIFFERENT entities with distance <= 0.80000001, world-normal dot <= -0.89999998,
// coplanarity <= 0.2, >= 1 side weldable; two mirrored 24 B weld records; both
// 60 B records' type byte rewritten to 5 (in the SHARED model array); both
// registry entries removed swap-with-last, the outer slot re-tested.]
void OcclusionWorld::weld_opposite_faces() {
    welds_.clear();
    int32_t count = static_cast<int32_t>(registry_.size());
    for (int32_t i = 0; i < count; ++i) {
        int32_t match = -1;
        for (int32_t j = i + 1; j < count; ++j) {
            const RegistryEntry &a = registry_[i];
            const RegistryEntry &b = registry_[j];
            if (a.entity == b.entity) continue;
            if (!a.weldable && !b.weldable) continue;
            const float dx = a.world_pos[0] - b.world_pos[0];
            const float dy = a.world_pos[1] - b.world_pos[1];
            const float dz = a.world_pos[2] - b.world_pos[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 0.80000001f) continue;
            const float ndot = a.world_normal[0] * b.world_normal[0] +
                               a.world_normal[1] * b.world_normal[1] +
                               a.world_normal[2] * b.world_normal[2];
            if (ndot > -0.89999998f) continue;
            const float da = a.world_normal[0] * a.world_pos[0] +
                             a.world_normal[1] * a.world_pos[1] +
                             a.world_normal[2] * a.world_pos[2];
            const float db = a.world_normal[0] * b.world_pos[0] +
                             a.world_normal[1] * b.world_pos[1] +
                             a.world_normal[2] * b.world_pos[2];
            if (std::fabs(db - da) > 0.2f) continue;
            match = j;
            break;
        }
        if (match < 0) continue;

        const RegistryEntry a = registry_[i];
        const RegistryEntry b = registry_[match];
        OcclusionModel &ma = models_[a.model_id];
        OcclusionModel &mb = models_[b.model_id];
        OcclusionPortalFace &fa = ma.records[a.face_index];
        OcclusionPortalFace &fb = mb.records[b.face_index];

        WeldRecord wa;
        wa.own_entity = a.entity;
        wa.own_section = fa.section_a; // [orig: the record byte +1, read at weld time]
        wa.own_face = a.face_index;
        wa.other_entity = b.entity;
        wa.other_face = b.face_index;
        wa.other_section = fb.section_a;
        welds_.push_back(wa);

        WeldRecord wb;
        wb.own_entity = b.entity;
        wb.own_section = fb.section_a;
        wb.own_face = b.face_index;
        wb.other_entity = a.entity;
        wb.other_face = a.face_index;
        wb.other_section = fa.section_a;
        welds_.push_back(wb);

        fa.type = kOccRecWeldedLink; // [orig: @ 0x5c5aab]
        fb.type = kOccRecWeldedLink; // [orig: @ 0x5c5abb]

        // Swap-with-last removal: inner first, then outer; outer re-tested.
        // Both refills read the CURRENT last valid entry — the second one is
        // count-1 after the first decrement (retail's registry[new_count] with
        // new_count = count-2), not the stale just-moved slot. [orig:
        // @ 0x5c5ac9-0x5c5b1f]
        if (match != count - 1) registry_[match] = registry_[count - 1];
        --count;
        if (i != count - 1) registry_[i] = registry_[count - 1];
        --count;
        registry_.resize(count);
        --i;
    }
}

// [orig: the Terrain_InitBuildingPortals tail @ 0x5c5860-0x5c58f5 — per building
// with occlusion records: zero +0x2CC..+0x2CF, then per record type==2 ->
// +0x2CD, type==5 -> +0x2CE, type==1 -> +0x2CC. Runs AFTER the weld so type-5
// rewrites are seen.]
void OcclusionWorld::stamp_building_flags(CollisionWorld &collision) {
    const int32_t buildings = collision.static_building_count();
    for (int32_t i = 0; i < buildings; ++i) {
        const CollisionWorld::StaticSlotView slot = collision.static_slot(i);
        Instance *inst = instance(slot.h);
        if (inst == nullptr) continue;
        const OcclusionModel *m = model(inst->model_id);
        if (m == nullptr || m->records.empty()) continue;
        inst->has_open = inst->has_windows = inst->has_links = false;
        for (const OcclusionPortalFace &rec : m->records) {
            if (rec.type == kOccRecWindow) inst->has_windows = true;
            if (rec.type == kOccRecWeldedLink) inst->has_links = true;
            if (rec.type == kOccRecOpen) inst->has_open = true;
        }
    }
}

// ----------------------------------------------------------------------------
// Bound sphere / view cull helpers
// ----------------------------------------------------------------------------

// [orig: Entity_ComputeBoundingSphere @ 0x5c69a0 — per axis the collision-
// header min is capped at 0x40000000 and the max floored at -0x40000000
// (@ 0x5c6a02..0x5c6a39), center = min + ((max - min) >> 1) (@ 0x5c6a45/
// 0x5c6a53/0x5c6a67), the radius components are max - center (@ 0x5c6a57/
// 0x5c6a6d/0x5c6a7d: the positive-side half, one word larger on an odd
// width), fsqrt, clamp to 0x7FFF0000 as float, ftol (@ 0x5c6ab8); then a
// nonzero anim/def scale multiplies the three center words and the radius
// with the +0x8000 rule (@ 0x5c6ac8..0x5c6b52).]
void OcclusionWorld::bound_sphere_fixed(const CollisionModel &m, int32_t center_local[3],
                                        int32_t &radius, int32_t scale_q16) {
    int32_t half[3];
    for (int axis = 0; axis < 3; ++axis) {
        const int32_t low = m.min[axis] < 0x40000000 ? m.min[axis] : 0x40000000;
        const int32_t high = m.max[axis] > -0x40000000 ? m.max[axis] : -0x40000000;
        const int32_t width = static_cast<int32_t>(static_cast<uint32_t>(high) -
                                                   static_cast<uint32_t>(low));
        center_local[axis] = static_cast<int32_t>(static_cast<uint32_t>(low) +
                                                  static_cast<uint32_t>(width >> 1));
        half[axis] = static_cast<int32_t>(static_cast<uint32_t>(high) -
                                          static_cast<uint32_t>(center_local[axis]));
    }
    const double len = std::sqrt(static_cast<double>(half[0]) * half[0] +
                                 static_cast<double>(half[1]) * half[1] +
                                 static_cast<double>(half[2]) * half[2]);
    radius = ftol(std::fmin(len, 2147418112.0));
    if (scale_q16 != 0) {
        for (int axis = 0; axis < 3; ++axis)
            center_local[axis] = retail_q16_mul_rhu(center_local[axis], scale_q16);
        radius = retail_q16_mul_rhu(radius, scale_q16);
    }
}

// The batch view cull: forward-depth against the fog distance, then the bound
// sphere against the host frustum planes. [orig: the |viewX| < radius + fogDist
// precull @ 0x5c6caf + Viewport_TransformAndClipPoint @ 0x4115e0 — the host
// planes stand in for the viewport projector, D-OCC-12.]
bool OcclusionWorld::sphere_in_view(const OcclusionFrameCamera &cam,
                                    const int32_t center_fixed[3],
                                    int32_t radius_fixed, int32_t *depth_out) const {
    const int64_t rx = center_fixed[0] - cam.pos_fixed[0];
    const int64_t ry = center_fixed[1] - cam.pos_fixed[1];
    const int64_t rz = center_fixed[2] - cam.pos_fixed[2];
    const int32_t depth = static_cast<int32_t>(
        (rx * cam.view_rows_q22[0][0] + ry * cam.view_rows_q22[0][1] +
         rz * cam.view_rows_q22[0][2] + 0x200000) >> 22);
    if (depth_out != nullptr) *depth_out = depth;
    if (abs32(depth) >= radius_fixed + cam.fog_dist) return false;

    float center[3];
    render_float_from_fixed(center_fixed, center);
    const float radius = static_cast<float>(radius_fixed) * (io::kInvFp16One);
    for (int32_t p = 0; p < cam.frustum_count; ++p) {
        const float d = center[0] * cam.frustum[p][0] + center[1] * cam.frustum[p][1] +
                        center[2] * cam.frustum[p][2] + cam.frustum[p][3];
        if (d < -radius) return false;
    }
    return true;
}

// [orig: PRNG_Next16_C @ 0x6131b0 on the process-wide stream its other
// consumers share: the world's prng16_c_state when bound through
// bind_focal_wind_random, latch_rng_ only when unbound;
// state = rol4(state + rol11(state)); return low16 ^ 1; state ^= 1.]
uint16_t OcclusionWorld::latch_rand16() {
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
	uint32_t &state = shared_latch_rng_ ? *shared_latch_rng_ : latch_rng_;
	const uint32_t rotated = rol(state + rol(state, 11), 4);
	state = rotated ^ 1u;
	return static_cast<uint16_t>((rotated & 0xFFFFu) ^ 1u);
}

// [orig: terrain_occlusion_check_three_rays @ 0x610ed0 — TRUE = some ray clear
// (Terrain_RaycastHeightmapHiRes nonzero = clear = our !los_terrain_blocked).
// Rays run from the camera one unit up (`add edx, 10000h` @ 0x610eef) to the
// bound-sphere top, then the two lateral silhouette points (+- radius along
// view row 1, + radius/2 along view row 2).]
bool OcclusionWorld::three_rays_clear(const CollisionWorld &collision,
                                      const OcclusionFrameCamera &cam,
                                      const int32_t target[3], int32_t radius,
                                      uint32_t debug_tick) const {
    if (collision.terrain == nullptr) return true;
    const int32_t start[3] = {cam.pos_fixed[0], cam.pos_fixed[1], cam.pos_fixed[2] + 0x10000};
    const auto record = [&](const int32_t ray_end[3], bool blocked) {
        collision.ray_debug_record(
                CollisionWorld::RayDebugCategory::kRenderOcclusion, debug_tick,
                start, ray_end, nullptr,
                blocked ? CollisionWorld::kRayDebugBlocked
                        : CollisionWorld::kRayDebugClear);
    };

    int32_t end[3] = {target[0], target[1], target[2] + radius};
    bool blocked = los_terrain_blocked(*collision.terrain, start, end);
    if (collision.ray_debug_enabled()) record(end, blocked);
    if (!blocked) return true;

    auto scaled = [](int32_t range, const int32_t row[3], int32_t out[3]) {
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<int32_t>(
                (static_cast<int64_t>(range) * row[i] + 0x8000) >> 16) >> 6;
    };
    int32_t fwd[3], right[3];
    scaled(radius, cam.view_rows_q22[1], fwd);
    scaled(radius >> 1, cam.view_rows_q22[2], right);

    end[0] = target[0] + right[0] - fwd[0];
    end[1] = target[1] + right[1] - fwd[1];
    end[2] = target[2] + right[2] - fwd[2];
    blocked = los_terrain_blocked(*collision.terrain, start, end);
    if (collision.ray_debug_enabled()) record(end, blocked);
    if (!blocked) return true;

    end[0] += 2 * fwd[0];
    end[1] += 2 * fwd[1];
    end[2] += 2 * fwd[2];
    blocked = los_terrain_blocked(*collision.terrain, start, end);
    if (collision.ray_debug_enabled()) record(end, blocked);
    return !blocked;
}

// ----------------------------------------------------------------------------
// Per-frame: building batch + portal slots
// ----------------------------------------------------------------------------

// [orig: collect_visible_sector_userpoints @ 0x5c6b60, main scene (NULL filter
// args from Terrain_CollectVisibleEntities @ 0x5c9183); the reflection-pass
// variant (def-flag 0x2000000) is not ported — D-OCC-13.]
void OcclusionWorld::collect_buildings(World &world, CollisionWorld &collision,
                                       const OcclusionFrameCamera &cam) {
    batch_.clear();
    slots_.clear();
    // 1 when the local player is outdoors. [orig: (~g_LocalPlayerBlinkFlags & 2) >> 1
    // @ 0x5c6b92]
    const bool outdoors = (~cam.local_blink_flags & 0x2u) != 0;

    const int32_t buildings = collision.static_building_count();
    for (int32_t i = 0; i < buildings; ++i) {
        const CollisionWorld::StaticSlotView slot = collision.static_slot(i);
        // Per-axis XY cull against the fog distance on the quantized prox
        // coords. [orig: @ 0x5c6bcb-0x5c6bff]
        const int32_t limit = (static_cast<int32_t>(slot.radius) << 16) + cam.fog_dist;
        const int32_t dx = abs32(static_slot_coord_q16(slot.x) - cam.pos_fixed[0]);
        if (dx > limit) continue;
        const int32_t dy = abs32(static_slot_coord_q16(slot.y) - cam.pos_fixed[1]);
        if (dy > limit) continue;

        Entity *e = world.registry.get(slot.h);
        if (e == nullptr) continue;
        const CollisionModel *cm = collision.model_for(world, slot.h);
        if (cm == nullptr || !cm->valid()) continue; // [orig: the entity+48 model gate]

        // Bound sphere: local center from the collision bounds, placed through
        // the entity's full Euler pose. [orig: Entity_ComputeBoundingSphere
        // @ 0x5c6c53 + Math_BuildFixedPointMatrixFromEulerAngles(entity+4)
        // @ 0x5c6c6a, the transform @ 0x5c6c80-0x5c6c94]
        // Statics do not move, so the placed sphere memoizes behind its pose/
        // model value keys; a guard miss (husk swap, a mover) recomputes the
        // identical way.
        int32_t epos[3];
        entity_pos_fixed(*e, epos);
        if (static_pose_memo_.size() <= static_cast<size_t>(i)) {
            static_pose_memo_.resize(static_cast<size_t>(buildings));
        }
        StaticPoseMemo &memo = static_pose_memo_[static_cast<size_t>(i)];
        const int32_t inst_model_id = instance_model_id(slot.h);
        const bool pose_hit = static_pose_memo_enabled_ &&
                              memo.handle_packed == slot.h.packed &&
                              memo.cm == cm && memo.model_id == inst_model_id &&
                              memo.yaw == e->yaw &&
                              memo.pitch == e->pitch && memo.roll == e->roll &&
                              memo.pos[0] == epos[0] && memo.pos[1] == epos[1] &&
                              memo.pos[2] == epos[2];
        if (!pose_hit) {
            int32_t center_local[3];
            bound_sphere_fixed(*cm, center_local, memo.radius, e->uniform_scale_q16);
            entity_euler_pose(*e, epos).transform_point(center_local, memo.center_world);
            memo.handle_packed = slot.h.packed;
            memo.cm = cm;
            memo.model_id = inst_model_id;
            memo.yaw = e->yaw;
            memo.pitch = e->pitch;
            memo.roll = e->roll;
            memo.pos[0] = epos[0];
            memo.pos[1] = epos[1];
            memo.pos[2] = epos[2];
            memo.records_valid = false;
        }

        if (!sphere_in_view(cam, memo.center_world, memo.radius)) continue;

        // Three-ray latch — buildings run it too. [orig: @ 0x5c6cd9-0x5c6d0d]
        if (e->occlusion_latch != 0) {
            --e->occlusion_latch;
        } else {
            if (outdoors && !three_rays_clear(collision, cam, memo.center_world,
                                              memo.radius, world.logic_tick))
                continue; // culled this frame; re-probed next
            e->occlusion_latch = static_cast<uint8_t>((latch_rand16() & 7) + 16);
        }

        BatchEntry entry;
        entry.entity = slot.h;
        entry.debug_entity = slot.h;
        batch_.push_back(entry);
        BatchEntry &appended = batch_.back();

        // Portal-slot collection: buildings within 250 u per-axis, 128-cap.
        // [orig: @ 0x5c6d7f-0x5c6ef7]
        if (dx >= kSlotDistFixed || dy >= kSlotDistFixed) continue;
        if (static_cast<int32_t>(slots_.size()) >= kSlotCap) continue;
        const Instance *inst = instance(slot.h);
        if (inst == nullptr) continue;
        const OcclusionModel *m = model(inst->model_id);
        if (m == nullptr || m->records.empty()) continue;
        // The record WORLD positions ride the same memo (pure of pose +
        // model); the type/radius bytes read live each pass — the weld pass
        // rewrites types in place.
        if (!memo.records_valid || memo.model_id != inst->model_id ||
            memo.record_world.size() != m->records.size()) {
            const RenderMatrix world_mat = render_matrix_from_entity_pose(*e);
            memo.record_world.resize(m->records.size());
            for (size_t r = 0; r < m->records.size(); ++r) {
                world_mat.transform_point(m->records[r].pos,
                                          memo.record_world[r].data());
            }
            memo.model_id = inst->model_id;
            memo.records_valid = true;
        }
        for (int32_t r = 0; r < static_cast<int32_t>(m->records.size()); ++r) {
            const OcclusionPortalFace &rec = m->records[r];
            // type 0 collects; ANY type >= 1 sets the batch open flag (the RE
            // record's "has a type-1 record" was imprecise — witnessed
            // @ 0x5c6dcd-0x5c6de1); only type 1 also collects.
            if (rec.type != kOccRecOccluder) {
                appended.open_flag = true;
                if (rec.type != kOccRecOpen) continue;
            }
            const float *rec_world =
                    memo.record_world[static_cast<size_t>(r)].data();
            // Sphere-vs-frustum stand-in for the viewport clip (D-OCC-12).
            // [orig: Viewport_TransformAndClipPoint @ 0x5c6e42, radius * 65536]
            bool in_view = true;
            for (int32_t p = 0; p < cam.frustum_count; ++p) {
                const float d = rec_world[0] * cam.frustum[p][0] +
                                rec_world[1] * cam.frustum[p][1] +
                                rec_world[2] * cam.frustum[p][2] + cam.frustum[p][3];
                if (d < -rec.radius) {
                    in_view = false;
                    break;
                }
            }
            if (!in_view) continue;
            // Angular-size gate: radius^2 / dist^2 must exceed 0.01 (dist <=
            // 10 radii). [orig: the fdiv + flt_7C56A8 compare @ 0x5c6e48-0x5c6e88]
            const float ddx = rec_world[0] - cam.pos_float[0];
            const float ddy = rec_world[1] - cam.pos_float[1];
            const float ddz = rec_world[2] - cam.pos_float[2];
            const float dist_sq = ddx * ddx + ddy * ddy + ddz * ddz;
            const float ratio = (rec.radius * rec.radius) / dist_sq;
            if (0.01f >= ratio) continue;
            Slot s;
            s.entity = slot.h;
            s.record_index = r;
            // [orig: priority = ratio * weight * 2^24 @ 0x5c6ea3-0x5c6eb1]
            s.priority = ftol(static_cast<double>(ratio * rec.slot_priority_scale * 16777216.0f));
            slots_.push_back(s);
            if (static_cast<int32_t>(slots_.size()) >= kSlotCap) break;
        }
    }
}

// ----------------------------------------------------------------------------
// Per-frame: the slot sort
// ----------------------------------------------------------------------------

// [orig: Terrain_SortPortalSlotsByPriority @ 0x5c4410 — count passes of
// adjacent 20-byte swaps whenever slot[j].key < slot[j+1].key (`cmp ecx,
// [eax+1Ch]; jge` @ 0x5c4443): a stable descending order; then
// g_PortalSlotCount = min(count, 14) @ 0x5c449f..0x5c44a4. JO's authored
// weights are all zero, so retail keeps the first 14 in collection order.]
void OcclusionWorld::sort_portal_slots() {
    std::stable_sort(slots_.begin(), slots_.end(),
                     [](const Slot &a, const Slot &b) { return a.priority > b.priority; });
    if (static_cast<int32_t>(slots_.size()) > kSortedSlotCap) slots_.resize(kSortedSlotCap);
}

// ----------------------------------------------------------------------------
// Per-frame: occluder planes
// ----------------------------------------------------------------------------

// [orig: Terrain_BuildPortalOccluderPlanes @ 0x5c44c0 per slot ->
// build_clip_planes_from_collision @ 0x5b34e0 (thunk @ 0x5b3ac0): front/back
// classify the record's faces from the camera; back faces contribute their
// edges to the silhouette (pairwise cancellation); silhouette edges become
// camera-through-edge planes; front faces (deduped BY PLANE INDEX) become world
// face planes.]
void OcclusionWorld::build_occluder_planes(World &world, const OcclusionFrameCamera &cam) {
    occluder_edge_planes_.clear();
    occluder_face_planes_.clear();
    occluder_edge_runs_.assign(slots_.size(), PlaneGroup{});
    occluder_face_runs_.assign(slots_.size(), PlaneGroup{});

    for (size_t si = 0; si < slots_.size(); ++si) {
        const Slot &slot = slots_[si];
        const Entity *e = world.registry.get(slot.entity);
        const Instance *inst = instance(slot.entity);
        if (e == nullptr || inst == nullptr) continue;
        const OcclusionModel *m = model(inst->model_id);
        if (m == nullptr) continue;
        const OcclusionPortalFace &rec = m->records[slot.record_index];
        const RenderMatrix world_mat = render_matrix_from_entity_pose(*e);

        uint16_t edges[128];
        int32_t edge_count = 0;
        uint16_t fronts[128];
        int32_t front_count = 0;

        for (int32_t f = 0; f < rec.face_count; ++f) {
            const OcclusionFaceRec &face = m->faces[rec.face_start + f];
            float v0w[3];
            world_mat.transform_point(m->vertices[rec.vert_start + face.v[0]].p, v0w);
            float nw[3];
            world_mat.rotate_vector(m->planes[rec.plane_start + face.plane].normal, nw);
            const float d = nw[0] * (v0w[0] - cam.pos_float[0]) +
                            nw[1] * (v0w[1] - cam.pos_float[1]) +
                            nw[2] * (v0w[2] - cam.pos_float[2]);
            if (d > 0.0f) {
                // Front face: dedup by the PLANE INDEX low byte. [orig:
                // @ 0x5b36d8-0x5b3719]
                const uint16_t word =
                    static_cast<uint16_t>(face.plane | (static_cast<uint16_t>(face.v[0]) << 8));
                bool seen = false;
                for (int32_t k = 0; k < front_count; ++k) {
                    if ((fronts[k] & 0xFF) == (word & 0xFF)) {
                        seen = true;
                        break;
                    }
                }
                if (!seen && front_count < 128) fronts[front_count++] = word;
            } else {
                // Back face: cancel/append its 3 edge words (low-15 identity,
                // swap-remove). [orig: @ 0x5b3665-0x5b36c4]
                for (int32_t k = 0; k < 3; ++k) {
                    const uint16_t w = face.edge[k];
                    int32_t at = -1;
                    for (int32_t x = 0; x < edge_count; ++x) {
                        if ((edges[x] & 0x7FFF) == (w & 0x7FFF)) {
                            at = x;
                            break;
                        }
                    }
                    if (at >= 0) {
                        edges[at] = edges[--edge_count];
                    } else if (edge_count < 128) {
                        edges[edge_count++] = w;
                    }
                }
            }
        }

        PlaneGroup &edge_run = occluder_edge_runs_[si];
        edge_run.start = static_cast<int32_t>(occluder_edge_planes_.size());
        for (int32_t k = 0; k < edge_count; ++k) {
            const uint16_t w = edges[k];
            float aw[3], bw[3];
            world_mat.transform_point(m->vertices[rec.vert_start + (w & 0xFF)].p, aw);
            world_mat.transform_point(m->vertices[rec.vert_start + ((w >> 8) & 0x7F)].p, bw);
            const float ex = bw[0] - aw[0], ey = bw[1] - aw[1], ez = bw[2] - aw[2];
            const float cx = cam.pos_float[0] - aw[0], cy = cam.pos_float[1] - aw[1],
                        cz = cam.pos_float[2] - aw[2];
            float nx, ny, nz;
            if ((w & 0x8000) != 0) {
                // [orig: winding set -> e x c @ 0x5b3834-0x5b3874]
                nx = cz * ey - cy * ez;
                ny = cx * ez - cz * ex;
                nz = cy * ex - cx * ey;
            } else {
                // [orig: winding clear -> c x e @ 0x5b3884-0x5b38b6]
                nx = cy * ez - cz * ey;
                ny = cz * ex - cx * ez;
                nz = cx * ey - cy * ex;
            }
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len == 0.0f) {
                nx = ny = nz = 0.0f; // [orig: the zero-length keep @ 0x5b38e5]
            } else {
                const float inv = 1.0f / len;
                nx *= inv;
                ny *= inv;
                nz *= inv;
            }
            OcclusionPlane pl;
            pl.normal[0] = nx;
            pl.normal[1] = ny;
            pl.normal[2] = nz;
            pl.d = -(nx * aw[0] + ny * aw[1] + nz * aw[2]);
            occluder_edge_planes_.push_back(pl);
        }
        edge_run.count = edge_count;

        PlaneGroup &face_run = occluder_face_runs_[si];
        face_run.start = static_cast<int32_t>(occluder_face_planes_.size());
        for (int32_t k = 0; k < front_count; ++k) {
            const uint16_t w = fronts[k];
            float nw[3];
            world_mat.rotate_vector(m->planes[rec.plane_start + (w & 0xFF)].normal, nw);
            float pw[3];
            world_mat.transform_point(m->vertices[rec.vert_start + ((w >> 8) & 0xFF)].p, pw);
            OcclusionPlane pl;
            pl.normal[0] = nw[0];
            pl.normal[1] = nw[1];
            pl.normal[2] = nw[2];
            pl.d = -(nw[0] * pw[0] + nw[1] * pw[1] + nw[2] * pw[2]);
            occluder_face_planes_.push_back(pl);
        }
        face_run.count = front_count;
    }
}

// ----------------------------------------------------------------------------
// The portal traversal — render_VPT
// ----------------------------------------------------------------------------

// [orig: render_visibility_portal_traversal @ 0x5c4ae0 — frameless stack args
// (currentSection, frustumPlanes, planeCount) witnessed at the seeder call
// sites @ 0x5c7448/0x5c73a5 and the internal recursions @ 0x5c5619/0x5c57fc.]
void OcclusionWorld::traverse(TraverseCtx &ctx, int32_t current_section,
                              const float (*planes)[4], int32_t plane_count) {
    if (ctx.depth > 10) return; // [orig: @ 0x5c4af4]
    const OcclusionModel &m = *ctx.model;
    for (int32_t ri = 0; ri < static_cast<int32_t>(m.records.size()); ++ri) {
        const OcclusionPortalFace &rec = m.records[ri];
        // Section match. [orig: @ 0x5c4b49-0x5c4b88]
        if (current_section != 0) {
            if (rec.type != kOccRecPortal && rec.type != kOccRecWindow &&
                rec.type != kOccRecWeldedLink)
                continue;
            if (rec.section_b != current_section && rec.section_a != current_section) continue;
        } else {
            if (rec.type != kOccRecWindow) continue;
        }
        if (rec.face_count <= 0 || rec.vert_count <= 0) continue;

        const OcclusionFaceRec &f0 = m.faces[rec.face_start];
        float v0w[3];
        ctx.entity_matrix.transform_point(m.vertices[rec.vert_start + f0.v[0]].p, v0w);
        float nw[3];
        ctx.entity_matrix.rotate_vector(m.planes[rec.plane_start + f0.plane].normal, nw);
        const float face_dot = nw[0] * (v0w[0] - ctx.camera[0]) +
                               nw[1] * (v0w[1] - ctx.camera[1]) +
                               nw[2] * (v0w[2] - ctx.camera[2]);
        float d_test = face_dot;
        if (current_section != 0 && rec.section_a == current_section) d_test = -face_dot;
        if (d_test > 0.0f) {
            // Backfacing. The camera section's own type-2 face latches the
            // exterior. [orig: @ 0x5c5748-0x5c57f2]
            if (rec.type == kOccRecWindow && current_section != 0 &&
                ctx.camera_section == current_section) {
                ctx.section_mask |= 1u;
                exterior_plane_set_ = true;
                exterior_plane_point_[0] = v0w[0];
                exterior_plane_point_[1] = v0w[1];
                exterior_plane_point_[2] = v0w[2];
                exterior_plane_normal_[0] = nw[0];
                exterior_plane_normal_[1] = nw[1];
                exterior_plane_normal_[2] = nw[2];
                if (ctx.recurse_windows && ctx.depth < 1) {
                    // Recurse OUTWARD with the caller's own clip planes.
                    // [orig: the pushes @ 0x5c57fc-0x5c5813]
                    ++ctx.depth;
                    traverse(ctx, 0, planes, plane_count);
                    --ctx.depth;
                }
            }
            continue;
        }

        // Transform the record polygon; frustum-clip it against the caller's
        // planes. [orig: @ 0x5c4c3e-0x5c4e0b]
        float verts[64][3];
        const int32_t vert_count = rec.vert_count > 64 ? 64 : rec.vert_count;
        for (int32_t v = 0; v < vert_count; ++v)
            ctx.entity_matrix.transform_point(m.vertices[rec.vert_start + v].p, verts[v]);
        uint32_t clip_mask = 0;
        bool fully_out = false;
        for (int32_t p = 0; p < plane_count; ++p) {
            int32_t below = 0;
            for (int32_t v = 0; v < vert_count; ++v) {
                const float d = verts[v][0] * planes[p][0] + verts[v][1] * planes[p][1] +
                                verts[v][2] * planes[p][2] + planes[p][3];
                if (d < 0.0f) ++below;
            }
            if (below >= vert_count) {
                fully_out = true;
                break;
            }
            // p can reach 63 on a recursed wedge plane set; & 31 reproduces the
            // x86 masked shift retail executes there instead of C++ UB.
            if (below > 0) clip_mask |= 1u << (p & 31);
        }
        if (fully_out) continue;

        // The expansion. [orig: @ 0x5c4e13-0x5c4e31 — the & 31 is the x86
        // masked shift retail executes for out-of-range authored ordinals]
        int32_t far_section = rec.section_a;
        if (far_section == current_section) far_section = rec.section_b;
        ctx.section_mask |= 1u << (far_section & 31);

        // Boundary edges (low-15 cancellation). [orig: @ 0x5c4e3a-0x5c4ed9]
        uint16_t edges[128];
        int32_t edge_count = 0;
        for (int32_t f = 0; f < rec.face_count; ++f) {
            const OcclusionFaceRec &face = m.faces[rec.face_start + f];
            for (int32_t k = 0; k < 3; ++k) {
                const uint16_t w = face.edge[k];
                int32_t at = -1;
                for (int32_t x = 0; x < edge_count; ++x) {
                    if ((edges[x] & 0x7FFF) == (w & 0x7FFF)) {
                        at = x;
                        break;
                    }
                }
                if (at >= 0) {
                    edges[at] = edges[--edge_count];
                } else if (edge_count < 128) {
                    edges[edge_count++] = w;
                }
            }
        }

        // The wedge: camera-through-edge planes + the partially-clipping caller
        // planes + the oriented face plane. [orig: @ 0x5c4ee3-0x5c5469]
        float wedge[kWedgePlaneCap][4];
        int32_t wedge_count = 0;
        for (int32_t k = 0; k < edge_count && wedge_count < kWedgePlaneCap - 1; ++k) {
            const uint16_t w = edges[k];
            float aw[3], bw[3];
            ctx.entity_matrix.transform_point(m.vertices[rec.vert_start + (w & 0xFF)].p, aw);
            ctx.entity_matrix.transform_point(m.vertices[rec.vert_start + ((w >> 8) & 0x7F)].p,
                                              bw);
            const float ex = bw[0] - aw[0], ey = bw[1] - aw[1], ez = bw[2] - aw[2];
            const float cx = ctx.camera[0] - aw[0], cy = ctx.camera[1] - aw[1],
                        cz = ctx.camera[2] - aw[2];
            const bool winding = (w & 0x8000) != 0;
            float nx, ny, nz;
            if ((face_dot <= 0.0f) == winding) {
                // [orig: c x e @ 0x5c5137-0x5c5181 / 0x5c50e6-0x5c50d5 pairing]
                nx = cy * ez - cz * ey;
                ny = cz * ex - cx * ez;
                nz = cx * ey - cy * ex;
            } else {
                nx = cz * ey - cy * ez;
                ny = cx * ez - cz * ex;
                nz = cy * ex - cx * ey;
            }
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len == 0.0f) {
                nx = ny = nz = 0.0f;
            } else {
                const float inv = 1.0f / len;
                nx *= inv;
                ny *= inv;
                nz *= inv;
            }
            wedge[wedge_count][0] = nx;
            wedge[wedge_count][1] = ny;
            wedge[wedge_count][2] = nz;
            wedge[wedge_count][3] = -(nx * aw[0] + ny * aw[1] + nz * aw[2]);
            ++wedge_count;
        }
        for (int32_t p = 0; p < plane_count && wedge_count < kWedgePlaneCap - 1; ++p) {
            if ((clip_mask & (1u << (p & 31))) == 0) continue;
            wedge[wedge_count][0] = planes[p][0];
            wedge[wedge_count][1] = planes[p][1];
            wedge[wedge_count][2] = planes[p][2];
            wedge[wedge_count][3] = planes[p][3];
            ++wedge_count;
        }
        // The face plane, oriented so the far side is wedge-interior.
        // [orig: @ 0x5c52bd-0x5c5469]
        if (face_dot <= 0.0f) {
            wedge[wedge_count][0] = -nw[0];
            wedge[wedge_count][1] = -nw[1];
            wedge[wedge_count][2] = -nw[2];
            wedge[wedge_count][3] = nw[0] * v0w[0] + nw[1] * v0w[1] + nw[2] * v0w[2];
        } else {
            wedge[wedge_count][0] = nw[0];
            wedge[wedge_count][1] = nw[1];
            wedge[wedge_count][2] = nw[2];
            wedge[wedge_count][3] = -(nw[0] * v0w[0] + nw[1] * v0w[1] + nw[2] * v0w[2]);
        }
        ++wedge_count;
        // [orig: the >= 64 "too many planes" error @ 0x5c5477 — retail prints
        // and keeps writing into adjacent stack; we clamp instead (D-OCC-11).]

        // Dispatch. [orig: @ 0x5c548e-0x5c581b]
        if (current_section != 0 &&
            (rec.type == kOccRecWindow || rec.type == kOccRecWeldedLink)) {
            if (camera_inside_mode_) {
                if (rec.type == kOccRecWeldedLink) {
                    // Track the weld record for the cross-building expansion.
                    // [orig: @ 0x5c54bd-0x5c550b]
                    for (int32_t wi = 0; wi < static_cast<int32_t>(welds_.size()); ++wi) {
                        const WeldRecord &wr = welds_[wi];
                        if (wr.own_entity == ctx.entity && wr.own_face == ri) {
                            if (static_cast<int32_t>(link_track_.size()) < kLinkTrackCap)
                                link_track_.push_back(wi);
                            break;
                        }
                    }
                } else {
                    // Window: latch the exterior + bank the wedge as a window
                    // frustum. [orig: @ 0x5c5528-0x5c55b4]
                    exterior_visible_ = true;
                    if (static_cast<int32_t>(window_groups_.size()) < kGroupCap &&
                        wedge_count + static_cast<int32_t>(window_group_planes_.size()) / 4 <
                            kGroupPlaneCap) {
                        PlaneGroup g;
                        g.start = static_cast<int32_t>(window_group_planes_.size()) / 4;
                        g.count = wedge_count;
                        for (int32_t k = 0; k < wedge_count; ++k)
                            for (int32_t c = 0; c < 4; ++c)
                                window_group_planes_.push_back(wedge[k][c]);
                        window_groups_.push_back(g);
                    }
                }
                // Both legs (the weld track and the window bank) join at the
                // recurse-windows gate, so a def with attrib bit 27 also walks
                // a type-5 link into its far section. [orig: the type-5
                // `jmp loc_5C560C` @ 0x5c5519 / 0x5c54ed, `cmp
                // g_PortalCtxRecurseWindows,0` @ 0x5c560c, the recursion
                // @ 0x5c5619]
                if (ctx.recurse_windows) {
                    ++ctx.depth;
                    traverse(ctx, far_section, wedge, wedge_count);
                    --ctx.depth;
                }
            } else if (rec.type != kOccRecWeldedLink && bank_viewthru_) {
                // Outside-in windows at depth >= 1 bank viewthru wedges. The
                // original banks UNGUARDED and errors after; we guard like the
                // window bank (divergence only in the overflow regime where
                // retail corrupts its own arrays — D-OCC-11).
                // [orig: @ 0x5c565a-0x5c56c9]
                if (static_cast<int32_t>(viewthru_groups_.size()) < kGroupCap &&
                    wedge_count + static_cast<int32_t>(viewthru_group_planes_.size()) / 4 <
                        kGroupPlaneCap) {
                    PlaneGroup g;
                    g.start = static_cast<int32_t>(viewthru_group_planes_.size()) / 4;
                    g.count = wedge_count;
                    for (int32_t k = 0; k < wedge_count; ++k)
                        for (int32_t c = 0; c < 4; ++c)
                            viewthru_group_planes_.push_back(wedge[k][c]);
                    viewthru_groups_.push_back(g);
                }
            }
        } else {
            // Type 3 (and top-level outside-in type 2): recurse through the
            // portal with the wedge. [orig: @ 0x5c5619-0x5c5640]
            ++ctx.depth;
            traverse(ctx, far_section, wedge, wedge_count);
            --ctx.depth;
        }
    }
}

// [orig: Terrain_TraversePortalsFromSection @ 0x5c73d0]
uint32_t OcclusionWorld::traverse_from_section(World &world, EntityHandle h, int32_t section,
                                               const OcclusionFrameCamera &cam) {
    const Entity *e = world.registry.get(h);
    const Instance *inst = instance(h);
    // A blink-carrying building without occlusion records traverses zero
    // records and keeps the seed bit (the camera's own section renders).
    // [orig: render_VPT loops model+0xDC = 0 times]
    if (e == nullptr || inst == nullptr) {
        camera_inside_mode_ = true;
        return 1u << (section & 31);
    }
    const OcclusionModel *m = model(inst->model_id);
    if (m == nullptr) {
        camera_inside_mode_ = true;
        return 1u << (section & 31);
    }
    TraverseCtx ctx;
    ctx.model = m;
    ctx.entity = h;
    ctx.entity_matrix = render_matrix_from_entity_pose(*e);
    ctx.camera[0] = cam.pos_float[0];
    ctx.camera[1] = cam.pos_float[1];
    ctx.camera[2] = cam.pos_float[2];
    ctx.depth = 0;
    ctx.section_mask = 1u << (section & 31);
    ctx.camera_section = section;
    ctx.exterior_seed = false;
    ctx.recurse_windows = inst->def_bits.recurse_windows; // [orig: attrib bit 27 @ 0x5c7456]
    camera_inside_mode_ = true;                           // [orig: @ 0x5c742c]
    traverse(ctx, section, cam.frustum, cam.frustum_count);
    uint32_t mask = ctx.section_mask;
    if (ctx.exterior_seed) mask |= 0x80000000u;
    return mask;
}

// [orig: Terrain_TraversePortalsFromExterior @ 0x5c7330 — mask seeds at 1,
// exterior_seed = 1 so the return always carries the bit-31 marker.]
uint32_t OcclusionWorld::traverse_from_exterior(World &world, EntityHandle h,
                                                const OcclusionFrameCamera &cam) {
    const Entity *e = world.registry.get(h);
    const Instance *inst = instance(h);
    if (e == nullptr || inst == nullptr) {
        camera_inside_mode_ = false;
        return 1u | 0x80000000u;
    }
    const OcclusionModel *m = model(inst->model_id);
    if (m == nullptr) {
        camera_inside_mode_ = false;
        return 1u | 0x80000000u;
    }
    TraverseCtx ctx;
    ctx.model = m;
    ctx.entity = h;
    ctx.entity_matrix = render_matrix_from_entity_pose(*e);
    ctx.camera[0] = cam.pos_float[0];
    ctx.camera[1] = cam.pos_float[1];
    ctx.camera[2] = cam.pos_float[2];
    ctx.depth = 0;
    ctx.section_mask = 1u;
    ctx.camera_section = 0;
    ctx.exterior_seed = true;
    ctx.recurse_windows = false;
    camera_inside_mode_ = false; // [orig: @ 0x5c738e]
    traverse(ctx, 0, cam.frustum, cam.frustum_count);
    return ctx.section_mask | 0x80000000u;
}

// ----------------------------------------------------------------------------
// render_TOC — the occluder culling test
// ----------------------------------------------------------------------------

// [orig: Terrain_TestSphereInPlaneGroups @ 0x5c4580 — TRUE when some group has
// every plane at dot + d >= -radius; an EMPTY group returns TRUE (the retail
// early break with inside = 1).]
bool OcclusionWorld::sphere_in_plane_groups(const float pos[3], float radius,
                                            const std::vector<float> &plane_bank,
                                            const PlaneGroup *groups,
                                            int32_t group_count) const {
    for (int32_t g = 0; g < group_count; ++g) {
        if (groups[g].count <= 0) return true;
        bool inside = true;
        const float *pl = plane_bank.data() + 4 * groups[g].start;
        for (int32_t p = 0; p < groups[g].count; ++p, pl += 4) {
            if (pos[0] * pl[0] + pos[1] * pl[1] + pos[2] * pl[2] + pl[3] < -radius) {
                inside = false;
                break;
            }
        }
        if (inside) return true;
    }
    return false;
}

namespace {

// The render_TOC fields of one entity. [orig: test_sector_entity_occlusion
// @ 0x5c4610 — entity+4 @ 0x5c4639, entity+0 @ 0x5c463e..0x5c4640, the
// entity+0x30 -> +0xB0 bounds @ 0x5c4920, the entity+4 pose @ 0x5c497f]
OcclusionWorld::TocCandidate toc_candidate(const Entity &e, const CollisionModel *cm) {
    OcclusionWorld::TocCandidate c;
    c.self = e.handle;
    entity_pos_fixed(e, c.pos_fixed);
    c.radius_q16 = to_fixed(e.bound_radius);
    c.model = cm;
    c.heading_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(e.yaw));
    c.pitch_bam = bam_from_degrees_wrapped(static_cast<double>(e.pitch));
    c.roll_bam = bam_from_degrees_wrapped(static_cast<double>(e.roll));
    return c;
}

} // namespace

// [orig: test_sector_entity_occlusion @ 0x5c4610 — "render_TOC()"; TRUE =
// occluded, and the batch entry is zeroed.]
bool OcclusionWorld::toc_occluded(World &world, CollisionWorld &collision, BatchEntry &entry) {
    const Entity *cand = world.registry.get(entry.entity);
    if (cand == nullptr) return false;
    if (!toc_occludes(toc_candidate(*cand, collision.model_for(world, entry.entity))))
        return false;
    entry.culled = true;
    entry.entity = EntityHandle{};
    return true;
}

// [orig: test_sector_entity_occlusion @ 0x5c4610 — "render_TOC()"; TRUE =
// occluded (every culling return zeroes the caller's list row).]
bool OcclusionWorld::toc_occludes(const TocCandidate &cand) const {
    const CollisionModel *cand_cm = cand.model;
    float pos[3];
    render_float_from_fixed(cand.pos_fixed, pos);
    // The candidate's entity+0 bound radius (the scaled model bound + 1/16,
    // stamped at entity init), fild * 1/65536.
    // [orig: `mov eax,[esi]; fild dword ptr [eax]` @ 0x5c463e..0x5c4640,
    //  `fmul flt_7C3310` @ 0x5c464f]
    const float radius = static_cast<float>(cand.radius_q16) * (io::kInvFp16One);

    // Camera-inside early rule. [orig: @ 0x5c4662-0x5c4721]
    const int32_t window_groups = static_cast<int32_t>(window_groups_.size());
    bool run_slots = false;
    if (window_groups != 0 && !exterior_plane_set_) {
        if (!sphere_in_plane_groups(pos, radius, window_group_planes_, window_groups_.data(),
                                    window_groups)) {
            return true;
        }
        run_slots = true;
    } else {
        if (camera_inside_mode_) {
            if (!exterior_plane_set_) {
                return true;
            }
        } else if (!exterior_plane_set_) {
            run_slots = true;
        }
        if (!run_slots) {
            // exterior_plane_set_: the latched-window half-space test.
            const float d = (exterior_plane_point_[0] - pos[0]) * exterior_plane_normal_[0] +
                            (exterior_plane_point_[1] - pos[1]) * exterior_plane_normal_[1] +
                            (exterior_plane_point_[2] - pos[2]) * exterior_plane_normal_[2];
            if (d > radius) {
                if (window_groups == 0 ||
                    !sphere_in_plane_groups(pos, radius, window_group_planes_,
                                            window_groups_.data(), window_groups)) {
                    return true;
                }
            }
        }
    }

    // The slot occluder loop. [orig: @ 0x5c4727-0x5c4ab9]
    for (size_t si = 0; si < slots_.size(); ++si) {
        const Slot &slot = slots_[si];
        if (slot.entity == cand.self) continue; // self
        const Instance *oinst = instance(slot.entity);
        if (oinst == nullptr) continue;
        const OcclusionModel *om = model(oinst->model_id);
        if (om == nullptr) continue;
        // A type-1 "open" slot occludes only when its building carries the
        // bit-31 marker. [orig: the signed `dest[idx] < 0` test @ 0x5c4758]
        if (om->records[slot.record_index].type == kOccRecOpen) {
            const uint32_t omask = masks_[slot.entity.slot() & (kMaskSlots - 1)];
            if ((omask & 0x80000000u) == 0) continue;
        }
        const PlaneGroup &edge_run = occluder_edge_runs_[si];
        const PlaneGroup &face_run = occluder_face_runs_[si];

        // 1. Candidate center behind ALL face planes -> this slot cannot
        // occlude. [orig: @ 0x5c4803-0x5c4844]
        if (face_run.count > 0) {
            bool in_front_of_any = false;
            for (int32_t p = 0; p < face_run.count; ++p) {
                const OcclusionPlane &pl = occluder_face_planes_[face_run.start + p];
                if (pos[0] * pl.normal[0] + pos[1] * pl.normal[1] + pos[2] * pl.normal[2] +
                        pl.d > 0.0f) {
                    in_front_of_any = true;
                    break;
                }
            }
            if (!in_front_of_any) continue;
        } else {
            continue; // [orig: num_hs_planes <= 0 skips to the next slot]
        }

        // 2. Sphere vs the silhouette wedge. [orig: @ 0x5c4851-0x5c48a1]
        bool partially_behind = false;
        bool fully_inside = true;
        for (int32_t p = 0; p < edge_run.count; ++p) {
            const OcclusionPlane &pl = occluder_edge_planes_[edge_run.start + p];
            const float d = pos[0] * pl.normal[0] + pos[1] * pl.normal[1] +
                            pos[2] * pl.normal[2] + pl.d;
            if (d < 0.0f) partially_behind = true;
            if (d > -radius) {
                fully_inside = false;
                break;
            }
        }
        if (edge_run.count <= 0) fully_inside = true;

        bool occluded = false;
        if (fully_inside) {
            occluded = true;
        } else if (partially_behind && cand_cm != nullptr && cand_cm->valid()) {
            // 8-corner collision-AABB refinement. [orig: @ 0x5c490e-0x5c4a4d —
            // the corner components swizzle the mission-axis bounds into the
            // 3DI float model frame: X = -y, Y = z, Z = x.]
            const float min_x = static_cast<float>(-cand_cm->min[1]) * (io::kInvFp16One);
            const float max_x = static_cast<float>(-cand_cm->max[1]) * (io::kInvFp16One);
            const float min_y = static_cast<float>(cand_cm->min[2]) * (io::kInvFp16One);
            const float max_y = static_cast<float>(cand_cm->max[2]) * (io::kInvFp16One);
            const float min_z = static_cast<float>(cand_cm->min[0]) * (io::kInvFp16One);
            const float max_z = static_cast<float>(cand_cm->max[0]) * (io::kInvFp16One);
            const RenderMatrix cand_mat = render_matrix_from_pose(
                cand.pos_fixed, cand.heading_bam, cand.pitch_bam, cand.roll_bam);
            bool all_corners_inside = true;
            for (int32_t corner = 0; corner < 8 && all_corners_inside; ++corner) {
                const float local[3] = {(corner & 1) ? min_x : max_x,
                                        (corner & 2) ? min_y : max_y,
                                        (corner & 4) ? min_z : max_z};
                float w[3];
                cand_mat.transform_point(local, w);
                for (int32_t p = 0; p < edge_run.count; ++p) {
                    const OcclusionPlane &pl = occluder_edge_planes_[edge_run.start + p];
                    if (w[0] * pl.normal[0] + w[1] * pl.normal[1] + w[2] * pl.normal[2] +
                            pl.d > 0.0f) {
                        all_corners_inside = false;
                        break;
                    }
                }
            }
            occluded = all_corners_inside;
        }

        if (occluded) {
            // The viewthru rescue. [orig: @ 0x5c48cc / 0x5c4a76]
            if (slot.viewthru_count > 0 &&
                sphere_in_plane_groups(pos, radius, viewthru_group_planes_,
                                       viewthru_groups_.data() + slot.viewthru_start,
                                       slot.viewthru_count))
                continue;
            return true;
        }
    }
    return false; // [orig: ++g_PortalTocVisibleCount and return 0]
}

// ----------------------------------------------------------------------------
// The section-mask build
// ----------------------------------------------------------------------------

// [orig: build_sector_visibility_masks @ 0x5c8610]
// [orig: the indoor walk's open-building leg @ 0x5c8a4e-0x5c8aab; the
//  outdoor twin @ 0x5c871f-0x5c87d1]
void OcclusionWorld::bank_open_building(World &world, EntityHandle entity,
                                        int32_t mask_index,
                                        const OcclusionFrameCamera &cam) {
    const int32_t saved = static_cast<int32_t>(viewthru_groups_.size());
    bank_viewthru_ = true; // [orig: @ 0x5c8a4e]
    masks_[mask_index] |= traverse_from_exterior(world, entity, cam);
    bank_viewthru_ = false;
    // Patch this building's type-1 slots with the banked run.
    // [orig: @ 0x5c8a79-0x5c8aab]
    const int32_t banked = static_cast<int32_t>(viewthru_groups_.size()) - saved;
    for (Slot &s : slots_) {
        const Instance *inst = instance(s.entity);
        const OcclusionModel *m = inst ? model(inst->model_id) : nullptr;
        if (s.entity == entity && m != nullptr &&
            m->records[s.record_index].type == kOccRecOpen) {
            s.viewthru_start = saved;
            s.viewthru_count = banked;
        }
    }
}

void OcclusionWorld::build_section_masks(World &world, CollisionWorld &collision,
                                         const OcclusionFrameCamera &cam) {
    masks_.assign(kMaskSlots, 0u);

    // Pre-pass: type-1 slots mark bit 30 ("has an active open slot").
    // [orig: @ 0x5c8639-0x5c8668]
    for (const Slot &s : slots_) {
        const Instance *inst = instance(s.entity);
        const OcclusionModel *m = inst ? model(inst->model_id) : nullptr;
        if (m != nullptr && m->records[s.record_index].type == kOccRecOpen)
            masks_[s.entity.slot() & (kMaskSlots - 1)] |= 0x40000000u;
    }

    // The camera blink query. [orig: Entity_QueryBlinkBoxesAtPoint @ 0x5c8679]
    BlinkAccum hits;
    collision.query_blink_boxes_at_point(world, cam.pos_fixed, hits);

    // Frame-state reset. [orig: @ 0x5c8687-0x5c86c9]
    camera_inside_mode_ = false;
    exterior_plane_set_ = false;
    window_groups_.clear();
    viewthru_groups_.clear();
    window_group_planes_.clear();
    viewthru_group_planes_.clear();
    bank_viewthru_ = false;
    link_track_.clear();
    water_visible_ = false;

    auto water_straddle = [&](EntityHandle h) {
        // A camera building straddles the water plane: z + the collision
        // header's max z > water AND z + its min z < water.
        // [orig: [model+0xB0]+0x2C / +0x28 @ 0x5c8900..0x5c8922]
        const Entity *e = world.registry.get(h);
        const CollisionModel *cm = collision.model_for(world, h);
        if (e == nullptr || cm == nullptr || !cm->valid()) return false;
        int32_t p[3];
        entity_pos_fixed(*e, p);
        return p[2] + cm->max[2] > cam.water_z && p[2] + cm->min[2] < cam.water_z;
    };
    auto link_water_straddle = [&](EntityHandle h) {
        // A linked building straddles by its bound SPHERE instead: z + the
        // sphere center's z +- the sphere radius (entity+0x204 / +0x208,
        // the center z added unrotated). [orig: @ 0x5c8985..0x5c89a9]
        const Entity *e = world.registry.get(h);
        const CollisionModel *cm = collision.model_for(world, h);
        if (e == nullptr || cm == nullptr || !cm->valid()) return false;
        int32_t center_local[3];
        int32_t radius = 0;
        bound_sphere_fixed(*cm, center_local, radius, e->uniform_scale_q16);
        const int32_t z = to_fixed(e->position.z) + center_local[2];
        return z + radius > cam.water_z && z - radius < cam.water_z;
    };

    // The camera-inside branch tests hit slot 0's WHOLE dword; the per-slot
    // entity resolution below tests the LOW WORD (nonzero section bits) — a
    // section-0 blink hit (packed = pool << 20) enters the inside branch with
    // a null slot entity, exactly like retail. [orig: the `if (hit_results)`
    // branch @ 0x5c86cf vs the (_WORD) slot gates @ 0x5c8840-0x5c88bf]
    const bool camera_inside = hits.hits[0] != 0;
    camera_indoors_ = camera_inside;

    if (camera_inside) {
        exterior_visible_ = false; // [orig: @ 0x5c883a]
        EntityHandle hit_entities[4];
        for (int32_t i = 0; i < 4; ++i) {
            if ((hits.hits[i] & 0xFFFF) != 0)
                hit_entities[i] = EntityHandle::make(2, BlinkAccum::hit_pool_entity_index(hits.hits[i]));
        }
        // Camera-containing buildings: traverse from the camera's section.
        // [orig: @ 0x5c88c9-0x5c8938]
        for (int32_t i = 0; i < hits.hit_count && i < 4; ++i) {
            if (!hit_entities[i].valid()) continue;
            const int32_t section = BlinkAccum::hit_section(hits.hits[i]);
            const int32_t mi = hit_entities[i].slot() & (kMaskSlots - 1);
            masks_[mi] |= traverse_from_section(world, hit_entities[i], section, cam);
            if (water_straddle(hit_entities[i])) water_visible_ = true;
        }
        // Cross-building links found during those traversals. [orig: @ 0x5c893f-0x5c89c1]
        for (size_t li = 0; li < link_track_.size(); ++li) {
            const WeldRecord &wr = welds_[link_track_[li]];
            const int32_t mi = wr.other_entity.slot() & (kMaskSlots - 1);
            masks_[mi] |= traverse_from_section(world, wr.other_entity, wr.other_section, cam);
            if ((masks_[mi] & 0xFFFFFFEu) != 0 && link_water_straddle(wr.other_entity))
                water_visible_ = true;
        }
        // Every other visible building: TOC, then outside-in or full.
        // [orig: @ 0x5c89c7-0x5c8acf]
        for (BatchEntry &b : batch_) {
            if (!b.entity.valid()) continue;
            bool is_hit = false;
            for (int32_t i = 0; i < 4; ++i)
                if (b.entity == hit_entities[i]) is_hit = true;
            if (is_hit) continue;
            const int32_t mi = b.entity.slot() & (kMaskSlots - 1);
            if ((masks_[mi] & 0xFFFFFFFu) != 0) continue;
            if (toc_occluded(world, collision, b)) continue;
            if (b.open_flag) {
                bank_open_building(world, b.entity, mi, cam); // [orig: @ 0x5c8a4e-0x5c8aab]
            } else {
                masks_[mi] = 0xFFFFFFFu; // [orig: @ 0x5c8ab4]
            }
        }
    } else {
        exterior_visible_ = true; // [orig: @ 0x5c86dd]
        // Type-1 slots mark bit 31 ("open slot may occlude") outdoors only.
        // [orig: @ 0x5c86e7-0x5c871b]
        for (const Slot &s : slots_) {
            const Instance *inst = instance(s.entity);
            const OcclusionModel *m = inst ? model(inst->model_id) : nullptr;
            if (m != nullptr && m->records[s.record_index].type == kOccRecOpen)
                masks_[s.entity.slot() & (kMaskSlots - 1)] |= 0x80000000u;
        }
        // Buildings with an active open slot: the outside-in traversal.
        // [orig: @ 0x5c871f-0x5c87d1]
        for (BatchEntry &b : batch_) {
            if (!b.entity.valid()) continue;
            const int32_t mi = b.entity.slot() & (kMaskSlots - 1);
            if ((masks_[mi] & 0x40000000u) == 0) continue;
            bank_open_building(world, b.entity, mi, cam);
        }
        // Everything else: TOC, then exterior-only unless the building has
        // window portals. [orig: @ 0x5c87d7-0x5c8830]
        for (BatchEntry &b : batch_) {
            if (!b.entity.valid()) continue;
            const int32_t mi = b.entity.slot() & (kMaskSlots - 1);
            if (toc_occluded(world, collision, b)) continue;
            if ((masks_[mi] & 0x40000000u) != 0) continue;
            const Instance *inst = instance(b.entity);
            const bool windows = inst != nullptr && inst->has_windows; // [orig: +0x2CD @ 717]
            masks_[mi] = windows ? 0xFFFFFFFu : 1u;
        }
    }

    // [orig: the tail @ 0x5c8ae9-0x5c8aeb]
    if (exterior_visible_ || exterior_plane_set_) water_visible_ = true;
}

// [orig: Terrain_CollectVisibleEntities @ 0x5c9160 steps 1-4 — the count resets,
// the building collect, the slot sort (@ 0x5c9188), the occluder-plane build,
// the mask build; the downstream entity collectors run through
// entity_render_visible.]
void OcclusionWorld::build_frame(World &world, CollisionWorld &collision,
                                 const OcclusionFrameCamera &cam) {
    collect_buildings(world, collision, cam);
    sort_portal_slots();
    build_occluder_planes(world, cam);
    build_section_masks(world, collision, cam);
}

// ----------------------------------------------------------------------------
// The entity render gates
// ----------------------------------------------------------------------------

// [orig: the shared per-entity occlusion rules of Terrain_CollectVisibleEntities_0
// @ 0x5c6f20 and collect_visible_entities_for_terrain @ 0x5c8c60 — the entity
// flag skip, the blink-hits gate, the view cull, and the three-ray latch —
// then the render waves' render_TOC for an entity in no blink box.]
bool OcclusionWorld::entity_render_visible(World &world, CollisionWorld &collision, Entity &ent,
                                           const OcclusionFrameCamera &cam) {
    if ((ent.flags & 1u) != 0) return false; // [orig: @ 0x5c6fec]
    if (!blink_hits_render_active(ent.blink_hits)) return false;

    int32_t epos[3];
    entity_pos_fixed(ent, epos);
    const CollisionModel *cm = collision.model_for(world, ent.handle);
    bool collected = false;
    if (ent.kind == EntityKind::Organic) {
        // Persons (def type 3) project their own position with the entity+0
        // bound radius. [orig: collect_visible_entities_for_terrain — the
        // type-3 branch @ 0x5c8dd7..0x5c8de9, the position @ 0x5c8e18]
        const bool parachute =
            ((ent.flags | ent.engine_flags) & kEntityFlagParachute) != 0;
        collected = person_render_visible(
            collision, cam, epos,
            person_collector_radius(to_fixed(ent.bound_radius), parachute),
            ent.occlusion_latch, world.logic_tick);
    } else {
        // Bound sphere + view cull (also paces the latch like the original —
        // the latch only ticks for view-collected entities).
        int32_t center_world[3];
        int32_t radius = 0x10000;
        if (cm != nullptr && cm->valid()) {
            int32_t center_local[3];
            bound_sphere_fixed(*cm, center_local, radius, ent.uniform_scale_q16);
            entity_euler_pose(ent, epos).transform_point(center_local, center_world);
        } else {
            // No collision instance: a position-centered unit sphere.
            center_world[0] = epos[0];
            center_world[1] = epos[1];
            center_world[2] = epos[2];
        }
        collected = sphere_render_visible(collision, cam, center_world, radius,
                                          ent.occlusion_latch, world.logic_tick);
    }
    if (!collected) return false;
    return !render_wave_toc_occluded(ent.blink_hits, toc_candidate(ent, cm));
}

// [orig: Terrain_RenderSectorEntities `cmp dword ptr [eax+1D0h],0; jnz`
// @ 0x5c7b92..0x5c7b99 around `call test_sector_entity_occlusion` @ 0x5c7b9c,
// its cull `jnz` @ 0x5c7ba6; Terrain_RenderSectorEntitiesBySide
// `mov eax,[esi+1D0h]; test; jnz` @ 0x5c7d8b..0x5c7d93, the call @ 0x5c7d96,
// the cull @ 0x5c7da0 (its contained leg instead fetches the pool-2
// building `shr eax,14h` @ 0x5c7da8 for the interior lighting)]
bool OcclusionWorld::render_wave_toc_occluded(const uint32_t hits[4],
                                              const TocCandidate &cand) const {
    if (hits[0] != 0) return false;
    return toc_occludes(cand);
}

// [orig: Terrain_CollectVisibleEntities_0 `cmp [ebp+1D0h],ebx; jz` @ 0x5c7022
// then the four-hit walk @ 0x5c7040-0x5c708a (the pool-2 building
// `shr eax,14h`, its g_BuildingSectionVisMask word tested against the
// section bit); the twin @ 0x5c8d70-0x5c8dd1]
bool OcclusionWorld::blink_hits_render_active(const uint32_t hits[4]) const {
    if (hits[0] == 0) return true;
    for (int32_t i = 0; i < 4; ++i) {
        const uint32_t h = hits[i];
        if (h == 0) continue;
        const uint32_t mask = masks_.empty() ? 0u : masks_[(h >> 20) & (kMaskSlots - 1)];
        if ((mask & (1u << ((h >> 12) & 0x1F))) != 0) return true;
    }
    return false;
}

// [orig: collect_visible_entities_for_terrain @ 0x5c8c60 — `mov esi,[edi]`
// (entity+0) @ 0x5c8df3, and under Flags 0x20 (`test byte ptr [edi+24h],20h`
// @ 0x5c8def) the special item-185 model's radius instead: gItemDefs[
// g_ParachuteItemIndex]+0xF0 -> model +0x14 @ 0x5c8df7..0x5c8e10]
int32_t OcclusionWorld::person_collector_radius(int32_t bound_radius_q16,
                                                bool parachute) const {
    return parachute ? parachute_radius_q16_ : bound_radius_q16;
}

// [orig: collect_visible_entities_for_terrain @ 0x5c8c60, the person leg: the
// |view depth| < fog + radius cull @ 0x5c8e26..0x5c8e3c, the viewport clip
// @ 0x5c8e4d, the sub-pixel floor `cmp dword_A784F0,0C000h; jle` @ 0x5c8e5e
// BEFORE the latch, then the latch @ 0x5c8e7b..0x5c8eab with the probe aimed
// at the entity position (`push ebx` = entity+4 @ 0x5c8e91)]
bool OcclusionWorld::person_render_visible(const CollisionWorld &collision,
                                           const OcclusionFrameCamera &cam,
                                           const int32_t pos[3], int32_t radius,
                                           uint8_t &latch, uint32_t logic_tick) {
    int32_t depth = 0;
    if (!sphere_in_view(cam, pos, radius, &depth)) return false;
    if (renderer::object_subpixel_culled(
                renderer::project_bound_sphere_radius_q16(radius, depth, cam.focal_pixels)))
        return false;
    return latch_render_visible(collision, cam, pos, radius, latch, logic_tick);
}

bool OcclusionWorld::sphere_render_visible(const CollisionWorld &collision,
                                           const OcclusionFrameCamera &cam,
                                           const int32_t center_world[3],
                                           int32_t radius, uint8_t &latch,
                                           uint32_t logic_tick) {
    if (!sphere_in_view(cam, center_world, radius)) return false;
    return latch_render_visible(collision, cam, center_world, radius, latch, logic_tick);
}

bool OcclusionWorld::latch_render_visible(const CollisionWorld &collision,
                                          const OcclusionFrameCamera &cam,
                                          const int32_t center_world[3],
                                          int32_t radius, uint8_t &latch,
                                          uint32_t logic_tick) {

    // Three-ray latch, outdoors only. [orig: @ 0x5c7125-0x5c7162]
    const bool outdoors = (~cam.local_blink_flags & 0x2u) != 0;
    if (latch != 0) {
        --latch;
        return true;
    }
    if (outdoors && !three_rays_clear(collision, cam, center_world, radius,
                                      logic_tick))
        return false;
    latch = static_cast<uint8_t>((latch_rand16() & 7) + 16);
    return true;
}

// ----------------------------------------------------------------------------
// Frame results
// ----------------------------------------------------------------------------

bool OcclusionWorld::building_visible(EntityHandle h) const {
    for (const BatchEntry &b : batch_)
        if (b.entity == h) return !b.culled;
    return false;
}

// [orig: the mask read `mov edi, g_BuildingSectionVisMask[eax*4]` in
// Terrain_RenderSectorModels @ 0x5c5d72]
uint32_t OcclusionWorld::section_mask(EntityHandle h) const {
    return masks_.empty() ? 0u : masks_[h.slot() & (kMaskSlots - 1)];
}

// [orig: Terrain_RenderSectorModels — `mov cl,[eax+891h]; test cl,cl; jz;
// or edx,-1; shl edx,cl` @ 0x5c5d7c..0x5c5d8d, the +0x892 twin
// @ 0x5c5d95..0x5c5da4 (x86 shl masks the count & 31)]
uint32_t OcclusionWorld::forced_section_mask(EntityHandle h) const {
    const auto it = forced_sections_.find(h.packed);
    if (it == forced_sections_.end()) return 0u;
    uint32_t mask = 0;
    for (const uint8_t first : it->second)
        if (first != 0) mask |= 0xFFFFFFFFu << (first & 31);
    return mask;
}

int32_t OcclusionWorld::instance_model_id(EntityHandle h) const {
    const Instance *inst = instance(h);
    return inst == nullptr ? -1 : inst->model_id;
}

bool OcclusionWorld::building_batched(EntityHandle h) const {
    // Keyed by the never-zeroed debug handle: render_TOC faithfully zeroes a
    // culled row's live `entity`, which would otherwise hide the row here.
    for (const BatchEntry &b : batch_)
        if (b.debug_entity == h) return true;
    return false;
}

bool OcclusionWorld::building_open_flagged(EntityHandle h) const {
    for (const BatchEntry &b : batch_)
        if (b.entity == h) return b.open_flag;
    return false;
}

OcclusionWorld::BuildingFlags OcclusionWorld::building_flags(EntityHandle h) const {
    BuildingFlags f;
    const Instance *inst = instance(h);
    if (inst != nullptr) {
        f.has_open = inst->has_open;
        f.has_windows = inst->has_windows;
        f.has_links = inst->has_links;
    }
    return f;
}

} // namespace opennova::world
