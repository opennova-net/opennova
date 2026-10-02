#include "network/join_target.h"

#include "network/lan_server_row.h"

namespace godot {

Ref<JoinTarget> JoinTarget::from_lan_row(const Ref<LanServerRow> &p_row) {
	Ref<JoinTarget> target;
	target.instantiate();
	if (p_row.is_null()) {
		return target;
	}
	target->host_ip_ = p_row->get_host_ip();
	target->port_ = p_row->get_port();
	target->server_name_ = p_row->get_server_name();
	target->game_type_ = static_cast<int>(p_row->get_gametype());
	target->server_flags_ = static_cast<int>(p_row->get_server_flags());
	// The row's 0x81 SUS2 is the host's expansion: the join switches to it
	// before it connects.
	target->expansion_ = p_row->get_expansion();
	target->expansion_known_ = true;
	return target;
}

void JoinTarget::_bind_methods() {
#define JOIN_TARGET_PROPERTY(m_variant, m_name)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &JoinTarget::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &JoinTarget::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	JOIN_TARGET_PROPERTY(Variant::STRING, host_ip)
	JOIN_TARGET_PROPERTY(Variant::INT, port)
	JOIN_TARGET_PROPERTY(Variant::STRING, player_name)
	JOIN_TARGET_PROPERTY(Variant::STRING, mission)
	JOIN_TARGET_PROPERTY(Variant::STRING, dir)
	JOIN_TARGET_PROPERTY(Variant::STRING, integrity_profile)
	JOIN_TARGET_PROPERTY(Variant::STRING, app_id)
	JOIN_TARGET_PROPERTY(Variant::PACKED_BYTE_ARRAY, cd_cookie)
	JOIN_TARGET_PROPERTY(Variant::STRING, server_name)
	JOIN_TARGET_PROPERTY(Variant::INT, game_type)
	JOIN_TARGET_PROPERTY(Variant::INT, server_flags)
	JOIN_TARGET_PROPERTY(Variant::INT, network_type)
	JOIN_TARGET_PROPERTY(Variant::INT, join_role)
	JOIN_TARGET_PROPERTY(Variant::STRING, spectator_password)
	JOIN_TARGET_PROPERTY(Variant::STRING, server_password)
	JOIN_TARGET_PROPERTY(Variant::STRING, join_password)
	JOIN_TARGET_PROPERTY(Variant::INT, team_request)
	JOIN_TARGET_PROPERTY(Variant::BOOL, role_explicit)
	JOIN_TARGET_PROPERTY(Variant::STRING, proxy_node)
	JOIN_TARGET_PROPERTY(Variant::STRING, proxy_relay)
	JOIN_TARGET_PROPERTY(Variant::INT, proxy_cookie)
	JOIN_TARGET_PROPERTY(Variant::INT, lobby_number)
	JOIN_TARGET_PROPERTY(Variant::STRING, expansion)
	JOIN_TARGET_PROPERTY(Variant::BOOL, expansion_known)
#undef JOIN_TARGET_PROPERTY
	ClassDB::bind_method(D_METHOD("has_join_proxy"), &JoinTarget::has_join_proxy);
	ClassDB::bind_method(D_METHOD("allows_team_choice"), &JoinTarget::allows_team_choice);
	ClassDB::bind_method(D_METHOD("has_team_password"), &JoinTarget::has_team_password);
	ClassDB::bind_method(D_METHOD("allows_spectators"), &JoinTarget::allows_spectators);
	ClassDB::bind_method(D_METHOD("server_password_required"),
			&JoinTarget::server_password_required);
	ClassDB::bind_method(D_METHOD("spectator_password_required"),
			&JoinTarget::spectator_password_required);
	ClassDB::bind_method(D_METHOD("entry_step", "preflighted"), &JoinTarget::entry_step);
	ClassDB::bind_static_method("JoinTarget", D_METHOD("from_lan_row", "row"),
			&JoinTarget::from_lan_row);
	BIND_ENUM_CONSTANT(ROLE_PLAYER);
	BIND_ENUM_CONSTANT(ROLE_SPECTATOR);
	BIND_ENUM_CONSTANT(FLAG_TEAM_CHOICE);
	BIND_ENUM_CONSTANT(FLAG_SERVER_PASSWORD);
	BIND_ENUM_CONSTANT(FLAG_BLUE_PASSWORD);
	BIND_ENUM_CONSTANT(FLAG_RED_PASSWORD);
	BIND_ENUM_CONSTANT(FLAG_ALLOW_SPECTATORS);
	BIND_ENUM_CONSTANT(FLAG_SPECTATOR_PASSWORD);
	BIND_ENUM_CONSTANT(NETWORK_NOVAWORLD);
	BIND_ENUM_CONSTANT(NETWORK_LAN);
	BIND_ENUM_CONSTANT(ENTRY_DIAL);
	BIND_ENUM_CONSTANT(ENTRY_PREFLIGHT);
	BIND_ENUM_CONSTANT(ENTRY_PROMPT);
}

} // namespace godot
