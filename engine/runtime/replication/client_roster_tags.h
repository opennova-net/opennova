#pragma once

// The joiner's PLAYER walk of the friendly-tags pass (D-HUD-20 residue a).
// Retail's second walk visits the player-slot table S2C 0x4C fills and labels
// each slot's entity [orig: HUD_DrawFriendlyTagsPass @0x5a4507..0x5a4597]; on a
// joiner the pool-0 players are decoded ClientEntityState rows, not World
// twins (ClientWorldMaterializer excludes pool 0), so this gather is the only
// way a remote player reaches the drawer there. The HostClient role reaches
// the same rows through world::collect_friendly_tags' slot walk over its
// authored World.

#include <runtime/replication/client_state.h>

#include <runtime/world/friendly_tags.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::replication {

// The def hit-point lookup for a decoded row's type, the joiner's item-table
// stand-in for entity->itemDef + Entity_GetMaxHealthWithDifficulty. 0 = NO
// DEF for that type, which takes the drawer's entry bail [orig: itemDef ==
// NULL @0x5a39fb]; a null callback resolves every row (max 1, the
// `if (!max) max = 1` fold @0x5a3b95).
using RosterTagMaxHealth = std::function<int32_t(uint16_t type_id)>;

// String_AppendN: append `src` while the total stays under `max_len` bytes
// with its terminator [orig: String_AppendN @0x617E50].
void string_append_n(std::string &dst, const std::string &src, size_t max_len);

// A player's tag label: the slot callsign, then "<ch>" registry tag "<co>"
// for a non-empty tag, within 64 bytes [orig: HUD_DrawEntityLabel
// @0x5a3f29..0x5a3f86 — Napi_CopyString(.., slot+0x14, 64), String_AppendN
// (.., 64) x3 over slot+0x20].
std::string roster_tag_label(const ClientRosterSlot &slot);

// Walk the S2C 0x4C slot table in table order [orig: HUD_DrawFriendlyTagsPass
// @0x5a4507..0x5a4597 over g_PlayerSlotPtrTable], each entry's roster slot
// with an entity (slot+0x0D active, slot+0x24 entity), and emit one tag
// source per player that passes the pass gates and the drawer's entry bails
// (world/friendly_tag_gates.h, shared with the authority walk): not self
// [orig: @0x5a39df], not CARRIED (state_flags & 1 @0x5a39eb), a resolved def
// [orig: @0x5a39fb], team 0 / local team / death screen
// [orig: @0x5a4552..0x5a456b], `g_GameType || death screen`
// [orig: @0x5a456d..0x5a457d]. Each source carries the slot's revive
// countdown and medic-request latch (slot+0x10 / slot+0x2C), the row's
// dead bit (state_flags & 2 — the `Flags & 2` latch @0x5a3c1c) and the
// radio-request fold: the row's +885 latch (receive event 0x6D) cleared by
// a def-type-1 carrier in the groundEntity walk [orig: @0x5a3bfe..0x5a3c1a].
// The walk runs over `carrier_world`'s materialized twins (the joiner's
// pool-1..3 rows, keyed by the wire handle) from the decoded row's
// carrier_handle — the remote's +0x28 as the player compact echoes it
// (mount wins over ground @0x4c0a08); null = no twins, the latch alone.
void collect_roster_tags(const ClientState &state, uint16_t self_handle,
                         uint8_t local_team, bool death_screen,
                         uint32_t game_type,
                         std::vector<world::FriendlyTagSource> &out,
                         const RosterTagMaxHealth &max_health,
                         const world::World *carrier_world);

} // namespace opennova::replication
