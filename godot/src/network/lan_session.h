#pragma once

#include <net/npwire/lan_discovery.h>
#include <net/npwire/net_ports.h>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "network/join_target.h"
#include "network/lan_server_row.h"
#include "network/udp_pump.h"
#include "network/udp_pump_datagram_socket.h"
#include "resource_index/resource_root.h"

#include <memory>

namespace godot {

// LAN browser for the retail game-server UDP range. The engine's
// LanDiscoveryBrowser (net/npwire/lan_discovery.h) owns the browse: the probe
// identity, the 30-second window, the 3-second re-announce cadence, the reply
// filter, the 32-row cap and the first-sighting rows. This node owns only the
// device legs (the transport socket, the LanServerRow shape) and the
// MpMenuCompanion-facing signals (servers_changed per new row, browse_finished
// at the window's end).
//
// The transport is the client's one socket for the network type, which the
// join from a row goes on with (D-NET-NEW-R3-3): retail opens one transport
// socket per mode, the browse's connection is the join's, and the NovaWorld
// lobby session rides the same socket. The browse binds its own over the client
// port range unless one is installed (the NovaWorld session's, set_transport),
// keeps it open past the window's end (the rows stay joinable on it), and hands
// it to the join (take_transport). The witness rides the engine's browser
// (net/npwire/lan_discovery.h LanDiscoveryBrowser) and the shared-socket demux
// (net/npwire/datagram_demux.h).
class LanSession : public Node {
	GDCLASS(LanSession, Node)

public:
	LanSession();
	~LanSession() override;

	// The mounted game whose title the browse speaks (the launch game's with none,
	// ResourceRoot::session_title): its probes carry the title's in-match identity
	// and only hosts of the same identity list. A browse refuses to start when the
	// title's identity is not witnessed in full.
	void set_resource_root(const Ref<ResourceRoot> &root) { resource_root_ = root; }
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }

	// `connect_type` is the network connect type the browse runs under
	// (opennova::kLanConnectType*): the LAN screen browses as LAN, a NovaWorld
	// endpoint's preflight as NovaWorld; a host whose P2 names another type is
	// not listed (lan_session_admits_connect_type).
	int start_browsing(const String &destination, int port_min, int port_max, int connect_type);
	// The target's proxy-assisted NovaWorld join fields (JoinTarget's typed
	// ProxyRendezvousConfig), installed on the browse before it starts: with all
	// six set, every announce burst goes out behind the 48-byte rendezvous to the
	// game node (LanDiscoveryBrowser::burst, D-NET-712). A null target or one with
	// no proxy installs none.
	void set_proxy_from_target(const Ref<JoinTarget> &target);
	// Whether the window expires (the LAN screen's 30-second search, the default);
	// the join's enumerate-and-join runs with none (LanDiscoveryBrowser::set_windowed,
	// D-NET-703). Set before start_browsing.
	void set_windowed(bool p_windowed) { browser_.set_windowed(p_windowed); }
	bool is_windowed() const { return browser_.windowed(); }
	// The socket the browse runs on, installed before start_browsing: the NovaWorld
	// session's (NovaWorldClient.get_game_pump), whose lobby protocol shares it. Null
	// = the browse binds its own over the client port range.
	void set_transport(const Ref<UdpPump> &p_pump);
	// The browse's socket for the join that goes on from one of its rows: the browse
	// stops and forgets it (nothing closes it here); null when the browse has none.
	Ref<UdpPump> take_transport();
	// Stop the enumerator and close the browse's own socket (an installed one stays
	// open: it is its owner's).
	void stop();
	TypedArray<LanServerRow> get_servers() const;
	bool is_browsing() const { return browser_.browsing(); }
	// The browse's connection index (its 0x41s' CI); a join from one of its rows
	// carries it (LanServerRow.client_index). 0 before the first browse.
	int64_t get_client_index() const { return static_cast<int64_t>(browser_.client_index()); }

	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	void poll_replies();
	int send_probe_burst(Error &first_send_error);
	void rebuild_rows();
	void emit_error(const String &message);
	// Stop the enumerator and release the game socket; the transport stays.
	void stop_enumerator();

	opennova::LanDiscoveryBrowser browser_;
	Ref<ResourceRoot> resource_root_;
	// The transport and the game protocol's view of it while the enumerator runs.
	Ref<UdpPump> pump_;
	bool pump_installed_ = false; // pump_ is its owner's (set_transport), never closed here
	std::unique_ptr<UdpPumpGameSocket> game_;
	TypedArray<LanServerRow> servers_;
};

} // namespace godot
