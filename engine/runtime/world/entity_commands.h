#pragma once

#include <cstdint>

#include <runtime/world/entity.h>
#include <runtime/world/vehicle_mount.h> // SeatSelectionMode default args
#include <runtime/world/weather_state.h>  // WeatherColorTarget

// EntityCommands: the shared host-authoritative command layer. Split from
// the world.h umbrella (W3-7); World holds it by value and world.h
// re-includes this header.

namespace opennova::world {

// Pool-3 marker def type the BMS PARTICLE_EFFECT action (27) selects.
// [orig: EventAction_SpawnParticleEffect (ex sub_4540E0) @0x4540e0 -- `result[20] == 6088`]
inline constexpr int32_t kParticleEffectMarkerTypeId = 6088;

class World;

// An authored SSN is resolved at dispatch; a bound WAC operand already names
// a pool slot. Preserve that distinction through shared commands: converting
// a handle back to an SSN can select another row with the same authored id.
class EntityTarget {
public:
    EntityTarget(uint16_t ssn) : ssn_(ssn) {}
    EntityTarget(EntityHandle handle) : handle_(handle), bound_(true) {}
    bool bound() const { return bound_; }
    EntityHandle handle() const { return handle_; }
    uint16_t ssn() const { return ssn_; }

private:
    uint16_t ssn_ = 0;
    EntityHandle handle_;
    bool bound_ = false;
};

// ----------------------------------------------------------------------------
// Shared entity-command primitive layer. Models the original Entity_* mutation
// functions (Entity_KillByNetId, Entity_SetWaypointByTeam, ...) that BOTH
// EventAction_Dispatch and the WacScript_*
// handlers funnel through. WAC handlers and BMS actions both call these.
// ----------------------------------------------------------------------------
class EntityCommands {
public:
    explicit EntityCommands(World &world) : world_(world) {}

    // The script-facing local-player SSN [orig: the dfx2med player-slot
    // convention — the SP player entity carries 10000 as its net id].
    static constexpr uint16_t kLocalPlayerSsn = 10000;

    // Script SSN -> entity handle (the WAC SSN*/BMS Single resolve), including
    // the retail player mapping: SSN 10000 = the local player. Our player
    // entities carry net_id 0 (the wire is handle-based), so the mapping lives
    // here at the script seam. [orig: EntityPool_FindByNetId @0x4f0a20]
    EntityHandle resolve_ssn(uint16_t ssn) const;
    EntityHandle resolve_target(EntityTarget target) const;

    // --- entity (authored SSN or already-bound handle) ---
    bool set_ssn_name(EntityTarget ssn, const std::string &name);
    bool ssn_critical(EntityTarget ssn) const;
    bool ssn_has_rider(EntityTarget target_ssn) const;
    bool order_boarding(EntityTarget source_ssn, EntityTarget target_ssn);
    // These arm the existing row's corpse lifecycle; they do not spawn immediately.
    bool set_ssn_respawns(EntityTarget ssn, int32_t count);
    void set_group_respawns(int32_t group, int32_t count);
    // BMS KillSingle: the first matching row; pool 0 also loses its attacker and
    // staged death clip; the class event fires phase 1 (pool 3: phase 4).
    // [orig: Entity_KillByNetId @0x43DBD0]
    bool kill_ssn(EntityTarget ssn);
    // WAC killSSN: the ItemTypeIndex gate, Health 0, lastAttacker cleared (and a
    // person's staged death clip), then the class event (e, 1, 0) with a cleared
    // hit record; the IDB name is a misnomer. [orig: Entity_ResetWeaponState @0x4F1E40]
    bool wac_kill_ssn(EntityTarget ssn);
    // The shared destroy (retail Entity_Destroy): no network notification.
    bool remove_ssn(EntityTarget ssn);
    // The script removal: S2C 0x12 to the joiners, then the shared destroy.
    // [orig: Server_RemoveEntityAndNotify @0x50a270]
    bool server_remove_and_notify(EntityTarget ssn);
    // BMS VaporizeSingle: the first pool 0..3 row carrying the SSN is removed
    // with the notification. [orig: find_entity_by_parent_and_dispatch @0x43e210]
    bool remove_bms_ref(int32_t ssn);
    // WAC SSNHP: the health word, the attacker cleared; no gate.
    // [orig: WacCmd_SsnHp @0x4F2100]
    bool set_ssn_hp(EntityTarget ssn, int32_t hp);
    // WAC SSNADDHP: the ItemDef gate, the 16-bit add floored at 0 and capped at
    // the def healthMax (each returns 1); an unclamped add clears the attacker
    // and returns 0. [orig: WacCmd_SsnAddHp @0x4F2170]
    int32_t add_ssn_hp(EntityTarget ssn, int32_t delta);
    // WAC accuracy writes the controller-slot error pair as max(0, 100-value).
    // [orig: WacCmd_SetAccuracy @0x4F2070]
    bool set_ssn_accuracy(EntityTarget ssn, int32_t primary, int32_t secondary);
    // WAC guard toggles entity Flags bit 0x40 even when the row has no AI brain.
    // [orig: WacCmd_SsnGuard @0x4F71C0]
    bool set_ssn_guard(EntityTarget ssn, bool guard);
    // Structural BMS single-entity actions.
    bool set_ssn_team(uint16_t ssn, int32_t team);
    bool set_ssn_group(uint16_t ssn, int32_t group);
    bool teleport_ssn_to_marker(uint16_t ssn, int32_t marker_wp_number);
    // WAC teleSSN has a witnessed marker self-copy; BMS uses the real teleport above.
    bool wac_teleport_ssn(EntityTarget source, int32_t marker_wp_number);
    bool play_ssn_soundset(EntityTarget target, const std::string &set);
    // WAC SSNtoWP: the node is always the nearest of the list; no detach and no
    // cooldown/carrier resets, then the brain copy and the turn-budget seed.
    // [orig: WacCmd_SsnToWp @0x4F1CE0]
    bool set_ssn_waypoint(EntityTarget ssn, int32_t wp);
    // BMS RedirectSingleTo (event action 19): the first pool-0 slot holder whose
    // net id matches takes the order and ends the walk (detach, resets, brain copy,
    // no budget); otherwise every matching pool-1 slot holder takes it with the
    // budget seed. `node < 0` selects the nearest node; BMS carries param3.
    // [orig: Entity_SetWaypointForTeam @0x43DD00, dispatched @0x4547DB]
    int redirect_ssn_to_waypoint(int32_t ssn, int32_t wp, int32_t node);
    bool set_ssn_engage_min(EntityTarget ssn, int32_t v);
    bool set_ssn_engage_max(EntityTarget ssn, int32_t v);
    bool set_ssn_attack_max(EntityTarget ssn, int32_t v);
    bool set_ssn_anim(EntityTarget ssn, int32_t anim_slot);
    bool set_local_anim(int32_t anim_state);
    bool raise_local_player();
    bool set_ssn_turn(EntityTarget ssn, int32_t heading_degrees);
    bool teleport_local_to_ssn(EntityTarget ssn);
    bool set_ssn_hidden(EntityTarget ssn, bool hidden);
    // WAC holdSSN/unholdSSN: cause_flags bit 0x2000 behind the ItemTypeIndex
    // gate. [orig: WacCmd_HoldSsn @0x4F7810; WacCmd_UnholdSsn @0x4F7870]
    bool set_ssn_held(EntityTarget ssn, bool held);
    bool set_ssn_disabled(EntityTarget ssn, bool disabled);

    // BMS 42..49 preserve raw source ids and write one selector word. Single
    // scans stop at the first pool-0/1 match; group scans visit all matches.
    // No alive, item-definition or brain gate. Zero source selects nothing.
    // [orig: EventAction_Dispatch @0x4542E0 -> 0x43D770..0x43DAC0]
    bool set_ssn_target_selector(int32_t ssn, AiTargetSelector field, int32_t value);
    int set_group_target_selector(int32_t group, AiTargetSelector field, int32_t value);

    // --- entity (by handle; the tool/probe mutation seam, ADR 0042 d5) ---
    // Write an entity's health through BOTH stores the scripted SETHP path
    // touches — the registry row (alive follows hp) and the AI motor's
    // entity+286 mirror [orig: the WAC SETHP op writes entity+286]. False when
    // the handle resolves no registry row.
    bool set_entity_health(EntityHandle h, int32_t hp);
    // Move an entity through both position stores (registry float + the AI
    // 16.16 mirror), mission-space coordinates. False when the handle resolves
    // no registry row.
    bool set_entity_position(EntityHandle h, const Vec3 &mission_pos);
    // Kill a PLAYER entity outright as `killer`'s victim: the health write the
    // real damage path would have made (so the recipient's 0x0A tail health --
    // the joiner's death channel -- reads the death too) plus the RoundDeath
    // record the round-end and kill-feed consumers key on. False when the
    // handle resolves no player row.
    bool kill_player(EntityHandle victim, EntityHandle killer);
    // Write the primary weapon slot's clip/reserve counts (retail's signed
    // i16 words). False when the handle resolves no registry row.
    bool set_entity_weapon_ammo(EntityHandle h, int32_t clip, int32_t reserve);
    // Override ONE entity's ItemDefAttrib words (items.def `attrib:` bits) in
    // place: both raw dwords plus the per-entity facts derived from them go
    // through the same stamp the trait sweep uses (world::stamp_item_attrib),
    // so live readers (NoDismember, NoDie, the AS zone gates, ...) see the
    // new value on their next read. Per-item caches keyed by item id
    // (item_death_traits, vehicle_traits) and the AI profile mirror stay as
    // the sweep left them; the override is not replicated (joiners re-stamp
    // from their own items.def) and the next resolve_item_traits sweep
    // (mission load, net topology sync) re-stamps it from the def. False when
    // the handle resolves no registry row.
    bool set_entity_item_attrib(EntityHandle h, uint32_t attrib, uint32_t attrib2);

    // --- the WAC weather handlers (world::WeatherState carries the cites) ---
    // Every environment command lands here: the VM's handlers, the BMS
    // actions, the F3 window and the MCP rows all mutate the ONE weather home
    // through these (ADR 0042 d5), which also keep the observable EnvState
    // mirror the behavior tests read.
    void set_fog_type(int32_t type);                       // fogtype
    void set_fog_distance(int32_t metres);                 // fogdist
    void move_fog(int32_t metres, int32_t seconds);        // whole-metre host control
    void set_fog_distance_q16(int32_t distance_q16);       // WAC fogdist
    void move_fog_q16(int32_t distance_q16, int32_t seconds); // WAC movefog
    void set_rain(int32_t percent, int32_t seconds);       // rain
    void set_snow(int32_t percent, int32_t seconds);       // snow
    void set_overcast(int32_t percent, int32_t seconds);   // overcast
    void set_sky_speed(int32_t rate);                      // skyspeed
    void set_fov(int32_t degrees);                        // fov
    void set_sky_height(int32_t height_raw);               // skyheight
    void quake(int32_t seconds);                           // quake
    void set_time_of_day_minutes(int32_t minute_of_day);   // TOD
    void debug_set_time_of_day_minutes(double minute_of_day);
    void sun_fade(int32_t percent, int32_t seconds);       // sunfade
    void set_color_fade(int32_t seconds);                  // colorfade
    void set_lightning_color(uint32_t rgb);                // lightning
    void lightning_flash();                                // flash
    void lightning_far_flash();                            // farflash
    void set_weather_color(WeatherColorTarget target, uint32_t rgb); // sun/sky/ground/floor/ceiling/cloud/fog/skyfog/gain
    void set_wind_scale(int32_t value);                    // the `wind` named value

    // --- queries ---
    // WAC SSNexists: a resolved row with an ItemTypeIndex.
    // [orig: WacCmd_SsnExists @0x4F1A70]
    bool ssn_exists(EntityTarget ssn) const;
    // The BMS SingleAlive/SingleDestroyed predicates (the `alive` latch).
    bool ssn_alive(EntityTarget ssn) const;
    bool ssn_dead(EntityTarget ssn) const;
    // WAC SSNdead/SSNalive: the Flags dead bit behind the ItemTypeIndex gate.
    // [orig: WacCmd_SsnDead @0x4F1AC0; WacCmd_SsnAlive @0x4F1B20]
    bool wac_ssn_dead(EntityTarget ssn) const;
    bool wac_ssn_alive(EntityTarget ssn) const;
    // [orig: WacCmd_SsnWounded @0x4F1B80] Signed 16-bit health <= the signed
    // def healthMax word halved.
    bool ssn_wounded(EntityTarget ssn) const;
    // BMS area predicates: all matching rows in pools 0/1, rather than the
    // general first-match SSN resolver used by bound WAC commands.
    bool ssn_in_area(int32_t ssn, int area_id) const;
    bool group_in_area(int32_t group, int area_id) const;
    bool ssn_in_script_area(EntityTarget target, int32_t zone_id, bool three_dimensional) const;
    bool ssn_at_location(EntityTarget target, int32_t location) const;
    void update_local_location(EntityHandle player);
    // True only when the mission has at least one ACTIVE area trigger and the
    // local player's X/Y sits inside none of them — Z is ignored, and a world
    // with no local player (a serve-only host) reads as in-bounds. Feeds the
    // BMS player-AWOL counter. [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40]
    bool local_player_out_of_bounds() const;

    // --- the cat-2 single-state trigger queries (EventTrigger cat 2;
    // bms-event-runtime-re §3b — every helper's RAW sense is POSITIVE, the
    // authored chain-negation bit does the flipping) ---
    // [orig: Entity_IsAliveByBmsRef @0x43e640] The BMS SingleAlive /
    // SingleDestroyed read: the first pool 0/1/2 row carrying the SSN answers
    // with its dead flag; SSN 0 and an SSN no row carries (never placed,
    // rejected at admission, or removed) read NOT alive.
    bool bms_ref_alive(int32_t ssn) const;
    // [orig: Entity_IsSsnAtAlertLevel @0x43e780] No AI component (aiRuntime
    // null) -> false; else the per-entity controller alert byte == level
    // (2 red / 1 yellow / 0 green).
    bool ssn_at_alert(uint16_t ssn, int level) const;
    // [orig: Entity_HasDamageCapacity @0x43e3d0] health <= healthMax - points,
    // signed — "has lost at least points HP". No alive gate (a corpse keeps
    // satisfying it), mirroring the original expression.
    bool ssn_damage_taken_at_least(uint16_t ssn, int32_t points) const;
    // [orig: Entity_HasFullHealth @0x43e470] health >= healthMax; an
    // unresolved def (our health_max == 0 marker; retail's null itemDef) -> false.
    bool ssn_full_health(uint16_t ssn) const;
    // [orig: Entity_HasHealthAboveThreshold @0x43e350] health >= threshold
    // (the def is not involved).
    bool ssn_health_at_least(uint16_t ssn, int32_t threshold) const;
    // [orig: Entity_IsSsnHoldingItemGroup @0x43e2f0] The carried-object link
    // (mounted_child) is set and the held object's command group == group.
    bool ssn_holding_group(uint16_t ssn, int group) const;
    // [orig: TriggerGroup_AnyMemberHoldingItemGroup @0x43c870] Any resolved
    // (item_type_index != 0, the retail ItemTypeIndex +0x1C gate) member of
    // holder_group holding an object of held_group; first match wins.
    bool group_holding_group(int holder_group, int held_group) const;
    // [orig: Entity_IsOnTopOfChain @0x4f19a0] target reachable from ssn's
    // groundEntity chain (ground_target) within 3 hops; both entities gated
    // on item_type_index != 0.
    bool ssn_on_chain_of(EntityTarget ssn, EntityTarget target_ssn) const;
    // Distances use Q16: WAC resolves literals; BMS shifts its whole metres.
    // [orig: Entity_CheckProximity @0x4F14C0] Wrapped center deltas, clamped
    // and truncated Euclidean length <= the raw distance operand.
    bool ssn_within_distance(EntityTarget ssn, EntityTarget target_ssn, int32_t distance_q16) const;
    bool ssn_leads_target(EntityTarget first, EntityTarget second,
                         EntityTarget target, int32_t lead_q16) const;
    // [orig: Entity_CheckLineOfSightInRange @0x4f15e0] Center distance
    // <= distance_q16 AND a radius-0 LOS ray between the two entities is clear.
    // Retail rays between the +0x1FC bbox-center offset points and picks the
    // entity-aware walker at <= 20 u vs terrain/sectors above — our port rays
    // through the one modeled LOS seam; the walker split remains tracked in §3b.
    bool ssn_los_clear_within(EntityTarget ssn, EntityTarget target_ssn, int32_t distance_q16) const;
    // [orig: Entity_CheckLineOfSight @0x4f17c0] ssn_los_clear_within PLUS the
    // facing gate: |wrap32(heading_bam - atan2(dy, dx)·(2^31/pi))| <= 30.0
    // deg (0x15555540 BAM), int32 wrap = shortest arc. Retail's cdq/xor/sub
    // abs leaves INT_MIN negative, so a target EXACTLY 180.0 deg astern
    // passes the signed compare — the quirk is carried bit-exactly.
    bool ssn_sees_within(EntityTarget ssn, EntityTarget target_ssn, int32_t distance_q16) const;

    // --- group (by group id) ---
    // WAC kill / BMS KillGroup: pools 2, 0, 1, every row of the group (dead rows
    // too): Health 0 and the class event (e, 1, 0); returns the rows visited.
    // [orig: Entity_KillAllByNetId @0x43C8E0]
    int kill_group(int group);
    // BMS RedirectGroupTo (event action 1) and WAC GtoWP. `node < 0` selects the
    // nearest node on the list; BMS passes its authored param3. Pool-0 members
    // detach, reset their cooldown/carrier words and seed the turn budget;
    // pool-1 members only take the slot and brain words.
    // [orig: Entity_SetWaypointByTeam @0x43CD20, dispatched @0x454315]
    int group_to_waypoint(int group, int32_t wp, int32_t node = -1);
    // WAC GroupHP: pools 0-2, the health word of every matching row; returns the
    // rows written. [orig: WacScript_SetEntityTeamSlot @0x4F7B30 (IDB misnomer)]
    int set_group_hp(int group, int32_t hp);
    int set_group_engage_min(int group, int32_t v);
    int set_group_engage_max(int group, int32_t v);
    int set_group_attack_max(int group, int32_t v);
    // WAC/BMS structural group actions. Pool coverage and dead-row rules are
    // kept inside these primitives so both script runtimes share one behavior.
    int remove_group(int group);
    int set_group_accuracy(int group, int32_t primary, int32_t secondary);
    bool set_group_move_speed_kph(int group, int32_t kph);
    int set_group_team(int group, int32_t team);
    int change_group(int old_group, int new_group);
    int teleport_group_to_marker(int group, int32_t marker_wp_number);
    // WAC groupdead/groupalive: the trigger group's live count (<= 0 / > 0),
    // the 62-tick rescan's word. [orig: WacCmd_GroupDead @0x4ED1A0;
    // WacCmd_GroupAlive @0x4ED1C0]
    bool group_dead(int group) const;
    bool group_alive(int group) const;

    // --- mount / emplacement (AttachToEmplaced) ---
    // [orig: WacScript_TryMountEntityToVehicle @0x4f70f0] Attach occupant_ssn into target_ssn's best
    // free root/child seat through the canonical vehicle attach operation. Reject if the
    // occupant is already mounted or the target has no free seat. Returns false on any reject.
    bool mount(EntityTarget occupant_ssn, EntityTarget target_ssn,
               SeatSelectionMode mode = SeatSelectionMode::Any);
    // Port-side helper for authored "Goto SSN and board" commands 123/124/125, not a retail
    // symbol. Retail path: Entity_UpdateInfantryAI @0x4ba9ad -> Entity_FindBestSeatSlot
    // @0x4351f0 -> Entity_RequestVehicleAttach @0x4364a0. FindBestSeatSlot applies the rules:
    // 123 only accepts `sitex`, 124 rejects `ctrlx`, and 125 uses normal best-seat priority.
    bool mount_boarding_command(EntityTarget occupant_ssn, EntityTarget target_ssn, uint8_t command_id);

    // WAC `ssnrelease` -- the release half of the AI boarding order.
    // [orig: WacCmd_SsnRelease @0x4f7420]
    bool release_boarding_command(EntityTarget occupant_ssn);
    // SSNUse mounts the controller's cached target, preserving its chosen child.
    // [orig: WacScript_TryMountEntityToVehicle @0x4F70F0]
    bool use_boarding_target(EntityTarget occupant);

    // BMS action 27 (PARTICLE_EFFECT): spawn the authored marker emitters whose
    // wp_number matches `wp_number`. Returns how many fired.
    // [orig: EventAction_Dispatch case 0x1B @0x4542e0 -> EventAction_SpawnParticleEffect]
    int spawn_marker_particle_effects(int32_t wp_number);
    const Entity *script_target(int32_t wp_number) const;
    int effect_at_ssn(int32_t effect, const std::string &name, EntityTarget ssn);
    int effect_at_target(int32_t effect, const std::string &name, int32_t target);
    int rain_effect(int32_t effect, const std::string &name, uint32_t random);
    int sound_at_target(int32_t sound, const std::string &name, int32_t target);
    int direct_sound(const std::string &name, int32_t distance, int32_t bearing);
    int fire_ammo_at_target(int32_t ammo, int32_t target);
    int fire_ammo_from_ssn(int32_t ammo, EntityTarget source, EntityTarget target);
    int fire_ammo_in_area(int32_t ammo, int32_t area_id);
    int rain_ammo_near_player(int32_t ammo, uint32_t random);
    // [orig: Entity_DetachFromVehicle @0x4355f0] Free the occupant's seat + clear its mount ref.
    bool dismount(EntityTarget occupant_ssn);
    // [orig: Vehicle_HasEnemyOccupant @0x4359f0] SSN of an entity riding target_ssn, else 0.
    uint16_t find_mounted_on(uint16_t target_ssn) const;

    // --- the BMS Player mount triggers (main type 7 subs 38-41) ---
    // All four resolve ssn, require a live local player, and test its mount/stand state
    // against the SSN entity, one carrier link deep. [orig: EventTrigger_EvaluateCondition
    // @0x453620 cat-7 subs 38-41 -> the four predicates @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0
    // (renamed 2026-07-16: Entity_IsLocalPlayerSeatedOnSsn / StandingOnSsn / DrivingSsn /
    // OnGunOfSsn — the shipped IDB names were permuted misnomers).]
    // PLYRATTACHED: seated in ANY seat of the SSN (or of something the SSN carries).
    bool local_player_attached_to_ssn(EntityTarget ssn) const;   // sub 38 @0x4f10d0
    // PLYRONSSN: STANDING on the SSN (ground/carrier reference), not seated.
    bool local_player_standing_on_ssn(EntityTarget ssn) const;   // sub 39 @0x4f1260
    // PLYRDRIVING: seated on the SSN chain in a ctrlx/drvrx seat.
    bool local_player_driving_ssn(EntityTarget ssn) const;       // sub 40 @0x4f1150
    // PLYRONGUN: seated on the SSN chain in the UseGun seat.
    bool local_player_on_gun_of_ssn(EntityTarget ssn) const;     // sub 41 @0x4f11e0

    // --- AI command (the AI-change action family) ---
    // [orig: Entity_ApplyCommand @0x43ab60, reached from EventAction_Dispatch @0x4542e0
    // via Entity_HandleAlertStateEvent @0x43dee0.] Apply an AI sub-type command to the
    // target's AI component, reached through World::ai. p2/p3/p4 are the sub-type's slots
    // (e.g. PLAYPARTANIM: p2=channel, p3=play_type, p4=time). No-op (returns false / 0)
    // when there is no AI system or no brain for the target.
    // The ChangeAI action's sub-type ids, the dfx2med token names
    // [orig: Entity_ApplyCommand @0x43ab60's switch]. Every arm of the switch is
    // carried; subs 1, 3, 4, 7, 9..14, 18..20, 24 and 25 have no editor token and
    // are dispatched by number; 35..39 are the switch's default (no arm).
    enum ChangeAiSub : int {
        kGuardBit = 2,
        kRedAlert = 5,
        kGreenAlert = 6,
        kAccuracy100 = 8,
        kBlindBit = 15,
        kBerserkBit = 16,
        kClimberBit = 17,
        kCowardBit = 21,
        kYellowAlert = 22,
        kClimbChase = 23,
        kDriveSkill = 26,
        kAimSkill = 27,
        kAiSetState = 28,
        kCombatSpeed = 29,
        kPatrolSpeed = 30,
        kFindAndUse = 31,
        kHudItem = 37,
        kTmateStatus = 39,
        kAiNodePath = 40,
        kAttackDistanceValue = 41,
        kEngageDistance = 42,
        kIndestructableBit = 43,
        kTargetSsn = 44,
        kAiStartFiring = 45,
        kAiFiringAngle = 46,
    };
    bool apply_ai_command(EntityTarget ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    // BMS ChangeSingleAI: the first pool 0..2 row carrying the SSN.
    // [orig: Entity_HandleAlertStateEvent @0x43dee0]
    bool apply_bms_single_ai_command(int32_t ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    // BMS ChangeGroupAI: pool 2 rows with an AI component, then pools 0 and 1.
    // [orig: Entity_HandleAlertCommand @0x43cf10]
    int apply_group_ai_command(int group, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    // BMS AreaAiRed/AreaAiBlue over the load-resolved action record: p1/p3
    // bound X and p4/reserved1 bound Y (the zone resolver stored x_min,
    // y_min, x_max, y_max there); pool 0 rows of `team` only.
    // [orig: Entity_KillTeamInBounds @0x43d030]
    int apply_area_ai_command(int team, int sub_type, int32_t p1, int32_t p2, int32_t p3,
                              int32_t p4, int32_t reserved1);

    World &world() { return world_; }

private:
    World &world_;
};

} // namespace opennova::world
