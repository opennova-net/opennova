#include "network/join_target.h"

#include "network/lan_server_row.h"

namespace godot {

opennova::inmatch::DiscoveredSessionRow JoinTarget::row_facts(const Ref<LanServerRow> &p_row) {
	opennova::inmatch::DiscoveredSessionRow row;
	if (p_row.is_null()) return row;
	row.host_ip = opennova::to_std(p_row->get_host_ip());
	row.port = p_row->get_port();
	row.server_name = opennova::to_std(p_row->get_server_name());
	row.game_type = p_row->get_gametype();
	row.server_flags = p_row->get_server_flags();
	row.expansion = opennova::to_std(p_row->get_expansion());
	row.host_key = p_row->get_host_key();
	row.password_required = p_row->get_password_required();
	row.client_index = p_row->get_client_index();
	return row;
}

Ref<JoinTarget> JoinTarget::from_lan_row(const Ref<LanServerRow> &p_row) {
	Ref<JoinTarget> target;
	target.instantiate();
	if (p_row.is_valid()) target->record_ = opennova::inmatch::join_target_from_row(row_facts(p_row));
	return target;
}

PackedByteArray JoinTarget::bytes(const std::vector<uint8_t> &p_bytes) {
	PackedByteArray out;
	out.resize(static_cast<int64_t>(p_bytes.size()));
	for (size_t i = 0; i < p_bytes.size(); ++i) out[static_cast<int64_t>(i)] = p_bytes[i];
	return out;
}

std::vector<uint8_t> JoinTarget::bytes(const PackedByteArray &p_bytes) {
	return std::vector<uint8_t>(p_bytes.ptr(), p_bytes.ptr() + p_bytes.size());
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
	JOIN_TARGET_PROPERTY(Variant::STRING, app_id)
	JOIN_TARGET_PROPERTY(Variant::PACKED_BYTE_ARRAY, cd_cookie)
	JOIN_TARGET_PROPERTY(Variant::PACKED_BYTE_ARRAY, glsvss_request)
	JOIN_TARGET_PROPERTY(Variant::STRING, server_name)
	JOIN_TARGET_PROPERTY(Variant::INT, game_type)
	JOIN_TARGET_PROPERTY(Variant::INT, server_flags)
	JOIN_TARGET_PROPERTY(Variant::INT, network_type)
	JOIN_TARGET_PROPERTY(Variant::INT, join_role)
	JOIN_TARGET_PROPERTY(Variant::STRING, spectator_password)
	JOIN_TARGET_PROPERTY(Variant::STRING, server_password)
	JOIN_TARGET_PROPERTY(Variant::STRING, join_password)
	JOIN_TARGET_PROPERTY(Variant::BOOL, role_explicit)
	JOIN_TARGET_PROPERTY(Variant::INT, lobby_number)
	JOIN_TARGET_PROPERTY(Variant::STRING, expansion)
	JOIN_TARGET_PROPERTY(Variant::BOOL, expansion_known)
	JOIN_TARGET_PROPERTY(Variant::BOOL, discovered_session)
	JOIN_TARGET_PROPERTY(Variant::INT, session_host_key)
	JOIN_TARGET_PROPERTY(Variant::BOOL, session_password_required)
	JOIN_TARGET_PROPERTY(Variant::INT, session_client_index)
#undef JOIN_TARGET_PROPERTY
	ClassDB::bind_method(D_METHOD("get_transport"), &JoinTarget::get_transport);
	ClassDB::bind_method(D_METHOD("set_transport", "pump"), &JoinTarget::set_transport);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "transport", PROPERTY_HINT_RESOURCE_TYPE, "UdpPump"),
			"set_transport", "get_transport");
	ClassDB::bind_method(D_METHOD("has_join_proxy"), &JoinTarget::has_join_proxy);
	ClassDB::bind_method(D_METHOD("has_team_password"), &JoinTarget::has_team_password);
	ClassDB::bind_method(D_METHOD("allows_spectators"), &JoinTarget::allows_spectators);
	ClassDB::bind_method(D_METHOD("server_password_required"),
			&JoinTarget::server_password_required);
	ClassDB::bind_method(D_METHOD("spectator_password_required"),
			&JoinTarget::spectator_password_required);
	ClassDB::bind_method(D_METHOD("entry_step"), &JoinTarget::entry_step);
	ClassDB::bind_method(D_METHOD("apply_pre_game_choice", "game_password", "spectate",
								 "team_password"),
			&JoinTarget::apply_pre_game_choice);
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
