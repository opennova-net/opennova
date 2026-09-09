#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h> // decode_script_remote_command

#include <utility>
#include <vector>

namespace opennova::replication {

// The S2C 0x23 script remote command leg of the joiner fold (ADR 0043 d4): the
// host VM's replicated WAC rows arrive here and the embedding role runs the
// registry row's handler against its own world (wac::run_remote_command).
// [orig: GameMode_DispatchRemoteCommand @0x4F81E0 — the authority returns
//  before reading the body @0x4f8249; the operands decode by registry row and
//  the row's handler runs only when its flags carry 0x18 @0x4f8429]
void ClientReplicaPipeline::apply_script_remote_command(
		const std::vector<uint8_t> &body) {
	ScriptRemoteCommand command;
	size_t consumed = 0;
	if (!decode_script_remote_command(body.data(), body.size(), command, consumed)) {
		++malformed_bodies_;
		return;
	}
	if (authority_recipient_) return;
	pending_script_remote_commands_.push_back(std::move(command));
}

std::vector<ScriptRemoteCommand> ClientReplicaPipeline::drain_script_remote_commands() {
	std::vector<ScriptRemoteCommand> out;
	out.swap(pending_script_remote_commands_);
	return out;
}

} // namespace opennova::replication
