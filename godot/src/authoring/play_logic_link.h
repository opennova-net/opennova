#pragma once

#include <cstdint>
#include <string>

#include <godot_cpp/classes/stream_peer_tcp.hpp>

#include <editor/session/live_logic_link.h>

namespace opennova::editor {
class ProjectSession;
}

namespace godot {

// The editor's link to the running game's MCP endpoint for its mission logic (ADR 0046 S24, Live): the byte half
// (a StreamPeerTCP to 127.0.0.1) of the session's LiveLogicLink, which does the rest (what to ask, when, and what
// the answers mean). Stepped by the editor app once a frame; it does nothing while the session does not poll (no
// game, the game install, no mission).
class PlayLogicLink {
public:
	PlayLogicLink() = default;
	PlayLogicLink(const PlayLogicLink &) = delete;
	PlayLogicLink &operator=(const PlayLogicLink &) = delete;

	void step(opennova::editor::ProjectSession &p_session, int64_t p_now_ms);

private:
	// The stream as the link's channel.
	class Channel : public opennova::editor::GameMcpChannel {
	public:
		void connect(int p_port) override;
		State state() override;
		bool send(const std::string &p_bytes) override;
		std::string receive() override;
		void close() override;

	private:
		Ref<StreamPeerTCP> peer_;
	};

	Channel channel_;
	opennova::editor::LiveLogicLink link_;
};

} // namespace godot
