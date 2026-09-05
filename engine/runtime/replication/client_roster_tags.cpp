#include <runtime/replication/client_roster_tags.h>

#include <runtime/world/friendly_tag_gates.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace opennova::replication {

void collect_roster_tags(const ClientState &state, uint16_t self_handle,
                         uint8_t local_team, bool death_screen,
                         uint32_t game_type,
                         std::vector<world::FriendlyTagSource> &out,
                         const RosterTagMaxHealth &max_health) {
	for (const ClientRosterSlot &slot : state.roster) {
		// slot+0x0D active, slot+0x24 entity [orig: @0x5a453b/@0x5a454e].
		if (!slot.bound || slot.entity_slot < 0) continue;
		const world::EntityHandle handle{
				static_cast<uint16_t>(slot.entity_slot & 0xFFF)}; // pool 0
		const ClientEntityState *row = state.find(handle.packed);
		if (row == nullptr) continue;
		// The drawer's entry bails, the shared predicate of
		// world/friendly_tag_gates.h [orig: @0x5a39df self; @0x5a39eb Flags & 1;
		// @0x5a39fb itemDef == NULL]. A decoded row's def is the joiner's item
		// table lookup: the max-health callback's 0 is its "no def" answer.
		const int32_t def_max = max_health ? max_health(row->type_id) : 0;
		const bool has_item_def = !max_health || def_max != 0;
		if (world::friendly_tag_entry_bails(row->handle == self_handle,
					row->state_flags & 0x01u ? world::kEntityFlagCarried : 0u,
					has_item_def))
			continue;
		// The pass gates [orig: @0x5a4552..0x5a457d]: entity+0x162 team vs the
		// local team unless the death screen is up, then `g_GameType || death
		// screen`. A row with no team-bearing record yet (0xFF) is not team 0.
		const uint8_t team = row->team == 0xFF ? 0 : row->team;
		if (!world::friendly_tag_pass_gates(team, local_team, death_screen, game_type))
			continue;

		world::FriendlyTagSource src;
		src.entity = handle;
		src.net_id = handle.packed; // the fallback-name index [orig: (pool<<12)|slot]
		src.position.x = static_cast<float>(row->x) / 65536.0f;
		src.position.y = static_cast<float>(row->y) / 65536.0f;
		src.position.z = static_cast<float>(row->z) / 65536.0f;
		// The remote body's entity+0x74 eye lift is written by the client-side
		// body updater retail runs on every player; the decoded row carries no
		// eye sample, so the anchor rides the origin + 0x4000 (record residue).
		src.eye_offset_z = 0;
		// The slot callsign is the label [orig: Napi_CopyString(name, slot+20)
		// @0x5a3f43]; the slot+32 `<ch>..<co>` wrap is the record's residue.
		src.name = slot.name;
		src.player = true;
		// health<<16 / max — the def hp (or 1 with no def) [orig: @0x5a3b91..
		// 0x5a3bb8], clamped like the pool-0 gather.
		const int64_t max_hp = std::max<int32_t>(1, def_max);
		const int64_t health = row->health_known ? row->health_word : 0;
		src.health_ratio_fp16 = static_cast<int32_t>(
				std::min<int64_t>((health << 16) / max_hp, 0x10000));
		// The remote player's class (entity+0x294) is not decoded on a joiner;
		// the plate stays off for roster rows (record residue).
		src.medic = false;
		src.dead = (row->state_flags & 0x02u) != 0;
		src.has_slot = true;
		src.revive_seconds = slot.downed_revive_seconds;
		src.medic_request = slot.medic_request_active;
		out.push_back(std::move(src));
	}
}

} // namespace opennova::replication
