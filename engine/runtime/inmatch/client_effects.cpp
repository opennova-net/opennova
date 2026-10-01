// Ordered client effects: class deaths, explosions, one-shots, revive, and radio.
#include "client_runtime.h"
#include <runtime/world/world.h>
#include <runtime/world/infantry_sound.h>
#include <runtime/world/radio_call.h>
#include <runtime/world/ai.h>
#include <runtime/world/infantry.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/audio/footstep_slot.h> // organic_slot_set (the org1 scream slot)
#include <runtime/audio/sound_profile.h> // compose_entity_sound_set (the player scream)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <variant>

namespace opennova::inmatch {
namespace {
// Remote organic rows deliberately have no World twin. Project the receive
// context from the canonical row; the local avatar may have a different native
// handle from its wire identity. HostClient can use its existing world entity.
// A pool-1..3 handle resolves to its materialized twin FIRST: retail reads the
// real pool row behind the handle, and the explosion sound leg consumes that
// row's +0x26C damage ammo (the 0x21 source is the credited lastAttacker or
// null, never the exploding item), which the row snapshot does not carry.
// [orig: NapiNPClientMsg_HandleSpawnEffect @0x430B10, pool resolve @0x430bec;
//  Entity_SpawnExplosionEffects @0x4399C0, +0x26C read @0x439a12 ->
//  Sound_PlayWithDistanceAttenuation @0x439a28; the two 0x21 senders
//  Entity_HandleDeathEvent @0x407233 (lastAttacker) and
//  Entity_HandleDeathOnAuthority @0x407d0e (null)]
bool sound_actor(const ClientRuntime &runtime, const world::World &world,
        uint16_t handle, world::Entity &out) {
    const auto native_handle = runtime.has_self_handle() && handle == runtime.self_handle()
            ? world.cached.local_player : world::EntityHandle{handle};
    if (native_handle == world.cached.local_player) {
        if (const auto *local = world.registry.get(native_handle)) { out = *local; return true; }
    }
    if (native_handle.pool() >= 1 && native_handle.pool() <= 3)
        if (const auto *twin = world.registry.get(native_handle)) { out = *twin; return true; }
    if (const auto *row = runtime.state().find(handle)) {
        out.handle = world::EntityHandle{row->handle};
        out.item_id = row->type_id;
        out.has_item_def = row->type_id != 0;
        out.position = {float(row->x) / 65536.0f, float(row->y) / 65536.0f, float(row->z) / 65536.0f};
        out.team = row->team_known ? row->team : 0;
        out.flags = row->state_flags;
        out.engine_flags = row->rm_entity_flags;
        out.anim_slot = row->spawn_anim_slot;
        out.player_class = row->spawn_player_class;
        out.equipped_adm_index = row->equipped_adm_index;
        out.radio_request = row->radio_request;
        out.radio_request_seconds = row->radio_request_seconds;
        if (row->mount_bone != 0) {
            out.mount_target = world::EntityHandle{row->carrier_handle};
            out.mount_type = world::SeatType::None;
            if (const auto *parent = world.registry.get(out.mount_target))
                for (const auto &seat : parent->seats)
                    if (seat.bone_index == row->mount_bone) { out.mount_type = seat.type; break; }
        }
        return true;
    }
    if (const auto *entity = world.registry.get(native_handle)) { out = *entity; return true; }
    return false;
}
} // namespace

// Context flag 7: active capture entry, inside its cylinder, with a visible
// minimap slot. The NEAREST qualifying entry (strict < on the truncated 2D
// distance) decides, and its Q16 coverage ((radius - dist) << 16) / radius is
// what the caller tests, zero on the radius itself.
// [orig: Entity_FindNearestProximityEntity @0x5380C0 (nearest store @0x5381C6,
//  coverage tail @0x5381F8..0x538225); caller VMacros_BuildShaderPassName @0x5BF5D0 -
//  push 7 @0x5BF918, call @0x5BF925, consumed only by the 0x10010 arm
//  @0x5BF9D4..0x5BF9DE]
bool in_active_radio_zone(const world::World &world, const world::Entity &speaker,
        const ClientRuntime &runtime) {
    int32_t best = 0x40000000;
    const world::Entity *nearest = nullptr;
    for (const auto &pair : runtime.zone_states()) {
        const auto &entry = pair.second.entry;
        if (!(entry.window_active || entry.value_active) ||
                (entry.value_active && entry.value_target == entry.value_limit)) continue;
        const auto *zone = world.registry.get(world::EntityHandle{pair.first});
        if (!zone) continue;
        const double dx = double(world::to_fixed(zone->position.x)) - world::to_fixed(speaker.position.x);
        const double dy = double(world::to_fixed(zone->position.y)) - world::to_fixed(speaker.position.y);
        // The x87 length is clamped at flt_7C19E0 (0x7FFF0000) before ftol.
        const int32_t dist = int32_t(std::min(std::sqrt(dx*dx+dy*dy), 2147418112.0));
        const int32_t radius = int32_t(zone->zone_radius) << 16;
        if (dist > radius ||
                std::abs(double(world::to_fixed(zone->position.z)) - world::to_fixed(speaker.position.z)) > radius/2) continue;
        const auto matches = [&](const auto &slot) {
            return slot.active && slot.handle == pair.first &&
                (!(zone->item_attrib & 0x40000u) || !zone->zone_number || (slot.flags & 0xC0u));
        };
        bool visible = false;
        for (const auto &slot : runtime.state().minimap.transient) if (matches(slot)) visible = true;
        for (const auto &slot : runtime.state().minimap.special) if (matches(slot)) visible = true;
        if (!visible || dist >= best) continue;
        best = dist;
        nearest = zone;
    }
    if (!nearest) return false;
    const int32_t radius = int32_t(nearest->zone_radius) << 16;
    // radius == 0: retail's idiv faults; the bounded port reports no coverage.
    if (best > radius || radius == 0) return false;
    return ((int64_t(radius - best) << 16) / radius) != 0;
}

namespace {
// The map's tracked target: a live, unhidden entity other than the local
// player, its timer 62 x its radio seconds while it holds the radio-request
// latch else 496 ticks, its position snapshot, friendly when on the local
// team or team 0, the colour palette[3] for a radio request else white.
// [orig: HUD_SetTrackedEntityTarget @0x59D050 — gates @0x59d05e..0x59d078,
//  timer @0x59d089..0x59d0a5, snapshot @0x59d0b2..0x59d0c4, friendly
//  @0x59d0ca..0x59d0e3, colour @0x59d0ef..0x59d0ff]
void set_tracked_entity_target(replication::ClientState &state, const world::World &world,
        const world::Entity &entity) {
    const world::Entity *local = world.registry.get(world.cached.local_player);
    if (local == nullptr) return;
    if (((entity.flags | entity.engine_flags) & world::kEntityFlagCarried) != 0) return;
    if (entity.handle == local->handle) return;
    auto &target = state.tracked_target;
    target.handle = entity.handle.packed;
    target.ticks_remaining = entity.radio_request == 1
            ? 62u * static_cast<uint32_t>(entity.radio_request_seconds) : 496u;
    target.position[0] = world::to_fixed(entity.position.x);
    target.position[1] = world::to_fixed(entity.position.y);
    target.position[2] = world::to_fixed(entity.position.z);
    target.friendly = entity.team == local->team || entity.team == 0;
    target.color = entity.radio_request == 1 ? hud::kHudPaletteLightBlue : 0xFFFFFFFFu;
    ++target.serial;
}

// PlayerSlot_IsEntityInGame: the entity's def is a person, type 3
// [orig: @0x434220..0x434232 — entity+0x20 def, def+0x5C == 3]. A world
// entity carries its def type; a decoded pool-0 row is an organic, whose
// items.def rows are persons.
bool entity_is_person(const ClientRuntime &runtime, const world::World &world,
        const world::Entity &entity) {
    if (const auto *row = runtime.state().find(entity.handle.packed))
        if (world.registry.get(entity.handle) == nullptr)
            return row->type_id != 0 &&
                    (row->cls == EntityClass::Player || row->cls == EntityClass::Infantry);
    return entity.has_item_def && entity.item_type == 3;
}

// The roster slot driving a raw pool-0 index, the retail PlayerSlot_FindByEntityPtr
// over the decoded roster.
const replication::ClientRosterSlot *slot_for_pool0(const replication::ClientState &state,
        uint8_t index) {
    for (const auto &slot : state.roster)
        if (slot.bound && slot.entity_slot == index) return &slot;
    return nullptr;
}
} // namespace

void ClientRuntime::tick_remote_stance_sounds(world::World &world) {
    // The scream leg of every remote body's death edge this tick, at the body
    // origin: a player body composes "<prefix>_DEATH" ("_DEATH_K" on a night
    // mission) from its anim-slot byte, an org1 body plays its profile slot 7
    // (8, SSNightDead, at night). The edge runs on every machine, so a client
    // screams for its remote rows itself; nothing rides the wire for it.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b4c4a..0x4b4c54 ->
    //  SoundProfile_FindByEntityAndType @0x528180 type 5/0 ->
    //  Entity_PlaySound3D_FullVolume; Entity_UpdateInfantryAI @0x4b9ca3..0x4b9cb5
    //  -> Entity_GetProfileSlotSound(entity, 8/7)]
    const bool night = (world.tables.mission_attrib_flags &
            world::MissionTables::kMissionAttribEnableNVG) != 0;
    for (const uint16_t handle : view_.drain_death_edges()) {
        const replication::ClientEntityState *row = state().find(handle);
        if (row == nullptr) continue;
        world::SoundSlotEvent scream;
        scream.source_handle = handle;
        scream.pos[0] = row->x;
        scream.pos[1] = row->y;
        scream.pos[2] = row->z;
        if (row->cls == EntityClass::Player) {
            scream.slot = static_cast<uint8_t>(night ? audio::kSlotNightDeath : audio::kSlotDeath);
            audio::compose_entity_sound_set(row->spawn_anim_slot,
                    night ? audio::kEntitySoundDeathNight : audio::kEntitySoundDeath,
                    scream.set_name, sizeof(scream.set_name));
        } else {
            const int slot = night ? audio::kSlotNightDeath : audio::kSlotDeath;
            const std::string *set = audio::organic_slot_set(world.tables.sound_profiles,
                    world.tables.organic_sound_profiles, row->type_id, false, slot);
            if (set == nullptr) continue; // the resolved-id-0 silence
            scream.slot = static_cast<uint8_t>(slot);
            std::snprintf(scream.set_name, sizeof(scream.set_name), "%s", set->c_str());
        }
        world.out.slot_sounds.push_back(scream);
    }
    for (auto &row : state().entities) {
        if (row.cls != EntityClass::Player || (row.state_flags & 1u) != 0 ||
                (has_self_handle() && row.handle == self_handle())) continue;
        // The prone clear reads the +0x16C MOUNT parent's def, never the ground
        // link the wire carrier also names; mount_bone is the row's mounted test
        // (the gate's Health(+0x11E) > 0 leg is the dead-row skip above).
        // [orig: Entity_UpdateInfantryPlayerBody parentEntity gate
        //  @0x4B41A2..0x4B41C0; prone clear @0x4B4709..0x4B471B]
        const auto *parent = row.mount_bone != 0
                ? world.registry.get(world::EntityHandle{row.carrier_handle}) : nullptr;
        const int32_t pos[3] = {row.x, row.y, row.z};
        world::emit_stance_change_sound(world, row.handle, pos, row.stance_sound_state,
                row.net_stance_bits, row.rm_entity_flags, parent != nullptr && parent->has_item_def);
    }
}

void ClientRuntime::apply_received_effects(world::World &world) {
    for (const auto &request : view_.drain_effect_commands()) {
        if (const auto *squad = std::get_if<replication::ClientSquadEvent>(&request)) {
            apply_squad_event(world, *squad);
        } else if (const auto *death = std::get_if<replication::EntityDeathEvent>(&request)) {
            // Authority already ran its callback. Remote organics use the
            // compact pose; pools 1..3 carry the materialized item/vehicle twin.
            // [orig: NapiNPClientMsg_EntityDeath @ 0x42EB50;
            // Entity_KillBySlotId @ 0x42BCE0]
            if (world.rules.logic_authority) continue;
            const world::EntityHandle handle{death->entity_handle};
            if (handle.pool() < 1 || handle.pool() > 3) continue;
            world::Entity *victim = world.registry.get(handle);
            if (!victim) continue;
            if (death->item_state) {
                world::apply_item_state_event(world, *victim, death->hit_section,
                        death->kill_flags);
            } else {
                victim->health = 0;
                victim->alive = false;
                victim->death_anim_state = death->death_anim_state_id;
                victim->last_attacker = world::EntityHandle{};
                world::destruction_notify_item_damage(world, *victim, 4);
            }
        } else if (const auto *door = std::get_if<replication::DoorRowUpdate>(&request)) {
            // The pool row the handle names; an entity this peer never
            // materialized has no door records to write.
            // [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x4312bf..0x431326]
            const world::EntityHandle handle{door->entity_handle};
            if (!handle.valid() || handle.pool() >= world::kEntityPoolCount) continue;
            if (const world::Entity *entity = world.registry.get(handle))
                world.doors.apply_wire_row(*entity, door->number, door->state);
        } else if (const auto *flash = std::get_if<replication::LightningTimerCommand>(&request)) {
            // [orig: NapiNPClientMsg_HandleTextCommand SETFLASH1 @0x429eea /
            //  @0x429ef5 -> g_EnvLightningTimerA]
            world.weather.command_set_flash_timer(flash->timer_a);
        } else if (const auto *effect=std::get_if<ExplosionEffectRecord>(&request)) {
            if (!world.rules.logic_authority && effect->type==0) {
                world::Entity snapshot;
                const auto *source=sound_actor(*this,world,effect->source,snapshot) ? &snapshot : nullptr;
                world::spawn_item_explosion(world,source,{effect->x,effect->y,effect->z},
                        int32_t(uint32_t(uint16_t(effect->heading))<<16),effect->count,false);
            }
        } else if (std::holds_alternative<replication::MedicVoiceRequest>(request)) {
            // [orig: NapiNPClientMsg_0x03A @0x422680; DialogSystem_Init @0x5275F0]
            const auto *local = world.registry.get(world.cached.local_player);
            if (!local) continue;
            world::SoundSlotEvent event;
            std::memcpy(event.set_name, "MEDIC_KIT_USE", sizeof("MEDIC_KIT_USE"));
            event.pos[0] = world::to_fixed(local->position.x);
            event.pos[1] = world::to_fixed(local->position.y);
            event.pos[2] = world::to_fixed(local->position.z);
            world.out.slot_sounds.push_back(event);
            world.script.voice.radio_set(world, "MEDIC_VOICE", local->handle);
        } else if (const auto *event = std::get_if<GameEventRecord>(&request)) {
            const auto *local = world.registry.get(world.cached.local_player);
            world::Entity actor;
            if (local && event->attacker_index != 0xFF &&
                    sound_actor(*this, world, event->attacker_index, actor))
                world::play_flag_event_sound(world, event->event_type, actor, *local,
                        game_type(), event->pos_x, event->pos_y);
        } else if (const auto *call = std::get_if<TrackedPlayerVoice>(&request)) {
            // Pool-0 byte is a raw entity index. Chat and voice have separate
            // local mute flags; an unbound speaker can still play a voice.
            // [orig: NapiNPClientMsg_HandleEntityDeath @0x430C50]
            world::Entity context;
            if (!sound_actor(*this, world, call->player_index, context) || !context.item_id) continue;
            auto *speaker = &context;
            const replication::ClientRosterSlot *roster = slot_for_pool0(state(), call->player_index);
            const bool in_zone = game_type() == 0x10010 && in_active_radio_zone(world, *speaker, *this);
            if (roster && !(roster->radio_mute_flags & 2)) {
                const auto key = world::radio_call_key(world, *speaker, call->event, 6, game_type(), in_zone);
                const std::string empty;
                const std::string *location = call->location < 0 ? nullptr :
                    size_t(call->location) < state().location_names.size() ?
                    &state().location_names[size_t(call->location)] : &empty;
                view_.post_chat_line({2, call->player_index,
                    world::radio_call_text(world, key, roster->name, location)});
            }
            if (!roster || !(roster->radio_mute_flags & 1)) {
                const auto key = world::radio_call_key(world, *speaker, call->event, 3, game_type(), in_zone);
                world.script.voice.radio_set(world, key, speaker->handle);
                speaker->radio_request = call->event == 6 ? 1 : 0;
                if (call->event == 6) {
                    speaker->radio_request_seconds = 30;
                    // [orig: HUD_SetTrackedEntityTarget @0x430de4]
                    set_tracked_entity_target(view_.state(), world, *speaker);
                }
                if (auto *row = view_.state().find(call->player_index)) {
                    row->radio_request = speaker->radio_request;
                    row->radio_request_seconds = speaker->radio_request_seconds;
                } else if (auto *entity = world.registry.get(speaker->handle)) {
                    entity->radio_request = speaker->radio_request;
                    entity->radio_request_seconds = speaker->radio_request_seconds;
                }
            }
        } else if (const auto *emote = std::get_if<EmoteBroadcast>(&request)) {
            // A nearby player's emote: its person's emote state, then, unless
            // its slot's voice-mute bit is set, the EMO_ voice at the speaker
            // and the map's tracked target.
            // [orig: NapiNPClientMsg_HandleEmote @0x427E90 -- Pool_GetEntryUnchecked
            //  @0x427eeb, the ItemTypeIndex gate @0x427ef5, the state write
            //  @0x427efb..0x427f18, PlayerSlot_FindByEntityPtr @0x427f29, the
            //  slot+50 bit 0 gate @0x427f39, sub_5BFB00(.., 9, ..) +
            //  Audio_StartEntityPlayback @0x427f44..0x427f55,
            //  HUD_SetTrackedEntityTarget @0x427f5b]
            world::Entity speaker;
            if (!sound_actor(*this, world, emote->player_index, speaker) || !speaker.item_id) continue;
            // The state lands on the speaker's secondary channel: a world body
            // (the listen host's players, a joiner's own) through the AI
            // system, a joiner's decoded peer on its row
            // (world::infantry_weapon_emote_stamp / stamp_row_emote).
            const bool row_speaker = role_ == Role::Joiner &&
                    speaker.handle != world.cached.local_player;
            if (row_speaker) {
                view_.stamp_row_emote(emote->player_index, emote->emote);
            } else if (world::AiEntity *body = world.ai.for_handle(speaker.handle)) {
                world::infantry_weapon_emote_stamp(body->inf, world.ai.root_motion, emote->emote);
            }
            const replication::ClientRosterSlot *roster = slot_for_pool0(state(), emote->player_index);
            if (roster != nullptr && (roster->radio_mute_flags & 1u) != 0) continue;
            // The EMO_ key with the body prefix (flags 9), then the voice
            // anchored at the speaker [orig: @0x427f3b..0x427f55].
            const bool in_zone = game_type() == 0x10010 &&
                    in_active_radio_zone(world, speaker, *this);
            world.script.voice.entity_set(world,
                    world::radio_call_key(world, speaker, emote->emote, 9, game_type(), in_zone),
                    speaker.handle, speaker.position, row_speaker);
            set_tracked_entity_target(view_.state(), world, speaker);
        } else if (const auto *tip = std::get_if<replication::TipEventCommand>(&request)) {
            // The receive legs' tip events join the world's in arrival order
            // (replication::TipEventCommand carries the witness).
            world.out.tip_events.push_back(tip->event);
        } else if (const auto *chat = std::get_if<replication::LocalChatSpeaker>(&request)) {
            // A local-channel line: the sender slot's person becomes the
            // tracked target (the fold already ran the dispatcher's slot
            // gate). [orig: Chat_DispatchToChannel @0x42B910 — slot+0x24
            //  @0x42b9ee, PlayerSlot_IsEntityInGame (def type 3) @0x42b9fb,
            //  HUD_SetTrackedEntityTarget @0x42ba09]
            const replication::ClientRosterSlot &slot = state().roster[chat->slot];
            if (!slot.bound || slot.entity_slot < 0) continue;
            world::Entity speaker;
            if (!sound_actor(*this, world, static_cast<uint16_t>(slot.entity_slot), speaker)) continue;
            if (!entity_is_person(*this, world, speaker)) continue;
            set_tracked_entity_target(view_.state(), world, speaker);
        } else {
            // [orig: NapiNPClientMsg_PlaySoundByName @0x4283A0]
            const auto &command = std::get<PlaySoundCommand>(request);
            if (command.flag == 0) {
                world::ScriptSoundEvent event;
                event.name = command.sound_name;
                event.kind = world::ScriptSoundEvent::Kind::Interface;
                world.out.script_sounds.push_back(std::move(event));
            } else {
                world::SoundSlotEvent event;
                if (command.sound_name.size() >= sizeof(event.set_name)) continue;
                std::memcpy(event.set_name, command.sound_name.data(), command.sound_name.size());
                event.pos[0] = int32_t{command.pos_x} * 65536;
                event.pos[1] = int32_t{command.pos_y} * 65536;
                event.pos[2] = int32_t{command.pos_z} * 65536;
                world.out.slot_sounds.push_back(event);
            }
        }
    }
    // An entity voice anchored on a decoded row follows that row, and stops
    // once the row is gone or dead, as a freed or dead anchor stops it.
    // [orig: Audio_UpdateAmbientStream @0x4ED9E8..0x4EDA2A]
    const world::EntityHandle anchor = world.script.voice.speaker();
    if (anchor.valid() && role_ == Role::Joiner) {
        const replication::ClientEntityState *row = state().find(anchor.packed);
        const bool present = row != nullptr && row->type_id != 0 &&
                (row->state_flags & world::kEntityFlagDead) == 0;
        world.script.voice.track_row_anchor(anchor, present, present
                ? world::Vec3{float(row->x) / 65536.0f, float(row->y) / 65536.0f,
                        float(row->z) / 65536.0f}
                : world::Vec3{});
    }
}
} // namespace opennova::inmatch
