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
// connection, a 0x81 by the one whose CI it carries whoever sends it, the
// others by the one found at the datagram's source address and port; the
// client-direction ones (0x41..0x47) only by a listening protocol or a
// server-side connection. So the lobby session takes what its server sends
// it and a 0x81 its connection takes, and the game protocol everything else.
// [orig: NapiNPManager_HandlePacket @0x622f10 — the protocol walk
//  @0x622f6c..0x622fac, cb_on_error(4) @0x622fdc..0x622ff9;
//  NapiNPProtocol_DispatchOpcode @0x622b40 over g_NPOpcodeHandlers @0x849d90;
//  NapiNPProtocol_HandleSessionPacket @0x626a00 — FindConnection(proto, 2,
//  addr, port) @0x626ac5, the local-key test @0x626b72;
//  NapiNPProtocol_HandleClientHello @0x6213b0 — its refusals @0x621463..0x621479, the listening
//  gate `cmp [ebp+1F5h]` @0x621463 first, then +0x1F6 and the size]
// [orig: Nwu_HandleServerHello @0x626d20: the CI lookup @0x627533, a miss returning
//  0 @0x626dcb]
//
// This class is that dispatch over one embedder socket: it drains the socket
// and queues each datagram for a session (when that session's claim takes it)
// or for the game (when a game protocol is attached; else it is dropped). The
// views are the IDatagramSocket each side pumps; a recv on any of them drains
// the socket first, so no side starves another. Sends pass straight through:
// every protocol sends from the one socket.
//
// Retail's manager carries one lobby session. A demux built with more session
// lanes carries one NovaWorld session per lane, offered in lane order ahead of
// the game, as the manager walks its list: opennova-serve's second listing
// (ServeOptions::also_master_host, a session to a second service beside the
// first's, none of retail's) rides lane 1.

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace opennova {

class DatagramDemux {
public:
	// Whether a lobby session's protocol takes a datagram (its handlers'
	// acceptance, NwuLobbySession::claims). Unset: the lane carries no
	// session and takes nothing.
	using Claim = std::function<bool(const PeerAddr &from, const uint8_t *data, std::size_t len)>;

	// `session_lanes`: how many lobby sessions the socket carries (at least one).
	explicit DatagramDemux(IDatagramSocket &socket, std::size_t session_lanes = 1);
	DatagramDemux(const DatagramDemux &) = delete;
	DatagramDemux &operator=(const DatagramDemux &) = delete;

	// Lane `lane`'s claim (below session_lanes()). Setting or clearing it
	// starts the lane's queue empty, as attaching the game starts the game's: a
	// new session never reads what its predecessor's claim took.
	void set_session_claim(Claim claim, std::size_t lane = 0);
	// Whether a game protocol is on the manager's list. Detached, a datagram
	// no session claims reaches no handler and is dropped, as retail's
	// manager drops what no protocol takes (before the session is created, or
	// after it is destroyed); attaching starts the game's queue empty.
	void set_game_attached(bool attached);
	bool game_attached() const { return game_attached_; }

	// Lane `lane`'s lobby session socket, and the game's.
	IDatagramSocket &session(std::size_t lane = 0);
	IDatagramSocket &game() { return game_view_; }
	std::size_t session_lanes() const { return lanes_.size(); }

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
	// One lobby session's claim, queue and socket view (the view reads the lane's queue).
	struct Lane {
		explicit Lane(DatagramDemux &owner) : view(owner, queue) {}
		Claim claim;
		std::deque<Datagram> queue;
		View view;
	};

	IDatagramSocket &socket_;
	std::vector<std::unique_ptr<Lane>> lanes_;
	bool game_attached_ = true;
	std::deque<Datagram> game_queue_;
	View game_view_;
	uint64_t dropped_ = 0;
};

} // namespace opennova
