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
#include <objbase.h>
#include <oleauto.h>
#include <d3d9.h>
#include <dxdiag.h>
#else
#include <unistd.h>
#endif

#include <base/io/cp1252.h>  // cp1252_strupr (the main loop's _strupr)
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)

#include <algorithm>
#include <cstdint>
#include <cwchar>
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

#ifdef _WIN32
// CLSID_DxDiagProvider and IID_IDxDiagProvider, the image's copies (no dxguid link).
// [orig: g_CLSIDDxDiagProvider @0x800110, g_IIDIDxDiagProvider @0x800100]
constexpr GUID kClsidDxDiagProvider = {
		0xA65B8071, 0x3BFE, 0x4213, {0x9A, 0x5B, 0x49, 0x1D, 0xA4, 0x46, 0x1C, 0xA7}};
constexpr GUID kIidDxDiagProvider = {
		0x9C6B4CB0, 0x23F8, 0x49CC, {0xA3, 0xED, 0x45, 0xA5, 0x50, 0x00, 0xA6, 0xD2}};

// DxDiag's display memory for the display device whose szDeviceIdentifier names `device`: its
// szDisplayMemoryEnglish's leading number, truncated; 0 with no such device or no number.
// [orig: CGfxDevice_QueryDxDiagDisplayMemory @0x67C820 - CoCreateInstance called @0x67C890,
//  Initialize {16, 111, FALSE} called @0x67C8D3, GetRootContainer called @0x67C8EC,
//  GetChildContainer("DxDiag_DisplayDevices") called @0x67C90A, GetNumberOfChildContainers called
//  @0x67C927; per child EnumChildContainerNames called @0x67C965 (256 characters),
//  GetChildContainer called @0x67C983, GetProp("szDeviceIdentifier") called @0x67C9CB,
//  IIDFromString called @0x67C9E3 when a BSTR, GUID_IsEqual_0 called @0x67C9F6; the match's
//  GetProp("szDisplayMemoryEnglish") called @0x67CA25, wcstod called @0x67CA45, no number when
//  the end is the start @0x67CA51]
int64_t dxdiag_display_memory_mb(const GUID &device) {
	IDxDiagProvider *provider = nullptr;
	if (FAILED(CoCreateInstance(kClsidDxDiagProvider, nullptr, CLSCTX_INPROC_SERVER,
			kIidDxDiagProvider, reinterpret_cast<void **>(&provider))))
		return 0;
	int64_t mb = 0;
	DXDIAG_INIT_PARAMS params{};
	params.dwSize = sizeof(params);
	params.dwDxDiagHeaderVersion = DXDIAG_DX9_SDK_VERSION;
	params.bAllowWHQLChecks = FALSE;
	IDxDiagContainer *root = nullptr;
	IDxDiagContainer *devices = nullptr;
	DWORD count = 0;
	if (SUCCEEDED(provider->Initialize(&params)) && SUCCEEDED(provider->GetRootContainer(&root)) &&
			SUCCEEDED(root->GetChildContainer(L"DxDiag_DisplayDevices", &devices)) &&
			SUCCEEDED(devices->GetNumberOfChildContainers(&count))) {
		for (DWORD i = 0; i < count; ++i) {
			WCHAR name[256] = {};
			IDxDiagContainer *child = nullptr;
			if (FAILED(devices->EnumChildContainerNames(i, name, 256)) ||
					FAILED(devices->GetChildContainer(name, &child)))
				continue;
			GUID identifier{};
			VARIANT value;
			VariantInit(&value);
			if (SUCCEEDED(child->GetProp(L"szDeviceIdentifier", &value)) && value.vt == VT_BSTR)
				IIDFromString(value.bstrVal, &identifier);
			VariantClear(&value);
			const bool match = IsEqualGUID(identifier, device) != FALSE;
			if (match) {
				VariantInit(&value);
				if (SUCCEEDED(child->GetProp(L"szDisplayMemoryEnglish", &value)) &&
						value.vt == VT_BSTR) {
					wchar_t *end = nullptr;
					const double number = std::wcstod(value.bstrVal, &end);
					if (end != value.bstrVal) mb = static_cast<int64_t>(number);
				}
				VariantClear(&value);
			}
			child->Release();
			if (match) break;
		}
	}
	if (devices != nullptr) devices->Release();
	if (root != nullptr) root->Release();
	provider->Release();
	return mb;
}
#endif

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

// [orig: Server_SendPingMetricsToGate @0x511BF0 - @0x511D88..0x511D8E, @0x511DE6..0x511DEC,
//  @0x511E3D]
bool read_gate_metrics_locale(GateMetricsLocale &out) {
#ifdef _WIN32
	char name[512]{};
	const int country = GetLocaleInfoA(0x400, LOCALE_SENGCOUNTRY, name, static_cast<int>(sizeof(name)));
	out.country_name = static_cast<signed char>(country) > 0 ? name : "";
	name[0] = '\0';
	const int language = GetLocaleInfoA(0x400, LOCALE_SENGLANGUAGE, name, static_cast<int>(sizeof(name)));
	out.language = static_cast<signed char>(language) > 0 ? name : "";
	TIME_ZONE_INFORMATION timezone{};
	GetTimeZoneInformation(&timezone);
	out.tz_bias = timezone.Bias;
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

// [orig: CGfxDevice_GetAdapterDescription @0x67C590 - the adapter count test @0x67C59F,
//  GetAdapterIdentifier called @0x67C5B8; CGfxDevice_QueryDxDiagDisplayMemory @0x67C820 - the
//  rendering adapter's identifier, GetAdapterIdentifier called @0x67C87C; CGfxDevice_CreateDevice
//  @0x67E5D0 - CoInitialize called @0x67EBE3, CoUninitialize called @0x67EBF3]
bool read_display_adapter(uint32_t adapter, HardwareIdentity &out) {
#ifdef _WIN32
	HMODULE library = LoadLibraryA("d3d9.dll");
	if (library == nullptr) return false;
	using Direct3DCreate9Fn = IDirect3D9 *(WINAPI *)(UINT);
	const auto create =
			reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(library, "Direct3DCreate9"));
	IDirect3D9 *d3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
	if (d3d == nullptr) {
		FreeLibrary(library);
		return false;
	}
	const UINT count = d3d->GetAdapterCount();
	D3DADAPTER_IDENTIFIER9 identifier{};
	out.has_adapter = count > 0;
	out.adapter_description.clear();
	if (count > 0 && SUCCEEDED(d3d->GetAdapterIdentifier(0, 0, &identifier)))
		out.adapter_description = identifier.Description;
	out.display_memory_mb = 0;
	if (adapter < count && SUCCEEDED(d3d->GetAdapterIdentifier(adapter, 0, &identifier))) {
		const HRESULT com = CoInitialize(nullptr);
		// The count in retail's 32-bit view (wow64_display_memory_mb).
		out.display_memory_mb = wow64_display_memory_mb(dxdiag_display_memory_mb(identifier.DeviceIdentifier));
		if (SUCCEEDED(com)) CoUninitialize();
	}
	d3d->Release();
	FreeLibrary(library);
	return true;
#else
	(void)adapter;
	(void)out;
	return false;
#endif
}

// [orig: System_GetCPUAndOSInfo @0x53CD30 - GlobalMemoryStatus called @0x53CF0E]
bool read_total_physical_memory(uint64_t &bytes) {
#ifdef _WIN32
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status)) return false;
	bytes = status.ullTotalPhys;
	return true;
#else
	const long pages = sysconf(_SC_PHYS_PAGES);
	const long page_size = sysconf(_SC_PAGESIZE);
	if (pages <= 0 || page_size <= 0) return false;
	bytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
	return true;
#endif
}

// [orig: Renderer_Init @0x586350 - @0x58635C / @0x586363; Config_SetDefaults @0x54D030 -
//  @0x54D0F3 / @0x54D0FB]
bool read_primary_screen_size(int32_t &width, int32_t &height) {
#ifdef _WIN32
	// SetThreadDpiAwarenessContext (Windows 10 1607 on) by name: where it is missing the process's
	// own context stands. DPI_AWARENESS_CONTEXT_UNAWARE is the handle value -1.
	using SetContextFn = HANDLE(WINAPI *)(HANDLE);
	const HMODULE user32 = GetModuleHandleA("user32.dll");
	const auto set_context = user32 == nullptr
			? nullptr
			: reinterpret_cast<SetContextFn>(
					  GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
	const HANDLE previous =
			set_context == nullptr ? nullptr : set_context(reinterpret_cast<HANDLE>(intptr_t{-1}));
	const int w = GetSystemMetrics(SM_CXSCREEN);
	const int h = GetSystemMetrics(SM_CYSCREEN);
	if (previous != nullptr) set_context(previous);
	if (w <= 0 || h <= 0) return false;
	width = w;
	height = h;
	return true;
#else
	(void)width;
	(void)height;
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

// [orig: Game_Run @0x4A7FB0 - _strupr over the process command line called @0x4A7FCF]
std::string read_menu_exe_basename() {
	return cp1252_strupr(read_exe_basename());
}

RetailMachineInputs fallback_machine_inputs(std::string_view stable_identity) {
	// The hash's successive words (an LCG over the FNV-1a seed), one per input position.
	uint32_t x = fnv1a_32(stable_identity);
	const auto next = [&x]() {
		x = x * 1664525u + 1013904223u;
		return x;
	};
	RetailMachineInputs in;
	in.volume_serial = next();
	in.maximum_component_length = next();
	in.filesystem_flags = next();
	const uint32_t adapter_low = next();
	const uint32_t adapter_high = next();
	for (std::size_t i = 0; i < 4; ++i)
		in.ethernet_address[i] = static_cast<uint8_t>(adapter_low >> (i * 8u));
	in.ethernet_address[4] = static_cast<uint8_t>(adapter_high);
	in.ethernet_address[5] = static_cast<uint8_t>(adapter_high >> 8u);
	in.has_ethernet_address = true;
	return in;
}

LobbyMachineTokens fallback_machine_tokens(std::string_view stable_identity) {
	return make_retail_machine_tokens(fallback_machine_inputs(stable_identity));
}

} // namespace opennova
