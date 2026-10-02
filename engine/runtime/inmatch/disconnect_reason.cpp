#include <runtime/inmatch/disconnect_reason.h>

#include <cstdio>

namespace opennova::inmatch {

namespace {

// The four code -> gameerr key tables, in retail's row order. A lookup walks
// them for an exact key match; a code with no row (NCC014, NDC002, every code
// past the last row) misses and takes the caller's ERR fallback.
// [orig: IntPairTable_FindByKey @0x62f480 over g_MPNetConnectCodes @0x82ba58,
//  g_MPGameConnectCodes @0x82bad8, g_MPNetDisconnectCodes @0x82bb08,
//  g_MPGameDisconnectCodes @0x82bb60]
struct CodeKey {
	uint32_t code;
	const char *key;
};

constexpr CodeKey kNetConnectCodes[] = {
		{1, "NCC001"}, {2, "NCC002"}, {3, "NCC003"}, {4, "NCC004"}, {5, "NCC005"},
		{6, "NCC006"}, {7, "NCC007"}, {8, "NCC008"}, {9, "NCC009"}, {10, "NCC010"},
		{11, "NCC011"}, {12, "NCC012"}, {13, "NCC013"}, {15, "NCC015"}, {16, "NCC016"},
};
constexpr CodeKey kGameConnectCodes[] = {
		{1, "GCC001"}, {2, "GCC002"}, {3, "GCC003"}, {4, "GCC004"}, {5, "GCC005"},
};
constexpr CodeKey kNetDisconnectCodes[] = {
		{1, "NDC001"}, {3, "NDC003"}, {4, "NDC004"}, {5, "NDC005"}, {6, "NDC006"},
		{7, "NDC007"}, {8, "NDC008"}, {9, "NDC009"}, {10, "NDC010"}, {11, "NDC011"},
};

template <size_t N>
const char *find_code(const CodeKey (&table)[N], uint32_t code) {
	for (const CodeKey &row : table)
		if (row.code == code) return row.key;
	return nullptr;
}

// The game-disconnect table holds every DPC 0..49 as "GDC%03u" (GDC049 has no
// gameerr.bin entry, so it renders as the lookup-miss marker).
std::string game_disconnect_key(uint32_t dpc) {
	if (dpc > 49) return {};
	char key[8];
	std::snprintf(key, sizeof(key), "GDC%03u", static_cast<unsigned>(dpc));
	return key;
}

constexpr const char *kMpErrors = "MP Errors";

// Every occurrence, left to right, the inserted text never rescanned.
// [orig: NapiUtil_ReplaceAllInPlace @0x6175d0]
void replace_all(std::string &text, const std::string &find, const std::string &replace) {
	size_t at = 0;
	while ((at = text.find(find, at)) != std::string::npos) {
		text.replace(at, find.size(), replace);
		at += replace.size();
	}
}

} // namespace

DisconnectReasonKey disconnect_reason_key(const ConnectionErrorRecord *conn) {
	DisconnectReasonKey out;
	if (conn == nullptr) {
		out.section = kMpErrors;
		out.key = "ERR1";
		return out;
	}
	if (conn->connect_error != 0) {
		out.substitutes = true;
		out.substitution = conn->connect_text;
		if (conn->connect_error == 14) {
			const char *key = find_code(kGameConnectCodes, conn->connect_param);
			out.section = key ? "MPGameConnectCodes" : kMpErrors;
			out.key = key ? key : "ERR3";
		} else {
			const char *key = find_code(kNetConnectCodes, conn->connect_error);
			out.section = key ? "MPNetConnectCodes" : kMpErrors;
			out.key = key ? key : "ERR2";
		}
		return out;
	}
	if (conn->disconnect_code == 0) return out;
	out.substitutes = true;
	out.substitution = conn->disconnect_text;
	if (conn->disconnect_code == 2) {
		const std::string key = game_disconnect_key(conn->disconnect_param);
		out.section = key.empty() ? kMpErrors : "MPGameDisconnectCodes";
		out.key = key.empty() ? "ERR5" : key;
	} else {
		const char *key = find_code(kNetDisconnectCodes, conn->disconnect_code);
		out.section = key ? "MPNetDisconnectCodes" : kMpErrors;
		out.key = key ? key : "ERR4";
	}
	return out;
}

std::string disconnect_reason_string(const ConnectionErrorRecord *conn,
		const rtxt::File *override_table, const rtxt::File *gameerr,
		const std::string &nw_server_message) {
	const DisconnectReasonKey key = disconnect_reason_key(conn);
	std::string out;
	if (key.section != nullptr) {
		// The 512-byte reason buffer takes at most 511 characters of the text
		// before the substitution. [orig: Napi_CopyString(buf, text, 512) @0x4c7108]
		out = rtxt::lookup_with_override(override_table, gameerr, key.section, key.key);
		if (out.size() > 511) out.resize(511);
		if (key.substitutes) replace_all(out, "[[$]]", key.substitution);
	}
	if (!nw_server_message.empty()) {
		if (out.empty())
			out = rtxt::lookup_with_override(override_table, gameerr, kMpErrors, "UNSPECIFIED");
		out += nw_server_message;
	}
	return out;
}

} // namespace opennova::inmatch
