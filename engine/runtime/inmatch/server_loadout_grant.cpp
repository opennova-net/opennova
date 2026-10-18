#include <runtime/inmatch/server_loadout_grant.h>

#include <net/npwire/ingame_encode.h>          // encode_weapon_loadout (the 0x5A body)
#include <runtime/world/weapon_inventory.h>    // the slot table's fill, pools and clips
#include <runtime/world/weapon_table_build.h>  // loadout_entry_permitted (D-NET-141)
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// One accepted kit entry: its weapon, its two counts, each read through a signed char
// (so a count byte of 0x80..0xFF is negative and takes the start rounds), and its raw
// fourth byte. [orig: NapiNPServerMsg_HandlePlayerLoadout - the reads
//  @0x5159DD..0x515A0F, the kept entry @0x515A5E..0x515A77; the loadout buffer keeps
//  the counts as "%d" text @0x515D00 / @0x515D34, which PlayerSlot_InitWeaponsFromLoadout
//  reads back with atol @0x51564C / @0x515674]
struct KitEntry {
	uint8_t adm_index = 0;
	const world::WeaponTableEntry *def = nullptr;
	int32_t count = -1;
	int32_t count2 = -1;
	uint8_t variant = 0;
};

// The entries the masks admit, in request order: the slot's loadout buffer (the map
// availability, D-NET-693, is not ported). [orig: @0x515A17..0x515A58]
std::vector<KitEntry> admitted_entries(const LoadoutSubmit &req, uint8_t avatar_class,
                                       const world::WeaponTable &table) {
	std::vector<KitEntry> out;
	for (const LoadoutSubmitEntry &e : req.entries) {
		const world::WeaponTableEntry *def = table.by_index(e.adm_index);
		if (def == nullptr) continue; // the AdmDef_GetEntryByIndex fail leg
		if (!world::loadout_entry_permitted(*def, req.team, avatar_class)) continue;
		KitEntry k;
		k.adm_index = e.adm_index;
		k.def = def;
		k.count = static_cast<int8_t>(e.ammo_primary);
		k.count2 = static_cast<int8_t>(e.ammo_secondary);
		k.variant = e.variant;
		out.push_back(k);
	}
	return out;
}

// The slot's display list from its buffer: each kit weapon by name, then its
// loadout_subclasses sub-variants (the entries after it that exist), the semi-mode and
// launcher weapons a stock client fires through.
// [orig: AvatarDef_BuildDisplayList @0x54B9E0, called @0x515DA6]
std::vector<std::string> kit_display_list(const std::vector<KitEntry> &entries,
                                          const world::WeaponTable &table) {
	std::vector<world::WeaponKitEntry> kit;
	for (const KitEntry &e : entries) {
		world::WeaponKitEntry k;
		k.name = e.def->name;
		kit.push_back(k);
	}
	return world::weapon_kit_expand_display_list(kit, table);
}

// The table the rebuild starts over: the slot's ammo pools and loaded-round buckets as
// they stand, its slots emptied by the fill; the first seed zeroes every pool, and only
// the revive latch keeps the buckets. [orig: WeaponSlotPool_ResetAllEntries @0x53F240
//  clears the 780 slots alone]
world::WeaponInventory slot_inventory(const world::WeaponTable &table,
                                      const NapiNPConnection &conn) {
	world::WeaponInventory inv;
	inv.reset(table);
	const size_t pools = std::min(inv.pools.size(), conn.reply.ammo_pools.size());
	for (size_t i = 0; i < pools; ++i) inv.pools[i] = conn.reply.ammo_pools[i];
	const size_t buckets = std::min(inv.shared_clips.size(), conn.reply.shared_clips.size());
	for (size_t i = 0; i < buckets; ++i) inv.shared_clips[i] = conn.reply.shared_clips[i];
	return inv;
}

// The 780 slots reset and filled from a display list, the first weapon on a combo
// keeping it. [orig: WeaponSlotPool_ResetAllEntries @0x53F240 +
//  WeaponSlotTable_LoadAllFromDefs @0x5414E0, the incumbent kept @0x5415D6]
// The fill's sniper lock reads the session's zoom rule [orig: WeaponSlot_InitFromDef
//  @0x53EF07..0x53EF25 - byte_A821F0 in a session on a peer, else the config's
//  allowSniperScopeZoom, the same value on a host].
void reload_slots(const world::WeaponTable &table, world::WeaponInventory &inv,
                  const std::vector<std::string> &display, uint8_t player_class,
                  bool allow_sniper_scope_zoom) {
	inv.slots.fill(world::WeaponInventorySlot{});
	world::weapon_inventory_load_from_display(table, display, inv, player_class,
			allow_sniper_scope_zoom);
}

// The display list rebuilt from the table: every populated slot's weapon, in combo
// order. [orig: PlayerSlot_BuildNameTable @0x4FE200]
std::vector<std::string> slot_name_table(const world::WeaponTable &table,
                                         const world::WeaponInventory &inv) {
	std::vector<std::string> names;
	for (int32_t combo = 0; combo < world::weapon_combo::kSlotCount; ++combo) {
		const world::WeaponInventorySlot *slot = inv.slot(combo);
		if (slot == nullptr || slot->adm_index < 0) continue;
		const world::WeaponTableEntry *def =
				table.by_index(static_cast<uint8_t>(slot->adm_index));
		if (def != nullptr) names.push_back(def->name);
	}
	return names;
}

// Each listed weapon's slot initialized again and its loaded-round bucket zeroed,
// which a controller or UseGun seat skips.
// [orig: PlayerSlot_ReinitListedSlots @0x4FE260 - WeaponSlot_InitFromDef, WeaponSlot_SetSharedLoadedRounds(def+0xDC, 0), its
//  seat test @0x540684..0x540692]
void reinit_listed_slots(const world::WeaponTable &table, world::WeaponInventory &inv,
                         const std::vector<std::string> &display, uint8_t player_class,
                         const world::WeaponAmmoSeat &seat, bool allow_sniper_scope_zoom) {
	for (const std::string &name : display) {
		const int adm = table.index_of(name.c_str());
		const world::WeaponTableEntry *def =
				adm >= 0 ? table.by_index(static_cast<uint8_t>(adm)) : nullptr;
		if (def == nullptr) continue;
		world::WeaponInventorySlot *slot =
				inv.slot(def->rank + world::weapon_combo::kRanksPerCategory * def->category);
		if (slot == nullptr) continue;
		world::weapon_slot_init_from_def(*def, static_cast<int16_t>(adm), *slot, player_class,
				allow_sniper_scope_zoom);
		const uint32_t bucket = static_cast<uint32_t>(def->ammo_bucket);
		if (world::weapon_seat_writes_buckets(seat) && bucket < inv.shared_clips.size())
			inv.shared_clips[bucket] = 0;
	}
}

// Outside the revive latch the pools are seeded from the defs under the slot's class
// and every loaded-round bucket zeroed; then one clip is drawn.
// [orig: PlayerSlot_SeedAmmoPoolsAndRecalc @0x4FDA60 - the +89932 test, the seed,
//  the 0x200-byte memset of +89176, the recalc]
void seed_and_recalc(const world::WeaponTable &table, world::WeaponInventory &inv,
                     uint8_t player_class, bool revive_latch, const world::WeaponAmmoSeat &seat) {
	if (!revive_latch) {
		world::weapon_inventory_seed_pools(table, inv, player_class);
		std::fill(inv.shared_clips.begin(), inv.shared_clips.end(), 0);
	}
	world::weapon_inventory_recalc_clips(table, inv, seat);
}

// Every populated slot whose def draws pool units has its loaded rounds zeroed (the
// slot word, or its def's bucket, which a controller seat does not write), unless the
// entity sits a UseGun seat (+0x168 3).
// [orig: WeaponOverlay_UpdateAllPerFrame @0x5423C0 (a misnomer) - the seat test
//  @0x5423C5, the def[56] gate @0x5423E6, WeaponSlot_SetSharedLoadedRounds(bucket, 0) (its seat test
//  @0x540684..0x540692) or slot+16 = 0]
void clear_loaded_rounds(const world::WeaponTable &table, world::WeaponInventory &inv,
                         const world::WeaponAmmoSeat &seat) {
	if (seat.parent_slot == 3) return;
	for (int32_t combo = 0; combo < world::weapon_combo::kSlotCount; ++combo) {
		const world::WeaponInventorySlot *slot = inv.slot(combo);
		if (slot == nullptr || slot->adm_index < 0) continue;
		const world::WeaponTableEntry *def =
				table.by_index(static_cast<uint8_t>(slot->adm_index));
		if (def == nullptr || def->ammo_class_count == 0) continue;
		if (def->ammo_bucket != 0 && !world::weapon_seat_writes_buckets(seat)) continue;
		world::weapon_inventory_set_loaded_rounds(table, inv, combo, 0);
	}
}

// Each entry's pool set from its count, in entry order: min(count, maxclips) clips,
// else its start rounds; then each sub-variant of another ammo class its pool from the
// second count the same way. A negative second count takes the sub-variant's start
// rounds, which the accept multiplies by its clipsize when they are not negative and
// the deploy does not. [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515E58..0x515E86,
//  the sub-variants @0x515E99..0x515F33 (the multiply @0x515F04..0x515F09);
//  PlayerSlot_InitWeaponsFromLoadout @0x5156C1..0x5156EA, @0x5156EF..0x515752]
void set_kit_pools(const world::WeaponTable &table, world::WeaponInventory &inv,
                   const KitEntry &e, bool accept) {
	int32_t count = e.count;
	if (count > e.def->maxclips) count = e.def->maxclips;
	world::weapon_pool_set(table, inv, e.def->ammo_class_id,
			count >= 0 ? count * e.def->clipsize : e.def->startrounds);
	for (int32_t k = 1; k < e.def->loadout_subclasses + 1; ++k) {
		const world::WeaponTableEntry *sub =
				table.by_index(static_cast<uint8_t>(e.adm_index + k));
		if (sub == nullptr || sub->ammo_class_id == e.def->ammo_class_id) continue;
		int32_t count2 = e.count2;
		if (count2 > sub->maxclips) count2 = sub->maxclips;
		int32_t pool = 0;
		if (accept) {
			pool = count2;
			if (pool >= 0 || (pool = sub->startrounds) >= 0) pool *= sub->clipsize;
		} else {
			pool = count2 >= 0 ? count2 * sub->clipsize : sub->startrounds;
		}
		world::weapon_pool_set(table, inv, sub->ammo_class_id, pool);
	}
}

// The rebuilt table onto the grant: one row per populated slot with the rounds it
// holds, the pool and bucket images (S2C 0x0F's player+88664 / +89176), and the 0x5A
// rows. The list walks the 780 slots by combo under the slot's team (+416) and class
// (+89820); each slot the masks admit gives its weapon, its total clips (-1, the
// no-clip sentinel, is 0xFF), the total clips at the slot of the first sub-variant of
// another ammo class (0xFF for none) and, filled by apply_granted_loadout, its
// damage-class byte; then the walk skips as many slots as the weapon has sub-variants,
// so a sub-variant outside them is a row of its own.
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 - the masks @0x502666 /
//  @0x502693 / @0x502716, the weapon @0x502720..0x50273B, its clips @0x50276B /
//  @0x50277D, the sub-variant walk @0x5027B3..0x502847 (the class compare @0x5027F8,
//  its clips @0x502830 / @0x502842), the skip @0x50284E]
void finish_grant(const world::WeaponTable &table, const world::WeaponInventory &inv,
                  uint8_t team, GrantedWeaponLoadout &grant) {
	for (int32_t combo = 0; combo < world::weapon_combo::kSlotCount; ++combo) {
		const world::WeaponInventorySlot *slot = inv.slot(combo);
		if (slot == nullptr || slot->adm_index < 0) continue;
		GrantedWeaponLoadout::Row row;
		row.combo = static_cast<uint16_t>(combo);
		row.adm_index = static_cast<uint8_t>(slot->adm_index);
		row.clip = static_cast<int16_t>(slot->clip);
		grant.rows.push_back(row);
	}
	const size_t pools = std::min(grant.ammo_pools.size(), inv.pools.size());
	for (size_t i = 0; i < pools; ++i) grant.ammo_pools[i] = inv.pools[i];
	const size_t buckets = std::min(grant.shared_clips.size(), inv.shared_clips.size());
	for (size_t i = 0; i < buckets; ++i) grant.shared_clips[i] = inv.shared_clips[i];
	for (int32_t combo = 0; combo < world::weapon_combo::kSlotCount; ++combo) {
		const world::WeaponInventorySlot *slot = inv.slot(combo);
		if (slot == nullptr || slot->adm_index < 0) continue;
		const world::WeaponTableEntry *def =
				table.by_index(static_cast<uint8_t>(slot->adm_index));
		if (def == nullptr ||
		    !world::loadout_entry_permitted(*def, team, grant.reply.avatar_class))
			continue;
		WeaponLoadoutSlot row;
		row.type_id = static_cast<uint8_t>(slot->adm_index);
		row.ammo_primary =
				static_cast<uint8_t>(world::weapon_inventory_total_clips(table, inv, combo));
		row.ammo_secondary = 0xFF;
		for (int32_t k = 1; k < def->loadout_subclasses + 1; ++k) {
			const world::WeaponTableEntry *sub =
					table.by_index(static_cast<uint8_t>(slot->adm_index + k));
			if (sub == nullptr || sub->ammo_class_id == def->ammo_class_id) continue;
			const int32_t sub_combo =
					sub->rank + world::weapon_combo::kRanksPerCategory * sub->category;
			row.ammo_secondary = static_cast<uint8_t>(
					world::weapon_inventory_total_clips(table, inv, sub_combo));
			break;
		}
		grant.reply.slots.push_back(row);
		if (def->loadout_subclasses > 0) combo += def->loadout_subclasses;
	}
}

} // namespace

// The 0x2F submission envelope, checked BEFORE anything is applied: the team byte must be 1..4 —
// above 4 passes only in a team-less game type — and a NONZERO class byte must be 5..9. Both header
// bytes are read SIGNED, so 0x80..0xFF is negative and fails the low bound. A failing envelope
// aborts the handler: retail re-sends the player's current slot list and writes nothing.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x5158a9 (team) / @0x5158b1 -> @0x515fa5 (class).
// Retail additionally requires the armory timer player+356 to have expired before it accepts a
// nonzero class (@0x5158d0) — the dispatcher's armory-reuse gate models that cooldown.]
bool loadout_envelope_accepted(const LoadoutSubmit &req, uint32_t game_type) {
	const int8_t team = static_cast<int8_t>(req.team);
	if (team < 1 || (team > 4 && game_type != 0)) return false;
	const int8_t player_class = static_cast<int8_t>(req.player_class);
	return player_class == 0 || (player_class >= 5 && player_class <= 9);
}

GrantedWeaponLoadout grant_weapon_loadout(const LoadoutSubmit &req,
										  uint16_t class_allow_mask,
										  const world::WeaponTable *table,
										  const NapiNPConnection &conn,
										  const world::WeaponAmmoSeat &seat,
										  bool allow_sniper_scope_zoom) {
	const bool allow_zoom = allow_sniper_scope_zoom;
	GrantedWeaponLoadout grant;
	WeaponLoadout &reply = grant.reply;
	// Class 0 passes the envelope and applies with no fix-up: its soldier-type mask is 0, so
	// no request entry can match, and entity+660 is stamped 0 [orig: the `if (v4)` skip
	// @0x5158b1, type_mask default @0x5159af, the stamp @0x515ab0].
	reply.avatar_class = req.player_class;
	if (req.player_class != 0) {
		// Class accept: a valid but disabled request scans the whole Soldier Class range
		// from 5 and takes its first enabled bit. With no enabled bit the already-valid
		// request survives; the final 8 clamp only covers an invalid value
		// [orig: g_HostClassAllowMask @0x24D59FC, @0x5158d6..@0x515915].
		if ((class_allow_mask & (uint16_t{1} << reply.avatar_class)) == 0) {
			for (uint8_t candidate = 5; candidate <= 9; ++candidate) {
				if ((class_allow_mask & (uint16_t{1} << candidate)) != 0) {
					reply.avatar_class = candidate;
					break;
				}
			}
		}
		if (reply.avatar_class < 5 || reply.avatar_class > 9) reply.avatar_class = 8;
	}

	if (table != nullptr && !table->empty()) {
		const std::vector<KitEntry> entries = admitted_entries(req, reply.avatar_class, *table);
		const uint8_t cls = reply.avatar_class;
		// The slot's table from the accepted entries: the display list, a fill keeping
		// the first weapon on a combo, the pools seeded and one clip drawn; the list
		// rebuilt from the table; the full update (a second fill, every listed slot
		// initialized again with its bucket zeroed, the seed and the draw); every
		// loaded round cleared; the entries' pools set from their counts; and one clip
		// drawn again. [orig: NapiNPServerMsg_HandlePlayerLoadout -
		//  AvatarDef_BuildDisplayList @0x515DA6, WeaponSlotPool_ResetAllEntries @0x515DB5,
		//  WeaponSlotTable_LoadAllFromDefs @0x515DBF, WeaponSlots_SeedAmmoPoolsFromDefs
		//  @0x515DD3, WeaponSlots_RecalculateAmmoFromCapacity @0x515DE3,
		//  PlayerSlot_BuildNameTable @0x515DE9, PlayerSlot_FullUpdatePipeline @0x515DEF
		//  (@0x50BA60), WeaponOverlay_UpdateAllPerFrame @0x515DFF, the entry loop
		//  @0x515E0D..0x515F33, the recalc @0x515F4D]
		world::WeaponInventory inv = slot_inventory(*table, conn);
		reload_slots(*table, inv, kit_display_list(entries, *table), cls, allow_zoom);
		world::weapon_inventory_seed_pools(*table, inv, cls);
		world::weapon_inventory_recalc_clips(*table, inv, seat);
		std::vector<std::string> display = slot_name_table(*table, inv);
		reload_slots(*table, inv, display, cls, allow_zoom);
		display = slot_name_table(*table, inv);
		reinit_listed_slots(*table, inv, display, cls, seat, allow_zoom);
		seed_and_recalc(*table, inv, cls, conn.reply.revive_pose_valid, seat);
		clear_loaded_rounds(*table, inv, seat);
		for (const KitEntry &e : entries) {
			// The entity's carry bits from the accepted entries alone: the fills' own
			// were cleared @0x515DF6 [orig: @0x515E2C..0x515E54].
			if (e.def->flags & world::weapon_flag::kArmor) grant.carry_flags |= 8u;
			if (e.def->flags2 & 2) grant.carry_flags |= 0x10u;
			set_kit_pools(*table, inv, e, /*accept=*/true);
			// The accept's store, in request order: the raw fourth byte at the
			// weapon's round ammo index [orig: @0x515E8B..0x515E92].
			grant.damage_class_writes.emplace_back(e.def->ammo_index, e.variant);
		}
		world::weapon_inventory_recalc_clips(*table, inv, seat);
		grant.ammo_pools = conn.reply.ammo_pools;
		grant.shared_clips = conn.reply.shared_clips;
		finish_grant(*table, inv, conn.assigned_team, grant);
		return grant;
	}

	if (req.player_class == 0) return grant; // no entry matches the empty mask
	// Resource-less test/diagnostic fallback: no AmmoDef relationship exists, but duplicate
	// ADM slots still behave like a table load (last entry wins) and serialize only once.
	for (const LoadoutSubmitEntry &e : req.entries) {
		WeaponLoadoutSlot s;
		s.type_id = e.adm_index;
		s.ammo_primary = e.ammo_primary;     // echoed; client clamps on apply (@0x4295d7)
		s.ammo_secondary = e.ammo_secondary; // echoed; sub-slot clamp (@0x429652)
		auto existing = std::find_if(reply.slots.begin(), reply.slots.end(),
								 [type_id = s.type_id](const WeaponLoadoutSlot &slot) {
									 return slot.type_id == type_id;
								 });
		if (existing == reply.slots.end()) reply.slots.push_back(s);
		else *existing = s;
	}
	std::sort(reply.slots.begin(), reply.slots.end(),
	          [](const WeaponLoadoutSlot &a, const WeaponLoadoutSlot &b) {
		          return a.type_id < b.type_id;
	          }); // table-less fallback: ascending adm index (coincides for the golden kit)
	return grant;
}

// A store outside the table is dropped (D-NET-692): retail's raw index aliases the
// fields around it. A def with no resolved round keeps the default -1 [orig:
// AdmDef_InitEntryDefaults @0x53FF58], whose store lands on +0x15E57, the high byte of
// the loaded-round bucket 127; 128..131 land on the S2C 0x57 countdown dword +0x15ED8
// and 132..135 on the class dword +0x15EDC.
void write_ammo_damage_classes(NapiNPConnection &conn, const GrantedWeaponLoadout &grant) {
	for (const auto &write : grant.damage_class_writes) {
		if (write.first < 0 ||
		    static_cast<size_t>(write.first) >= conn.ammo_damage_classes.size())
			continue;
		conn.ammo_damage_classes[static_cast<size_t>(write.first)] = write.second;
	}
}

void apply_granted_loadout(NapiNPConnection &conn, const GrantedWeaponLoadout &grant,
                           bool rebuild_rows) {
	SessionReplyState &st = conn.reply;
	// Each row's fourth byte is the slot's damage-class byte at the weapon's OWN AdmDef
	// index, read when the list is built: so a weapon whose index equals an accepted
	// weapon's ammo index shows that entry's raw byte (D-NET-508). A row index of 128 or
	// more reads the countdown and class dwords in retail, 0 here (D-NET-692).
	// [orig: Server_SendWeaponSlotListToPlayer @0x502550 - the index @0x502720, the read
	//  @0x50286A..0x502871, emitted @0x50288A]
	WeaponLoadout reply = grant.reply;
	for (WeaponLoadoutSlot &slot : reply.slots)
		slot.ammo_alt = slot.type_id < conn.ammo_damage_classes.size()
				? conn.ammo_damage_classes[slot.type_id]
				: uint8_t{0};
	// Retain the GRANTED body: the deploy-release bundle re-sends it (the client's
	// 0x5A apply is the deploy un-latcher — resets g_ClientDeployHold; §5.30, D-NET-156).
	st.last_loadout_reply = encode_weapon_loadout(reply);
	st.ammo_pools = grant.ammo_pools;
	st.shared_clips = grant.shared_clips;
	// The 0x06 pipeline used to seed a full clip on first fire; the rebuild now owns
	// the rows retail's leaves behind.
	if (!rebuild_rows) return;
	conn.weapon_slots.clear();
	for (const GrantedWeaponLoadout::Row &row : grant.rows) {
		WeaponSlotState &slot = conn.weapon_slots[row.combo];
		slot.adm_index = row.adm_index;
		slot.clip = row.clip;
	}
}

void PlayerSlot_InitWeaponsFromLoadout(NapiNPConnection &conn, world::World &world,
                                       world::Entity &player) {
	const world::WeaponTable &table = world.tables.weapons;
	// A slot that never submitted has an empty buffer and list: the same rebuild
	// leaves it an empty table with every pool zeroed [orig: the buffer test
	//  @0x5155EB skips the entry walk alone].
	if (table.empty()) return;
	const world::WeaponAmmoSeat seat = world::entity_ammo_seat(world, player);
	const bool allow_zoom = world.rules.allow_sniper_scope_zoom;
	const LoadoutSubmit &buffer = conn.reply.loadout_buffer;
	const uint8_t cls = static_cast<uint8_t>(conn.slot_class);
	const std::vector<KitEntry> entries = admitted_entries(buffer, buffer.player_class, table);
	// The deploy's own rebuild over the display list the last rebuild left (its slots'
	// weapons in combo order, which a fill of the buffer's list reproduces): a fill, the
	// pools seeded and one clip drawn; the list rebuilt; a second fill, the list rebuilt
	// and every listed slot initialized again with its bucket zeroed; the seed and the
	// draw (this call never runs under the revive latch); every loaded round cleared;
	// each buffer entry's pools from its counts; and one clip drawn again. No class, no
	// damage-class table, no carry-bit clear beyond the fills' own.
	// [orig: PlayerSlot_InitWeaponsFromLoadout @0x515550 - the fills @0x515571..0x515581
	//  / @0x5155B8..0x5155C2, the seed and draw @0x515595..0x5155A9,
	//  PlayerSlot_BuildNameTable @0x5155AF / @0x5155CB, PlayerSlot_ReinitListedSlots @0x5155D1,
	//  PlayerSlot_SeedAmmoPoolsAndRecalc @0x5155D7, WeaponOverlay_UpdateAllPerFrame
	//  @0x5155E1, the buffer walk @0x5155EB..0x515761 (each name resolved @0x51560D),
	//  the recalc @0x51577C]
	world::WeaponInventory inv = slot_inventory(table, conn);
	reload_slots(table, inv, kit_display_list(entries, table), cls, allow_zoom);
	world::weapon_inventory_seed_pools(table, inv, cls);
	world::weapon_inventory_recalc_clips(table, inv, seat);
	std::vector<std::string> display = slot_name_table(table, inv);
	reload_slots(table, inv, display, cls, allow_zoom);
	display = slot_name_table(table, inv);
	reinit_listed_slots(table, inv, display, cls, seat, allow_zoom);
	seed_and_recalc(table, inv, cls, conn.reply.revive_pose_valid, seat);
	clear_loaded_rounds(table, inv, seat);
	for (const KitEntry &e : entries) set_kit_pools(table, inv, e, /*accept=*/false);
	world::weapon_inventory_recalc_clips(table, inv, seat);
	GrantedWeaponLoadout grant;
	grant.reply.avatar_class = cls;
	grant.ammo_pools = conn.reply.ammo_pools;
	grant.shared_clips = conn.reply.shared_clips;
	finish_grant(table, inv, conn.assigned_team, grant);
	apply_granted_loadout(conn, grant, /*rebuild_rows=*/true);
	// The fill's carry bits stay on the entity: every listed weapon's
	// [orig: WeaponSlotTable_LoadAllFromDefs - the clear @0x541503, the bits
	//  @0x5415AA / @0x5415BC].
	player.carry_flags = (player.carry_flags & ~0x18u) | inv.carry_flags;
}

void PlayerSlot_RebuildKitForRound(NapiNPConnection &conn, world::World &world,
                                   world::Entity &player) {
	const world::WeaponTable &table = world.tables.weapons;
	if (table.empty()) return;
	const LoadoutSubmit &buffer = conn.reply.loadout_buffer;
	const uint8_t cls = static_cast<uint8_t>(conn.slot_class);
	const std::vector<KitEntry> entries = admitted_entries(buffer, buffer.player_class, table);
	const world::WeaponAmmoSeat seat = world::entity_ammo_seat(world, player);
	const bool allow_zoom = world.rules.allow_sniper_scope_zoom;
	// The round init's rebuild of a kept slot from its buffer: the display list, a
	// fill, the list rebuilt, every listed slot initialized again with its bucket
	// zeroed, and the seed and the draw. No entry count is applied; the slot's next
	// deploy applies them. [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 -
	//  AvatarDef_BuildDisplayList @0x516BF3, WeaponSlotPool_ResetAllEntries @0x516C03,
	//  WeaponSlotTable_LoadAllFromDefs @0x516C0E, PlayerSlot_BuildNameTable @0x516C14,
	//  PlayerSlot_ReinitListedSlots @0x516C1A, PlayerSlot_SeedAmmoPoolsAndRecalc @0x516C20]
	world::WeaponInventory inv = slot_inventory(table, conn);
	reload_slots(table, inv, kit_display_list(entries, table), cls, allow_zoom);
	const std::vector<std::string> display = slot_name_table(table, inv);
	reinit_listed_slots(table, inv, display, cls, seat, allow_zoom);
	seed_and_recalc(table, inv, cls, conn.reply.revive_pose_valid, seat);
	GrantedWeaponLoadout grant;
	grant.reply.avatar_class = cls;
	grant.ammo_pools = conn.reply.ammo_pools;
	grant.shared_clips = conn.reply.shared_clips;
	finish_grant(table, inv, conn.assigned_team, grant);
	apply_granted_loadout(conn, grant, /*rebuild_rows=*/true);
	// The fill's carry bits [orig: WeaponSlotTable_LoadAllFromDefs @0x541503 /
	//  @0x5415AA / @0x5415BC].
	player.carry_flags = (player.carry_flags & ~0x18u) | inv.carry_flags;
}

// The player's live soldier class (entity+660), the header byte a current-slot-list re-send carries
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 reads player+89820 = player[22455]].
uint8_t current_player_class(const NapiNPConnection &conn, const world::World *world) {
	if (world == nullptr || !conn.link.owned_entity.valid()) return 0;
	const world::Entity *pe = world->registry.get(conn.link.owned_entity);
	return pe != nullptr ? pe->player_class : 0;
}

// The tag=0x5A body for every re-send that grants NOTHING new (the 0x2F envelope abort, the deploy
// release): the retained granted body when one exists, else the empty-table shape headed by the
// player's live class. [orig: Server_SendWeaponSlotListToPlayer @0x502550 — header byte
// player+89820, rows walked off the player's own 780-slot weapon table, which a player that never
// submitted a loadout has none of.]
std::vector<uint8_t> build_current_loadout_reply(const std::vector<uint8_t> &retained,
                                                 uint8_t player_class) {
	if (!retained.empty()) return retained;
	WeaponLoadout current;
	current.avatar_class = player_class;
	return encode_weapon_loadout(current);
}

// The list send first sets the slot's PlayerType to its class, then builds the list.
// [orig: Server_SendWeaponSlotListToPlayer @0x502550: slot+0x15EDC @0x5025A8, the
//  SetOrCreate @0x5025DB, no test ahead of it]
std::vector<uint8_t> current_weapon_slot_list(NapiNPConnection &conn, const world::World *world) {
	const uint8_t player_class = current_player_class(conn, world);
	conn.player_list_type = player_class;
	return build_current_loadout_reply(conn.reply.last_loadout_reply, player_class);
}

} // namespace opennova::inmatch
