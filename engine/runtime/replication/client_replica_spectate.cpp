// The client-local spectate state: the death screen's spectate sub-mode
// (dword_A860F0: 0 free, 1 chase, 2 first person) and target (dword_A860F4),
// their writers — the target walk, the sub-mode cycle, the SPECTATORTARGET
// track, the destroyed-target re-pick and the S2C 0x75 spectator-mode fold —
// over the replica rows, which hold every peer's pool-0 players for both the
// joiner and the listen host's own client.
// [orig: Spectator_CycleTarget_0 @0x52ac20; sub_52AFF0 @0x52aff0;
//  Entity_TrySetMinimapTrackTarget @0x52abc0; Entity_Destroy @0x43e820..0x43e838;
//  NapiNPClientMsg_SetSpectatorMode @0x4259e0]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>

#include <algorithm>

namespace opennova::replication {

namespace {

constexpr uint16_t kNoTarget = 0xFFFF;

// The walk's acceptance test: not the local player, a live ItemTypeIndex,
// the player classifier (Flags & 0x100) and neither hidden (Flags & 1) nor
// dead (Flags & 2) [orig: @0x52ad09..0x52ad1f]. The row's class stands for
// the classifier bit and its compact state flags for the low two bits.
bool accepts_row(const ClientEntityState &row, uint16_t local) {
	if (row.handle == local || row.type_id == 0) return false;
	if (row.cls != EntityClass::Player) return false;
	return !row.state_flags_known || (row.state_flags & 3u) == 0;
}

} // namespace

void ClientReplicaPipeline::spectate_cycle_target(int direction) {
	// [orig: Spectator_CycleTarget_0 @0x52ac20]
	if (direction == 0) {
		state_.spectate_target = kNoTarget; // [orig: @0x52ac2b]
		state_.death_screen_submode = 0;    // [orig: @0x52ac31]
		state_.mark_changed();
		return;
	}
	const int step = direction < 0 ? -1 : 1; // [orig: the sign bit @0x52ac4e]
	const uint16_t local = spectate_local_handle_;
	// No target walks from the local player [orig: @0x52ac54..0x52ac5b].
	if (state_.spectate_target == kNoTarget) state_.spectate_target = local;
	const uint16_t current = state_.spectate_target;
	const uint16_t pool = static_cast<uint16_t>(current & 0xF000u);
	// The pool's used count (Pool_GetUsedCount, its allocation high-water
	// mark): the rows' highest slot in the pool, the local player's included.
	// Slots past the last row hold nothing the test accepts, so the walk
	// visits the same candidates in the same order [orig: @0x52ac95].
	int used = 0;
	for (const ClientEntityState &row : state_.entities)
		if ((row.handle & 0xF000u) == pool) used = std::max(used, (row.handle & 0xFFF) + 1);
	if ((local & 0xF000u) == pool && local != kNoTarget) used = std::max(used, (local & 0xFFF) + 1);
	int slot = (current & 0xFFF) + step; // [orig: @0x52aca0]
	for (int iterations = 0; iterations < used; ++iterations) { // [orig: @0x52ad2a]
		if (slot < 0) slot = used - 1;          // [orig: @0x52acba]
		else if (slot >= used) slot = 0;        // [orig: @0x52acdd]
		const uint16_t candidate = static_cast<uint16_t>(pool | (slot & 0xFFF));
		const ClientEntityState *row = state_.find(candidate);
		if (row != nullptr && accepts_row(*row, local)) {
			state_.spectate_target = candidate; // [orig: @0x52ad2e]
			break;
		}
		slot += step; // [orig: @0x52ad21]
	}
	// Nothing but the local player: back to the free sub-mode with no target
	// [orig: @0x52ad3d..0x52ad46].
	if (state_.spectate_target == local) {
		state_.spectate_target = kNoTarget;
		state_.death_screen_submode = 0;
	}
	state_.mark_changed();
}

bool ClientReplicaPipeline::spectate_cycle_mode(int direction) {
	// [orig: sub_52AFF0 @0x52aff0]
	if (direction == 0) return false;
	const uint16_t local = spectate_local_handle_;
	const auto without_target = [&] {
		return state_.spectate_target == kNoTarget || state_.spectate_target == local;
	};
	int mode = state_.death_screen_submode;
	bool pick = false;
	bool place = false;
	if (direction < 0) {
		--mode; // [orig: @0x52afff]
		if (mode < 0) mode = 2; // [orig: @0x52b009]
		pick = mode == 1 || mode == 2;
		// A backward step onto free places the entity, target or not
		// [orig: `jmp short loc_52B082` @0x52b043].
		place = mode == 0;
	} else {
		++mode; // [orig: @0x52b04e]
		if (mode > 2) mode = 0; // [orig: @0x52b05b]
		pick = mode == 1 || mode == 2; // [orig: @0x52b06f]
		// The forward wrap onto free places it only with a target
		// [orig: @0x52b071..0x52b080].
		place = mode == 0 && !without_target();
	}
	state_.death_screen_submode = static_cast<uint8_t>(mode);
	state_.mark_changed();
	// A chase or first-person sub-mode with no target picks the next one
	// [orig: @0x52b013..0x52b026].
	if (pick && without_target()) spectate_cycle_target(1);
	return place;
}

bool ClientReplicaPipeline::spectate_action(int code) {
	switch (code) {
	case kSpectateActionCycleMode: // [orig: `push 1; call sub_52AFF0` @0x49bd58]
		return spectate_cycle_mode(1);
	case kSpectateActionNextTarget: // [orig: @0x49bd67..0x49bd7c]
	case kSpectateActionPrevTarget: // [orig: @0x49bd89..0x49bd9e]
		if (state_.death_screen_submode == 1 || state_.death_screen_submode == 2)
			spectate_cycle_target(code == kSpectateActionNextTarget ? 1 : -1);
		return false;
	default:
		return false;
	}
}

void ClientReplicaPipeline::spectate_track(uint16_t handle) {
	// [orig: Entity_TrySetMinimapTrackTarget @0x52abc0 — a null, the local
	//  player and a typeless entity are refused @0x52abc6..0x52abd0; the SU
	//  flag, else the player classifier with neither hidden nor dead, admits
	//  @0x52abd6..0x52abed]
	const ClientEntityState *row = state_.find(handle);
	if (row == nullptr || handle == spectate_local_handle_ || row->type_id == 0) return;
	const bool admitted = state_.scoreboard_status_suffix != 0 ||
			(row->cls == EntityClass::Player &&
					(!row->state_flags_known || (row->state_flags & 3u) == 0));
	if (!admitted) return;
	// A free sub-mode steps up to chase [orig: @0x52abf6..0x52abf8].
	if (state_.death_screen_submode == 0) state_.death_screen_submode = 1;
	state_.spectate_target = handle; // [orig: @0x52ac0c]
	state_.mark_changed();
}

void ClientReplicaPipeline::spectate_on_entity_removed(uint16_t handle) {
	// A destroyed target is dropped and the walk picks the next one from the
	// local player [orig: Entity_Destroy @0x43e820..0x43e838 — the
	// is_mp_session_peer gate holds on every client this pipeline serves].
	if (handle == kNoTarget || handle != state_.spectate_target) return;
	state_.spectate_target = kNoTarget;
	spectate_cycle_target(1);
}

void ClientReplicaPipeline::apply_spectator_mode(const std::vector<uint8_t> &body) {
	// The 2-byte spectator-mode record: bit 0 of byte 0 sets the death screen,
	// the sub-mode and the target clear, and a set bit grants the enemy tags
	// (byte 1, the slot team, lands on the connection's team latch). Short
	// reads zero-fill [orig: NapiNPClientMsg_SetSpectatorMode @0x4259e0 —
	// @0x4259f3..0x425a09 the reads, @0x425a18..0x425a3a the stores].
	const uint8_t flags = body.empty() ? 0 : body[0];
	state_.death_screen_active = (flags & 1u) != 0;
	state_.death_screen_submode = 0;
	state_.spectate_target = kNoTarget;
	if ((flags & 1u) != 0) state_.enemy_tags_visible = true;
	state_.mark_changed();
}

} // namespace opennova::replication
