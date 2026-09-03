#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/npwire/net_ports.h>

namespace godot {

class LanServerRow;

// Typed record for dialing a co-op host as a JOINER (ADR 0017): the LAN
// browser row the player activated, the NovaWorld panel's resolved join, or
// the --lan-join launch flag, carried shell -> GameWorld.load_mission_as_joiner
// -> MissionSetupOptions.join_target. Retail LAN enumeration supplies an
// ENDPOINT, not a map: the normal path authenticates first and learns the
// mission from the S2C 0x7B session record. `mission` is only a browse/debug
// display hint; the join path never opens it locally (D-NET-194). Ported from
// join_target.gd (ADR 0043 slice G5) so the mission root reads it typed.
class JoinTarget : public RefCounted {
	GDCLASS(JoinTarget, RefCounted)

public:
	enum Role {
		ROLE_PLAYER = 0,
		ROLE_SPECTATOR = 1,
	};
	// The ServerHello P2 flag bits the LAN row carries (LanServerRow.server_flags).
	enum ServerFlag {
		FLAG_ALLOW_SPECTATORS = 0x2000,
		FLAG_SPECTATOR_PASSWORD = 0x4000,
	};

#define JOIN_TARGET_TEXT(m_name)                                       \
	String get_##m_name() const { return m_name##_; }                  \
	void set_##m_name(const String &p_value) { m_name##_ = p_value; }
	JOIN_TARGET_TEXT(host_ip)
	// The joiner's callsign; the shell fills its profile default when empty.
	JOIN_TARGET_TEXT(player_name)
	// Non-authoritative display hint; wire 0x7B/0x0B always owns the load.
	JOIN_TARGET_TEXT(mission)
	// Resource-dir override (dev/tests); empty = the persisted directory.
	JOIN_TARGET_TEXT(dir)
	// Explicit registered retail corpus; empty = safe CRC silence.
	JOIN_TARGET_TEXT(integrity_profile)
	// The game-session APPID join token the client recovers from the NWJoin
	// .joi CK; a NovaWorld host validates it in ClientAuth (reject code 9).
	// "0" = the LAN default.
	JOIN_TARGET_TEXT(app_id)
	// Browse-time DISPLAY HINT for the loading screen only, never session
	// state: the authoritative value arrives post-auth in the 0x7B record.
	JOIN_TARGET_TEXT(server_name)
	JOIN_TARGET_TEXT(spectator_password)
#undef JOIN_TARGET_TEXT

	int get_port() const { return port_; }
	void set_port(int p_port) { port_ = p_port; }
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN — the
	// NovaWorld NAMEINFO/PCID/SQUADINFO/JOINTICKET the host validates (codes
	// 23/24/25/28). Empty = LAN.
	PackedByteArray get_cd_cookie() const { return cd_cookie_; }
	void set_cd_cookie(const PackedByteArray &p_cookie) { cd_cookie_ = p_cookie; }
	// Numeric g_GameType hint; -1 = unknown before authentication.
	int get_game_type() const { return game_type_; }
	void set_game_type(int p_value) { game_type_ = p_value; }
	// ServerHello P2; -1 = not discovered yet.
	int get_server_flags() const { return server_flags_; }
	void set_server_flags(int p_value) { server_flags_ = p_value; }
	int get_join_role() const { return join_role_; }
	void set_join_role(int p_value) { join_role_ = p_value; }
	bool get_role_explicit() const { return role_explicit_; }
	void set_role_explicit(bool p_value) { role_explicit_ = p_value; }

	bool allows_spectators() const {
		return server_flags_ >= 0 && (server_flags_ & FLAG_ALLOW_SPECTATORS) != 0;
	}
	bool spectator_password_required() const {
		return server_flags_ >= 0 && (server_flags_ & FLAG_SPECTATOR_PASSWORD) != 0;
	}

	// Decode a LanSession discovery row. Map identity is deliberately absent
	// here: retail LAN enumeration has not joined the session yet, so the
	// mission arrives in the normal post-auth 0x7B stream.
	static Ref<JoinTarget> from_lan_row(const Ref<LanServerRow> &p_row);

protected:
	static void _bind_methods();

private:
	String host_ip_ = "127.0.0.1";
	int port_ = opennova::kRetailLanPortMin;
	String player_name_;
	String mission_;
	String dir_;
	String integrity_profile_;
	String app_id_ = "0";
	PackedByteArray cd_cookie_;
	String server_name_;
	int game_type_ = -1;
	int server_flags_ = -1;
	int join_role_ = ROLE_PLAYER;
	String spectator_password_;
	bool role_explicit_ = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::JoinTarget::Role);
VARIANT_ENUM_CAST(godot::JoinTarget::ServerFlag);
