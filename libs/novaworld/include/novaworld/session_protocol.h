#pragma once

#include <novaworld/game_server_runtime.h>
#include <novaworld/protocol_message.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

enum class SessionProtocolKind {
	Unsupported,
	Lobby,
	JointOperations,
};

struct SessionProtocolDispatchResult {
	bool handled = false;
	std::vector<ProtocolMessage> replies;
	std::string label;
};

SessionProtocolKind classify_session_protocol(std::string_view pn);

SessionProtocolDispatchResult dispatch_in_match_session_messages(
		SessionProtocolKind protocol,
		GameServerRuntime &runtime,
		const std::string &session_id,
		const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick = 0);

} // namespace opennova
