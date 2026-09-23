#include <runtime/inmatch/server_entity_routes.h>

#include <cstdint>
#include <variant>
#include <vector>

#include <base/io/le.h>
#include <net/npwire/ingame_decode.h>          // ExplosionEffectRecord / ObjectiveNotification / GuidedRecord
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/napi_np_connection.h> // is_in_match / active_player_recipient
#include <runtime/replication/connection_fan.h> // build_water_cross_messages
#include <runtime/world/world.h>

namespace opennova::inmatch {

// Item callbacks' state packets, explosion effects, authoritative removals and
// the HUD relays, fanned and released.
void Server_FanEntityEvents(NapiNPServerCtx &ctx, world::World &world) {
    // Item callbacks' state packets and authoritative removals: mask 0x90
    // includes active remote slots regardless of health, excluding the local host.
    // [orig: Server_SendEntityStatePacket @0x509D70;
    // Server_RemoveEntityAndNotify @0x50A270 -> NapiNPServer_SendFiltered @0x4C87E0]
    if (ctx.is_in_session) {
        for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
            if (!active_player_recipient(conn) ||
                    conn.link.mode == replication::TransportMode::Loopback) continue;
            for (const auto &event : world.out.entity_events) {
                if (const auto *state = std::get_if<world::ItemStateEvent>(&event)) {
                    std::vector<uint8_t> body;
                    io::append_u16_le(body, state->handle);
                    io::append_u16_le(body, static_cast<uint16_t>(state->section));
                    conn.link.transport->host_send(s2c::KILL_SYNC, body, true, 0);
                } else if (const auto *effect = std::get_if<world::ItemExplosionEvent>(&event)) {
                    ExplosionEffectRecord record;
                    record.count = effect->count;
                    record.source = effect->source;
                    record.x = effect->position.x;
                    record.y = effect->position.y;
                    record.z = effect->position.z;
                    record.heading = int16_t(uint32_t(effect->heading) >> 16);
                    conn.link.transport->host_send(s2c::EXPLOSION_EFFECT,
                            encode_explosion_effect(record), true, 0);
                } else if (const auto *removal = std::get_if<world::EntityRemoveEvent>(&event)) {
                    std::vector<uint8_t> body;
                    io::append_u16_le(body, removal->handle);
                    conn.link.transport->host_send(s2c::ENTITY_REMOVE, body, true, 0);
                }
            }
            // The HUD relays ride the same 0x90 mask: every active remote
            // slot, never the local host. [orig:
            //  Server_BroadcastEntityActionPacket @0x5080D0 — send_mask 90h
            //  @0x50818f, the NapiNPServer_SendFiltered(0x3F) call @0x508199]
            for (const world::HudRelay &relay : world.out.hud_relays) {
                ObjectiveNotification wire;
                wire.kind = relay.kind;
                wire.slot = relay.slot;
                wire.is_win = relay.is_win;
                wire.is_active = relay.is_active;
                wire.flag = relay.flag;
                wire.team = relay.team;
                wire.key = relay.key;
                conn.link.transport->host_send(s2c::OBJECTIVE_NOTIFICATION,
                        encode_objective_notification(wire), true, 0);
            }
        }
    }
    world.out.entity_events.clear();
    world.out.hud_relays.clear();
}

// The water-surface crossings the motor recorded, one S2C 0x34 each.
void Server_RouteWaterCrossings(NapiNPServerCtx &ctx, world::World &world) {
	// (2d) Water-surface crossings: S2C 0x34 to every ALIVE in-match player, one
	// message per crossing the motor recorded this tick. Retail fans the splash
	// with send_mask 128 (alive players) the moment a hull crosses the plane, so
	// clients spawn the same effect at the same spot; the queue is drained and
	// cleared every tick whether or not anyone is listening, because a crossing
	// is presentation, never simulation state.
	// [orig: Server_SendOverlayActionToAlive @0x50a1b0, send_mask 128]
	if (ctx.is_in_session && !world.out.water_crossings.events.empty()) {
		const std::vector<std::vector<uint8_t>> splashes =
				replication::build_water_cross_messages(world);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
			if (conn.link.mode == replication::TransportMode::Loopback) continue;
			bool alive = true;
			if (conn.link.owned_entity.valid()) {
				const world::Entity *e = world.registry.get(conn.link.owned_entity);
				alive = e != nullptr && e->health > 0;
			}
			if (!alive) continue; // the mask-128 alive filter
			for (const std::vector<uint8_t> &body : splashes)
				conn.link.transport->host_send(s2c::PLAY_SOUND, body,
				                               /*reliable=*/false);
		}
	}
	world.out.water_crossings.clear();
}

// The guided-round updates, queued behind the tick's 0x0A so each follows the
// frame that carried its round's birth.
void Server_RouteGuidance(NapiNPServerCtx &ctx, world::World &world) {
    // Guidance follows the frame carrying the corresponding fired-round birth.
    // [orig: Entity_SendEffectPacket @0x445C40, routed serializer @0x4D6240]
    if (ctx.is_in_session) for (const auto &update : world.round_sim.guided_updates) {
        for (int group = 1; group <= 6; ++group) {
            if (!(update.groups & (1u << group))) continue;
            GuidedRecord record;
            record.target_slot = update.state.target; record.weapon_type = update.state.phase;
            record.pos_x = update.state.steer[0]; record.pos_y = update.state.steer[1]; record.pos_z = update.state.steer[2];
            record.attach_x = update.state.saved[0]; record.attach_y = update.state.saved[1]; record.attach_z = update.state.saved[2];
            std::vector<uint8_t> body;
            io::append_u16_le(body, update.shooter); io::append_u16_le(body, update.net_id); body.push_back(uint8_t(group));
            const auto payload = encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup(group), record);
            body.insert(body.end(), payload.begin(), payload.end());
            for (auto &conn : ctx.np_protocol.connection_list)
                if (active_player_recipient(conn) && conn.link.mode != replication::TransportMode::Loopback)
                    conn.pending_guidance.push_back(body);
        }
    }
    world.round_sim.guided_updates.clear();
    for (auto &conn : ctx.np_protocol.connection_list) {
        if (!ctx.is_in_session || !active_player_recipient(conn)) {
            conn.pending_guidance.clear();
            continue;
        }
        if (conn.type == NapiNPConnection::kTypeServerSide && !conn.s2c_send_boundary_open) continue;
        for (const auto &body : conn.pending_guidance)
            conn.link.transport->host_send(0x44, body, true, 0);
        conn.pending_guidance.clear();
    }
}

} // namespace opennova::inmatch
