// The nested ClientVarList containers an incoming lobby message carries, as the
// LobbySession dispatch tests author them: a "VarList" field naming the list,
// then one "ClientVar" child per entry with VarName / VarValue (the shape
// lobby_session's extract_var_lists parses). Distinct from the production
// opennova::make_client_var_list, which also stamps VarFNum. Header-only test
// infrastructure (no retail counterpart to cite).
#pragma once

#include <net/napi/tlv.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace test_novaworld {

inline opennova::NapiMessage make_client_var(const std::string &name, const std::string &value) {
	opennova::NapiMessage v;
	v.name = "ClientVar";
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

} // namespace test_novaworld
