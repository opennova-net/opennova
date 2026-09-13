// The local player: the one body the embedder drives -- its movement keys and
// look, the equipped weapon and loadout, the view state and tracker, the
// stance latch and the medic-call cooldown -- plus the verbs over them, which
// MissionKernel carried before ADR 0043 slice E7. Bound to its world at
// construction; every method body still names that world `world`. The
// kernel keeps the asset-bound legs (the def tables, the .adm clips, the
// spawn entries) and reaches this state as `local`.
#pragma once

#include <runtime/hud/hud_minimap.h>
#include <runtime/world/entity.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_look.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/vehicle_attach.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

class World;

class LocalPlayer {
public:
    explicit LocalPlayer(World &world) : world_(world) {}
    LocalPlayer(const LocalPlayer &) = delete;
    LocalPlayer &operator=(const LocalPlayer &) = delete;

    LocalPlayerWeapon weapon;
    // The resident kit: the mission's loadout/availability chunks promoted
    // through the SP gate at load_weapon_table, then the Player_InitPlayer
    // weapon leg (the spawn-default select + the slot pool the reloads refill).
    LocalPlayerLoadout loadout;
    WeaponInventory inventory;
    bool inventory_valid = false;
    PlayerViewState view;
    hud::HudMapControl hud_map_control;
    uint64_t round_reset_revision = 0;
    LocalPlayerViewTracker view_tracker;
    PlayerLookSettings look_settings;
    int aspect_mode = -1; // session video setting; survives player reinitialization
    // What the view arbiter reads from the embedder's session (death screen,
    // end-round, the death-camera target): a live embedder refreshes this
    // before each session frame; the bare kernel keeps the no-session default.
    LocalViewSessionInputs view_session_inputs;
    // The post-tick pump's wire-facing outcomes, overwritten every pump: a
    // serving embedder relays the reload onto its loopback (the witnessed
    // local reload producer -> the S2C 0x49 broadcast) and a joiner ships the
    // fired round; the bare kernel drops both, having already applied them.
    LocalWeaponFiredWire last_fired;
    LocalWeaponReloadWire last_reload;

    bool has_local_player() const;
    Entity *player();
    const Entity *player() const;
    AiEntity *player_ai();
    Vec3 player_position() const; // mission space (Z-up)
    std::string player_anim_key() const; // "anim_<state>"
    int32_t player_health() const;
    // The movement keys the pre-tick packs onto the body (look rides look()).
    PlayerInput input;
    // One frame of movement keys: packs the keys plus the sim-owned stance
    // latch onto `input`, runs the witnessed movement-held unscope (while
    // SETTLED at scope on a Scoped weapon, any direction key routes through
    // the full unscope; the ForceScoped pin keeps pinned sights raised), and
    // refreshes the view aggregates. [orig: Player_PackInputStateToEntity
    // @0x4df450 — g_movementKeyHeld @0x4df29c; the unscope route
    // @0x4df4c9..0x4df4ec; the ForceScoped pin @0x4df12d]
    void set_movement_keys(bool forward, bool back, bool left, bool right,
            bool lean_left, bool lean_right, bool jump);
    // The zero-step keys [orig: Player_AdjustWeaponZoomLevel @0x4dbcc0]: the
    // -1 floor keys on the session's sniper-zoom rule bit; a change clicks
    // GF_SCOPE_ZERO, moves the look pitch by the elevation delta and
    // recomputes the equipped slot's zero-yaw term (WeaponSlotState::zero_yaw,
    // retail MountSlot+8). Returns whether the zero changed.
    bool request_scope_zero(int delta);
    void set_view_keys(bool free_look, bool up, bool down, bool left, bool right);
    // Stance SELECT request (0 stand / 1 crouch / 2 prone): mutual exclusion
    // at apply, REFUSED while the equipped weapon has ForceCrouch or the
    // player sits in the UseGun seat. Returns whether the latch changed.
    // [orig: input cases 169/170/172 @0x4e0d77.. -> NapiNPServerMsg_HandleStanceChange
    // @0x501c60; the ForceCrouch gate Entity_CheckWeaponSeatFlags(equipped,
    // 0x40000) @0x4e0d8a; the `parentSlot == 3` gate @0x4e0da0..0x4e0db5]
    bool request_stance(int stance);
    // The sim-owned stance latch (0 stand, 1 crouch, 2 prone) — the
    // dword_B76484 prone-latch equivalent the render-slot drape gate reads.
    int stance_latch() const { return stance_latch_; }
    // Mouse pixels onto the look angles (the center-lock accumulator).
    void look(float dx_px, float dy_px);
    // Point the look straight at a mission-space target from a mission-space
    // eye (absolute heading + pitch, engine BAM frame).
    void aim_at(const Vec3 &eye, const Vec3 &target);
    void teleport_local_player(const Vec3 &mission_pos, double yaw_deg,
            double pitch_deg);
    void set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed);
    // The USE-ITEM mount toggle [orig: Input_ProcessFrame release edge
    // @0x49d6dc -> Entity_ToggleVehicleMount @0x436950], including the
    // out-of-session UseGun rejection (session_open gates it).
    bool toggle_mount();
    // Numbered seat keys share the panel's list and their own idle/overheat
    // gate; a joiner uses the query then waits for the authority's reply.
    bool find_numbered_seat(int index, VehicleSeatSelection &out,
            const VehicleOccupancySource *source = nullptr);
    bool select_numbered_seat(int index);
    LocalPlayerViewFrame view_frame();
    // The Player_CanFireWeapon verdict the body updater and the HUD share
    // [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80; Sighted helper
    // @0x4dcd30].
    bool local_player_can_fire();
    // The seat/armory labels the HUD draws around the local player: nothing
    // for a dead or absent player; armory mode is the raw entity flag [orig:
    // is_armory_mode = entity Flags & 0x400000 @0x5a32c4]; the nearest-only
    // gate is the CanFire verdict above, computed here so camera changes
    // cannot lag one logic tick.
    void collect_attach_labels(std::vector<AttachLabel> &out, const VehicleOccupancySource *source = nullptr);
    // The authority's read of the local player's dead bit (the entity flags;
    // a joiner reads its replica through inmatch::ClientRuntime::local_player_dead).
    bool local_player_dead() const;
    // --- the medic call (the dead player's C2S 0x2E) -------------------------
    // The retail client medic-call cooldown: 310 ticks stamped at the send
    // [orig: Input_HandleActionBinding case 217 @0x49b511 `dword_B76804 =
    // 0x136`; decremented once per frame in Player_UpdatePerFrame @0x4de73e;
    // cleared on the local death path @0x4b4d06; net-re 0x2E].
    static constexpr int kMedicRequestCooldownTicks = 0x136;
    int medic_request_cooldown_ticks = 0;
    int medic_request_serial = 0;
    // The action gates past the session/entity checks: a dead local player
    // with the cooldown at zero [orig: case 217 @0x49b4b4..0x49b4da].
    bool medic_request_allowed(bool local_dead) const {
        return local_dead && medic_request_cooldown_ticks == 0;
    }
    // The send stamp: the cooldown and the serial the HUD keys its line on.
    void stamp_medic_request();
    // One per tick: the local death edge zeroes the cooldown, else it counts
    // down [orig: Player_UpdatePerFrame @0x4de736..0x4de744; @0x4b4d06].
    void tick_medic_cooldown(bool local_dead);

    // --- the per-tick legs a session frame orders around its pump -----------
    // Pack the frame input onto the local player's body before the logic tick
    // (the view-flag stamps ride along); no local player = no-op.
    void apply_player_input_pre_tick();
    // The body-pass scoped/binocular drift, after weight dispersion and before
    // upper-body decay. Writes persistent aim, including the next input fold.
    // [orig: Entity_UpdateInfantryPlayerBody @ 0x4B40E0, block @0x4B5966..0x4B5C97]
    void apply_scoped_aim_drift(AiEntity &body, uint32_t logic_tick);
    // Preserve the retail process-global oscillators across a kernel replacement.
    // Other local input, weapon and view state still belongs to the new mission.
    void carry_scoped_aim_drift_from(const LocalPlayer &previous);
    // The post-tick local pumps in retail order: the sim-wrote-the-view fold,
    // the per-frame view promoter, then the equipped-slot FSM pump.
    void run_local_player_post_tick();
    // One 62.5 Hz tick of the view state over view_session_inputs, before the
    // weapon pump (the order the world tick keeps: retail's
    // Player_UpdatePerFrame call precedes the later
    // WeaponAction_ProcessAllEntities call). The joiner frame runs it between
    // its heading fold and its own weapon pump.
    void tick_view();
    void update_aim_target();
    // Reset the frame-input state and seed the look heading from the (auto-)
    // spawned local player's facing — the session bring-up's tail.
    void reset_local_player_input_to_player_facing();
    // The local deployment reset, after the body snap and before kit rebuild.
    // Retains NVG, camera-shake filters, map zoom, and physical held keys.
    // [orig: Game_InitNewRound @0x422740]
    void reset_for_new_round();
    // The same reset with an explicit heading (the joiner's spawn/redeploy
    // edges hand the authoritative facing in).
    void reset_local_player_input(int32_t look_heading_bam);
    // The post-tick "sim wrote the view" fold, exposed for the embedder's
    // mount-change edges (the joiner's authoritative attach echo).
    void sync_local_mounted_input_heading();

private:
    World &world_;
    float look_accum_x_ = 0.0f;
    float look_accum_y_ = 0.0f;
    // The sim-owned stance latch (0 stand, 1 crouch, 2 prone).
    int stance_latch_ = 0;
    bool medic_dead_edge_seen_ = false;
    // These process globals have no round, weapon, scope-toggle or player-spawn
    // reset writer. Each axis resets its drift only when it observes a changed
    // stance at its own period boundary. [orig: 0xA860F8..0xA86118]
    struct ScopedAimAxis {
        int32_t drift = 0;
        int32_t step = 0;
        int32_t limit = 0;
        uint16_t stance_bits = 0;
        bool decreasing = false;
    };
    ScopedAimAxis scope_yaw_;
    ScopedAimAxis scope_pitch_;
};

} // namespace opennova::world
