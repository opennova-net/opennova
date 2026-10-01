// The session-variable list -- see session_vars.h.

#include <net/npwire/session_vars.h>

#include <base/io/le.h>
#include <base/io/strutil.h>

#include <cstring>

namespace opennova {

namespace {

void append_kv(std::vector<uint8_t> &out, const char *name, const void *data, uint32_t len) {
	const size_t name_len = std::strlen(name);
	out.insert(out.end(), name, name + name_len);
	out.push_back(0);
	io::append_u32_le(out, len);
	const auto *bytes = static_cast<const uint8_t *>(data);
	out.insert(out.end(), bytes, bytes + len);
}

void append_string_kv(std::vector<uint8_t> &out, const char *name, const std::string &value) {
	append_kv(out, name, value.c_str(), static_cast<uint32_t>(value.size() + 1));
}

// Napi_CopyString(dst, src, n): at most n - 1 characters, stopping at the
// source NUL, always terminated -- bounded here by the stream's end.
std::string copy_capped(const uint8_t *src, std::size_t available, std::size_t cap) {
	std::size_t n = 0;
	while (n + 1 < cap && n < available && src[n] != 0) ++n;
	return std::string(reinterpret_cast<const char *>(src), n);
}

} // namespace

// [orig: Game_SerializeMissionInfoToDataStream @0x523620 -- the key order
//  @0x5236ec, @0x523871, @0x5238cb, @0x52394e, @0x5239ec, @0x523a59]
std::vector<uint8_t> encode_session_vars(const SessionVars &vars) {
	std::vector<uint8_t> out;
	append_string_kv(out, "SERVERNAME", vars.server_name);
	append_string_kv(out, "MISSIONNAME", vars.mission_name);
	const uint8_t game_type[4] = {
		static_cast<uint8_t>(vars.game_type & 0xFFu),
		static_cast<uint8_t>((vars.game_type >> 8) & 0xFFu),
		static_cast<uint8_t>((vars.game_type >> 16) & 0xFFu),
		static_cast<uint8_t>((vars.game_type >> 24) & 0xFFu),
	};
	append_kv(out, "GAMETYPE", game_type, 4);
	append_string_kv(out, "CUSTOMTEXT", vars.custom_text);
	append_string_kv(out, "MISSIONFILENAME", vars.mission_file);
	const uint8_t exp_fanfare[2] = {
		static_cast<uint8_t>(vars.exp_fanfare & 0xFFu),
		static_cast<uint8_t>((vars.exp_fanfare >> 8) & 0xFFu),
	};
	append_kv(out, "EXP_FANFARE", exp_fanfare, 2);
	return out;
}

// [orig: Client_ParseServerSessionVariables @0x5202f0 -- the entry walk
//  @0x52036d..0x52048a]
void decode_session_vars(const uint8_t *data, std::size_t size, SessionVars &vars) {
	vars.server_name.clear();
	vars.mission_name.clear();
	vars.game_type = 0;
	vars.custom_text.clear();
	vars.mission_file.clear();
	if (data == nullptr || size == 0)
		return;
	std::size_t pos = 0;
	while (pos < size) {
		const uint8_t *key = data + pos;
		std::size_t key_len = 0;
		while (pos + key_len < size && key[key_len] != 0) ++key_len;
		if (pos + key_len >= size)
			break;
		const std::string name = copy_capped(key, key_len + 1, 512);
		pos += key_len + 1;
		if (pos + 4 > size)
			break;
		const uint32_t value_len = io::read_u32_le(data + pos);
		pos += 4;
		const uint8_t *value = data + pos;
		const std::size_t available = size - pos;
		if (strutil::iequals(name, "SERVERNAME"))
			vars.server_name = copy_capped(value, available, 32);
		else if (strutil::iequals(name, "MISSIONNAME"))
			vars.mission_name = copy_capped(value, available, 64);
		else if (strutil::iequals(name, "GAMETYPE")) {
			if (available >= 4)
				vars.game_type = io::read_u32_le(value);
		} else if (strutil::iequals(name, "CUSTOMTEXT"))
			vars.custom_text = copy_capped(value, available, 512);
		else if (strutil::iequals(name, "MISSIONFILENAME"))
			vars.mission_file = copy_capped(value, available, 64);
		else if (strutil::iequals(name, "EXP_FANFARE")) {
			if (available >= 2)
				vars.exp_fanfare = static_cast<uint16_t>(value[0] | (value[1] << 8));
		}
		if (value_len > available)
			break;
		pos += value_len;
	}
}

} // namespace opennova
