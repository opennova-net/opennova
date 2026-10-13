#include <runtime/inmatch/server_loadout_grant.h>

#include <net/npwire/ingame_encode.h>          // encode_weapon_loadout (the 0x5A body)
#include <runtime/world/weapon_table_build.h>  // loadout_entry_permitted / resolve_loadout_ammo (D-NET-141)
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace opennova::inmatch {

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
										  const world::WeaponTable *table) {
	GrantedWeaponLoadout grant;
	WeaponLoadout &reply = grant.reply;
	// Class 0 passes the envelope and applies with an EMPTY grant: its soldier-type mask is 0, so
	// no request entry can match, and entity+660 is stamped 0 [orig: type_mask default @0x5159af,
	// the stamp @0x515ab0].
	if (req.player_class == 0) return grant; // avatar_class stays 0
	// Class accept: a valid but disabled request scans the whole Soldier Class range from 5 and
	// takes its first enabled bit. With no enabled bit the already-valid request survives; the
	// final 8 clamp only covers an invalid value [orig: g_HostClassAllowMask @0x24D59FC,
	// @0x5158d6..@0x515915].
	reply.avatar_class = req.player_class;
	if ((class_allow_mask & (uint16_t{1} << reply.avatar_class)) == 0) {
		for (uint8_t candidate = 5; candidate <= 9; ++candidate) {
			if ((class_allow_mask & (uint16_t{1} << candidate)) != 0) {
				reply.avatar_class = candidate;
				break;
			}
		}
	}
	if (reply.avatar_class < 5 || reply.avatar_class > 9) reply.avatar_class = 8;

	if (table != nullptr && !table->empty()) {
		struct GrantedSlot {
			uint16_t combo = 0;
			WeaponLoadoutSlot wire;
		};
		// The original first loads a 780-slot table keyed by category*65+rank. Repeating
		// that slot replaces it; the later reply walk therefore emits it exactly once.
		std::vector<GrantedSlot> accepted;
		for (const LoadoutSubmitEntry &e : req.entries) {
			const world::WeaponTableEntry *we = table->by_index(e.adm_index);
			if (we == nullptr) continue; // the AdmDef_GetEntryByIndex fail leg
			if (!world::loadout_entry_permitted(*we, req.team, reply.avatar_class))
				continue; // team/char mask filter [orig: @0x502716]
			// Retail's next test, the map availability (0 refused, 2 only inside an
			// armory zone), is not ported: D-NET-693 [orig: @0x515A3F..0x515A58].
			if (we->flags & world::weapon_flag::kArmor) grant.carry_flags |= 8u;
			if (we->flags2 & 2) grant.carry_flags |= 0x10u;
			if (we->ammo_class_id >= 0 && we->ammo_class_id < 128) {
				int32_t pool = e.ammo_primary != 0xFF
						? std::min<int32_t>(e.ammo_primary, we->maxclips) *
								we->clipsize
						: we->startrounds;
				if (we->ammo_class_id <
				    static_cast<int>(table->ammo_class_caps.size())) {
					const int32_t cap = table->ammo_class_caps[
							static_cast<size_t>(we->ammo_class_id)];
					if (pool > cap) pool = cap;
				}
				grant.ammo_pools[
						static_cast<size_t>(we->ammo_class_id)] = pool;
			}
			// The accept's store, in request order: the raw fourth byte at the
			// weapon's round ammo index [orig: @0x515E8B..0x515E92].
			grant.damage_class_writes.emplace_back(we->ammo_index, e.variant);
			const world::LoadoutAmmoBytes ammo =
					world::resolve_loadout_ammo(*table, e.adm_index, e.ammo_primary);
			GrantedSlot s;
			s.combo = static_cast<uint16_t>(we->category * 65u + we->rank);
			s.wire.type_id = e.adm_index;
			s.wire.ammo_primary = ammo.primary;
			s.wire.ammo_secondary = ammo.secondary;
			auto existing = std::find_if(accepted.begin(), accepted.end(),
									 [combo = s.combo](const GrantedSlot &slot) {
										 return slot.combo == combo;
									 });
			if (existing == accepted.end()) accepted.push_back(s);
			else *existing = s; // the last request entry loaded into this retail slot wins
		}
		std::sort(accepted.begin(), accepted.end(),
		          [](const GrantedSlot &a, const GrantedSlot &b) { return a.combo < b.combo; });
		for (GrantedSlot &slot : accepted) {
			// Rebuilding a retail weapon-slot table finishes by drawing one initial
			// clip from every populated slot before S2C 0x0F copies player+88664.
			// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515db5-@0x515de3 and
			// @0x515e0d-@0x515f4d -> WeaponSlots_RecalculateAmmoFromCapacity]
			// Golden team-1 witness: 300->270 (M16), 70->63 (.45), 3->2
			// (AT4/smoke), 2->1 (HE), and 1->0 (flashbang).
			const world::WeaponTableEntry *we = table->by_index(slot.wire.type_id);
			GrantedWeaponLoadout::Row row;
			row.combo = slot.combo;
			row.adm_index = slot.wire.type_id;
			row.clip = we != nullptr && we->ammo_bucket == 0
                    ? static_cast<int16_t>(we->clipsize) : int16_t{0};
            const bool shared = we != nullptr && we->ammo_bucket != 0;
            const uint32_t bucket = we != nullptr ? static_cast<uint32_t>(we->ammo_bucket) : 0;
            const bool bucket_valid = bucket < grant.shared_clips.size() &&
                    bucket < table->ammo_class_names.size();
			if (we != nullptr && we->ammo_class_id >= 0 &&
			    we->ammo_class_id < static_cast<int>(grant.ammo_pools.size()) &&
			    we->ammo_class_count != 0 && we->clipsize != -1) {
				int32_t &pool = grant.ammo_pools[
						static_cast<size_t>(we->ammo_class_id)];
				if (shared && bucket_valid && grant.shared_clips[bucket] != 0) {
                    pool += grant.shared_clips[bucket] * we->ammo_class_count;
                    if (static_cast<size_t>(we->ammo_class_id) < table->ammo_class_caps.size())
                        pool = std::min(pool, table->ammo_class_caps[we->ammo_class_id]);
                }
                int32_t draw = static_cast<int32_t>(we->clipsize) * we->ammo_class_count;
				if (draw > pool) draw = pool;
				pool -= draw;
				if (pool < 0) pool = 0; // WeaponSlot_DecrementAmmo's lower clamp
				// slot+16 = the drawn rounds [orig: @0x5423xx in the recalc]
				if (!shared) row.clip = static_cast<int16_t>(draw / we->ammo_class_count);
                else if (bucket_valid) grant.shared_clips[bucket] = draw / we->ammo_class_count;
			}
			grant.rows.push_back(row);
			reply.slots.push_back(slot.wire);
		}
		return grant;
	}

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

void PlayerSlot_InitWeaponsFromLoadout(const GameConfig &config, NapiNPConnection &conn,
                                       const world::WeaponTable &table) {
	if (!conn.reply.loadout_buffer_set || table.empty()) return;
	// The rebuild loads the buffer's names over a fresh table, seeds the pools from
	// the defs under the slot's class, sets each entry's pool from its requested
	// count (min(count, maxclips) clips, else startrounds) and draws the clips again
	// [orig: @0x515571..0x5155A9, the entry walk @0x51560D..0x515752, the final
	//  recalc @0x51577C] -- the accept's own rebuild over the same entries, so the
	// grant reproduces it, the 0x5A body the deploy re-sends included
	// [orig: Server_SendWeaponSlotListToPlayer @0x5178F2 walks the rebuilt table].
	apply_granted_loadout(conn,
			grant_weapon_loadout(conn.reply.loadout_buffer, config.class_allow_mask, &table),
			/*rebuild_rows=*/true);
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
