#include <base/os_registry/os_registry.h>

#include <base/io/file_io.h>
#include <base/io/strutil.h>

#include <cstdint>
#include <iterator>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace opennova {

namespace {

// The path split as NapiRegistry_OpenKeyPath splits it: the root's name, then the rest. A path
// with no subkey is refused (fewer than two components), as is a root none of retail's six names.
// [orig: NapiRegistry_OpenKeyPath @0x619e80 - Registry_CountPathComponents @0x619edf, the refusal
//  @0x619ee7; the root names @0x61a010..0x61a0ae]
struct KeyPath {
	int root = -1; // the index into kRootNames
	std::string subkey;
};

constexpr const char *kRootNames[] = { "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER",
	"HKEY_LOCAL_MACHINE", "HKEY_USERS", "HKEY_CURRENT_CONFIG", "HKEY_DYN_DATA" };
constexpr int kLocalMachine = 2; // kRootNames' HKEY_LOCAL_MACHINE

bool split_key_path(const std::string &key, KeyPath &out) {
	const std::size_t slash = key.find('\\');
	if (slash == std::string::npos || slash + 1 >= key.size())
		return false;
	const std::string_view root(key.data(), slash);
	for (int i = 0; i < static_cast<int>(std::size(kRootNames)); ++i) {
		if (strutil::iequals(root, kRootNames[i])) {
			out.root = i;
			out.subkey = key.substr(slash + 1);
			return true;
		}
	}
	return false;
}

#if defined(_WIN32)
// The root handles in kRootNames' order. [orig: NapiRegistry_OpenKeyPath @0x619e80 -
//  0x80000000..0x80000006 @0x61a01c..0x61a0b4]
HKEY root_handle(int root) {
	static const HKEY kRoots[] = { HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
		HKEY_USERS, HKEY_CURRENT_CONFIG, HKEY_DYN_DATA };
	return kRoots[root];
}

// The REG_SZ `value` under `path`, opened in the registry view `view` (KEY_WOW64_32KEY, or 0 for
// the path as named), into `out` to its first NUL: ERROR_SUCCESS; ERROR_FILE_NOT_FOUND where the
// key or the value is absent; another error where the value is no string or is longer than
// `capacity`. `out` is set only on ERROR_SUCCESS.
LONG read_value(const KeyPath &path, REGSAM view, const std::string &value, std::size_t capacity,
		std::string &out) {
	HKEY handle = nullptr;
	LONG status = RegOpenKeyExA(
			root_handle(path.root), path.subkey.c_str(), 0, KEY_QUERY_VALUE | view, &handle);
	if (status != ERROR_SUCCESS)
		return status;
	std::vector<char> data(capacity, '\0');
	DWORD type = 0;
	DWORD size = static_cast<DWORD>(capacity);
	status = RegQueryValueExA(
			handle, value.c_str(), nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &size);
	RegCloseKey(handle);
	if (status != ERROR_SUCCESS)
		return status;
	if (type != REG_SZ)
		return ERROR_INVALID_DATA;
	// The stored text to its NUL (the value as retail's buffer holds it).
	std::size_t length = 0;
	while (length < size && data[length] != '\0')
		++length;
	out.assign(data.data(), length);
	return ERROR_SUCCESS;
}

// `stored` and its NUL set as the REG_SZ `value` under `path` in the view `view`, the missing keys
// created: ERROR_SUCCESS, or the create's or the set's error (ERROR_ACCESS_DENIED where the token
// may not write there).
LONG write_value(
		const KeyPath &path, REGSAM view, const std::string &value, const std::string &stored) {
	HKEY handle = nullptr;
	DWORD disposition = 0;
	LONG status = RegCreateKeyExA(root_handle(path.root), path.subkey.c_str(), 0, nullptr, 0,
			KEY_ALL_ACCESS | view, nullptr, &handle, &disposition);
	if (status != ERROR_SUCCESS)
		return status;
	status = RegSetValueExA(handle, value.c_str(), 0, REG_SZ,
			reinterpret_cast<const BYTE *>(stored.c_str()), static_cast<DWORD>(stored.size() + 1));
	RegCloseKey(handle);
	return status;
}
#endif

// True where this process's token is elevated (TokenElevation); false where it is not, where that
// cannot be told, and off Windows.
bool token_elevated() {
#if defined(_WIN32)
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	TOKEN_ELEVATION elevation{};
	DWORD size = 0;
	const BOOL queried =
			GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
	CloseHandle(token);
	return queried && elevation.TokenIsElevated != 0;
#else
	return false;
#endif
}

} // namespace

std::string os_registry_virtual_store_key(const std::string &key, bool elevated) {
	static constexpr std::string_view kSoftware = "Software";
	KeyPath path;
	if (elevated || !split_key_path(key, path) || path.root != kLocalMachine ||
			!strutil::starts_with_icase(path.subkey, kSoftware))
		return {};
	const std::string rest = path.subkey.substr(kSoftware.size());
	if (!rest.empty() && rest.front() != '\\')
		return {};
	return "HKEY_CURRENT_USER\\Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node" +
			rest;
}

bool os_registry_read_string_virtualized(const std::string &key, const std::string &copy_key,
		const std::string &value, std::size_t capacity, const std::string &stand_in,
		std::string &out) {
	KeyPath path;
	if (!split_key_path(key, path) || value.empty() || capacity == 0)
		return false;
#if defined(_WIN32)
	(void)stand_in;
	// The copy's value first; the machine key's only where the copy holds none.
	KeyPath copy;
	if (split_key_path(copy_key, copy)) {
		const LONG status = read_value(copy, 0, value, capacity, out);
		if (status != ERROR_FILE_NOT_FOUND)
			return status == ERROR_SUCCESS;
	}
	return read_value(path, KEY_WOW64_32KEY, value, capacity, out) == ERROR_SUCCESS;
#else
	(void)copy_key;
	std::string text;
	std::string error;
	if (stand_in.empty() || !io::read_file_text(stand_in, text, error))
		return false;
	const std::size_t nul = text.find('\0');
	if (nul != std::string::npos)
		text.resize(nul);
	if (text.size() + 1 > capacity)
		return false;
	out = std::move(text);
	return true;
#endif
}

bool os_registry_write_string_virtualized(const std::string &key, const std::string &copy_key,
		const std::string &value, const std::string &text, const std::string &stand_in) {
	KeyPath path;
	if (!split_key_path(key, path) || value.empty())
		return false;
	// The text as a C string: to its first NUL (RegSetValueExA's strlen + 1).
	const std::string stored = text.substr(0, text.find('\0'));
#if defined(_WIN32)
	(void)stand_in;
	LONG status = write_value(path, KEY_WOW64_32KEY, value, stored);
	// A write the machine key refuses goes to the copy.
	KeyPath copy;
	if (status == ERROR_ACCESS_DENIED && split_key_path(copy_key, copy))
		status = write_value(copy, 0, value, stored);
	return status == ERROR_SUCCESS;
#else
	(void)copy_key;
	std::string error;
	return !stand_in.empty() && io::write_file_atomic(stand_in, stored.data(), stored.size(), error);
#endif
}

bool os_registry_read_string(const std::string &key, const std::string &value, std::size_t capacity,
		const std::string &stand_in, std::string &out) {
	return os_registry_read_string_virtualized(key,
			os_registry_virtual_store_key(key, token_elevated()), value, capacity, stand_in, out);
}

bool os_registry_write_string(const std::string &key, const std::string &value,
		const std::string &text, const std::string &stand_in) {
	return os_registry_write_string_virtualized(
			key, os_registry_virtual_store_key(key, token_elevated()), value, text, stand_in);
}

} // namespace opennova
