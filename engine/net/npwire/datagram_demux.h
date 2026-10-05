#pragma once

// One bound UDP socket shared by the NP protocols built on it: the game's own
// protocol (the in-match host or client) and the NOVAWORLDUDP lobby session.
// Retail opens the transport socket once per network type and builds every
// protocol on the one NP manager that owns it: the NovaWorld lobby session is
// constructed on `g_NapiNPCtx.np_manager` (the manager the game's server or
// client protocol rides), and on network type 1 that socket binds the
// mpnovaworld quad whatever the authority, so a NovaWorld host's game port is
// the source port of its NWU datagrams and the service's only endpoint for a
// dedicated host is that source (D-NET-346).
// [orig: CNapiGameSession_InitNPConnection @0x4d3be0 — the protocol built on
//  g_NapiNPCtx.np_manager @0x4d3c6b..0x4d3c89; CNapiNetwork_OpenTransportSocket
//  @0x4c6a40 — the one open @0x4c6a7c, the NovaWorld quad @0x4c6af6..0x4c6b08]
//
// The receive side is the manager's dispatch: every datagram is offered to
// each protocol on the manager's list in turn, and the first whose opcode
// handler takes it wins; a datagram no protocol takes is dropped through the
// error callback. The handlers' own tests keep the two protocols apart: the
// server-direction opcodes (0x81..0x87) are handled only by a client-side
// connection found at the datagram's source address and port, the
// client-direction ones (0x41..0x47) only by a listening protocol or a
// server-side connection, so the lobby session takes exactly what its server
// sends it and the game protocol everything else.
// [orig: NapiNPManager_HandlePacket @0x622f10 — the protocol walk
//  @0x622f6c..0x622fac, cb_on_error(4) @0x622fdc..0x622ff9;
//  NapiNPProtocol_DispatchOpcode @0x622b40 over g_NPOpcodeHandlers @0x849d90;
//  NapiNPProtocol_HandleSessionPacket @0x626a00 — FindConnection(proto, 2,
//  addr, port) @0x626ac5, the local-key test @0x626b72;
//  NapiNPProtocol_HandleClientHello @0x6213b0 — the listening gate @0x621479]
//
// This class is that dispatch over one embedder socket: it drains the socket
// and queues each datagram for the session (when the session's claim takes it)
// or for the game (when a game protocol is attached; else it is dropped). The
// two views are the IDatagramSocket each side pumps; a recv on either drains
// the socket first, so neither side starves the other. Sends pass straight
// through: both protocols send from the one socket.

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace opennova {

class DatagramDemux {
public:
	// Whether the lobby session's protocol takes a datagram (its handlers'
	// acceptance, NwuLobbySession::claims). Unset: the socket carries no
	// session and every datagram is the game's.
	using Claim = std::function<bool(const PeerAddr &from, const uint8_t *data, std::size_t len)>;

	explicit DatagramDemux(IDatagramSocket &socket);
	DatagramDemux(const DatagramDemux &) = delete;
	DatagramDemux &operator=(const DatagramDemux &) = delete;

	void set_session_claim(Claim claim) { claim_ = std::move(claim); }
	// Whether a game protocol is on the manager's list. Detached, a datagram
	// the session does not claim reaches no handler and is dropped, as retail's
	// manager drops what no protocol takes (before the session is created, or
	// after it is destroyed); attaching starts the game's queue empty.
	void set_game_attached(bool attached);
	bool game_attached() const { return game_attached_; }

	// The lobby session's socket and the game's.
	IDatagramSocket &session() { return session_view_; }
	IDatagramSocket &game() { return game_view_; }

	// Drain every pending datagram off the socket and route it.
	void pump();

	// Datagrams dropped because no protocol took them (diagnostics).
	uint64_t dropped() const { return dropped_; }

private:
	struct Datagram {
		PeerAddr from;
		std::vector<uint8_t> bytes;
	};
	class View final : public IDatagramSocket {
	public:
		View(DatagramDemux &owner, std::deque<Datagram> &queue) : owner_(owner), queue_(queue) {}
		int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override;
		void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override;

	private:
		DatagramDemux &owner_;
		std::deque<Datagram> &queue_;
	};

	IDatagramSocket &socket_;
	Claim claim_;
	bool game_attached_ = true;
	std::deque<Datagram> session_queue_;
	std::deque<Datagram> game_queue_;
	View session_view_;
	View game_view_;
	uint64_t dropped_ = 0;
};

} // namespace opennova
