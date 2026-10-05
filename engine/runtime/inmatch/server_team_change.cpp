// The host side of the death screen's team change -- see server_team_change.h.

#include <runtime/inmatch/server_team_change.h>

#include <net/npwire/ingame_encode.h>     // encode_chat_broadcast
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/mission_rotation.h> // HostRotation::previous_game_type
#include <runtime/inmatch/server_spawn.h> // Server_ChangeEntityTeam
#include <runtime/inmatch/server_tick.h>  // Server_ProcessPlayerDeath
#include <runtime/world/entity.h>
#include <runtime/world/round_sim.h>      // RoundDeath
#include <runtime/world/world.h>

#include <algorithm>
#include <vector>

namespace opennova::inmatch {

namespace {

constexpr uint32_t kGameTypeTeamBit = 0x10000u; // g_GameType & 0x10000: a team game
constexpr uint32_t kGameTypeCoopBit = 0x20000u; // g_GameType & 0x20000: co-op
constexpr uint32_t kSystemMessageRetentionFlushes = 310; // SendFiltered(0x14, 1, 310)

// The handler's `sprintf(msg_buffer, format, slot+40)`: the stock C2Blue /
// C2Red lines carry one `%s` (the CRT call is a platform primitive).
std::string format_team_change_line(const std::string &format, const std::string &name) {
	const size_t marker = format.find("%s");
	if (marker == std::string::npos) return format;
	return format.substr(0, marker) + name + format.substr(marker + 2);
}

} // namespace

int32_t Server_CalcTeamImbalance(const NapiNPServerCtx &ctx) {
	int32_t imbalance = 0;
	if ((ctx.config.game_type & kGameTypeTeamBit) == 0) return imbalance;
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.phase < ConnectionPhase::PlayerAdded) continue; // slot+4
		const uint8_t team = c.assigned_team_valid ? c.assigned_team : uint8_t{0}; // slot+416
		if (team == 1) ++imbalance;
		else if (team == 2) --imbalance;
	}
	return imbalance;
}

bool Server_ShouldAutoBalance(const NapiNPServerCtx &ctx) {
	const uint32_t game_type = ctx.config.game_type;
	if (ctx.is_authority == 0 || ctx.is_in_session == 0 ||
			(game_type & kGameTypeCoopBit) != 0 || (game_type & kGameTypeTeamBit) == 0)
		return false;
	// dword_24D212C: the type the previous mission was set up under (the
	// round init re-stamps it after this test, @0x516AD5).
	const uint32_t round_game_type =
			ctx.rotation != nullptr ? ctx.rotation->previous_game_type : game_type;
	if ((round_game_type & kGameTypeTeamBit) == 0 || (round_game_type & kGameTypeCoopBit) != 0)
		return true;
	if (!ctx.config.auto_balance_enabled) return false;
	int32_t imbalance = Server_CalcTeamImbalance(ctx);
	if (imbalance < 0) imbalance = -imbalance;
	if (imbalance <= 1 || imbalance < ctx.config.auto_balance_min_difference) return false;
	return imbalance >= ctx.config.auto_balance_trigger_difference;
}

void Server_AutoBalanceTeams(NapiNPServerCtx &ctx, world::World *world) {
	// The pair list: (time in the server, slot) for every active slot that is
	// not the host's own, in slot order. The connection's join stamp stands
	// in for the slot's +94384 connect time.
	struct Pair {
		int32_t key;
		NapiNPConnection *slot;
	};
	std::vector<NapiNPConnection *> slots;
	for (NapiNPConnection &c : ctx.np_protocol.connection_list)
		if (c.phase >= ConnectionPhase::PlayerAdded && c.type != NapiNPConnection::kTypeClientSide)
			slots.push_back(&c);
	std::stable_sort(slots.begin(), slots.end(), [](const NapiNPConnection *a,
			const NapiNPConnection *b) { return a->reply.player_slot < b->reply.player_slot; });
	std::vector<Pair> pairs;
	const uint32_t now_ms = ctx.np_protocol.host_run_duration_ms;
	for (NapiNPConnection *c : slots)
		pairs.push_back({static_cast<int32_t>(now_ms - c->join_validated_host_ms), c});
	// CPairList_ShellSort: Knuth's gaps, descending by key, 1-based indexes.
	const int count = static_cast<int>(pairs.size());
	int gap = 1;
	if (count / 9 >= 1) {
		do gap = 3 * gap + 1;
		while (gap <= count / 9);
	}
	for (; gap > 0; gap /= 3) {
		for (int i = gap + 1; i <= count; ++i) {
			const Pair insert = pairs[static_cast<size_t>(i - 1)];
			int j = i;
			while (j > gap && pairs[static_cast<size_t>(j - gap - 1)].key >= insert.key) {
				pairs[static_cast<size_t>(j - 1)] = pairs[static_cast<size_t>(j - gap - 1)];
				j -= gap;
			}
			pairs[static_cast<size_t>(j - 1)] = insert;
		}
	}
	int32_t imbalance = Server_CalcTeamImbalance(ctx);
	int32_t magnitude = imbalance < 0 ? -imbalance : imbalance;
	for (const Pair &pair : pairs) {
		if (magnitude < 2 || magnitude < ctx.config.auto_balance_min_difference) return;
		NapiNPConnection &slot = *pair.slot;
		const uint8_t team = slot.assigned_team_valid ? slot.assigned_team : uint8_t{0};
		uint8_t moved_to = 0;
		if (team == 1 && imbalance > 0) {
			moved_to = 2;
			imbalance -= 2;
		} else if (team == 2 && imbalance < 0) {
			moved_to = 1;
			imbalance += 2;
		} else {
			continue;
		}
		magnitude -= 2;
		slot.assigned_team = moved_to;
		slot.assigned_team_valid = true;
		world::Entity *entity = world != nullptr && slot.link.owned_entity.valid()
				? world->registry.get(slot.link.owned_entity)
				: nullptr;
		if (entity == nullptr) continue;
		entity->team = moved_to;
		const auto listed = std::find(ctx.team_change_entities.begin(),
				ctx.team_change_entities.end(), entity->handle);
		if (listed == ctx.team_change_entities.end())
			ctx.team_change_entities.push_back(entity->handle);
	}
}

void Server_BroadcastSystemMessage(NapiNPServerCtx &ctx, const std::string &message) {
	if (ctx.is_authority == 0 || message.empty()) return;
	ChatBroadcast line;
	line.channel = 7;
	line.sender_slot = 255;
	line.text = message;
	const std::vector<uint8_t> body = encode_chat_broadcast(line);
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!is_in_match(c) || c.link.transport == nullptr) continue;
		c.link.transport->host_send(s2c::CHAT_BROADCAST, body, true, 0, false,
				kSystemMessageRetentionFlushes);
	}
}

void Server_HandleTeamChangeRequest(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world, uint32_t now_ms) {
	if (ctx.is_authority == 0) return;
	// The sender's player slot and its entity [orig: @0x518F91..0x518FA8, the
	// entity @0x519008].
	if (conn.phase < ConnectionPhase::PlayerAdded) return;
	world::Entity *entity = world.registry.get(conn.link.owned_entity);
	if (entity == nullptr) return;
	if (conn.link.spectator) return; // slot+100567
	// The interval since the last stamp, as unsigned dword arithmetic.
	if (conn.reply.team_change_ms != 0 &&
			conn.reply.team_change_ms +
					static_cast<uint32_t>(ctx.config.change_team_interval_seconds) * 1000u >
					now_ms)
		return;
	if ((ctx.config.mp_attributes & GameConfig::kMpAttribTeamChoose) == 0) return;
	if (ctx.config.auto_balance_enabled && Server_ShouldAutoBalance(ctx)) return;
	const uint8_t team = entity->team;
	if (team == 1 || team == 2) {
		const uint8_t to = team == 2 ? uint8_t{1} : uint8_t{2};
		const world::EntityHandle handle = conn.link.owned_entity;
		Server_ChangeEntityTeam(ctx, world, handle, to);
		const std::string &format = to == 1 ? ctx.server_text.change_to_blue_format
		                                    : ctx.server_text.change_to_red_format;
		Server_BroadcastSystemMessage(ctx, format_team_change_line(format, conn.reply.player_name));
		entity = world.registry.get(handle);
		if (entity != nullptr) {
			entity->health = 0;                           // +0x11E
			entity->last_attacker = world::EntityHandle{}; // +0x178
			world::RoundDeath death;
			death.victim = handle;
			death.victim_handle = handle.packed;
			death.event_flags = entity->cause_flags & 0xF00u;
			Server_ProcessPlayerDeath(ctx, world, death);
			if ((entity = world.registry.get(handle)) != nullptr) entity->flags |= 2u;
		}
		// The penalty replaces the death's respawn holds, and the revive
		// window closes.
		conn.link.respawn_delay_seconds =
				static_cast<uint32_t>(ctx.config.change_team_penalty_seconds);
		conn.link.spawn_target_hold_seconds =
				static_cast<uint32_t>(ctx.config.change_team_penalty_seconds);
		conn.link.downed_revive_seconds = 0;
	}
	conn.reply.team_change_ms = now_ms;
}

} // namespace opennova::inmatch
