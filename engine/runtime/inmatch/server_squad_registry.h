#pragma once

// THE AUTHORITY'S NOVAWORLD SQUAD REGISTRY: one node per squad id a joined
// account carries (NapiNPServerCtx::squad_registry, the list at 0x24E0E54
// inside the curated `g_SpawnWaveList`), keyed from each player slot by the
// slot's squad key (+0x188DC, NapiNPConnection::squad_key). The player add
// files a node for a remote joiner whose account carries a squad id and
// announces a squad's first slot; the disconnect of a squad's last slot
// announces it gone and frees its node; the map change keeps both the nodes
// and the slots' keys; a session reset clears the registry. Its readers are
// the C2S 0x4E walk (server_message_dispatch.cpp), the chat line's tag suffix
// and channel 12 (server_chat.cpp) and the 0x56 row's third string.
// [orig: Server_PlayerAdd @0x51CBC0 (@0x51CDE0..0x51CEEF, @0x51D182..0x51D20E);
//  Server_HandlePlayerDisconnect @0x51B5C0 (@0x51B6D8..0x51B7FD);
//  Server_DisconnectAndResetAllPlayerSlots @0x516160 (keeps an active slot's
//  +0x188DC); Server_ReleaseSessionTables @0x51B500]

#include <cstdint>
#include <string>

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::inmatch {

// The first node whose id equals `squad_id`, null when none.
// [orig: CLinkedList_FindByTag @0x52B200 - the active list's head (+0x14),
//  next (+8), id (+12)]
const SquadRegistryNode *find_squad_registry_node(const NapiNPServerCtx &ctx, int32_t squad_id);

// The active player slots whose squad key is `key`; 0 for a key of 0.
// [orig: CSlotArray152_CountActiveByKey @0x500660 - the key test @0x500669,
//  the active byte (+4) @0x500690, the key (+0x188DC) @0x500699]
int32_t count_active_slots_by_squad_key(const NapiNPServerCtx &ctx, int32_t key);

// The player add's squad leg, on a remote add (the local arm skips it, and
// the map change's round init re-adds no slot through it): the slot's key
// clears; an account with a nonzero squad id finds or creates its node (the
// account's name and tag copied over the node's, 64 and 8 chars), the slot
// takes the node's id as its key, and when the slot is the squad's only
// active one, S2C 0x6A {1, id, name, tag} goes reliable on send mask 0x1010
// (the added slot's own copy staged for the spawn pump's metadata). Call it
// once the slot is active.
// [orig: Server_PlayerAdd - the clear @0x51CDE0, the net-player and squad-id
//  tests @0x51CDE6..0x51CDF8, CLinkedList_FindOrCreateByNetId @0x51CE16
//  (@0x52B540: the copies @0x52B572 / @0x52B58D), the key @0x51CE2E, the count
//  @0x51CE34..0x51CE3C, the 0x6A @0x51CE42..0x51CEEF]
void Server_FileSquadRegistryNode(NapiNPServerCtx &ctx, NapiNPConnection &conn);

// The add's squadron line: for a slot whose key is set and which is its
// squad's only active slot, S2C 0x32 {3, the node's name} goes reliable on
// send mask 0x80, ahead of the add's join line.
// [orig: Server_PlayerAdd @0x51D182..0x51D20E, ahead of the 0x32 type 1
//  @0x51D21E..0x51D291]
void Server_AnnounceSquadronEntered(NapiNPServerCtx &ctx, const NapiNPConnection &conn);

// The disconnect's squad leg, after its leave line and with the leaver still
// active: with its key set and at most one active slot of the squad, S2C 0x32
// {4, the node's name} on send mask 0x80, then S2C 0x6A {2, id} on send mask
// 0x1010 and the node freed; the slot's key clears either way.
// [orig: Server_HandlePlayerDisconnect @0x51B6D8..0x51B781 (the 0x32),
//  @0x51B786..0x51B7EB (the 0x6A), COverlayList_ReleaseNode @0x51B7F5, the
//  clear @0x51B7FD]
void Server_ReleaseSquadRegistryNode(NapiNPServerCtx &ctx, NapiNPConnection &leaver);

// The tag of the node the active slot `player_slot` keys, empty when none:
// the end-of-round row's third string (the caller caps it at 15 chars).
// [orig: Server_BuildEndOfRoundScoreboard @0x5090E8..0x509116]
std::string squad_registry_tag_for_slot(const NapiNPServerCtx &ctx, uint8_t player_slot);

} // namespace opennova::inmatch
