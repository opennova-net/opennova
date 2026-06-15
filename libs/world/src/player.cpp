// Local-player on-foot motor (entity class org2) — the input-driven twin of the AI
// infantry motor. [orig: Entity_UpdateInfantryPhysics @0x4b40e0, continuation @0x4b434f.]
// Built incrementally; see docs/world/player-controller-re.md and the plan.
//   M1: look apply (yaw + pitch clamp).
//   M2: root-motion locomotion + gravity (-208/tick x1) + ground settle (shared 0x4b2bd0).
//   M3: stance; M4: FP camera; M5: recoil/aim.
// Reuses the AI motor's root-motion rotate + ground/fall-damage mechanics (org1 and org2
// share Entity_ProcessCollisionAndPlatformPhysics @0x4b2bd0 and the same entity layout);
// the player differs in command source (input), gravity cadence, and the look/recoil layer.
#include <climits>
#include <cmath>
#include <cstdint>

#include "world/ai.h"
#include "world/player.h"

namespace opennova::world {

namespace {
// View-pitch clamp immediates, BAM (deg * 2^32/360). [orig: the look action handler
// Input_HandleActionBinding_0 @0x4e0420 clamps entity+0x14: max @0x4e0d44, reduced when
// crouched (entity+0x12C & 0x100) @0x4e0ffe.] 0x38E38E00 = 80.0 deg, 0x1C71C700 = 40.0 deg.
constexpr int32_t kPitchClampStand = 0x38E38E00;
constexpr int32_t kPitchClampCrouch = 0x1C71C700;

// Player gravity. [orig: vel_z(+0xA0) += -208 (0xFFFFFF30) per tick @0x4b7ac8, gated off
// on-platform/in-water; terminal -32768 @0x4b7c77; pos_z(+0xC) += vel_z x1 @0x4b7ce0.] This
// is the single-rate form of the AI motor's -416/2-ticks x2 (D-PLR-4).
constexpr int32_t kPlayerGravityStep = 208;
constexpr int32_t kTerminalVelZ = -32768; // shared with the AI motor

// First-person eye height above the body origin for a foot soldier (16.16 world).
// [orig: Camera_ComputeThirdPersonView @0x437d10 bumps g_view_pos_z += 0x10000 @0x437e8f.]
constexpr int32_t kEyeHeight = 0x10000;

constexpr double kPi = 3.14159265358979323846;
} // namespace

void AiSystem::set_player_input(const PlayerInputCommand &cmd) {
    if (local_player_index_ < 0 || local_player_index_ >= static_cast<int>(entities_.size()))
        return;
    entities_[local_player_index_].player.input = cmd;
}

// Map the resolved input to a locomotion clip + a body-relative move-direction offset.
// [orig: the 8-way index (entity+0x12C bits0-2) selects a per-direction locomotion clip whose
// root motion is the speed.] M2 approximation (D-PLR-11): until the per-direction .adm clips
// (strafe/back/diagonals, per stance) are wired, we drive the forward locomotion gait and
// rotate its root motion by the 8-way offset (45 deg per index, BAM) so all directions move
// correctly; the strafe *animation* fidelity is the follow-up.
int AiSystem::player_select_anim(const PlayerInputCommand &cmd, int32_t &move_offset) const {
    move_offset = 0;
    if (!cmd.is_moving) return anim_state::kIdle; // stance-specific idle is M3
    // 45 deg per 8-way step = 2^32/8 = 0x20000000 (wraps for >=180 deg). dir 0=fwd, 2=strafe-L
    // (+90), 4=back (180), 6=strafe-R (-90).
    move_offset = static_cast<int32_t>(static_cast<uint32_t>(cmd.move_dir & 7) * 0x20000000u);
    return anim_state::kWalkForward;
}

// [orig: Entity_UpdateInfantryPhysics @0x4b40e0.]
void AiSystem::tick_player(AiEntity &e, World &world, uint32_t logic_tick) {
    (void)world;
    PlayerState &p = e.player;
    const PlayerInputCommand &cmd = p.input;

    // ---- Look (M1): yaw += delta (no turn-rate clamp), pitch += delta clamped per stance ----
    // [orig: act166/167 yaw @0x4e10a7/0x4e110c; act164/165 pitch; clamp @0x4e0d44 / 0x4e0ffe]
    e.heading += cmd.look_yaw_delta;
    {
        const int32_t bound = cmd.crouch ? kPitchClampCrouch : kPitchClampStand;
        int32_t pitch = e.pitch + cmd.look_pitch_delta;
        if (pitch > bound) pitch = bound;
        else if (pitch < -bound) pitch = -bound;
        e.pitch = pitch;
    }

    // ---- Stance (M3): derive from input (prone takes precedence; both bits never co-set).
    // [orig: entity+0x12C 0x100 crouch / 0x200 prone; server-authoritative in retail, applied
    // locally here.] Affects the pitch clamp (above) and the camera/anim below. ----
    p.stance = cmd.prone ? 2 : (cmd.crouch ? 1 : 0);

    // ---- Locomotion: select the clip, advance its root motion (every tick) ----
    int32_t move_offset = 0;
    p.anim_state = player_select_anim(cmd, move_offset);
    RootMotionFrame frame;
    bool have_clip = false;
    if (root_motion != nullptr)
        have_clip = root_motion->advance(p.anim_state, p.clip_phase, frame);
    p.last_events = have_clip ? frame.events : 0;

    // ---- Ground resample (every 8 ticks; same sampler the AI motor uses) ----
    const uint32_t key = logic_tick + 36u * static_cast<uint32_t>(e.net_id);
    if (terrain != nullptr && ((key & 7u) == 0 || !p.ground_cache_valid)) {
        GroundClearance clearance = ground_clearance;
        clearance.has_physics = e.has_physics;
        clearance.use_dead = (e.health <= 0);
        p.ground_cache = calc_average_ground_height(*terrain, e.pos, 0, clearance);
        p.ground_cache_valid = true;
    }

    // ---- Rotate the root delta by (heading + move_offset) and integrate ----
    // [orig: the velocity->position integrate in 0x4b40e0; the full-precision 2^22 sin/cos
    // rotate is shared with the AI motor (@0x4b9910 dump 4758-4779).]
    {
        const int32_t fwd = frame.dx, lat = frame.dy;
        const int32_t dir_angle = e.heading + move_offset;
        const double rad = static_cast<double>(dir_angle) * (kPi / 2147483648.0);
        const int32_t c = static_cast<int32_t>(std::cos(rad) * 4194304.0);
        const int32_t s = static_cast<int32_t>(std::sin(rad) * 4194304.0);
        const int32_t wx = static_cast<int32_t>((static_cast<int64_t>(fwd) * c) >> 22) -
                           static_cast<int32_t>((static_cast<int64_t>(lat) * s) >> 22);
        const int32_t wy = static_cast<int32_t>((static_cast<int64_t>(fwd) * s) >> 22) +
                           static_cast<int32_t>((static_cast<int64_t>(lat) * c) >> 22);
        e.pos[0] += wx + p.vel[0];
        e.pos[1] += wy + p.vel[1];
        e.pos[2] += frame.dz;
    }

    // ---- Gravity (-208/tick x1, terminal -32768) + ground settle ----
    // [orig: @0x4b7ac8 / 0x4b7c77 / 0x4b7ce0 gravity; settle + fall damage via the shared
    // resolver @0x4b2bd0 (byte-identical to the AI block @0x4bf7f2).] The player settles to
    // the resolver height WITHOUT the AI mover's +0x50000 stand offset (D-PLR-2); the eye
    // height is added by the camera (M4), not the body. Terrain-free (unit-test) worlds keep
    // the authored Z, like the infantry motor.
    if (terrain != nullptr) {
        p.vel[2] -= kPlayerGravityStep;
        if (p.vel[2] < kTerminalVelZ) p.vel[2] = kTerminalVelZ;
        e.pos[2] += p.vel[2];
        if (p.ground_cache_valid && p.ground_cache != INT32_MIN) {
            const int32_t floor_z = p.ground_cache; // no +0x50000 (D-PLR-2)
            if (e.pos[2] <= floor_z) {
                if (fall_damage_scale > 0 && p.vel[2] <= -1057 * fall_damage_scale) {
                    int32_t excess = (-1057 * fall_damage_scale) - p.vel[2];
                    int32_t dmg = excess >> 4;
                    if (dmg > e.health) dmg = e.health;
                    e.health = static_cast<int16_t>(e.health - dmg);
                }
                e.pos[2] = floor_z;
                p.vel[2] = 0;
            }
        }
    }

    // Mirror the transform into the brain working fields the present snapshot reads (like the
    // infantry motor); kWorkPosZ stays the grounded Z.
    e.brain.f[AiBrain::kWorkPosX] = e.pos[0];
    e.brain.f[AiBrain::kWorkPosY] = e.pos[1];
    e.brain.f[AiBrain::kWorkPosZ] = e.pos[2];
    e.brain.f[AiBrain::kWorkHeading] = e.heading;

    // ---- First-person camera (M4): compose the eye transform from the post-motor pose ----
    p.camera = compute_player_camera(e);

    advance_part_anim(e); // PANM channels integrate regardless of the motor path
}

// [orig: Camera_ComputeThirdPersonView @0x437d10 (mode-0 foot path) + Player_UpdateFirstPersonCamera
// @0x4dd380.] M4 ports the foot eye-height stage: eye = body position + the 1.0 eye-height bump,
// view angles = the body euler. The stage-2 refinements — the equipped-weapon bone offset
// (Def+0xF4..0xFC), the view biases, the velocity lead (>>7 clamped +/-0x400 X/Y, +/-0x1000 Z),
// and the prone drop (-0x500) — need the equipped weapon def (EquippedSlot, seeded with the M5
// loadout) and are deferred. Output is engine frame (Z-up, BAM); the host converts at the present
// boundary via the single-sourced MissionObjectPlacer handoff.
PlayerCamera AiSystem::compute_player_camera(const AiEntity &e) const {
    PlayerCamera cam;
    cam.eye[0] = e.pos[0];
    cam.eye[1] = e.pos[1];
    cam.eye[2] = e.pos[2] + kEyeHeight;
    cam.view_yaw = e.heading;
    cam.view_pitch = e.pitch;
    cam.view_roll = e.roll;
    cam.valid = true;
    return cam;
}

} // namespace opennova::world
