// ClientRuntime's mission-start pump legs: Game_StartMission's send and receive pumps, its
// periodic updates, the load's first leg and the push's end (split off client_runtime.cpp).
#include <runtime/inmatch/client_runtime.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace opennova::inmatch {

// Game_StartMission's pumps between the 0x1A and the final wait for the S2C 0x0F. A send pump
// builds only at a spent countdown and reloads it from the dictated period, and every send pump
// call ages the flush; it writes its NET SENDING lines at its send, between the load's receives.
// A receive pump is the client protocol's alone (flags 26: no manager receive): it runs the client
// pump over what the load's periodic updates queued (D-NET-NEW-L2-54), then steps the countdown.
// [orig: CNapiNPConnection_PumpFlags @0x629780 - the step @0x6297ac, the flush @0x6297d5, the
//  reload @0x629802; CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - the build gate
//  @0x62927b; CNapiNetwork_PumpClientProtocolRecv @0x4C4FE0 (flags 26, no manager receive);
//  NapiNPManager_Pump @0x62AC40 - the socket read only under flag 4 (NapiNPManager_PumpReceive
//  @0x623010); NetClient_FlushAndSync @0x424710 (the send pump @0x42472c)]
void ClientRuntime::mission_start_send_pump(std::vector<std::vector<uint8_t>> &outbound) {
	if (send_holdoff_countdown_ != 0) {
		joiner_->complete_send_flush();
		return;
	}
	const std::size_t built_from = outbound.size();
	build_send_boundary(outbound, current_tick_);
	for (std::size_t i = built_from; i < outbound.size(); ++i) joiner_->connect_log_send(outbound[i]);
	send_holdoff_countdown_ = send_holdoff_ticks_;
}

// The session's periodic update at a loading step: a NovaWorld join's reads the game's socket
// (the role's deposit, then the manager receive), then runs the NovaWorld session's own half
// (ClientProcessState::nwu_periodic_update, D-NET-NEW-L9-25); a LAN session's returns at its head
// (D-NET-NEW-L2-54; the header carries the witness) [orig: CNapiGameSession_ProcessPeriodicUpdate
// @0x4D4400 - @0x4D4403 / @0x4D4411, NapiNPManager_Pump(flags 4) @0x4D441F, NapiNPProtocol_Pump(-1)
// @0x4D442E].
void ClientRuntime::mission_start_periodic_update(std::vector<std::vector<uint8_t>> &outbound,
		const MissionStartPumpLegs &legs) {
	if (!legs.periodic_deposit) return;
	legs.periodic_deposit();
	manager_receive_pass(outbound);
	if (process_state().nwu_periodic_update) process_state().nwu_periodic_update();
}

// A receive pump of the load: the client pump, the role's handler writes (none after a teardown
// in it), then the countdown's step [orig: Game_StartMission @0x524360 - the receives @0x525784 /
// @0x525BD9; NapiNPProtocol_Pump @0x62A650 - PumpRecvQueues @0x62A6AC, PumpFlags @0x62A6F8].
void ClientRuntime::mission_start_receive_pump(std::vector<std::vector<uint8_t>> &outbound,
		const MissionStartPumpLegs &legs) {
	receive_send_marks_.clear();
	receive_loadout_grants_.clear();
	receive_loadout_revision_base_ = authoritative_loadout_revision_;
	if (!client_receive_pass(outbound)) return;
	if (legs.after_receive) legs.after_receive();
	receive_send_marks_.clear();
	if (send_holdoff_countdown_ > 0) --send_holdoff_countdown_;
}

// The load's first leg, once its definition and model loads are in: the send pump the load ran
// ahead of them, the updates, its receive, the first 0x2F built past it and queued, and the four
// send pumps ahead of the model freeze behind the steps' updates, so a countdown spent there puts
// the first 0x2F on the wire alone, mid-load (D-NET-NEW-L2-35, D-NET-NEW-L2-54).
// [orig: Game_StartMission @0x524360 - NetClient_FlushAndSync @0x5253EF, the loads
//  @0x525495..0x525762 with their updates @0x525405..0x52571F, the receive @0x525784, the update
//  @0x52578E, g_LocalTeam @0x525793, the first 0x2F @0x525836, the update @0x525889,
//  NetClient_FlushAndSync @0x5258A2, the update @0x5258BC, @0x5258EA, the updates @0x525962 /
//  @0x5259E7, @0x5259F1, the update @0x525A57, @0x525A61 (ahead of Render_InitMissionTextures @0x525A6E, the model freeze inside it)]
bool ClientRuntime::mission_start_first_leg(std::vector<std::vector<uint8_t>> &outbound,
		const MissionStartPumpLegs &legs) {
	if (joiner_ == nullptr || !joiner_->mission_start_first_loadout_owed()) return false;
	mission_start_send_pump(outbound);
	mission_start_periodic_update(outbound, legs);
	mission_start_receive_pump(outbound, legs);
	mission_start_periodic_update(outbound, legs);
	ProtocolMessage first;
	if (joiner_->take_mission_start_first_loadout(first)) send_queue_.push_back(std::move(first));
	for (int call = 0; call < 4; ++call) {
		mission_start_periodic_update(outbound, legs);
		mission_start_send_pump(outbound);
	}
	return true;
}

std::vector<std::vector<uint8_t>> ClientRuntime::push_mission_start_first_loadout(
		const MissionStartPumpLegs &legs) {
	std::vector<std::vector<uint8_t>> outbound;
	(void)mission_start_first_leg(outbound, legs);
	receive_sends_logged_ = 0; // every send wrote its line at its place
	receive_leave_logged_ = false;
	return outbound;
}

std::vector<std::vector<uint8_t>> ClientRuntime::pump_mission_start_manager_receive() {
	std::vector<std::vector<uint8_t>> outbound;
	if (!declared_load_running()) return outbound;
	manager_receive_pass(outbound);
	receive_sends_logged_ = 0; // their lines written inside the receive
	return outbound;
}

std::vector<std::vector<uint8_t>> ClientRuntime::push_mission_start_end(
		const MissionStartEndLegs &legs) {
	std::vector<std::vector<uint8_t>> outbound;
	if (joiner_ == nullptr || !joiner_->mission_start_released()) return outbound;
	// The first leg here when the embedder's load did not run it at its point.
	(void)mission_start_first_leg(outbound, legs);
	// The update and the pump after the freeze, the update after them, then the load's zero of
	// the frame tick (no main frame runs until the final wait ends), Player_InitPlayer (the
	// submissions' take binds the local player entity, the role's leg inits it), the receive, the
	// update past it, the second 0x2F, the 100 % present and its update, the 0x0B queued, the
	// last send. What the update past the receive reads reaches no handler ahead of the second
	// 0x2F: it waits for the final wait's first receive (D-NET-NEW-L2-54).
	// [orig: Game_StartMission @0x524360 - the update @0x525A8E, NetClient_FlushAndSync @0x525ACA,
	//  the update @0x525B09, `mov g_CurrentTick, ebx` @0x525B9F, Player_InitPlayer @0x525BBC, the
	//  receive @0x525BD9, the update @0x525BE3, the second 0x2F @0x525C2E, the present @0x525D29,
	//  the update @0x525D33, the 0x0B @0x52624B queued, the send @0x526255]
	mission_start_periodic_update(outbound, legs);
	mission_start_send_pump(outbound);
	mission_start_periodic_update(outbound, legs);
	view_.zero_frame_tick();
	JoinerConnection::MissionStartSubmissions submissions;
	(void)joiner_->take_mission_start_submissions(submissions);
	if (legs.init_player) legs.init_player();
	mission_start_receive_pump(outbound, legs);
	mission_start_periodic_update(outbound, legs);
	// The second 0x2F's words as they stand past the init and the receive (the team, the
	// side's class, the equipped slot), read at its build (D-NET-NEW-L2-53).
	// [orig: Game_StartMission @0x524360 - g_LocalTeam @0x525BE8, g_CurrentWeaponSlot
	//  @0x525C1A, NetPacket_SendLoadoutSubmit @0x525C2E]
	send_queue_.push_back(joiner_->mission_start_second_loadout());
	// The block on the local entity: its indoors bit and revive latch cleared, unhidden,
	// every proximity list rebuilt, hidden again (the header carries the witness).
	if (legs.start_block) legs.start_block();
	// The loading screen's 100 %, past the second 0x2F's queue and ahead of the 0x0B's: the
	// mission start presents it itself, ahead of the final wait (the header carries the
	// witness, D-NET-NEW-L8-41).
	if (legs.present) legs.present();
	mission_start_periodic_update(outbound, legs);
	// The Attack & Defend latch, past the present and ahead of the 0x0B's queue.
	if (legs.latch) legs.latch();
	send_queue_.push_back(joiner_->mission_start_status());
	mission_start_send_pump(outbound);
	// The push ends here. What the countdown still holds waits in the queue for the final
	// wait for the S2C 0x0F, whose every pass reads the socket and runs the receive
	// handlers before its send: everything that arrived since the load's last read is folded
	// first, its ACK rides the held submissions' packet and its handlers' replies queue behind
	// them. That is the WorldStateWait loop's first pass, its receive then its send, which
	// the role runs right after this push, in the same call (JoinerRole::push_mission_start_end).
	// [orig: Game_StartMission @0x526295..0x5262e5 - PumpManagerReceive @0x5262b4,
	//  PumpClientProtocolRecv @0x5262be, PumpClientProtocolSend @0x5262da]
	receive_sends_logged_ = 0; // every send wrote its line at its place
	receive_leave_logged_ = false;
	return outbound;
}

} // namespace opennova::inmatch
