#include "client_replica_pipeline.h"

#include <net/npwire/ingame_message_id.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/entity.h>

namespace opennova::replication {

void ClientReplicaPipeline::apply(uint8_t tag, const std::vector<uint8_t> &body) {
	switch (tag) {
	case s2c::SESSION_CONFIG: { // field 3 = shared g_GameType
		SessionConfig config;
		if (decode_session_config(body.data(), body.size(), config)) {
			game_type_ = static_cast<uint32_t>(config.fields[3]);
			state_.permanent_death = (config.bitflags & 0x8000u) != 0;
			state_.spectators_allowed = (config.bitflags & 0x2000u) != 0;
		} else
			++malformed_bodies_;
		break;
	}
	case s2c::FULL_PLAYER_INFO: { // extra = shared g_GameType
		FullPlayerInfo info;
		if (decode_full_player_info(body.data(), body.size(), info))
			game_type_ = info.extra;
		else
			++malformed_bodies_;
		break;
	}
	case s2c::PER_FRAME_UPDATE:
		apply_frame_update(body);
		break;
	case s2c::PLAY_SOUND: {
		// [orig: NapiNPClientMsg_PlaySoundByName @0x4283A0]
		if (!mp_session_) break;
		PlaySoundCommand command;
		if (!decode_play_sound(body.data(), body.size(), command))
			++malformed_bodies_;
		else if (command.flag <= 1)
			pending_effect_commands_.push_back(std::move(command));
		break;
	}
	case s2c::TRACKED_PLAYER_VOICE: {
        if (!mp_session_) break;
        TrackedPlayerVoice command;
        if (!decode_tracked_player_voice(body.data(), body.size(), command)) ++malformed_bodies_;
        pending_effect_commands_.push_back(command);
        break;
    }
	case s2c::WEAPON_RESTRICTIONS:
		apply_weapon_restrictions(body);
		break;
	case s2c::TEXT_COMMAND:
        apply_text_command(body);
        break;
	case s2c::MEDIC_REVIVING:
		// The receive edge always tries both sounds, even if already latched.
		// The radio voice itself declines while channel zero is occupied.
		// [orig: NapiNPClientMsg_0x03A @0x422680]
		if (decode_medic_reviving(body.data(), body.size())) {
			if (mp_session_) pending_effect_commands_.push_back(MedicVoiceRequest{});
			if (!state_.local_medic_reviving) {
				state_.local_medic_reviving = true;
				state_.mark_changed();
			}
		}
		break;
	case s2c::WORLD_STATE_LOAD: {
		// The 0x0F's client-global fold modeled here: the deploy-map overlay is
		// zeroed, then armed from game_flags bit0 UNLESS the death screen is
		// already up. (The spawn pose / completion burst / waypoint legs live on
		// JoinerConnection; this reducer owns only the retained client globals.)
		// [orig: NapiNPClientMsg_0x00F — g_deploy_screen_active = 0 @0x42e2d8;
		//  `if (game_flags & 1) g_deploy_screen_active = !g_death_screen_active`
		//  @0x42e2f8]
		WorldStateLoad wsl;
		if (decode_world_state_load(body.data(), body.size(), wsl,
				game_type::is_waypoint_family(game_type_))) {
			// [orig: NapiNPClientMsg_0x00F @ 0x42E200, flag store @ 0x42E314] This flag only sets here.
			if (wsl.game_flags & 8u) state_.cease_fire = true;
			state_.location_names = wsl.team_names;
			state_.deploy_check_secured_spawn = (wsl.game_flags & 2u) != 0;
			state_.deploy_overlay_active =
					(wsl.game_flags & 0x01u) != 0 && !state_.death_screen_active;
			// The trigger falling is what clears the open latch
			// [orig: the close-on-clear leg @0x5cac8e -> @0x54b954].
			if (!state_.deploy_overlay_active) state_.deploy_overlay_open_latch = false;
			state_.mark_changed();
		} else {
			++malformed_bodies_;
		}
		break;
	}
	case s2c::WEAPON_RELOAD: { // reload echo (same four-byte body as c2s::WEAPON_RELOAD_REQUEST)
		WeaponReload reload;
		size_t consumed = 0;
		if (decode_weapon_reload(body.data(), body.size(), reload, consumed) &&
		    consumed == body.size())
			pending_weapon_reloads_.push_back(reload);
		else
			++malformed_bodies_;
		break;
	}
	case s2c::TEAM_ASSIGN: {
		TeamAssign assign;
		size_t consumed = 0;
		if (decode_team_assign(body.data(), body.size(), assign, consumed))
			apply_team_assign(assign.entity_handle, assign.team);
		else
			++malformed_bodies_;
		break;
	}
	case s2c::EMPTY_SLOT_SWEEP: {
		DestroyEntityList destroyed;
		if (decode_destroy_entity_list(body.data(), body.size(), destroyed)) {
			for (uint16_t index : destroyed.pool0_indices)
				destroy_pool0_slot(index);
		} else {
			++malformed_bodies_;
		}
		break;
	}
	case s2c::ENTITY_DEATH: {
		// The host's per-death notify for every NON-PLAYER victim (the AI/item
		// leg of Entity_CheckAndProcessDeath) — a destructible's ONLY live death
		// channel beside 0x26; the load-stream 0x10/0x20 batches do not
		// re-stream after load. Retail resolves the pool row (slot < that
		// pool's capacity), zeroes Health, stamps the killer source, and runs
		// the class death callback with reason 4 — for a destructible item that
		// callback IS the local husk-swap/explosion chain the world twin runs
		// when this drains.
		// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — gates @0x42eba2/@0x42ebbb,
		//  Health = 0 @0x42ebd6, deathAnimStateId @0x42ebdf, cb(entity, 4, 0)
		//  @0x42ebf5; sender Entity_CheckAndProcessDeath @0x51b550 (msg 19)]
		EntityDeathRecord death;
		size_t consumed = 0;
		if (!decode_entity_death(body.data(), body.size(), death, consumed)) {
			++malformed_bodies_;
			break;
		}
		apply_entity_death(death.entity_handle, death.death_anim_state_id);
		break;
	}
	case s2c::DEATH_CAMERA_TARGET:
		apply_death_camera_target(body);
		break;
	case s2c::PLAYER_DOWNED_STATE:
		apply_player_downed_state(body);
		break;
    case s2c::EXPLOSION_EFFECT: {
        ExplosionEffectRecord event;
        decode_explosion_effect(body.data(),body.size(),event);
        pending_effect_commands_.push_back(event);
        break;
    }
	case s2c::KILL_SYNC: {
		// The SECOND client death route — the destructible deathCallback's own
		// authority resend rides this tag. Entity_KillBySlotId resolves the
		// slot the same way, zeroes Health, and runs the same cb(entity, 4,
		// flags); the native fold preserves its not-already-dead gate.
		// [orig: NapiNPClientMsg_0x026 @0x42EC30 → Entity_KillBySlotId
		//  @0x42BCE0 — gates @0x42bcfc/@0x42bd15, Flags&2 gate @0x42bd2f,
		//  Health zero @0x42bd33, cb(entity, 4, flags) @0x42bd6a; the
		//  destructible resend Server_SendEntityStatePacket @0x509d70 via
		//  Entity_HandleDestructibleDeathEvent @0x440210]
		KillRecord kill;
		size_t consumed = 0;
		if (!decode_kill_record(body.data(), body.size(), kill, consumed)) {
			++malformed_bodies_;
			break;
		}
		apply_entity_death(kill.victim_slot, kill.section, true);
		break;
	}
	case s2c::FULL_ENTITY_SPAWN:
		apply_full_entity_spawn(body);
		break;
	case s2c::ENTITY_SPAWN_BATCH: // pool-0 organic spawn batch (§5.23)
		apply_organic_spawn(body);
		break;
	case s2c::POOL_SPAWN: // pool-1 entity spawn batch (§5.11)
		apply_pool_spawn(body);
		break;
	case s2c::STATIC_ENTITY_BATCH: // pool-2 (§5.9)
		apply_static_batch(body);
		break;
	case s2c::POOL3_SYNC: // pool-3 marker/waypoint sync batch (§5.12)
		apply_pool3_batch(body);
		break;
	case s2c::CAPTURE_ZONE_STATE:
		apply_capture_zone_overlay(body);
		break;
	case s2c::MINIMAP_OVERLAY:
		apply_minimap_overlay_batch(body);
		break;
	case s2c::END_ROUND_HEADER:
		apply_end_round_header(body);
		break;
	case s2c::END_ROUND_STATS: // §5.61 the chunked post-round stat board (0x56)
		apply_end_round_stats_chunk(body);
		break;
	case s2c::PLAYER_LIST: // §5.20 the Tab scoreboard (0x16)
		apply_player_list(body);
		break;
	case s2c::PLAYER_SYNC: // §5.21 the connection-slot roster (0x46)
		apply_player_sync(body);
		break;
	case s2c::GAME_EVENT: // §5.26 the kill/objective/medic feed lane (0x1E)
		apply_game_event(body);
		break;
	case s2c::CHAT_BROADCAST: // §5.52 the player-chat fan-out (0x14)
		apply_chat_broadcast(body);
		break;
	case s2c::ENTITY_ROUTED: // §5.36 sub-header + §5.15 guided body (0x44)
		apply_entity_routed(body);
		break;
	case s2c::DEPLOYED_ITEM: // live pool-1 placed-device spawn/update (§5.36)
		apply_deployed_item(body);
		break;
	case s2c::ENTITY_REMOVE: // live packed-handle retirement (0x12)
		apply_entity_remove(body);
		break;
	case s2c::OBJECTIVE_ENTITY_STATE: // flag/carryable pose + carry links (0x2F)
		apply_objective_entity_state(body);
		break;
	case s2c::SPAWN_WAVE_STATUS:
		apply_spawn_wave_status(body);
		break;
	case s2c::SCORE_DELTA_SOUND:
		apply_score_delta_sound(body);
		break;
	case s2c::SCRIPT_REMOTE_COMMAND: // the host VM's replicated WAC command
		apply_script_remote_command(body);
		break;
	default:
		// Game-start scalars and other non-entity tags this reducer
		// does not model.
		++unknown_tags_;
		break;
	}
}

std::vector<ClientRoundEvent> ClientReplicaPipeline::drain_round_events() {
	std::vector<ClientRoundEvent> out;
	out.swap(pending_round_events_);
	return out;
}

std::vector<ClientGameEvent> ClientReplicaPipeline::drain_game_events() {
	std::vector<ClientGameEvent> out;
	out.swap(pending_game_events_);
	return out;
}

std::vector<ClientChatLine> ClientReplicaPipeline::drain_chat_lines() {
	std::vector<ClientChatLine> out;
	out.swap(pending_chat_lines_);
	return out;
}

std::vector<WeaponReload> ClientReplicaPipeline::drain_weapon_reloads() {
	std::vector<WeaponReload> out;
	out.swap(pending_weapon_reloads_);
	return out;
}


void ClientReplicaPipeline::pump(ISessionTransport &channel) {
	Datagram dg;
	while (channel.client_recv(dg)) apply(dg.tag, dg.body);
}

} // namespace opennova::replication
