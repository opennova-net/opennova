// The remote-admin console's player-slot walks (admin_console.h): the PLAYER verbs, the
// PETERRABBIT easter egg and the QUERY status report, each over the authority's slot table
// in slot order. The table is the connection list the port keeps as retail's player-slot
// table; a dedicated host here has no connectionless slot 0 of its own (D-NET-352).
#include <runtime/inmatch/admin_console.h>

#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_ban_lists.h>
#include <runtime/inmatch/server_spawn.h> // Server_ChangeEntityTeam
#include <runtime/inmatch/server_tick.h>  // Server_ProcessPlayerDeath / Server_StageHostDisconnect
#include <runtime/world/ai.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <base/gameprofile/game_type.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <net/npwire/ingame_decode.h> // ChatBroadcast
#include <net/npwire/ingame_encode.h> // encode_chat_broadcast
#include <net/npwire/ingame_message_id.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace opennova::inmatch {

namespace {

constexpr const char *kPlayerUsage = "USAGE -  PLAYER [LIST | PUNT | BAN | SWAPTEAM | KILL | ZEROSCORE] [# | ALL]";
constexpr const char *kTableHeader = "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n";
// SendFiltered(0x14, 1, 0x136). [orig: CAdminServer_HandleFunCommand, ARMAGEDDON]
constexpr uint32_t kChatRetentionFlushes = 0x136;

bool ieq(std::string_view a, std::string_view b) {
	return strutil::iequals(a, b);
}

int32_t atol_of(const std::string &s) {
	return io::retail_atol(s.c_str());
}

// Every active slot (the +4 byte: a player added) in slot order.
std::vector<NapiNPConnection *> active_slots(NapiNPServerCtx &ctx) {
	std::vector<NapiNPConnection *> out;
	const uint32_t capacity = ctx.config.total_player_slot_capacity();
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (player_slot_active(conn) && conn.reply.player_slot < capacity) out.push_back(&conn);
	std::sort(out.begin(), out.end(), [](const NapiNPConnection *a, const NapiNPConnection *b) {
		return a->reply.player_slot < b->reply.player_slot;
	});
	return out;
}

std::vector<const NapiNPConnection *> active_slots(const NapiNPServerCtx &ctx) {
	std::vector<const NapiNPConnection *> out;
	for (NapiNPConnection *conn : active_slots(const_cast<NapiNPServerCtx &>(ctx))) out.push_back(conn);
	return out;
}

// The slot's local byte (+5): the host's own loopback connection.
bool slot_is_local(const NapiNPConnection &conn) {
	return conn.type == NapiNPConnection::kTypeClientSide ||
	       conn.link.mode == replication::TransportMode::Loopback;
}

world::Entity *slot_entity(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	return ctx.world != nullptr ? ctx.world->registry.get(conn.link.owned_entity) : nullptr;
}

// The slot's team byte (+416), read through its bound entity as the status page reads it
// (D-NET-132), else the reserved assignment.
uint8_t slot_team(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if (const world::Entity *e = slot_entity(ctx, conn)) return e->team;
	return conn.assigned_team_valid ? conn.assigned_team : uint8_t{0};
}

// One PLAYER LIST / QUERY row: the name padded (never cut) to 16, the slot number, the team,
// the class word, CPlayerStats fields 4 and 6 (Kills, Deaths: the direct layout's +1 words),
// the ping. [orig: "%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n" @0x7C0900; CPlayerStats_GetFieldPlusOne
//  @0x52D7D0]
std::string slot_row(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	const world::Entity *entity = slot_entity(ctx, conn);
	int32_t kills = 0;
	int32_t deaths = 0;
	if (ctx.world != nullptr) {
		if (const world::MatchPlayer *p = ctx.world->match.player(conn.link.owned_entity)) {
			kills = p->stats[world::MatchStats::kEnemyKills];
			deaths = p->stats[world::MatchStats::kDeaths];
		}
	}
	char line[160];
	std::snprintf(line, sizeof(line), "%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n", conn.reply.player_name.c_str(),
			static_cast<int>(conn.reply.player_slot), static_cast<int>(slot_team(ctx, conn)),
			entity != nullptr ? static_cast<int>(entity->player_class) : 0, kills, deaths,
			static_cast<int>(conn.reply.rtt_ms));
	return line;
}

// Server_SendValidatedChatToPlayer (a misnomer: the punt): on the authority an in-range active
// slot that is neither local nor dropping (+96481, which nothing sets on this host) gets the
// disconnect description, reason `code`, extra info `detail`.
// [orig: Server_SendValidatedChatToPlayer @0x50A210 -> CNapiNPConnection_TrySendChatMessage
//  @0x5006C0]
void validated_punt(NapiNPServerCtx &ctx, int32_t index, uint32_t code, const char *detail) {
	if (ctx.is_authority == 0) return;
	if (index < 0 || index >= static_cast<int32_t>(ctx.config.total_player_slot_capacity())) return;
	for (NapiNPConnection *slot : active_slots(ctx)) {
		if (slot->reply.player_slot != static_cast<uint32_t>(index)) continue;
		if (slot_is_local(*slot)) return;
		DisconnectEvent event;
		event.ds = 1;
		event.dc = 2;
		event.dpc = code;
		event.ddstr = detail;
		Server_StageHostDisconnect(*slot, event);
		return;
	}
}

// GameEvent_PlayerDeath on the slot's entity, scored against its last attacker (+0x178) as it
// stands, then Flags |= 2; health is not touched. [orig: CAdminServer_HandlePlayer
//  @0x404253..0x40425E -> GameEvent_PlayerDeath @0x516DD0 (the killer the victim's +0x178)]
void kill_entity(NapiNPServerCtx &ctx, world::EntityHandle handle) {
	world::World &world = *ctx.world;
	world::Entity *entity = world.registry.get(handle);
	if (entity == nullptr) return;
	world::RoundDeath death;
	death.victim = handle;
	death.victim_handle = handle.packed;
	death.killer = entity->last_attacker;
	death.killer_handle = entity->last_attacker.packed;
	death.event_flags = entity->cause_flags & 0xF00u;
	Server_ProcessPlayerDeath(ctx, world, death);
	if ((entity = world.registry.get(handle)) != nullptr) entity->flags |= 2u;
}

// CPlayerStats_FreeAllBuffers zeroes the record, CPlayerStats_InitWeaponTracking sets field
// 35. [orig: @0x52C3E0; @0x52BF60 (+140 = 1)]
void zero_score(NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if (ctx.world == nullptr) return;
	if (world::MatchPlayer *row = ctx.world->match.player(conn.link.owned_entity)) {
		row->stats = world::MatchStats{};
		row->stats[world::MatchStats::kRoundMarker] = 1;
	}
}

void swap_team(NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	world::Entity *entity = slot_entity(ctx, conn);
	if (entity == nullptr) return; // a null deref in retail (D-NET-369)
	Server_ChangeEntityTeam(ctx, *ctx.world, conn.link.owned_entity, entity->team == 2 ? uint8_t{1} : uint8_t{2});
}

std::string slot_message(const char *format, const NapiNPConnection &conn) {
	char line[256];
	std::snprintf(line, sizeof(line), format, static_cast<long>(conn.reply.player_slot),
			conn.reply.player_name.c_str());
	return line;
}

// One slot's PLAYER BAN: the PCID appended to banlist.txt's list (saved, and confirmed, only
// where the list exists), then the punt; false (and the error line) for a slot with no PCID.
// Every slot here carries its net player, so retail's `not connected to a network` (a
// connectionless slot) cannot arise.
// [orig: CAdminServer_HandlePlayer — ALL @0x404335..0x404431, one slot @0x404476..0x40452B]
bool ban_slot(NapiNPServerCtx &ctx, const NapiNPConnection &conn, std::vector<std::string> &replies) {
	if (conn.account.pcid.empty()) {
		replies.push_back(slot_message("ERROR - Player #%ld \"%s\" doesn't have a PCID.", conn));
		return false;
	}
	if (ctx.bans.pcids.has_value() && banlist::add(*ctx.bans.pcids, conn.account.pcid, conn.reply.player_name)) {
		Server_SavePcidBanList(ctx);
		replies.push_back(slot_message("OK - Banned Player #%ld \"%s\".", conn));
	}
	validated_punt(ctx, conn.reply.player_slot, 33, "AdminPunt");
	return true;
}

} // namespace

std::string AdminConsole::player_table() const {
	std::string out = kTableHeader;
	for (const NapiNPConnection *conn : active_slots(ctx_)) out += slot_row(ctx_, *conn);
	return out;
}

// [orig: CAdminServer_HandlePlayer @0x403EF0 — LIST in the Game Loop only (the header
//  @0x403F7C, a row per active slot @0x403FE9); the target ALL or `(u8)atol` matched against
//  each active slot's number (+20), else `ERROR - Player Not Found.` before the sub-verb is
//  read @0x40407E..0x404136; PUNT, SWAPTEAM @0x4041ED..0x404202, KILL @0x404253..0x40425E,
//  ZEROSCORE, BAN @0x40445D..0x404539, CEASEFIRE @0x404543..0x404573 (nothing changes); any
//  other sub-verb no reply]
void AdminConsole::handle_player(const Args &args, std::vector<std::string> &replies) {
	if (args.empty()) {
		replies.emplace_back(kPlayerUsage);
		return;
	}
	const std::string &sub = args[0];
	if (ieq(sub, "LIST")) {
		replies.push_back(in_game() ? player_table() : std::string("ERROR - Not in Game State."));
		return;
	}
	if (args.size() < 2) {
		replies.emplace_back(kPlayerUsage);
		return;
	}
	const bool all = ieq(args[1], "ALL");
	const uint8_t number = all ? 0 : static_cast<uint8_t>(atol_of(args[1]));
	NapiNPConnection *target = nullptr;
	if (!all) {
		for (NapiNPConnection *slot : active_slots(ctx_))
			if (slot->reply.player_slot == number) {
				target = slot;
				break;
			}
		if (target == nullptr) {
			replies.emplace_back("ERROR - Player Not Found.");
			return;
		}
	}
	if (ieq(sub, "PUNT")) {
		// The number, not the matched slot, is what is tested and punted.
		// [orig: @0x40409F..0x40414F]
		if (all) {
			for (int32_t index = 1; index < 251; ++index) validated_punt(ctx_, index, 33, "AdminPuntAll");
			replies.emplace_back("OK - All Players punted.");
		} else if (number != 0) {
			validated_punt(ctx_, number, 33, "AdminPunt");
			replies.emplace_back("OK - Player punted.");
		} else {
			replies.emplace_back("ERROR - Cannot punt Host.");
		}
		return;
	}
	if (ieq(sub, "SWAPTEAM")) {
		if (all) {
			for (NapiNPConnection *slot : active_slots(ctx_)) swap_team(ctx_, *slot);
			replies.emplace_back("OK - All Players Swapped.");
		} else {
			swap_team(ctx_, *target);
			replies.emplace_back("OK - Player Swapped.");
		}
		return;
	}
	if (ieq(sub, "KILL")) {
		// The single form tests for no entity in retail (D-NET-369); the walk skips one.
		if (ctx_.world != nullptr) {
			if (all) {
				for (NapiNPConnection *slot : active_slots(ctx_))
					if (slot_entity(ctx_, *slot) != nullptr) kill_entity(ctx_, slot->link.owned_entity);
			} else {
				kill_entity(ctx_, target->link.owned_entity);
			}
		}
		replies.emplace_back(all ? "OK - All Players Killed." : "OK - Player Killed.");
		return;
	}
	if (ieq(sub, "ZEROSCORE")) {
		if (all) {
			for (NapiNPConnection *slot : active_slots(ctx_))
				if (slot_entity(ctx_, *slot) != nullptr) zero_score(ctx_, *slot);
			replies.emplace_back("OK - All Players Zeroed.");
		} else {
			zero_score(ctx_, *target);
			replies.emplace_back("OK - Player Zeroed.");
		}
		return;
	}
	if (ieq(sub, "BAN")) {
		if (all) {
			// Every active non-local slot with an entity: its lines, the punt and `OK - All
			// Players Banned.` each, and no closing reply. [orig: @0x404298..0x40444B]
			for (NapiNPConnection *slot : active_slots(ctx_)) {
				if (slot_entity(ctx_, *slot) == nullptr || slot_is_local(*slot)) continue;
				if (ban_slot(ctx_, *slot, replies)) replies.emplace_back("OK - All Players Banned.");
			}
			return;
		}
		if (ban_slot(ctx_, *target, replies)) replies.emplace_back("OK - Player Banned.");
		return;
	}
	if (ieq(sub, "CEASEFIRE")) replies.emplace_back(all ? "OK - All Players Modified." : "OK - Player Modified.");
}

// [orig: CAdminServer_HandleFunCommand @0x404E30 (@0x404E3F..0x40517F) — no argument an empty
//  reply; every sub-verb, known or not, `OK - Hippity Hoppity`]
void AdminConsole::handle_fun(const Args &args, std::vector<std::string> &replies) {
	if (args.empty()) {
		replies.emplace_back("");
		return;
	}
	const std::string &sub = args[0];
	world::World *world = ctx_.world;
	if (ieq(sub, "SEXCHANGE") && args.size() > 1) {
		// Two combos from the avatar table: the first female head (the found combo's +280 sex)
		// of an evil nationality and of a good one, walking nationality 0..31, division
		// 0..15, the combos in table order; each matching slot (compared by its TEAM byte
		// +416, not its number) gets the other side's: a team-1 slot's side-A word (+442) the
		// good one, any other's side-B word (+444) the evil one, then its entity's team byte
		// cleared and Server_ChangeEntityTeam back to the slot's team, which re-stamps the
		// NetId from those words. [orig: sub_57AEC0 @0x57AEC0, sub_57AE90 @0x57AE90 ->
		//  MinimapSlot_FindByPackedId @0x57A270, MinimapSlot_GetFieldByIndex @0x579E70 (the
		//  nationality's alignment)]
		uint16_t evil_model = 0xFFFF;
		uint16_t good_model = 0xFFFF;
		const bool all = ieq(args[1], "ALL");
		const uint8_t target = all ? 0 : static_cast<uint8_t>(atol_of(args[1]));
		const auto both = [&] { return evil_model != 0xFFFF && good_model != 0xFFFF; };
		if (seams_.characters != nullptr) {
			const CharacterRegistry &registry = *seams_.characters;
			for (int nationality = 0; nationality < 32 && !both(); ++nationality) {
				for (int division = 0; division < 16 && !both(); ++division) {
					for (const CharacterEntry &entry : registry.entries()) {
						if (both()) break;
						if (entry.nationality_id != nationality || entry.division_id != division) continue;
						const CharacterEntry *found = registry.find_by_packed_id(entry.packed_id);
						const bool female = found != nullptr && found->head_female;
						if (evil_model == 0xFFFF && female && entry.alignment != 0)
							evil_model = entry.packed_id;
						else if (good_model == 0xFFFF && female && entry.alignment == 0)
							good_model = entry.packed_id;
					}
				}
			}
		}
		if (world != nullptr) {
			for (NapiNPConnection *slot : active_slots(ctx_)) {
				world::Entity *entity = slot_entity(ctx_, *slot);
				if (entity == nullptr) continue;
				const uint8_t team = slot->assigned_team_valid ? slot->assigned_team : uint8_t{0};
				if (!all && team != target) continue;
				if (team == 1)
					slot->char_vars.char_id[0] = good_model;
				else
					slot->char_vars.char_id[1] = evil_model;
				entity->team = 0;
				Server_ChangeEntityTeam(ctx_, *world, slot->link.owned_entity, team);
			}
		}
	} else if (ieq(sub, "ARMAGEDDON")) {
		// Every active slot's entity to the origin (+4 / +8 / +12), then S2C 0x14 [0][slot]
		// ["ARMAGEDDON!!!"] to every in-match slot, the slot byte read one past the table's
		// last slot in retail (D-NET-369: no sender, 0xFF, here).
		if (world != nullptr) {
			for (NapiNPConnection *slot : active_slots(ctx_))
				if (world::Entity *entity = slot_entity(ctx_, *slot)) entity->position = world::Vec3{};
		}
		ChatBroadcast line;
		line.channel = 0;
		line.sender_slot = 0xFF;
		line.text = "ARMAGEDDON!!!";
		const std::vector<uint8_t> body = encode_chat_broadcast(line);
		for (NapiNPConnection &conn : ctx_.np_protocol.connection_list) {
			if (!active_player_recipient(conn)) continue;
			conn.link.transport->host_send(s2c::CHAT_BROADCAST, body, true, 0, false, kChatRetentionFlushes);
		}
	} else if (ieq(sub, "FLING")) {
		// None, or ALL, means every active slot; else the slot whose number matches. Each one's
		// vertical velocity (+160, the organic's InfantryState::vel[2]) becomes
		// `(rand() + 0xFFFF) & 0x7FFFFFFF`; a slot with no entity is skipped (D-NET-369).
		const bool all = args.size() == 1 || ieq(args[1], "ALL");
		const uint8_t target = all ? 0 : static_cast<uint8_t>(atol_of(args[1]));
		if (world != nullptr) {
			for (NapiNPConnection *slot : active_slots(ctx_)) {
				if (!all && slot->reply.player_slot != target) continue;
				if (slot_entity(ctx_, *slot) == nullptr) continue;
				const int32_t speed = static_cast<int32_t>((world->crt_rand.next() + 0xFFFFu) & 0x7FFFFFFFu);
				if (world::AiEntity *body = world->ai.for_handle(slot->link.owned_entity)) body->inf.vel[2] = speed;
			}
		}
	}
	replies.emplace_back("OK - Hippity Hoppity");
}

// The plaintext QUERY's report: the session header (CR LF lines), PLAYER LIST's table, and
// the map queue in MISSION LIST's format with empty fillers (LF lines); built unbounded where
// retail writes a 16 KB report and a 2 KB queue (D-NET-363).
// [orig: CAdminServer_HandleStatus @0x402E30 — SERVER: the session name dword_24D1FA4
//  @0x402E83; UP-TIME `%ld %2.2ld:%2.2ld:%2.2ld` of CSessionTimer_GetElapsedMS; CURRENT TOD the
//  signed high word of g_EnvCurTimeFixed24 @0x402F6F; the active-slot count; g_MapFileName;
//  GameType_GetAbbreviation(g_GameType, NULL); the table @0x40314D..0x403182; MAP QUEUE in the
//  Game Loop with a rotation, else `No missions in queue.`]
std::string AdminConsole::status_report() {
	const world::World *world = ctx_.world;
	const std::vector<const NapiNPConnection *> slots = active_slots(static_cast<const NapiNPServerCtx &>(ctx_));
	std::string out;
	char line[96];
	// dword_24D1FA4 is the session's copy of the cfg name; SET ServerName does not reach it,
	// the ServerCommand's SetServerName does (server_admin_command.cpp).
	out += "SERVER: " + ctx_.config.server_name + "\r\n";
	// The session clock the 0x58 status block reports (session_status.h).
	const uint32_t seconds = ctx_.np_protocol.host_run_duration_ms / 1000u;
	std::snprintf(line, sizeof(line), "UP-TIME: %ld %2.2ld:%2.2ld:%2.2ld\r\n", static_cast<long>(seconds / 86400u),
			static_cast<long>(seconds / 3600u % 24u), static_cast<long>(seconds / 60u % 60u),
			static_cast<long>(seconds % 60u));
	out += line;
	const int16_t tod_high = world != nullptr ? static_cast<int16_t>(world->weather.tod_fixed24 >> 16) : int16_t{0};
	out += "CURRENT TOD: " + std::to_string(tod_high) + "\r\n";
	out += "CURRENT # PLAYERS: " + std::to_string(slots.size()) + "\r\n";
	out += "CURRENT MAP: " + ctx_.config.mission_file + "\r\n";
	// The NULL arm's Overlays key; the waypoint family's "&" rides the debug word
	// dword_24C1930's 0x10000 (an input-binding toggle), clear on a server.
	// [orig: GameType_GetAbbreviation @0x520FD0]
	const uint32_t game_type = world != nullptr ? world->match.rules().game_type : ctx_.config.game_type;
	const char *key = game_type::abbreviation_overlay_key(game_type);
	std::string abbreviation;
	if (key[0] != '\0' && seams_.game_text) abbreviation = seams_.game_text("Overlays", key);
	out += "CURRENT GAME TYPE: " + abbreviation + "\r\n";
	out += "CURRENT PLAYERS: \r\n";
	out += kTableHeader;
	for (const NapiNPConnection *conn : slots) out += slot_row(ctx_, *conn);
	out += "MAP QUEUE: \r\n";
	// The queue buffer starts as `No missions in queue.`, which only a nonempty rotation in the
	// Game Loop overwrites.
	const bool queued = in_game() && seams_.rotation != nullptr && seams_.rotation->list().exists &&
			!seams_.rotation->list().entries.empty();
	out += queued ? rotation_lines(true) : std::string("No missions in queue.");
	return out;
}

} // namespace opennova::inmatch
