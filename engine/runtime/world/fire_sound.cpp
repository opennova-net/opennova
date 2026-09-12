// The fire-sound legs on the logic clock — witness map in world/fire_sound.h.
#include <runtime/world/fire_sound.h>

#include <runtime/world/ammo_table.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>
#include <runtime/audio/oneshot_play.h>

#include <cmath>

namespace opennova::world {

void FireSoundQueue::push_ready(const char *set_name, const Vec3 &pos,
                                int32_t source_bms_id, uint32_t sound_id,
                                bool interface_set) {
    if (set_name == nullptr || set_name[0] == '\0') return;
    // The peer gate lives INSIDE the play: a host with no stamped listener
    // (dedicated) readies nothing, whichever leg asked [orig: the
    // is_mp_session_peer test at the head of Sound_Play3DPositional
    // @ 0x527cb3, which every Entity_PlaySound3D_FullVolume call reaches].
    if (!listener_valid_) return;
    if (ready_.size() >= kMaxReady) return;
    ReadyFireSound sound;
    sound.interface_set = interface_set;
    sound.set_name = set_name;
    sound.pos = pos;
    sound.source_bms_id = source_bms_id;
    sound.sound_id = sound_id;
    ready_.push_back(std::move(sound));
}

void FireSoundQueue::play_immediate(const char *set_name, const Vec3 &pos,
                                    int32_t source_bms_id, uint16_t source_handle) {
    push_ready(set_name, pos, source_bms_id,
               audio::oneshot_sound_id(source_handle, source_bms_id));
}

void FireSoundQueue::play_with_distance_delay(const char *set_name,
                                              const Vec3 &pos,
                                              int32_t source_bms_id,
                                              uint16_t source_handle) {
    if (set_name == nullptr || set_name[0] == '\0') return;
    // The peer gate lives INSIDE the witnessed function: a host with no
    // stamped listener (dedicated) plays nothing [orig: the
    // is_mp_session_peer test at the head of Sound_PlayWithDistanceAttenuation
    // @ 0x528e57].
    if (!listener_valid_) return;
    // Truncating distance in integer units — retail loads the 16.16 deltas as
    // floats, sqrts, truncates the fixed value and shifts the fraction off
    // [orig: @ 0x528e7c..0x528ec0 (ftol then >> 16)].
    const double dx = static_cast<double>(pos.x - listener_.x);
    const double dy = static_cast<double>(pos.y - listener_.y);
    const double dz = static_cast<double>(pos.z - listener_.z);
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int32_t dist_units =
            dist >= 2147483647.0 ? 2147483647 : static_cast<int32_t>(dist);
    // The set's max-range gate ran here in retail (soundDef+72 @ 0x528ec6);
    // our audio bank culls at play time instead — the tracked D-AI-8 delta.
    if (dist_units >= kMinDelayDistUnits) {
        // [orig: the delay formula @ 0x528ef2 — integer division, then >> 2,
        //  the witnessed quarter-compression of physical travel time]
        int32_t countdown = (62 * dist_units / kSoundSpeedUnits) >> 2;
        if (countdown == 0) countdown = 1; // [orig: @ 0x527c94]
        for (PendingFireSound &slot : slots_) {
            if (slot.active) continue;
            slot.active = true;
            slot.set_name = set_name;
            slot.pos = pos;
            slot.countdown = countdown;
            slot.source_bms_id = source_bms_id;
            slot.interface_set = false;
            return;
        }
        // Full pool: the retail allocator returns without a slot — the sound
        // is dropped [orig: @ 0x527c47].
        return;
    }
    // The near leg forwards the entity into the full-volume play [orig:
    // @ 0x528f07]; the delayed slot above stored none.
    push_ready(set_name, pos, source_bms_id,
               audio::oneshot_sound_id(source_handle, source_bms_id));
}


void FireSoundQueue::play_throttled_interface(const char *set_name, const Vec3 &pos,
        int32_t delay_ticks, int32_t suppression_ticks) {
    // [orig: Server_TrackEntityInTable @ 0x527B30]
    if (!set_name || !*set_name) return;
    TriggerHold *free = nullptr;
    for (auto &hold : trigger_holds_) {
        if (hold.name == set_name) return;
        if (hold.name.empty() && !free) free = &hold;
    }
    if (!free) return;
    free->name = set_name;
    free->countdown = suppression_ticks;
    // [orig: EffectSlot_AllocateAndInit @ 0x527C30]
    for (auto &slot : slots_) {
        if (slot.active) continue;
        slot.active = true;
        slot.set_name = set_name;
        slot.pos = pos;
        slot.countdown = delay_ticks == 0 ? 1 : delay_ticks;
        slot.source_bms_id = 0;
        slot.interface_set = true;
        return;
    }
}

void FireSoundQueue::tick() {
    // [orig: sub_5292D0 @ 0x5292D0] Only old countdown 1 clears a hold.
    for (auto &hold : trigger_holds_) {
        if (hold.name.empty()) continue;
        const int32_t old = hold.countdown;
        hold.countdown = static_cast<int32_t>(static_cast<uint32_t>(old) - 1u);
        if (old == 1) hold.name.clear();
    }
    for (PendingFireSound &slot : slots_) {
        if (!slot.active) continue;
        const int32_t old = slot.countdown;
        slot.countdown = static_cast<int32_t>(static_cast<uint32_t>(old) - 1u);
        if (old != 1) continue;
        // Flags bit2 plays as an interface set on a presenting peer. Positional
        // slots use the recorded position and play with a NULL entity: no
        // own-channel key [orig: Sound_TickPendingSlots @ 0x529310, the NULL
        // entity @ 0x52937b]. push_ready carries the peer gate for both.
        push_ready(slot.set_name.c_str(), slot.pos, slot.source_bms_id, 0, slot.interface_set);
        slot.active = false;
        slot.set_name.clear();
    }
}

std::vector<ReadyFireSound> FireSoundQueue::drain() {
    std::vector<ReadyFireSound> out;
    out.swap(ready_);
    return out;
}

void FireSoundQueue::clear() {
    for (auto &hold : trigger_holds_) hold = {};
    for (PendingFireSound &slot : slots_) {
        slot.active = false;
        slot.set_name.clear();
    }
    ready_.clear();
    // The listener survives a mission restart on purpose: retail's listener
    // global is camera state, not per-mission state.
}

int FireSoundQueue::pending_count() const {
    int count = 0;
    for (const PendingFireSound &slot : slots_) {
        if (slot.active) ++count;
    }
    return count;
}



void play_flag_event_sound(World &world, uint8_t event, const Entity &actor,
        const Entity &local, uint32_t game_type, int16_t x, int16_t y) {
    // [orig: NetPacket_HandleGameEvent @ 0x426270, cases 19..21]
    const bool own = actor.handle == local.handle;
    const bool teammate = (game_type & 0x10000u) != 0 && actor.team == local.team;
    const char *cue = nullptr;
    const char *voice = nullptr;
    if (event == 19) {
        if (own) {
            if (game_type == 65540) { cue = "FLAG_DO_P"; voice = "FLAG_VXDO_P"; }
            else if (game_type == 65544 || game_type == 8) {
                cue = "FLAG_WIN_P"; voice = "FLAG_VWIN_P";
            }
        } else if (teammate) {
            cue = "FLAG_DO_T";
            if (game_type == 65540) { cue = "FLAG_WIN_T"; voice = "FLAG_VXDO_T"; }
            else if (game_type == 65544) { cue = "FLAG_WIN_T"; voice = "FLAG_VWIN_T"; }
        } else if (game_type == 65540) {
            cue = "FLAG_DO_OT"; voice = "FLAG_VXDO_OT";
        } else if (game_type == 65544 || game_type == 8) {
            cue = "FLAG_DO_OT"; voice = "FLAG_VWIN_OT";
        }
    } else if (event == 20) {
        cue = own ? "FLAG_PU_P" : teammate ? "FLAG_PU_T" : "FLAG_PU_OT";
        if (game_type == 65540)
            voice = own ? "FLAG_VXPU_P" : teammate ? "FLAG_VXPU_T" : "FLAG_VXPU_OT";
    } else if (event == 21) {
        cue = own ? "FLAG_SV_P" : teammate ? "FLAG_SV_T" : "FLAG_SV_OT";
        voice = own ? "FLAG_VXSV_P" : teammate ? "FLAG_VXSV_T" : "FLAG_VXSV_OT";
    } else return;
    // DialogSystem_Init resolves these fixed names against the loaded bank
    // chain before any event. Missing sets do not occupy the suppression table.
    // [orig: DialogSystem_Init @ 0x5275E0, table @ 0x82F590]
    const auto exists = [&world](const char *name) {
        return name && world.tables.sound_sets && world.tables.sound_sets->has(name);
    };
    if (exists(cue)) {
        if (own) {
            ScriptSoundEvent sound;
            sound.kind = ScriptSoundEvent::Kind::Interface;
            sound.name = cue;
            world.out.script_sounds.push_back(std::move(sound));
        } else {
            // Wire XY are signed whole units; the third coordinate is ZERO.
            // The misnamed HUD_DrawDefaultProgressBar is a positional sound wrapper.
            // [orig: HUD_DrawDefaultProgressBar @ 0x527E60 -> Sound_Play3DPositional]
            world.out.fire_sounds.play_immediate(cue, {float(x), float(y), 0.0f}, 0);
        }
    }
    if (exists(voice))
        world.out.fire_sounds.play_throttled_interface(voice, local.position, 62, 3720);
    if (event == 19 && own)
        world.script.waypoints.select_nearest_enemy_base(world.registry, local, game_type);
}

// [orig: WeaponAction_Fire @0x542ccc..0x542ce9;
// WeaponAction_ProcessFrame @0x541262..0x54132a]
void weapon_sound_publish(World &world, const Entity &owner,
                          const WeaponFsmDef &def, const WeaponFsmEvents &events) {
    if (!world.out.fire_sounds.listener_valid()) return;
    if (events.head_started)
        world.out.fire_sounds.play_immediate(def.soundhead, owner.position, owner.bms_id,
                                             owner.handle.packed);
    if (events.trailoff_started)
        world.out.fire_sounds.play_immediate(def.soundtrailoff, owner.position, owner.bms_id,
                                             owner.handle.packed);
    if (events.fireloop_lifetime_ticks == 0) return;
    const Entity *source = &owner;
    if (owner.mount_type == SeatType::Gunner) {
        const Entity *parent = world.registry.get(owner.mount_target);
        if (parent != nullptr) source = parent;
    }
    SoundEmitterEvent event;
    event.source_spawn_id = source->registry_spawn_id;
    event.source_handle = source->handle.packed;
    event.pos = owner.position;
    event.source_bms_id = source->bms_id;
    event.emitted_tick = world.logic_tick + 1;
    event.lifetime_ticks = events.fireloop_lifetime_ticks;
    event.pitch_q16 = 0x10000;
    event.volume_q8_8 = 0xFFFF;
    event.set_name = def.soundfireloop;
    world.out.sound_emitters.publish(std::move(event));
}

void fire_sound_on_spawn(World &world, const RoundSpawnParams &params) {
    FireSoundQueue &queue = world.out.fire_sounds;
    // The dedicated-server gate is NOT here: retail tests is_mp_session_peer
    // inside the plays (Sound_Play3DPositional @ 0x527cb3,
    // Sound_PlayWithDistanceAttenuation @ 0x528e57), so a listener-less host
    // still runs the adm arm's kick/heat replay below and push_ready readies
    // nothing.
    // The local player's own fire keeps its action-slot presentation; the
    // shell self-filtered exactly this case before the move.
    // [orig: ActionSlot_ExecuteActionTick @ 0x541A70 routing]
    if (world.cached.local_player.valid() &&
            params.owner == world.cached.local_player)
        return;
    const Entity *shooter = world.registry.get(params.owner);
    const int32_t source_bms_id = shooter != nullptr ? shooter->bms_id : 0;
    const bool adm_arm =
            (params.wire_round_flags & round_event_flag::kAltFire) == 0 &&
            (params.wire_round_flags & round_event_flag::kAdmIndexed) != 0;
    if (adm_arm) {
        // The adm-indexed arm plays no ammo-def sound. It executes the
        // addressed def's FIRE and RECOIL action rows; each row plays its
        // soundset at begin AND its soundsetend (the receive path stamps the
        // one-shot mode so the end shim runs immediately), all positional at
        // the SHOOTER'S entity position with no distance delay — JOX fire
        // rows author the audible gunshot in soundsetend.
        // [orig: ActionSlot_ExecuteAction @ 0x4020a0 — soundset @ 0x4020ef,
        //  the +90==64 one-shot stamp gate @ 0x4020ff into
        //  ActionSlot_PlayEndSoundAndDupes @ 0x401100 (both at entity+4);
        //  rows +684/+688 @ 0x42f777/@ 0x42f785 and @ 0x42f98f/@ 0x42f9d0]
        const WeaponTableEntry *def = world.tables.weapons.by_index(params.adm_index);
        if (def == nullptr) return;
        // The shooter's entity position; a decoded wire shooter with no local
        // entity supplies its row position through the spawn params, and the
        // wire fire origin (the eye) is the last resort.
        const Vec3 pos = shooter != nullptr ? shooter->position
                : params.shooter_pos_valid ? params.shooter_pos
                                           : params.origin;
        const auto *source = params.source_state;
        if (source && !source->action_allowed) return;
        WeaponSlotState temporary;
        WeaponSlotState &slot = source && source->action_slot ? *source->action_slot : temporary;
        const bool mounted = source && source->mounted_action;
        const WeaponFsmDef &slot_def = mounted && source->action_slot_def ?
                *source->action_slot_def : def->action_fsm;
        slot.current = weapon_action::kFire;
        slot.phase = weapon_phase::kHeld;
        constexpr int32_t kRows[2] = {weapon_action::kFire, weapon_action::kRecoil};
        for (const int32_t row_id : kRows) {
            if (mounted && row_id == weapon_action::kRecoil)
                slot.current = slot.next = weapon_action::kRecoil;
            const WeaponFsmAction &row = def->action_fsm.actions[row_id];
            queue.play_immediate(row.soundset, pos, source_bms_id, params.owner.packed);
            queue.play_immediate(row.soundsetend, pos, source_bms_id, params.owner.packed);
            WeaponFsmEvents events;
            weapon_fsm_replay_action(slot_def, slot, static_cast<int32_t>(world.logic_tick), events);
            if (events.head_started)
                queue.play_immediate(slot_def.soundhead, pos, source_bms_id, params.owner.packed);
        }
        if (mounted) slot.current = slot.next = weapon_action::kIdle;
        return;
    }
    // The ammo-def arm (wire bit 0) and host/AI-originated fire (zero flags)
    // both play ammoDef+64 at the fire origin through the distance gate.
    // [orig: WeaponSlot_FireAndSpawnEffects @ 0x53f440 ->
    //  Sound_PlayWithDistanceAttenuation @ 0x528e40; the wire ammo arm
    //  @ 0x42f5dc]
    const AmmoTableEntry *ammo = world.tables.ammo.by_index(params.ammo_index);
    if (ammo == nullptr || ammo->ai_launch_set.empty()) return;
    queue.play_with_distance_delay(ammo->ai_launch_set.c_str(), params.origin,
            source_bms_id, params.owner.packed);
}

} // namespace opennova::world
