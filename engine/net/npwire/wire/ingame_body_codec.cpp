#include <net/npwire/ingame_body_codec.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/replication_model.h>
#include <net/npwire/visible_players.h>

// The body codec registry [orig: the per-tag handlers of g_NPMsgInfoClient
// @0x82AE28 (S2C) and g_NPMsgInfoServer @0x82B5D8 (C2S)]. Each entry names the
// decoder it trusts for the body's extent and the encoder that writes the same
// body from the decoded fields; the per-tag citations live at those functions.

namespace opennova {

IngameBodyCodecContext IngameBodyCodecContext::for_game_type(uint32_t game_type) {
	IngameBodyCodecContext ctx;
	// The two off-wire gates the decoders take [orig: the 0x0A objective body
	// gate g_GameType & 0x20000; the 0x0F waypoint gate
	// (g_GameType & 0xFFFDFFFF) == 0x10020] (net-re §5.9, §5.29).
	ctx.objective_gametype = (game_type & 0x20000u) != 0;
	ctx.waypoint_gametype = (game_type & ~0x20000u) == 0x10020u;
	return ctx;
}

namespace {

using Result = IngameBodyCodecResult;

// A decoder that returns true only when it read the whole body.
Result whole(bool ok, size_t len) {
	Result r;
	r.decode = ok ? BodyDecode::Exact : BodyDecode::Rejected;
	r.consumed = ok ? len : 0;
	return r;
}

// A decoder that reports how far it read.
Result counted(bool ok, size_t consumed, size_t len) {
	Result r;
	r.consumed = ok ? consumed : 0;
	r.decode = !ok ? BodyDecode::Rejected
	               : (consumed == len ? BodyDecode::Exact : BodyDecode::ShortConsume);
	return r;
}

Result with_bytes(Result r, std::vector<uint8_t> bytes) {
	if (r.decode == BodyDecode::Rejected) return r;
	r.reencoded = true;
	r.bytes = std::move(bytes);
	return r;
}

// ---- S2C, with a struct encoder ----------------------------------------

Result s_frame_update(const uint8_t *b, size_t n, const IngameBodyCodecContext &ctx) {
	FrameUpdate fu;
	const bool ok = decode_frame_update(b, n, ctx.class_of, fu, ctx.objective_gametype);
	return with_bytes(counted(ok && fu.complete, fu.consumed, n), encode_frame_update(fu));
}

Result s_organic_spawn(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	OrganicSpawnBatch v;
	const bool ok = decode_organic_spawn_batch(b, n, v);
	return with_bytes(whole(ok, n), encode_organic_spawn_batch(v));
}

Result s_pool_spawn(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	PoolSpawnBatch v;
	const bool ok = decode_pool_spawn_batch(b, n, v);
	return with_bytes(whole(ok, n), encode_pool_spawn_batch(v));
}

Result s_static_entity(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	StaticEntityBatch v;
	const bool ok = decode_static_entity_batch(b, n, v);
	return with_bytes(whole(ok, n), encode_static_entity_batch(v));
}

Result s_pool3_sync(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	Pool3SyncBatch v;
	const bool ok = decode_pool3_sync_batch(b, n, v);
	return with_bytes(whole(ok, n), encode_pool3_sync_batch(v));
}

Result s_terrain_load(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	TerrainLoadBatch v;
	const bool ok = decode_terrain_load_batch(b, n, v);
	return with_bytes(whole(ok, n), encode_terrain_load_batch(v));
}

Result s_player_list(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	PlayerList v;
	const bool ok = decode_player_list(b, n, v);
	PlayerListFrame frame;
	frame.flags = v.flags;
	for (const PlayerListRow &row : v.players) {
		PlayerListEntry e;
		e.slot = row.slot_id;
		e.status_flags = row.status_flags;
		e.score1 = row.score1;
		e.score2 = row.score2;
		e.team = static_cast<uint8_t>(row.flags >> 1);
		e.spectator = (row.flags & 1u) != 0;
		frame.players.push_back(e);
	}
	frame.team_count = v.team_count;
	frame.teams = v.teams;
	frame.in_game_count = v.in_game_count;
	frame.spectator_count = v.spectator_count;
	return with_bytes(whole(ok, n), encode_player_list(frame));
}

Result s_player_sync(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	PlayerSync v;
	const bool ok = decode_player_sync(b, n, v);
	if (v.removal)
		return with_bytes(whole(ok, n),
		                  encode_player_sync_removal(v.slot_id, v.queue_ack));
	PlayerReplicationState s;
	s.player_slot = v.slot_id;
	s.entity_handle = v.entity_slot_id;
	s.player_name = v.name;
	s.clan_tag = v.clan;
	s.account_pcid = v.id_label;
	s.team = v.team;
	s.downed_state = v.downed_state;
	s.spectator_in_game = v.field_1000;
	s.squad_leader = v.field_0040;
	s.fireteam = v.field_0080;
	s.quality = v.quality;
	s.account_squad_id = v.account_id;
	return with_bytes(whole(ok, n), encode_player_sync(s, v.field_bitmask));
}

Result weapon_reload(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	WeaponReload v;
	size_t used = 0;
	const bool ok = decode_weapon_reload(b, n, v, used);
	return with_bytes(counted(ok, used, n), encode_weapon_reload(v));
}

Result s_visible_players(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	VisiblePlayers v;
	bool clean = false;
	decode_visible_players(b, n, v, &clean);
	return with_bytes(whole(clean, n), encode_visible_players(v));
}

Result s_spawn_slot_notice(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	SpawnSlotNotice v;
	decode_spawn_slot_notice(b, n, v);
	// The handler reads one byte; a short body reads slot 0 and a longer one
	// leaves bytes unread.
	return with_bytes(counted(n >= 1, n >= 1 ? 1 : 0, n), encode_spawn_slot_notice(v));
}

Result s_batch_kill(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	BatchKillBatch v;
	const bool ok = decode_batch_kill(b, n, v);
	return with_bytes(whole(ok, n), encode_batch_kill(v));
}

Result s_weapon_loadout(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	WeaponLoadout v;
	const bool ok = decode_weapon_loadout(b, n, v);
	return with_bytes(whole(ok, n), encode_weapon_loadout(v));
}

Result s_destroy_list(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	DestroyEntityList v;
	const bool ok = decode_destroy_entity_list(b, n, v);
	return with_bytes(whole(ok, n), encode_destroy_entity_list(v));
}

// ---- C2S, with a struct encoder ----------------------------------------

Result c_auto_medic(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	AutoMedicPreference v;
	size_t used = 0;
	const bool ok = decode_auto_medic_preference(b, n, v, used);
	return with_bytes(counted(ok, used, n), encode_auto_medic_preference(v));
}

Result c_fired_round(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	ClientFiredRound v;
	size_t used = 0;
	const bool ok = decode_client_fired_round(b, n, v, used);
	return with_bytes(counted(ok, used, n), encode_client_fired_round(v));
}

// [5-B sub-header] then the sub-op's body: the type-10 extended uplink, or a
// type-11 compact record whose width the item class picks (net-re §5.10b).
Result c_entity_uplink(const uint8_t *b, size_t n, const IngameBodyCodecContext &ctx) {
	EntityPacketSubHeader hdr;
	size_t head = 0;
	if (!decode_entity_packet_sub_header(b, n, hdr, head)) return Result{};
	std::vector<uint8_t> out = encode_entity_packet_sub_header(hdr);
	const uint8_t *rest = b + head;
	const size_t rest_n = n - head;
	size_t used = 0;
	bool ok = false;
	std::vector<uint8_t> body;
	if (hdr.sub_op == ENTITY_SUB_OP_EXTENDED) {
		PlayerExtendedUplink v;
		ok = decode_player_extended_uplink(rest, rest_n, v, used);
		body = encode_player_extended_uplink(v);
	} else if (hdr.sub_op == ENTITY_SUB_OP_COMPACT && ctx.class_of) {
		switch (ctx.class_of(hdr.item_type_id)) {
		case EntityClass::Player: {
			PlayerCompactRecord v;
			ok = decode_player_compact_record(rest, rest_n, v, used);
			body = encode_player_compact_record(v);
			break;
		}
		case EntityClass::Vehicle: {
			VehicleCompactRecord v;
			ok = decode_vehicle_compact_record(rest, rest_n, v, used);
			body = encode_vehicle_compact_record(v);
			break;
		}
		case EntityClass::Infantry: {
			InfantryCompactRecord v;
			ok = decode_infantry_compact_record(rest, rest_n, v, used);
			body = encode_infantry_compact_record(v);
			break;
		}
		default:
			break;
		}
	}
	out.insert(out.end(), body.begin(), body.end());
	return with_bytes(counted(ok, head + used, n), std::move(out));
}

Result c_kill_window_request(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	BurstLoadoutRequest v;
	size_t used = 0;
	const bool ok = decode_burst_loadout_request(b, n, v, used);
	return with_bytes(counted(ok, used, n), encode_burst_loadout_request(v));
}

Result c_clan_roster_walk(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	ClanRosterWalkRequest v;
	size_t used = 0;
	const bool ok = decode_clan_roster_walk_request(b, n, v, used);
	return with_bytes(counted(ok, used, n), encode_clan_roster_walk_request(v));
}

// ---- decode-only (the host or client writes these inline) -----------------

template <typename T, bool (*Decode)(const uint8_t *, size_t, T &)>
Result whole_only(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	T v;
	return whole(Decode(b, n, v), n);
}

template <typename T, bool (*Decode)(const uint8_t *, size_t, T &, size_t &)>
Result counted_only(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	T v;
	size_t used = 0;
	const bool ok = Decode(b, n, v, used);
	return counted(ok, used, n);
}

Result s_world_state_load(const uint8_t *b, size_t n, const IngameBodyCodecContext &ctx) {
	WorldStateLoad v;
	return whole(decode_world_state_load(b, n, v, ctx.waypoint_gametype), n);
}

Result s_session_status(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	SessionStatusBlock v;
	const bool ok = decode_session_status(b, n, v);
	// The body ends in the writer's sentinel pair; bytes past it are surfaced.
	return counted(ok, n - v.trailing_bytes, n);
}

Result s_file_chunk(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	FileTransferChunk v;
	// The chunk is the rest of the body after the 12-B header.
	return whole(decode_file_transfer_chunk(b, n, v), n);
}

Result s_tick_seed(const uint8_t *b, size_t n, const IngameBodyCodecContext &) {
	uint32_t seed = 0;
	decode_tick_seed(b, n, seed);
	// The handler reads one u32 when four bytes are present.
	return counted(n >= 4, n >= 4 ? 4 : 0, n);
}

Result c_empty_body(const uint8_t *, size_t n, const IngameBodyCodecContext &) {
	// C2S 0x23 / 0x32: the host handler reads no field.
	return counted(true, 0, n);
}

constexpr const char *kNone = nullptr;

const IngameBodyCodec kCodecs[] = {
	// S2C with a struct encoder
	{'S', s2c::PER_FRAME_UPDATE, "decode_frame_update", "encode_frame_update", s_frame_update},
	{'S', s2c::ENTITY_SPAWN_BATCH, "decode_organic_spawn_batch", "encode_organic_spawn_batch", s_organic_spawn},
	{'S', s2c::POOL_SPAWN, "decode_pool_spawn_batch", "encode_pool_spawn_batch", s_pool_spawn},
	{'S', s2c::STATIC_ENTITY_BATCH, "decode_static_entity_batch", "encode_static_entity_batch", s_static_entity},
	{'S', s2c::PLAYER_LIST, "decode_player_list", "encode_player_list", s_player_list},
	{'S', s2c::POOL3_SYNC, "decode_pool3_sync_batch", "encode_pool3_sync_batch", s_pool3_sync},
	{'S', s2c::TERRAIN_LOAD, "decode_terrain_load_batch", "encode_terrain_load_batch", s_terrain_load},
	{'S', s2c::PLAYER_SYNC, "decode_player_sync", "encode_player_sync", s_player_sync},
	{'S', s2c::WEAPON_RELOAD, "decode_weapon_reload", "encode_weapon_reload", weapon_reload},
	{'S', s2c::VISIBLE_PLAYERS, "decode_visible_players", "encode_visible_players", s_visible_players},
	{'S', s2c::SPAWN_SLOT_NOTICE, "decode_spawn_slot_notice", "encode_spawn_slot_notice", s_spawn_slot_notice},
	{'S', s2c::KILL_BY_SLOT, "decode_batch_kill", "encode_batch_kill", s_batch_kill},
	{'S', s2c::WEAPON_LOADOUT, "decode_weapon_loadout", "encode_weapon_loadout", s_weapon_loadout},
	{'S', s2c::EMPTY_SLOT_SWEEP, "decode_destroy_entity_list", "encode_destroy_entity_list", s_destroy_list},
	// S2C decode-only
	{'S', s2c::JOIN_PADDING_PROBE, "decode_join_padding_probe", kNone, whole_only<JoinPaddingProbe, decode_join_padding_probe>},
	{'S', s2c::SESSION_SLOT_CONFIG, "decode_session_slot_config", kNone, whole_only<SessionSlotConfig, decode_session_slot_config>},
	{'S', s2c::SESSION_CONFIG, "decode_session_config", kNone, whole_only<SessionConfig, decode_session_config>},
	{'S', s2c::WORLD_STATE_LOAD, "decode_world_state_load", kNone, s_world_state_load},
	{'S', s2c::SPAWN_ACK_TIMESTAMP, "decode_u32_scalar", kNone, counted_only<uint32_t, decode_u32_scalar>},
	{'S', s2c::GAME_EVENT, "decode_game_event", kNone, counted_only<GameEventRecord, decode_game_event>},
	{'S', s2c::CHAT_HISTORY, "decode_chat_history_entry", kNone, counted_only<ChatHistoryEntry, decode_chat_history_entry>},
	{'S', s2c::MISSION_MAP_NAMES, "decode_mission_map_names", kNone, whole_only<MissionMapNames, decode_mission_map_names>},
	{'S', s2c::ENTITY_CHECKSUM_REQ, "decode_entity_checksum_request", kNone, counted_only<EntityChecksumRequest, decode_entity_checksum_request>},
	{'S', s2c::LOADOUT_CRC_REQ, "decode_loadout_crc_request", kNone, counted_only<LoadoutCrcRequest, decode_loadout_crc_request>},
	{'S', s2c::CAPTURE_ZONE_STATE, "decode_capture_zone_overlay", kNone, whole_only<CaptureZoneOverlayBatch, decode_capture_zone_overlay>},
	{'S', s2c::CHARATTR_DISABLED_PROPERTIES, "decode_charattr_disabled_properties", kNone, counted_only<uint16_t, decode_charattr_disabled_properties>},
	{'S', s2c::RTT_ECHO, "decode_rtt_sample", kNone, counted_only<RttSample, decode_rtt_sample>},
	{'S', s2c::SESSION_STATUS, "decode_session_status", kNone, s_session_status},
	{'S', s2c::FILE_TRANSFER_CHUNK, "decode_file_transfer_chunk", kNone, s_file_chunk},
	{'S', s2c::TICK_SEED, "decode_tick_seed", kNone, s_tick_seed},
	{'S', s2c::MISSION_DATA_CHUNK, "decode_file_transfer_chunk", kNone, s_file_chunk},
	{'S', s2c::SPAWN_WAVE_STATUS, "decode_spawn_wave_status", kNone, whole_only<SpawnWaveStatus, decode_spawn_wave_status>},
	{'S', s2c::ZONE_TIMER_VALUE, "decode_zone_timer_value", kNone, counted_only<ZoneTimerValue, decode_zone_timer_value>},
	{'S', s2c::NETWORK_QUALITY, "decode_network_quality", kNone, counted_only<uint8_t, decode_network_quality>},
	{'S', s2c::FULL_PLAYER_INFO, "decode_full_player_info", kNone, whole_only<FullPlayerInfo, decode_full_player_info>},
	{'S', s2c::SERVER_CONFIG_STRINGS, "decode_server_config_strings", kNone, whole_only<ServerConfigStrings, decode_server_config_strings>},
	// C2S with a struct encoder
	{'C', c2s::AUTO_MEDIC_PREFERENCE, "decode_auto_medic_preference", "encode_auto_medic_preference", c_auto_medic},
	{'C', c2s::FIRED_ROUND, "decode_client_fired_round", "encode_client_fired_round", c_fired_round},
	{'C', c2s::ENTITY_UPLINK, "decode_entity_packet_sub_header", "encode_entity_packet_sub_header", c_entity_uplink},
	{'C', c2s::WEAPON_RELOAD_REQUEST, "decode_weapon_reload", "encode_weapon_reload", weapon_reload},
	{'C', c2s::LOADOUT_REQUEST, "decode_burst_loadout_request", "encode_burst_loadout_request", c_kill_window_request},
	{'C', c2s::GAME_START_ACK, "decode_clan_roster_walk_request", "encode_clan_roster_walk_request", c_clan_roster_walk},
	// C2S decode-only
	{'C', c2s::CHECKSUM_REPLY, "decode_client_checksum_reply", kNone, counted_only<ClientChecksumReply, decode_client_checksum_reply>},
	{'C', c2s::PLAYER_SYNC_REQUEST, "decode_burst_player_sync_request", kNone, counted_only<BurstPlayerSyncRequest, decode_burst_player_sync_request>},
	{'C', c2s::VISIBLE_PLAYERS_REQUEST, "decode_burst_visible_request", kNone, c_empty_body},
	{'C', c2s::TEAM_SPAWN_ACK, "decode_team_spawn_ack", kNone, counted_only<TeamSpawnAck, decode_team_spawn_ack>},
	{'C', c2s::RTT_CONSUMED, "decode_rtt_sample", kNone, counted_only<RttSample, decode_rtt_sample>},
	{'C', c2s::EMPTY_SLOT_SWEEP_REQUEST, "decode_empty_slots_request", kNone, c_empty_body},
	{'C', c2s::CLIENT_QUALITY, "decode_burst_client_quality", kNone, counted_only<BurstClientQuality, decode_burst_client_quality>},
};

} // namespace

const IngameBodyCodec *ingame_body_codecs(size_t *count) {
	if (count != nullptr) *count = sizeof(kCodecs) / sizeof(kCodecs[0]);
	return kCodecs;
}

const IngameBodyCodec *lookup_ingame_body_codec(char dir, uint8_t tag) {
	for (const IngameBodyCodec &c : kCodecs)
		if (c.dir == dir && c.tag == tag) return &c;
	return nullptr;
}

} // namespace opennova
