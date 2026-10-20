#include <runtime/inmatch/server_initial_state.h>

#include <runtime/inmatch/batch_chunker.h> // inmatch::slice_batch_pages (the shared byte-budget pager, ADR 0013)
#include <runtime/inmatch/end_round_protocol.h> // build_end_round_header (a join into a finished round)
#include <runtime/inmatch/server_dispatch_join.h> // dispatch_player_sync_request (the loopback's own 0x09)
#include <runtime/inmatch/server_entity_state.h> // Server_SendEntityStateToPlayer (the join's 0x0A)
#include <runtime/inmatch/server_flags.h> // the flag word's named bits
#include <runtime/inmatch/server_tick.h> // Server_RerollPlayerTickSeed
#include <runtime/inmatch/server_visible_players.h> // fan_spawn_slot_notice

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <formats/charattr/charattr.h>            // the restriction properties (the join's 0x41s)
#include <formats/def/reserved_items.h>           // DEF_TYPE_NAMED_LOCATION (the 0x0F location names)
#include <formats/mission/bms.h>                  // bms::File, bms::encode_loaded_header_blob (0x0B body)
#include <runtime/mission/mission_text.h>         // MissionText::location_name (the 0x0F location names)
#include <runtime/replication/client_state.h>     // kLocationNameSlotBytes (g_LocationNames' stride)
#include <runtime/replication/entity_wire_bridge.h>    // build_pool0_organic_batch / build_pool3_spawn_marker_batch
#include <base/gameprofile/game_type.h>          // is_waypoint_family (the §5.32 selector)
#include <net/npwire/ingame_encode.h>      // encode_organic_spawn_batch / encode_pool3_sync_batch
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/visible_players.h>   // encode_spawn_slot_notice (0x4D)
#include <net/title/title_identity.h>     // title_in_match (the 0x0F flags, DFX2-2)
#include <runtime/world/ai.h>                     // AiEntity (the join's placement reaches the motor)
#include <runtime/world/angle.h>                  // world::spawn_angle_bam (0x0F spawn pose)
#include <runtime/world/entity_spawn.h>           // world::warm_up_player_body (the join placement)
#include <runtime/world/entity.h>                 // world::Entity (0x0F spawn pose)
#include <runtime/world/spawn_select.h>           // world_has_spawn_zone (0x0F gameFlags bit0)
#include <runtime/world/world.h>
#include <base/io/le.h>
#include <base/io/log.h>

namespace opennova::inmatch {

namespace {

// ---------------------------------------------------------------------------
// §5.2a player-sync serializers. These were left "emit nothing / deferred" through P3-P6; ported
// here from the witnessed originals (grilled 2026-06-27, cross-checked vs the retail-lan-host-join
// golden frames 144-160). Each writes the host's own config the way the original writes its g_*
// globals — bytes a stock client accepts; golden byte-parity needs the host config seeded.
// ---------------------------------------------------------------------------

void put_cstr(std::vector<uint8_t> &b, const std::string &s) {
	b.insert(b.end(), s.begin(), s.end());
	b.push_back(0);
}

// [orig: NetPacket_WriteServerNameAndMapFile @0x505780] S2C 0x2C: server name + map file, two
// NUL-terminated C strings (was Kong-misnamed "TypeNameAndBaseName"). golden frame 144 = "Untitled\0"
// + "TDH_I5A.BMS\0". Sourced from the session config (the host's own server name + mission file).
std::vector<uint8_t> serialize_server_name_map(const GameConfig &cfg) {
	std::vector<uint8_t> b;
	put_cstr(b, cfg.server_name);
	put_cstr(b, cfg.mission_file);
	return b;
}

// [orig: CNapiServerConfig_BuildFlags @0x4c4dc0] The trailing flags dword of the 0x08 block.
uint32_t build_server_config_flags_impl(const NapiNPServerCtx &ctx) {
	const GameConfig &r = ctx.config;   // rule-flag inputs (squad / perm-death / config_flag_*)
	const GameConfig &gs = ctx.config;  // §6.4 game_settings inputs (passwords / game_type / mp_attributes)
	uint32_t flags = 0;
	if (!ctx.is_in_session) return flags;     // gated on is_in_session (+0x58)
	// dword_2550A04 is the one live mp-attribute store. Its TeamChoose bit feeds
	// BuildFlags even for a non-team game (fresh retail DM advertises 0x904).
	if ((r.mp_attributes & GameConfig::kMpAttribTeamChoose) != 0)
		flags = server_flag::kTeamChoice;
	switch (static_cast<uint32_t>(ctx.transport_mode)) {
	case 1: flags |= 0x400u; break;           // NovaWorld
	case 2: flags |= 0x100u; break;           // LAN
	case 3: flags |= 0x200u; break;           // (mode 3)
	default: break;
	}
	flags |= 0x800u;
	if (static_cast<uint32_t>(ctx.transport_mode) == 1) flags &= ~0x800u; // NovaWorld clears it
	if (!gs.server_password.empty()) flags |= server_flag::kServerPassword;
	// The password bit is NESTED under spectators-enabled: a configured
	// password with spectating off advertises neither bit.
	// [orig: CNapiServerConfig_BuildFlags @0x4c4dc0 — the 0x4000 strlen check
	// @0x4c4ead sits inside the `if (g_GameConfigState.maxSpectators_26C)` @0x4c4e8a]
	if (gs.spectator_slots != 0) {
		flags |= server_flag::kSpectators;
		if (!gs.spectator_password.empty()) flags |= server_flag::kSpectatorPassword;
	}
	if ((gs.game_type & 0x10000u) != 0) {     // team game
		if ((gs.mp_attributes & GameConfig::kMpAttribTeamChoose) != 0) flags |= server_flag::kTeamChoice;
		if (!gs.side_a_password.empty()) flags |= server_flag::kSideAPassword;
		if (!gs.side_b_password.empty()) flags |= server_flag::kSideBPassword;
	}
	if (r.permanent_death) flags |= 0x8000u;
	if (r.allow_sniper_scope_zoom) flags |= 0x10000u; // [orig: g_GameConfigState.allowSniperScopeZoom_5EC @0x2550CA4]
	return flags;
}

// [orig: ServerConfig_SerializeToPacket @0x505bd0] S2C 0x08: 10 rule dwords + 7 bytes + flags dword.
std::vector<uint8_t> serialize_server_config(const NapiNPServerCtx &ctx) {
	const GameConfig &r = ctx.config;
	std::vector<uint8_t> b;
	b.reserve(51);
	io::append_u32_le(b, r.respawn_time);
	io::append_u32_le(b, r.time_limit_minutes);
	io::append_u32_le(b, r.replay_enabled);      // [orig: g_ReplayEnabled @0x24D2120, SET `replay`]
	io::append_u32_le(b, r.game_type);
	io::append_u32_le(b, r.max_team_lives);      // [orig: g_MaxTeamLives @0x24D2130, SET `max_team_lives`]
	io::append_u32_le(b, r.score_limit);
	io::append_u32_le(b, r.respawn_timeout);     // [orig: g_RespawnTimeout @0x24D214C, SET `timeout`]
	io::append_u32_le(b, r.start_delay);
	io::append_u32_le(b, r.destroy_buildings);   // [orig: g_DestroyBuildings @0x24D2164, SET `destroybuild`]
	io::append_u32_le(b, r.death_messages);      // [orig: g_DeathMessages @0x24D2168, SET `deathmes`]
	for (int i = 0; i < 7; ++i) b.push_back(r.config_bytes[i]);
	// P2 advertised the create-session snapshot. Keep the later 0x08 record on
	// that same immutable contract even if a caller mutates ctx.config while a
	// paced joiner is still entering the world.
	io::append_u32_le(b, ctx.np_protocol.build_flags);
	return b; // 51 bytes
}

// [orig: NetPacket_SerializeWeaponRestrictionTable @0x5102c0] S2C 0x66: count byte, then (index,value)
// pairs for each restricted weapon (value 0 or the title's armory-only value: JO's 2, DFX2's 3,
// DFX2-10 [orig: @0x50d3b0 / @0x50d3de (dfx2.exe)]). The original scans a 255-entry table; we hold
// the restricted set directly (empty = count 0 -> single byte 0, golden frame 160).
std::vector<uint8_t> serialize_weapon_restrictions(const NapiNPServerCtx &ctx) {
	std::vector<uint8_t> b;
	b.push_back(static_cast<uint8_t>(ctx.weapon_restrictions.size()));
	for (const auto &e : ctx.weapon_restrictions) {
		b.push_back(e.first);  // weapon index
		b.push_back(e.second); // restriction value
	}
	return b;
}

// [orig: NetPacket_WriteClassAllowMask @0x510350] S2C 0x76: the configured host-global
// class-availability word consumed by NapiNPClientMsg_HandleClassAllowMask @0x42d540.
std::vector<uint8_t> serialize_class_allow_mask(uint16_t class_allow_mask) {
	std::vector<uint8_t> b;
	io::append_u16_le(b, class_allow_mask);
	return b;
}

// The host's GetTickCount: the S2C 0x19 / 0x1A / 0x0F stamps a joiner echoes
// back as its C2S 0x28 kill-page window, in the domain the 0x26 sender stamps
// entity+560 with (the world's logic tick through the deterministic seam; the
// connection clock only on the World-less unit path).
// [orig: NetPacket_WriteTimestamp @0x5046C0 / NetPacket_WriteTimestampB
//  @0x5046F0 / NetPacket_WriteWorldStateLoad0x0F @0x502D27 — GetTickCount();
//  Server_SendEntityStatePacket @0x509D7A]
uint32_t host_tick_count(const NapiNPServerCtx &ctx, uint32_t now_tick) {
	return io::host_milliseconds_for_logic_tick(
			ctx.world != nullptr ? ctx.world->logic_tick : now_tick);
}

// [orig: NetPacket_WriteTimestamp @0x5046c0] S2C 0x1A: the 4-byte GetTickCount.
std::vector<uint8_t> serialize_timestamp(uint32_t tick_count) {
	std::vector<uint8_t> b;
	io::append_u32_le(b, tick_count);
	return b;
}

// [orig: NetPacket_WriteBriefingText @0x506620] S2C 0x7E: MissionText
// [info]/briefing3 followed by [info]/briefing2 (or [info]/briefing fallback),
// copied byte-for-byte as two NUL-terminated cp1252 strings.
std::vector<uint8_t> serialize_briefing_text(const NapiNPServerCtx &ctx) {
	std::vector<uint8_t> b;
	b.reserve(ctx.mission_briefing3.size() + ctx.mission_briefing2.size() + 2);
	put_cstr(b, ctx.mission_briefing3);
	put_cstr(b, ctx.mission_briefing2);
	return b;
}

// [orig: Server_SendEntityStateToPlayer @0x517ba0 / NapiNPClientMsg_0x00F @0x42E200; §5.29] S2C 0x0F
// world-state-load — the game-start deploy unsticker. Carries the joiner's spawn pose, game flags, and
// the fixed 128-entry ammo-pool block; the client handler clears its g_ClientDeployHold load-gate and queues
// the post-load C2S burst (0x22/0x23/0x28/0x29/0x2D/0x32) that lets it deploy. Without it a retail
// joiner world-loads but stays undeployed (floods C2S 0x0f) — the observed live "stuck at 7%". Minimal
// faithful body: the TDM/DM path has waypointCount 0; waypoint Co-op serializes the recipient's
// blue route plus MissionText-resolved location labels. Layout is the §5.29 field map consumed by
// decode_world_state_load.
std::vector<uint8_t> serialize_world_state_load(NapiNPServerCtx &ctx, const NapiNPConnection &conn,
                                                uint32_t now_tick) {
	int32_t px = static_cast<int32_t>(ctx.config.spawn_x);
	int32_t py = static_cast<int32_t>(ctx.config.spawn_y);
	int32_t pz = static_cast<int32_t>(ctx.config.spawn_z);
	int16_t yaw = 0;
	int16_t pitch = 0;
	int16_t roll = 0;
	uint8_t recipient_team = 0;
	// Prefer the joiner's live spawned pool-0 entity (bound by Server_ProcessPendingPlayerSpawns before
	// the burst); fall back to the host-advertised spawn from the session config.
	if (ctx.world != nullptr && conn.link.owned_entity.valid()) {
		if (const world::Entity *e = ctx.world->registry.get(conn.link.owned_entity)) {
			// The position words, raw (D-NET-507) [orig: NetPacket_WriteWorldStateLoad0x0F @0x502D10 -
			//  entity+4 / +8 / +0xC @0x502D3A / @0x502D4B / @0x502D5C].
			px = e->position.x.q16;
			py = e->position.y.q16;
			pz = e->position.z.q16;
			// §5.29 wire yaw is an i16 the client <<16 to a 16.16 BAM = the high half of the engine-frame
			// heading (90 - mission_yaw); matches snapshot_of / pose_for_conn (D-NET-86). The fresh
			// player's angles are its start marker's placement angles, so the high halves are the
			// spawn angles' (world::spawn_angle_bam, low half zero) [orig:
			// NetPacket_WriteWorldStateLoad0x0F `movzx edx, word ptr [ebp+12h]` @0x502D6D].
			yaw = static_cast<int16_t>(world::spawn_angle_bam(90 - e->yaw) >> 16);
			// Pitch/roll are pure degree-to-BAM axes (no frame inversion); the
			// wire carries their 16.16 high words and the retail joiner applies
			// them straight onto its local entity (<<16) at every 0x0F.
			// [orig: NetPacket_WriteWorldStateLoad0x0F @0x502D80 (pitch hi)
			//  / @0x502D9A (roll hi); NapiNPClientMsg_0x00F @0x42E3E9/@0x42E3F2]
			pitch = static_cast<int16_t>(world::spawn_angle_bam(e->pitch) >> 16);
			roll = static_cast<int16_t>(world::spawn_angle_bam(e->roll) >> 16);
			recipient_team = e->team;
		}
	}
	std::vector<uint8_t> b;
	b.reserve(640);
	io::append_u32_le(b, host_tick_count(ctx, now_tick)); // GetTickCount @0x502D27
	io::append_u32_le(b, static_cast<uint32_t>(px));        // spawn pos (16.16)
	io::append_u32_le(b, static_cast<uint32_t>(py));
	io::append_u32_le(b, static_cast<uint32_t>(pz));
	io::append_u16_le(b, static_cast<uint16_t>(yaw));       // yaw  (i16, client <<16)
	io::append_u16_le(b, static_cast<uint16_t>(pitch));     // pitch (i16 hi word, client <<16)
	io::append_u16_le(b, static_cast<uint16_t>(roll));      // roll  (i16 hi word, client <<16)
	const bool has_spawn_zones =
			ctx.world != nullptr && ctx.world->zones.has_spawn_zone();
	// A DFX2 host sets bit 0 whatever the mission (DFX2-2) [orig: the flags byte's 1 in
	//  NetPacket_WriteWorldStateLoad0x0F @0x4ffeed (dfx2.exe)].
	const bool spawn_zone_flag =
			has_spawn_zones || title_in_match(ctx.np_protocol.identity).spawn_zone_flag_always;
	uint8_t game_flags = spawn_zone_flag ? 0x01u : 0x00u;
	if (ctx.config.default_spawn_requires_no_team_zone != 0 &&
			ctx.is_in_session != 0) {
		game_flags |= 0x02u;
	}
	// [orig: NetPacket_WriteWorldStateLoad0x0F @ 0x502D10, flag store @ 0x502DC7]
	if (ctx.world != nullptr && ctx.world->rules.cease_fire) game_flags |= 8u;
	b.push_back(game_flags);
	// bit0 = SpawnZoneList nonempty; bit1 = the target-less spawn restriction
	// while in session. [orig: NetPacket_WriteWorldStateLoad0x0F
	// @0x502DA7..0x502DC4; g_RespawnRequiresTeamDead @0x24D2260]
	// The fixed 128-i32 block is the authority player's per-ammo-class pool
	// table (serverPlayer+88664 -> client g_LocalAmmoPools @0xB75FE8), retained
	// when this connection's C2S 0x2F loadout is accepted.
	for (int32_t value : conn.reply.ammo_pools)
		io::append_u32_le(b, static_cast<uint32_t>(value));
	// The waypoint body is present only for the witnessed waypoint gametype and
	// a blue/team-1 recipient. Promotion retains the first PlayerRoute route in
	// World::waypoints; each entry's node is the corresponding pool-3 marker
	// index, and the wire word IS that raw pool-3 index — NOT a 0x3000|node
	// handle: the writer stores the same dword it passed to
	// Pool_GetEntryUnchecked(3, index) (`movzx ecx, word ptr [ebx]` @0x502f35 ->
	// `mov [edi], cx` @0x502f45), and the stock client indexes
	// `pool3.base + word * stride` with it unmasked (@0x42e4a3 ->
	// Pool_GetEntryUnchecked @0x441fc0, then strncpy into that record). A
	// 0x3000|node word sent a retail joiner 12288 records past the pool
	// (corrected 2026-09-10, jo-c cross-check). [orig: NetPacket_WriteWorldStateLoad0x0F @0x502e41..0x502edb]
	const bool waypoint_gametype =
			game_type::is_waypoint_family(ctx.config.game_type);
	std::vector<const world::WaypointEntry *> waypoints;
	if (waypoint_gametype && recipient_team == 1 && ctx.world != nullptr) {
		waypoints.reserve(std::min<std::size_t>(
				ctx.world->script.waypoints.entries.size(), 128));
		for (const world::WaypointEntry &entry : ctx.world->script.waypoints.entries) {
			if (waypoints.size() >= 128) break;
			if (entry.node < 0 || entry.node > 0x0FFF) continue;
			waypoints.push_back(&entry);
		}
	}
	io::append_u16_le(b, static_cast<uint16_t>(waypoints.size()));
	for (const world::WaypointEntry *entry : waypoints) {
		io::append_u16_le(b, static_cast<uint16_t>(entry->node));
		io::append_u16_le(b, static_cast<uint16_t>(entry->name_id));
		// The marker's done byte: a DFX2 joiner stores it, JO's skips it.
		// [orig: `mov al, [eax+218h]` @0x502f5f] [orig: @0x50008f (dfx2.exe)]
		b.push_back(entry->done ? 1u : 0u);
	}
	// LOCATION NAMES [orig: NetPacket_WriteWorldStateLoad0x0F @0x502D10 tail — u16 count +
	// cstrings from g_LocationNames (64-B stride), registered at BMS spawn of def-type 2044
	// markers in spawn order (Entity_SpawnFromBMSRecord @0x40f182-0x40f221; the text is the
	// mission's Locations/LOCATION%03i string, fallback = the key string)]. The client's 0x0F
	// handler overwrites its LOCAL copies — the deploy-map name labels (golden ASH_I5A: 6
	// names, "North Sea Village".."Katulus' Mound"). The host shell supplies resolved
	// MissionText values; tests/tools without that table fall back to registry marker labels.
	std::vector<std::string> location_names = ctx.mission_location_names;
	if (location_names.empty() && ctx.world != nullptr) {
		ctx.world->registry.for_each([&](const world::Entity &e) {
			if (e.handle.pool() != 3 || e.item_id != def::DEF_TYPE_NAMED_LOCATION) return;
			location_names.push_back(e.name);
		});
	}
	const std::size_t location_count = std::min<std::size_t>(
			location_names.size(), 0xFFFFu);
	io::append_u16_le(b, static_cast<uint16_t>(location_count));
	for (std::size_t i = 0; i < location_count; ++i) {
		const std::string &n = location_names[i];
		for (char ch : n) b.push_back(static_cast<uint8_t>(ch));
		b.push_back(0);
	}
	return b; // 539 B + the location-name block (golden 624 B with the 6 ASH names)
}

// [orig: NetPacket_CopyTenBytes @0x503900 over the table @0x82F1D8] S2C 0x2A: one 10-byte record.
// The table's six 16-byte rows (subphases 10..15) each carry threshold 0 and this same record
// (the bytes at 0x82F1D8..0x82F237); a row is skipped, its subphase spent with no send, while its
// threshold exceeds the slot's signed byte +7 (@0x51BFB9..0x51BFC2), which a threshold of 0 never
// does unless +7 goes negative, and no writer of +7 is witnessed: all six go out (golden frames
// 148-158).
constexpr std::array<uint8_t, 10> k0x2aRecord = {0x00, 0x04, 0xb0, 0xab, 0xb2, 0xb2, 0xbf, 0xbc, 0xbd, 0xba};

// How the machine treats one tag this step.
enum class Action { EmitEmpty, EmitBody, SkipSilent };

// Page a world-stream pool into per-datagram batches and push each as its own InitialStateMessage.
// The original caps each S2C world-stream datagram at ~650 B and advances a pool cursor across calls
// [orig: Server_SendInitialGameStateToPlayer @0x51bba0]. The selected limit preserves each serializer's
// witnessed post-write margin; 0x45 tiles select their distinct pre-write cap.
// `encode_page(off, cnt)` slices the pool's records [off, off+cnt) into a sub-batch and returns its
// encoded body. Each page climbs the F3 entity_batch_count. An empty pool still emits one header-only
// batch (a faithful empty-batch marker). Instruments the host log with the record/page/byte counts so
// a join's world-load stream is visible directly (e.g. an empty 0x10 vs the full paged static set).
// PACED: emit pages of one pool starting from the record cursor `b.phase_loop_counter`, at most
// `budget` of them (one per step for a remote, retail's one page per call). Returns true if the
// pool is fully emitted (cursor reset to 0), false if the budget ran out mid-pool (cursor saved
// for the next step).
template <typename EncodePage>
bool emit_paged_pool(uint8_t tag, std::size_t n_records, BatchPageLimit page_limit,
                     EncodePage encode_page,
                     InitialStateStep &step, InitialStateBurst &b, std::size_t budget) {
	if (n_records == 0) {
		// Empty pool: one header-only marker (emitted on first visit), then done.
		step.messages.push_back(InitialStateMessage{tag, encode_page(0, 0)});
		++b.entity_batch_count;
		++step.world_batches_emitted;
		b.phase_loop_counter = 0;
		return true;
	}
	// Page from the saved cursor within this tick's remaining datagram budget, via the shared chunker.
	const std::size_t max_pages = budget > step.messages.size() ? budget - step.messages.size() : 0;
	BatchPageResult res =
			slice_batch_pages(n_records, page_limit, encode_page, b.phase_loop_counter, max_pages);
	for (std::vector<uint8_t> &body : res.pages) {
		step.messages.push_back(InitialStateMessage{tag, std::move(body)});
		++b.entity_batch_count;
		++step.world_batches_emitted;
	}
	b.phase_loop_counter = res.exhausted ? 0 : static_cast<uint16_t>(res.next_cursor);
	return res.exhausted;
}

// Advance the §5.2a burst by exactly ONE phase: resolve the current (tag, action) for the cursor,
// emit it into `step` (or not), climb the world-batch counter, then move the cursor (handling the
// player-sync -> world-stream transition and the terminator). The one-shot driver below loops this
// until the burst completes. `b` is conn.burst.
void advance_burst_one_phase(NapiNPServerCtx &ctx, NapiNPConnection &conn, InitialStateStep &step,
                             uint32_t now_tick, std::size_t budget) {
	InitialStateBurst &b = conn.burst;
	uint8_t tag = 0;
	Action action = Action::SkipSilent;
	bool is_world_batch = false;
	bool world_pool_done = true; // false when a world-stream pool was paused mid-pool (budget hit)

	if (conn.slot_state == PlayerSlotState::PlayerSync &&
			b.player_sync_subphase >= InitialStateBurst::kSyncTail) {
		// From subphase 16 the track's one call sends its whole tail, 0x1C, the
		// loaded mission's 616-byte header 0x0B, 0x66, 0x76 and 0x11 (the 00TRg oracle
		// carries exactly these in one 0x83 boundary after the sixth 0x2A), then enters
		// game state 9 and slot state 3 with the subphase back to 0. The 0x0B is never
		// skipped: a host in session always holds its mission's header (the null test
		// is for World-only unit fixtures with no mission).
		// [orig: Server_SendInitialGameStateToPlayer @0x51BBA0 - the tail
		//  @0x51C03A..0x51C10E (the 0x0B NetPacket_WriteBMSHeader over g_BmsHeaderBlock
		//  @0x51C054..0x51C077), game state 9 @0x51C113..0x51C11C, slot state 3 @0x51C134,
		//  subphase 0 @0x51C13C]
		step.messages.push_back(InitialStateMessage{s2c::NOOP, {}});
		if (ctx.mission != nullptr) {
			std::vector<uint8_t> body;
			std::string err;
			if (bms::encode_loaded_header_blob(*ctx.mission, body, err))
				step.messages.push_back(
						InitialStateMessage{s2c::BMS_HEADER, std::move(body)});
		}
		step.messages.push_back(InitialStateMessage{
				s2c::WEAPON_RESTRICTIONS, serialize_weapon_restrictions(ctx)});
		step.messages.push_back(InitialStateMessage{
				s2c::CLASS_ALLOW_MASK, serialize_class_allow_mask(ctx.config.class_allow_mask)});
		step.messages.push_back(
				InitialStateMessage{s2c::DISCONNECT_UNLOCK, {}});
		b.game_state = 9;
		conn.slot_state = PlayerSlotState::AwaitWorldRequest;
		b.player_sync_subphase = InitialStateBurst::kSyncDone;
		return;
	} else if (conn.slot_state == PlayerSlotState::PlayerSync) {
		// Player-sync track: one tag per subphase (8..15); the tail above from 16.
		switch (b.player_sync_subphase) {
		case InitialStateBurst::kSyncMissionMapNames: tag = s2c::MISSION_MAP_NAMES; action = Action::EmitBody; break;  // NetPacket_WriteServerNameAndMapFile @0x505780
		case InitialStateBurst::kSyncSessionConfig: tag = s2c::SESSION_CONFIG; action = Action::EmitBody; break;  // ServerConfig_SerializeToPacket @0x505bd0
		case InitialStateBurst::kSyncChatHistoryFirst: case InitialStateBurst::kSyncChatHistoryFirst + 1:
		case InitialStateBurst::kSyncChatHistoryFirst + 2: case InitialStateBurst::kSyncChatHistoryFirst + 3:
		case InitialStateBurst::kSyncChatHistoryFirst + 4: case InitialStateBurst::kSyncChatHistoryLast:
		         tag = s2c::CHAT_HISTORY; action = Action::EmitBody; break;  // NetPacket_CopyTenBytes @0x503900 (×6, table @0x82F1D8)
		default: break;
		}
	} else if (conn.slot_state == PlayerSlotState::WorldStream) {
		// World-stream track: stream EACH pool in FULL, PAGED into per-datagram batches. A retail joiner
		// needs the complete static / pool-1 / pool-3 world to finish loading the map — the prior
		// minimal stream (empty 0x10, skipped 0x0D, spawn-marker-only 0x20) stalled a live retail join at
		// 7%. This reverses the D-NET-97/98 shortcut (was sized to our own client; a stock client needs
		// the real world-state). [orig: Server_SendInitialGameStateToPlayer @0x51bba0, phases
		// 0x10 -> 0x0D -> 0x0C -> 0x20]. The paged emits push directly; the action switch is a no-op here.
		action = Action::SkipSilent;
		switch (b.world_stream_phase) {
		case InitialStateBurst::kStreamInit: // phase-0 init [orig: @0x51bc1a case 0]: zero the page cursor, advance to phase 1
		        // (one no-emit tick). The C2S 0x0A handler re-enters here on every spawn-menu
		        // request (it writes world_stream_phase = 0 [orig: @0x5132f6]).
			b.phase_loop_counter = 0;
			break;
		case InitialStateBurst::kStreamPool2Static: { // 0x10 pool-2 static structures [orig: NetPacket_SerializePool2StaticToBuffer @0x5042f0]
			const opennova::StaticEntityBatch full = opennova::replication::build_pool2_static_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x10, full.records.size(),
			                                initial_state_page_limits::pool2_static(),
			                                [&](std::size_t off, std::size_t cnt) {
				opennova::StaticEntityBatch p;
				p.start_index = static_cast<uint16_t>(full.start_index + off);
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<int16_t>(p.records.size());
				// Each page is one serializer call: its zone outs start over (D-NET-394).
				opennova::replication::stream_pool2_zone_blocks(*ctx.world, p);
				return opennova::encode_static_entity_batch(p);
			}, step, b, budget);
			break;
		}
		case InitialStateBurst::kStreamPool1Items: { // 0x0D pool-1 destructibles / items / vehicles [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940]
			const opennova::PoolSpawnBatch full = opennova::replication::build_pool1_spawn_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x0D, full.records.size(),
			                                initial_state_page_limits::pool1_entities(),
			                                [&](std::size_t off, std::size_t cnt) {
				opennova::PoolSpawnBatch p;
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<int16_t>(p.records.size());
				// Each page is one serializer call: its zone outs start over (D-NET-394).
				opennova::replication::stream_pool1_zone_bytes(*ctx.world, p);
				return opennova::encode_pool_spawn_batch(p);
			}, step, b, budget);
			break;
		}
		case InitialStateBurst::kStreamPool0Organics: { // 0x0C pool-0 organics (carry entity+0x78 dcb for the joiner owner-ID match) [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030a0]
			// The same records for every recipient: each player's flags word carries its own
			// entity's bit0 (deploy screen / spectating), not a recipient marker (D-NET-136).
			const opennova::OrganicSpawnBatch full =
					opennova::replication::build_pool0_organic_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x0C, full.records.size(),
			                                initial_state_page_limits::pool0_organics(),
			                                [&](std::size_t off, std::size_t cnt) {
				opennova::OrganicSpawnBatch p;
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<uint16_t>(p.records.size());
				return opennova::encode_organic_spawn_batch(p);
			}, step, b, budget);
			break;
		}
		case InitialStateBurst::kStreamPool3Markers: { // 0x20 pool-3 markers / waypoints / nav nodes (FULL, not just spawn markers) [orig: NetPacket_SerializeEntityPoolToPacket @0x503460]
			const opennova::Pool3SyncBatch full = opennova::replication::build_pool3_marker_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x20, full.records.size(),
			                                initial_state_page_limits::pool3_markers(),
			                                [&](std::size_t off, std::size_t cnt) {
				opennova::Pool3SyncBatch p;
				// The start word is the pool cursor, a slot: the records are slot-aligned,
				// zero words included (D-NET-416). [orig: NetPacket_SerializeEntityPoolToPacket
				// @0x503460 -- the cursor written @0x5034C5]
				p.start_index = static_cast<uint16_t>(full.start_index + off);
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<int16_t>(p.records.size());
				return opennova::encode_pool3_sync_batch(p);
			}, step, b, budget);
			break;
		}
		case InitialStateBurst::kStreamTerrainTiles: { // 0x45 terrain-tile (.til) load [orig: Terrain_SerializeTiles @0x6080F0, §5.37/D-NET-83]
			// Stream the mission's terrain-tile array so the joiner's g_LoadingProgress climbs 5 -> 6 and
			// its terrain finishes loading. The raw .til header maps 1:1 onto the 0x45 header
			// (`[u32 'til0'][u32 count][u32 size_word][u32 word12]` then count × 12-B entries, the two
			// words as the file holds them, formats/til TilHeaderFacts). EMPTY .til =>
			// faithfully skip (Terrain_SerializeTiles returns 0 with no tile data). Page boundaries are set
			// by the shared byte-budget chunker (client reassembles by start/end index, so the split is
			// transport-transparent — not a byte-parity field like the entity pools).
			action = Action::SkipSilent;
			const std::vector<uint8_t> &til = ctx.terrain_til_data;
			constexpr std::size_t kTilHeaderBytes = 16, kTilEntryBytes = 12;
			auto rd_u32 = [](const uint8_t *p) -> uint32_t {
				return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
			};
			if (til.size() < kTilHeaderBytes || rd_u32(til.data()) != kTerrainTileMagic) {
				world_pool_done = true; // no (valid) .til -> emit nothing, advance past phase 5
				break;
			}
			const uint32_t total_count = rd_u32(til.data() + 4);
			const uint32_t size_word = rd_u32(til.data() + 8);
			const uint32_t word12 = rd_u32(til.data() + 12);
			std::size_t avail = (til.size() - kTilHeaderBytes) / kTilEntryBytes;
			std::size_t n_tiles = total_count < avail ? total_count : avail; // bound to actual bytes
			if (n_tiles == 0) { // no tiles -> Terrain_SerializeTiles returns 0 (emit nothing), not an empty marker
				world_pool_done = true;
				break;
			}
			// The last chunk leaves the cursor at the end, and the phase advances on the
			// NEXT call, whose serializer writes nothing: one more step with no send.
			// [orig: Server_SendInitialGameStateToPlayer case 5 @0x51BDE7..0x51BE00 -
			//  NetPacket_WriteTerrainTiles @0x506570 returns 0 when Terrain_SerializeTiles
			//  writes nothing past the +89884 cursor]
			if (b.phase_loop_counter >= n_tiles) {
				b.phase_loop_counter = 0;
				world_pool_done = true;
				break;
			}
			world_pool_done = false;
			const bool tiles_drained = emit_paged_pool(0x45, n_tiles, initial_state_page_limits::terrain_tiles(),
			                                [&](std::size_t off, std::size_t cnt) {
				opennova::TerrainLoadBatch batch;
				batch.has_header = (off == 0);
				batch.start_index = static_cast<uint16_t>(off);
				batch.end_index = static_cast<uint16_t>(off + cnt);
				if (batch.has_header) {
					batch.magic = kTerrainTileMagic;
					batch.tile_count = total_count;   // TOTAL set size (drives the client's alloc)
					batch.header_field2 = size_word;
					batch.header_field3 = word12;
				}
				batch.tiles.reserve(cnt);
				for (std::size_t i = 0; i < cnt; ++i) {
					const uint8_t *e = til.data() + kTilHeaderBytes + (off + i) * kTilEntryBytes;
					opennova::TerrainTileEntry t;
					t.word0 = rd_u32(e);
					t.word1 = rd_u32(e + 4);
					t.word2 = rd_u32(e + 8);
					batch.tiles.push_back(t);
				}
				return opennova::encode_terrain_load_batch(batch);
			}, step, b, budget);
			if (tiles_drained) b.phase_loop_counter = static_cast<uint16_t>(n_tiles);
			break;
		}
		case InitialStateBurst::kStreamMissionText:
			// The original phase is conditional on a loaded MissionText resource. A
			// valid table may contain empty strings, so use the explicit loaded bit
			// rather than treating an empty value as absence.
			if (ctx.mission_text_loaded) {
				tag = s2c::SERVER_CONFIG_STRINGS;
				action = Action::EmitBody;
			}
			break;
		case InitialStateBurst::kStreamWaitForGameStartAck: tag = s2c::WAIT_FOR_GAME_START_ACK; action = Action::EmitBody; break; // NetPacket_WriteTimestamp @0x5046c0
		default: break;
		}
	}

	// Resolve the action into an emitted message (or not).
	switch (action) {
	case Action::EmitEmpty:
		step.messages.push_back(InitialStateMessage{tag, {}});
		break;
	case Action::EmitBody: {
		std::vector<uint8_t> body;
		if (tag == s2c::MISSION_MAP_NAMES) {
			body = serialize_server_name_map(ctx.config);
		} else if (tag == s2c::SESSION_CONFIG) {
			body = serialize_server_config(ctx);
		} else if (tag == s2c::CHAT_HISTORY) {
			body.assign(k0x2aRecord.begin(), k0x2aRecord.end());
		} else if (tag == s2c::WEAPON_RESTRICTIONS) {
			body = serialize_weapon_restrictions(ctx);
		} else if (tag == s2c::CLASS_ALLOW_MASK) {
			body = serialize_class_allow_mask(ctx.config.class_allow_mask);
		} else if (tag == s2c::WAIT_FOR_GAME_START_ACK) {
			body = serialize_timestamp(host_tick_count(ctx, now_tick));
		} else if (tag == s2c::SERVER_CONFIG_STRINGS) {
			body = serialize_briefing_text(ctx);
		}
		if (action == Action::EmitBody) step.messages.push_back(InitialStateMessage{tag, std::move(body)});
		break;
	}
	case Action::SkipSilent:
		break;
	}

	// A world-stream message landing (incl. the empty 0x10 marker) climbs entity_batch_count — the
	// F3 readiness signal the streaming-entered latch reads.
	if (is_world_batch && (action == Action::EmitBody || action == Action::EmitEmpty)) {
		++b.entity_batch_count;
		++step.world_batches_emitted;
	}

	// Advance the cursor and handle track transitions / the terminator.
	// The tail's state 3 PARKS the burst: the world stream starts only when the client's C2S
	// 0x0A spawn-menu request advances 3 -> 4 (NapiNPServerMsg_HandlePlayerSpawnRequest
	// @0x513260; golden retail-ashi5a S 0x11 f=201572 -> C 0x0A f=201573 -> first S 0x10
	// f=201713). Streaming without that request raced a cold retail client's mission build
	// and mis-bound its own CharacterEntity (D-NET-150).
	if (conn.slot_state == PlayerSlotState::PlayerSync) {
		++b.player_sync_subphase; // [orig: LABEL_42 @0x51C007]
	} else if (conn.slot_state == PlayerSlotState::WorldStream) {
		// A paged world-stream pool (phases 1-4) that hit the per-tick budget mid-pool is NOT done —
		// stay on this phase and resume from b.phase_loop_counter next tick (the pacing resume point).
		if (!world_pool_done) {
			// remain on the same world_stream_phase; cursor saved in b.phase_loop_counter
		} else if (b.world_stream_phase >= InitialStateBurst::kStreamWaitForGameStartAck) {
			// Case 7's 0x1A ends the stream: game state 9 and slot state 5, where the slot
			// waits for the joiner's C2S 0x0B (Server_OnPlayerJoin).
			// [orig: Server_SendInitialGameStateToPlayer case 7 - CNetPlayer_SetGameState(.., 9)
			//  @0x51BED7, slot state 5 @0x51BEF3]
			b.game_state = 9;
			conn.slot_state = PlayerSlotState::AwaitJoin;
		} else {
			++b.world_stream_phase;
		}
	}
}

// The slot's network state reset at its state-6 entry, in session alone: the time-sync round
// state and both silence counters zeroed, the control quartet's countdown seeded with its
// 744-tick period (so the first quartet comes 744 gated ticks after the live-age gate opens,
// not on its first), and the loaded-model reply stamp set to the frame clock. The other words
// it resets (+0x178F0, +0x178F8 = 62, +0x17CFC, the two 512-byte blocks, +0x17D10, +0x17D14,
// +0x17D1C) have no reader here. (D-NET-532)
// [orig: PlayerSlot_ResetNetworkState @0x4F9FD0 - is_in_session @0x4F9FD9, dwords 22486..22493
//  @0x4F9FF1..0x4FA01B, the countdown +0x15F20 = 744 @0x4FA021, +0x15F24 = 0 @0x4FA02B,
//  +97560 = dword_A87060 @0x4FA073; called from Server_OnPlayerJoin @0x51A72B]
void PlayerSlot_ResetNetworkState(const NapiNPServerCtx &ctx, NapiNPConnection &conn,
		uint32_t now_tick) {
	if (!ctx.is_in_session) return;
	SessionReplyState &st = conn.reply;
	st.time_sync_host_baseline_ms = 0;
	st.time_sync_round_host_ms = 0;
	st.time_sync_current_host_ms = 0;
	st.time_sync_client_baseline_ms = 0;
	st.time_sync_previous_client_ms = 0;
	st.time_sync_latest_client_ms = 0;
	st.time_sync_sequence = 0;
	st.time_sync_unanswered_count = 0;
	st.control_request_countdown = CONTROL_REQUEST_PERIOD_TICKS;
	st.charattr_unanswered_count = 0;
	st.loaded_model_reply_frame = now_tick;
}

} // namespace

// [orig: Server_OnPlayerJoin @0x51A680]
std::vector<InitialStateMessage> Server_OnPlayerJoin(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		uint32_t now_tick) {
	std::vector<InitialStateMessage> out;
	world::World &world = *ctx.world;
	SessionReplyState &st = conn.reply;
	const TitleInMatch &title_rules = title_in_match(ctx.np_protocol.identity);
	// The head (D-NET-502): the join, not the add, writes these slot words, so a stock
	// client's join-time C2S 0x2F pair (ahead of its 0x0B in one packet) has seeded the
	// armory cooldown before the join zeroes it.
	// The slot gates are the 0x0B receiver's (Server_HandlePlayerJoinRequest)
	// [orig: Server_OnPlayerJoin @0x51a6a3..0x51a6c7].
	// [orig: Server_OnPlayerJoin @0x51A680 - +100360 = 0 @0x51A6CD; the state byte +89912 = 1
	//  @0x51A6D3, 3 while g_PreRoundDelayTimer runs @0x51A6DA..0x51A6E2, |= 0x10 with spawn zones
	//  @0x51A6E9..0x51A6F2; CNetPlayer_SetGameState(.., 10) @0x51A6FD; the state-6 entry
	//  @0x51A70A..0x51A770; DFX2's fixed 0x13 @0x517717 (dfx2.exe)]
	st.chat_last_ms = 0;
	// Bit 1, the pre-round loadout latch: set while the pre-round timer runs, always for DFX2.
	conn.link.preround_loadout_latch =
			world.preround_delay_seconds != 0 || title_rules.join_state_fixed;
	// Bit 0x10, undeployed: with deploy-selectable spawn zones (always for DFX2). Out of a
	// session the hold shows nothing (its hidden bit and overlay bit both ride the per-frame
	// 0x0A writer, which only a session runs), so the single player is never held.
	const bool is_host_own = conn.type == NapiNPConnection::kTypeClientSide;
	conn.link.respawn_pending =
			(world.zones.has_spawn_zone() || title_rules.join_state_fixed) &&
			(!is_host_own || world.rules.mp_session);
	// The roster row mirrors the undeployed bit, which event 25 reads [orig:
	//  Server_UpdateCaptureZoneProximity @0x5087A2].
	world.match.set_player_respawn_pending(conn.link.owned_entity, conn.link.respawn_pending);
	// SetGameState(10) sets 0x04 (the frontier hint) and clears 0x08 (the refused-touch
	// hold), as the state-6 entry's `& 0xF2 | 5` does again.
	// [orig: CNetPlayer_SetGameState @0x4C4213..0x4C421F; @0x51A730..0x51A73A]
	// Entering game state 10 arms the round-event watermark at the frame clock less one
	// (NEW-L1-8) [orig: CNetPlayer_SetGameState @0x4C4225..0x4C422D].
	st.frontier_hint_pending = true;
	st.capture_nag_held = false;
	conn.burst.game_state = 10;
	conn.link.round_watermark = world.out.rounds.frame_clock - 1;
	if (conn.slot_state != PlayerSlotState::Joined) {
		conn.slot_state = PlayerSlotState::Joined; // [orig: the test @0x51A707..0x51A70A, @0x51A712]
		conn.link.round_watermark = world.out.rounds.frame_clock - 1; // [orig: @0x51A740..0x51A74C]
		// The quality level and its resend flag: a level kept across a map change (or stored
		// ahead of the join) is gone, and the client's next C2S 0x4C above 0 dirties it again.
		// [orig: +418 = 0 @0x51A71F, +89860 = 0 @0x51A725]
		st.client_quality = 0;
		st.client_quality_dirty = false;
		PlayerSlot_ResetNetworkState(ctx, conn, now_tick); // [orig: @0x51A72B] (D-NET-532)
		conn.link.armory_reuse_seconds = 0;                // +356 [orig: @0x51A752]
		st.rtt_request_countdown = 0;                      // +89816 [orig: @0x51A758]
		st.state6_entry_host_ms = ctx.np_protocol.host_run_duration_ms; // +100580 [orig: @0x51A770]
		st.state6_entry_host_ms_valid = true;
	}

	// The join places the body again, in session (D-NET-503): the add hid it and placed it
	// (Entity_SpawnFromAnimSlotProperty's call with a spawn word of 0xFFFF), nothing moved it
	// since, and this second no-pick placement (a spawn word whose low half is 0, which no
	// target resolves) is the one the 0x0F carries: the Co-op arm's marker copy, or a team
	// mode's next start marker off the spawn cycle. The motor takes the placed pose.
	// [orig: Server_OnPlayerJoin @0x51A786 (in session @0x51A77D) -> Server_PositionPlayerForSpawn
	//  @0x50CF60, the no-pick arm @0x50CFB1; Entity_SpawnFromAnimSlotProperty @0x43C459]
	world::Entity *body = world.registry.get(conn.link.owned_entity);
	if (ctx.is_in_session && body != nullptr) {
		const world::SpawnSlotState slot_state{conn.link.spectator, conn.spectator_restore_team};
		const world::SpawnPointResult pose = world::resolve_player_spawn_pose(world, body->handle,
				world::EntityHandle{}, st.player_slot, conn.assigned_team, ctx.config.game_type,
				slot_state);
		if (pose.found) {
			body->position = pose.position;
			body->yaw = pose.yaw;
			body->pitch = pose.pitch;
			body->roll = pose.roll;
			world::apply_spawn_point_latches(*body, pose);
		}
		// The scored arm's warm-up, where the pick left the body (D-NET-431).
		// [orig: Server_PositionPlayerForSpawn @0x50D572]
		if (pose.warm_up) world::warm_up_player_body(world, *body);
		// The motor's heading word and smoothing target follow (the position is
		// the body's own words; the placement writes nothing else).
		if (pose.found || pose.warm_up) {
			if (world::AiEntity *motor = world.ai.for_handle(body->handle)) {
				const int32_t placed[3] = {
						body->position.x.q16, body->position.y.q16, body->position.z.q16};
				for (int axis = 0; axis < 3; ++axis)
					motor->net_smooth_target[axis] = placed[axis];
				if (pose.found) {
					motor->heading = pose.heading_bam;
					motor->net_smooth_heading = pose.heading_bam;
				}
			}
		}
	}
	// A spectator's body stays hidden and damage-disabled; any other's hidden bit clears
	// (+96481, the gate's other byte, the add zeroes @0x51D534 and nothing sets). In session
	// the 0x0A writer's stores OR it back while the slot is undeployed, the join's own 0x0A
	// first; out of session the clear stands.
	// [orig: Server_OnPlayerJoin @0x51A78E..0x51A7DA (the spectator arm @0x51A7AE / @0x51A7B3,
	//  the clear @0x51A7DA); NetPacket_WritePlayerState @0x4FF7AA..0x4FF7B8]
	if (body != nullptr) {
		if (conn.link.spectator) {
			body->flags |= 1u;
			body->damage_state = -1;
		} else {
			body->flags &= ~1u;
		}
	}

	// The tail: the deploy unsticker. Without it a retail joiner world-loads but stays
	// undeployed (floods C2S 0x0F, "stuck at 7%"). 0x41 per charattr restriction, 0x42 the
	// charattr latches, in session the slot's first 0x0A, 0x0F world-state-load (clears the
	// client's g_ClientDeployHold load-gate and queues its deploy burst), 0x4D player-index,
	// 0x61 session-key/seed, the cease-fire line and the round-over legs when they apply,
	// 0x3E terminator. [§5.29 / §5.43 / D-NET-114]
	// Each set restriction word, one 0x41 of the property it disables, to this slot alone
	// (D-NET-374). [orig: Server_OnPlayerJoin @0x51a7e2 -> Server_SendCharAttrRestrictionsToPlayer
	//  @0x509950: g_SessionNoCharAbilities -> 0 @0x5099b4, g_SessionNoWeaponRecoil -> 5 @0x5099eb,
	//  g_SessionNoCrossHairSpread -> 6 @0x509a22, g_SessionNoScopeDrift -> 7 @0x509a59; mask 0x20,
	//  msgClass 1, one byte each]
	const std::pair<int32_t, uint8_t> restrictions[] = {
			{ctx.config.no_char_abilities, charattr::kAttributes},
			{ctx.config.no_weapon_recoil, charattr::kRecoilMute},
			{ctx.config.no_crosshair_spread, charattr::kXhairMute},
			{ctx.config.no_scope_drift, charattr::kScopeMute},
	};
	for (const auto &[word, property] : restrictions)
		if (word != 0) out.push_back(InitialStateMessage{s2c::CHARATTR_PROPERTY_CLEAR, {property}});
	// The latches as the authority holds them [orig: NetPacket_WriteCharAttrDisabledProperties
	//  @0x505BA0 -> CharAttr_PackDisabledProperties @0x412550, sent @0x51A81F userParam=1].
	const uint16_t latches = world.tables.charattr_disabled_word;
	out.push_back(InitialStateMessage{s2c::CHARATTR_DISABLED_PROPERTIES,
			{static_cast<uint8_t>(latches), static_cast<uint8_t>(latches >> 8)}, /*reliable=*/false});
	// In session the slot's entity state goes out now, ahead of the 0x0F, so the joiner reads
	// one 0x0A before its world-state load and the server tick's next after the tail
	// (D-NET-501). The slot is in state 6 already; the frame reads the body as the join
	// placed it, before the 620 below.
	// [orig: Server_OnPlayerJoin @0x51A82A..0x51A82D -> Server_SendEntityStateToPlayer
	//  @0x517BA0 (the state-6 gate @0x517BD6, sent 0x0A msgClass 0 userParam 1 @0x517CB5)]
	if (ctx.is_in_session) {
		std::vector<uint8_t> frame;
		if (Server_SendEntityStateToPlayer(ctx, world, conn, frame))
			out.push_back(InitialStateMessage{s2c::PER_FRAME_UPDATE, std::move(frame),
					/*reliable=*/false});
	}
	// [orig: Server_OnPlayerJoin @0x51A84C -> NetPacket_WriteWorldStateLoad0x0F @0x502D10,
	//  sent @0x51A869]
	out.push_back(InitialStateMessage{0x0F, serialize_world_state_load(ctx, conn, now_tick)});
	// Every joined body starts under the 620-tick spawn protection, stored after the 0x0F
	// (a bot slot +96483 stores 0; our roster has none): the per-tick arm counts it down in a
	// live MP session and holds a spectator at -1 (D-NET-217).
	// [orig: Server_OnPlayerJoin @0x51A86E..0x51A882 (620 @0x51A882, the bot's 0 @0x51A878)]
	if (body != nullptr) body->damage_state = 620;
	// The join's other slot words have no reader to port: +89940 = 0 (@0x51A896) is read only
	// by Server_SendEntityOverlayState @0x50FF60, which nothing calls; +96488 = the 0x0B's
	// client version (@0x51A951) is never read; +97540 (cleared @0x51A98A..0x51A992) is read
	// by that test alone.
	// The join notice: this slot's copy rides the bundle, every other in-game client gets
	// the same byte now and re-requests its visible-players snapshot. [orig: Server_OnPlayerJoin
	//  @0x51a93f..0x51a97a (send_mask 0x80) -> NapiNPClientMsg_HandleSpawnSlot @0x4317B0]
	SpawnSlotNotice notice;
	notice.slot = static_cast<uint8_t>(st.player_slot);
	out.push_back(InitialStateMessage{0x4D, encode_spawn_slot_notice(notice)});
	fan_spawn_slot_notice(ctx.np_protocol.connection_list, conn);
	// The join tick seed is PER PLAYER, re-rolled per connection: the client anchors
	// currentTick (and its fire freshness) to it, and the host stamps the same value as that
	// player's freshness floor. Never the session constant.
	// [orig: Server_SendRandomSeedToPlayer @0x5101a0 - value @0x5101d4
	//  ((rand() & 0xFE) + 1) << 16; join sender @0x51a982]
	out.push_back(InitialStateMessage{0x61, Server_RerollPlayerTickSeed(conn)});
	// A cease fire in effect tells the joiner so: one reliable chat line, channel 0, sender
	// slot 0xFF.
	// [orig: Server_OnPlayerJoin @0x51A998..0x51A9F3 - `cmp g_InCeaseFire` @0x51A998, the text
	//  @0x51A9A7, NetPacket_WriteTwoBytesAndCString(.., 0xFF, 0, ..) @0x51A9B9..0x51A9D6, sent
	//  0x14 msgClass 1 userParam 0 @0x51A9F3]
	if (world.rules.cease_fire) {
		ChatBroadcast cease_fire;
		cease_fire.channel = 0;
		cease_fire.sender_slot = 0xFF;
		cease_fire.text = "* * * CEASE FIRE IS IN EFFECT! * * *";
		out.push_back(InitialStateMessage{s2c::CHAT_BROADCAST, encode_chat_broadcast(cease_fire)});
	}
	// A join into a finished round (g_SpawnSuccessGate, our match outcome's `ended`) takes the
	// round end's own legs: the zero 0x61 disarming the seed just sent, this slot's 0x1D
	// header, game state 11 and slot state 7, the round-event watermark zeroed on leaving
	// state 6 (the join just entered it).
	// [orig: Server_OnPlayerJoin @0x51A9F8..0x51AA5C - `cmp g_SpawnSuccessGate` @0x51A9F8,
	//  Server_SendRandomSeedToPlayer(slot, 0) @0x51AA02, EndRoundScoreboard_SerializeHeader
	//  @0x51AA1E sent 0x1D @0x51AA3B, CNetPlayer_SetGameState(.., 11) @0x51AA44, +97544 = 0
	//  @0x51AA56, slot state 7 @0x51AA5C; the header's form pick
	//  EndRoundScoreboard_SerializeHeader @0x5052a6]
	if (world.match.outcome().ended) {
		out.push_back(InitialStateMessage{
				s2c::TICK_SEED, Server_DisarmPlayerTickSeed(conn, world.logic_tick)});
		const world::MatchResult &result = world.match.result();
		const bool non_team_header =
				world.rules.mp_session && (result.game_type & 0x10000u) == 0;
		out.push_back(InitialStateMessage{s2c::END_ROUND_HEADER,
				encode_end_round_header(
						build_end_round_header(result, static_cast<uint8_t>(st.player_slot),
								non_team_header),
						non_team_header)});
		// Leaving game state 10, then slot state 6, disarms the watermark
		// [orig: CNetPlayer_SetGameState @0x4C4135 from @0x51AA44; @0x51AA54..0x51AA56].
		conn.burst.game_state = 11;
		conn.link.round_watermark = 0;
		if (conn.slot_state != PlayerSlotState::RoundOver)
			conn.slot_state = PlayerSlotState::RoundOver;
	}
	// The NovaWorld minutes-left line (Client/STRCLI02 over the net player's +0x48, behind its
	// byte +0x4C) has no setter in JO: NapiNPPlayer_Create zeroes the byte (@0x4C79CC) and
	// this join clears it (@0x51AAC9), so the leg never sends. [orig: Server_OnPlayerJoin
	//  @0x51AA64..0x51AAC9]
	// The last of the tail: the joiner clears its map-overlay banks
	// [orig: Server_OnPlayerJoin @0x51aaee -> NapiNPClientMsg_0x03E @0x4226D0].
	out.push_back(InitialStateMessage{s2c::MAP_OVERLAY_RESET, {}});
	conn.burst.spawned = true; // the in-match latch (is_in_match) and the PeerSpawned source
	conn.phase = ConnectionPhase::Spawned;
	return out;
}

uint32_t build_server_config_flags(const NapiNPServerCtx &ctx) {
	return build_server_config_flags_impl(ctx);
}

// [orig: Server_SendInitialGameStateToPlayer @0x51bba0]
InitialStateStep Server_SendInitialGameStateToPlayer(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                     uint32_t now_tick) {
	InitialStateStep step;

	// Eligible only once the connection's pool-0 entity exists (PlayerAdded) and the World is wired.
	if (ctx.world == nullptr) return step;            // P2 unit-test path: no-op (keep-green lever)
	// The producer runs only while the mission runs: g_MissionRunning is 0 from the mission
	// load's start until its end and again from the round end (ours: no World yet, or the
	// match outcome latched), and both tracks also wait while the round-over flag is up
	// (g_SpawnSuccessGate, the same latch here). A joiner arriving in the post-round linger
	// so waits for the map change, its track where it stood. (D-NET-NEW-L1-7)
	// [orig: Server_SendInitialGameStateToPlayer @0x51BBA0 - `cmp g_MissionRunning, ebx; jz`
	//  @0x51BBA3..0x51BBA9; `cmp g_SpawnSuccessGate, ebx; jnz` @0x51BBE8..0x51BBEE (world
	//  track) / @0x51BEFF..0x51BF05 (player-sync track); Game_SetMissionRunning(0)
	//  @0x5245E3 and (1) @0x525D58 in Game_StartMission; the round end's clear
	//  Server_ProcessRoundEnd @0x5164FC]
	if (ctx.world->match.outcome().ended) return step;
	if (conn.phase < ConnectionPhase::PlayerAdded) return step;
	if (conn.burst.spawned) return step;              // already complete

	InitialStateBurst &b = conn.burst;
	const bool is_remote = (conn.type == NapiNPConnection::kTypeServerSide);
	// The player-sync track runs in slot state 2 alone, which only the joiner's C2S 0x09
	// sets (dispatch_player_sync_request) once its mission load completed; until then the
	// producer sends nothing (D-NET-500). The host's own client sends its 0x09 in-process:
	// the type-2 loopback applies the same handler here.
	// [orig: Server_SendInitialGameStateToPlayer @0x51BBA0 - the slot-state switch
	//  @0x51BBB4..0x51BBBB, the track @0x51BEFF; NapiNPServerMsg_0x009 @0x513200]
	if (!is_remote && conn.slot_state == PlayerSlotState::Added)
		dispatch_player_sync_request(conn);
	// Each pass of the track enters at subphase 8 at least: a fresh slot's 0, or the 0 the
	// last tail left [orig: @0x51BF2D..0x51BF2F].
	if (conn.slot_state == PlayerSlotState::PlayerSync &&
			b.player_sync_subphase < InitialStateBurst::kSyncMissionMapNames)
		b.player_sync_subphase = InitialStateBurst::kSyncMissionMapNames;

	// One STEP per call, and the caller calls once per open send boundary
	// (tick_connections skips a closed one): each step sends its one record, or several
	// (the track's atomic tail; one paged batch otherwise), or nothing (world case 0, the
	// terrain's closing call, an absent briefing), and the next step waits for the next
	// boundary either way. The host's own loopback has no remote client to pace: it runs
	// its steps back to back (a host-local stand-in for its in-process self-join, no
	// remote wire effect).
	// [orig: Server_TickUpdate's tail slot walk, conn+0x648 == 0 @0x51E3D6..0x51E3DD, the
	//  call @0x51E3E4; Server_SendInitialGameStateToPlayer @0x51BBA0, one case per call]
	const std::size_t page_budget = is_remote ? 1u : 0xFFFFu;

	// The backlog gate: both tracks send nothing while 20 or more of the connection's S2C
	// messages are on the wire unacknowledged (conn+0x768: every message BuildOutgoingPackets
	// packed, linked on the sent list and stamped with its packet's sequence, until the ack
	// sweep destroys it) - the client-paced backpressure that stretches a world stream over a
	// cold client's mission build. Ours is the session's retained message count, kept the same
	// way (framed, then retired by the peer's echoed ack).
	// [orig: Server_SendInitialGameStateToPlayer - `cmp [ecx+768h], 14h; jge`
	//  @0x51BBFD..0x51BC04 (world track) / @0x51BF14..0x51BF1B (player-sync track);
	//  CNapiNPConnection_BuildOutgoingPackets @0x628430 - the sent-list link and count
	//  @0x62855D..0x628584, the sequence stamp @0x6285C5; CNapiNPConnection_ParseMessages
	//  @0x625BC0 - the ack sweep @0x625D90..0x625DA2]
	if (is_remote && conn.seq.retained_outbound_message_count >= 20) return step;

	while (conn.slot_state == PlayerSlotState::PlayerSync ||
			conn.slot_state == PlayerSlotState::AwaitWorldRequest ||
			conn.slot_state == PlayerSlotState::WorldStream) {
		if (conn.slot_state == PlayerSlotState::AwaitWorldRequest) {
			// PARKED [orig: @0x51c134 leaves state 3; the emitter has no state-3 arm]: the world
			// stream starts only when the client's C2S 0x0A spawn-menu request advances 3 -> 4
			// (dispatch port of NapiNPServerMsg_HandlePlayerSpawnRequest @0x513260). No timeout —
			// a joiner that never asks never streams, exactly like retail (D-NET-145 pattern).
			// The host's own loopback has no 0x0A sender — retail's host-local client sends it
			// from the shared in-process loading loop [orig: NapiClient_WaitForGameStart
			// @0x42cc10] — so the type-2 loopback applies the handler's advance inline.
			if (is_remote) break;
			b.game_state = 9;         // [orig: CNetPlayer_SetGameState(.., 9) @0x513295]
			conn.slot_state = PlayerSlotState::WorldStream; // [orig: @0x5132b1]
			b.world_stream_phase = InitialStateBurst::kStreamInit; // [orig: @0x5132f6]
		}
		advance_burst_one_phase(ctx, conn, step, now_tick, page_budget);
		if (is_remote) break; // one step per send boundary
	}
	// The host's own client sends its C2S 0x0B in-process once its world loaded: the type-2
	// loopback runs the join here (a remote joiner's runs from its 0x0B's dispatch,
	// Server_HandlePlayerJoinRequest). The 0x0B is every session peer's, the host's own client
	// included, and the receiver is the one player-path caller of the join (the other two are
	// the bot arms behind the slot's +0x178E3 bot byte).
	// [orig: Game_StartMission @0x526189 (the is_mp_session_peer gate) -> the C2S 0x0B
	//  @0x52624B; the bot arms' calls @0x51B93A / @0x51D2E1, behind @0x51B928 / @0x51D2CF]
	if (!is_remote && conn.slot_state == PlayerSlotState::AwaitJoin) {
		for (InitialStateMessage &m : Server_OnPlayerJoin(ctx, conn, now_tick))
			step.messages.push_back(std::move(m));
		step.reached_in_game = true;
	}

	// Instrumentation (paced): log this tick's emitted tags, and a one-line summary when the burst
	// finishes, so the host log shows the world-load stream cadence directly.
	if (!step.messages.empty()) {
		std::string seq;
		seq.reserve(step.messages.size() * 8);
		for (const InitialStateMessage &m : step.messages) {
			char buf[24];
			std::snprintf(buf, sizeof(buf), "%02X(%u) ", static_cast<unsigned>(m.tag),
			              static_cast<unsigned>(m.body.size()));
			seq += buf;
		}
		opennova::io::logf(opennova::io::LogLevel::kDebug,
		"[burst] tick: %zu msg(s) | %s%s", step.messages.size(), seq.c_str(),
		             conn.slot_state == PlayerSlotState::Joined ? "(§5.2a COMPLETE)" : "");
	}

	step.advanced = true;
	return step;
}

void install_mission_location_names(NapiNPServerCtx &ctx, const world::World &world,
                                    const bms::File &mission, const mission::MissionText &text) {
	ctx.mission_location_names.clear();
	// The pool-3 load hands record k to the spawn in record order; each
	// type-2044 record the spawn admits (its slot is live: a refused record
	// returns before the type tests and leaves its slot empty) appends the
	// name its own name_index keys at the count's slot, then counts. So the
	// list is in spawn order, keyed by the record, not by the count.
	// [orig: Mission_LoadBMSFile pool-3 loop @0x40FA5B..0x40FAA4;
	//  Entity_SpawnFromBMSRecord `cmp dword ptr [edi], 7FCh` @0x40F17C /
	//  @0x40F182, entity+0x280 = g_LocationNameCount @0x40F188..0x40F18F, the
	//  copy into g_LocationNames[count] @0x40F1C4..0x40F1DF,
	//  g_LocationNameCount++ @0x40F20C; the refusals, each to the return-0
	//  epilogue @0x40EB07..0x40EB1B: the attribute gates @0x40EA94..0x40EAC2,
	//  the def attrib 0x10000 gate @0x40EAD2..0x40EB05, the team-3/4 gate
	//  @0x40EB1C..0x40EB2E]
	// Every pool's records take the leg, in the load's pool order: items (pool 1),
	// buildings (2), markers (3), organics (0) (D-NET-656). [orig: Mission_LoadBMSFile
	//  @0x40F4E0 - the spawn calls @0x40F9C6 / @0x40FA34 / @0x40FAA4 / @0x40FB19]
	std::vector<std::string> names;
	const std::pair<int, const std::vector<bms::Entity> *> sections[] = {
			{1, &mission.items}, {2, &mission.buildings}, {3, &mission.markers}, {0, &mission.organics}};
	for (const auto &[pool, records] : sections) {
		for (std::size_t slot = 0; slot < records->size(); ++slot) {
			if ((*records)[slot].type_id != def::DEF_TYPE_NAMED_LOCATION) continue;
			if (world.registry.get(world::EntityHandle::make(pool, static_cast<int>(slot))) == nullptr)
				continue;
			names.push_back(text.location_name((*records)[slot].name_index));
		}
	}
	ctx.mission_location_names = location_slot_strings(names);
}

std::vector<std::string> location_slot_strings(const std::vector<std::string> &names) {
	// Each copy runs to the name's NUL wherever that lands, so one of a slot
	// or more runs on into the next slot, whose own copy then overwrites it
	// from that slot's start; a slot read to its NUL is its first 64 bytes
	// followed by the next slot's read, and the last name stays whole.
	// [orig: Entity_SpawnFromBMSRecord: the text's copy @0x40F1D3..0x40F1DF,
	//  the key's @0x40F200..0x40F20A, neither bounded; the readers to the NUL:
	//  NetPacket_WriteWorldStateLoad0x0F's strlen @0x502EA0..0x502EAC and copy
	//  @0x502EC2..0x502ECC, NapiNPServer_HandleChatMessage's String_AppendN of
	//  g_LocationNames + 64 * index @0x5139F1..0x513A0A]
	const std::size_t slot_bytes = replication::kLocationNameSlotBytes;
	std::vector<std::string> slots(names.size());
	for (std::size_t k = names.size(); k-- > 0;) {
		if (names[k].size() < slot_bytes || k + 1 == names.size())
			slots[k] = names[k];
		else
			slots[k] = names[k].substr(0, slot_bytes) + slots[k + 1];
	}
	return slots;
}

} // namespace opennova::inmatch
