#include "listing.h"

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
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
	const JVal *get(const char *key) const {
		for (const auto &kv : o)
			if (kv.first == key) return &kv.second;
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
	bool at(char c) const { return i < src.size() && src[i] == c; }
	bool fail(const std::string &m) {
		if (err.empty()) err = m + " at offset " + std::to_string(i);
		return false;
	}
	bool str(std::string &out) {
		if (!at('"')) return fail("expected string");
		++i;
		while (i < src.size() && src[i] != '"') {
			const char c = src[i++];
			if (c != '\\' || i >= src.size()) {
				out += c;
				continue;
			}
			const char e = src[i++];
			switch (e) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u': {
				if (i + 4 > src.size()) return fail("bad \\u escape");
				const auto cp = strutil::parse_ulong(src.substr(i, 4), 16);
				if (!cp) return fail("bad \\u escape");
				i += 4;
				// Latin-1 only: the game's text is single-byte.
				out += static_cast<char>(*cp <= 0xFF ? *cp : '?');
				break;
			}
			default: out += e; break;
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
			if (at('}')) { ++i; return true; }
			for (;;) {
				ws();
				std::string k;
				if (!str(k)) return false;
				ws();
				if (!at(':')) return fail("expected ':'");
				++i;
				JVal child;
				if (!val(child)) return false;
				v.o.emplace_back(std::move(k), std::move(child));
				ws();
				if (at(',')) { ++i; continue; }
				if (at('}')) { ++i; return true; }
				return fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			v.t = JVal::T::Arr;
			++i;
			ws();
			if (at(']')) { ++i; return true; }
			for (;;) {
				JVal child;
				if (!val(child)) return false;
				v.a.push_back(std::move(child));
				ws();
				if (at(',')) { ++i; continue; }
				if (at(']')) { ++i; return true; }
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
	if (v->t == JVal::T::Num) return std::to_string(static_cast<long long>(v->n));
	return dflt;
}
int as_int(const JVal *v, int dflt) {
	if (!v) return dflt;
	if (v->t == JVal::T::Num) return static_cast<int>(v->n);
	if (v->t == JVal::T::Str) return strutil::parse_int(v->s).value_or(dflt);
	return dflt;
}
bool as_bool(const JVal *v, bool dflt) {
	return v && v->t == JVal::T::Bool ? v->b : dflt;
}

} // namespace

bool load_listing(const std::string &path, Listing &out, std::string &error) {
	std::ifstream f(io::os_path(path), std::ios::binary);
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
	Listing listing;
	HostRegistration &r = listing.columns;
	r.server_name = as_str(root.get("server_name"), "nw-lister");
	r.server_message = as_str(root.get("msg"), "");
	r.mission_name = as_str(root.get("mission"), "");
	r.game_type = as_str(root.get("game_type"), "COOP");
	r.max_players = as_int(root.get("max_players"), 32);
	r.expansion = as_str(root.get("exp"), "");
	r.password = as_bool(root.get("password"), false);
	r.locked = as_bool(root.get("locked"), false);
	r.tracers = as_bool(root.get("tracers"), true);
	r.skins = as_bool(root.get("skins"), false);
	r.dedicated_server = as_bool(root.get("dedicated"), true);
	r.listen_host = !r.dedicated_server;
	r.country = as_str(root.get("country"), r.country);
	// Whole minutes survive the Host list's / 3720; no key (or a negative one) is untimed.
	const int minutes = as_int(root.get("time_left_minutes"), -1);
	r.round_time_remaining_ticks = minutes < 0 ? -1 : minutes * 3720 + 3719;
	const std::string region = strutil::to_lower(as_str(root.get("region"), ""));
	r.region_index = region == "desert" ? 1 : region == "snow" ? 2 : 0;
	const std::string tod = strutil::to_lower(as_str(root.get("time_of_day"), ""));
	r.time_of_day = tod == "dawn" ? 1 : tod == "day" ? 2 : tod == "dusk" ? 3 : tod == "night" ? 4 : 0;

	if (const JVal *players = root.get("players"); players && players->t == JVal::T::Arr) {
		for (const JVal &e : players->a) {
			HostPlayerSlot s;
			s.slot = -1;
			s.type = "0";
			if (e.t == JVal::T::Str) {
				s.player_name = e.s;
			} else if (e.t == JVal::T::Obj) {
				s.player_name = as_str(e.get("name"), "");
				s.slot = as_int(e.get("slot"), -1);
				s.team = as_str(e.get("team"), "");
				s.ip_and_port = as_str(e.get("ip_and_port"), "");
				s.pcid = as_str(e.get("pcid"), "");
				s.type = as_str(e.get("type"), "0");
			}
			if (!s.player_name.empty()) listing.players.push_back(std::move(s));
		}
	}
	out = std::move(listing);
	return true;
}

bool load_credentials(const std::string &path, Credentials &out, std::string &error) {
	std::ifstream f(io::os_path(path), std::ios::binary);
	if (!f) {
		error = "cannot open the credentials file";
		return false;
	}
	Credentials c;
	std::string raw;
	while (std::getline(f, raw)) {
		const std::string line = strutil::trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = strutil::trim(line.substr(0, eq));
		std::string value = strutil::trim(line.substr(eq + 1));
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
