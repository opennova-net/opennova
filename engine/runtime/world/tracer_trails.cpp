#include <runtime/world/tracer_trails.h>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <runtime/world/collision.h>

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
        // [orig: c[0] = desc+8 @ 0x5db243, c[1] = desc+0xC @ 0x5db249]
        c.wobble_rate = tracer_style_wobble_rate(style_id);
        c.wobble_amplitude = tracer_style_wobble_amplitude(style_id);
        // A fresh channel starts at count -1, so the first append only raises
        // it to 0 [orig: CEffectChannel_Init @ 0x5db233].
        c.count = -1;
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
    if (c.count < 0) {
        // The fresh channel's first append stores nothing and leaves the age:
        // it only lifts count from -1 to 0 [orig: the `jl` @ 0x5db2c3 to the
        // bare count++ @ 0x5db333].
        ++c.count;
        return;
    }
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

// [orig: Projectile_GetTrailAnchorPos @ 0x4E64E0]
Vec3 tracer_trail_anchor(const TracerTrailChannel &channel, const Vec3 &position,
                         int32_t heading_bam, int32_t pitch_bam, int32_t roll_bam,
                         uint32_t entity_update_counter) {
    const int32_t origin[3] = {to_fixed(position.x), to_fixed(position.y),
                               to_fixed(position.z)};
    // The matrix over round+4 (position) and +0x10..+0x18 (heading, pitch, roll)
    // [orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x4E64FE].
    const CollisionMatrix matrix =
            collision_matrix_from_euler(heading_bam, pitch_bam, roll_bam, origin);
    // The table index: the 32-bit product plus half a step, its top ten bits
    // [orig: imul g_EntityUpdateCounter @ 0x4E650B, add 200000h @ 0x4E6512,
    //  shr 16h @ 0x4E6518].
    const uint32_t phase = entity_update_counter * static_cast<uint32_t>(channel.wobble_rate) +
                           0x200000u;
    const int32_t sine_q22 = static_cast<int32_t>(
            io::bam_table_sin(io::bam_table_index(phase)) * io::kQ22One);
    const int32_t sine_q16 = sine_q22 >> 6; // [orig: sar ecx, 6 @ 0x4E652D]
    // [orig: imul; add 8000h; adc; shrd 10h @ 0x4E653F..0x4E654B, the same
    //  amplitude word for both @ 0x4E652A / @ 0x4E6559]
    const int32_t d = static_cast<int32_t>(
            (static_cast<int64_t>(channel.wobble_amplitude) * sine_q16 + 0x8000) >> 16);
    const int32_t local[3] = {0, d, d}; // [orig: point.x = 0 @ 0x4E6522]
    int32_t anchor[3];
    matrix.transform_point(local, anchor); // [orig: Math_FixedPointTransformPoint22 @ 0x4E658B]
    return Vec3{static_cast<float>(from_fixed(anchor[0])), static_cast<float>(from_fixed(anchor[1])),
                static_cast<float>(from_fixed(anchor[2]))};
}

uint16_t TracerTrailPool::next_rand16() {
    // LCG (MSVC constants) — presentation-only jitter source; see the header note.
    jitter_seed_ = jitter_seed_ * 214013u + 2531011u;
    return static_cast<uint16_t>(jitter_seed_ >> 16);
}

} // namespace opennova::world
