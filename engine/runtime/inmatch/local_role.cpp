#include <runtime/inmatch/local_role.h>

#include <runtime/mission/mission_kernel.h>

namespace opennova::inmatch {

// The kernel's bare authoritative tick: pre-tick input apply, ONE
// run_logic_tick, the local view/weapon pumps, the new-soldier .adm ground,
// then the medic-cooldown leg. The view arbiter's session inputs refresh
// first (the local-dead bit lives on the world even without a session).
void LocalRole::run_tick(const TickInput &) {
	mission::MissionKernel &kernel = *kernel_;
	kernel.local.view_session_inputs = view_session_inputs_for(
			nullptr, /*joiner=*/false, kernel.local.local_player_dead());
	kernel.local.apply_player_input_pre_tick();
	kernel.world.run_logic_tick(/*is_authority=*/true, world::TickPhase::Gameplay);
	// The VM's replicated commands have no connection to reach without a
	// session; the handler already ran locally, so the tick's queue is released.
	// [orig: WacScript_ExecuteBytecode @0x4F58B0 -> NapiNPServer_SendFiltered
	//  @0x4C87E0 walks an empty connection list]
	kernel.world.out.script_remote_commands.clear();
	// The weather tick follows the entity update [orig: Game_ProcessMainFrame
	// @ 0x52674b -> @ 0x526774].
	kernel.tick_weather();
	kernel.local.run_local_player_post_tick();
	kernel.resolve_new_infantry_adm_ids();
	kernel.local.tick_medic_cooldown(kernel.local.local_player_dead()); // Player_UpdatePerFrame's cooldown leg
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
