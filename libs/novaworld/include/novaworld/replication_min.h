#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <novaworld/ingame_decode.h> // EntityClass (the §5.10b replication class)

namespace opennova {

// In-game replication payload builders. Each builder produces raw payload bytes
// for one inner-message tag; the caller wraps it in the :64220 protocol
// envelope. The input structs here are intentionally transport-neutral so the
// CLI server, Godot server scene, tests, and future game client share one game
// state model.

struct PlayerReplicationState {
	// Player identity for the joining client.
	std::string player_name = "DevUser"; // ≤32 chars; what `tag=0x16` populates as the roster entry name and `tag=0x46` echoes back.
	std::string clan_tag = "";           // ≤16 chars; mostly cosmetic.
	uint8_t player_slot = 0;             // Slot index in `dword_A87048` player table; matches `Pool_GetEntryUnchecked(0, slot)`.
	uint8_t entity_slot = 0;             // Index of the player's entity in pool 0.
	uint8_t team = 1;                    // entity+354 / tag=0x46 team field. 0 = "no team / spectator" (player can shoot but not move). 1 = blue team. 2 = red team. NapiNPClientMsg_0x05A@0x4290e0 reads g_local_player_entity->Team and treats Team==1 || Team==3 specially. Default 1 = playable.
	uint32_t mi = 0x3CDEu;               // Server-side MI from ServerAuth — this is what `sub_4E0090@0x4E0090` matches against entity[120].
	// Spawn coordinates for the player on the joining map (dvxi5 by default).
	// Engine units are signed 32-bit ints; the values below land near the
	// dvxi5 map center based on the retail capture's tag=0x0C spawn (see
	// `apps/novaworld/src/main.cpp` for the kPosBytes literal).
	uint32_t spawn_x = 0xfe56f854u;
	uint32_t spawn_y = 0x0049f5f0u;
	uint32_t spawn_z = 0x003a5e6au;
	// Yaw / pitch / roll as raw u16 (client shifts each by 16 to fixed-point).
	uint16_t yaw = 0;
	uint16_t pitch = 0;
	uint16_t roll = 0;
	// Player entity item-template id. `0x14B9` (5305) matches retail's
	// "default infantry player" template observed in capture frame ~82500
	// from the existing tag=0x0C spawn body in `apps/novaworld/src/main.cpp`.
	// Kept for player-state builders that still need the infantry type id.
	uint16_t entity_type_id = 0x14B9u;
};

// Mission entity data shared by all game-server frontends. The CLI can fill
// this from parsed .bms pool-2 records; Godot scenes can later author the same
// records directly. Runtime replication code treats this as the source of
// server-owned world entities and never reaches back into a transport layer.
struct GameEntitySnapshot {
	uint8_t pool = 2;
	uint16_t slot = 0;
	uint16_t type_id = 0;
	uint16_t flags = 0;
	uint8_t team = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	// §5.10b replication class for the S2C 0x0A event loop (D-NET-50): selects the
	// compact encoder. Unknown ⇒ the entity is NOT emitted as a 0x0A compact record
	// (the authoring / .bms layer sets it from the item's *_function class tag).
	EntityClass entity_class = EntityClass::Unknown;
};

struct EntityBatchBuildResult {
	std::vector<uint8_t> payload;
	size_t next_cursor = 0;
	size_t entity_count = 0;
};

// tag=0x0F WORLD-STATE-LOAD (we historically labeled this "GAME-START" — the
// label stays in our function name for backwards compat, but the IDA decompile
// of `NapiNPClientMsg_0x00F@0x42E200` shows it loads spawn pos + angles + team
// scores + waypoint names, not just a "start" signal). The actual UI-banner
// "GAME-START" is tag=0x05 (`NapiNPClientMsg_HandleGameStart@0x42E180`); we
// don't currently emit it. Witnessed at `NapiNPClientMsg_0x00F@0x42E200` (Jointops
// retail dispatcher — see `notes/dispatcher_table.md` for naming-convention details).
//
// This is the load-bearing message for movement: setting `dword_81474C = 0`
// is what makes the client start sending heartbeats (tag=0x2c) and player-input
// updates (tag=0x0c) every frame. Without it the client just acks the protocol
// stream and never produces gameplay traffic.
//
// Layout: 4×u32 (state + spawn X/Y/Z), 3×u16 (yaw/pitch/roll), 1×u8 flags,
// 128×u32 team-scores zero-pad (the size of `dword_B75FE8 .. dword_B761E8`),
// 1×u16 weapon_pool_count = 0, 1×u16 team_count = 0. Total 539 B — matches
// retail's frame 82537 byte-for-byte except the spawn coords + state-id.
std::vector<uint8_t> build_tag_0f_game_start(const PlayerReplicationState &player);

// tag=0x16 PLAYER-LIST. Witnessed at `NapiNPClientMsg_0x016@0x42FAE0`.
// Populates `dword_A87048` with one player entry (the joining client itself).
// We use the simplest 1-player roster with no teams — enough for the client
// to find itself in the list when sub_4348D0 / sub_434780 lookups fire.
std::vector<uint8_t> build_tag_16_player_list(const PlayerReplicationState &player);

// tag=0x46 PLAYER-SYNC. Witnessed at `NapiNPClientMsg_0x046@0x431370`. We
// emit a single-player sync with name + clan + team flags, no ack-trigger
// (bit 14 unset) — the client just absorbs it without responding.
std::vector<uint8_t> build_tag_46_player_sync(const PlayerReplicationState &player);

// tag=0x10 ENTITY-BATCH. Witnessed at `NapiNPClientMsg_0x010@0x433400`.
// Retail sends continuous ~612-644B batches while the player is in world. The
// minimal decoded body is `[u16 start_slot][u16 count]` followed by records:
// `[u16 type_id][u16 flags][i32 x][i32 y][i32 z]`.
EntityBatchBuildResult build_tag_10_entity_batch(
		const std::vector<GameEntitySnapshot> &entities,
		size_t start_cursor = 0,
		size_t max_payload_bytes = 620);

// tag=0x10 ENTITY-BATCH (zero entities). Witnessed at
// `NapiNPClientMsg_0x010@0x433400`. Empty batch is wire-valid: 4 bytes
// `[u16 start=0][u16 count=0]`. Used as a periodic heartbeat to satisfy
// any state machine that gates on having seen at least one tag=0x10.
std::vector<uint8_t> build_tag_10_entity_batch_empty();

// tag=0x57 RTT request. Witnessed at `NapiNPClientMsg_0x057_RTT@0x432210`.
// Sends `[u32 tick][u8 echo_request=1]` so the client replies with tag=0x2C
// carrying our tick back. Useful to confirm the client is alive AND in the
// active gameplay state (gated on `dword_24C1928 == 0`).
std::vector<uint8_t> build_tag_57_rtt_request(uint32_t tick);

// tag=0x2A CHAT-HISTORY entry. Witnessed at
// `NapiNPClientMsg_0x02A@0x425BA0`. Fixed 10-byte payload `[u32 a][u32 b][u16 c]`.
// Only processed if the client is non-authority. Currently unused in the
// minimum unblock burst, but exposed for completeness.
std::vector<uint8_t> build_tag_2a_chat_history(uint32_t a, uint32_t b, uint16_t c);

// tag=0x5A WEAPON-LOADOUT-SYNC. Witnessed at
// `NapiNPClientMsg_0x05A@0x4290e0` (Phase D.2.20). The handler:
//   - Reads [i8 class_index] (v55 in IDA decomp; gets stored at
//     g_local_player_entity->pad8[304] = class slot, used by sub_4127B0
//     to derive the player's actual entity-template id)
//   - Reads [u8 first_weapon_id], then loops reading
//     [u8 ammo][u8 alt_ammo][u8 ?][u8 next_weapon_id], terminating when
//     next_weapon_id == 0xFF. Up to 40 entries.
//   - Calls sub_53F240/sub_5414E0/sub_541690 to install the weapon-slot
//     strings + refresh terrain vertex weights
//   - At LABEL_76 (line 0x429706) sets `dword_81474C = 0` — the SAME
//     gate `Client_ProcessNetworkFrame@0x42c180` checks before sending
//     tag=0x2c heartbeat / tag=0x0c player-input. So tag=0x5A is the
//     missing trigger that unblocks WASD movement after the player has
//     spawned via tag=0x0F.
//
// We push the 34-byte payload retail captured in frame 82537 verbatim
// for now (no per-class customization yet — the client only validates
// against ItemList type-id existence, not specific values, so any
// loadout retail's server actually sent will satisfy the validator).
std::vector<uint8_t> build_tag_5a_weapon_loadout();

// tag=0x0A WORLD-REFERENCE frame. Witnessed at
// `NetPacket_SerializePlayerState @ 0x4C09C0` (formerly sub_4C09C0), case 2 =
// "read (deserialize from packet and apply to entity)". This is THE MOVEMENT
// ENABLER — without tag=0x0A, the client's local player Position NEVER
// advances. Retail's server pushes tag=0x0A (~620 bytes) every ~100ms; client
// deserializes into the local entity + calls Game_InitNewRound when
// appropriate. Payload bytes 0-11 = Position X/Y/Z as signed int32 LE.
//
// First-pass implementation: verbatim copy of retail capture3 frame 211497
// (623 bytes), with bytes 0-11 patched to ctx.spawn_x/y/z so the client
// locks to OUR spawn coord, not retail's.
std::vector<uint8_t> build_tag_0a_world_reference(const PlayerReplicationState &player,
                                                  const std::vector<GameEntitySnapshot> &entities = {});
std::vector<uint8_t> build_tag_0a_player_state(const PlayerReplicationState &player,
                                               const std::vector<GameEntitySnapshot> &entities = {});

// tag=0x1E GAME-EVENT. Witnessed at `GameEvent_BuildPayload@0x5054E0` (server-
// side payload builder) + `NetPacket_HandleGameEvent@0x426270` (client-side
// receiver — handles ~60 event types). Phase D.0.7.
//
// Wire format (8 bytes, fixed):
//   [u8  event_type]          — one of ~60 event IDs (kills, deaths, spawns,
//                                 objective captures, zone control, etc.)
//   [u8  attacker_pool_idx]   — 0xFF if N/A
//   [u8  victim_pool_idx]     — 0xFF if N/A
//   [u8  aux_pool_idx]        — 0xFF if N/A
//   [u16 pos_x_high_word]     — high word of position X (signed fixed-point)
//   [u16 pos_y_high_word]     — high word of position Y
//
// Retail capture frame ~82540 contains exactly: `3a 04 ff ff 00 00 00 00`
// — event_type=0x3A (one of the ~60 events; meaning not yet decoded but
// retail emits it immediately after tag=0x5A in the post-spawn burst), with
// no attacker/victim/aux pool refs and zero position. We push this verbatim
// for now since (a) it's part of the expected post-spawn sequence retail
// captures show, (b) NetPacket_HandleGameEvent has many side effects per
// event type that may matter for client state transitions.
//
// IMPORTANT CAVEAT (Phase D.0.7 finding): tag=0x1E does NOT directly call
// `Game_InitNewRound` on the client side — Game_InitNewRound has only 2
// callers, neither in NetPacket_HandleGameEvent. So this push alone may
// not unblock movement; it's a probe. If movement remains gated, the next
// step is to identify which other msg routes through `sub_4C09C0` (the
// other Game_InitNewRound caller) and push that.
std::vector<uint8_t> build_tag_1e_game_event_post_spawn();

// tag=0x0D LOCAL-PLAYER-SPAWN. Experimental/quarantined.
//
// The latest live client SYSDUMP faults in `NapiNPClientMsg_0x00D + 0x730`
// while processing our standalone local-player 38-byte payload. Reference
// captures do not send this shape during the loading boundary, so production
// session code must not call this builder. It remains available only as an
// opt-in fixture for tests and future IDA comparison.
//
// Witnessed at `NapiNPClientMsg_0x00D@0x432C40`
// (Phase D.0). tag=0x0D is the per-pool entity-spawn tag — `slot_id` packs
// `(pool << 12) | slot`, letting the server target ANY pool including pool 0
// (the player pool). This is the route to write `entity[36] = 0` on the local
// player, which clears the bit-1 movement-gate that `Player_BuildTag0CInputBody
// @ 0x42A550` checks before serializing a tag=0x0C frame.
//
// Why we need this: after Phase D.2.20 (tag=0x5A clears `dword_81474C`), the
// client emits tag=0x0C with varying mouse-look + WASD bitmask, but the
// position bytes inside each tag=0x0C are constant. Client-local physics
// integration is gated by `(g_local_player_entity->pad0[9] & 2)` (entity+36
// bit 1). Sending tag=0x0D with `flags & 0x20` set and entity36 = 0 is the
// only known wire route to explicitly clear that bit on the local player.
//
// Layout per `NapiNPClientMsg_0x00D` decomp:
//   [u16 count]                              // we send 1
//   [u16 flags]                              // 0x0010 (team) | 0x0020 (entity+36 write) = 0x0030
//   [u16 slot_id]                            // (0 << 12) | player_slot = pool 0, slot N
//   [u16 type_id]                            // entity type — must be a valid ItemList type
//   [name\0]                                 // null-terminated, player.player_name
//   [u32 entity36]      because flags&0x20:  // 0 — clears ALL bits including bit 1
//   [u32 pos_x]                              // spawn coord
//   [u32 pos_y]
//   [u32 pos_z]
//   [u8  team]          because flags&0x10:  // ctx.team
//   [u16 weapon0]       because flags&0x100
//   [u16 weapon1]       because flags&0x200
//   [u8  weapon_seat]   because flags&0x400
//   [u8  v9[145] = 0]                        // ALWAYS - bone-attach byte
std::vector<uint8_t> build_tag_0d_local_player_spawn(const PlayerReplicationState &player);

// tag=0x51 PLAYER-SPAWN per `NapiNPClientMsg_HandlePlayerSpawn @ 0x431BB0`.
// 8-byte body that completes the local-player spawn flow:
//
//   offset size field            effect on receiver
//   ------ ---- ---------------- -----------------------------------------------
//   0      2    team_u16         echoed back by client as (team+1) in C2S 0x29
//   2      2    entity_slot      (pool << 12) | slot — local player's entity
//   4      1    team_byte        → entity[+354] (the actual team field)
//   5      2    weapon_index     → entity[+348] if entity has PLAYER flag (0x100)
//   7      1    camera_byte      → entity[+884] if entity has PLAYER flag
//
// Server emits this in response to client's C2S tag=0x29 (sent from
// `NapiNPClientMsg_0x00F` line 0x42e61e after our tag=0x0F WORLD-STATE-LOAD
// arrives, with payload `00 00`). Without it the spawn-select menu stays
// open and the player can't enter the world. Per the 2026-04-25 IDA sweep
// (NapiNPServerMsg_0x029 @ 0x514F10), this is the canonical retail wire
// route; the previously-emitted tag=0x1E only shows a HUD tip.
std::vector<uint8_t> build_tag_51_player_spawn(const PlayerReplicationState &player);

// One spawn-point record for `build_tag_0d_spawn_points()`. Sourced from
// parsed .bms pool-1 entities whose type_id appears in kSpawnPointTypeIds.
struct SpawnPointEntity {
	uint8_t pool;           // 1 for .bms spawn points (per `sub_4FE110` check,
	                        // pools 0/1/2 accepted; .bms layout uses pool 1).
	uint16_t slot;          // .bms slot index (low 12 bits of packed handle).
	uint16_t type_id;       // one of kSpawnPointTypeIds (SpawnPoint attrib in
	                        // items.def). Client's `sub_4FE110` requires
	                        // `item_def[84] & 0x8000` which the SpawnPoint
	                        // attrib sets.
	uint8_t team;           // .bms record byte 73. Player's team must match
	                        // OR player team == 0 (per `sub_4FE110`).
	int32_t x, y, z;        // world engine-coord position.
};

// Hardcoded catalog of item-def type_ids with the "SpawnPoint" attrib, extracted
// offline from JO's localres.pff/jox01.pff items.def via libs/scr decryption
// (key=SCR_KEY_JO_DFX2=0x2A5A8EAD) and libs/def::def_parse_items(). Verified
// against IDA `Entity_BuildSpawnPointList @ 0x42DE40` which also walks pool
// entities and filters by `item_def[84] & 0x8000`. All 18 types here are
// "landable SpawnPoint ChangeTeam" objects (LFP towers, concrete bunkers,
// rebel HQs, etc.) placed in .bms pool 1 as team-owned spawn volumes.
inline constexpr uint16_t kSpawnPointTypeIds[] = {
	130, 131, 302, 303, 304, 305, 306, 307, 308, 309, 310,
	508, 1336, 1359, 1397, 1398, 1399, 1884
};
inline constexpr size_t kSpawnPointTypeIdsCount =
		sizeof(kSpawnPointTypeIds) / sizeof(kSpawnPointTypeIds[0]);

// Build a multi-entity tag=0x0D body that registers every given spawn point
// into its source pool (typically pool 1). Witnessed at
// `NapiNPClientMsg_0x00D @ 0x432C40` — same layout as
// `build_tag_0d_local_player_spawn` but with one entity per input vector
// element and an empty name string (spawn-point entities don't need names).
//
// Why this exists: the CTF-menu / spawn-select UI on the client walks pool 0/1/2
// looking for entities whose item-def has bit 0x8000 set (spawn-point flag).
// If pool 1 is empty on the joining client, clicks on any spawn marker in the
// map view resolve to 0xFFFF (no valid entity) and the menu can't commit the
// spawn. Populating pool 1 with the .bms spawn-point entities via this tag
// gives the menu something to bind clicks to.
std::vector<uint8_t> build_tag_0d_spawn_points(const std::vector<SpawnPointEntity> &points);

// tag=0x40 CAPTURE-ZONE state broadcast. Latest capture/IDA alignment says
// this belongs to pool-2 capture-zone objects, not pool-1 spawn points. The
// runtime keeps it disabled until we classify those objects from mission data.
// Witnessed at
// `NapiNPClientMsg_0x040 @ 0x425A50` → `sub_425A54` → `MapOverlay_DecodeOverlayEntries` →
// `MapOverlay_UpdateOrCreateSlot` → `sub_5BE970`. The chain ALLOCATES entries in the
// `unk_28E5620` minimap-overlay array (1160 slots × 32 bytes) — which is the
// ACTUAL source of clickable spawn markers in both the CMAP menu
// (`sub_5492A0`) AND the DEATH spawn-select menu (`sub_5536A0`). Filters
// applied when the menu reads the array: entity team matches player team,
// and `item_def[84] & 0x40000` (the SpawnPoint bit set by the `SpawnPoint`
// attrib in items.def, witnessed at `ItemDef_ParseProperty @ 0x4A0BBA`).
//
// Wire format (MapOverlay_DecodeOverlayEntries parses; full field map +
// controlled-capture witness in docs/net/novaworld-net-re.md §5.19):
//   [u8 count]
//   count × 6-byte record:
//     [u16 packed_handle]   ; (pool << 12) | slot — 0xFFFF = skip
//     [u8  param]           ; entry+2 → slot+2; retail uses 0x00
//     [u8  iconColor]       ; entry+3 → g_minimap_overlay_color_table idx (0x0c neutral / 0x09 Red / 0x0a Blue) — capture state
//     [u8  flags]           ; entry+4 → 0x10 persistent capture-zone, 0x20 clear slot
//     [u8  source]          ; entry+5 → slot+4
//   (corrected 2026-06-17: entry+3/+4/+5 ARE read by MapOverlay_UpdateOrCreateSlot,
//    not stride-only as an earlier note here claimed.)
//
// Retail capture3 (dvxi5 AS, same map as ours) sends this tag 399× over 235s
// ≈ 1.7 Hz cadence. Example body for 4 spawn points (pool 1 slots 49-52 of
// ASH_I5A.bms, matching our `bms_spawn_points`):
//
//   04  31 10 00 0c 10 c2  32 10 00 0c 10 c2  33 10 00 0a 10 01  34 10 00 09 10 03
//
// Without this push, `unk_28E5620` stays empty on the joining client, so the
// spawn-select menu has no entries regardless of pool 1/2/3 content. Symptoms:
// single fallback marker visible, clicks resolve to 0xFFFF, pressing Space
// triggers tag=0x0E auto-pick but nothing else.
std::vector<uint8_t> build_tag_40_capture_zone_state(
		const std::vector<GameEntitySnapshot> &entities);

} // namespace opennova
