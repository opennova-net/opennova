// The host's fired-round event ring — the source of the S2C 0x0A tag-2 round-event
// records every in-match recipient is served from (docs/net/novaworld-net-re.md §5.9.1).
//
// [orig: g_round_ring @0xC8D848 — 256 x 36-B records, write cursor g_round_ring_cursor
// @0xC8FC4C, saturating count g_round_ring_count @0xC8FC48; appended ONLY by
// RoundData_AddRound @0x4fdb40 (alt-fire, AI fire, and the local/re-entrant primary-fire
// paths of Server_ClientFiredRound @0x50baa0 — a NET primary fire reaches it through the
// adm 'fire' action -> Entity_FireWeaponAndSendPacket @0x42bd80 -> local re-entry);
// consumed per-recipient by Server_BuildRoundEventListForPlayer @0x4ffee0.]
#ifndef OPENNOVA_WORLD_ROUND_RING_H
#define OPENNOVA_WORLD_ROUND_RING_H

#include <array>
#include <cstdint>

namespace opennova::world {

// One fired-round EVENT: the fire origin + direction a recipient's client re-simulates
// the round from — NOT an impact record (the §5.9.1 "weapon-hit" reading was a decode-era
// guess; witness 2026-07-03). Field sources are the 36-B ring record:
struct RoundEvent {
    // Monotonic append sequence, compared against each recipient's watermark. [orig:
    // ring+0 = stat_id @0xC86FB0 (the frame clock); ours is a per-append sequence — the
    // same strictly-greater watermark semantics at finer grain.]
    uint32_t stat = 0;
    // ring+4 — the SHOOTER's pool<<12|slot handle (RoundData_AddRound resolves the
    // shooter entity ptr, NOT the hit target) [orig: @0x4fdca8].
    uint16_t shooter_handle = 0xFFFF;
    // ring+8/+12/+16 — the fire origin (i32 16.16 world), pre-spread [orig: read from the
    // fire request BEFORE RoundData_SpawnRound applies weapon spread, @0x4fdbce].
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t origin_z = 0;
    // ring+20/+24 — fire direction yaw/pitch as BAM32 (the C2S 0x06 raw i32 << 16)
    // [orig: @0x513436/@0x513449; serialized as the high word @0x504a18].
    int32_t dir_yaw = 0;
    int32_t dir_pitch = 0;
    // ring+28 — per-shot sequence word; the C2S 0x06 hit_part fire counter round-trips
    // here via word_B7C670 -> the spawned round's +120 word [orig: @0x50c2ba -> @0x4fdcf5].
    uint16_t shot_seq = 0;
    // ring+30 — the fire-mode byte (bit0 alt-fire, bit1 adm-indexed, bits 4-5 =
    // pre-consume MountSlot+0x10 magazine count low two bits); becomes the wire
    // flags low bits [orig: build @0x542c11 before consume @0x542c75; ring @0x4fdcde].
    uint8_t mode_flags = 0;
    // ring+31 — shooter fire-context composite: (extra_byte2 & 0x3F) | ((extra_byte2>>7)<<7)
    // [orig: @0x50bd75..@0x50bd83 -> roundParams[4] @0x50c7bd; wire byte 3].
    uint8_t subtype = 0;
    // ring+32 — weapon-slot id / the uplink misc_byte; non-zero gates the wire 0x80 flag
    // byte [orig: @0x4fdcfc; serializer gate @0x5048bb].
    uint8_t slot_byte = 0;
    // ring+33 — the AdmDef weapon index [orig: @0x4fdce6].
    uint8_t adm_index = 0;
};

struct RoundRing {
    static constexpr int kCapacity = 256; // [orig: cursor wrap @0x4fdd2d]

    std::array<RoundEvent, kCapacity> records{};
    int32_t cursor = 0;      // next write index [orig: g_round_ring_cursor @0xC8FC4C]
    int32_t count = 0;       // saturates at capacity [orig: g_round_ring_count @0xC8FC48]
    uint32_t next_stat = 1;  // append sequence; 0 stays "before any round" for watermarks

    // Last stamped sequence — what a recipient's watermark advances to after a sweep
    // [orig: playerSlot+97544 = stat_id after Server_BuildRoundEventListForPlayer].
    uint32_t last_stat() const { return next_stat - 1; }

    void add(RoundEvent ev) {
        ev.stat = next_stat++;
        records[static_cast<size_t>(cursor)] = ev;
        if (count < kCapacity) ++count;          // [orig: @0x4fdd0d]
        if (++cursor >= kCapacity) cursor = 0;   // [orig: @0x4fdd1e/@0x4fdd2d]
    }
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ROUND_RING_H
