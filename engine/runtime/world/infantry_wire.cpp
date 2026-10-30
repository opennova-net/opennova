#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>

#include <base/io/crt_ftol.h>

#include <algorithm>

namespace opennova::world {

// The record's ratio is the body AnimMap's elapsed ticks. While the map cross-fades (its second
// half holds a clip) it is the fade's weight over its step, the ticks the fade has run; else the
// playing channel's t over its step, the ticks into its current pass: a loop's t wraps at 1, so
// the byte restarts every pass, and a one-shot's (or an armed loop's) t parks at 0.99999, so the
// byte holds there. The quotient goes through the CRT's ftol (the x87 value to a double, then
// truncated), a ratio past 255 is written 255, and the low byte goes out.
// [orig: NetPacket_SerializePlayerState @0x4C0CF2..0x4C0D35 - the map +0x188 @0x4C0CF2, the
//  second half's clip +0x14 @0x4C0CFF, weight +0x28 over step +0x2C @0x4C0D05..0x4C0D08, t +8
//  over step +0xC @0x4C0D0D..0x4C0D10, _ftol2_sse @0x4C0D13, the 255 cap @0x4C0D18..0x4C0D1F;
//  the halves: AnimChannel_AdvanceBlendedPlayback @0x40B1E0 (weight += step @0x40B1F7..0x40B209,
//  the second half into the first at the countdown's end @0x40B20C..0x40B224);
//  AnimChannel_InitFromParams @0x410640 (weight 0 @0x410690, step 1 / duration @0x410695..0x41069D)]
uint8_t wire_anim_ratio(const InfantryState &inf, const IRootMotionSource *source) {
    double ratio = 0.0;
    if (inf.body_blend_active()) {
        ratio = double(inf.anim_blend_weight) / double(inf.anim_blend_step);
    } else {
        float time = 0.0f;
        float delta = 0.0f;
        const int32_t armed = inf.body_phase_parked() ? inf.clip_phase : -1;
        if (source == nullptr ||
                !source->clip_clock_at(inf.adm_id, inf.body_clip_state(), inf.anim_variant,
                                       inf.clip_phase, armed, time, delta))
            // A provider without the retail clock: the elapsed ticks themselves.
            return static_cast<uint8_t>(std::clamp(inf.clip_phase, 0, 255));
        ratio = double(time) / double(delta);
    }
    int32_t value = io::retail_ftol_sse2(ratio);
    if (value > 0xFF) value = 0xFF;
    return static_cast<uint8_t>(value);
}

// Mirror the motor-selected body-anim state onto the world Entity — the store snapshot_of reads
// for the 0x0A player record byte 14 (emit reads pending ?: current [orig: @0x4c0cc7]). Byte 15,
// the channel ratio, is no mirror: retail's writer divides the live channel's words only while
// it writes a record, so replication::snapshot_world reads wire_anim_ratio there
// [orig: NetPacket_SerializePlayerState @0x4C0CF2..0x4C0D35]. The LOCAL player additionally exports
// its packed MoveOrder low byte (bits 0-2 dir, bit 3 moving) so its own record echoes real
// input to the peers that motor-drive its avatar [orig: Player_PackInputStateToEntity
// @0x4df68f-0x4df6a1 packs it; the record write reads entity+0x12C low @0x4c0c9c].
void AiSystem::mirror_wire_anim(AiEntity &e, World &world) {
    if (!e.inf.active) return;
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;
    const InfantryState &inf = e.inf;
    ent->net_anim_state = static_cast<uint8_t>(inf.anim_state);
    ent->net_anim_pending = inf.anim_pending;
    // The eye-offset mirror (entity+0x6C/+0x70/+0x74): the body tick's restamp
    // reaches the registry entity the friendly-tag gather (z) and the retail
    // camera consumer (full triple) walk [orig: the same entity fields the
    // writers, HUD_DrawEntityLabel @0x5a3a84, and Camera_ComputeThirdPersonView
    // @0x437fa5 share].
    ent->eye_offset_x = inf.eye_offset_x;
    ent->eye_offset_y = inf.eye_offset_y;
    ent->eye_offset_z = inf.eye_offset_z;
    if (inf.is_local_player) {
        // Bits 0-2 dir, 3 moving, 5 the packed jump bit, 6/7 the lean keys — the
        // MoveOrder LOW byte layout the uplink's byte 19 carries [orig: the
        // packer @0x4df68f-0x4df741; jump bit 5 @0x4df6fa-0x4df701]. A retail
        // host launches + stamps anim 30/31 from bit 5 [orig: the jump gate
        // @0x4b7e8c-0x4b7f06], so omitting it made a joiner's jump invisible.
        // The fields hold the last pack's word: a joiner under a send holdoff
        // packs once per boundary, from every key held during the window.
        ent->net_move_input = static_cast<uint8_t>((inf.player_move_dir_index & 7) |
                                                   (inf.player_moving ? 8 : 0) |
                                                   (inf.free_look ? Entity::kMoveOrderFreeLook : 0) |
                                                   (inf.jump_held ? Entity::kMoveOrderJump : 0) |
                                                   (inf.lean_left ? 0x40 : 0) |
                                                   (inf.lean_right ? 0x80 : 0));
        ent->local_view_input = inf.view_input_bits;
        // Local stance mirrors into the MoveOrder bits 8-9 model too (prone bit0/crouch bit1)
        // so the host's own 0x0A tail echo carries it [orig: dword_B76484/dword_B76480 latch
        // the same bits the packer writes @0x4df6a7-0x4df6cd].
        ent->net_stance_bits = static_cast<uint8_t>(
            inf.stance == InfantryState::Stance::kProne
                ? 1u
                : (inf.stance == InfantryState::Stance::kCrouch ? 2u : 0u));
    }
}

} // namespace opennova::world
