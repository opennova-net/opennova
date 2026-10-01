#include <runtime/inmatch/server_squad.h>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/session_hello.h> // DisconnectEvent
#include <net/npwire/squad_messages.h>
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect
#include <runtime/world/world.h>

#include <array>
#include <cmath>

namespace opennova::inmatch {

namespace {

// The punt tally's per-slot counts: a 251-dword stack array, one per row of
// the 251-row slot table the capacity clamps to (GameConfig::
// total_player_slot_capacity) [orig: Server_ProcessVoteKickResults — the
// 0x3EC-byte `vote_tallies` memset @0x511496].
constexpr size_t kVoteTallyRows = 251;

// PlayerState_GetByIndex + the slot's active byte (+4): the connection
// holding player slot `index` once its player was added.
// [orig: PlayerState_GetByIndex @0x500850]
NapiNPConnection *slot_connection(NapiNPServerCtx &ctx, uint8_t index) {
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (player_slot_active(c) && c.reply.player_slot == index) return &c;
	}
	return nullptr;
}

// The squad break-up's member test, slot state 6 exactly: an active slot in
// the match before the round end moves it to 7 (Match's outcome latch stands
// for that store, as in the play-tick walk). No transport test, and the bot
// byte +0x178E3 is always clear here (no bot slots exist).
// [orig: Server_SendPlayerStateAndSquad @0x518c3f..0x518c70 — +4,
//  `+0x20 == 6`, !+0x178E3; Server_ProcessRoundEnd — the 6 -> 7 store
//  @0x51685e]
bool slot_state_6(const NapiNPServerCtx &ctx, const NapiNPConnection &c) {
	if (!player_slot_active(c) || !is_in_match(c)) return false;
	return ctx.world == nullptr || !ctx.world->match.outcome().ended;
}

// The slot's team byte (+0x1A0): the reserved / assigned team, 0 for an
// unused slot (the table is zeroed).
uint8_t slot_team(const NapiNPConnection *c) {
	return c != nullptr && c->assigned_team_valid ? c->assigned_team : uint8_t{0};
}

// A slot's leader byte: an unused slot reads 0 — the slot table's allocation
// and the disconnect zero the whole row, and the player add alone seeds 0xFF.
// [orig: Server_AllocatePlayerSlotTable @0x51c1cb; Server_HandlePlayerDisconnect
//  @0x51b87d; Server_PlayerAdd @0x51cf0a]
uint8_t slot_leader(const NapiNPConnection *c) {
	return c != nullptr ? c->squad_leader : uint8_t{0};
}

// The squad handlers' common gate: the authority, a slot with a live player,
// not a spectator (+100567) [orig: e.g. @0x510ae0..0x510b0c].
bool squad_sender(const NapiNPServerCtx &ctx, const NapiNPConnection &sender) {
	return ctx.is_authority != 0 && sender.link.owned_entity.valid() && !sender.link.spectator;
}

// The send filter's in-game test (slot state 6/7, the exclusion byte clear).
bool in_game(const NapiNPConnection &c) {
	return is_in_match(c) && c.link.transport != nullptr;
}

void send_to(NapiNPConnection &c, uint8_t tag, const std::vector<uint8_t> &body) {
	if (c.link.transport != nullptr) c.link.transport->host_send(tag, body);
}

// Mask 0x180: every in-game slot on `team`.
void send_team(NapiNPServerCtx &ctx, uint8_t team, uint8_t tag, const std::vector<uint8_t> &body) {
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!in_game(c) || slot_team(&c) != team) continue;
		send_to(c, tag, body);
	}
}

// The waypoint share (C2S 0x17): target 0xFF fans S2C 0x33 to every in-game
// slot the sender leads (mask 0xA0 per slot); any other target reaches that
// slot alone when it is in game on the sender's team (mask 0x1A0). The owner
// byte is the sender's pool-0 entity index.
// [orig: NapiNPServerMsg_HandleChatOrWhisper @0x514850 — the owner
//  Pool_GetIndexFromPtr(0, ...) @0x514897, the 0xFF fan @0x514921..0x5149b6
//  (`slot+100576 == sender id`), the targeted send @0x5149c0..0x514a27]
void relay_waypoint_share(NapiNPServerCtx &ctx, NapiNPConnection &sender,
		const std::vector<uint8_t> &body) {
	const WaypointShare share = decode_waypoint_share(body.data(), body.size());
	WaypointCreate create;
	create.name = share.name;
	create.x = share.x;
	create.y = share.y;
	create.z = share.z;
	create.owner_index = world::pool0_index_byte(sender.link.owned_entity);
	const std::vector<uint8_t> out = encode_waypoint_create(create);
	if (share.target == 0xFFu) {
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!in_game(c) || c.squad_leader != sender.reply.player_slot) continue;
			send_to(c, s2c::WAYPOINT_CREATE, out);
		}
		return;
	}
	NapiNPConnection *target = slot_connection(ctx, share.target);
	if (target == nullptr || !in_game(*target) || slot_team(target) != slot_team(&sender)) return;
	send_to(*target, s2c::WAYPOINT_CREATE, out);
}

// The waypoint delete (C2S 0x4F): its handle to every in-game slot the sender
// leads [orig: NapiNPServerMsg_0x04F @0x514a40 — mask 0xA0 per slot].
void relay_waypoint_delete(NapiNPServerCtx &ctx, NapiNPConnection &sender,
		const std::vector<uint8_t> &body) {
	const std::vector<uint8_t> out =
			encode_entity_handle16(decode_entity_handle16(body.data(), body.size()));
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!in_game(c) || c.squad_leader != sender.reply.player_slot) continue;
		send_to(c, s2c::DESTROY_ENTITY, out);
	}
}

} // namespace

bool Server_LinkSquadMember(NapiNPServerCtx &ctx, NapiNPConnection &member, uint8_t leader,
		uint8_t &stored) {
	const uint8_t member_id = member.reply.player_slot;
	const uint32_t capacity = ctx.config.total_player_slot_capacity();
	uint8_t link = leader;
	uint8_t leader_team = 0;
	if (leader == member_id || leader == 0xFFu) {
		// [orig: @0x5106ee..0x5106f8 — the link becomes 0xFF]
		link = 0xFF;
	} else {
		// [orig: @0x510700..0x510760 — the leader's range, its team, then the
		//  walk up its chain: reaching the member rejects, a slot out of range
		//  rejects, 0xFF ends the walk]
		if (leader >= capacity) return false;
		const NapiNPConnection *leader_slot = slot_connection(ctx, leader);
		leader_team = slot_team(leader_slot);
		uint8_t up = slot_leader(leader_slot);
		// An empty row's leader reads 0, so a chain reaching one steps to row
		// 0; when row 0's own chain leads back through an empty row (a stale
		// link the break-up leaves on a member past state 6), or row 0 is
		// itself empty (a port host with no local row), the walk spins
		// forever, a retail host hang. Every step lands on a row below the
		// capacity, so a walk still going after `capacity` steps has revisited
		// a row and can only be such a spin; the port rejects the link there.
		uint32_t steps = 0;
		while (up != 0xFFu) {
			if (up == member_id) return false;
			if (up >= capacity) return false;
			if (++steps > capacity) return false;
			up = slot_leader(slot_connection(ctx, up));
		}
	}
	// [orig: @0x510766..0x5107a4 — the member in range; a real link needs
	//  the member's team to equal the leader's]
	if (member_id >= capacity) return false;
	if (link != 0xFFu && slot_team(&member) != leader_team) return false;
	member.squad_leader = link;
	stored = link;
	return true;
}

void Server_DissolveSquadOf(NapiNPServerCtx &ctx, NapiNPConnection &player) {
	const auto dissolve_one = [&ctx](NapiNPConnection &p) {
		// [orig: the link @0x518b48..0x518b4f (CO 0xFF, fireteam 0), the 0x71
		//  @0x518b56..0x518ba7 (mask 0x180), the 0x72 pair @0x518bac..0x518c2c
		//  (mask 0xA0)]
		p.squad_leader = 0xFF;
		p.fireteam = 0;
		SquadJoin join;
		join.leader = 0xFF;
		join.member = p.reply.player_slot;
		send_team(ctx, slot_team(&p), s2c::SQUAD_JOIN, encode_squad_join(join));
		if (!in_game(p)) return;
		for (uint8_t kind = 0; kind < 2; ++kind) {
			SquadOrder line;
			line.kind = kind;
			send_to(p, s2c::SQUAD_ORDER, encode_squad_order(line));
		}
	};
	const uint8_t id = player.reply.player_slot;
	dissolve_one(player);
	// Every slot in state 6 it led goes the same way; a member still loading
	// (or past the round end, state 7) keeps its leader byte on the freed row,
	// as in retail [orig: @0x518c31..0x518d65 — +4, state == 6, +0x178E3
	//  clear, CO == the player's slot].
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (&c == &player || !slot_state_6(ctx, c) || c.squad_leader != id) continue;
		dissolve_one(c);
	}
}

void Server_ProcessVoteKickResults(NapiNPServerCtx &ctx) {
	const GameConfig &cfg = ctx.config;
	if (!cfg.voting_enabled) return;
	// [orig: @0x511400 — the active count (+4), the minimum, the threshold
	//  (int)(active * percent + 0.5)]
	const uint32_t capacity = cfg.total_player_slot_capacity();
	int32_t active = 0;
	for (NapiNPConnection &c : ctx.np_protocol.connection_list)
		if (c.phase >= ConnectionPhase::PlayerAdded) ++active;
	if (active < cfg.voting_min_players) return;
	const uint32_t threshold = static_cast<uint32_t>(static_cast<int64_t>(
			static_cast<double>(active) * static_cast<double>(cfg.voting_percent) + 0.5));
	std::array<uint32_t, kVoteTallyRows> tally{};
	for (uint32_t index = 0; index < capacity; ++index) {
		NapiNPConnection *voter = slot_connection(ctx, static_cast<uint8_t>(index));
		if (voter == nullptr) continue;
		const uint8_t target = voter->punt_vote;
		if (target == 0xFFu) continue;
		// A stored target past the capacity (the 0x3F handler stores any byte)
		// has no row: retail reads its local byte through a NULL row, a host
		// crash; the port counts no vote for it. Below the capacity the count
		// stays inside the 251 rows.
		// [orig: `target < capacity ? row : 0` @0x5114c2..0x5114d0, the +5
		//  read @0x5114dc]
		if (target >= capacity) continue;
		// The vote counts unless the target slot is the local one (+5).
		NapiNPConnection *victim = slot_connection(ctx, target);
		if (victim != nullptr && victim->type == NapiNPConnection::kTypeClientSide) continue;
		if (++tally[target] < threshold) continue;
		// Each vote that reaches the threshold punts again:
		// Server_SendValidatedChatToPlayer(target, "", 40, "VOTEDOFF") — the
		// authority, the slot active, not local, no disconnect latched.
		// [orig: @0x5114f8; Server_SendValidatedChatToPlayer @0x50a210]
		if (victim == nullptr) continue;
		DisconnectEvent event;
		event.ds = 1;
		event.dc = 2;
		event.dpc = 40;
		event.ddstr = "VOTEDOFF";
		Server_StageHostDisconnect(*victim, event);
	}
	// Every live voter for a punted target starts over [orig: the second pass
	//  — +4 and an entity, +100578 = 0xFF]. A stored target past the 251
	//  rows reads no count (retail reads past its stack array there, after
	//  the first pass has already crashed on that vote) [orig: @0x51154b].
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.phase < ConnectionPhase::PlayerAdded || !c.link.owned_entity.valid()) continue;
		if (c.punt_vote == 0xFFu) continue;
		const uint32_t count = c.punt_vote < tally.size() ? tally[c.punt_vote] : 0u;
		if (count >= threshold) c.punt_vote = 0xFF;
	}
}

bool Server_HandleSquadMessage(NapiNPServerCtx &ctx, NapiNPConnection &sender, uint8_t tag,
		const std::vector<uint8_t> &body, world::World &world) {
	(void)world;
	switch (tag) {
	case c2s::WAYPOINT_SHARE:
		if (squad_sender(ctx, sender)) relay_waypoint_share(ctx, sender, body);
		return true;
	case c2s::WAYPOINT_DELETE:
		if (squad_sender(ctx, sender)) relay_waypoint_delete(ctx, sender, body);
		return true;
	case c2s::SQUAD_JOIN_REQUEST: {
		// [orig: Server_HandleEntitySync @0x510990 — the link, then S2C 0x71
		//  to the member's team (mask 0x180); the 0xFF-only 0x72 pair after it
		//  compares a zero-extended byte with -1 @0x510a3e and never runs]
		if (!squad_sender(ctx, sender)) return true;
		const uint8_t leader = decode_squad_join_request(body.data(), body.size());
		uint8_t stored = 0;
		if (!Server_LinkSquadMember(ctx, sender, leader, stored)) return true;
		SquadJoin join;
		join.leader = stored;
		join.member = sender.reply.player_slot;
		send_team(ctx, slot_team(&sender), s2c::SQUAD_JOIN, encode_squad_join(join));
		return true;
	}
	case c2s::SQUAD_ORDER_REQUEST: {
		// [orig: NapiNPServerMsg_HandleChatBroadcast @0x510ae0 — S2C 0x72
		//  [kind][text] to each listed slot below the capacity (mask 0xA0); no
		//  team or leader check]
		if (!squad_sender(ctx, sender)) return true;
		const SquadOrderRequest order = decode_squad_order_request(body.data(), body.size());
		SquadOrder line;
		line.kind = order.kind;
		line.text = order.text;
		const std::vector<uint8_t> out = encode_squad_order(line);
		const uint32_t capacity = ctx.config.total_player_slot_capacity();
		for (const uint8_t id : order.targets) {
			if (id >= capacity) continue;
			NapiNPConnection *c = slot_connection(ctx, id);
			if (c != nullptr && in_game(*c)) send_to(*c, s2c::SQUAD_ORDER, out);
		}
		return true;
	}
	case c2s::FIRETEAM_ASSIGN: {
		// [orig: NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00 — each
		//  listed slot's +100577, then S2C 0x73 [slot][fireteam] to its team
		//  (mask 0x180). Retail dereferences a NULL slot for an id past the
		//  capacity (a host crash); the port skips that id.]
		if (!squad_sender(ctx, sender)) return true;
		const FireteamAssign assign = decode_fireteam_assign(body.data(), body.size());
		for (const uint8_t id : assign.members) {
			NapiNPConnection *c = slot_connection(ctx, id);
			if (c == nullptr) continue;
			c->fireteam = assign.fireteam;
			FireteamSet set;
			set.member = c->reply.player_slot;
			set.fireteam = assign.fireteam;
			send_team(ctx, slot_team(c), s2c::FIRETEAM_SET, encode_fireteam_set(set));
		}
		return true;
	}
	case c2s::SQUAD_RECRUIT: {
		// [orig: NapiNPServerMsg_HandleVoteKick @0x510d20 — S2C 0x74
		//  [recruiter] to the target slot (mask 0xA0)]
		if (!squad_sender(ctx, sender)) return true;
		const SquadRecruit recruit = decode_squad_recruit(body.data(), body.size());
		NapiNPConnection *target = slot_connection(ctx, recruit.target);
		if (target != nullptr && in_game(*target))
			send_to(*target, s2c::SQUAD_RECRUITED, encode_squad_recruited(recruit.recruiter));
		return true;
	}
	case c2s::GO_CODE: {
		// [orig: NapiNPServer_BroadcastPlayerProfileUpdate @0x510dc0 — S2C
		//  0x78 [leader][code] to every in-game slot whose leader is byte 0
		//  (mask 0x20)]
		if (!squad_sender(ctx, sender)) return true;
		const GoCode code = decode_go_code(body.data(), body.size());
		const std::vector<uint8_t> out = encode_go_code(code);
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!in_game(c) || c.squad_leader != code.leader) continue;
			send_to(c, s2c::GO_CODE, out);
		}
		return true;
	}
	case c2s::PUNT_VOTE: {
		// [orig: NapiNPServerMsg_VoteKick @0x518f10 — the authority and a
		//  slot; with voting on the vote lands and a valid target runs the
		//  tally]
		if (ctx.is_authority == 0 || !sender.link.owned_entity.valid()) return true;
		const uint8_t target = decode_punt_vote(body.data(), body.size());
		if (!ctx.config.voting_enabled) return true;
		sender.punt_vote = target;
		if (slot_connection(ctx, target) != nullptr) Server_ProcessVoteKickResults(ctx);
		return true;
	}
	default:
		return false;
	}
}

} // namespace opennova::inmatch
