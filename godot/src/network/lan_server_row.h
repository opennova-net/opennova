#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace godot {

// One LAN browser row (LanSession.get_servers / servers_changed): the
// endpoint the 0x81 reply came from and the ServerHello facts it carried —
// the cp1252-decoded server name, the player counts, the game type word,
// the P2 server flags (JoinTarget.FLAG_*), the session id and the
// advertised expansion. Map identity is deliberately absent: retail LAN
// enumeration has not joined the session yet (engine:
// net/npwire/lan_discovery.h LanDiscoveryRow).
#define LAN_SERVER_ROW_FIELDS(X)        \
	X(String, server_name, String())    \
	X(String, host_ip, String())        \
	X(int, port, 0)                     \
	X(int, players, 0)                  \
	X(int, max_players, 0)              \
	X(int64_t, gametype, -1)            \
	X(int64_t, server_flags, -1)        \
	X(String, session_id, String())     \
	X(String, expansion, String())

class LanServerRow : public RefCounted {
	GDCLASS(LanServerRow, RefCounted)

public:
#define LAN_SERVER_ROW_ACCESSORS(m_type, m_name, m_default)   \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	LAN_SERVER_ROW_FIELDS(LAN_SERVER_ROW_ACCESSORS)
#undef LAN_SERVER_ROW_ACCESSORS

	// The test/fixture constructor: the endpoint plus the display name.
	static Ref<LanServerRow> make(const String &p_server_name, const String &p_host_ip, int p_port);

protected:
	static void _bind_methods();

private:
#define LAN_SERVER_ROW_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	LAN_SERVER_ROW_FIELDS(LAN_SERVER_ROW_MEMBER)
#undef LAN_SERVER_ROW_MEMBER
};

} // namespace godot
