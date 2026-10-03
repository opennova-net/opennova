#include <runtime/inmatch/local_role.h>

#include <runtime/inmatch/mission_exit.h>
#include <runtime/mission/mission_kernel.h>

#include <string>

namespace opennova::inmatch {

// The kernel's bare authoritative tick: pre-tick input apply, ONE
// run_logic_tick, the local view/weapon pumps, the new-soldier .adm ground,
// then the medic-cooldown leg. The view arbiter's session inputs refresh
// first (the local-dead bit lives on the world even without a session).
void LocalRole::run_tick(const TickInput &) {
	mission::MissionKernel &kernel = *kernel_;
	kernel.local.view_session_inputs = view_session_inputs_for(nullptr, /*joiner=*/false,
			kernel.local.local_player_dead(), kernel.world.rules.mp_session);
	// Single player sends every tick, so the input pack runs every frame
	// [orig: NapiNPServer_GetSendHoldoffTicks @0x4C4AB0 -- transport mode 0
	//  returns 1; Client_ProcessNetworkFrame @0x42C3DD -> @0x42C3E9].
	kernel.local.apply_player_input_pre_tick(/*pack_input=*/true);
	// The single-player authority runs the server tick too, so its WAC 'humans'
	// count is rebuilt ahead of the script pass as a host's is: the local player
	// keeps the world-run gate open.
	// [orig: Game_ProcessMainFrame @0x5266b4 -> Server_TickUpdate, its
	//  Server_BuildEntitySlotLists call @0x51d89a]
	kernel.world.cached.humans = kernel.world.registry.count_humans();
	kernel.world.run_logic_tick(/*is_authority=*/true, world::TickPhase::Gameplay);
	// The VM's replicated commands have no connection to reach without a
	// session; the handler already ran locally, so the tick's queue is released.
	// [orig: WacScript_ExecuteBytecode @0x4F58B0 -> NapiNPServer_SendFiltered
	//  @0x4C87E0 walks an empty connection list]
	kernel.world.out.script_remote_commands.clear();
	// The HUD relays (S2C 0x3F) likewise have no connection to reach; the
	// lines already posted locally. [orig: Server_BroadcastEntityActionPacket
	//  @0x5080D0 — the NapiNPServer_SendFiltered call @0x508199]
	kernel.world.out.hud_relays.clear();
	// A local role resolves only its own body, so no remote-player powerup
	// grant is produced here; the outbox stays clear regardless. Its own
	// `weapon` grants already landed in place, and their S2C 0x35 has no
	// connection to reach.
	kernel.world.out.powerup_grants.clear();
	kernel.world.out.powerup_weapon_grants.clear();
	// The frame tail laps onto the stats board's player-tail row; the weapon
	// walk keeps its own row.
	devtools::ProfileLap tail(kernel.world.profile);
	// The weather tick follows the entity update [orig: Game_ProcessMainFrame
	// @ 0x52674b -> @ 0x526774].
	kernel.tick_weather();
	kernel.local.run_local_view_tick();
	tail.mark(devtools::Slot::SIM_PLAYER_TAIL);
	// The frame's one weapon-action walk follows the camera compose: the
	// local player's slot pumps at its own pool-0 slot, the gunners around it.
	// [orig: Game_ProcessMainFrame -- Camera_ComputeThirdPersonView @0x526781,
	//  the WeaponAction_ProcessAllEntities call @0x526786]
	kernel.world.pump_weapon_actions();
	tail.restart();
	kernel.resolve_new_infantry_adm_ids();
	tail.mark(devtools::Slot::SIM_ADM_RESOLVE);
	kernel.local.tick_medic_cooldown(kernel.local.local_player_dead()); // Player_UpdatePerFrame's cooldown leg
	tail.mark(devtools::Slot::SIM_PLAYER_TAIL);
}

// The world's two exit values are the session layer's [orig: g_MissionExitReason].
static_assert(world::kWorldMissionExitQuit == kMissionExitQuit, "the quit reason");
static_assert(world::kWorldMissionExitRestart == kMissionExitRoundOver, "the SP restart reason");

bool LocalRole::session_lost(SessionError &error) const {
	if (kernel_ == nullptr || kernel_->world.mission_exit_reason == 0) return false;
	error = {SessionErrorCode::SessionLost,
			"mission exit " + std::to_string(kernel_->world.mission_exit_reason)};
	return true;
}

bool LocalRole::reset_to_baseline(SessionError &error) {
	if (kernel_->restore_baseline()) return true;
	error = {SessionErrorCode::TickFailed, "mission baseline is unavailable"};
	return false;
}

// The bare authority's mission exit: the teardown's pool destruction and the
// one-shot PostMission sweep [orig: Game_TeardownMission @0x52266C].
void LocalRole::close() {
	if (kernel_ != nullptr) kernel_->run_post_mission_pass(/*is_authority=*/true);
}

} // namespace opennova::inmatch
