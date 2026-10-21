#include <runtime/inmatch/loadout_submit.h>

#include <net/title/title_identity.h> // DFX2-12, the sender's fixed class
#include <runtime/world/weapon_table.h>

#include <vector>

namespace opennova::inmatch {

namespace {

// One side block -> (clamped class, ADM-resolved rows). Retail re-reads the
// profile block the wire team byte names on EVERY profile-sourced
// submission, so the row resolve is shared by the resident-kit rows and by
// both resident side blocks.
std::vector<LoadoutSubmitEntry> resolve_side(const world::World &world,
		const playersav::Side &s, uint8_t klass) {
	std::vector<LoadoutSubmitEntry> rows;
	const playersav::KitPage *p = s.page_for_class(klass);
	if (p == nullptr) return rows;
	for (const playersav::KitEntry &entry : p->entries) {
		const int adm = world.tables.weapons.index_of(entry.name.c_str());
		if (adm < 0) continue; // [orig: the AdmDef_FindByName gate @0x42cf0b]
		rows.push_back(LoadoutSubmitEntry{
				static_cast<uint8_t>(adm),
				static_cast<uint8_t>(entry.ammo_primary),
				static_cast<uint8_t>(entry.ammo_secondary),
				static_cast<uint8_t>(entry.flags)});
	}
	return rows;
}

} // namespace

uint8_t loadout_side_team(const world::World &world, uint8_t assigned_team) {
	if (assigned_team != 0) return assigned_team;
	const world::Entity *local = world.registry.get(world.cached.local_player);
	return local != nullptr ? local->team : 0;
}

bool seed_session_kit_from_profile(world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		world::LocalPlayerLoadout &loadout, int &seeded_side) {
	if (world.tables.weapons.empty()) return false;
	const uint8_t team = loadout_side_team(world, assigned_team);
	// An UNLATCHED team must not commit a page. The side selector is the S2C 0x04 tail
	// byte [orig: g_LocalTeam @0x425499], and retail cannot reach this copy before it is
	// latched — admission delivers 0x04 long before Game_StartMission runs, so
	// @0x525798's `team == 1 || team == 3` always sees a real value. Our catalog can land
	// first, and since anything-not-1-or-3 selects the RED block, seeding at team 0 would
	// commit the wrong side's page and then have to flip it. Wait instead; the pump
	// re-seeds the moment the latch (or a later 0x50 reassignment) changes the side.
	if (team == 0) return false;
	const playersav::Side &side =
			profile.side(playersav::side_for_team(team));
	uint8_t player_class = side.player_class;
	if (player_class < 5 || player_class > 9) player_class = 8;
	const playersav::KitPage *page = side.page_for_class(player_class);
	if (page == nullptr || page->entries.empty()) return false;
	std::vector<world::WeaponKitEntry> kit;
	kit.reserve(page->entries.size());
	for (const playersav::KitEntry &e : page->entries)
		kit.push_back(world::WeaponKitEntry{e.name, e.ammo_primary,
		                                    e.ammo_secondary, e.flags});
	loadout.spawn_kit = std::move(kit);
	loadout.spawn_kit_set = true;
	seeded_side = static_cast<int>(playersav::side_for_team(team));
	return true;
}

bool reseed_session_kit_on_side_change(world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		world::LocalPlayerLoadout &loadout, int &seeded_side) {
	const uint8_t team = loadout_side_team(world, assigned_team);
	if (team == 0) return false;
	if (static_cast<int>(playersav::side_for_team(team)) == seeded_side)
		return false;
	return seed_session_kit_from_profile(
			world, profile, assigned_team, loadout, seeded_side);
}

int32_t resolve_loadout_submit_combo(const world::WeaponTable &weapons,
		const world::WeaponInventory *inventory, uint8_t team,
		int32_t requested_combo) {
	if (inventory == nullptr || requested_combo < 0 ||
			requested_combo >= world::weapon_combo::kSlotCount)
		return requested_combo;

	// g_LocalTeam teams 1/3 use BLUE (2), 2/4 use RED (1), and every
	// other latch value accepts either side (3) [orig: @0x42ce2d..0x42ce58].
	const uint8_t side_mask =
			(team == 1 || team == 3) ? 2u : ((team == 2 || team == 4) ? 1u : 3u);
	const auto is_side_legal = [&](int32_t combo) {
		const world::WeaponInventorySlot *slot = inventory->slot(combo);
		if (slot == nullptr || slot->adm_index < 0 || slot->adm_index > 255)
			return false;
		const world::WeaponTableEntry *def =
				weapons.by_index(static_cast<uint8_t>(slot->adm_index));
		return def != nullptr && (def->teamfilter & side_mask) != 0;
	};

	if (is_side_legal(requested_combo)) return requested_combo;
	const int32_t category_base =
			(requested_combo / world::weapon_combo::kRanksPerCategory) *
			world::weapon_combo::kRanksPerCategory;
	for (int32_t rank = 0; rank < world::weapon_combo::kRanksPerCategory; ++rank) {
		const int32_t candidate = category_base + rank;
		if (is_side_legal(candidate)) return candidate;
	}
	return requested_combo;
}

LoadoutSubmit armory_accept_in_session(world::World &world, world::LocalPlayer &local,
		const std::vector<world::WeaponKitEntry> &kit, uint8_t armory_class, bool authority,
		bms::MissionType mission_type) {
	// The whole selection, unfiltered, on both legs, into the process's loadout name list
	// (g_LoadoutWeaponNameList, whose first name every body the process spawns takes), not the
	// spawn buffer: the resident kit (LocalPlayerLoadout::spawn_kit, g_SpawnLoadoutBuffer)
	// keeps its rows until the host's list reaches it (a joiner's 0x5A apply copies the rebuilt
	// list over it, a listen host's own list send its slot's buffer) [orig: the rep movsd into
	// g_LoadoutWeaponNameList @0x565D46..0x565D4B; NapiNPClientMsg_HandleWeaponLoadoutSync
	// @0x4293D1..0x4293E9; Server_SendWeaponSlotListToPlayer @0x502617..0x502627].
	if (world.loadout_weapon_names != nullptr) {
		world.loadout_weapon_names->entries = kit;
		world.loadout_weapon_names->set = true;
	}
	const world::WeaponInventory *inventory = local.inventory_valid ? &local.inventory : nullptr;
	const int32_t current_slot = inventory != nullptr ? inventory->equipped_combo : -1;
	// The slot pool's entries alone, off the authority alone: the current slot word
	// stays for the 0x2F and its readers until the 0x5A's select.
	// [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (the call @0x565D66) ->
	//  WeaponSlotPool_ResetAllEntries @0x53F240]
	if (!authority) local.inventory.reset_slot_entries();
	LoadoutSubmit submit;
	const world::Entity *body = world.registry.get(world.cached.local_player);
	submit.team = body != nullptr ? body->team : uint8_t{0};
	submit.player_class = armory_class;
	// DFX2's sender: 8 (rifleman), or 7 (gunner) in a mission of type 2, whatever the class
	// [orig: dfx2.exe NetPacket_SendLoadoutSubmit @0x42EB40 (@0x42EC15..0x42EC3E)].
	if (title_in_match(title_identity(world.rules.title)).loadout_class_fixed)
		submit.player_class = mission_type == bms::MissionType::CombatVehicleMission
				? uint8_t{7}
				: uint8_t{8};
	submit.weapon_slot_index = static_cast<uint32_t>(
			resolve_loadout_submit_combo(world.tables.weapons, inventory, submit.team, current_slot));
	for (const world::WeaponKitEntry &entry : kit) {
		const int adm = world.tables.weapons.index_of(entry.name.c_str());
		if (adm < 0) continue; // [orig: the AdmDef_FindByName gate @0x42cf0b]
		submit.entries.push_back(LoadoutSubmitEntry{static_cast<uint8_t>(adm),
				static_cast<uint8_t>(entry.ammo_primary),
				static_cast<uint8_t>(entry.ammo_secondary),
				static_cast<uint8_t>(entry.flags)});
	}
	return submit;
}

void build_joiner_loadout_kit(const world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		const world::LocalPlayerLoadout &loadout, int equipped_combo,
		const world::WeaponInventory *inventory,
		JoinerConnection::LoadoutKit &out) {
	// The side selector. The host's S2C 0x04 tail byte is retail's g_LocalTeam, and
	// teams 1/3 read the BLUE block, 2/4 the RED one [orig: @0x525788]. Before that
	// latch lands (assigned_team() == 0) fall back to the local entity's own team when
	// L already exists. With neither, side_for_team(0) resolves to RED — anything that
	// is not 1 or 3 reads the red block @0x525798 — which is why the resident buffer is
	// NOT copied at team 0 (see seed_session_kit_from_profile) and why the pump re-seeds
	// once the latch lands. The seam itself still arms so the runtime has content.
	const uint8_t team = loadout_side_team(world, assigned_team);
	const playersav::Side &side =
			profile.side(playersav::side_for_team(team));
	// ONE integer: the wire class byte AND the page index [orig: eax = *(u8*)esi, the
	// switch(class-5) page map]. The [5,9] clamp is retail's own per-side session-start
	// clamp [orig: Game_ApplySessionSettingsToGlobals @0x5516ab..@0x5516ec]; 8
	// (rifleman) is the shipped profile default [orig: PlayerProfile_InitDefaults
	// @0x54bbe0/@0x54bbe3] and also what the host's 0x2F envelope requires — it aborts
	// on a nonzero class outside [5,9] [orig: @0x5158b1 -> @0x515fa5].
	uint8_t player_class = side.player_class;
	if (player_class < 5 || player_class > 9) player_class = 8;
	out.player_class = player_class;
	// The pair's SECOND submit carries the live equipped slot instead of the fixed 195.
	// The sender re-resolves that slot against the populated local pool and the
	// assigned side; the pre-Player_InitPlayer first submit and team-change submit
	// retain their separate raw-195 paths because they never consume this field
	// [orig: Game_StartMission @0x525c2e; NetPacket_SendLoadoutSubmit
	// @0x42ce2d..0x42ce8b].
	out.equipped_combo = resolve_loadout_submit_combo(
			world.tables.weapons, inventory, team, equipped_combo);
	// The RESIDENT rows are the resident kit buffer, not a fresh read of the profile
	// page: Game_StartMission copies the profile page into restrictionData
	// (g_SpawnLoadoutBuffer), Player_InitPlayer builds the local display list from it,
	// and every submit but the ACCEPT's serializes it (the mission start's pair, the
	// team assign's resubmit). The in-session ACCEPT sends its own class buffer and
	// writes the loadout name list, not restrictionData, so a resubmit between an ACCEPT
	// and the host's list carries the pre-ACCEPT rows; the list's apply copies the grant
	// over it [orig: NetPacket_SendLoadoutSubmit's restrictionData argument: g_SpawnLoadoutBuffer
	// @0x52582F / @0x525C24 / @0x431AA3, the ACCEPT's class buffer @0x565D7F..0x565D85;
	// WeaponLoadout_ApplyFromBuffer @0x565D46; NapiNPClientMsg_HandleWeaponLoadoutSync
	// @0x4293D1..0x4293E9]. `loadout.spawn_kit` is our restrictionData;
	// `seed_session_kit_from_profile` is the Game_StartMission copy that fills it.
	{
		const std::vector<world::WeaponKitEntry> &resident =
				loadout.spawn_kit_set ? loadout.spawn_kit
				                      : world::weapon_kit_default(world.rules.title);
		for (const world::WeaponKitEntry &entry : resident) {
			const int adm = world.tables.weapons.index_of(entry.name.c_str());
			if (adm < 0) continue; // [orig: the AdmDef_FindByName gate @0x42cf0b]
			out.rows.push_back(LoadoutSubmitEntry{
					static_cast<uint8_t>(adm),
					static_cast<uint8_t>(entry.ammo_primary),
					static_cast<uint8_t>(entry.ammo_secondary),
					static_cast<uint8_t>(entry.flags)});
		}
	}
	// BOTH side blocks stay resident on the seam. Retail keeps the whole profile in
	// memory and re-selects the side the NEW team byte names when the S2C 0x50 team
	// assign moves us across the line — class AND page together [orig:
	// NapiNPClientMsg_TeamAssign @0x431a35..@0x431a9a]. Without these the resubmit
	// would ship whatever side was resident when the seam was last pushed, i.e. the
	// OLD side's page against the NEW side's team byte.
	const auto fill_side = [&](JoinerConnection::LoadoutKit::SideKit &side_out,
			const playersav::Side &s) {
		uint8_t k = s.player_class;
		if (k < 5 || k > 9) k = 8; // [orig: the per-side clamp @0x5516ab..@0x5516ec]
		side_out.set = true;
		side_out.player_class = k;
		side_out.rows = resolve_side(world, s, k);
	};
	fill_side(out.blue, profile.blue);
	fill_side(out.red, profile.red);
}

void kit_from_authoritative_grant(const world::WeaponTable &weapons,
		const WeaponLoadout &grant,
		std::vector<world::WeaponKitEntry> &r_kit) {
	r_kit.clear();
	for (const WeaponLoadoutSlot &slot : grant.slots) {
		const world::WeaponTableEntry *def = weapons.by_index(slot.type_id);
		if (def == nullptr) continue; // retail drops failed AdmDef lookups
		world::WeaponKitEntry entry;
		entry.name = def->name;
		entry.ammo_primary = static_cast<int32_t>(
				static_cast<int8_t>(slot.ammo_primary));
		entry.ammo_secondary = static_cast<int32_t>(
				static_cast<int8_t>(slot.ammo_secondary));
		entry.flags = static_cast<int32_t>(
				static_cast<int8_t>(slot.ammo_alt));
		r_kit.push_back(std::move(entry));
	}
}

} // namespace opennova::inmatch
