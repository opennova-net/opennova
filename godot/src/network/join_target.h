#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "network/udp_pump.h"
#include "util/string_convert.h"

#include <runtime/inmatch/join_flow.h> // JoinTargetRecord (the record this wraps)

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

#define JOIN_TARGET_TEXT(m_name)                                            \
	String get_##m_name() const { return opennova::to_gd(record_.m_name); } \
	void set_##m_name(const String &p_value) { record_.m_name = opennova::to_std(p_value); }
	JOIN_TARGET_TEXT(host_ip)
	// The joiner's callsign; the dial fills its profile default when empty.
	JOIN_TARGET_TEXT(player_name)
	// Non-authoritative display hint; wire 0x7B/0x0B always owns the load.
	JOIN_TARGET_TEXT(mission)
	// Resource-dir override (dev/tests); empty = the persisted directory.
	JOIN_TARGET_TEXT(dir)
	JOIN_TARGET_TEXT(app_id)
	JOIN_TARGET_TEXT(server_name)
	JOIN_TARGET_TEXT(spectator_password)
	JOIN_TARGET_TEXT(server_password)
	JOIN_TARGET_TEXT(join_password)
	JOIN_TARGET_TEXT(expansion)
#undef JOIN_TARGET_TEXT
#define JOIN_TARGET_VALUE(m_type, m_name)                                       \
	m_type get_##m_name() const { return static_cast<m_type>(record_.m_name); } \
	void set_##m_name(m_type p_value) { record_.m_name = p_value; }
	JOIN_TARGET_VALUE(bool, expansion_known)
	JOIN_TARGET_VALUE(bool, discovered_session)
	JOIN_TARGET_VALUE(int64_t, session_host_key)
	JOIN_TARGET_VALUE(bool, session_password_required)
	JOIN_TARGET_VALUE(int64_t, session_client_index)
	JOIN_TARGET_VALUE(int, port)
	JOIN_TARGET_VALUE(int, game_type)
	JOIN_TARGET_VALUE(bool, role_explicit)
	JOIN_TARGET_VALUE(int, lobby_number)
#undef JOIN_TARGET_VALUE

	// The client's transport socket the join goes on with (D-NET-1019): the
	// browse's that found the row (LanSession.take_transport), which on NovaWorld is
	// the session's own (NovaWorldClient.get_game_pump). Null = the join binds its own
	// over the client port range.
	Ref<UdpPump> get_transport() const { return transport_; }
	void set_transport(const Ref<UdpPump> &p_pump) { transport_ = p_pump; }

	PackedByteArray get_cd_cookie() const { return bytes(record_.cd_cookie); }
	void set_cd_cookie(const PackedByteArray &p_cookie) { record_.cd_cookie = bytes(p_cookie); }
	PackedByteArray get_glsvss_request() const { return bytes(record_.glsvss_request); }
	void set_glsvss_request(const PackedByteArray &p_value) { record_.glsvss_request = bytes(p_value); }
	// ServerHello P2; -1 = not discovered yet.
	int get_server_flags() const { return static_cast<int>(record_.server_flags); }
	void set_server_flags(int p_value) { record_.server_flags = static_cast<int32_t>(p_value); }
	int get_network_type() const { return static_cast<int>(record_.network_type); }
	void set_network_type(int p_value) {
		record_.network_type = p_value == NETWORK_NOVAWORLD ? opennova::inmatch::NetworkType::NovaWorld
															 : opennova::inmatch::NetworkType::Lan;
	}
	int get_join_role() const { return static_cast<int>(record_.join_role); }
	void set_join_role(int p_value) {
		record_.join_role = p_value == ROLE_SPECTATOR ? opennova::inmatch::JoinRole::Spectator
													  : opennova::inmatch::JoinRole::Player;
	}

	// The proxy-assisted NovaWorld join as the engine built it (proxy_rendezvous_from_join,
	// net/novaworld/http_flow.h). C++ only; NovaWorldClient.copy_join_proxy_to fills it.
	const opennova::ProxyRendezvousConfig &get_join_proxy() const { return record_.join_proxy; }
	void set_join_proxy(const opennova::ProxyRendezvousConfig &p_proxy) { record_.join_proxy = p_proxy; }
	bool has_join_proxy() const { return record_.join_proxy.enabled(); }

	bool has_team_password() const {
		return opennova::inmatch::server_has_side_password(record_.server_flags);
	}
	bool server_password_required() const {
		return opennova::inmatch::server_password_required(record_.server_flags);
	}
	bool allows_spectators() const {
		return opennova::inmatch::server_allows_spectators(record_.server_flags);
	}
	bool spectator_password_required() const {
		return opennova::inmatch::server_spectator_password_required(record_.server_flags);
	}
	// The join screen's choice committed (inmatch::apply_pre_game_choice).
	void apply_pre_game_choice(const String &p_game_password, bool p_spectate,
			const String &p_team_password) {
		opennova::inmatch::apply_pre_game_choice(record_, opennova::to_std(p_game_password),
				p_spectate, opennova::to_std(p_team_password));
	}
	// What the join does with this target before dialing (inmatch::join_entry_step).
	int entry_step() const {
		switch (opennova::inmatch::join_entry_step(record_)) {
		case opennova::inmatch::JoinEntryStep::Preflight: return ENTRY_PREFLIGHT;
		case opennova::inmatch::JoinEntryStep::Prompt: return ENTRY_PROMPT;
		case opennova::inmatch::JoinEntryStep::Dial: break;
		}
		return ENTRY_DIAL;
	}

	// The engine record the join flow carries (inmatch/join_flow.h); C++ only.
	const opennova::inmatch::JoinTargetRecord &record() const { return record_; }
	void set_record(const opennova::inmatch::JoinTargetRecord &p_record) { record_ = p_record; }

	// A LanSession discovery row's target (inmatch::join_target_from_row); its row facts
	// (C++ only, the join flow's browse rows).
	static Ref<JoinTarget> from_lan_row(const Ref<LanServerRow> &p_row);
	static opennova::inmatch::DiscoveredSessionRow row_facts(const Ref<LanServerRow> &p_row);

protected:
	static void _bind_methods();

private:
	static PackedByteArray bytes(const std::vector<uint8_t> &p_bytes);
	static std::vector<uint8_t> bytes(const PackedByteArray &p_bytes);

	opennova::inmatch::JoinTargetRecord record_;
	Ref<UdpPump> transport_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::JoinTarget::Role);
VARIANT_ENUM_CAST(godot::JoinTarget::ServerFlag);
VARIANT_ENUM_CAST(godot::JoinTarget::EntryStep);
VARIANT_ENUM_CAST(godot::JoinTarget::NetworkType);
