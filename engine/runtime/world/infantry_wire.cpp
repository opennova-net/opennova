#include <runtime/world/ai.h>
#include <runtime/world/world.h>

namespace opennova::world {

// Mirror the motor-selected body-anim state + channel phase onto the world Entity — the store
// snapshot_of reads for the 0x0A player record bytes 14/15 (emit reads pending ?: current
// [orig: @0x4c0cc7]; ratio = elapsed ticks in the current loop pass, clamp 255 [orig:
// AnimChannel_AdvancePlayback @0x40B140 via @0x4c0cf2]). The LOCAL player additionally exports
// its packed MoveOrder low byte (bits 0-2 dir, bit 3 moving) so its own record echoes real
// input to the peers that motor-drive its avatar [orig: Player_PackInputStateToEntity
// @0x4df68f-0x4df6a1 packs it; the record write reads entity+0x12C low @0x4c0c9c].
void AiSystem::mirror_wire_anim(AiEntity &e, World &world) {
    if (!e.inf.active) return;
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;
    const InfantryState &inf = e.inf;
    ent->net_anim_state = static_cast<uint8_t>(inf.anim_state);
    ent->net_anim_pending = static_cast<uint8_t>(inf.anim_pending);
    // The eye-offset mirror (entity+0x6C/+0x70/+0x74): the body tick's restamp
    // reaches the registry entity the friendly-tag gather (z) and the retail
    // camera consumer (full triple) walk [orig: the same entity fields the
    // writers, HUD_DrawEntityLabel @0x5a3a84, and Camera_ComputeThirdPersonView
    // @0x437fa5 share].
    ent->eye_offset_x = inf.eye_offset_x;
    ent->eye_offset_y = inf.eye_offset_y;
    ent->eye_offset_z = inf.eye_offset_z;
    ent->net_anim_phase =
        static_cast<uint8_t>(inf.clip_phase < 0 ? 0 : (inf.clip_phase > 255 ? 255 : inf.clip_phase));
    if (inf.is_local_player) {
        // Bits 0-2 dir, 3 moving, 5 the HELD jump key, 6/7 the lean keys — the
        // MoveOrder LOW byte layout the uplink's byte 19 carries [orig: the
        // packer @0x4df68f-0x4df741; jump bit 5 @0x4df6fa-0x4df701]. A retail
        // host launches + stamps anim 30/31 from bit 5 [orig: the jump gate
        // @0x4b7e8c-0x4b7f06], so omitting it made a joiner's jump invisible.
        ent->net_move_input = static_cast<uint8_t>((inf.player_move_dir_index & 7) |
                                                   (inf.player_moving ? 8 : 0) |
                                                   (inf.free_look ? 0x10 : 0) |
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
