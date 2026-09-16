#include <runtime/world/collision_force.h>
#include <runtime/world/world.h>
#include <runtime/world/dir_table.h>
#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <cmath>

namespace opennova::world {
namespace {
// The retail `(uint64)(k * table) >> 22` planar impulse: the 64-bit product
// logically shifted, then truncated into the 32-bit velocity — which is the
// floor of the Q22 product for every magnitude this function feeds.
// [orig: @0x4af5bc / @0x4af5f3 / @0x4af676 / @0x4af6a1]
int32_t impulse_q22(int64_t scale, int32_t table_q22) {
    return static_cast<int32_t>((scale * table_q22) >> 22);
}
} // namespace

// [orig: Entity_ApplyCollisionForce @0x4AF4A0]
void apply_collision_force(World &world, Entity &target, uint8_t hit_type,
        uint8_t force_type, const Vec3 &source, EntityHandle attacker) {
    // The burn byte (+0x368), the velocity triple (+0x98/+0x9C/+0xA0) and the
    // engine-frame pose live on the infantry body; a person without one has
    // no target for either half.
    AiEntity *body = world.ai.for_handle(target.handle);
    if (body == nullptr) return;
    InfantryState &inf = body->inf;
    const uint32_t flags = target.flags | target.engine_flags;

    int damage_multiplier = 4; // [orig: @0x4af4c6]
    // atan2 * 683565275.5764316 (2^32 / 2pi) into a BAM bearing [orig: @0x4af4d8]
    const int32_t dx = io::bam_sub(body->pos[0], static_cast<int32_t>(source.x * io::kFp16One));
    const int32_t dy = io::bam_sub(body->pos[1], static_cast<int32_t>(source.y * io::kFp16One));
    const int32_t angle_to_source = static_cast<int32_t>(static_cast<uint32_t>(
            static_cast<int64_t>(std::atan2(double(dy), double(dx)) * io::kBamPerRadian)));
    // Source within 90 degrees of the yaw (abs32 < 0x3FFFFFC0) [orig: @0x4af4e7]
    if (io::bam_abs(io::bam_sub(angle_to_source, body->heading)) < 0x3FFFFFC0)
        damage_multiplier = 3;
    const Entity *owner = world.registry.get(attacker);

    // The burn half: hitType stamps +0x368 on a live body [orig: @0x4af4f8..0x4af541]
    if (hit_type != 0 && (flags & kEntityFlagDead) == 0) {
        inf.burn_state = hit_type; // [orig: @0x4af506]
        if ((flags & kEntityFlagPlayer) != 0) {
            inf.idle_counter = 1; // [orig: @0x4af50e]
        } else {
            // Retail assumes a valid attacker here (otherEntity+142 = commandGroup).
            // An ownerless embedder event uses the ordinary duration; it cannot
            // establish a same-command-group reaction.
            const bool same_group = owner != nullptr && owner->group_id == target.group_id;
            inf.combat_move_timer = same_group ? (2 * damage_multiplier) >> 2    // [orig: @0x4af530]
                                               : (48 * damage_multiplier) >> 2;  // [orig: @0x4af541]
        }
    }

    // The force half runs regardless of hitType and of the dead bit — the
    // dead bit only picks the smaller constants [orig: switch @0x4af558].
    // Flags 0x40 (kEntityFlagMounted) blocks every velocity write.
    switch (force_type) {
    case 1: { // walk push [orig: @0x4af56b..0x4af602]
        const int32_t current_slide = inf.vel[2]; // slideDecay +0xA0
        if (current_slide < 4096 && (flags & kEntityFlagMounted) == 0) {
            int32_t cos22 = 0, sin22 = 0;
            quantized_dir(angle_to_source, cos22, sin22); // [orig: @0x4af583..0x4af59d]
            if ((flags & kEntityFlagDead) != 0) {
                inf.vel[2] = io::bam_add(current_slide, 2560);             // [orig: @0x4af5a9]
                inf.vel[0] = io::bam_add(inf.vel[0], impulse_q22(4608, cos22)); // [orig: @0x4af5bc]
                inf.vel[1] = io::bam_add(inf.vel[1], impulse_q22(4608, sin22)); // [orig: @0x4af5cb]
            } else {
                inf.vel[2] = io::bam_add(current_slide, 5120);             // [orig: @0x4af5e0]
                inf.vel[0] = io::bam_add(inf.vel[0], impulse_q22(9216, cos22)); // [orig: @0x4af5f3]
                inf.vel[1] = io::bam_add(inf.vel[1], impulse_q22(9216, sin22)); // [orig: @0x4af602]
            }
        }
        break;
    }
    case 2: { // drift [orig: @0x4af631..0x4af6b0]
        if (io::bam_add(io::bam_abs(inf.vel[0]), io::bam_abs(inf.vel[1])) < 4096 &&
            (flags & kEntityFlagMounted) == 0) { // [orig: @0x4af631 / @0x4af63d]
            int32_t cos22 = 0, sin22 = 0;
            quantized_dir(angle_to_source, cos22, sin22);
            const int64_t scale = (flags & kEntityFlagDead) != 0 ? 2304 : 9216;
            inf.vel[0] = io::bam_add(inf.vel[0], impulse_q22(scale, cos22)); // [orig: @0x4af676 / @0x4af6a1]
            inf.vel[1] = io::bam_add(inf.vel[1], impulse_q22(scale, sin22)); // [orig: @0x4af685 / @0x4af6b0]
        }
        break;
    }
    case 3: { // the local-player hit blackout [orig: @0x4af6c5..0x4af77e]
        if (!(target.handle == world.cached.local_player)) break;
        // In session AND a non-team game type (g_GameType bit 0x10000 is the
        // team-game bit: TDM = 0x10000, Co-op 0x30020) — in a session DM every
        // hit is an enemy hit, whatever the command group / team says.
        // [orig: @0x4af6e8]
        const bool session_non_team = world.rules.mp_session &&
                (world.match.rules().game_type & 0x10000u) == 0;
        // Self hit, or same commandGroup (+284) AND same team byte (+354)
        // outside a session DM [orig: @0x4af70e]. Retail assumes a valid
        // otherEntity; an ownerless embedder event takes the enemy leg.
        const bool friendly = attacker == target.handle ||
                (owner != nullptr && owner->group_id == target.group_id &&
                 owner->team == target.team && !session_non_team);
        if (friendly) {
            // Suppressed in session when the rules word carries NoFriendlyFire
            // (g_rules_flags & 0x200) [orig: @0x4af752]; otherwise intensity
            // 0xA000, rate 0x8000 / ((124 * mult) >> 2) [orig: @0x4af764..0x4af77e]
            if (!world.rules.mp_session || !world.rules.no_friendly_fire)
                world.weather.core.hit_dim.arm(damage_multiplier == 3, /*friendly=*/true);
        } else {
            // intensity 0xA000, rate ((mult << 15) >> 2) / ((310 * mult) >> 2)
            // [orig: @0x4af724..0x4af73e]
            world.weather.core.hit_dim.arm(damage_multiplier == 3, /*friendly=*/false);
        }
        // Both legs also write dword_B764B8 = 255 [orig: @0x4af769 / @0x4af729]
        // — a HUD hit indicator shared with Entity_OnDamageReceived @0x4af822
        // and reset by Game_InitNewRound @0x422784; its readers
        // (Player_UpdatePerFrame @0x4de5a7, HUD_RenderAllOverlays @0x5a8098,
        // Render_ProcessMainSceneFrame @0x5caba7) are unwitnessed, so it has
        // no port home yet (world-wac-ai-re §17.3b).
        break;
    }
    case 4: { // direct push [orig: @0x4af794..0x4af7dd]
        if (inf.vel[2] < 4096 && (flags & kEntityFlagMounted) == 0) {
            int32_t cos22 = 0, sin22 = 0;
            quantized_dir(angle_to_source, cos22, sin22);
            // (table << 10) >> 22 [orig: @0x4af7c4 / @0x4af7d3]
            inf.vel[0] = io::bam_add(inf.vel[0],
                    static_cast<int32_t>((static_cast<int64_t>(cos22) << 10) >> 22));
            inf.vel[1] = io::bam_add(inf.vel[1],
                    static_cast<int32_t>((static_cast<int64_t>(sin22) << 10) >> 22));
            inf.vel[2] = io::bam_add(inf.vel[2], 1024); // [orig: @0x4af7dd]
        }
        break;
    }
    default:
        break;
    }
}

// [orig: org1 @0x4BBF8F; org2 @0x4B70D9]
int select_infantry_burn(InfantryState &inf, const IRootMotionSource *motion,
        bool player, uint32_t tick) {
    int selected = player ? inf.anim_state : 0;
    if (inf.burn_state == 0) return selected;
    if (inf.burn_state <= 4) {
        const int state = 110 + inf.burn_state;
        if (motion != nullptr && motion->has_clip(inf.adm_id, state)) selected = state;
        else if (!player) inf.burn_state = 0;
    }
    if (!player || (tick & 15u) == 0) {
        int32_t &timer = player ? inf.idle_counter : inf.combat_move_timer;
        if (timer != 0) timer = io::bam_sub(timer, 1);
        else inf.burn_state = 0;
    }
    return selected;
}
} // namespace opennova::world
