#include <runtime/inmatch/joiner_role.h>

#include <base/io/perf_clock.h>
#include <runtime/mission/mission_kernel.h>

#include <utility>

namespace opennova::inmatch {

ClientRuntime &JoinerRole::create_runtime(const std::string &player_name, JoinRole join_role,
		const std::string &spectator_password) {
	runtime = std::make_unique<ClientRuntime>(player_name);
	runtime->set_profile(kernel_ != nullptr ? &kernel_->profile : nullptr);
	runtime->set_join_request(join_role, spectator_password);
	return *runtime;
}

void JoinerRole::bind(mission::MissionKernel &kernel) {
	Role::bind(kernel);
	if (runtime) runtime->set_profile(&kernel.profile);
}

// The joiner frame: provider wiring, hello, recv-fold + uplink + folds, the
// decoded-consequence application, the local World tick and the post-tick
// recompose live in the bridge (S10a, ADR 0028); the shell's hooks supply the
// socket, the render-coupled asset resolution, the loadout profile seams,
// device input and the view/weapon pumps. The wire leg ends where the local
// (non-authority) world work begins: the bridge fires on_wire_leg_complete
// there, which is the stats board's net split.
// [orig: Game_ProcessMainFrame @0x5263f0 -- the client-side branch of the one
//  frame function the host frame also rides]
void JoinerRole::run_tick(const TickInput &) {
	mission::MissionKernel &kernel = *kernel_;
	if (!runtime) {
		last_net_us_ = 0;
		return;
	}
	kernel.local.view_session_inputs = view_session_inputs_for(
			runtime.get(), /*joiner=*/true, runtime->local_player_dead());
	const int64_t net_start = static_cast<int64_t>(io::perf_now_us());
	JoinerWorldBridge::PumpContext ctx{
			kernel.world, *runtime, kernel.local.weapon, kernel.local.loadout,
			kernel.local.inventory, kernel.local.inventory_valid, kernel.seat_specs,
			kernel.root_motion.empty() ? nullptr : &kernel.root_motion};
	JoinerWorldBridge::PumpHooks frame_hooks = hooks;
	const auto shell_wire_leg_complete = hooks.on_wire_leg_complete;
	frame_hooks.on_wire_leg_complete = [this, net_start, &shell_wire_leg_complete] {
		last_net_us_ = static_cast<int64_t>(io::perf_now_us()) - net_start;
		if (shell_wire_leg_complete) shell_wire_leg_complete();
	};
	bridge.pump(ctx, frame_hooks);
	if (hooks.after_pump) hooks.after_pump();
	// The pump's phases, its wire leg and the local world tick's own breakdown
	// all land on the kernel's profile, once per tick.
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_NET, last_net_us_);
	const int64_t adm_start = static_cast<int64_t>(io::perf_now_us());
	kernel.resolve_new_infantry_adm_ids();
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_ADM_RESOLVE,
				static_cast<int64_t>(io::perf_now_us()) - adm_start);
}

bool JoinerRole::session_lost(SessionError &error) const {
	if (!runtime || !runtime->session_lost()) return false;
	error = {SessionErrorCode::SessionLost, runtime->session_loss_reason()};
	return true;
}

// The editor Stop/Start rewind on a joiner: the kernel's restore, then one
// exact rematerialization fold of the retained ClientState (the body-empty
// baseline removed its registry carriers; the portal tables stay, their
// handles are identical and the weld records one-shot mutable).
bool JoinerRole::reset_to_baseline(SessionError &error) {
	if (!kernel_->restore_baseline()) {
		error = {SessionErrorCode::TickFailed, "mission baseline is unavailable"};
		return false;
	}
	if (bridge.wire_header_world()) bridge.reset_materialization();
	return true;
}

// Leaving: the disconnect datagrams ride the shell's send leg.
void JoinerRole::close() {
	if (!runtime) return;
	for (const std::vector<uint8_t> &dg : runtime->disconnect())
		if (hooks.send) hooks.send(dg);
}

} // namespace opennova::inmatch
