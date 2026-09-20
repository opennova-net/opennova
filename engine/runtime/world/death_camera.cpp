#include <runtime/world/death_camera.h>

#include <cmath>
#include <cstdint>
#include <base/io/fixed.h>
#include <base/io/bam.h>

namespace opennova::world {

namespace {

constexpr double kPi = io::kPi;
// dbl_7C19D8 = 2^32 / (2 pi): radians -> BAM32 [orig: @0x7c19d8].
constexpr double kBamPerRadian = 683565275.5764316;
// flt_7C32BC = 65536.0: the float normalise scale [orig: @0x7c32bc].
constexpr double kQ16 = 65536.0;

// g_bam_sin_table_q22 / the cos table (off_849934): 1024 Q22 entries indexed
// by (angle + 0x200000) >> 22 [orig: @0x438c4e / @0x438c5b].
inline int32_t sin_q22(uint32_t index) {
    return static_cast<int32_t>(std::sin(static_cast<double>(index & 0x3FFu) * (2.0 * kPi / 1024.0)) * io::kQ22One);
}
inline int32_t cos_q22(uint32_t index) {
    return static_cast<int32_t>(std::cos(static_cast<double>(index & 0x3FFu) * (2.0 * kPi / 1024.0)) * io::kQ22One);
}
inline uint32_t table_index(uint32_t angle_bam) { return (angle_bam + 0x200000u) >> 22; }

// (a * b + 0x8000) >> 16 in 64-bit, the original's rounded 16.16 multiply.
inline int32_t mul_q16(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b + 0x8000) >> 16);
}
inline int32_t mul_q22(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
}

// The float normalise the original runs through the x87: v / |v| * 65536,
// truncated per axis; a zero-length vector becomes zero
// [orig: the fsqrt / fucomp-zero / fdivr flt_7C32BC blocks @0x438bd6..0x438c1e].
void normalise_q16(int32_t v[3], int32_t *length_q16) {
    const double x = v[0], y = v[1], z = v[2];
    const double len = std::sqrt(x * x + y * y + z * z);
    if (length_q16 != nullptr) *length_q16 = static_cast<int32_t>(len);
    if (len != 0.0) {
        const double s = kQ16 / len;
        v[0] = static_cast<int32_t>(x * s);
        v[1] = static_cast<int32_t>(y * s);
        v[2] = static_cast<int32_t>(z * s);
    } else {
        v[0] = v[1] = v[2] = 0;
    }
}

inline int32_t atan2_bam(double y, double x) {
    return static_cast<int32_t>(std::atan2(y, x) * kBamPerRadian);
}

} // namespace

int32_t death_camera_probe_terrain(const CameraTerrainSampler &terrain,
                                   int bone_count,
                                   const int32_t origin[3],
                                   const int32_t dir_q16[3], int32_t lift,
                                   int32_t max_dist) {
    // [orig: Camera_RaycastCollisionOffset @0x4378b0]
    if (bone_count <= 0) return max_dist; // @0x4378c4: no bones -> untouched reach
    int32_t accum[3] = {0, 0, 0};
    int32_t reached = 0;
    bool hit = false;
    int sample = 1;
    if ((max_dist >> 14) > 1) {
        int32_t accum_x = 0x10000;
        do {
            if (hit) break;
            reached = accum_x >> 2; // 0.25 u steps
            const int32_t px = mul_q16(reached, dir_q16[0]) + origin[0];
            const int32_t py = mul_q16(reached, dir_q16[1]) + origin[1];
            const int32_t pz = mul_q16(reached, dir_q16[2]) + origin[2];
            for (int bone = 0; bone < bone_count; ++bone) {
                // The bone force leg (Entity_ComputeCollisionForceFromBones
                // @0x4afff0) is the tracked residue; the terrain test that
                // follows it in the same loop body runs per bone like retail.
                const int32_t ground = terrain ? terrain(px, py) : INT32_MIN / 2;
                if (lift + ground > pz) {
                    accum[0] -= dir_q16[0] >> 2;
                    accum[1] -= dir_q16[1] >> 2;
                    accum[2] -= dir_q16[2] >> 2;
                    hit = true;
                }
            }
            accum_x += 0x10000;
            ++sample;
        } while (sample < (max_dist >> 14));
    }
    return reached + mul_q16(dir_q16[0], accum[0]) + mul_q16(dir_q16[1], accum[1]) +
           mul_q16(dir_q16[2], accum[2]);
}

void death_camera_compute(const int32_t player_pos[3],
                          const int32_t anchor_pos[3],
                          const CameraRayProbe &probe,
                          DeathCameraState &out) {
    // [orig: Camera_ComputeThirdPersonPositions @0x438b80]
    int32_t norm[3] = {anchor_pos[0] - player_pos[0], anchor_pos[1] - player_pos[1],
                       anchor_pos[2] - player_pos[2]};
    int32_t length = 0;
    normalise_q16(norm, &length);
    bool too_close = false;
    if (length < 0x10000) { // @0x438c27: under 1.0 u the direction is +X
        norm[0] = 0x10000;
        norm[1] = 0;
        norm[2] = 0;
        too_close = true;
    }
    int32_t best[3] = {norm[0], norm[1], norm[2]};
    int32_t best_reach = 0;
    int32_t from[3] = {player_pos[0], player_pos[1], player_pos[2] + kDeathCameraLiftQ16};
    const int32_t neg_z = -norm[2];
    // Trial angles +step, -step, +2 step, ... about the vertical, the first
    // strictly farther reach wins, a full reach ends the search
    // [orig: the do/while @0x438c4e..0x438e9a].
    for (uint32_t step = static_cast<uint32_t>(kDeathCameraTrialStepBam); step < 0x7FFFFFFFu;
         step += static_cast<uint32_t>(kDeathCameraTrialStepBam)) {
        bool done = false;
        for (int sign = 0; sign < 2 && !done; ++sign) {
            const uint32_t index = sign == 0 ? table_index(step)
                                             : ((0x1FFFFFu - step) >> 22);
            const int32_t s = sin_q22(index);
            const int32_t c = cos_q22(index);
            int32_t trial[3];
            trial[0] = mul_q16(s, norm[1]) - mul_q16(c, norm[0]);
            trial[1] = -(mul_q16(s, norm[0]) + mul_q16(c, norm[1]));
            trial[2] = neg_z;
            normalise_q16(trial, nullptr);
            const int32_t reach = probe ? probe(from, trial, kDeathCameraLiftQ16, kDeathCameraReachQ16)
                                        : kDeathCameraReachQ16;
            if (reach > best_reach) {
                best[0] = trial[0];
                best[1] = trial[1];
                best[2] = trial[2];
                best_reach = reach;
                if (reach >= kDeathCameraReachQ16) done = true;
            }
        }
        if (done) break;
    }
    // FROM: the player lifted 0.5 u, pushed out along the best direction by
    // its reach, looking back along -best [orig: @0x438ea0..0x438f5b].
    from[0] += mul_q16(best_reach, best[0]);
    from[1] += mul_q16(best_reach, best[1]);
    from[2] += mul_q16(best_reach, best[2]);
    out.from.pos[0] = from[0];
    out.from.pos[1] = from[1];
    out.from.pos[2] = from[2];
    out.from.yaw_bam = atan2_bam(static_cast<double>(-best[1]), static_cast<double>(-best[0]));
    {
        const uint32_t yi = table_index(static_cast<uint32_t>(out.from.yaw_bam));
        const int32_t horiz = mul_q22(cos_q22(yi), -best[0]) + mul_q22(sin_q22(yi), -best[1]);
        out.from.pitch_bam = atan2_bam(static_cast<double>(-best[2]), static_cast<double>(horiz));
    }
    out.from.roll_bam = 0;
    // TO: the lifted player pushed back along -dir by that reach, looking
    // along dir toward the anchor [orig: @0x438f66..0x439053].
    int32_t to[3] = {player_pos[0], player_pos[1], player_pos[2] + kDeathCameraLiftQ16};
    const int32_t back[3] = {-norm[0], -norm[1], neg_z};
    const int32_t reach_back = probe ? probe(to, back, kDeathCameraLiftQ16, kDeathCameraReachQ16)
                                     : kDeathCameraReachQ16;
    to[0] += mul_q16(reach_back, back[0]);
    to[1] += mul_q16(reach_back, back[1]);
    to[2] += mul_q16(reach_back, back[2]);
    out.to.pos[0] = to[0];
    out.to.pos[1] = to[1];
    out.to.pos[2] = to[2];
    out.to.yaw_bam = atan2_bam(static_cast<double>(norm[1]), static_cast<double>(norm[0]));
    {
        const uint32_t yi = table_index(static_cast<uint32_t>(out.to.yaw_bam));
        const int32_t horiz = mul_q22(cos_q22(yi), norm[0]) + mul_q22(sin_q22(yi), norm[1]);
        out.to.pitch_bam = atan2_bam(static_cast<double>(norm[2]), static_cast<double>(horiz));
    }
    out.to.roll_bam = 0;
    if (too_close) out.to = out.from; // @0x439058: no target -> hold the FROM pose
    out.valid = true;
}

void death_camera_view(const DeathCameraState &state, uint32_t tick,
                       DeathCameraPose &out) {
    // [orig: @0x4389f8..0x438b3d]
    int32_t t = static_cast<int32_t>((tick - state.start_tick) << 9);
    if (t > 0x10000) t = 0x10000;
    auto lerp = [t](int32_t a, int32_t b) {
        return a + static_cast<int32_t>((static_cast<int64_t>(b - a) * t + 0x8000) >> 16);
    };
    for (int i = 0; i < 3; ++i) out.pos[i] = lerp(state.from.pos[i], state.to.pos[i]);
    out.yaw_bam = lerp(state.from.yaw_bam, state.to.yaw_bam);
    out.pitch_bam = lerp(state.from.pitch_bam, state.to.pitch_bam);
    out.roll_bam = lerp(state.from.roll_bam, state.to.roll_bam);
}

} // namespace opennova::world
