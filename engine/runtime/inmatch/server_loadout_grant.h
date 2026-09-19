// The C2S 0x2F loadout grant — the accept half of NapiNPServerMsg_HandlePlayerLoadout:
// the submission envelope, the granted-kit resolve over the host's armory table (the S2C
// 0x5A body, the authority ammo pools, the per-ammo damage classes, the rebuilt host-side
// slot rows), and the current-slot-list re-send body every abort and the deploy release
// answer with. Split out of server_message_dispatch.cpp, whose 0x2F case and
// Server_ReleasePlayerDeployment are the callers; internal to the inmatch group.
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <net/npwire/ingame_decode.h> // LoadoutSubmit / WeaponLoadout

#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::world {
class World;
struct WeaponTable;
}

namespace opennova::inmatch {

// tag=0x5A WEAPON-LOADOUT-SYNC, built from the joiner's own C2S 0x2F loadout submit.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 — parses the request, validates the
// envelope (loadout_envelope_accepted), clamps the in-range soldier type to [5,9]-else-8
// (@0x515913), stamps entity+660 playerClass (@0x515ab0), loads the entries
// into the player's 780-slot weapon table, then Server_SendWeaponSlotListToPlayer @0x502550
// walks that table ascending by weapon-slot combo (category*65 + rank, @0x5026e5..@0x5028a0),
// filtering each slot by the team/char masks (@0x502716) and emitting one 4-byte group:
// [admIdx = AvatarDef_FindIndexByName @0x50273b][ammoPrimary = WeaponSlot_GetTotalClips
// @0x502794][ammoSecondary = the same count for the first different-ammoclass sub-variant in
// parent+1..parent+LSC (@0x5027c8), else 0xFF][per-ammo damage class from
// player+89688: 1 = x0.9, 2 = x1.1, other/default = 0].]
//
// With the armory table fed (world::World::weapons), the reply resolves REAL counts through the
// witnessed rules (weapon_table_build: loadout_entry_permitted / resolve_loadout_ammo) — the
// golden ASH_I5A reply {2:255, 3:10, 21:10, 76:1, 77:2, 78:3, 83:3} reproduces from the host's
// own resolved weapon.def (D-NET-141). Table-less hosts (unit paths / no resource root) keep the
// prior request-echo: the client clamps echoed bytes on apply [orig: @0x4295d7-0x4295e9], a
// tracked divergence for that configuration only. The accepted fourth byte is the
// player+89688 per-ammo damage class; captured 0xFF defaults normalize to 0.
struct GrantedWeaponLoadout {
	WeaponLoadout reply;
	// The serverPlayer+88664 authority pool image copied by S2C 0x0F. Each
	// accepted request writes its ammo class in wire order, so a later weapon
	// sharing that class wins exactly as retail does.
	std::array<int32_t, 128> ammo_pools{};
    // Loaded-round buckets [orig: player+89176; sub_540670 @0x540670].
    std::array<int32_t, 128> shared_clips{};
	// Final player+89688 values, keyed by the resolved AmmoDef index. The retail
	// request walk overwrites this table in request order, so the last accepted
	// weapon using an ammo type controls every granted slot that uses that ammo.
	std::vector<std::pair<int16_t, uint8_t>> ammo_damage_classes;
	uint32_t carry_flags = 0;
	// The host-side 780-row weapon-slot table this accept rebuilds, one row per
	// granted combo with the clip the rebuild drew (rounds) — the rows the C2S
	// 0x06 clip check / 0x25 relay refill and the 1 Hz kit-weight recompute
	// read. [orig: playerSlot+464 + 100*combo, slot+16 loaded rounds after
	//  WeaponSlots_RecalculateAmmoFromCapacity @0x515db5..0x515f4d]
	struct Row {
		uint16_t combo = 0;
		uint8_t adm_index = 0;
		int16_t clip = 0;
	};
	std::vector<Row> rows;
};


// The 0x2F submission envelope, checked BEFORE anything is applied (team 1..4, above 4 only
// in a team-less game type; a NONZERO class 5..9). See the definition for the witness.
bool loadout_envelope_accepted(const LoadoutSubmit &req, uint32_t game_type);

// Resolve the accepted submission into the granted kit: the S2C 0x5A reply, the authority
// ammo pool image, the per-ammo damage classes, the carry flags and the rebuilt slot rows.
// A null/empty `table` is the resource-less request-echo fallback.
GrantedWeaponLoadout grant_weapon_loadout(const LoadoutSubmit &req,
										  uint16_t class_allow_mask,
										  const world::WeaponTable *table);

// The player's live soldier class (entity+660), the header byte a current-slot-list
// re-send carries.
uint8_t current_player_class(const NapiNPConnection &conn, const world::World *world);

// The tag=0x5A body for every re-send that grants NOTHING new: the retained granted body
// when one exists, else the empty-table shape headed by the player's live class.
std::vector<uint8_t> build_current_loadout_reply(const std::vector<uint8_t> &retained,
                                                 uint8_t player_class,
                                                 const world::WeaponTable *armory);

} // namespace opennova::inmatch
