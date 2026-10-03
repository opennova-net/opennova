#pragma once

#include <vector>

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h> // IRemoteWeaponTables

namespace opennova::world {
struct WeaponInventory;
struct WeaponTable;
} // namespace opennova::world

namespace opennova::inmatch {

// A connection's weapon tables as one inventory: retail's player-slot block
// (the 780-slot table at +0x1D0, the class pools at +0x15A58, the shared clips
// at +0x15C58) as this host keeps it -- the seeded slot rows plus the 0x0F
// reply's pools. `store` writes an inventory back: every held slot's row (new
// rows included) and the pools. [orig: Entity_ValidatePtr @0x500910 and the
// block offsets every authority arm reads]
world::WeaponInventory conn_weapon_inventory(const NapiNPConnection &conn,
		const world::WeaponTable &table);
void store_conn_weapon_inventory(NapiNPConnection &conn, const world::WeaponInventory &inventory);

// The roster's tables behind the World's remote-player seam: the session
// installs one around its entity pass, where the pickup's refusal walks read
// a remote picker's tables. The listen host's own player is the local one and
// has none here (world/powerup.cpp reads its live inventory).
// [orig: WeaponSlot_RecalculateScore @0x542450 -> Entity_ValidatePtr @0x542464]
class ServerRemoteWeaponTables final : public world::IRemoteWeaponTables {
public:
	explicit ServerRemoteWeaponTables(std::vector<NapiNPConnection> &roster)
			: roster_(roster) {}
	bool remote_weapon_tables(const world::World &world, world::EntityHandle player,
			world::WeaponInventory &out) const override;

private:
	std::vector<NapiNPConnection> &roster_;
};

// Drain the world's powerup grants (world/powerup.h) onto the owning
// connections' pool tables and slot clips: the per-class adds and the
// `allammo` re-seed the way retail's authority arms write the validated
// entity's per-connection tables; the listen host's own player never
// produces one (its live inventory is written in place). Runs in the entity
// pass routes; exposed for its tests.
// [orig: WeaponSlot_AddAmmo @0x540A20 (@0x540AC2..0x540AF1);
//  Entity_UpdateWeaponOverlayFrameState @0x4DC340 (@0x4DC348..0x4DC38F)]
void Server_ApplyPowerupGrants(std::vector<NapiNPConnection> &roster, world::World &world);

// Drain the world's powerup weapon grants: a REMOTE picker's weapon lands on
// its connection's slot table (WeaponSlot_InitFromAvatarDef over the block,
// the reload stamping the host's copy of its body), then S2C 0x35 [u16 picker]
// [u16 powerup] goes reliable to every in-match slot but the listen host, for
// every grant (the listen host's own pickup included). Runs in the entity pass
// routes; exposed for its tests.
// [orig: Server_BroadcastWeaponOverlayUpdate @0x509FC0]
void Server_BroadcastWeaponOverlayUpdates(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
