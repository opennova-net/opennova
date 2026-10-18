#include "server_phase.h"

#include <algorithm>

namespace opennova::novaworld_server::hosted {

const char *phase_text(Phase phase) {
	switch (phase) {
		case Phase::Queued:
			return "queued";
		case Phase::Restarting:
			return "restarting";
		case Phase::Moving:
			return "moving";
		case Phase::DataUnavailable:
			return "data_unavailable";
		case Phase::LaunchesHeld:
			return "launches_held";
		case Phase::LaunchingMachine:
			return "launching_machine";
		case Phase::WaitingForSlot:
			return "waiting_for_slot";
		case Phase::DownloadingData:
			return "downloading_data";
		case Phase::DownloadingServer:
			return "downloading_server";
		case Phase::StartingServer:
			return "starting_server";
		case Phase::SigningIn:
			return "signing_in";
		case Phase::Running:
			return "running";
		case Phase::Stopping:
			return "stopping";
		case Phase::Stopped:
			return "stopped";
		case Phase::Failed:
			return "failed";
	}
	return "queued";
}

ServerPhase server_phase(const PhaseInputs &in) {
	ServerPhase out;
	switch (in.state) {
		case State::Requested:
			out.phase = Phase::Queued;
			return out;
		case State::Listing:
			out.phase = Phase::SigningIn;
			return out;
		case State::Running:
			out.phase = Phase::Running;
			return out;
		case State::Stopping:
			out.phase = Phase::Stopping;
			return out;
		case State::Stopped:
			out.phase = Phase::Stopped;
			return out;
		case State::Failed:
			out.phase = Phase::Failed;
			return out;
		case State::Provisioning: {
			// Why its last run ended says what this wait is; with none, where the
			// fleet stands for it.
			const std::optional<StartCause> ended =
					in.last_start && !in.last_start->ended_at.empty() ? in.last_start->cause
																	  : std::nullopt;
			if ((ended == StartCause::Crash || ended == StartCause::Timeout) && in.restarts > 0) {
				out.phase = Phase::Restarting;
				out.restart = in.restarts;
				out.restart_limit = in.restart_limit;
				out.restart_cause = *ended;
			} else if (ended == StartCause::Fleet || ended == StartCause::Update) {
				out.phase = Phase::Moving;
			} else if (ended == StartCause::DataUnavailable) {
				out.phase = Phase::DataUnavailable;
				if (in.admin)
					out.line = in.last_start->reason;
			} else if (in.launches_held) {
				out.phase = Phase::LaunchesHeld;
			} else if (in.machine_launching) {
				out.phase = Phase::LaunchingMachine;
			} else {
				out.phase = Phase::WaitingForSlot;
			}
			return out;
		}
		case State::Booting: {
			// Its machine still boots: the wait is the machine's.
			if (!in.machine_state || *in.machine_state == FleetMachineState::Launching ||
					*in.machine_state == FleetMachineState::Booting) {
				out.phase = Phase::LaunchingMachine;
				return out;
			}
			const ContainerWait wait = in.report ? in.report->waiting : ContainerWait::None;
			if (wait == ContainerWait::Trees) {
				out.phase = Phase::DownloadingData;
				for (const MachineTree &t : in.trees)
					out.bytes_done += t.bytes;
				out.bytes_total = std::max(in.data_bytes_total, out.bytes_done);
			} else if (wait == ContainerWait::Image) {
				out.phase = Phase::DownloadingServer;
			} else {
				out.phase = Phase::StartingServer;
			}
			return out;
		}
	}
	return out;
}

std::optional<StoppedBecause> stopped_because(
		State state, const std::optional<ServerStart> &last_start, bool admin) {
	if (is_live(state) || !last_start || last_start->ended_at.empty() || !last_start->cause)
		return std::nullopt;
	StoppedBecause out;
	out.cause = *last_start->cause;
	out.exit_code = last_start->exit_code;
	out.oom = last_start->oom;
	// The agent's reason a start never ran is no line of the server's.
	if (admin || out.cause != StartCause::DataUnavailable)
		out.line = last_start->reason;
	return out;
}

} // namespace opennova::novaworld_server::hosted
