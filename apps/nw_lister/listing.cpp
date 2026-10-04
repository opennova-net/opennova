#include "listing.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

namespace opennova::lister {
namespace {

// ---- a minimal JSON reader (objects, arrays, strings, numbers, bools, null) ----
struct JVal {
	enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<JVal> a;
	std::vector<std::pair<std::string, JVal>> o;
	const JVal *get(const std::string &k) const {
		for (const auto &kv : o)
			if (kv.first == k) return &kv.second;
		return nullptr;
	}
};

struct JParser {
	const std::string &src;
	size_t i = 0;
	std::string err;
	explicit JParser(const std::string &s) : src(s) {}
	void ws() {
		while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
	}
	bool fail(const std::string &m) {
		if (err.empty()) err = m + " at offset " + std::to_string(i);
		return false;
	}
	bool str(std::string &out) {
		if (src[i] != '"') return fail("expected string");
		++i;
		while (i < src.size() && src[i] != '"') {
			char c = src[i++];
			if (c == '\\' && i < src.size()) {
				char e = src[i++];
				switch (e) {
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u': {
					if (i + 4 > src.size()) return fail("bad \\u escape");
					const unsigned cp = static_cast<unsigned>(std::strtoul(src.substr(i, 4).c_str(), nullptr, 16));
					i += 4;
					// Latin-1 range only (the game's text is single-byte).
					out += static_cast<char>(cp <= 0xFF ? cp : '?');
					break;
				}
				default: out += e; break;
				}
			} else {
				out += c;
			}
		}
		if (i >= src.size()) return fail("unterminated string");
		++i;
		return true;
	}
	bool val(JVal &v) {
		ws();
		if (i >= src.size()) return fail("unexpected end");
		const char c = src[i];
		if (c == '{') {
			v.t = JVal::T::Obj;
			++i;
			ws();
			if (src[i] == '}') { ++i; return true; }
			for (;;) {
				ws();
				std::string k;
				if (!str(k)) return false;
				ws();
				if (src[i] != ':') return fail("expected ':'");
				++i;
				JVal child;
				if (!val(child)) return false;
				v.o.emplace_back(std::move(k), std::move(child));
				ws();
				if (src[i] == ',') { ++i; continue; }
				if (src[i] == '}') { ++i; return true; }
				return fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			v.t = JVal::T::Arr;
			++i;
			ws();
			if (src[i] == ']') { ++i; return true; }
			for (;;) {
				JVal child;
				if (!val(child)) return false;
				v.a.push_back(std::move(child));
				ws();
				if (src[i] == ',') { ++i; continue; }
				if (src[i] == ']') { ++i; return true; }
				return fail("expected ',' or ']'");
			}
		}
		if (c == '"') { v.t = JVal::T::Str; return str(v.s); }
		if (src.compare(i, 4, "true") == 0) { v.t = JVal::T::Bool; v.b = true; i += 4; return true; }
		if (src.compare(i, 5, "false") == 0) { v.t = JVal::T::Bool; v.b = false; i += 5; return true; }
		if (src.compare(i, 4, "null") == 0) { v.t = JVal::T::Null; i += 4; return true; }
		char *end = nullptr;
		v.n = std::strtod(src.c_str() + i, &end);
		if (end == src.c_str() + i) return fail("bad value");
		v.t = JVal::T::Num;
		i = static_cast<size_t>(end - src.c_str());
		return true;
	}
};

std::string as_str(const JVal *v, const std::string &dflt) {
	if (!v) return dflt;
	if (v->t == JVal::T::Str) return v->s;
	if (v->t == JVal::T::Num) {
		std::ostringstream os;
		os << static_cast<long long>(v->n);
		return os.str();
	}
	if (v->t == JVal::T::Bool) return v->b ? "1" : "0";
	return dflt;
}
int as_int(const JVal *v, int dflt) {
	if (!v) return dflt;
	if (v->t == JVal::T::Num) return static_cast<int>(v->n);
	if (v->t == JVal::T::Str && !v->s.empty()) return std::atoi(v->s.c_str());
	if (v->t == JVal::T::Bool) return v->b ? 1 : 0;
	return dflt;
}
bool as_bool(const JVal *v, bool dflt) {
	if (!v) return dflt;
	if (v->t == JVal::T::Bool) return v->b;
	if (v->t == JVal::T::Num) return v->n != 0;
	if (v->t == JVal::T::Str) return v->s == "1" || v->s == "true" || v->s == "yes" || v->s == "Y" || v->s == "y";
	return dflt;
}
const JVal *first(const JVal &o, std::initializer_list<const char *> keys) {
	for (const char *k : keys)
		if (const JVal *v = o.get(k)) return v;
	return nullptr;
}
std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
std::string trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
	return s.substr(b, e - b);
}

} // namespace

bool load_listing(const std::string &path, Listing &out, std::string &error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot open " + path;
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	std::string text = ss.str();
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3); // BOM
	JParser p(text);
	JVal root;
	if (!p.val(root) || root.t != JVal::T::Obj) {
		error = p.err.empty() ? "listing root must be an object" : p.err;
		return false;
	}
	Listing L;
	auto &r = L.reg;
	// Defaults suited to a mirrored dedicated server.
	r.listen_host = false;      // Dedicated = Yes / HostSetup Dedicated=1
	r.dedicated_server = true;  // adds CountryName/Lang/TZB (g_is_dedicated_server)
	r.allow_ping = true;
	r.tracers = true;
	r.country = "XX";
	// Lobby text tokens: the genuine .204 GSB (fixtures/novaworld/nw204_jop_2.gsb)
	// shows Y/N booleans, "NA" for no time limit, regions Jungle/Desert/Snow.
	L.text.yes = "Y";
	L.text.no = "N";
	L.text.no_time_limit = "NA";
	L.text.region = {"Jungle", "Desert", "Snow"};

	r.server_name = as_str(first(root, {"server_name", "ServerName"}), "nw-lister");
	r.server_message = as_str(first(root, {"msg", "message", "Msg"}), "");
	r.mission_name = as_str(first(root, {"mission", "map", "mission_name", "MissionName"}), "");
	r.game_type = as_str(first(root, {"game_type", "GameType"}), "COOP");
	r.max_players = as_int(first(root, {"max_players", "MaxPlayers"}), 32);
	r.password = as_bool(first(root, {"password", "Password"}), false);
	r.expansion = as_str(first(root, {"exp", "expansion", "Exp"}), "");
	r.country = as_str(root.get("country"), r.country);
	r.locked = as_bool(root.get("locked"), false);
	r.tracers = as_bool(root.get("tracers"), true);
	r.skins = as_bool(root.get("skins"), false);
	r.allow_ping = as_bool(root.get("allow_ping"), true);
	r.pb_server = as_bool(root.get("pb_server"), false);
	r.bb_mode = as_int(root.get("bb_mode"), 0);
	r.gcc = as_str(root.get("gcc"), "");
	r.version = as_str(root.get("version"), "");
	r.mi1 = as_int(root.get("mi1"), 0);
	r.mi2 = as_int(root.get("mi2"), 0);
	r.mi3 = as_int(root.get("mi3"), 0);
	r.lan_only = as_int(root.get("lan"), 0);
	r.access_code_list = as_str(root.get("access_code_list"), "");
	r.send_player_list = as_bool(root.get("send_player_list"), true);
	const bool dedicated = as_bool(root.get("dedicated"), true);
	r.listen_host = !dedicated;
	r.dedicated_server = dedicated;
	r.country_name = as_str(root.get("country_name"), "United Kingdom");
	r.language = as_str(root.get("language"), "English");
	r.tz_bias = as_int(root.get("tz_bias"), 0);
	const int tl = as_int(first(root, {"time_left_minutes", "time_left"}), -1);
	r.round_time_remaining_ticks = tl < 0 ? -1 : tl * 3720 + 3719; // whole minutes survive the /3720
	if (const JVal *tod = root.get("time_of_day")) {
		if (tod->t == JVal::T::Str) {
			const std::string t = lower(tod->s);
			r.time_of_day = t == "dawn" ? 1 : t == "day" ? 2 : t == "dusk" ? 3 : t == "night" ? 4 : 0;
		} else {
			r.time_of_day = as_int(tod, 0);
		}
	}
	if (const JVal *reg = first(root, {"region", "terrain"})) {
		if (reg->t == JVal::T::Str) {
			const std::string t = lower(reg->s);
			r.region_index = t == "jungle" ? 0 : t == "desert" ? 1 : t == "snow" ? 2 : 0;
		} else {
			r.region_index = as_int(reg, 0);
		}
	}
	if (const JVal *lt = root.get("lobby_text"); lt && lt->t == JVal::T::Obj) {
		L.text.yes = as_str(lt->get("yes"), L.text.yes);
		L.text.no = as_str(lt->get("no"), L.text.no);
		L.text.no_time_limit = as_str(lt->get("no_time_limit"), L.text.no_time_limit);
		if (const JVal *rg = lt->get("regions"); rg && rg->t == JVal::T::Arr) {
			for (size_t k = 0; k < rg->a.size() && k < 3; ++k) L.text.region[k] = as_str(&rg->a[k], "");
		}
	}
	L.lobby_name_override = as_str(root.get("lobby_name"), "");
	L.installed_exp_bits = as_str(root.get("installed_exp_bits"), "0");
	L.player_count_override = as_int(root.get("player_count"), -1);

	if (const JVal *pl = root.get("players"); pl && pl->t == JVal::T::Arr) {
		int next_slot = as_int(root.get("first_player_slot"), 0);
		for (const JVal &e : pl->a) {
			opennova::HostPlayerSlot s;
			if (e.t == JVal::T::Str) {
				s.player_name = e.s;
				s.slot = next_slot++;
			} else if (e.t == JVal::T::Obj) {
				s.player_name = as_str(first(e, {"name", "player_name"}), "");
				s.slot = as_int(e.get("slot"), next_slot);
				next_slot = s.slot + 1;
				s.ip_and_port = as_str(first(e, {"ip_and_port", "ip"}), "");
				s.pcid = as_str(e.get("pcid"), "");
				s.team = as_str(e.get("team"), "");
				s.type = as_str(e.get("type"), "0");
			} else {
				continue;
			}
			if (s.type.empty()) s.type = "0";
			if (!s.player_name.empty()) L.players.push_back(std::move(s));
		}
	}
	out = std::move(L);
	return true;
}

bool load_credentials(const std::string &path, Credentials &out, std::string &error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot open credentials file";
		return false;
	}
	std::string line;
	Credentials c;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = trim(line.substr(0, eq));
		std::string value = trim(line.substr(eq + 1));
		if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
			value = value.substr(1, value.size() - 2);
		if (key == "NOVAWORLD_USER") c.user = value;
		else if (key == "NOVAWORLD_PASS") c.pass = value;
		else if (key == "ADMIN_USER") c.admin_user = value;
		else if (key == "ADMIN_PASS") c.admin_pass = value;
	}
	out = c;
	return true;
}

} // namespace opennova::lister
