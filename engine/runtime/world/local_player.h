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
#include <runtime/world/radar_contacts.h>
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
    // The radar contact table, edge timers, sector rings and missile list the
    // damage/whiz/missile legs fill and the spinmap's bit-10 legs draw
    // (world/radar_contacts.h) [orig: the HUD globals @0x2721EEC..0x2723EAC].
    RadarContactState radar;
    hud::HudMapControl hud_map_control;
    uint64_t round_reset_revision = 0;
    LocalPlayerViewTracker view_tracker;
    PlayerLookSettings look_settings;
    int aspect_mode = -1; // session video setting; survives player reinitialization
    // What the view arbiter reads from the embedder's session (death screen,
    // end-round, the death-camera target): a live embedder refreshes this
    // before each session frame; the bare kernel keeps the no-session default.
    LocalViewSessionInputs view_session_inputs;
    // The post-tick pump's wire-facing outcome, overwritten every pump: a
    // serving embedder relays the reload onto its loopback (the witnessed
    // local reload producer -> the S2C 0x49 broadcast); the bare kernel drops
    // it, having already applied it.
    LocalWeaponReloadWire last_reload;
    // The Attack & Defend side word the TEAMID line reads — 1 defending, 2
    // attacking, 0 outside game type 0x10002 or with no target — latched once
    // per mission start [orig: dword_B78FE8, written only by sub_524110,
    // called once from Game_StartMission @0x5260C1].
    uint32_t attack_defend_role = 0;
    // The latch [orig: sub_524110 @0x524110]: the first def-bearing entity
    // whose items.def attrib carries the TARGET bit 0x8000 — pool 2 first,
    // then pool 1 — against the local player's team: the same team defends
    // (1), any other attacks (2) [orig: `test [eax+54h], 8000h` @0x524160 /
    // @0x524190; `cmp al, [edx+162h]; setnz; add ecx, 1` @0x5241A3..0x5241AC].
    // Retail dereferences g_LocalPlayerEntity unguarded; with no local player
    // yet (a joiner's pre-spawn start) the word stays 0.
    void latch_attack_defend_role(uint32_t game_type);

    bool has_local_player() const;
    Entity *player();
    const Entity *player() const;
    AiEntity *player_ai();
    Vec3 player_position() const; // mission space (Z-up)
    std::string player_anim_key() const; // "anim_<state>"
    int32_t player_health() const;
    // This frame's held keys (look rides look()); the pre-tick folds them into
    // `input_flags`.
    PlayerInput input;
    // The input-flag word the held keys fold into every frame and the pack
    // reads and clears [orig: g_InputFlags @0xB3B728 / g_InputFlagsPrev @0xB3B72C].
    PlayerInputFlags input_flags;
    // The local player's MoveOrder word (entity+0x12C) as the last pack wrote
    // it. It persists between packs: the motor, the keyboard look rotation and
    // the wire mirror read it every tick, and only a pack rewrites it.
    // [orig: Player_PackInputStateToEntity @0x4df68f..0x4df790 -- the sole
    //  writer of the local player's movement bits; Entity_ApplyFreeLookRotation
    //  @0x4ae090 and Entity_UpdateInfantryPlayerBody @0x4b40e0 read it per tick]
    PlayerBodyInput move_order;
    // One frame of movement keys: stores the keys plus the sim-owned stance
    // latch onto `input` and refreshes the view aggregates, whose binocular
    // suppression reads the input word this frame's fold will leave. The
    // movement-held latch and the unscope-on-move belong to the pack
    // (apply_player_input_pre_tick). [orig: Player_UpdatePerFrame
    // `test byte ptr g_InputFlags, 1Eh` @0x4de3ae]
    void set_movement_keys(bool forward, bool back, bool left, bool right,
            bool lean_left, bool lean_right, bool jump);
    // The zero-step keys [orig: Player_AdjustWeaponZoomLevel @0x4dbcc0]: the
    // -1 floor keys on the session's sniper-zoom rule bit; a change clicks
    // GF_SCOPE_ZERO, moves the look pitch by the elevation delta and
    // recomputes the equipped slot's zero-yaw term (WeaponSlotState::zero_yaw,
    // retail MountSlot+8). Returns whether the zero changed.
    bool request_scope_zero(int delta);
    void set_view_keys(bool free_look, bool up, bool down, bool left, bool right);
    // The stance keys' action ids, the C2S 0x1D body: 0 stand / 1 crouch /
    // 2 prone -> 172 / 169 / 170. [orig: Input_HandleActionBinding_0 cases
    //  172 @0x4e0e3e, 169 @0x4e0d77 (`push 0A9h` @0x4e0dbb), 170 @0x4e0df3]
    static constexpr uint16_t kStanceActionIds[3] = {0xAC, 0xA9, 0xAA};
    // The bare kernel's stance SELECT (0 stand / 1 crouch / 2 prone), which
    // has no session to loop the C2S 0x1D through: the keys' refusals, then
    // the handler's mutual-exclusion apply at once. Returns whether the latch
    // changed. A session's players send the 0x1D instead (HostRole /
    // JoinerRole::request_stance).
    // [orig: input cases 169/170/172 @0x4e0d77.. -> NapiNPServerMsg_HandleStanceChange
    // @0x501c60; the ForceCrouch gate Entity_CheckWeaponSeatFlags(equipped,
    // 0x40000) @0x4e0d8a; the `parentSlot == 3` gate @0x4e0da0..0x4e0db5]
    bool request_stance(int stance);
    // The stance keys' own refusals, ahead of the C2S 0x1D send: a ForceCrouch
    // weapon or the UseGun seat. [orig: @0x4e0d8a; @0x4e0da0..0x4e0db5]
    bool stance_request_allowed() const;
    // The stance latches and MoveOrder's stance bits from one stance pair
    // (bit 0 prone, bit 1 crouch), written together by both their writers:
    // the authority's 0x1D handler when the sender is its own player, and on
    // a non-authority client every 0x0A whose header carries the recipient
    // tail. [orig: NapiNPServerMsg_HandleStanceChange @0x501c60 -- MoveOrder
    //  @0x501cc9..0x501d01, the latches @0x501d1b / @0x501d2d;
    //  NapiNPClientMsg_0x00A @0x430549..0x43058f]
    void latch_stance(uint8_t bits);
    // The sim-owned stance latch (0 stand, 1 crouch, 2 prone) — the
    // dword_B76484 prone-latch equivalent the render-slot drape gate reads.
    int stance_latch() const { return stance_latch_; }
    // A vehicle attach or detach of the local player clears both latches
    // with MoveOrder's stance bits: the player stands on mounting and on
    // dismounting. [orig: Entity_ProcessVehicleAttach @0x435c42..0x435c59;
    //  Entity_DetachFromVehicle @0x43560c..0x435624]
    void clear_stance_latches();
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
    // out-of-session UseGun rejection (rules.mp_session, the retail
    // is_in_session fact, gates it).
    bool toggle_mount();
    // Numbered seat keys share the panel's list and their own idle/overheat
    // gate; a joiner uses the query then waits for the authority's reply.
    bool find_numbered_seat(int index, VehicleSeatSelection &out,
            const VehicleOccupancySource *source = nullptr);
    bool select_numbered_seat(int index);
    // The RENDERED frame: the binocular sway latch, the main scene's camera
    // compose (and the Inset scene's), then the main scene's optical offsets.
    // It advances the composition state, so the presenter's per-frame leg is
    // its one live caller [orig: Render_ProcessMainSceneFrame @0x5CA0F0].
    LocalPlayerViewFrame present_view_frame();
    // The same frame OBSERVED: the camera is the last composed view and
    // nothing advances (weapon-event placement, mode refresh, diagnostics).
    LocalPlayerViewFrame view_frame();
    // The Player_IsOpticalViewVisible verdict the body updater and the HUD share
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
    // The frame's input onto the local player's body before the logic tick
    // (the view-flag stamps ride along); no local player = no-op. The held
    // keys fold into `input_flags` every frame; `pack_input` runs the pack
    // (the word into `move_order` and its movement legs -- the movement-held
    // latch, the binocular toggle drop, the unscope-on-move and the hip-fire
    // camera legs -- then the clear), which retail runs only
    // inside the client network frame's send block -- every frame for the
    // host and single player, every send-holdoff period for a joiner. The
    // persisted `move_order` reaches the body every frame either way.
    // [orig: Input_ProcessFrame @0x49d541; Client_ProcessNetworkFrame
    //  @0x42c3dd gate -> Player_PackInputStateToEntity @0x42c3e9]
    void apply_player_input_pre_tick(bool pack_input);
    // The body-pass scoped/binocular drift, after weight dispersion and before
    // upper-body decay. Writes persistent aim, including the next input fold.
    // [orig: Entity_UpdateInfantryPlayerBody @ 0x4B40E0, block @0x4B5966..0x4B5C97]
    void apply_scoped_aim_drift(AiEntity &body, uint32_t logic_tick);
    // Preserve the retail process globals across a kernel replacement: the
    // scoped-aim oscillators and the input pack's analog hysteresis. Other
    // local input, weapon and view state still belongs to the new mission.
    void carry_process_globals_from(const LocalPlayer &previous);
    // The post-tick local view in retail order: the sim-wrote-the-view fold,
    // then the per-frame view promoter and the camera compose.
    void run_local_view_tick();
    // The authority's own-slot fire gate the embedding role decides each frame
    // ahead of the weapon walk (true outside an MP listen host).
    // [orig: Entity_FireWeaponAndSendPacket @0x42be3a]
    bool authority_fire_admitted = true;
    // The equipped-slot FSM pump: the local player's visit in the world's
    // weapon-action walk (World::pump_weapon_actions calls it at the local
    // player's own pool-0 slot), after run_local_view_tick.
    // [orig: WeaponAction_ProcessAllEntities @0x542690 -> WeaponAction_ProcessFrame
    //  @0x540E60 for g_LocalPlayerEntity's slot]
    void pump_local_weapon();
    // One 62.5 Hz tick of the view state over view_session_inputs, then the
    // aim acquisition and the quantum's camera compose, before the weapon
    // pump (the order the world tick keeps: retail's Player_UpdatePerFrame
    // call precedes the later WeaponAction_ProcessAllEntities call). The
    // joiner frame runs it between its heading fold and its own weapon pump.
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
    // The input pack's analog hysteresis: written only by the pack, never
    // reset. [orig: byte_B79442 (axes latch), byte_B79440 (throttle latch),
    //  byte_B79445/44/43 (the last stored X/Y/Z), byte_B79441 (the last throttle)]
    struct AnalogPackState {
        bool axes_active = false;
        bool throttle_active = false;
        int8_t last_x = 0, last_y = 0, last_z = 0;
        int8_t last_throttle = 0;
    };
    AnalogPackState analog_pack_;
    // The pack's analog legs onto the local player's entity+0x130..+0x133.
    // [orig: Player_PackInputStateToEntity @0x4df793..0x4df8f8]
    void pack_analog_axes(Entity &entity, bool moving);
};

} // namespace opennova::world
