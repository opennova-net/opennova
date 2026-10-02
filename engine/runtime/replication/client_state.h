#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/hud/feed_format.h>
#include <net/npwire/entity_class.h> // EntityClass
#include <net/npwire/ingame_decode.h> // EndRoundStats (the 0x56 board)
#include <net/npwire/wire_handle.h>
#include <runtime/world/parachute.h>

namespace opennova::replication {

// One connection-slot roster binding, folded from S2C 0x46 player-sync. This
// is the scoreboard's NAME-JOIN table, keyed by CONNECTION SLOT — a different
// key from the entity handle the feed resolves names by; entity_slot is the
// join between the two. The entity binding lands UNCONDITIONALLY on every
// non-removal sync (it is not bitmask-gated) [orig: the slot+36/slot+15
// stores @0x431477/@0x431480]; the named fields land per-bit last-write-wins
// [orig: the bit-gated stores in NapiNPClientMsg_PlayerSync @0x431370].
// A removal DEACTIVATES the slot and wipes it [orig: PlayerSlot_ClearAndUnlink
// @0x434730 zeroes active/team/names/entity; a re-bind re-inits every field
// @0x4346c0, and an unbound slot is never read (its 0x16 rows are dropped),
// so the clan/quality bytes ClearAndUnlink happens to skip are unobservable
// and the fold resets the whole slot].
struct ClientRosterSlot {
	bool bound = false;
	std::string name;         // 0x0001
	std::string clan;         // 0x0002 (the serializer's "team string"; retail ships "")
	uint8_t team = 0;         // 0x0004 [orig: @0x4315f7]; every accepted 0x16 row
	                          // refreshes it too [orig: @0x42fc7c]
	uint8_t downed_revive_seconds = 0; // 0x0008 / S2C 0x54 low seven bits
	bool medic_request_active = false; // 0x0008 / S2C 0x54 bit seven
	uint8_t radio_mute_flags = 0; // local preferences: bit0 voice, bit1 chat [orig: @0x430C50]
	uint8_t quality = 0;      // 0x0400, clamped 4 [orig: @0x43170d] — the connection-icon band
	int16_t entity_slot = -1; // pool-0 slot this connection drives; -1 = none
	                          // [orig: the no-entity -1 store @0x431489]
	// 0x0800: the NovaWorld account netId (slot dword 15, 0 on LAN) — the
	// clan-registry key [orig: the store @0x431736].
	uint32_t account_id = 0;
	// The slot's registry-resolved clan tag (slot+0x20, the table's 9-byte
	// "extra" buffer): the registry node's tag when the account id is nonzero
	// and a node carries it, else empty — re-resolved on every 0x800 store and
	// every 0x6A change [orig: PlayerSlot_SetName @0x4348f0 — Napi_CopyString
	// (slot+0x20, node+81, 9) @0x434935, "" @0x434909; the buffer size
	// PlayerSlotTable_Create(.., 9) @0x434bb0]. The kill feed's <ch>..<co>
	// suffix and the Tab row's label read it.
	std::string registry_clan;
	// THE COMMAND MAP'S SQUAD BYTES. The squad leader's slot (+48; 0xFF
	// none) the 0x46 0x0040 field and S2C 0x71 write, the fireteam (+49; 0
	// none, 1..3 A..C) the 0x0080 field, S2C 0x73 and a 0x71 (reset to 0)
	// write, the 0x0020 byte (+45) and the spectator byte (+46, field
	// 0x1000). The local-only squad colour index (+51, the TEAMLIST Map
	// column's (i + 1) % 14 cycle) and the PLAYERS tab's punt mark (+52,
	// the voted slot's own index, 0xFF none). A slot's first activation
	// seeds +48 = +52 = 0xFF and +45/+46/+49 = 0; the mute flags (+50,
	// radio_mute_flags) and the colour (+51) survive a removal and a
	// re-activation — nothing but the table's (re)creation clears them.
	// [orig: PlayerSlotTable_GetOrInitSlot @0x4346c0 — @0x4346f6..0x43470e;
	//  PlayerSlot_ClearAndUnlink @0x434730 leaves +48..+52;
	//  NapiNPClientMsg_PlayerSync @0x431370 — 0x20 -> +45, 0x1000 -> +46,
	//  0x40 -> +48, 0x80 -> +49]
	uint8_t squad_leader = 0xFF;
	uint8_t fireteam = 0;
	uint8_t vehicle_score = 0;
	bool spectator = false;
	uint8_t squad_color = 0;
	uint8_t punt_mark = 0xFF;
};

// One S2C 0x4C entry of the player-slot pointer table: the roster slot the
// entry points at (the table keeps the slot pointer, so every reader sees the
// slot's LIVE fields) and the entity the snapshot named, 0xFFFF where retail
// resolves a null pointer (the sentinel or a pool nibble past the five
// pools); the only reader of that entity is the map's own-slot test.
// [orig: NapiNPClientMsg_0x04C @0x428570 — {entity, slot} pairs @0x42868e /
//  @0x428695, the handle gates @0x428679..0x42868c]
struct ClientVisiblePlayer {
	uint8_t slot = 0;
	uint16_t entity_handle = 0xFFFF;
};

// One C2S 0x22 {slot, fields} + C2S 0x23 pair a receive handler queues so the
// host re-sends the slot's 0x46 row and this client's 0x4C snapshot (the
// runtime frames and clears them). [orig: NapiNPClientMsg_HandleSpawnSlot
// @0x431804..0x43183e; NapiNPClientMsg_TeamAssign @0x431acb..0x431b05]
struct ClientVisiblePlayersRefresh {
	uint8_t slot = 0;
	uint16_t fields = 0;
};

// One S2C 0x6A clan-registry node [orig: the CLinkedList node — netId +0xC,
// name +16 (char[65]), tag +81 (char[9]); CLinkedList_FindOrCreateByNetId
// @0x52B540 copies both with Napi_CopyString 65 / 9].
struct ClientClanRegistryNode {
	uint32_t net_id = 0;
	std::string name;
	std::string tag;
};

// The decoded S2C 0x16 scoreboard. Rows arrive PRE-SORTED by the server (team
// modes by the accumulated points, others by the mode stat [orig:
// Player_ComputeScore @0x500A80 feeding Server_BuildAndBroadcastScoreboard
// @0x50D960]) and the client never re-sorts — the drawer walks the records in
// wire order [orig: HUD_DrawKillList @0x423A30]; slot_id is authoritative, not
// row position. Every well-formed update applies UNCONDITIONALLY: retail
// zeroes its row count before the row loop and parses the team table and
// trailer even for a zero-row list [orig: @0x42fb46], and the drawer shows
// whatever is stored — an empty update yields an empty board. Name/clan are
// copied INTO the row at apply time (retail joins them into its 56-byte
// records inside the parser [orig: @0x42fd4c..0x42fd8f]), so a later roster
// removal does not blank rows already on the board.
struct ClientScoreboardRow {
	uint8_t slot_id = 0;
	uint16_t status_flags = 0;  // the glyph bitfield, NOT a ping [orig: store @0x42fdb4]
	int16_t score1 = 0;         // the mode's primary stat — the only score the
	                            // Tab list draws, SIGN-EXTENDED into the record
	                            // like retail's movsx [orig: @0x42fb9d; drawn
	                            // "%3i" @0x423e76]
	uint16_t score2 = 0;        // accumulated points/EXP (the server's team-mode
	                            // sort key; the retail client never reads it —
	                            // no read width witnessed, stays the raw wire u16)
	uint8_t team = 0;           // flags >> 1
	bool spectator = false;     // flags bit0
	std::string name;           // joined from the roster at apply time
	std::string clan;
	// The slot's registry clan tag copied into rec+32 with a 7-character cap
	// [orig: Napi_CopyString(rec + 32, slot+0x20, 8) @0x42fd85; "" @0x42fd8f]
	// — the board's <ch>..<co> label.
	std::string label;
};

// One team-table row. The u16 pair carries the SAME two stats as the player
// rows — the mode stat and the accumulated points [orig: the team stores
// @0x50dcb8/@0x50dce4] — and the byte pair is mode-specific: the KOTH hold
// byte (game type 0x10001 [orig: @0x50dc62]) and the CTF flag state (types
// 0x10002/0x90002/0x10004 [orig: @0x50dd30]). The old player_count/alive_count
// names were decode-era guesses.
// Both words are SIGN-EXTENDED into their dwords like the player rows'
// [orig: the movsx stores @0x42fe08 / @0x42fe19 into 0xA85AEC + 16t], and
// the header block prints them "%3i" / "%i".
struct ClientScoreboardTeam {
	int16_t score1 = 0;
	int16_t score2 = 0;
	uint8_t koth_hold = 0;
	uint8_t ctf_flag = 0;
};

struct ClientScoreboard {
	bool known = false;
	bool team_mode = false;   // flags bit0 -> g_ScoreboardFlags
	bool timed = false;       // flags bit1 — set only for solo KOTH (game type 1)
	                          // [orig: @0x50dd54]
	uint8_t in_game_count = 0;
	uint8_t spectator_count = 0;
	// Despite the IDB name g_ScoreboardDeadRowCount, this counts live,
	// nonspectating entities only, at 0x16 parse time in permanent-death mode.
	// [orig: NapiNPClientMsg_PlayerList @ 0x42FAE0, increment @ 0x42FD2A]
	int alive_player_count = 0;
	std::vector<ClientScoreboardRow> rows;
	std::vector<ClientScoreboardTeam> teams;  // T0 neutral + one per team
	// The team-table count byte — the host serializes it from its configured
	// side count, so on a joiner it IS g_NumTeamsConfig
	// [orig: g_ScoreboardTeamCount @0x42fdda <- @0x50db3a].
	uint8_t team_count = 0;
	// Rows skipped because their connection slot has no roster binding yet.
	// Retail drops these too and queues one reliable C2S 0x22 {slot, 0x1CF7}
	// re-request per dropped row [orig: @0x42fc05..0x42fc3a]: the DATA rule is
	// the drop (counted here); the slot ids wait in pending_sync_requests for
	// the runtime's send path, which frames and clears them.
	std::uint32_t rows_dropped_unknown_slot = 0;
	std::vector<uint8_t> pending_sync_requests;
	std::uint64_t revision = 0;
};

// One decoded S2C 0x0A tag-2 fire descriptor, lifted into absolute fixed-point
// coordinates. It is a remote ROUND SPAWN, not a hit/impact notification: the
// receiving client re-simulates it visually and authority remains on the host.
struct ClientRoundEvent {
	uint8_t flags = 0;
	uint8_t adm_index = 0;
	uint8_t subtype = 0;
	uint8_t slot_byte = 0;
	uint16_t shooter_handle = 0xFFFF;
	uint16_t target_handle = 0xFFFF;
	uint16_t shot_seq = 0;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
	int32_t origin_z = 0;
	int32_t dir_yaw_bam = 0;
	int32_t dir_pitch_bam = 0;
};

// One folded S2C 0x1E game event — the kill-feed lane. The wire record is
// carried through verbatim plus the classification the feed needs; string
// resolution ($A/$B against the roster, the STRCND lookup) happens where the
// name table lives [orig: NetPacket_HandleGameEvent @0x426270 ->
// HUD_FormatKillEventMessage @0x422DA0 -> Chat_FormatMessage @0x422C60].
// `kind` is hud::GameEventKind (runtime/hud/feed_format.h) carried as its
// underlying byte so this header stays free of the runtime include.
// Every ring-bound record (0x1E game events, 0x14 chat lines, 0x32 game texts)
// carries `feed_order`, the replica's dispatch stamp: retail posts each line
// the moment its message dispatches, so the HUD rings take the lines in this
// order across the three lanes (hud::order_feed_posts) [orig: the 0x1E arm
// NetPacket_HandleGameEvent @0x426270 -> Chat_AddMessageChannel2, the 0x32
// arm @0x428181..0x428195, the 0x14 arm Chat_DispatchToChannel @0x42b910 —
// each posts inside its own message handler].
struct ClientGameEvent {
	uint8_t event_type = 0;
	uint8_t attacker_index = 0xFF;
	uint8_t victim_index = 0xFF;
	uint8_t aux_index = 0xFF;
	int16_t pos_x = 0;
	int16_t pos_y = 0;
	uint8_t kind = 0;
	uint32_t feed_order = 0;
};

// One folded S2C 0x32 formatted game text — the join/leave SYSTEM-ring lane.
// The record rides verbatim (subtype, the $A text, the signed team byte of the
// join/leave pair); the Client-template pick and the $A substitution are the
// HUD's (hud/feed_format.h formatted_game_text_line), where gametext lives
// [orig: NapiNPClientMsg_0x032 @0x428060 -> Chat_FormatPlayerTokens @0x4a5190
//  -> Chat_AddMessageChannel2(line, 0xFFAFAFAF, 930) @0x428195].
struct ClientGameText {
	int8_t subtype = 0;
	std::string text;
	int8_t team = 0;
	uint32_t feed_order = 0;
};

// One folded S2C 0x14 chat line — the player-chat lane. The wire carries
// [channel][sender_slot][cstr text] (D-NET-215); the text is already the
// server-formatted "name(/squad): text" line. Which ring it lands in and which
// colour it takes is the HUD's channel table (hud/feed_format.h
// chat_channel_sink/color); the sender gate (a chat-muted slot, or a
// spectator slot while the spawn gate is down) drops the line in the fold
// [orig: NapiNPClientMsg_ChatMessage @0x42f240 -> Chat_DispatchToChannel
//  @0x42b910 — the slot gate @0x42b923..0x42b943, the switch @0x42b95d].
struct ClientChatLine {
	int8_t channel = 0;      // body[0], sign-extended into the switch
	uint8_t sender_slot = 0; // body[1], the roster index
	std::string text;
	uint32_t feed_order = 0;
};

// One command-map squad or waypoint consequence an S2C fold leaves for the
// embedding role (the world rows and the sounds, inmatch client_squad.cpp)
// and for the HUD (the lines, hud/squad_feed.h).
// [orig: NapiNPClientMsg_HandleSquadJoin @0x425600, NapiNPClientMsg_0x072
//  @0x425710, NapiNPClientMsg_0x073 @0x425770, NapiNPClientMsg_PlayerRecruited
//  @0x4258b0, NapiNPClientMsg_0x078 @0x425970, NapiNPClientMsg_0x033 @0x425fa0,
//  NapiNPClientMsg_0x07C @0x426020]
struct ClientSquadEvent {
	enum class Kind : uint8_t {
		MemberJoined,   // S2C 0x71 naming the local slot as leader
		SquadLeft,      // S2C 0x71 with leader 0xFF
		FireteamSet,    // S2C 0x73 on the local slot
		Recruited,      // S2C 0x74
		GoCode,         // S2C 0x78
		OrderSound,     // S2C 0x72 with a non-empty line
		WaypointCreate, // S2C 0x33
		EntityDestroy,  // S2C 0x7C
	};
	Kind kind = Kind::OrderSound;
	uint8_t slot = 0;         // the subject's roster slot
	int16_t entity_slot = -1; // that slot's pool-0 entity index (-1 none)
	std::string name;         // the subject's name / the waypoint's name
	uint8_t value = 0;        // the fireteam / the go code
	uint8_t mute = 0;         // the mute flags the handler reads (+50)
	int32_t x = 0;            // the shared waypoint's position
	int32_t y = 0;
	uint16_t handle = 0xFFFF; // 0x7C's packed handle / 0x33's owner index
	uint32_t feed_order = 0;  // the dispatch stamp (ClientGameEvent::feed_order)
};

// One entity as the local client has DECODED it off the wire. Per ADR 0011 the
// present pass reads THIS, not the authoritative sim directly — so single-player
// Vehicle compact flags_byte dead-pose/wreck bit — the vehicle-class analog of
// the organic dead bit 1 (a wreck keeps replicating the short frozen-pose form
// with this bit set) [orig: the §5.13 short-form select; decoder
// is_dead_pose = (flags_byte & 4)].
inline constexpr uint8_t kVehicleFlagDeadPose = 0x04;

// The carried-objective ids: the flags whose client state S2C 0x2F writes.
// [orig: NapiNPClientMsg_0x02F @0x430F19 (the 4091/4093/4095 gate)]
inline bool is_carry_objective(uint16_t item_id) {
	return item_id == 4091 || item_id == 4093 || item_id == 4095;
}

// renders exactly the state a networked peer would see.
struct ClientEntityState {
	uint16_t handle = 0;                          // (pool<<12)|slot
	uint16_t type_id = 0;
	EntityClass cls = EntityClass::Unknown;
	// Load-stream identity retained by the canonical replica fold. Presentation,
	// history, and the header-only native-world materializer share this metadata;
	// it never participates in compact record sizing. Keeping it on the decoded
	// row lets every consumer use one entity model instead of decoding spawn
	// batches twice.
	// The entity Name (entity+0xF4, world::Entity::display_name) as the spawn
	// handlers store it: 0x0C copies at most 15 characters for every organic,
	// 0x0D and 0x18 copy it whole for an AIData def (the only records whose
	// serializer writes one).
	// [orig: NapiNPClientMsg_0x00C @0x42E867..0x42E8EA; NapiNPClientMsg_0x00D
	//  @0x433320..0x43334A; NapiNPClientMsg_FullEntitySpawn @0x433D37..0x433D61]
	std::string display_name;
	uint16_t net_id = 0xFFFF;
	uint8_t spawn_tag = 0;
	int32_t x = 0;                                // world i32 16.16 (decompressed
	int32_t y = 0;                                // compact position + the frame anchor)
	int32_t z = 0;
	uint8_t yaw_byte = 0;                         // coarse heading (compact high byte)
	// Full client-side entity+0x10 heading (BAM32) — presentation reads THIS,
	// not yaw_byte. Snap mode (host/SP fold) re-seeds it from each compact
	// sample (yaw_byte << 24 / vehicle euler_z << 16); remote-motion mode
	// maintains it via the per-class chase (the mover cluster below). Body-local
	// effects can then retain sub-byte motion (notably the PRNG-signed recoil
	// half-step) without changing the wire sample.
	int32_t heading_bam = 0;
	// Full/reconstructed entity+20 pitch plus entity+24 roll. Vehicles retain the
	// last spawn/dead-pose values because live compacts omit both. Infantry pitch
	// is reconstructed here from the compact aim target using retail's one-eighth
	// chase; Player live pitch remains in pitch_byte below.
	int32_t pitch_bam = 0;
	int32_t roll_bam = 0;
	// Normalized raw bytes retained from the class-specific compact organic record.
	// carrier_handle is PlayerCompactRecord::carrier_handle for players and
	// InfantryCompactRecord::vehicle_slot_handle for infantry. For players it can
	// also name a standing-on ground entity; mount_bone distinguishes an actual
	// seat mount (retail detaches player bone 0). No new wire fields are introduced.
	uint16_t carrier_handle = 0xFFFF;
	uint8_t mount_bone = 0;                       // player vehicle_bone / infantry seat_bone_idx
	uint8_t seat_type = 0;                        // player-only seat attribute; 0 for infantry
	uint8_t pitch_byte = 0;                       // class-specific witnessed compact byte
	uint8_t aim_yaw_byte = 0;                     // infantry-only entity+720 byte
	uint8_t anim_state_id = 0;                    // player anim_state_id / infantry anim_byte
	uint8_t anim_channel_ratio = 0;               // player-only; 0 for infantry
	// A body-anim state that was applied and then OVERWRITTEN by a later record
	// before the presenter drained this row. Retail applies each record's anim
	// byte through the receive arbitration as it decodes [orig: @0x4c1153] —
	// since D-NET-209 that arbitration runs natively per folded record (the
	// net_anim_* pair below) and armed rows present the arbitrated channel;
	// the pulse remains the DISARMED-row fallback and the legacy-publish
	// rollback seam. -1 = none. The presenter consumes it first, then the
	// current state, and clears it (ClientState::clear_anim_pulses).
	int16_t anim_state_pulse = -1;
	uint8_t anim_pulse_ratio = 0;
	// The receive-side body-state ARBITRATION pair — the retail entity fields
	// the per-record apply writes AS IT DECODES [orig: player @0x4c1153,
	// infantry @0x4c0600..0x4c0641]: current (+0x2BC) and the clip-end
	// deferred pending (+0x2B8). A same-as-current record is a pure no-op
	// (the pending SURVIVES); a current in queue class 4, or class 0x20 with
	// a non-idle (flags bit0 clear) arrival, queues the record as pending;
	// everything else commits directly (current = decoded, pending = 0).
	// Retail's own pending sentinel is 0, ambiguity included. -1 current =
	// no record arbitrated yet.
	int16_t net_anim_current = -1;
	int16_t net_anim_pending = 0;
	// The remote death edge's inputs. A wire-dead record on a live row parks
	// its anim byte in deathAnimStateId (+0x2C0) and zeroes Health; the S2C
	// 0x13 parks its word there and zeroes Health too; the mover's death edge
	// then commits the parked state (or the generic 174, or 175 afloat) on the
	// next tick and latches Flags bit 2 (rm_entity_flags). An alive player
	// record raises Health again (the health-class apply), and the respawn
	// edge clears both [orig: parks @0x4c10f5 / @0x4c0509 / @0x42ebdf; Health
	// zero @0x4c10fb / @0x4c1027 / @0x4c04e1 / @0x42ebd6; the edges
	// Entity_UpdateInfantryPlayerBody @0x4b4bf1..0x4b4cdb and
	// Entity_UpdateInfantryAI @0x4b9c51..0x4b9d3e].
	int16_t net_death_anim = 0;
	bool net_health_zero = false;
	// The corpse timer (moveTimer) the death edge seeds from the def's
	// deathtime and the dead tail counts down; at 186 the def's decay effect
	// spawns, and an org1 corpse is destroyed at 0 on a session client
	// [orig: seeds @0x4b4c3e / @0x4b9c97; tails @0x4b4d63..0x4b4e5f /
	// @0x4b9e54..0x4b9f93].
	int32_t net_corpse_timer = 0;
	uint8_t net_stance_bits = 0; // retained MoveOrder bits 8/9, rebit on player receive
	uint8_t stance_sound_state = 0; // player body entity+0x304
	uint8_t radio_request = 0; // entity+885, receive event 0x6D
	uint8_t radio_request_seconds = 0; // entity+886
	// The pending promotion boundary in the growing rm_phase convention,
	// armed by the tick when a pending is present (retail: the deferral ORs
	// 0x40000 into the channel each tick and AnimChannel_AdvancePlayback
	// latches 0x20000 at the next loop wrap / one-shot end [orig:
	// @0x40b7db/@0x40b7ad; @0x40b1ae/@0x40b18f]). <0 = unarmed.
	int32_t net_anim_pending_boundary = -1;
	// The +0x377 one-shot phase seed mirror: retail stores the ratio byte
	// ONLY on the direct-commit leg [orig: @0x4c11a6], so a queued record's
	// phase never seeds the current channel. Player records only.
	uint8_t net_anim_ratio = 0;
	bool net_anim_ratio_live = false;
	// entity+0x2B0, the peer's equipped AdmDef index (the player compact's off-16
	// `anim_def_index`). Retail's client stores it straight onto the peer entity and its
	// body updater re-reads it every selection pass to pick that peer's upper-body hold
	// pose — so this byte, not any replicated anim id, is how an observer knows what a
	// remote player is holding. 0xFF = none. [orig: client store @0x4c11f2]
	uint8_t equipped_adm_index = 0xFF;
	// Runtime borrow, never a wire field; on-foot remote slots are null.
	uint16_t weapon_slot_handle = 0xFFFF;
	// BMS team (1=Blue/2=Red) from every world-stream record that carries
	// entity+354 (pool-0 0x0C, pool-1 0x0D, pool-2 0x10, pool-3 0x20).
	// A decoded flag-gated zero is assigned too; 0xFF means no team-bearing
	// record has been witnessed yet.
	uint8_t team = 0xFF;
	bool team_known = false;
	// Spawn batches gate full orientation fields. Compact Person/Vehicle records
	// always make heading known; retaining the gate keeps replay history honest
	// for spawn-only static/marker rows.
	bool heading_known = false;
	// Pool-1 0x0D zone block, retained exactly as received. The packed byte is
	// zoneNumber + 32*rank at entity+538; radius lands at entity+350. Pool-2's
	// identically placed weapon_byte/attach_ref fields land here too.
	uint8_t zone_number_rank = 0;
	uint16_t zone_radius = 0;
	// Pool-3 S2C 0x20 flag 0x02 carries the raw entity+0 Q16 dword. Marker
	// 6005/6006/2044 use it as their authored waypoint/proximity radius.
	// [orig: NapiNPClientMsg_0x020 @0x425D07..0x425D1B]
	int32_t spawn_bound_radius_q16 = 0;
	// Full load-stream entity metadata needed to construct a client-side World
	// row when the joiner loaded only the 616-byte BMS header. These are kept
	// separate from the live compact state_flags byte: the 0x0D/0x10 dword is
	// entity+36 in full, and truncating it loses Building/NoShadow/etc.
	uint32_t spawn_entity_flags = 0;
	uint32_t spawn_section_mask = 0;
	uint16_t spawn_ammo_count = 0;
	uint8_t spawn_ref_num = 0;
	uint8_t spawn_sub_type = 0;
	// The full 0x18 repair carries these fields in addition to pool-load data.
	uint32_t spawn_owner_connection_id = 0;
	uint8_t spawn_player_class = 0;
	uint8_t spawn_ai_state = 0;
	uint8_t spawn_anim_slot = 0;
	uint8_t spawn_byte_154 = 0;
	// Pool-1 0x0D's fixed retail mountHandles image. Slots 0..7 are
	// selected by spawn_mount_mask; slots 8/9 are the block's two
	// unconditional tail handles. Preserve all ten raw decoder values here:
	// materialization structurally resolves slots 0..7, while retail stores
	// slots 8/9 raw. Seat definitions remain model-owned and are deliberately
	// not inferred from this mask.
	uint8_t spawn_mount_mask = 0;
	std::array<uint16_t, 10> spawn_mount_handles{{
			0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
			0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};
	// Advances for every pool-load record applied to this exact row. Retail
	// reinitializes the complete slot even when (handle,type) is unchanged; the
	// native materializer represents each such generation as a fresh lifetime.
	uint32_t spawn_revision = 0;
	// The 0x0A off-12 movement-input byte; remote players are motor-driven from it,
	// and bits 6/7 are lean L/R. [orig: pack @0x4df68f; write @0x4c0c9c]
	uint8_t move_input = 0;
	// Locally integrated lean angle (BAM32): only the bits ride the wire — both ends
	// integrate the angle per body tick (decay then ramp, equilibrium ~±0x30000000).
	// [orig: decay lean -= (lean+8)>>4 @0x4b5c97, then the ramp @0x4b7dbf/@0x4b7dd6
	//  (∓0x3000000/tick)]
	int32_t lean_angle = 0;
	// The remote ARMS DIP. Retail's reload broadcast does NOT put a peer into the
	// 65/66 reload pose on a pure client: the 0x49 handler branches on the entity's
	// item type and, for a remote PERSON, stamps the dip window and returns without
	// ever touching the +0x372 reload window that the pose selector reads. So an
	// observer's whole feedback for "that player reloaded" is this dip.
	// [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42c0a0 — the remote-person branch
	//  @0x42c105/@0x42c109 stamps entity+0x371 = 80 @0x42c10b and returns @0x42c113;
	//  the pose window entity+0x372 is written only by WeaponSlot_ReloadAmmo @0x54173c]
	int32_t arms_dip_ticks = 0;
	// The term the dip drives. entity+0x36C carried the IDB name headLookDecay, which is
	// a misnomer — it is not a head-look term at all. It is a PITCH OFFSET accumulator
	// that lands directly in the entity's own Pitch, and from there in every overlay
	// matrix and the held-weapon attach basis. Verified at the consumer:
	// `Pitch = savedPitch + [0x36C] + 2*[0x380]`
	// [orig: @0x4b1bd4..0x4b1bf5, sibling reads @0x4b1c1b / @0x4b1d96]. Renamed to
	// pitchKickAccum in the IDB and pitch_kick_accum across the port on 2026-07-27.
	// [orig: entity+0x36C, driven @0x4b5cb7, eased @0x4b5cc7..0x4b5cd5]
	int32_t pitch_kick_accum = 0;
	// Remote recoil accumulator (entity+0x380). Round receive applies the
	// stance-indexed ammo impulse after calculating that shot's spread; the
	// client body pass decays it later in the same frame.
	int32_t recoil_pitch = 0;
	// Client-owned +0x322/+0x324 gun channel, advanced by the joiner tick.
	int16_t emplaced_gun_yaw_word = 0;
	int16_t emplaced_gun_pitch_word = 0;
	uint16_t emplaced_spin_phase = 0; // local ewep class update; not a wire field
	bool emplaced_controls_valid = false;
	// The joiner's form of this carrier brain's turret words (the +0x1D8 yaw /
	// +0x1DC pitch high words): a joiner runs no brains, so an ewep child's
	// class update publishes its gun words here for the carrier's render
	// callback. Not a wire field. [orig: Entity_UpdateTransformAndTurret
	//  @0x440F70..0x441020; HUD_CacheEntityDebugStats @0x449ECF..0x449EE2]
	int16_t carried_gun_yaw_word = 0;
	int16_t carried_gun_pitch_word = 0;
	bool carried_gun_words_valid = false;
	// Pool-1 0x0D entity+368 relationship. The spawn positions are absolute;
	// ClientReplicaPipeline captures this row's rigid carrier-local pose after the
	// whole batch is present, then recomposes it from the followed carrier's live
	// pose each tick. This is the retail path for NoNetworkCallback addeweap
	// children; the followed carrier is target_handle (groundEntity) when set,
	// else this parent (non-pool-0 only, D-NET-195). The parent_local_* cluster
	// below stores the captured pose for whichever carrier is followed.
	uint16_t parent_handle = 0xFFFF;
	int32_t parent_local_x = 0;
	int32_t parent_local_y = 0;
	int32_t parent_local_z = 0;
	// Full wrapped heading delta captured from the absolute 0x0D poses. The
	// persistent no-callback attachment follows the parent's LIVE heading_bam
	// between compact records; yaw_byte remains the coarse presentation mirror.
	int32_t parent_local_heading_bam = 0;
	int32_t parent_local_pitch_bam = 0;
	int32_t parent_local_roll_bam = 0;
	bool parent_pose_valid = false;
	// The 0x0D record's separate TARGET relationship: the STRUCTURAL carrier a
	// pool-1 mounted child rides (retail groundEntity, entity+40) — an occupied
	// boat gun's target is the DRIVING hull while parent_handle above carries
	// the occupant/driver back-ref (+368). Vehicle compacts re-land the same
	// +40 slot per record; this is its 0x0D seed. For a compact-less
	// (NoNetworkCallback) child this slot also drives the LIVE per-tick follow:
	// retail's 'ewep' move function recomposes the child from groundEntity
	// every tick, so the gun tracks a DRIVING carrier between/without records
	// (refresh_carried_entities).
	// [orig: NapiNPClientMsg_0x00D @0x432C40 — target → groundEntity
	//  resolve @0x4332bc, store @0x4332d7; parent → occupantEntity (+368) store @0x433289;
	//  'ewep' move fn Entity_UpdateTransformAndTurret @0x440ca0 via the class
	//  table row @0x82abe0]
	uint16_t target_handle = 0xFFFF;
	// The S2C 0x2F states a flag row took. A client writes a flag's pose,
	// flags byte and carry links from a new state only: its own drop, fall
	// and ride move the flag between states (ClientWorldMaterializer). The
	// destroy of the flag's person carrier makes a state too.
	// [orig: NapiNPClientMsg_0x02F @0x430E10, the one client writer of a
	//  flag's pose]
	uint32_t objective_state_serial = 0;
	// The dying carrier's last pose, taken when the client destroys the row
	// of the person carrying this flag: Entity_Destroy drops the carried
	// object off it before its fields are wiped. Valid for the state that
	// destroy made (objective_drop_serial == objective_state_serial).
	// [orig: Entity_Destroy @0x43E8B1..0x43E8B8 -> Entity_DropCarriedObject]
	int32_t objective_drop_x = 0;
	int32_t objective_drop_y = 0;
	int32_t objective_drop_z = 0;
	int32_t objective_drop_heading_bam = 0;
	int32_t objective_drop_pitch_bam = 0;
	uint32_t objective_drop_serial = 0;
	uint16_t fire_target_handle = 0xFFFF; // shooter AI lock from the tag-2 fire descriptor
	// The pure-client stale-carrier sweep's run length: consecutive mover
	// ticks this no-callback child's persistent carrier stayed unresolvable.
	// At 128 the sweep queues C2S 0x0F for the carrier AND the child, then
	// locally destroys the child pending the authority re-spawn — retail keys
	// the same 128-tick cadence on `tick & 0x7F == 127` with the carrier's
	// staleness marker; the per-child run is this transport layer's
	// structural translation of that pair (our unresolvable-carrier state IS
	// the staleness the marker tracks).
	// [orig: Entity_UpdateTransformAndTurret @0x440ca0, the sweep
	//  @0x440d41..0x440e2f]
	int32_t carrier_missing_ticks = 0;
	// Replica-row vertical velocity — the caller-owned gravity channel of the
	// org movers, integrated before the contact resolve and zeroed on landing
	// [orig: org2 vel_z -= 208 then pos += vel @0x4B7CE0..0x4B7CEF; org1
	//  vel_z -= 416 then pos += 2*vel; landing zero in the shared tail].
	int32_t rm_vel_z = 0;
    world::ParachuteState rm_parachute;
    uint32_t rm_chute_carry_flags = 0;
	// The planar velocity pair (retail +0x98/+0x9C) — the momentum channel the
	// org movers maintain beside the anim root: per-tick decay (in-air 63/64
	// with the optional MoveOrder-bit3 air-steer nudge and the root pair
	// ZEROED; grounded/org1 (7v+4)>>3 with the |v|<=8 snap), integrated as
	// pos += vel + root, fed by the org2 ledge 3/4 momentum carry and drained
	// by the water drags [orig: maintenance @0x4b78a8..0x4b79dc /
	// @0x4bf5cb..0x4bf61f; integrate @0x4b7cbf..0x4b7cd2 / @0x4bf684..;
	// carry @0x4b7e43..0x4b7e6d; drags @0x4b8124..0x4b8149].
	int32_t rm_vel_xy[2] = {};
	// The contact resolver's ground-probe hit for this row (wire handle;
	// 0xFFFF = terrain/none) — retail's groundEntity (+0x28) store
	// [orig: Entity_RaycastGroundHeightAndObject @0x414370]. The deck-ride
	// consumes it: the row follows the hit entity's per-tick pose delta.
	uint16_t resolved_ground = 0xFFFF;
	// The row's persistent retail entity-Flags mirror — the caller-owned word
	// the resolver's latch sites and the mover flag channels read and write on
	// a registry entity (InAir 0x2000, Drowning/float 0x8000, LadderContact
	// 0x100000, Submerged 0x200000, Indoors 0x800000). Set/cleared by the
	// resolve, the airborne/landing edges, and the water block; consumed by
	// the root suppressions, the gravity gate, the probe's indoors skip, and
	// the resolver's full-update discriminant.
	uint32_t rm_entity_flags = 0;
	// Raw entity flags from the latest compact organic record: PlayerCompactRecord::
	// state_flags or InfantryCompactRecord::flags_byte. Bit 0 is hidden and bit 1
	// is dead/undeployed. Spawns carry no compact flags, so `state_flags_known`
	// distinguishes an unwitnessed zero from a witnessed alive sample.
	uint8_t state_flags = 0;
	bool state_flags_known = false;
	// Player compact health/class byte, retained for remote seat health bands.
	// [orig: Entity_SetHealthFromDifficultyByte @0x4AD580]
	uint8_t health_class_byte = 0;
	// Last explicit vehicle compact health sample. `health_known` prevents a
	// load-only row from treating its default zero as death.
	uint16_t health_word = 0;
	bool health_known = false;
	// Advances on each witnessed dead -> alive edge. Keeping the epoch in the
	// decoded view preserves a respawn even if several 0x0A frames are folded by
	// one client pump before presentation runs.
	std::uint32_t respawn_revision = 0;
	bool seen_this_frame = false;

	// --- The client-side between-update mover cluster (net-re §5.38e, D-NET-196).
	// On a JOINER (remote-motion mode), every compact read STAGES the target here
	// and ClientReplicaPipeline::tick_remote_motion chases the live pose (x/y/z above =
	// retail entity+4/+8/+0xC; heading_bam above = entity+0x10) one step per
	// 62.5 Hz tick. On the host/SP roles the fold keeps writing the live pose
	// directly (full-rate loopback; the authority never interpolates, D-NET-89).
	// Staged wire target; the per-class chase overwrites it with the per-step
	// vector at staging time, exactly like retail [orig: entity+0x234/238/23C,
	// step re-store @0x4b9b2e (org1) / @0x4B459F (org2) / watercraft @0x48D480].
	int32_t net_smooth_target[3] = {};
	// Recaptured from the live pose at the top of every mover tick
	// [orig: entity+0x80/84/88 recapture @0x4b9a5f and each family head].
	int32_t net_saved_live_pose[3] = {};
	// Staged heading/pitch targets, mutated into per-step deltas at staging
	// [orig: entity+0x240/+0x244].
	int32_t net_smooth_heading = 0;
	int32_t net_smooth_pitch = 0;
	// org1 only: the PREVIOUS staged heading, promoted on each fold before the
	// new store — the AI-infantry heading chase runs one record behind the wire
	// [orig: entity+0x1A8 promote in the mode-2 read @0x4C0320].
	int32_t net_target_heading_bam = 0;
	// Chase bookkeeping [orig: entity+0x27C / entity+0x27E].
	int16_t net_interp_progress = 0;
	int16_t net_interp_steps = 0;
	// Vehicle speed register mirror (retail vehicleData[177]) — the DECOMPRESSED
	// 16.16 wire value [orig: the mode-2 read decompresses weaponAimY before the
	// store]. Gates the fast-vehicle snap threshold (>= 293 ~ 0.28 m/s ->
	// 0x60000) and decays on starvation [orig: @0x48D480 interp block]; also the
	// prediction leg's commanded-speed source ([136] = [177] mirror).
	int32_t vehicle_speed_reg = 0;
	// Vehicle steer register mirror (retail vehicleData[179], the read-dest of
	// the wire weapon_heading_bam whose write source is the host's steer target
	// [132] — §5.13/D-NET-63). The non-driver machine adopts it as its own
	// steer-command register ([132] = [179]) for the prediction leg
	// [orig: @0x48D480 interp tail mirror].
	int32_t vehicle_steer_bam = 0;
	// Air lateral command mirror (retail vehicleData[178], the read-dest of the
	// wire weapon_aim_z, decompressed) — the CHel/cpln prediction leg's second
	// axis [orig: the three-register air mirror @0x490C9E].
	int32_t vehicle_lat_reg = 0;
	// entity+0xA0 slideDecay mirror — the vertical velocity (16.16 u/tick) the
	// family prediction integrates. Every compact whose wire flags clear bit 0x02
	// re-lands it (the dead-pose form lands 0); `pending` marks a fresh landing
	// for the joiner's world mover to adopt as its slide_z
	// [orig: the mode-2 read @0x460910..0x46091e; the short form's 0 @0x460684].
	int32_t vehicle_vertical_velocity = 0;
	bool vehicle_vertical_velocity_pending = false;
	// Armed by the first folded compact for this row: the chase never runs
	// toward a zero-initialized target on rows that only ever saw load-stream
	// spawns (pool-2/3 statics).
	bool net_has_compact = false;
	// Carried rows (carrier_handle set): the latest record's seat-local offset,
	// re-composed against the carrier's CURRENT chased pose every mover tick —
	// the row-level translation of retail rendering mounted riders through the
	// carrier attach each frame (the rider's own mover is bit0-skipped)
	// [orig: Entity_AttachCarriedObject bit0 set @0x43C14A; the D-NET-67 lift]. A
	// record with carrier 0xFFFF clears it (per-record consumption, D-NET-195).
	// A world-mover VEHICLE row is only a seat-follow when its carrier is a
	// pool-1 deck; on a static carrier (pool 2/3) the fold composes the record
	// once and the sim predicts from that sample (no per-tick recompose).
	// Set by the embedding sim when a WORLD-side family mover owns this row's
	// motion (the joiner's pool-1 prediction, §5.38e B-facet): the fold live-
	// snaps the wire sample, tick_remote_motion skips the row, and the sim
	// mirrors the predicted world pose back after each tick.
	bool net_world_mover = false;
	// Air-family rows use the AIR chase constant set (snap 0xA0000, deadband
	// 0x2AAA, buckets {8,10,15,20,25,32}) when no world mover predicts them.
	// Stamped by the embedding sim from the resolved vehicle family — replication
	// itself resolves only the wire class, never the motion family.
	bool net_air_family = false;
	// Bumped once per folded compact record for this row — the sim's staging
	// edge detector (a fresh wire sample arrived since it last staged).
	uint32_t compact_revision = 0;
	// --- The anim-root-motion dead-reckoning cluster (net-re §5.38e; retail's
	// remote body runs the SAME AnimMap primary channel and integrates its root
	// delta — chase early, root late, additive on the same fields [orig: anim
	// update @0x4B41DF; root integration @0x4B7CB4..0x4B7CEF; org1 twin
	// @0x4BF684..0x4BF6A2]. The channel state machine mirrors
	// world::InfantryState's body channel (begin_body_transition /
	// advance_primary_channel) through the shared IRootMotionSource API.
	int16_t rm_adm_id = -2;       // -2 unresolved (embedder stamps), -1 none
	int16_t rm_state = -1;        // playing/target state (the entity+700 mirror)
	int16_t rm_prev_state = -1;   // blend primary [orig: AnimMap prev channel]
	int32_t rm_phase = 0;         // target playhead, half-frame ticks
	int32_t rm_prev_phase = 0;
	float rm_blend_weight = 1.0f; // += 0.1/tick, or 1/15 when the target state
	float rm_blend_step = 0.0f;   // carries flag 0x400 [orig: @0x410640]
	// org2 leg-chain state: the LEGS chase the wire yaw (quarter-step clamp
	// ±0x3000000, twist ±0x30000000, staggered idle re-plant windows) and the
	// body heading is their midpoint — the root delta rotates by THIS, not the
	// view yaw [orig: @0x4b4945..0x4b4ac1; midpoint @0x4b4ab5]. org1 rows
	// rotate by heading_bam (retail pins body == render heading for org1).
	// The witnessed vertical: dz is REPLACED by the capsule-bottom history
	// delta while the prev-bottom slot is live; climbs 32..35 and grenade
	// deaths 176..179 reset the slot every update [orig: AnimMap_UpdateEntity
	// state tests/clear @0x40B607..0x40B637 and bottom delta/store
	// @0x40B88E..0x40B8A0]. The player
	// compact's phase byte seeds ONE transition then dies (retail zeroes
	// entity+0x377 after use [orig: AnimMap_UpdateEntity @0x40B7E4]).
	int32_t rm_prev_bottom = 0;
	bool rm_prev_bottom_live = false;
	int32_t rm_leg_yaw[2] = {};
	int32_t rm_leg_target[2] = {};
	int32_t rm_body_heading = 0;
	bool rm_leg_seeded = false;
	// --- The secondary (upper-body weapon) AnimMap channel, advanced locally
	// for every armed player row, as retail's client runs it for every player
	// body: the wire carries no secondary state. The 16-tick selection with
	// its locked / emote commit (the hold ladder off the wire ADM index and
	// Flags byte), the clip-end deferred promotion, the playhead and the blend,
	// on the replica's variant-0 track (D-NET-196). The S2C 0x2D emote stamps
	// the target. wpn_state < 0 = unarmed (no root-motion source).
	// [orig: Entity_UpdateInfantryPlayerBody @0x4b5d71..0x4b5ea3 (selection +
	//  commit); AnimMap_UpdateDualChannels @0x40b8c0 (advance);
	//  NapiNPClientMsg_HandleEmote @0x427f12..0x427f18 (the stamp)]
	int16_t wpn_state = -1;      // entity+0x2C8, the target
	int16_t wpn_deferred = 0;    // entity+0x2C4
	int16_t wpn_playing = -1;    // the channel's playing state
	int16_t wpn_prev = -1;       // the blend source
	int32_t wpn_phase = 0;
	int32_t wpn_prev_phase = 0;
	int32_t wpn_deferred_boundary = -1;
	float wpn_blend_weight = 1.0f;
	float wpn_blend_step = 0.0f;
	int32_t net_seat_local[3] = {};
	// The record's carrier-LOCAL heading (BAM32): the player/infantry yaw byte
	// widened (<< 24), the vehicle euler_z high half (<< 16). Every carried class
	// composes world = carrier + local on each recompose [orig: the player read
	// @0x4c10d4; the vehicle read's Entity_TransformLocalToWorld @0x4608ce,
	// out[3] = carrier[3] + local[3] @0x43bd00, then the entity+576 store @0x4607f5].
	int32_t net_seat_local_heading_bam = 0;
	bool net_seat_valid = false;
};

// The carrier a compact-less (no-callback) child's pose follows: its 0x0D
// TARGET (groundEntity, +0x28) when streamed, else a parent outside pool 0.
// A pool-0 parent is the occupantEntity (+0x170) back-reference of a gunner
// or driver, never a transform parent (D-NET-195). One rule for the ClientState
// recompose, the world materializer, the joiner mirror and the present rows.
// [orig: NapiNPClientMsg_0x00D occupantEntity store @0x433289, groundEntity
//  store @0x4332D7; the ewep move fn Entity_UpdateTransformAndTurret reads
//  groundEntity @0x440CBF]
inline uint16_t persistent_carrier_handle(const ClientEntityState &row) {
	if (row.target_handle != wire_handle::kInvalid) return row.target_handle;
	if (row.parent_handle == wire_handle::kInvalid ||
			wire_handle::pool(row.parent_handle) == wire_handle::kPoolOrganic)
		return wire_handle::kInvalid;
	return row.parent_handle;
}

// Latest environment sample carried by the S2C 0x0A header. The revision is an
// ordered-message edge for optional history consumers; the live simulation may
// simply read the latest value.
struct ClientEnvironmentState {
	// Phase-2 fields previously decoded and then discarded. Keep the complete
	// retail snapshot so a joined OpenNova client can project the host's weather.
	uint8_t rain_pct = 0;
	uint8_t env_param = 0;
	bool present = false;
	uint16_t fog_dist = 0;
	uint16_t fog_accel = 0;
	uint16_t tod_fixed = 0;
	uint8_t quake_ticks = 0;
	uint8_t cloud_scroll = 0;
	uint8_t overcast = 0;
	std::uint32_t revision = 0;
};

// Latest complete recipient-scoped phase-8 mounted-ammo snapshot. The World
// binding consumes revisions once so local weapon actions between phase-8
// samples are not repeatedly reset by the same network value.
struct ClientMountedAmmoState {
	bool present = false;
	uint16_t mount_handle = 0xFFFF;
	bool has_mount = false;
	uint16_t clip = 0;
	uint16_t reserve = 0;
	std::uint32_t revision = 0;
};

// Client-retained map-overlay banks. The capacities and routing bits are the
// original fixed tables; keeping them bounded makes refresh, clear, and expiry
// behavior independent from presentation.
// [orig: MapOverlay_UpdateOrCreateSlot @0x5BEA60; MapOverlay_AllocSlot @0x5BE970;
//  MapOverlay_UpdateTimers @0x5BFCE0]
inline constexpr std::size_t kMinimapTransientCapacity = 328;
inline constexpr std::size_t kMinimapPersistentCapacity = 328;
inline constexpr std::size_t kMinimapSpecialCapacity = 504;
inline constexpr std::size_t kMinimapLinkedCapacity = 251;
inline constexpr uint16_t kMinimapOverlayLifetimeTicks = 1984;

struct ClientMinimapOverlaySlot {
	bool active = false;
	uint16_t handle = 0xFFFF;
	uint8_t param = 0;      // icon byte (wire +2 on 0x40; 253/24 on 0x6B)
	uint8_t icon_color = 0; // wire color-table index (+3 on 0x40)
	uint8_t flags = 0;      // 0x10 persistent, 0x20 clear, 0x40 special, 0x6B writes 0xC4
	uint8_t source = 0;
	uint32_t argb = 0xFFFFFFFFu;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;          // 0x6B slots: the ring height (16.16)
	int32_t heading_bam = 0;
	uint16_t remaining_ticks = 0;
	// Regular (non-special) markers draw from the live decoded entity; this
	// mirrors retail's entity[538] draw gate and is refreshed each tick.
	// [orig: Render_MinimapSlotBlip @0x5be4b8]
	bool entity_known = false;
};

// One 0x6B keep-alive link: while it lives it refreshes its special slot's
// lifetime and handle each tick; its expiry clears the slot.
// [orig: linked table @0x28E1B28, MapOverlay_UpdateTimers @0x5bfd3a..]
struct ClientMinimapLinkedSlot {
	// The nearest-designation query reads the retained link, even if its
	// special map slot could not be allocated. [orig: @0x5BEC95..0x5BECB5]
	int32_t x = 0, y = 0, radius_q16 = 0;
	uint8_t type = 0;
	bool active = false;
	uint16_t handle = 0xFFFF;
	uint32_t remaining_ticks = 0; // wire seconds x62 [orig: @0x4255c9..0x4255d6]
	// The link's STORED special-bank slot (retail keeps a raw slot pointer at
	// link+24 and consults ONLY it — never a handle search — so a coexisting
	// 0x40 special badge for the same handle keeps its own slot). -1 = none
	// (allocation failed while the special bank was full).
	// [orig: overlay_obj = link[6] @0x5bece4; fresh-link alloc @0x5bed39]
	int16_t slot_index = -1;
};

struct ClientMinimapState {
	std::array<ClientMinimapOverlaySlot, kMinimapTransientCapacity> transient{};
	std::array<ClientMinimapOverlaySlot, kMinimapPersistentCapacity> persistent{};
	std::array<ClientMinimapOverlaySlot, kMinimapSpecialCapacity> special{};
	std::array<ClientMinimapLinkedSlot, kMinimapLinkedCapacity> linked{};
	std::uint64_t revision = 0;
};

// The decoded world the client holds after pumping the loopback. Positions are
// post-compression (lossy, ~|v|>>11 quantization) — exactly what the original
// client renders for its decoded peers. Callers must NOT "correct" them toward the
// authoritative value.
// The reassembly stream plus the decoded board. The stream is per-session
// state whose lifetime the decoder does not own, so it lives here -- retail
// keeps the same thing in one global stream [orig: g_ScoreReassemblyStream
// @0xA82324, reset only by an offset-0 chunk @0x431D79 and otherwise kept
// across completed decodes].
struct ClientEndRoundStats {
	bool header_known = false;
	EndRoundHeader header;
	// One per accepted 0x1D header: the edge a joiner latches its round-over
	// gate on (header_known stays set). [orig: NapiNPClientMsg_0x01D @0x430840]
	uint32_t header_updates = 0;
	// True once a complete board has been decoded at least once. A later
	// partial chunk does not clear it, so the screen keeps showing the last
	// complete board while the next one streams in.
	bool known = false;
	EndRoundStats board;
	// The stream: filled at each chunk's offset, completion tested against
	// each chunk's own declared total, retained after a decode.
	std::vector<uint8_t> buffer;
	// Chunks that arrived since the last offset-0 reset -- diagnostic only.
	uint32_t chunks_seen = 0;
};

// Requester-local S2C 0x81 sample. `updates` is the consume edge for the
// presentation/audio owner; `delta` retains retail's wrapping signed
// current-minus-previous result. The periodic Tab board remains independent.
// [orig: NapiNPClientMsg_ScoreDeltaSound @0x42A0B0]
struct ClientScoreFeedback {
	int32_t score = 0;
	int32_t delta = 0;
	uint32_t updates = 0;
};

// Latest requester-specific deploy-wave panel plus a valid-packet edge. The
// group body is already the strict npwire decode; retaining it here lets both
// loopback and remote clients consume one canonical state.
// [orig: NapiNPClientMsg_HandleSquadRosterSync @0x429880]
struct ClientSpawnWaveStatus {
	bool known = false;
	uint32_t updates = 0;
	SpawnWaveStatus value;
	// The zone whose member list names the local player (retail word_A85BC0:
	// reset to -1 at every fold, set to the group's zone handle when a member
	// equals the local handle @0x429a04..0x429a0b); 0xFFFF = none.
	uint16_t self_zone_handle = 0xFFFF;
};

// One zone ENTITY's two 0x6E-written words: the member-count byte (+550) and
// the wave countdown (+548). Each group whose zone handle resolves inside the
// pool tables rewrites them; a later 0x6E that leaves the zone out writes
// nothing, so the DEATH map keeps showing the last values.
// [orig: NapiNPClientMsg_HandleSquadRosterSync @0x429880 — the resolve
//  @0x429985..0x4299bd (`handle & 0xF000 < 0x5000`, slot < capacity), the
//  stores @0x4299bf / @0x4299c5]
struct ClientZoneWaveCounts {
	uint16_t zone_handle = 0xFFFF;
	uint8_t member_count = 0;   // entity+550
	uint16_t wave_countdown = 0; // entity+548
};

// Latest victim-local S2C 0x52 camera anchor. Retail stores the three fixed
// coordinates globally and Camera_ComputeThirdPersonPositions consumes them.
// [orig: NapiNPClientMsg_0x052 @0x428A80; consumer @0x438B80]
struct ClientDeathCameraTarget {
	bool known = false;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint32_t updates = 0;
};

// S2C 0x0F's authoritative local-player landing, retained by the reducer for
// the joiner frame to apply once per revision: the pose the host serialized
// right after Server_PositionPlayerForSpawn, and — for a waypoint gametype
// only, the off-wire hint the decoder is given — the host-filtered route the
// waypoint track walks. Retail writes both straight from the handler onto
// g_LocalPlayerEntity / g_WaypointList; our reducer keeps no entity, so the
// role lands them [orig: NapiNPClientMsg_0x00F @0x42E200 — the pose stores
//  (Position, Yaw, g_LocalPlayerLookYaw, Pitch @0x42E3E9, Roll @0x42E3F2);
//  the g_WaypointList rebuild (slot @0x42E47F, name id @0x42E492, the skipped
//  byte @0x42E49F, Pool_GetEntryUnchecked(3, slot) @0x42E4A3)].
struct ClientWorldStateLoad {
	std::uint32_t revision = 0; // advances once per decoded 0x0F
	int32_t pos_x = 0, pos_y = 0, pos_z = 0; // i32 16.16 world
	int32_t yaw_bam = 0;   // the wire i16 high word << 16 (BAM32)
	int32_t pitch_bam = 0;
	int32_t roll_bam = 0;
	// The waypoint-gametype hint was set when this landed: `waypoints` is the
	// authoritative route (empty = the host sent none). Off the hint the wire
	// carries no records and the local track is left alone.
	bool waypoints_set = false;
	std::vector<WorldStateWaypoint> waypoints;
};

// The connection quality the client itself measures: the last completed S2C
// 0x57 round trip, the ten-entry ring's mean, and the bucketed 0..4 level the
// C2S 0x4C report carries [orig: dword_A860D4 / CNetStats_GetAveragePing
//  @0x4C2750 / g_NetQuality @0x82BF88 via CNetQuality_SetLevel @0x4C3060].
struct ClientNetQuality {
	std::uint32_t ping_ms = 0;
	std::uint32_t average_ping_ms = 0;
	std::uint8_t level = 0;
};

// The session status a client keeps from S2C 0x58 (the authority builds the
// same record from its own report): the CMAP RULES text's source. The fold
// memsets it, keeps at most 31 characters of the server name and 63 of the
// mission name, the three bytes, the uptime and the 39 stat values, then only
// the first 8 option pairs whose key is 9 or less (the cursor still reads
// every advertised pair, each short read 0), stamps the local clock and marks
// it valid.
// [orig: NapiNPClientMsg_SessionStatus @0x4228c0 -> SessionStatus_ParseFromBuffer
//  @0x530ed0 into g_SessionStatus @0x24E3E88 — the name loops (32 / 64-byte
//  fields), the pair gate `count < 8 && key <= 9` @0x53107f, the GetTickCount
//  stamp +0x74 and valid @0x5310aa]
struct ClientSessionStatus {
	bool valid = false;               // +0x00
	std::string server_name;          // +0x04 g_SessionStatusServerName
	std::string mission_name;         // +0x24 g_ServerMissionName
	uint32_t game_type_byte = 0;      // +0x64
	uint32_t score_table = 0;         // +0x68
	uint32_t max_players = 0;         // +0x6C g_ServerMaxPlayers
	uint32_t uptime_ms = 0;           // +0x70
	uint32_t stamp_ms = 0;            // +0x74, the local clock at the fold
	std::array<int32_t, 39> stats{};  // +0x78
	struct Option {
		uint32_t key = 0;
		uint32_t value = 0;
	};
	std::vector<Option> options;      // +0x114 g_SessionStatusOptionCount, +0x118
};
ClientSessionStatus fold_session_status(const SessionStatusBlock &block, uint32_t now_ms);
// The session's elapsed milliseconds: 0 while not valid, else the uptime plus
// the time since the stamp (u32 wrap) [orig: SessionStatus_GetElapsedMS @0x52d5f0].
uint32_t session_status_elapsed_ms(const ClientSessionStatus &status, uint32_t now_ms);

// The S2C 0x7E strings a client keeps: each strncpy'd into its own 1024-byte
// buffer, the second buffer (byte_A86120) lying directly below the first
// (byte_A86520). The handler reads the first string up to its NUL (or the
// body end) and the second from just past it.
// [orig: NapiNPClientMsg_ServerConfigStrings @0x425e20 — strncpy(byte_A86520,
//  body, 0x400) @0x425e56, strncpy(byte_A86120, second, 0x400) @0x425e6d]
struct ClientServerConfigStrings {
	std::string first;  // the raw first string (briefing3 on a stock host)
	std::string second; // the raw second string (briefing2, else briefing)
};
void fold_server_config_strings(const std::vector<uint8_t> &body, ClientServerConfigStrings &out);
// The two buffers as a C-string read sees them: byte_A86520 (at most its 1024
// bytes: what lies past it is not this record's) and byte_A86120, whose read
// runs on into byte_A86520 when strncpy left its 1024 bytes unterminated.
std::string server_config_first_text(const ClientServerConfigStrings &strings);
std::string server_config_second_text(const ClientServerConfigStrings &strings);

struct ClientState {
	// Monotonic decoded-state edges. topology_revision changes only when the
	// ordered (handle,type) row layout changes; revision also covers field updates.
	// Presenters can therefore invalidate their row plan without coupling that
	// decision to optional history/event journaling.
	std::uint64_t revision = 0;
	std::uint64_t topology_revision = 0;
	// Advances only for accepted 0x0D/0x10/0x20 load records and live 0x59/0x12
	// placed-device lifecycle records. Unlike revision, per-frame compacts do not
	// touch it; world materializers can therefore skip ordinary 0x0A traffic.
	std::uint64_t world_stream_revision = 0;
	int32_t anchor_x = 0;                         // latest 0x0A frame anchor
	int32_t anchor_y = 0;
	int32_t anchor_z = 0;
	int16_t local_health = 0;
	// The 0x0A header tail's state byte: the authority's copy of this client's
	// own stance, bit 0 prone and bit 1 crouch (MoveOrder bits 8/9 >> 8). The
	// client re-latches its stance from it on every frame that carries the tail
	// (each such frame also advances health_updates_applied).
	// [orig: NapiNPClientMsg_0x00A -- the tail read @0x4303e5 (`mov dh, al`),
	//  the latches @0x430562 / @0x430570, MoveOrder bits 8/9 @0x430576..0x43058f]
	uint8_t local_stance_bits = 0;
	// Latest phase-0 0x0A projection of the authority's whole-second
	// pre-round timer. It is the client's Entity_UpdateAllEntities freeze gate;
	// networking and maintenance remain live while nonzero.
	// [orig: reader @0x430064; Game_ProcessMainFrame gate @0x52672C]
	std::uint8_t preround_delay_seconds = 0;
	uint8_t vehicle_reload_seconds = 0;
	uint32_t owned_zone_mask = 0;
	// S2C 0x66 replaces the complete 255-entry armory availability image.
	// Revision zero means no host policy has arrived.
	// [orig: NapiNPClientMsg_HandleWeaponRestrictions @0x42D4C0]
	std::array<int32_t, 255> weapon_availability{};
	uint64_t weapon_availability_revision = 0;
	bool cease_fire = false; // g_InCeaseFire @ 0x24C196C
	// The per-recipient SU gate: S2C 0x24 "SU <n>" stores (u8)atol(n); it
	// zeroes the 0x16 status words at parse time while clear and gates the
	// Tab board's " [..]" suffix; a session start clears it
	// [orig: g_ScoreboardStatusSuffixEnabled @0xA85B49 — the store
	//  NapiNPClientMsg_HandleTextCommand @0x429f71, the parse gate @0x42fbfb,
	//  the drawer test @0x423ef8, the reset Client_ResetDisconnectState
	//  @0x5202d9].
	uint8_t scoreboard_status_suffix = 0;
	hud::KillAnnouncement kill_announcement;
	// [orig: NapiNPClientMsg_HandleSessionConfig @ 0x4281D0]
	bool permanent_death = false;
	// The 0x08 record's trailing rules word whole (its bits 13/15/16 are the
	// latches beside it); the death screen reads its TeamChoose bit on a
	// joiner [orig: dword_A821E4, NapiNPClientMsg_HandleSessionConfig
	//  @0x4281D0, the store @0x428368; read by DeathScreen_UpdateUI @0x55345C].
	uint32_t session_rules_flags = 0;
	// The session's KOTH time limit in minutes, the 0x08 record's second rule
	// dword — a joiner's GAMEINFO team timers read this copy where the
	// authority reads its own g_TimeLimitMinutes [orig: dword_A821C0, stored
	// @0x428218; read by HUD_DrawGameTimerOverlay @0x59CCEB, and by the Tab
	// board's KOTH countdowns @0x423281 / @0x423a58].
	int32_t session_time_limit_minutes = 0;
	bool spectators_allowed = false;
	// The flag carrier the FlagBall / type-8 header line names: the carrier
	// handle of the last S2C 0x2F for a flag item (4091/4093/4095) in those
	// game types, 0xFFFF when none [orig: dword_A860C4 — the stores
	// NapiNPClientMsg_0x02F @0x43114d/@0x43115a; cleared by ZoneTimers_ResetState
	// @0x4244ac].
	uint16_t flag_carrier_handle = 0xFFFF;
	// The talk keys' reset hold: raised by S2C 0x25 on a client, lowered by
	// the next S2C 0x0F [orig: dword_24C195C — NapiNPClientMsg_GameReset
	// @0x42284e, NapiNPClientMsg_0x00F @0x42e396; read by the talk arms
	// @0x49b9ad].
	bool round_reset_hold = false;
	// The round-over latch (g_SpawnSuccessGate): a client raises it on the
	// 0x1D header ahead of its parse and on S2C 0x25; the authority's own
	// round end raises it beside the 0x1D it broadcasts; only a mission start
	// lowers it (ClientReplicaPipeline::begin_mission). The talk keys, the
	// chat senders and the sender gate, the breath bar and the Esc chain
	// read it. [orig: NapiNPClientMsg_0x01D @0x430858; NapiNPClientMsg_GameReset
	// @0x422849; Server_ProcessRoundEnd @0x5168e4; Game_StartMission @0x524a1f]
	bool spawn_success_gate = false;
	// [orig: NapiNPClientMsg_0x00F @ 0x42E200, byte_A860DD]
	bool deploy_check_secured_spawn = false;
	// The joiner's copy of the round clock, in 62 Hz ticks (-1 = untimed),
	// folded from the 0x0A sub-block-1 timer snapshot: 62 x the wire's whole
	// seconds, or -1 when the wire value is negative. Feeds the end-round
	// ladder's game-time line and timed/untimed arm picks (D-HUD-25).
	// [orig: g_RoundTimeRemaining @0x24C1958 — the store
	//  NapiNPClientMsg_0x00A @0x430219..0x430235; mission-start seed -1
	//  @0x524A89]
	std::int32_t round_time_remaining_ticks = -1;
	// The authority's breath seconds and fall-damage tolerance, the two WAC
	// named values the same sub-block-1 timer state carries, zero-extended
	// from their wire bytes; until the first one lands they hold the
	// WacScript_FreeAll seeds 20 / 13.
	// [orig: NapiNPClientMsg_0x00A `mov g_WacVarBreathTime,edx` @0x4301A1, `mov
	//  g_WacVarFallMps,eax` @0x4301BC; seeds @0x4F6381 / @0x4F638B]
	std::int32_t breathtime = 20;
	std::int32_t fallmps = 13;
	// The local player's underwater breath samples (four per submerged second;
	// the drown limit is 4 * breathtime): the host's playerSlot+460 crossing
	// as the phase-0 sub-block byte, the breath bar's counter. Retained
	// between phase cycles like the client global.
	// [orig: NapiNPClientMsg_0x00A @0x430104 -> word_A85B7C; the host's write
	//  NetPacket_WritePlayerState @0x4FF8D5; reader HUD_DrawBreathBar @0x59D6F0]
	std::uint16_t breath_samples = 0;
	// The other three phase-0 0x0A sub-block-0 whole-second timers the DEATH
	// screen reads [orig: NapiNPClientMsg_0x00A stores @0x430084 dword_A85B5C
	// (slot+360, the respawn penalty — STROVER_PENALTYTIMER), @0x43009f
	// dword_A85B60 (slot+368, the local revive window — STROVER_MEDICTIMER /
	// STROVER_CALLMEDIC), @0x4300c3 dword_A85B68 (slot+364, the spawn-target
	// hold — STROVER_PSPRESPAWN); consumer UI_UpdateDeathScreenContent
	// @0x5536a0]. Retained between phase cycles like the client globals.
	// The client-local death screen (retail g_DeathScreenActive): the 0x0A
	// header's flags1 bit 0 EDGES — a rising edge opens it and zeroes the
	// sub-mode / kill-cam target and arms the enemy-tag grant; a falling edge
	// closes it and clears the grant [orig: NapiNPClientMsg_0x00A
	// @0x42ff88..0x43002b — dword_A860F0/A860F4 = 0 @0x42ffa6, g_EnemyTagsVisible
	// @0x42ffb2/@0x430025]. S2C 0x75 writes the same latch, sub-mode and
	// target [orig: NapiNPClientMsg_SetSpectatorMode @0x4259e0]; the
	// spectate actions cycle them (client_replica_spectate.cpp).
	uint8_t hud_hit_feedback_frames = 0; // [orig: dword_A8235C @0x42FF60..0x42FF74]
	bool death_screen_active = false;
	// 0 free, 1 chase, 2 first person [orig: dword_A860F0].
	std::uint8_t death_screen_submode = 0;
	// The spectated entity's wire handle, 0xFFFF none [orig: dword_A860F4, a
	// pool pointer]. The falling edge leaves it stale, as retail's does; every
	// reader gates on the death screen.
	std::uint16_t spectate_target = 0xFFFF;
	// The local entity's +0x1E0 "a medic is reviving me" latch: set by S2C
	// 0x3A, cleared when the local player's own dead->alive edge runs
	// Game_InitNewRound and at mission start. The DEATH screen hides its
	// MEDIC/CALLMEDIC statics while it is set [orig: NapiNPClientMsg_0x03A
	// @0x422680 store; Game_InitNewRound @0x422740 clear; the statics test
	// `!entity+0x1E0` @0x553ec5].
	bool local_medic_reviving = false;
	// The self row's respawn_revision the latch clear last consumed.
	std::uint32_t local_respawn_revision_seen = 0;
	bool enemy_tags_visible = false;
	// The deploy-map OVERLAY (retail g_DeployScreenActive @0xA860DC): armed by
	// the S2C 0x0F game_flags bit0 unless the death screen is already up, then
	// host-maintained — set AND cleared — every per-frame 0x0A from flags1 bit1.
	// It is a UI signal only (the frame loop opens death.mnu's DEATH screen once
	// off it, latched); it never gates the spawn. [orig: NapiNPClientMsg_0x00F
	// zero @0x42e2d8 + arm @0x42e2f8; NapiNPClientMsg_0x00A per-frame assign
	// @0x42ff82; the death.mnu open latch Render_ProcessMainSceneFrame
	// @0x5cab5e..0x5cab8b]
	bool deploy_overlay_active = false;
	// The death.mnu DEATH screen's open latch (retail g_DeathMenuOpenLatch
	// @0x24C1894): the frame loop opens the screen once per arming and stamps
	// the latch result-blind; only the trigger falling clears it (the
	// close-on-clear leg -> Game_CloseInGameScreens), never the player's own
	// dismiss, so a host that keeps the bit set all session shows the screen
	// once and a host that clears and re-arms it shows it again. The
	// no-active-menu gate (sub_54B970 != 0) and the spawn-success gate stay
	// the shell's / unmodeled. [orig: Render_ProcessMainSceneFrame
	// @0x5cab5e..0x5cab8b (gate, latch test @0x5cab70, stamp @0x5cab8b);
	// Game_CloseInGameScreens @0x54b940 zero @0x54b954, called when the
	// triggers clear @0x5cac8e..0x5cac9c]
	bool deploy_overlay_open_latch = false;
	// The frame loop's open decision: true exactly once per arming, and the
	// latch is stamped whether or not the shell's open succeeds.
	bool take_deploy_overlay_open() {
		if (!deploy_overlay_active || deploy_overlay_open_latch) return false;
		deploy_overlay_open_latch = true;
		return true;
	}
	std::uint8_t respawn_penalty_seconds = 0;
	std::uint8_t local_revive_seconds = 0;
	std::uint8_t spawn_hold_seconds = 0;
	// Advances only when the complete seven-byte recipient-local 0x0A tail was
	// decoded. frames_applied remains the lenient partial-presentation counter.
	// Every compact entity record folded from an 0x0A. The replication heartbeat: it
	// climbing means peers' poses are still arriving, flat means they are not.
	std::uint32_t compact_records_applied = 0;
	std::uint32_t health_updates_applied = 0;
	// Phase-3 objective masks, present when g_GameType bit 0x20000 is active.
	// Text IDs remain mission-local; these four authoritative masks drive them.
	std::uint32_t objective_won = 0;
	std::uint32_t objective_lost = 0;
	std::uint32_t objective_show_win = 0;
	std::uint32_t objective_show_lose = 0;
	std::uint32_t objective_updates_applied = 0;
	ClientEnvironmentState environment;
	ClientMountedAmmoState mounted_ammo;
	ClientMinimapState minimap;
	ClientWorldStateLoad world_state;
	ClientNetQuality net;

	// The Tab board's two folded lanes: the 0x16 scoreboard and the 0x46
	// connection-slot roster it joins names from.
	// The END-OF-ROUND STAT BOARD, reassembled from the chunked S2C 0x56 lane.
	// Distinct from `scoreboard` above: that is the in-match Tab list refreshed
	// every 311 ticks, this is the post-round board the stat.mnu screen reads
	// [orig: NapiNPClientMsg_0x056 @0x431D10].
	ClientEndRoundStats end_round;
	ClientScoreboard scoreboard;
	ClientScoreFeedback score_feedback;
	// The host VarList's EXP_FANFARE u16 (lo/hi thresholds of the 0x81 tone
	// ladder, hud/score_fanfare.h) [orig: g_SessionVarExpFanfare @0x24d5a10].
	uint16_t exp_fanfare = 0;
	ClientSpawnWaveStatus spawn_waves;
	std::vector<ClientZoneWaveCounts> zone_wave_counts;
	// The client's local millisecond clock (the platform stand-in for
	// GetTickCount the 0x58 fold stamps and the elapsed read compares):
	// advanced io::kTickMs per client net frame.
	uint32_t local_clock_ms = 0;
	// S2C 0x58 (the CMAP RULES text's record) and S2C 0x7E (a client's
	// briefing strings).
	ClientSessionStatus session_status;
	ClientServerConfigStrings server_config_strings;
	// The two HUD order lines S2C 0x72 writes: [0] the individual order, [1]
	// the fireteam order, 127 characters each; an empty line cancels. They
	// hold until overwritten or the next mission start clears them.
	// [orig: Team_SetNameByIndex @0x59c2d0 (a misnomer: the order-line
	//  store) — strncpy(byte_2721DB8 + kind*128, str, 127) for kind < 2;
	//  cleared by HUD_InitOverlaySystem @0x5a49b0]
	std::array<std::string, 2> squad_orders;
	// Bumps on every S2C 0x71 / 0x73 fold: the CMAP team list re-populates
	// on it [orig: j_cmap_populate_team_list_0 @0x54e3c0 from @0x4256e5 /
	// @0x425892].
	uint32_t squad_revision = 0;
	ClientDeathCameraTarget death_camera;
	std::array<ClientRosterSlot, 256> roster{};
	// The S2C 0x6A clan registry, in node insertion order (retail walks its
	// list head-first by netId, ids unique) [orig: g_SpawnWaveList.field_C —
	// the IDB name is a misnomer; NapiNPClientMsg_HandlePlayerJoinLeave
	// @0x432510].
	std::vector<ClientClanRegistryNode> clan_registry;
	// The C2S 0x4E {netId} continuations each action-3 update queues (the
	// runtime frames and clears them) [orig: CNapiNetwork_QueueReliableMessage
	// (0x4E, {netId}) @0x43266c].
	std::vector<uint32_t> pending_clan_walk_requests;
	// The S2C 0x4C player-slot pointer table, replaced whole by every snapshot
	// [orig: g_PlayerSlotPtrTable / g_PlayerSlotPtrCount @0xA822D0/D4 — freed
	//  and zeroed @0x428583..0x4285a1, one entry per snapshot entry @0x428698].
	std::vector<ClientVisiblePlayer> visible_players;
	// The 0x22 + 0x23 refresh pairs the 0x4D / 0x50 folds queue.
	std::vector<ClientVisiblePlayersRefresh> pending_visible_refreshes;
	std::vector<std::string> location_names;
	// The map's tracked target (the retail globals g_HUDTrackedTarget and
	// dword_2721EBC..dword_2721ED0), set by HUD_SetTrackedEntityTarget from
	// the S2C 0x6D event-6 radio call, the S2C 0x2D emote and a chat line on
	// channel 13; the per-tick decrement is the entity update's.
	// [orig: HUD_SetTrackedEntityTarget @0x59D050 — callers
	//  NapiNPClientMsg_HandleEntityDeath @0x430de4, NapiNPClientMsg_HandleEmote
	//  @0x427f5b, Chat_DispatchToChannel @0x42ba09; the decrement sub_590950
	//  from Entity_UpdateAllEntities @0x4C2221]
	struct TrackedTarget {
		uint16_t handle = 0xFFFF;
		uint32_t ticks_remaining = 0; // dword_2721EBC
		int32_t position[3] = {};     // dword_2721EC0..EC8
		bool friendly = false;        // byte_2721ECC
		uint32_t color = 0xFFFFFFFFu; // dword_2721ED0 at the set
		// Bumps on every set: the map's tracked callout restamps its colour
		// global then [orig: HUD_SetTrackedEntityTarget @0x59D0EF..0x59D0FF].
		uint32_t serial = 0;
	} tracked_target;
	std::vector<ClientEntityState> entities;
	std::uint32_t frames_applied = 0;

	ClientEntityState *find(uint16_t handle);
	const ClientEntityState *find(uint16_t handle) const;
	ClientEntityState &upsert(uint16_t handle);
	void mark_changed() { ++revision; }
	void mark_topology_changed() {
		++topology_revision;
		++revision;
	}
	// Consume-once drain for the per-row anim transition pulses: the presenter
	// calls this after building a snapshot so each pulse dispatches exactly one
	// frame (see ClientEntityState::anim_state_pulse).
	void clear_anim_pulses();
};

// The joiner-side map grid-origin resolve: the first decoded pool-3 entity
// of type 2043, the same client pool scan retail's HUD init runs. The host
// resolves the same rule from the mission doc at promotion
// (World::map_grid_origin_*); this is the ONE decoded-view home so the
// selection rule cannot fork per embedder. Returns false when no origin
// entity has decoded yet.
// [orig: HUD_InitOverlaySystem @0x5a4999 pool scan (entity+80 == 2043)]
bool client_minimap_grid_origin(const ClientState &state, int32_t &out_x_q16,
		int32_t &out_y_q16);

// The retail team-byte -> overlay-color resolve for a live regular marker
// row: team 1 -> table[0x0A] blue, team 2 -> table[0x09] red, anything else
// (including undecoded) -> table[0x0C] neutral green. The ONE home for the
// mapping so an embedder restoring a client-local row (the deployed local
// player) cannot fork the palette.
// [orig: the team switch @0x5becb8..0x5bece4 over
//  g_MinimapOverlayColorTable @0x840A10]
uint32_t minimap_team_argb(uint8_t team);

} // namespace opennova::replication
