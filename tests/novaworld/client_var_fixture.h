// The nested ClientVarList containers an incoming lobby message carries, as the
// LobbySession dispatch tests author them: a "VarList" field naming the list,
// then one "ClientVar" child per entry with VarFNum / VarName / VarValue (the
// shape lobby_session's extract_var_lists parses; VarFNum is the retail
// per-entry index, 0 for a plain variable and the player slot for the indexed
// PlayerList). Distinct from the production opennova::make_client_var_list.
// Header-only test infrastructure (no retail counterpart to cite).
#pragma once

#include <net/napi/tlv.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace test_novaworld {

inline opennova::NapiMessage make_client_var(const std::string &name, const std::string &value,
                                             int fnum = 0) {
	opennova::NapiMessage v;
	v.name = "ClientVar";
	const std::string fnum_text = std::to_string(fnum);
	v.fields.push_back({"VarFNum",  std::vector<uint8_t>(fnum_text.begin(), fnum_text.end())});
	v.fields.push_back({"VarName",  std::vector<uint8_t>(name.begin(),  name.end())});
	v.fields.push_back({"VarValue", std::vector<uint8_t>(value.begin(), value.end())});
	return v;
}

inline opennova::NapiMessage make_client_var_list(const std::string &list_name,
		const std::vector<std::pair<std::string, std::string>> &entries) {
	opennova::NapiMessage l;
	l.name = "ClientVarList";
	l.fields.push_back({"VarList", std::vector<uint8_t>(list_name.begin(), list_name.end())});
	for (const auto &[k, v] : entries) {
		l.children.push_back(make_client_var(k, v));
	}
	return l;
}

// One indexed entry: {fnum, name, value}.
struct IndexedVar {
	int fnum;
	std::string name;
	std::string value;
};

inline opennova::NapiMessage make_indexed_var_list(const std::string &list_name,
		const std::vector<IndexedVar> &entries) {
	opennova::NapiMessage l;
	l.name = "ClientVarList";
	l.fields.push_back({"VarList", std::vector<uint8_t>(list_name.begin(), list_name.end())});
	for (const auto &e : entries) {
		l.children.push_back(make_client_var(e.name, e.value, e.fnum));
	}
	return l;
}

// The five retail per-slot PlayerList vars for one player
// [orig: Server_PlayerAdd @0x51d441..0x51d4aa].
inline std::vector<IndexedVar> player_slot_vars(int slot, const std::string &name,
		const std::string &ip_and_port, const std::string &pcid,
		const std::string &team, const std::string &type) {
	return {
		{slot, "PlayerName", name},
		{slot, "PlayerIpAndPort", ip_and_port},
		{slot, "PlayerPCID", pcid},
		{slot, "PlayerTeam", team},
		{slot, "PlayerType", type},
	};
}

} // namespace test_novaworld
