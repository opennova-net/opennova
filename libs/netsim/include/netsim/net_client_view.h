#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <novaworld/ingame_decode.h> // EntityClass

#include "netsim/client_state.h"
#include "netsim/session_transport.h"

namespace opennova::netsim {

// The local client's decode pump: drains S2C datagrams off the loopback and folds
// them into a ClientState via the witnessed ingame_decode codec. This is the "local
// client decodes them via libs/novaworld/ingame_decode" half of the SP in-process
// listen server (ADR 0011). The same ClientState feeds the Godot present pass.
class NetClientView {
public:
	NetClientView();
	explicit NetClientView(std::function<EntityClass(uint16_t)> resolver);

	// Drain every pending S2C datagram and apply it to the held ClientState.
	void pump(ISessionTransport &channel);

	const ClientState &state() const { return state_; }
	ClientState &state() { return state_; }
	std::uint32_t frames_applied() const { return state_.frames_applied; }
	std::size_t unknown_tags() const { return unknown_tags_; }

private:
	void apply_frame_update(const std::vector<uint8_t> &body);

	ClientState state_;
	std::function<EntityClass(uint16_t)> resolver_;
	std::size_t unknown_tags_ = 0;
};

} // namespace opennova::netsim
