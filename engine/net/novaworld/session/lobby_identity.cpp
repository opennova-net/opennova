#include <net/novaworld/lobby_identity.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#endif

#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace opennova {

namespace {

uint32_t fnv1a_32(std::string_view value) {
	uint32_t hash = 2166136261u;
	for (const unsigned char byte : value) {
		hash ^= byte;
		hash *= 16777619u;
	}
	return hash;
}

} // namespace

// [orig: CNapiSession_ReadLocaleInfo @0x4ce390 — the English country / language names and the base
//  time-zone Bias]
bool read_locale_identity(LobbyIdentityParams &out) {
#ifdef _WIN32
	char name[512]{};
	if (GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SENGCOUNTRY, name, static_cast<int>(sizeof(name))) > 0)
		out.country = name;
	if (GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SENGLANGUAGE, name, static_cast<int>(sizeof(name))) > 0)
		out.language = name;
	TIME_ZONE_INFORMATION timezone{};
	if (GetTimeZoneInformation(&timezone) != TIME_ZONE_ID_INVALID) out.tz_bias = std::to_string(timezone.Bias);
	return true;
#else
	(void)out;
	return false;
#endif
}

// [orig: CDKey_GenerateHardwareFingerprint @0x4a4a00 / _0 @0x4a4d00 — GetVolumeInformationA on the
//  current drive, then GetAdaptersInfo's first Ethernet adapter address]
bool read_retail_machine_inputs(RetailMachineInputs &out) {
#ifdef _WIN32
	char volume_name[MAX_PATH + 1]{};
	char filesystem_name[MAX_PATH + 1]{};
	DWORD serial = 0;
	DWORD maximum_component_length = 0;
	DWORD filesystem_flags = 0;
	if (!GetVolumeInformationA(nullptr, volume_name, static_cast<DWORD>(sizeof(volume_name)), &serial,
			&maximum_component_length, &filesystem_flags, filesystem_name,
			static_cast<DWORD>(sizeof(filesystem_name)))) {
		return false;
	}
	out.volume_serial = serial;
	out.maximum_component_length = maximum_component_length;
	out.filesystem_flags = filesystem_flags;
	out.volume_name = volume_name;
	out.filesystem_name = filesystem_name;

	ULONG adapter_bytes = 0;
	if (GetAdaptersInfo(nullptr, &adapter_bytes) == ERROR_BUFFER_OVERFLOW && adapter_bytes > 0) {
		std::vector<uint8_t> storage(adapter_bytes);
		auto *adapter = reinterpret_cast<PIP_ADAPTER_INFO>(storage.data());
		if (GetAdaptersInfo(adapter, &adapter_bytes) == ERROR_SUCCESS) {
			for (; adapter; adapter = adapter->Next) {
				if (adapter->Type != MIB_IF_TYPE_ETHERNET ||
				    adapter->AddressLength != out.ethernet_address.size())
					continue;
				std::copy_n(adapter->Address, out.ethernet_address.size(), out.ethernet_address.begin());
				out.has_ethernet_address = true;
				break;
			}
		}
	}
	return true;
#else
	(void)out;
	return false;
#endif
}

// [orig: Napi_GetExeBasename @0x62E0B0 - the first token @0x62E13E; Path_BasenameInPlace
//  @0x617530 - each backslash moves the start past it @0x617550..0x617555, the rest copied
//  @0x617566..0x617570]
std::string exe_basename_from_command_line(std::string_view command_line) {
	const std::vector<std::string> tokens = tokenize_quoted(command_line);
	if (tokens.empty()) return {};
	const std::string &path = tokens.front();
	const std::size_t slash = path.rfind('\\');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string read_exe_basename() {
#ifdef _WIN32
	const char *command_line = GetCommandLineA();
	return command_line == nullptr ? std::string() : exe_basename_from_command_line(command_line);
#else
	std::ifstream in("/proc/self/cmdline", std::ios::binary);
	std::string first;
	std::getline(in, first, '\0');
	const std::size_t slash = first.rfind('/');
	return slash == std::string::npos ? first : first.substr(slash + 1);
#endif
}

LobbyMachineTokens fallback_machine_tokens(std::string_view stable_identity) {
	const uint32_t seed = fnv1a_32(stable_identity);
	LobbyMachineTokens tokens;
	tokens.nwpssk = az_fingerprint(seed ^ 0x5053534Bu, kNwpsskLen);
	tokens.nwusid = az_fingerprint(seed ^ 0x55534944u, kNwusidLen);
	return tokens;
}

} // namespace opennova
