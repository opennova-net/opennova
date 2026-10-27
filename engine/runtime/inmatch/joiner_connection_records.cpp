// JoinerConnection - process_session_admission's tag arms, by message family: each admitted game
// record of an in-order session packet runs the families in the chain's order, the first arm
// that takes it ending the walk. Split out of joiner_connection.cpp; the class header is the
// shared declaration.
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/joiner_connection_words.h> // le32_pair / le32_value
#include <cstring>
#include <formats/mission/bms.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/flat_tlv.h>         // append_flat_tlv (the C2S 0x00 JOIN body)
#include <net/npwire/lan_discovery.h>    // build_lan_discovery_probe, next_browse_connection_index, the record lines
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h> // decode_cs_config_update
#include <net/npwire/session_hello.h>
#include <runtime/inmatch/punt_log.h> // ConnectLog and the client callbacks' line texts
#include <net/npwire/session_keys.h>
#include <net/novacrypto/crc32.h>
#include <base/gameprofile/gameprofile.h>
#include <net/title/title_identity.h>
#include <base/io/le.h>
#include <base/io/log.h>
#include <base/vfs/vfs.h>
#include <runtime/inmatch/join_character_profile.h> // fresh_profile_character_vars
#include <runtime/inmatch/mission_exit.h> // ProcessDisconnectLatch (the connect start's clear)
#include <runtime/world/tick_count_clock.h> // frame_tick_count_ms64 (the connection's clock)
#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

namespace {

// C2S 0x02 echo of the post-auth join probe. Retail writes THREE leading dwords — the probe's two
// position values plus the client's net-frame counter — then random filler up to the requested
// padding_len clamped to [12, 512]: a negative request writes no filler, a request beyond 512
// clamps, and the three dwords always ship, so the echo is never empty and never exceeds one
// ordinary datagram. The retail server reads dwords 0 and 2 into its stats sampler, so the counter
// dword is consumed data, not filler. (Retail fills each pad byte with CRT rand()>>3; the CRT
// generator is a platform primitive, so the filler rides the session RNG instead.)
// [orig: NetPacket_WritePositionWithPadding @0x42A360 (clamp + three-dword prefix); caller
// NapiNPClientMsg_HandleJoinResponse @0x42e0f0 passes the @0xA822A4 net-frame counter; consumed
// by NapiNPServerMsg_0x002 @0x512fd0]
std::vector<uint8_t> build_join_padding_echo(
		const JoinPaddingProbe &probe, uint32_t net_frame_counter) {
	constexpr int32_t kRetailPaddingClampBytes = 512; // [orig: @0x42a360 clamp to 512]
	const int32_t requested = static_cast<int32_t>(probe.padding_len);
	const int32_t clamped =
			requested < 0 ? 0 : (requested > kRetailPaddingClampBytes ? kRetailPaddingClampBytes
	                                                                  : requested);
	const std::size_t body_len = clamped > 12 ? static_cast<std::size_t>(clamped) : 12u;

	std::vector<uint8_t> echo(body_len, 0);
	auto write_i32 = [&](std::size_t off, int32_t value) {
		opennova::io::write_u32_le(echo.data() + off, static_cast<uint32_t>(value));
	};
	write_i32(0, probe.pos_x);
	write_i32(4, probe.pos_y);
	write_i32(8, static_cast<int32_t>(net_frame_counter));
	for (std::size_t off = 12; off < echo.size();) {
		const uint32_t random = make_random_session_u32();
		for (unsigned byte = 0; byte < 4 && off < echo.size(); ++byte, ++off)
			echo[off] = static_cast<uint8_t>(random >> (byte * 8));
	}
	return echo;
}

} // namespace

// process_session_admission's tag arms for the join's admission and its transfers: the 0x00 JOIN
// ack, the 0x02 probe, the game-start signal, the 0x60 server-info and 0x64 mission-metadata
// chunks. False when no arm takes the record (the next family's arms test it).
bool JoinerConnection::on_join_record(const ProtocolMessage &m, RecordPass &pass) {
	PollResult &out = pass.out;
	std::vector<ProtocolMessage> &periodic_replies = pass.periodic_replies;
	if (m.tag == s2c::INIT && post_auth_stage_ == PostAuthStage::AwaitJoinAck) {
		// Golden retail frames 9-11: acknowledge S2C 0x00 with a header-only sequence, then post
		// the one-byte zero form body. An empty 0x01 is not accepted by the retail host FSM.
		out.outbound.push_back(frame_session({}));
		out.outbound.push_back(frame_inner_pending(0x01, {0x00}));
		post_auth_stage_ = PostAuthStage::AwaitPaddingProbe;
	} else if (m.tag == s2c::JOIN_PADDING_PROBE &&
	           post_auth_stage_ == PostAuthStage::AwaitPaddingProbe) {
		// Retail's response to the post-auth join probe. This sequenced reply is what causes the
		// server to send the post-handshake burst containing authoritative S2C 0x7B metadata.
		JoinPaddingProbe probe;
		if (decode_join_padding_probe(m.payload.data(), m.payload.size(), probe)) {
			// The retail builder always produces at least the three-dword prefix, so the echo
			// always ships and the stage always advances — a small or zero padding_len must not
			// wedge the FSM here.
			out.outbound.push_back(frame_inner_pending(
					0x02, build_join_padding_echo(probe, client_net_frame_counter())));
			post_auth_stage_ = PostAuthStage::AwaitGameStart;
		}
	} else if (m.tag == s2c::GAME_START_SIGNAL &&
	           post_auth_stage_ == PostAuthStage::AwaitGameStart &&
	           !m.payload.empty() && m.payload[0] != 0) {
		// Golden frame 17: the game-start flag completes verification with ONE grouped C2S
		// packet. This exchange runs before wire-header world construction; 0x48 echoes ServerAuth.MI.
		out.outbound.push_back(frame_session({
				make_protocol_message(c2s::GAME_START_ACK, std::vector<uint8_t>(4, 0)),
				make_protocol_message(
						c2s::AUTO_MEDIC_PREFERENCE,
						encode_auto_medic_preference(
								AutoMedicPreference{auto_medic_disabled_})),
				make_protocol_message(c2s::CLIENT_ACK, le32_value(conn_.connection_id)),
				make_protocol_message(c2s::PING, {}),
				// [the last 0x60 id, 0]: eight zero bytes on a first join
				// [orig: SaveFile_SendAndWaitForServerAck @0x5204B0, dword_A822C0].
				make_protocol_message(c2s::FILE_CHUNK_REQUEST,
						le32_pair(server_info_transfer_id_, 0)),
		}));
		post_auth_stage_ = PostAuthStage::AwaitServerInfo;
		out.mission_start_head = true;
	} else if (m.tag == s2c::FILE_TRANSFER_CHUNK &&
	           post_auth_stage_ == PostAuthStage::AwaitServerInfo) {
		FileTransferChunk chunk;
		if (decode_file_transfer_chunk(
				m.payload.data(), m.payload.size(), chunk)) {
			retain_server_info_chunk(chunk);
			if (!chunk.is_final()) {
				const uint32_t next_offset = static_cast<uint32_t>(
						uint64_t(chunk.chunk_offset) + chunk.chunk_size);
				out.outbound.push_back(frame_inner_pending(
						0x33, le32_pair(chunk.transfer_id, next_offset)));
			} else {
				// Golden frames 19-20 are separate packets: state re-broadcast, then the
				// initial eight-zero mission-data request.
				out.outbound.push_back(frame_inner_pending(0x47, {}));
				// [the last 0x64 id, 0] [orig: CNapiGameSession_InitRandomSeedOrRequest
				// @0x51E8F0, dword_A822C4].
				out.outbound.push_back(frame_inner_pending(
						0x37, le32_pair(mission_metadata_transfer_id_, 0)));
				post_auth_stage_ = PostAuthStage::AwaitMissionData;
			}
		}
	} else if (m.tag == s2c::MISSION_DATA_CHUNK &&
	           post_auth_stage_ == PostAuthStage::AwaitMissionData) {
		FileTransferChunk chunk;
		if (decode_file_transfer_chunk(
				m.payload.data(), m.payload.size(), chunk)) {
			retain_mission_metadata_chunk(chunk);
			if (!chunk.is_final()) {
				const uint32_t next_offset = static_cast<uint32_t>(
						uint64_t(chunk.chunk_offset) + chunk.chunk_size);
				out.outbound.push_back(frame_inner_pending(
						0x37, le32_pair(chunk.transfer_id, next_offset)));
			} else {
				// The last chunk copies the server name and the map's file out
				// of the block: on a map change, the next map's g_MapFileName
				// [orig: NapiNPClientMsg_HandleMissionDataChunk @0x4324CC (the
				//  server name, block+52), @0x4324DD (g_MapFileName, block+84),
				//  each Napi_CopyString(.., 32)].
				const auto block_string = [&](std::size_t at) {
					std::string text;
					for (std::size_t i = at; i < at + 31 && i < mission_metadata_bytes_.size() &&
							mission_metadata_bytes_[i] != 0; ++i)
						text.push_back(static_cast<char>(mission_metadata_bytes_[i]));
					return text;
				};
				if (mission_metadata_bytes_.size() >= 116) {
					server_name_ = block_string(52);
					map_file_ = block_string(84);
				}
				out.mission_started = true;
				// The mission start's joiner arm queues the empty reliable C2S 0x09 once
				// the mission-data leg and its local load ran, whatever S2C 0x16 came, and
				// waits for the 0x11 at its own send pace; a 0x16 queues only its per-row
				// 0x22 re-requests (ClientRuntime's fold), which join the 0x09 when they
				// land before that pace opens [orig: Game_StartMission @0x524360 (the
				// call @0x524dab) -> NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42CB20 (the queue
				// @0x42cb43, the send @0x42cb4d)]. A declared load (the effect system's,
				// set_mission_data_loading) holds it until the load has ended.
				if (mission_data_loading_) {
					sync_request_owed_ = true;
					mission_data_load_running_ = true;
				} else
					periodic_replies.push_back(
							make_protocol_message(c2s::CHECKSUM_RESPONSE, {}));
				enter_initial_sync_tail();
			}
		}
	} else {
		return false;
	}
	return true;
}

// process_session_admission's tag arms for the round and the mission's records: the 0x1D / 0x56
// round end, the 0x0B BMS header, the session config, the map names, the class mask, the 0x7B
// player info, the 0x45 terrain pages. False when no arm takes the record (the next family's arms
// test it).
bool JoinerConnection::on_mission_record(const ProtocolMessage &m, RecordPass &pass) {
	std::vector<ProtocolMessage> &periodic_replies = pass.periodic_replies;
	if (m.tag == s2c::END_ROUND_HEADER) {
		EndRoundHeader header;
		// A joiner is in-session by definition, so the header form is
		// the game-type bit alone. [orig: NapiNPClientMsg_0x01D form
		// pick @0x43086c..0x430883]
		if (decode_end_round_header(
				m.payload.data(), m.payload.size(),
				(game_type_ & 0x10000u) == 0, header)) {
			// A valid 0x1D immediately starts the requester-driven board stream
			// at zero. [orig: NapiNPClientMsg_0x01D @0x430840 queues
			// reliable C2S 0x2B {0}]
			periodic_replies.push_back(make_protocol_message(
					c2s::END_ROUND_STATS_REQUEST,
					encode_end_round_stats_request(0)));
		}
	} else if (m.tag == s2c::END_ROUND_STATS) {
		EndRoundStatsChunk chunk;
		if (decode_end_round_stats_chunk(
				m.payload.data(), m.payload.size(), chunk)) {
			if (!chunk.complete()) {
				const uint16_t next = static_cast<uint16_t>(
						chunk.chunk_offset + chunk.chunk.size());
				periodic_replies.push_back(make_protocol_message(
						c2s::END_ROUND_STATS_REQUEST,
						encode_end_round_stats_request(next)));
			}
		}
	} else if (m.tag == s2c::BMS_HEADER) {
		// Retail memcpy's the received 0x268-byte block verbatim. Retain the
		// exact body rather than parsing/re-encoding it: ignored/padding bytes
		// remain authoritative inputs to the later terrain/environment load.
		// Short or long bodies are not a complete header and cannot replace a
		// previously accepted one.
		// [orig: NapiNPClientMsg_HandleBMSHeader (0x0B) @0x422660]
		if (m.payload.size() == opennova::bms::kHeaderSize)
			mission_header_bytes_ = m.payload;
	} else if (m.tag == s2c::SESSION_CONFIG) {
		// Our initial-state burst carries the same g_GameType in the fixed
		// session-config block before any live frame. Retail keeps this equal
		// to the later 0x7B `extra` value.
		SessionConfig config;
		if (decode_session_config(m.payload.data(), m.payload.size(), config))
			game_type_ = static_cast<uint32_t>(config.fields[3]);
	} else if (m.tag == s2c::MISSION_MAP_NAMES) {
		// The player-sync track's first record rewrites g_MapFileName with its second
		// string, after the final 0x64 did: the value the C2S 0x0B's 17+-character side
		// password runs on into (mission_status_side_password)
		// [orig: NapiNPClientMsg_MissionMapNames @0x427e10 (an authority returns
		//  @0x427e17), the copy into g_MapFileName @0x427e6c..0x427e7f].
		MissionMapNames names;
		if (decode_mission_map_names(m.payload.data(), m.payload.size(), names))
			map_file_ = std::move(names.map_file_name);
	} else if (m.tag == s2c::CLASS_ALLOW_MASK) {
		// Retail's handler reads exactly one little-endian word and clears the
		// global to zero when the packet is too short.
		// [orig: NapiNPClientMsg_HandleClassAllowMask @0x42d540]
		class_allow_mask_ = m.payload.size() >= 2
				? static_cast<uint16_t>(m.payload[0]) |
					  (static_cast<uint16_t>(m.payload[1]) << 8)
				: 0u;
	} else if (m.tag == s2c::FULL_PLAYER_INFO) {
		// The retail post-auth source of truth for the session the joiner is about to load.
		FullPlayerInfo info;
		if (decode_full_player_info(m.payload.data(), m.payload.size(), info)) {
			server_name_ = std::move(info.server_name);
			mission_name_ = std::move(info.mission_name);
			map_file_ = std::move(info.map_file);
			game_type_ = info.extra;
			expansion_ = std::move(info.game_name);
			mission_known_ = true;
		}
	} else if (m.tag == s2c::TERRAIN_LOAD) {
		retain_terrain_load_page(m.payload);
	} else {
		return false;
	}
	return true;
}

// process_session_admission's tag arms for the world stream and the roster: the 0x0C / 0x18 spawns,
// the 0x04 slot, the spectator flags, the 0x50 team, the 0x5D sweep, the 0x46 sync, the spawn ack,
// the 0x0F world-state load, the 0x1A. False when no arm takes the record (the next family's arms
// test it).
bool JoinerConnection::on_world_record(const ProtocolMessage &m, RecordPass &pass) {
	PollResult &out = pass.out;
	std::vector<ProtocolMessage> &periodic_replies = pass.periodic_replies;
	if (m.tag == s2c::ENTITY_SPAWN_BATCH) {
		// The spawn's entity+120 is the connection ID, not entity flags. The
		// local player is a pool-0 player whose owner matches ServerAuth.MI.
		// [orig: NapiNPClientMsg_0x00C @0x42E864; Player_FindLocalPlayerEntity
		// @0x4E0090; NapiNP_GetLocalConnectionId @0x4C6D40]
		OrganicSpawnBatch batch;
		if (!decode_organic_spawn_batch(m.payload.data(), m.payload.size(), batch) &&
				batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		// Match the replica fold: complete records survive a malformed tail.
		for (const OrganicSpawnRecord &rec : batch.records) {
			// The original stops the page at the first out-of-capacity slot.
			// [orig: NapiNPClientMsg_0x00C @0x42E7CE]
			if ((rec.slot_id & 0x0FFFu) >= world::retail_pool_capacity(rec.slot_id >> 12))
				break;
			SelfSpawn spawn;
			spawn.pos_x = rec.pos_x;
			spawn.pos_y = rec.pos_y;
			spawn.pos_z = rec.pos_z;
			spawn.orientation = rec.orientation;
			spawn.team = rec.team;
			spawn.item_type_id = rec.item_type_id;
			spawn.anim_slot = rec.anim_slot;
			spawn.net_id = rec.net_id;
			apply_self_spawn(rec.slot_id, rec.has_body(), rec.owner_connection_id,
					rec.minimap_flags, spawn, out);
		}
	} else if (m.tag == s2c::FULL_ENTITY_SPAWN) {
		// A repair destroys and rebuilds this slot, even for the same type.
		// Ownership can disappear or move here without another organic page.
		// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780]
		FullEntitySpawnRecord rec;
		if (decode_full_entity_spawn(m.payload.data(), m.payload.size(), rec)) {
			SelfSpawn spawn;
			spawn.pos_x = rec.pos_x;
			spawn.pos_y = rec.pos_y;
			spawn.pos_z = rec.pos_z;
			spawn.orientation = static_cast<int32_t>(uint32_t(rec.heading_hi) << 16);
			spawn.team = rec.team;
			spawn.item_type_id = rec.item_type_id;
			spawn.anim_slot = rec.anim_slot;
			spawn.net_id = rec.net_id;
			apply_self_spawn(rec.slot_id, rec.item_type != 0, rec.entity_flags,
					rec.minimap_flags, spawn, out);
		}
	} else if (m.tag == s2c::SESSION_SLOT_CONFIG) {
		// S2C 0x04 slot assignment: the tail byte is this joiner's server-assigned team —
		// retail's g_LocalTeam, the 0x2F loadout-submit header byte 0. A retail host always
		// writes the full 24-byte body [orig: NetPacket_WriteSlotAssignment @0x502b30];
		// latch only that witnessed shape and keep the golden default otherwise.
		// [orig: NapiNPClientMsg_SessionSlotConfig (0x04) @0x425410 -> g_LocalTeam @0x425499]
		// Byte 17 is this joiner's OWN roster slot id — retail's
		// g_LocalPlayerSlotId, and the same value the host keeps at its
		// per-player record slot+20 [orig: the write side
		// NetPacket_WriteSlotAssignment @0x502b30]. It is NOT cosmetic: the client
		// packs it into the high 7 bits of every fired round's hit_part word, which
		// is how the host attributes the shot. Latching 0 makes every shot we send
		// read as SLOT 0 — the host's own — see simulation's hit_part builder.
		if (m.payload.size() >= 24) {
			local_player_slot_ = m.payload[17];
			max_player_slot_ = m.payload[18];
			assigned_team_ = m.payload[23];
		}
	} else if (m.tag == s2c::SPECTATOR_FLAGS) {
		// Despite the historical catalog name, byte 1 is the live player-slot
		// team. Retail overwrites the same g_LocalTeam latch installed by
		// 0x04, and its subsequent 0x2F copies this value verbatim.
		// [orig: NetPacket_SerializeHostEntityState (ex sub_510890) @0x510890 writes playerSlot+416;
		// NapiNPClientMsg_SetSpectatorMode @0x425A32]
		if (!m.payload.empty()) spectator_mode_ = (m.payload[0] & 1u) != 0;
		if (m.payload.size() >= 2) assigned_team_ = m.payload[1];
	} else if (m.tag == s2c::TEAM_ASSIGN) {
		// S2C 0x50 TEAM ASSIGN — the SECOND witnessed writer of the g_LocalTeam
		// team latch (the S2C 0x04 tail is the first). The host emits it for any
		// entity retargeted by Server_ChangeEntityTeam @0x518D70, capture zones
		// included. Witnessed legs, in order:
		//   (1) entity == local player -> g_LocalTeam = team @0x4319db;
		//   (2) !is_authority -> entity->Team = team @0x4319ee (any pool 0..4);
		//   (3) the player-slot team byte mirrors it @0x431a0b;
		//   (4) player (Flags & 0x100) + self -> per-side profile reselect and ONE
		//       C2S 0x2F re-submission @0x431AAD carrying the NEW team and slot 195
		//       RAW (the pre-Player_InitPlayer form, NOT g_CurrentWeaponSlot).
		// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910]
		TeamAssign assign;
		std::size_t assign_consumed = 0;
		decode_team_assign(m.payload.data(), m.payload.size(), assign, assign_consumed);
		const world::EntityHandle assigned{assign.entity_handle};
		if (assigned.valid() && assigned.pool() < world::kEntityPoolCount &&
		    static_cast<std::size_t>(assigned.slot()) <
		            world::retail_pool_capacity(assigned.pool())) {
			// Retail's header gates: not the 0xFFFF sentinel, the pool nibble
			// addressing one of the five entity pools, and a slot inside its
			// pool's table [orig: @0x431984..0x4319b3].
			out.entity_team_assigns.emplace_back(
					assign.entity_handle, assign.team);
			// Leg (1) and the local arm compare the 0x50's entity with the local
			// player entity, which only the mission start's Player_InitPlayer binds:
			// a self 0x50 ahead of the process's first mission start latches nothing
			// and re-submits nothing (the mission start's submissions read the older
			// g_LocalTeam), and one after a mission start, or a stale binding that
			// names the same slot, runs both (D-NET-572).
			// [orig: `cmp entity, g_LocalPlayerEntity` @0x4319CF, the latch @0x4319db]
			const bool local_entity = assign.entity_handle == *local_player_entity_;
			if (local_entity) assigned_team_ = assign.team;
			if (has_self_handle_ && assign.entity_handle == self_handle_) {
				// Leg (2) lands on the joiner's own dcb whether or not its
				// mission start has made it the local entity yet, so L spawns
				// on it: Player_InitPlayer reads that Team.
				// [orig: @0x4319ee; Player_InitPlayer @0x4E15F0 - @0x4E1809]
				spawn_.team = assign.team;
				out.self_team_changed = true;
				out.self_team = assign.team;
			}
			// The local arm (a player entity, Flags & 0x100, that is the local one):
			// the per-side profile reselect, PORTED -- the latched team byte picks the
			// profile side block, that block's class byte is the wire class, and the
			// page that class indexes is the kit -- and ONE C2S 0x2F with slot 195 RAW
			// (the pre-Player_InitPlayer form, NOT g_CurrentWeaponSlot). The C2S
			// 0x22/0x23 pair that follows (@0x431acb..0x431b05) is the replica fold's
			// (ClientReplicaPipeline, S2C 0x50), as is the identity pair
			// @0x431b3a..0x431b91 on the replica row (D-NET-253); L's pair and the
			// Player_InitPlayer(1) re-run @0x431b14 (same compare @0x431B0A) are
			// D-NET-570.
			// [orig: `test Flags, 100h` @0x431A15, `cmp entity, g_LocalPlayerEntity`
			//  @0x431A1B, the reselect @0x431a35..0x431a9a, the submit @0x431AAD]
			if (local_entity) {
				const LoadoutSubmit resubmit =
						build_profile_loadout_submit(assigned_team_, false);
				periodic_replies.push_back(make_protocol_message(
						0x2F, encode_loadout_submit(resubmit)));
				out.local_resubmit = true;
				out.local_resubmit_entries.push_back(out.inbound_reducer.size());
				out.local_resubmit_teams.push_back(assigned_team_);
			}
		}
	} else if (m.tag == s2c::EMPTY_SLOT_SWEEP) {
		// S2C 0x5D EMPTY-SLOT SWEEP — the reply to our C2S 0x32. Every entry is a
		// RAW pool-0 index the host considers empty; the client destroys whatever
		// it still holds there and unlinks the matching player slot. This is the
		// only channel that retires a peer's entity permanently, so without it a
		// leaver's decoded row lives on as a ghost proxy (D-NET-176).
		// [orig: NapiNPClientMsg_DestroyEntityList @0x429730 (!is_authority) ->
		//  Entity_Destroy + PlayerSlot_FindByType(idx) -> PlayerSlot_ClearAndUnlink
		//  @0x434730]
		DestroyEntityList destroy_list;
		if (decode_destroy_entity_list(
				m.payload.data(), m.payload.size(), destroy_list)) {
			for (uint16_t index : destroy_list.pool0_indices) {
				out.destroyed_pool0_slots.push_back(index);
				retire_self_handle(index);
			}
		}
	} else if (m.tag == s2c::PLAYER_SYNC) {
		// S2C 0x46 PLAYER-SYNC. Bit 15 (0x8000) is the roster REMOVAL form: no
		// entity byte follows, and PlayerSlot_ClearAndUnlink clears BOOKKEEPING
		// ONLY (active flag, names, team, entity ref). The ENTITY IS NEVER
		// DESTROYED on this leg — the entity-field wipes @0x431437 are dead code
		// there because entitySlotPtr is null. Only the 0x5D sweep retires an
		// entity (this corrects the D-NET-80 note that read the removal bit as the
		// per-entity removal channel).
		// [orig: NapiNPClientMsg_PlayerSync @0x431370 @0x431411..0x43144c]
		PlayerSync sync;
		if (decode_player_sync(m.payload.data(), m.payload.size(), sync)) {
			// The roster fold (D-HUD-24) lives in the replica pipeline, over the
			// reducer stream; the connection keeps the bookkeeping below.
			// [orig: NapiNPClientMsg_PlayerSync @0x431370]
			// Bit 0x4000 is the ACK-driven roster cursor. Retail advances
			// through EMPTY rows too and treats the 0x04 capacity byte as an
			// inclusive final slot: max=4 requests 0,1,2,3,4, then stops.
			// Every continuation uses the fixed 0x5CF7 field mask.
			// [orig: NapiNPClientMsg_PlayerSync @0x431370 tail;
			//  golden retail-to-retail f190/193/196/198/200]
			if (sync.queue_ack && sync.slot_id < max_player_slot_) {
				periodic_replies.push_back(make_protocol_message(
						c2s::PLAYER_SYNC_REQUEST,
						{static_cast<uint8_t>(sync.slot_id + 1), 0xF7, 0x5C}));
			}
		}
	} else if (m.tag == s2c::SPAWN_ACK_TIMESTAMP) {
		// This is not merely an acknowledgement: retail carries the scalar into
		// dword 0 of its later C2S 0x28 world-completion loadout request. A body
		// of any size other than four clears the retained value.
		// [orig: NapiNPClientMsg_0x019 @0x425e80 -> dword_A82360]
		spawn_ack_timestamp_ = 0;
		if (m.payload.size() == sizeof(uint32_t)) {
			std::size_t consumed = 0;
			decode_u32_scalar(m.payload.data(), m.payload.size(),
					spawn_ack_timestamp_, consumed);
		}
	} else if (m.tag == s2c::WORLD_STATE_LOAD) {
		// Retail's exact post-world completion burst, queued at the tail of
		// EVERY 0x0F handler run with no latch: a host normally sends one
		// 0x0F per admission (NetPacket_WriteWorldStateLoad0x0F's only
		// caller is Server_OnPlayerJoin), but a round-boundary re-join
		// re-runs that leg and expects the burst again. C2S 0x28 is
		// [S19 timestamp][S0F session tick][u16 0]; the remaining bodies and
		// order are fixed. Besides requesting the 0x5D empty-slot sweep, the
		// 0x22/0x23 legs make the host return the full player rows used to bind
		// the newly admitted local player.
		// [orig: NapiNPClientMsg_0x00F @0x42e5af..0x42e6ab; queues exact order
		//  0x28,0x29,0x2D,0x32,0x22,0x23; the one caller @0x51a84c]
		// The reducer folds the 0x0F's retained client globals (the
		// deploy-map overlay arm) in this same wire position
		// [orig: g_DeployScreenActive @0x42e2d8/@0x42e2f8].
		uint32_t world_state_tick = 0;
		std::size_t consumed = 0;
		decode_u32_scalar(m.payload.data(), m.payload.size(),
				world_state_tick, consumed);
		std::vector<uint8_t> loadout_request =
				le32_pair(spawn_ack_timestamp_, world_state_tick);
		loadout_request.push_back(0);
		loadout_request.push_back(0);

		periodic_replies.push_back(make_protocol_message(
				c2s::LOADOUT_REQUEST, std::move(loadout_request)));
		periodic_replies.push_back(make_protocol_message(
				c2s::TEAM_SPAWN_ACK, {0x00, 0x00}));
		periodic_replies.push_back(make_protocol_message(c2s::BURST_MEMBER_2D, {}));
		periodic_replies.push_back(make_protocol_message(
				c2s::EMPTY_SLOT_SWEEP_REQUEST, {}));
		periodic_replies.push_back(make_protocol_message(
				c2s::PLAYER_SYNC_REQUEST, {0x00, 0xF7, 0x5C}));
		periodic_replies.push_back(make_protocol_message(
				c2s::VISIBLE_PLAYERS_REQUEST, {}));
		// The handler clears g_ClientDeployHold whatever the role, so the 0x0F
		// ends the deploy wait as a 0x5A does: a spectator's 0x2F draws no 0x5A,
		// making this its release; a player's 0x5A came ahead of it (D-NET-678).
		// [orig: NapiNPClientMsg_0x00F @0x42E391, the zero @0x42E353 / @0x42E35E]
		out.gameplay_release_applied = true;
		pass.world_state_load_seen = true;
		if (post_auth_stage_ == PostAuthStage::AwaitDeployment)
			release_deployment(/*deployment_complete=*/false, out);
	} else if (m.tag == s2c::WAIT_FOR_GAME_START_ACK &&
	           post_auth_stage_ == PostAuthStage::AwaitWorldStreamEnd &&
	           !mission_start_aborted_ && (!round_over_gate_ || out.mission_started)) {
		// The world-stream terminator ends the game-start wait and releases the mission
		// start, whose end queues and pumps the loadout/status submissions
		// (ClientRuntime::push_mission_start_end) when the embedder's load ends.
		// [orig: NapiClient_WaitForGameStart @0x42cc10 (the exit test @0x42ccbf) from
		//  Game_StartMission @0x524e29]
		post_auth_stage_ = PostAuthStage::AwaitMissionStart;
		mission_start_first_loadout_taken_ = false;
		mission_start_released_this_pass_ = true;
		out.game_start_acked = true;
	} else {
		return false;
	}
	return true;
}

// process_session_admission's tag arms for the host's probes and clocks: the RTT echo, the charattr
// and checksum challenges, the loaded-model page request, the time-sync ping, the tick seed. False
// when no arm takes the record (the next family's arms test it).
bool JoinerConnection::on_control_record(const ProtocolMessage &m, RecordPass &pass) {
	PollResult &out = pass.out;
	std::vector<ProtocolMessage> &periodic_replies = pass.periodic_replies;
	if (m.tag == s2c::RTT_ECHO) {
		// While the round-over gate is up the handler returns at once: no pong for
		// the host's ping, no round trip measured for ours.
		// [orig: NapiNPClientMsg_0x057_RTT @0x432210 - `cmp g_RoundOver, 0`
		//  / `jnz locret` @0x432210..0x432217]
		RttSample sample;
		std::size_t consumed = 0;
		if (!round_over_gate_ && decode_rtt_sample(
				m.payload.data(), m.payload.size(), sample, consumed) &&
		    consumed == m.payload.size()) {
			if (sample.echo_flag != 0) {
				std::vector<uint8_t> pong = le32_value(sample.timestamp);
				pong.push_back(0);
				ProtocolMessage reply =
						make_protocol_message(c2s::RTT_CONSUMED, std::move(pong));
				// The reactive pong uses the same one-send queue parameter as the
				// per-frame RTT producer: userParam=1, not retained reliability.
				// [orig: NapiNPClientMsg_0x057_RTT @0x432210, queue @0x43226D]
				reply.reliable = false;
				periodic_replies.push_back(std::move(reply));
			} else {
				// The pong of our own C2S 0x2C ping: the completed round trip
				// lands in the ten-entry ring and the current-ping word (the
				// quality metric's ping term). Retail measures against
				// GetTickCount; our 0x2C stamps monotonic milliseconds, so
				// the same clock closes the loop.
				// [orig: @0x432280 `rtt = GetTickCount() - ts`,
				//  ring[index] = rtt @0x432282, dword_A860D4 = rtt,
				//  `++index >= 10 -> 0`]
				const uint32_t rtt_ms = monotonic_milliseconds32() - sample.timestamp;
				rtt_ring_[rtt_ring_index_] = rtt_ms;
				rtt_current_ms_ = rtt_ms;
				if (++rtt_ring_index_ >= rtt_ring_.size()) rtt_ring_index_ = 0;
			}
		}
	} else if (m.tag == s2c::CHARATTR_PROPERTY_CLEAR) {
		// The session's restriction of the boot charattr table: the property
		// zeroed in every class, then disabled. A short body defaults to
		// property 0; trailing bytes are ignored. Every row is touched before
		// the next inner message dispatches. A host sends 0, 5, 6 and 7 at a
		// join, one for each of its mp_No* words.
		// [orig: NapiNPClientMsg_CharAttrDisableProperty @0x4254C0 ->
		//  CharAttr_SetProperty @0x412890, CharAttr_SetPropertyAllowed
		//  @0x4125C0; the sender Server_SendCharAttrRestrictionsToPlayer
		//  @0x509950]
		const uint8_t property_id =
				m.payload.empty() ? uint8_t{0} : m.payload[0];
		charattr_disable_property(charattr_table_, property_id);
		++challenge_diagnostics_.property_clears;
	} else if (m.tag == s2c::CHARATTR_DISABLED_PROPERTIES) {
		// The authority's disable latches: eight of the table's fourteen set
		// from the word (a body short of two bytes reads 0), at the join and
		// with every periodic challenge.
		// [orig: NapiNPClientMsg_CharAttrDisabledProperties @0x4281A0 ->
		//  CharAttr_UnpackDisabledProperties @0x4124D0]
		uint16_t word = 0;
		std::size_t word_consumed = 0;
		if (!decode_charattr_disabled_properties(m.payload.data(), m.payload.size(), word, word_consumed))
			word = 0;
		charattr_unpack_disabled(charattr_table_, word);
	} else if (m.tag == s2c::CHARATTR_CRC_CHALLENGE) {
		// Anti-cheat character-attribute CRC challenge. The reply is one
		// 4-byte C2S 0x1C, and
		// the host DISCARDS the value: its handler calls its own
		// checksum function, throws the result away, and the message's only
		// effect is zeroing a per-player counter. That counter is what matters —
		// each unanswered challenge leaves it climbing, and a live retail host was
		// observed stopping the joiner's entire entity-record stream after eight
		// silent challenges while the transport stayed healthy.
		// The selected 124-byte source is one g_CharAttr CHARACTER row loaded
		// from charattr.def. Its embedded class must match the latest
		// authoritative 0x5A class; absent/inactive/mismatched rows return zero.
		// [orig: NapiNPClientMsg_HandleChecksumChallenge @0x42E6D0 (reply tag 0x1C,
		//  len 4 @0x42e723); CharAttr_GetClassChecksum @0x412aa0 (IDB
		//  AnimMap_GetSlotChecksum until 2026-10-12) — the `return 0` arm
		//  @0x412abf; the host's discard + counter reset
		//  NapiNPServerMsg_AnimChecksumRequest @0x501D40 @0x501d71/@0x501d79]
		uint32_t challenge_seed = 0;
		std::size_t seed_consumed = 0;
		decode_u32_scalar(m.payload.data(), m.payload.size(), challenge_seed,
				seed_consumed);
		++challenge_diagnostics_.charattr_seen;
		bool row_found = false;
		const uint32_t checksum = charattr_class_checksum(
				charattr_table_, current_player_class_, challenge_seed, &row_found);
		if (!row_found) ++challenge_diagnostics_.charattr_row_missing;
		periodic_replies.push_back(
				make_protocol_message(c2s::CHARATTR_CRC_REPLY, le32_value(checksum)));
	} else if (m.tag == s2c::ENTITY_CHECKSUM_REQ) {
		// Anti-cheat weapon-definition checksum challenge. The reply is one 5-byte
		// C2S 0x20: [u8 echoed id][u32 challenge ^ source]. Id 0xFF with a NONZERO
		// challenge short-circuits to the literal 42 without reading the table; with
		// challenge zero (the only form a stock host sends) the source is the
		// sanitized CRC of the whole loaded weapon.def table; any other id is one
		// row's CRC, 0 for a row with no name.
		// [orig: NapiNPClientMsg_HandleChecksumRequest @0x431170 (dispatch row 0x30
		//  @0x82b1a8; reply tag 0x20 @0x4311d7) -> NetPacket_WriteEntityChecksum
		//  @0x42afb0 — the 42 arm @0x42afcc, AnimDef_ComputeChecksum @0x541ef0 (the
		//  no-name arm @0x541f30), WeaponSlotDef_ComputeSanitizedChecksum @0x53f8f0]
		//
		// The table CRC comes from this peer's own loaded weapon.def, computed as
		// retail computes it (integrity_sources.h). A named row's own CRC hashes
		// process pointers and is never computable, and a table the sources could
		// not compute is unknown: both stay silent, since a guessed value can only
		// mismatch and the host punts on a difference [orig:
		// Server_ValidateWeaponCRC @0x501F70, "PUNT WCRC"] (D-NET-181; net-re
		// section 5.65).
		EntityChecksumRequest request;
		std::size_t consumed = 0;
		++challenge_diagnostics_.entity_checksum_seen;
		if (decode_entity_checksum_request(m.payload.data(), m.payload.size(),
				request, consumed) &&
		    consumed == m.payload.size()) {
			uint32_t source_crc = 0;
			bool source_known = false;
			if (request.entity_id == 0xFFu && request.checksum != 0) {
				source_crc = 42u;
				source_known = true;
			} else if (request.entity_id == 0xFFu) {
				source_crc = integrity_sources_.weapon_table_crc;
				source_known = integrity_sources_.weapon_table_known;
			} else {
				source_known = integrity_sources_.weapon_row_crc(
						request.entity_id, source_crc);
			}
			if (source_known) {
				std::vector<uint8_t> body{request.entity_id};
				const std::vector<uint8_t> value = le32_value(
						static_cast<uint32_t>(request.checksum) ^ source_crc);
				body.insert(body.end(), value.begin(), value.end());
				periodic_replies.push_back(make_protocol_message(
						c2s::ENTITY_CHECKSUM_REPLY, std::move(body)));
				++challenge_diagnostics_.entity_checksum_answered;
			}
		}
	} else if (m.tag == s2c::LOADOUT_CRC_REQ) {
		// Anti-cheat ammo-definition CRC challenge. The reply is one 9-byte C2S 0x21:
		// [u8 echoed index][u32 crc][u32 echoed key]. Retail CRCs the indexed 276-byte
		// in-memory ammo.def row with six pointer dwords zeroed and ships key ^ crc;
		// an index outside [0, capacity), read as a signed char, writes a ZERO crc
		// dword rather than key ^ 0, so the key echo is then the only
		// challenge-derived field. The rows come from this peer's own loaded
		// ammo.def (integrity_sources.h); with none computed the challenge stays
		// unanswered (D-NET-181; net-re section 5.65).
		// [orig: NapiNPClientMsg_0x031 @0x4311e0 (dispatch row 0x31 @0x82b1b8, the
		//  signed index; reply tag 0x21 @0x431245) -> NetPacket_WriteEntityCRCChecksum
		//  @0x42b020 — the range gate @0x42b038, the out-of-range arm @0x42b114, the
		//  key echo @0x42b14d]
		LoadoutCrcRequest crc_request;
		std::size_t crc_consumed = 0;
		++challenge_diagnostics_.loadout_crc_seen;
		if (decode_loadout_crc_request(m.payload.data(), m.payload.size(),
				crc_request, crc_consumed) &&
		    crc_consumed == m.payload.size()) {
			uint32_t source_crc = 0;
			const IntegritySources::AmmoRow ammo_row =
					integrity_sources_.ammo_row(crc_request.ammo_index, source_crc);
			// Retail writes literal zero out of range, not xor_key ^ 0.
			const uint32_t reply_crc = ammo_row == IntegritySources::AmmoRow::Crc
					? static_cast<uint32_t>(crc_request.xor_key) ^ source_crc
					: 0u;
			if (ammo_row != IntegritySources::AmmoRow::Silent) {
				std::vector<uint8_t> body{crc_request.ammo_index};
				const std::vector<uint8_t> crc_bytes = le32_value(reply_crc);
				const std::vector<uint8_t> key_bytes =
						le32_value(crc_request.xor_key);
				body.insert(body.end(), crc_bytes.begin(), crc_bytes.end());
				body.insert(body.end(), key_bytes.begin(), key_bytes.end());
				periodic_replies.push_back(make_protocol_message(
						c2s::CHECKSUM_REPLY, std::move(body)));
				++challenge_diagnostics_.loadout_crc_answered;
			}
		}
	} else if (m.tag == s2c::LOADED_MODEL_PAGE_REQUEST) {
		// Loaded-model index-page request. The reply is C2S 0x3D, whose body the host
		// never parses (its handler ignores data/dataLen and only stores a global
		// into the player record), so what matters is that the reply ARRIVES.
		// Retail's writer emits `[u32 startIndex]` and then up to fifty dwords
		// from the renderer's frozen, non-foliage loaded-.3DI snapshot — but
		// only while the cursor is inside its list, so a
		// start index at or past the end writes the header ALONE. The binding
		// supplies that snapshot after renderer-resource finalization; an empty
		// snapshot therefore produces the original header-only form.
		// [orig: NapiNPClientMsg_0x068 @0x42DAA0 (reply tag 0x3D @0x42dae5);
		//  NetPacket_WriteEntityIndexList @0x42d950 — header @0x42d9bd, the
		//  `current_index < count` page gate @0x42d9c3; source snapshot getter
		//  @0x5B1560, builder @0x5B3A80; the host's body-ignoring handler
		//  NapiNPServerMsg_0x03D @0x500EC0]
		uint32_t start_index = 0;
		std::size_t index_consumed = 0;
		decode_u32_scalar(m.payload.data(), m.payload.size(), start_index,
				index_consumed);
		periodic_replies.push_back(
				make_loaded_model_page_reply(start_index));
	} else if (m.tag == s2c::TIME_SYNC_PING) {
		// Time-sync / anti-speedhack ping. The client echoes the server's stamp
		// back with its OWN clock appended, and the host validates that the
		// client's reported time deltas track wall-clock within 3% — so a client
		// that never answers is a client whose clock the host cannot verify. The
		// reply is one 8-byte C2S 0x08: [u32 echoed server stamp][u32 local ms].
		// (Retail reads GetTickCount here; that is an OS primitive, so the
		// monotonic millisecond clock is the equivalent.)
		// [orig: NapiNPClientMsg_0x043 @0x42FA90 — echo @0x42faa7, local stamp
		//  @0x42fac4, CNapiNetwork_QueueReliableMessage(tag 8, len 8) (the call
		//  @0x42FAD3); the host-side validator NapiNPServerMsg_ValidateTimeSync @0x502210]
		uint32_t server_stamp = 0;
		std::size_t scalar_consumed = 0;
		decode_u32_scalar(m.payload.data(), m.payload.size(), server_stamp,
				scalar_consumed);
		const uint32_t local_stamp =
				static_cast<uint32_t>(monotonic_milliseconds_() & 0xFFFFFFFFu);
		std::vector<uint8_t> reply;
		reply.reserve(8);
		for (int shift = 0; shift < 32; shift += 8)
			reply.push_back(static_cast<uint8_t>((server_stamp >> shift) & 0xFFu));
		for (int shift = 0; shift < 32; shift += 8)
			reply.push_back(static_cast<uint8_t>((local_stamp >> shift) & 0xFFu));
		periodic_replies.push_back(
				make_protocol_message(c2s::TIME_SYNC_REPLY, std::move(reply)));
	} else if (m.tag == s2c::TICK_SEED) {
		// The per-player TICK SEED. Every C2S fired-round descriptor stamps the
		// client's network-role tick at off-0, and the host rejects a shot whose
		// tick is zero or not past the seed it stamped into our slot — so an
		// unseeded client's fire is silently discarded forever. Re-sent on join,
		// death, revive and deploy release; the four-zero-byte form disarms.
		// [orig: NapiNPClientMsg_HandleTickSeed @0x4297c0 (short body -> 0
		//  @0x4297eb); the host stamp Server_SendRandomSeedToPlayer @0x5101a0;
		//  the gate PlayerSlot_IsActive @0x4fc760 via
		//  NapiNPServerMsg_0x006_ClientFiredRound @0x51358d]
		decode_tick_seed(m.payload.data(), m.payload.size(), out.tick_seed);
		out.tick_seed_set = true;
	} else {
		return false;
	}
	return true;
}

// process_session_admission's tag arms for the deployment's records: the 0x5A grants, the 0x12
// removal, the 0x11 sync-tail marker. False when no arm takes the record (the next family's arms
// test it).
bool JoinerConnection::on_deployment_record(const ProtocolMessage &m, RecordPass &pass) {
	PollResult &out = pass.out;
	if (m.tag == s2c::WEAPON_LOADOUT && pass.valid_weapon_loadout &&
	           post_auth_stage_ == PostAuthStage::AwaitDeployment) {
		// Every valid retail 0x5A apply clears g_ClientDeployHold immediately,
		// independently from the admission the 0x0F completes.
		// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0]
		release_deployment(/*deployment_complete=*/false, out);
	} else if (m.tag == s2c::WEAPON_LOADOUT && pass.valid_weapon_loadout &&
	           post_auth_stage_ == PostAuthStage::AwaitDeployRelease) {
		// A split second grant may arrive after we have queued the deploy
		// pick. Only a packet that cumulatively ACKs the C2S 0x0E can be its
		// causal post-pick release.
		if (deployment_pick_sent_ &&
		    pass.packet_ack >= deployment_pick_sequence_) {
			release_deployment(/*deployment_complete=*/true, out);
		}
	} else if (m.tag == s2c::ENTITY_REMOVE) {
		EntityRemove removal;
		std::size_t consumed = 0;
		if (decode_entity_remove(
				m.payload.data(), m.payload.size(), removal, consumed) &&
				consumed == m.payload.size()) {
			retire_self_handle(removal.entity_handle);
		}
	} else if (m.tag == s2c::DISCONNECT_UNLOCK) {
		// S2C 0x11 is the terminal pre-world admission marker. It ends the mission start's
		// sync-tail wait; the joiner arm loads the 0x0B header's terrain and environment
		// and the avatar combo models (an embedder's declared load, set_sync_tail_loading),
		// then NapiClient_WaitForGameStart queues the empty reliable C2S 0x0A at its head and
		// pumps it; the rest of the load waits for the S2C 0x1A's mission start
		// (push_mission_start_end at its end; D-NET-NEW-L2-19).
		// [orig: NapiNPClientMsg_0x011 @0x4226E0; Game_StartMission @0x524360 - the joiner
		//  arm's NapiClient_SendPlayerSyncAndWaitForSyncTail @0x524DAB, then
		//  Game_LoadTerrainDuringConnect @0x524DF2, Avatars_LoadComboModels @0x524E24,
		//  NapiClient_WaitForGameStart @0x524E29; NapiClient_WaitForGameStart @0x42CC10 - the
		//  0x0A @0x42CC3F, the send pump @0x42CC92]
		// The host emits this marker exactly ONCE, paced behind its C2S 0x02 reply regardless
		// of our transfer progress (our host: ~13 ticks, §5.5 bundle pacing). Latch an early
		// arrival — a joiner stalled past that window mid-0x60/0x64 would otherwise drop it
		// forever and park both sides (never preload_ready here, sync state 3 host-side).
		sync_tail_seen_ = true;
		// The sync-tail wait tests the round-over gate ahead of the 0x11's flag: a gate up
		// already abandons the mission start, and no 0x0A answers (mission_start_aborted).
		// A pass that carried the final 0x64 has the mission start's clear ahead of the wait.
		// [orig: NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42CB20 - @0x42CBC5 ahead of
		//  @0x42CBE7; Game_StartMission @0x524360 - the clear @0x524A1F]
		if (round_over_gate_ && !out.mission_started &&
				post_auth_stage_ == PostAuthStage::AwaitInitialSyncTail)
			mission_start_aborted_ = true;
		// The 0x11 sets the wait's flag alone: the wait reads it after the whole pass and
		// its send, behind the round-over test, and the game-start wait that follows
		// queues the 0x0A at its head, so the 0x0A is owed to the send boundary
		// (take_world_stream_request) and a gate a later message of this pass raises still
		// withdraws it
		// (finish_receive_pump). The world stream's stage starts now. (A 0x1A in this same
		// pass cannot come from a stock host, which sends it only after the joiner's 0x0A;
		// retail's game-start wait would clear its flag at its head.)
		// [orig: NapiNPClientMsg_0x011 @0x4226E0 (the flag); NapiClient_SendPlayerSyncAnd
		//  WaitForSyncTail @0x42CB20 - the receive, the paced send, @0x42CBC5 ahead of
		//  @0x42CBE7; NapiClient_WaitForGameStart @0x42CC10 - the 0x0A @0x42CC3F, the 0x1A's
		//  flag cleared @0x42CC51 (NapiNPClientMsg_0x01A @0x425EB0 - the store @0x425ECB)]
		if (!mission_start_aborted_ &&
				post_auth_stage_ == PostAuthStage::AwaitInitialSyncTail) {
			preload_ready_ = true;
			pending_spawn_menu_request_ = true;
			sync_tail_this_pass_ = true;
			post_auth_stage_ = PostAuthStage::AwaitWorldStreamEnd;
		}
	} else {
		return false;
	}
	return true;
}
} // namespace opennova::inmatch
