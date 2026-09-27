// The relative-position metrics against the original x86
// [orig: Entity_ComputeRelativePositionMetrics @0x545710]: quarter-turn yaw frames
// (exact in Q22 on both sides) with synthetic points. Both distances come from
// `fild; fsqrt; fistp` under the nearest-even control word
// (@0x5457CD..0x5457D3, @0x5457EC..0x5457F2); the two angles truncate through
// _ftol2_sse. Regenerate the vectors with scripts/oracles/aim_metrics_parity.py.
#include <runtime/world/ai.h>
#include <cstdio>

using namespace opennova::world;

namespace {
int failures = 0;
} // namespace

int main() {
    // {quarter, frame x/y/z, point x/y/z, hdist, dist, yaw, pitch}
    static const int32_t cases[][11] = {
#include "fixtures/aim_metrics_vectors.inc"
    };
    for (const auto &v : cases) {
        const int32_t heading = static_cast<int32_t>(static_cast<uint32_t>(v[0]) << 30);
        const int32_t pose[6] = {v[1], v[2], v[3], heading, 0, 0};
        const int32_t aim[3] = {v[4], v[5], v[6]};
        int32_t metrics[6];
        AiSystem::weapon_relative_metrics(pose, aim, metrics);
        const int32_t actual[4] = {metrics[0], metrics[2], metrics[3], metrics[4]};
        for (int i = 0; i < 4; ++i) {
            if (actual[i] == v[7 + i]) continue;
            std::printf("metrics quarter=%d point=(%d,%d,%d) field=%d: %d != %d\n", v[0], v[4],
                        v[5], v[6], i, actual[i], v[7 + i]);
            ++failures;
            break;
        }
    }
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("aim_metrics_parity: %zu original-instruction cases passed\n",
                sizeof(cases) / sizeof(cases[0]));
    return 0;
}
