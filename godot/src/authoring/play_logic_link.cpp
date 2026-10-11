#include "play_logic_link.h"

#include <cstring>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <editor/session/project_session.h>

namespace godot {

void PlayLogicLink::step(opennova::editor::ProjectSession &p_session, int64_t p_now_ms) {
	link_.step(p_session, channel_, p_now_ms);
}

void PlayLogicLink::Channel::connect(int p_port) {
	close();
	peer_.instantiate();
	if (peer_->connect_to_host("127.0.0.1", p_port) != OK) peer_.unref();
}

opennova::editor::GameMcpChannel::State PlayLogicLink::Channel::state() {
	if (peer_.is_null()) return State::Closed;
	peer_->poll();
	switch (peer_->get_status()) {
	case StreamPeerTCP::STATUS_CONNECTING: return State::Connecting;
	case StreamPeerTCP::STATUS_CONNECTED: return State::Connected;
	case StreamPeerTCP::STATUS_ERROR: return State::Failed;
	case StreamPeerTCP::STATUS_NONE: break;
	}
	return State::Closed;
}

bool PlayLogicLink::Channel::send(const std::string &p_bytes) {
	if (peer_.is_null()) return false;
	PackedByteArray data;
	data.resize(int64_t(p_bytes.size()));
	if (!p_bytes.empty()) std::memcpy(data.ptrw(), p_bytes.data(), p_bytes.size());
	if (peer_->put_data(data) != OK) {
		close();
		return false;
	}
	return true;
}

std::string PlayLogicLink::Channel::receive() {
	if (peer_.is_null()) return std::string();
	peer_->poll();
	const int32_t available = peer_->get_available_bytes();
	if (available <= 0) return std::string();
	const Array got = peer_->get_partial_data(available);
	if (got.size() != 2 || int(got[0]) != OK) return std::string();
	const PackedByteArray bytes = got[1];
	return std::string(reinterpret_cast<const char *>(bytes.ptr()), size_t(bytes.size()));
}

void PlayLogicLink::Channel::close() {
	if (peer_.is_valid()) peer_->disconnect_from_host();
	peer_.unref();
}

} // namespace godot
