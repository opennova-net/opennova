#include <novaworld/session_protocol.h>

#include <utility>

namespace opennova {

SessionProtocolKind classify_session_protocol(std::string_view pn) {
	if (pn == "NOVAWORLDUDP") {
		return SessionProtocolKind::Lobby;
	}
	if (pn == "JointOperations" || pn == "JOINTOPERATIONS") {
		return SessionProtocolKind::JointOperations;
	}
	return SessionProtocolKind::Unsupported;
}

SessionProtocolDispatchResult dispatch_in_match_session_messages(
		SessionProtocolKind protocol,
		GameServerRuntime &runtime,
		const std::string &session_id,
		const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick) {
	SessionProtocolDispatchResult out;
	if (protocol != SessionProtocolKind::JointOperations) {
		out.label = "unsupported session protocol";
		return out;
	}

	out.handled = true;
	std::vector<ProtocolMessage> gameplay_messages;
	gameplay_messages.reserve(messages.size());
	for (const ProtocolMessage &message : messages) {
		if (message.flags.settings_update || message.full_tag >= 0x100u) {
			continue;
		}
		gameplay_messages.push_back(message);
	}

	if (!messages.empty() && gameplay_messages.empty()) {
		out.label = "in-game protocol control ignored";
		return out;
	}

	GameServerDispatch dispatch =
			runtime.handle_messages(session_id, gameplay_messages, now_tick);
	out.replies = std::move(dispatch.replies);
	out.label = std::move(dispatch.label);
	return out;
}

} // namespace opennova
