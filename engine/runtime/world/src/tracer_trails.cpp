#include "world/tracer_trails.h"

namespace opennova::world {

// [orig: CEffectEmitterPool_AllocSlot @ 0x5db7a0 — linear scan of the 256 alloc flags,
// first zero wins, slot re-initialized from the style block.]
int TracerTrailPool::alloc(int32_t style_id) {
    for (int i = 0; i < kChannels; ++i) {
        TracerTrailChannel &c = channels[static_cast<size_t>(i)];
        if (c.active) continue;
        c.active = true;
        c.kill = false;
        c.style_id = style_id;
        c.cap = tracer_style_cap(style_id); // [orig: c[3] = desc+0x10 @ 0x5db228]
        c.count = 0;
        c.age = 0;
        return i;
    }
    return -1; // [orig: all 256 occupied -> nullptr @ 0x5db7bc]
}

// [orig: CEffectChannel_AppendPoint @ 0x5db290.]
void TracerTrailPool::append(int slot, const Vec3 &pos) {
    if (slot < 0 || slot >= kChannels) return;
    TracerTrailChannel &c = channels[static_cast<size_t>(slot)];
    if (!c.active) return;
    if (c.count >= c.cap) {
        // Full ring: shift the oldest out [orig: memcpy down + count-- @ 0x5db2b2].
        for (int i = 1; i < c.count; ++i)
            c.pts[static_cast<size_t>(i - 1)] = c.pts[static_cast<size_t>(i)];
        --c.count;
    }
    if (c.count < 0) return;
    TracerTrailPoint &p = c.pts[static_cast<size_t>(c.count)];
    p.pos = pos;
    // Jitter styles stamp a per-point width multiplier [orig: desc+4 gate @ 0x5db2e2,
    // w = PRNG16 * 0.0000099999997 + 1.0 @ 0x5db30a]; plain tracers write exactly 1.0.
    p.w = tracer_style_jitter(c.style_id)
                  ? 1.0f + static_cast<float>(next_rand16()) * 0.0000099999997f
                  : 1.0f;
    ++c.count;
    c.age = 0; // [orig: self[5] = 0 @ 0x5db312/0x5db32c — drain starts only after death]
}

// [orig: CEffectChannel_RequestKill @ 0x5db380 — c[10] = 1.]
void TracerTrailPool::request_kill(int slot) {
    if (slot < 0 || slot >= kChannels) return;
    TracerTrailChannel &c = channels[static_cast<size_t>(slot)];
    if (c.active) c.kill = true;
}

// [orig: CEffectEmitterPool_Tick @ 0x5db830 — once per 62 Hz frame.]
void TracerTrailPool::tick() {
    for (auto &c : channels) {
        if (!c.active) continue;
        if (c.age < c.cap) {
            ++c.age; // [orig: @ 0x5db850 — cap-length grace before the drain starts]
        } else if (c.count > 0) {
            // Pop the oldest point [orig: memcpy down @ 0x5db873 + count-- @ 0x5db87b].
            for (int i = 1; i < c.count; ++i)
                c.pts[static_cast<size_t>(i - 1)] = c.pts[static_cast<size_t>(i)];
            --c.count;
        }
        // Kill-requested and drained -> free the slot [orig: @ 0x5db885-0x5db88b].
        if (c.kill && c.count == 0) c.active = false;
    }
}

void TracerTrailPool::reset() noexcept {
    for (auto &c : channels) c = TracerTrailChannel{}; // keep the heap block
}

uint16_t TracerTrailPool::next_rand16() {
    // LCG (MSVC constants) — presentation-only jitter source; see the header note.
    jitter_seed_ = jitter_seed_ * 214013u + 2531011u;
    return static_cast<uint16_t>(jitter_seed_ >> 16);
}

} // namespace opennova::world
