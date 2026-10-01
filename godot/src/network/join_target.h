#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/npwire/net_ports.h>
#include <runtime/inmatch/napi_np_server_ctx.h> // NetworkType
#include <runtime/inmatch/server_flags.h>

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
	// The ServerHello P2 flag bits the LAN row carries (LanServerRow.server_flags;
	// the engine's inmatch/server_flags.h names them).
	enum ServerFlag {
		FLAG_TEAM_CHOICE = opennova::inmatch::server_flag::kTeamChoice,
		FLAG_SERVER_PASSWORD = opennova::inmatch::server_flag::kServerPassword,
		FLAG_RED_PASSWORD = opennova::inmatch::server_flag::kSideBPassword,
		FLAG_BLUE_PASSWORD = opennova::inmatch::server_flag::kSideAPassword,
		FLAG_ALLOW_SPECTATORS = opennova::inmatch::server_flag::kSpectators,
		FLAG_SPECTATOR_PASSWORD = opennova::inmatch::server_flag::kSpectatorPassword,
	};
	// The network type the join rides: the menu's connect type, retail's
	// transport_mode on the client (inmatch::NetworkType carries the witness);
	// the squad talk row gates on it.
	enum NetworkType {
		NETWORK_NOVAWORLD = static_cast<int>(opennova::inmatch::NetworkType::NovaWorld),
		NETWORK_LAN = static_cast<int>(opennova::inmatch::NetworkType::Lan),
	};
	// The join entry's next step (inmatch::join_entry_step).
	enum EntryStep {
		ENTRY_DIAL = 0,
		ENTRY_PREFLIGHT = 1,
		ENTRY_PROMPT = 2,
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
	JOIN_TARGET_TEXT(server_password)
	JOIN_TARGET_TEXT(join_password)
	// The proxy-assisted NovaWorld join (the .joi NI/NP next to NK): the game
	// node "ip:port" the 48-byte rendezvous targets and the relay "ip:port" the
	// ordinary dial uses. Empty = no proxy (LAN, or a .joi without them).
	JOIN_TARGET_TEXT(proxy_node)
	JOIN_TARGET_TEXT(proxy_relay)
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
	int get_network_type() const { return network_type_; }
	void set_network_type(int p_value) {
		network_type_ = p_value == NETWORK_NOVAWORLD ? NETWORK_NOVAWORLD : NETWORK_LAN;
	}
	int get_join_role() const { return join_role_; }
	void set_join_role(int p_value) { join_role_ = p_value; }
	bool get_role_explicit() const { return role_explicit_; }
	void set_role_explicit(bool p_value) { role_explicit_ = p_value; }

	// The proxy relay cookie (the .joi BK); 0 = no proxy.
	int64_t get_proxy_cookie() const { return proxy_cookie_; }
	void set_proxy_cookie(int64_t p_value) { proxy_cookie_ = p_value; }
	bool has_join_proxy() const {
		return proxy_cookie_ != 0 && !proxy_node_.is_empty() && !proxy_relay_.is_empty();
	}
	// The .joi LN lobby number; nonzero means the dial targets the LAN-discovered
	// endpoint rather than the NK relay. 0 = the ordinary NovaWorld/LAN dial.
	int get_lobby_number() const { return lobby_number_; }
	void set_lobby_number(int p_value) { lobby_number_ = p_value; }

	int get_team_request() const { return team_request_; }
	void set_team_request(int p_value) { team_request_ = p_value == 0 || p_value == 1 ? p_value : -1; }
	bool allows_team_choice() const {
		return opennova::inmatch::server_allows_team_choice(server_flags_);
	}
	bool has_team_password() const {
		return opennova::inmatch::server_has_side_password(server_flags_);
	}
	bool server_password_required() const {
		return opennova::inmatch::server_password_required(server_flags_);
	}
	bool allows_spectators() const {
		return opennova::inmatch::server_allows_spectators(server_flags_);
	}
	bool spectator_password_required() const {
		return opennova::inmatch::server_spectator_password_required(server_flags_);
	}
	// What the shell does with this target before dialing: dial, enumerate
	// the endpoint for its flag word first (`preflighted`: that enumeration
	// already ran), or ask the player/spectator question.
	int entry_step(bool p_preflighted) const {
		opennova::inmatch::JoinEntryFacts facts;
		facts.server_flags = server_flags_;
		facts.role_explicit = role_explicit_;
		facts.spectator = join_role_ == ROLE_SPECTATOR;
		facts.server_password_given = !server_password_.is_empty();
		facts.side_password_given = !join_password_.is_empty();
		switch (opennova::inmatch::join_entry_step(facts, p_preflighted)) {
		case opennova::inmatch::JoinEntryStep::Preflight: return ENTRY_PREFLIGHT;
		case opennova::inmatch::JoinEntryStep::Prompt: return ENTRY_PROMPT;
		case opennova::inmatch::JoinEntryStep::Dial: break;
		}
		return ENTRY_DIAL;
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
	int network_type_ = NETWORK_LAN;
	int join_role_ = ROLE_PLAYER;
	String spectator_password_;
	String server_password_;
	String join_password_;
	int team_request_ = -1; // -1 automatic, 0 blue, 1 red
	bool role_explicit_ = false;
	String proxy_node_;
	String proxy_relay_;
	int64_t proxy_cookie_ = 0;
	int lobby_number_ = 0;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::JoinTarget::Role);
VARIANT_ENUM_CAST(godot::JoinTarget::ServerFlag);
VARIANT_ENUM_CAST(godot::JoinTarget::EntryStep);
VARIANT_ENUM_CAST(godot::JoinTarget::NetworkType);
