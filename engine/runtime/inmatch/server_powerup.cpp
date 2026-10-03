#include <runtime/inmatch/server_powerup.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <net/npwire/ingame_decode.h>     // WeaponPickupNotice
#include <net/npwire/ingame_encode.h>     // encode_weapon_pickup
#include <net/npwire/ingame_message_id.h> // s2c::WEAPON_PICKUP
#include <runtime/world/powerup.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table.h>

namespace opennova::inmatch {

namespace {

// The connection that owns `player` in the roster's in-match range, or null.
NapiNPConnection *owning_connection(std::vector<NapiNPConnection> &roster,
		world::EntityHandle player) {
	for (NapiNPConnection &conn : roster) {
		if (conn.phase < ConnectionPhase::PlayerAdded ||
				conn.phase >= ConnectionPhase::Goodbye ||
				conn.link.owned_entity != player)
			continue;
		return &conn;
	}
	return nullptr;
}

} // namespace

world::WeaponInventory conn_weapon_inventory(const NapiNPConnection &conn,
		const world::WeaponTable &table) {
	world::WeaponInventory inventory;
	inventory.reset(table);
	for (const auto &[combo, row] : conn.weapon_slots) {
		world::WeaponInventorySlot *slot = inventory.slot(combo);
		if (slot == nullptr) continue;
		slot->adm_index = row.adm_index;
		slot->clip = row.clip;
	}
	const size_t pool_count = std::min(inventory.pools.size(), conn.reply.ammo_pools.size());
	for (size_t i = 0; i < pool_count; ++i) {
		inventory.pools[i] = conn.reply.ammo_pools[i];
		inventory.shared_clips[i] = conn.reply.shared_clips[i];
	}
	return inventory;
}

void store_conn_weapon_inventory(NapiNPConnection &conn, const world::WeaponInventory &inventory) {
	const size_t pool_count = std::min(inventory.pools.size(), conn.reply.ammo_pools.size());
	for (size_t i = 0; i < pool_count; ++i) {
		conn.reply.ammo_pools[i] = inventory.pools[i];
		conn.reply.shared_clips[i] = inventory.shared_clips[i];
	}
	for (int32_t combo = 0; combo < world::weapon_combo::kSlotCount; ++combo) {
		const world::WeaponInventorySlot *slot = inventory.slot(combo);
		if (slot == nullptr || slot->adm_index < 0) continue;
		WeaponSlotState &row = conn.weapon_slots[static_cast<uint16_t>(combo)];
		row.adm_index = static_cast<uint8_t>(slot->adm_index);
		row.clip = static_cast<int16_t>(slot->clip);
	}
}

bool ServerRemoteWeaponTables::remote_weapon_tables(const world::World &world,
		world::EntityHandle player, world::WeaponInventory &out) const {
	if (world.tables.weapons.empty()) return false;
	NapiNPConnection *conn = owning_connection(roster_, player);
	if (conn == nullptr) return false; // Entity_ValidatePtr finds no slot
	out = conn_weapon_inventory(*conn, world.tables.weapons);
	return true;
}

// A remote player's powerup pickup: the per-class adds and the `allammo`
// re-seed land on the owning connection's pools and slot rows, the way retail's
// authority arms write the validated entity's per-connection tables (the listen
// host's own player writes its live inventory inside world/powerup.cpp). The
// joiner ran the same pickup on its own pools, so no message follows. Only the
// slots the host has seeded take the re-seed (D-PWR-4, docs/world/powerup-re.md).
// [orig: WeaponSlot_AddAmmo @0x540A20 -- Entity_ValidatePtr @0x540AC5, the pool
//  add and cap clamp @0x540AD8..0x540AF1; Entity_UpdateWeaponOverlayFrameState
//  @0x4DC340 -- WeaponSlots_SeedAmmoPoolsFromDefs @0x4DC373 then
//  WeaponSlots_RecalculateAmmoFromCapacity @0x4DC38F over the slot tables]
void Server_ApplyPowerupGrants(std::vector<NapiNPConnection> &roster, world::World &world) {
	if (world.out.powerup_grants.empty()) return;
	const world::WeaponTable &table = world.tables.weapons;
	for (const world::PowerupGrant &grant : world.out.powerup_grants) {
		if (table.empty()) break;
		NapiNPConnection *conn = owning_connection(roster, grant.picker);
		if (conn == nullptr) continue;
		const world::Entity *body = world.registry.get(grant.picker);
		world::WeaponInventory inventory = conn_weapon_inventory(*conn, table);
		if (grant.allammo) {
			world::weapon_inventory_seed_pools(
					table, inventory, body != nullptr ? body->player_class : 0);
			world::weapon_inventory_recalc_clips(table, inventory);
		}
		for (const auto &[class_id, amount] : grant.ammo_adds)
			world::weapon_pool_add(table, inventory, class_id, amount);
		store_conn_weapon_inventory(*conn, inventory);
	}
	world.out.powerup_grants.clear();
}

void Server_BroadcastWeaponOverlayUpdates(NapiNPServerCtx &ctx, world::World &world) {
	if (world.out.powerup_weapon_grants.empty()) return;
	const world::WeaponTable &table = world.tables.weapons;
	for (const world::PowerupWeaponGrant &grant : world.out.powerup_weapon_grants) {
		// The validated slot's table takes the weapon [orig: Entity_ValidatePtr
		//  @0x509FDB, WeaponSlot_InitFromAvatarDef(slot + 0x1D0, picker, row)
		//  @0x509FE9]. The listen host's own player has the one live inventory,
		//  which its pickup already wrote.
		NapiNPConnection *conn = grant.picker == world.cached.local_player
				? nullptr
				: owning_connection(ctx.np_protocol.connection_list, grant.picker);
		if (conn != nullptr && !table.empty()) {
			const world::Entity *body = world.registry.get(grant.picker);
			world::WeaponInventory inventory = conn_weapon_inventory(*conn, table);
			const world::WeaponAvatarGrant landed = world::weapon_inventory_init_from_avatar_def(
					table, inventory, grant.weapon, body != nullptr ? body->player_class : 0,
					world.rules.allow_sniper_scope_zoom);
			store_conn_weapon_inventory(*conn, inventory);
			// The reload's entry stamp on the host's copy of the body
			// [orig: WeaponSlot_ReloadAmmo @0x54173c]
			if (landed.reloaded)
				if (world::AiEntity *peer = world.ai.for_handle(grant.picker))
					if (peer->inf.active) peer->inf.reload_anim_ticks = 80;
		}
		// S2C 0x35 to every in-match slot but the listen host's own (mask 0x90)
		// [orig: @0x509FF1..0x50A07D]
		WeaponPickupNotice notice;
		notice.picker_handle = grant.picker.packed;
		notice.powerup_handle = grant.powerup.packed;
		const std::vector<uint8_t> body = encode_weapon_pickup(notice);
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!is_in_match(c) || c.link.transport == nullptr) continue;
			if (c.link.mode == replication::TransportMode::Loopback) continue;
			c.link.transport->host_send(s2c::WEAPON_PICKUP, body);
		}
	}
	world.out.powerup_weapon_grants.clear();
}

} // namespace opennova::inmatch
