#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <runtime/world/player_spawn.h> // world::kRetailPlayerMinEntitySlot (canonical)

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

// P3 — the World-driven host-side player spawn (§5.2a steps 1-2 / §5.2b). Replaces the
// fixture/game_runtime spawn-gate: the listen-server host runs its own server-side spawn machinery
// in-process and registers the pool-0 player entity in `World` (ADR 0012), writing entity+0x78
// ownerConnectionId. Free functions over NapiNPServerCtx + world::World, mirroring the original flow
// 1:1. Godot-free / socket-free (engine/CLAUDE.md). The §5.2b field-init itself is already ported in
// engine/runtime/world (world::spawn_player / spawn_remote_player); these functions are the orchestration above
// it. [orig: Server_InitNewRoundState @0x51c8e0 -> CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0
// -> Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0; net-re §5.2a/§5.2b]
namespace opennova::world {
class World;
struct Entity;
struct SpawnPointResult;
}
namespace opennova::replication {
class ISessionTransport;
}

namespace opennova::inmatch {

struct HostRotation;

// Re-export the canonical world::kRetailPlayerMinEntitySlot into np for the spawn call sites (one
// definition, shared with the Godot listen host). [orig: §5.2b spawn placement]
inline constexpr uint16_t kRetailPlayerMinEntitySlot = world::kRetailPlayerMinEntitySlot;

// Reserve the team written to both the pre-spawn S2C 0x04 slot assignment and
// the later player entity. Repeated calls for one connection are idempotent.
// The balance tally reads the slot rows (each claiming connection's team byte),
// never the bodies, so admissions received in one socket drain all count.
// The side arms (a side password, a TeamChoose request) return the team the
// rotation's side-to-team map gives that side (null: sides 1, 2), which the
// Attack-and-Defend swap exchanges between the halves.
// [orig: Server_AssignPlayerTeam @0x4fe310 writes playerSlot+416 before
// NetPacket_WriteSlotAssignment @0x502b30 reads it; g_RotationSideTeam0 / g_RotationSideTeam1
// @0x4FE408, @0x4FE456, @0x4FE492, @0x4FE507, @0x4FE543, @0x4FE56D]
// The balance tally counts every active slot's team but the joiner's, so a
// Serve Only host's own slot 0 (`host_slot`, its team 1 from the session
// create) counts there too (D-NET-352), a loading joiner counts, and a session
// spectator's team 0 counts nowhere (D-NET-NEW-L3-6). [orig: @0x4FE5BD..0x4FE5DE]
uint8_t Server_ReservePlayerTeam(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		const HostRotation *rotation = nullptr, const HostSlotRow *host_slot = nullptr);

// Reserve a free fixed roster-table row before S2C 0x04 advertises it: of the
// inactive rows below `slot_capacity`, the one free the longest by
// `free_ticks` (signed, the lowest index on a tie; null counts all zero), a
// remote never row 0; the reservation zeroes the row's count, as the add's
// memset does. Existing player bindings and other pre-spawn reservations both
// occupy rows; repeated calls for one connection return the same row.
// Player-add consumes the reservation, while erasing the connection releases
// it (D-NET-351).
// [orig: Server_PlayerAdd, the call @0x51CC95 -> PlayerSlot_PickFreeSlot @0x4FD7B0
//  (@0x4FD7D0..0x4FD7EE), the memset @0x51CD06; NetPacket_WriteSlotAssignment
//  @0x502B30 reads the row]
std::optional<uint8_t> Server_ReservePlayerSlot(
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		uint32_t slot_capacity, PlayerRowFreeTicks *free_ticks = nullptr);

// The server tick's tail walk's arm for an inactive row: its free-tick count
// steps once, saturating at 0x7FFFFFFF (D-NET-351). A row is active while a
// connection claims it, or (row 0) while a Serve Only host's own row is.
// [orig: Server_TickUpdate - `cmp byte ptr [esi+4], 0; jz` @0x51E3A0..0x51E3A4,
//  @0x51E457..0x51E467]
void Server_AdvanceFreeRowTicks(NapiNPServerCtx &ctx);

// The session create's profile init of the listen host's own slot class (+0x15EDC):
// side A's profile class (g_CharClassTeam1, the own connection's char_vars.char_class[0])
// when the side requirement (g_SideRequirement, the cfg's side_req: GameConfig::
// host_side_request) is 0, else side B's, stored raw. The own add stores no class
// (its local arm): the create's profile init follows it, so ours runs this in the
// own add's first arm, after its PlayerList entry; its own accepts store theirs.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 - `cmp g_SideRequirement, ebx`
//  @0x4C9952, g_CharClassTeam1 @0x4C995A / g_CharClassTeam2 @0x4C9966, the profile init
//  call @0x4C9CCE; PlayerSession_InitFromProfile @0x50CA80 - the store `mov [esi+15EDCh],
//  edx` @0x50CB7C; Game_ApplySessionSettingsToGlobals: g_SideRequirement from
//  sideRequirement_424 @0x551B91..0x551BA2]
void PlayerSession_InitOwnSlotClass(const NapiNPServerCtx &ctx, NapiNPConnection &own);

// The same profile init's team for the listen host's own slot (+0x1A0), which overwrites the
// round init's assignment after the own add wrote its PlayerList entry from that one: the net
// config's side byte (GameConfig::host_side_request) when it is 1 or 2, else 1 for a slot whose
// spectator latch is clear and 0 for one that is set. Stored into the connection and returned.
// [orig: PlayerSession_InitFromProfile @0x50CA80 - the side byte against 1 and 2
//  @0x50CA92..0x50CAA7, `cmp [esi+188D7h], al; setz al` @0x50CAA9..0x50CAB8, the store
//  `mov [esi+1A0h], cl` @0x50CB72; its argument `movsx edx, byte [ebp+107Ch]` @0x4C9C9A]
uint8_t PlayerSession_InitOwnSlotTeam(const NapiNPServerCtx &ctx, NapiNPConnection &own);

// The session create's profile init of a Serve Only host's own row (HostSlotRow): the side
// bytes the host's player profile carries (its char ids and avatars) and the class by the side
// requirement, as PlayerSession_InitOwnSlotClass picks a listen host's (side A's profile class
// when the cfg's side_req is 0, else side B's, stored raw). Retail's game-launched Serve Only
// host reads the player profile it runs under; a host with no profile of its own
// (opennova-serve) passes a fresh record's, which the round init already left. Nothing for an
// inactive row. [orig: CNapiGameSession_CreateSession @0x4C97C0 - a non-peer jumps into the
//  profile init @0x4C9C9A..0x4C9CCE, the class @0x4C9952..0x4C996C; PlayerSession_InitFromProfile
//  @0x50CA80 - the side ids +0x1B8 / +0x1BC @0x50CB67..0x50CB8F, the avatars
//  @0x50CB96..0x50CBA2, the class @0x50CB7C; their source Game_ApplySessionSettingsToGlobals
//  @0x551500 over the current profile]
void PlayerSession_InitHostRowFromProfile(NapiNPServerCtx &ctx, const CharacterJoinVars &vars);

// Player_InitPlayer's per-team re-stamp of the local body (the mission start's
// g_LocalPlayerEntity: a listen host's or the single player's own body, a Serve Only host's
// row body): its class word from the session start's profile class of the body team's side
// and its animSlot from that side's avatar byte, side A's for a team byte of 1 or 3, else side
// B's, with no game-type test. The two words are the host's own slot's: a listen host's own
// connection's char_vars (the profile's, which in a session the start clamped into 5..9 and
// every weapon-slot list sent to the own slot rewrites with the slot's class,
// weapon_slot_list_own_legs), a Serve Only host's row's (PlayerSession_InitHostRowFromProfile).
// Nothing when the host has neither.
// [orig: Player_InitPlayer @0x4E15F0 - the team byte +0x162 against 1 and 3
//  @0x4E1809..0x4E1817, g_CharClassTeam2 / g_AvatarTeam2 @0x4E1819..0x4E1825,
//  g_CharClassTeam1 / g_AvatarTeam1 @0x4E182C..0x4E1838, the stores +0x294 @0x4E181F /
//  @0x4E1832 and +0x374 @0x4E1843; the words' session start Game_ApplySessionSettingsToGlobals
//  @0x5516D0..0x5516FD; Server_SendWeaponSlotListToPlayer @0x502629..0x50264B]
void Player_InitPlayerSideWords(const NapiNPServerCtx &ctx, world::Entity &body);

// A Serve Only host's own slot 0's body (HostSlotRow::body), the round init's spawn of the
// row at every mission start, first in slot order: the slot's class (+0x15EDC) and team, the
// side's character id and avatar, owner connection 0, a spawn marker as a joiner's add takes
// one; it binds as the host's local player (its owner connection, 0, is what
// NapiNP_GetLocalConnectionId returns with no client connection) and the mission start hides
// it, which no 0x0A writer ever clears for a row with no net player, so every joiner receives a
// hidden organic owned by connection 0. Nothing for an inactive row (a listen host's) or off the
// authority. Returns the body (invalid when pool 0 is full).
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - the active test @0x516AE0..0x516AE4,
//  Entity_SpawnFromAnimSlotProperty(slot, +0x1B8, +0x15EDC) @0x516B56..0x516B67, the owner
//  +0x18 @0x516B6C..0x516B72, the team @0x516B78..0x516B7E, the animSlot @0x516B84..0x516B8E;
//  NapiNP_GetLocalConnectionId @0x4C6D40..0x4C6D4A; Game_StartMission's hide of
//  g_LocalPlayerEntity @0x525C95..0x525C9E]
world::EntityHandle Server_SpawnHostSlotBody(NapiNPServerCtx &ctx, world::World &world);

// §5.2a step 1 — [orig: Server_InitNewRoundState @0x51c8e0]. Set up the local-player/round context
// for an authority host. Structural: clears the loading-progress counter for a fresh round (the
// timeout gate, NOT the spawn-success gate, which the per-frame 0x0A drops, §5.2a step 4). No-op
// unless ctx.is_authority.
void Server_InitNewRoundState(NapiNPServerCtx &ctx);

// §5.2a step 2 — [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]. Gated
// is_authority && !world.match.outcome().ended. Walks connection_list; for each accepted-but-unspawned node
// (self_id_seen && phase < PlayerAdded) builds + registers its pool-0 player entity in `world`. The
// host's own type-2 loopback is just another entry in this list. Returns the number spawned this pass.
int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World &world);

// The C2S 0x0B mission-file report, the join's one player-path trigger: from an added slot
// whose net player is not yet in game state 10 and whose entity exists it runs the join
// (Server_OnPlayerJoin) with the body's two map checksums, whose game-start messages go out
// on the connection's transport in order, a checksum punt staged at its place (into
// `replies` for a transport-less test seam, which stages none); any other 0x0B is dropped.
// The body: the side password's C string, then three u32 (two checksums, the version), each
// 0 where the body ends first. (D-NET-678, D-NET-502, D-NET-NEW-L1-5)
// [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10 - the authority @0x51AB10..0x51AB17
//  (the dispatcher's), the player context @0x51AB21..0x51AB29, the slot
//  @0x51AB30..0x51AB38, game state 10 @0x51AB3E..0x51AB45, the slot's entity
//  @0x51AB47..0x51AB4B; the string's end @0x51AB5A..0x51AB73, the checksums
//  @0x51AB7A..0x51AB91; the Server_OnPlayerJoin calls @0x51ABA1 / @0x51ABB5]
void Server_HandlePlayerJoinRequest(NapiNPServerCtx *ctx, NapiNPConnection &conn,
		world::World *world, uint32_t now_tick, const std::vector<uint8_t> &body,
		std::vector<ProtocolMessage> &replies);

// §5.2a step 2 (per player) — [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd
// @0x51cbc0]. Build the spawn pose from the mission's promoted start markers (§5.2c,
// world::resolve_player_spawn_pose — never an NPC's spot) and register the pool-0 0x14B9 entity, stamping
// entity+0x78 = conn.connection_id. The type-2 loopback (the host's own client) -> world::spawn_player
// (publishes World::cached.local_player); a type-1 remote joiner -> world::spawn_remote_player (the
// host never republishes its local player). Binds the handle onto conn.link.owned_entity and advances
// conn.phase = PlayerAdded. Returns the spawned handle. The row is added whenever a free row
// exists (conn.phase reaches PlayerAdded): a body that does not spawn (pool 0 full) leaves it
// bodyless with an invalid handle (D-NET-NEW-L1-10); no free row adds nothing and returns
// invalid. `round_init` is the map change's re-add of a kept slot
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0]: a remote slot spawns with the slot's
// class (NapiNPConnection::slot_class), where the pump's add picks the join's and stores it.
world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world, bool round_init);

// The player add's PlayerList entry (NapiNPConnection::player_list_team /
// player_list_type): the bound entity's signed team byte and its class, or with
// no body the slot's team byte and class. The
// spawn pump's add and the synthetic admit stamp it; the round init's re-add
// of a kept slot does not.
void stamp_player_list_add(NapiNPConnection &conn, const world::World &world);

// server_deploy.cpp defines the deploy, death, spectator and team-change legs
// declared below; admit_synthetic_peer stays with the add in server_spawn.cpp.

// Move an already-added authoritative player into retail spectator state (the
// convert's slot/entity mutation). The connection flag is serialized by S2C
// 0x75; the neutral/hidden entity keeps its roster identity while simulation
// continues. `spectator` false is refused: retail has no leave, a converted
// slot returns at the next mission start's per-slot reset (D-NET-407).
// Authority-only.
// [orig: Server_PlayerAdd @0x51cbc0; NapiNPClientMsg_SetSpectatorMode
// @0x4259e0; Entity_UpdateInfantryPlayerBody @0x4b40e0;
// Server_ResetAllPlayerSlotFrameState @0x4FC320]
bool Server_SetPlayerSpectator(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world, bool spectator);

// Retarget an entity's team on the authority. A no-op unless the team
// changes. A Player with a slot also gets its stats reset (field 35 = 1, the
// script var at +408 cleared), its score-sound cache and armory cooldown
// zeroed, its slot team set, its NetId / animSlot re-stamped from the new
// side's character vars (side A for teams 1/3 or a non-team game), its squad
// link cleared with S2C 0x71 [0xFF][slot] to its new team and 0x72 [0][""] /
// [1][""] to itself. In a session every in-match slot then gets S2C 0x50
// (the identity pair zero for a non-player), and the entity joins the
// team-change list the C2S 0x29 walk reads. The zone half of retail's callers
// is world::ZoneCapture's team change (its 0x50 rides the zone event fan).
// [orig: Server_ChangeEntityTeam @0x518D70 — gates @0x518D84..0x518DAA, team
//  @0x518DB7, Entity_ValidatePtr @0x518DC5, CPlayerStats_ResetAllArrays
//  @0x518DD6, slot stores @0x518DDB..0x518DEF, side pick @0x518E27..0x518E71,
//  NetId / animSlot @0x518E7E / @0x518E8C, Server_SendPlayerStateAndSquad
//  @0x518B40 (the call @0x518E92), +356 @0x518E9A, 0x50 @0x518EA5..0x518EE1,
//  CBufferList_AddOrFind @0x518EEC]
void Server_ChangeEntityTeam(NapiNPServerCtx &ctx, world::World &world,
		world::EntityHandle entity, uint8_t team);

// The retail kill-and-convert of a C2S 0x51, run inline in the receive pump
// by its handler: the slot's spectator latch and hide byte, its team cleared,
// the entity's command group cleared, the entity hidden with health 1 and
// damage disabled, its spawn-wave removal, the hold latch (the request named
// no target), the deploy leg (Server_ReleasePlayerDeployment) over the
// request's handle, its private sends queued on the slot's connection at their
// call (returned instead to a transport-less fixture), then S2C 0x32 [5][name]
// to every active player (D-NET-686).
// [orig: Server_ConvertPlayerToSpectator
//  @0x519E00: gates @0x519E16..0x519E3F, the convert @0x519E5B..0x519EA4,
//  SpawnWaveList_RemovePlayer @0x519EB3, the latch @0x519EBC..0x519EC8,
//  Server_DeployPlayer @0x519ECE, 0x32 @0x519EDC..0x519F43; called from
//  Server_ProcessClientRequestSpectatorRespawn @0x51C8D5]
std::vector<ProtocolMessage> Server_ConvertPlayerToSpectator(NapiNPServerCtx &ctx,
		NapiNPConnection &conn, world::World &world, uint16_t request);


// The deploy leg's placement, between its raise and its spawn-state reset:
// the convert's hold arm (conn.reply.convert_holds_pose) places nothing, the
// body keeping its position with its three angle words zeroed; any other
// deploy runs Server_PositionPlayerForSpawn over the target, resolved again
// against the player entity's team (a miss takes the no-pick arm; the
// spectator latch's team substitute, a medic revive's saved position, else
// the previous reset's position when nothing was placed). Writes the player's pose and
// returns the placement whose heading word and latches the deploy reads on.
// [orig: Server_DeployPlayer @0x517837..0x517868]
world::SpawnPointResult Server_PositionDeployingPlayer(const GameConfig &config,
		NapiNPConnection &conn, world::World &world, world::Entity &player,
		world::EntityHandle target_zone);

// The deploy's spawn protection (entity+292) for a non-bot slot without the revive latch: 620
// ticks; dfx2.exe's 310 when the deploy targets a vehicle (ItemDef type 1) or a spawn-point item
// (attrib 0x40000), 620 otherwise (docs/titles/dfx2-vs-jo-re.md DFX2-17). `title` is a
// gameprofile::GameId, `target` the deploy's target entity or null.
// [orig: Server_DeployPlayer @0x517740 — 620 @0x517937 (type 1) / @0x517952 (0x40000) /
//  @0x517960] [orig: Server_DeployPlayer @0x514990 — 310 @0x514b78 / @0x514b93, 620 @0x514ba1
//  (dfx2.exe)]
int32_t deploy_spawn_protection_ticks(int title, const world::Entity *target);

// Synthetic in-process peer admit WITHOUT a handshake — an owner/test hook (the Godot binding's
// admit_test_remote_peer). Spawns a pool-0 REMOTE player at `spawn` (net_id forced to 0 — a player
// carries no SSN, D-NET-112; identity is handle + ownerConnectionId), registers (or reuses) a type-1
// connection for `peer` already in-match (burst.spawned), and
// binds conn.link.owned_entity + the supplied NON-OWNING transport. Mirrors the post-PeerSpawned state
// the handshake pipeline leaves, minus the socket legs; NEVER publishes World::cached.local_player
// (spawn_remote_player). Returns the spawned handle (invalid if pool 0 is full / no AiSystem). The real
// path is handle_server_datagram -> Server_ProcessPendingPlayerSpawns.
world::EntityHandle admit_synthetic_peer(NapiNPServerCtx &ctx, world::World &world, const PeerAddr &peer,
                                         const world::PlayerSpawn &spawn,
                                         replication::ISessionTransport *transport);

// The Respawn key's request (C2S 0x41, no body; the IDB's name is a misnomer),
// on the authority. For a sender whose player slot has an entity it zeroes the
// entity's Health word and its last attacker (+0x178), stores the configured
// respawn timeout itself, unfloored, into the slot's respawn delay (+360) and
// spawn-target hold (+364), and closes the revive window (+368). Nothing is
// sent and nothing is queued: Health 0 is the player body's death edge in the
// entity update after this frame's tick, for an unhidden body whose dead bit is
// clear, which on the authority raises the player-death transaction over these
// holds (infantry_death.cpp), so the frame's periodic decrement and 0x0A fan
// see the body undead at Health 0.
// [orig: NapiNPServerMsg_DeathTimeoutReset @0x510540 (the C2S table's 0x41 row
//  @0x82B948) -- the authority @0x510542, the connection @0x51054E, its slot
//  @0x510558, the entity @0x510562, +0x11E @0x510568, +0x178 @0x51056F,
//  g_RespawnTimeout into +360 @0x51057B and +364 @0x510587, +368 @0x51058D;
//  the edge Entity_UpdateInfantryPlayerBody @0x4B40E0 (the hidden bit
//  @0x4B411E..0x4B4127, Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE, the
//  authority's Entity_CheckAndProcessDeath call @0x4B4CEA)]
void Server_HandleDeathTimeoutReset(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world);

} // namespace opennova::inmatch
