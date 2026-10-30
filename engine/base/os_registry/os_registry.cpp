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
#endif

} // namespace

bool os_registry_read_string(const std::string &key, const std::string &value, std::size_t capacity,
		const std::string &stand_in, std::string &out) {
	KeyPath path;
	if (!split_key_path(key, path) || value.empty() || capacity == 0)
		return false;
#if defined(_WIN32)
	(void)stand_in;
	HKEY handle = nullptr;
	if (RegOpenKeyExA(root_handle(path.root), path.subkey.c_str(), 0,
				KEY_QUERY_VALUE | KEY_WOW64_32KEY, &handle) != ERROR_SUCCESS)
		return false;
	std::vector<char> data(capacity, '\0');
	DWORD type = 0;
	DWORD size = static_cast<DWORD>(capacity);
	const LONG queried = RegQueryValueExA(
			handle, value.c_str(), nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &size);
	RegCloseKey(handle);
	if (queried != ERROR_SUCCESS || type != REG_SZ)
		return false;
	// The stored text to its NUL (the value as retail's buffer holds it).
	std::size_t length = 0;
	while (length < size && data[length] != '\0')
		++length;
	out.assign(data.data(), length);
	return true;
#else
	(void)path;
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

bool os_registry_write_string(const std::string &key, const std::string &value,
		const std::string &text, const std::string &stand_in) {
	KeyPath path;
	if (!split_key_path(key, path) || value.empty())
		return false;
	// The text as a C string: to its first NUL (RegSetValueExA's strlen + 1).
	const std::string stored = text.substr(0, text.find('\0'));
#if defined(_WIN32)
	(void)stand_in;
	HKEY handle = nullptr;
	DWORD disposition = 0;
	if (RegCreateKeyExA(root_handle(path.root), path.subkey.c_str(), 0, nullptr, 0,
				KEY_ALL_ACCESS | KEY_WOW64_32KEY, nullptr, &handle, &disposition) != ERROR_SUCCESS)
		return false;
	const LONG set = RegSetValueExA(handle, value.c_str(), 0, REG_SZ,
			reinterpret_cast<const BYTE *>(stored.c_str()), static_cast<DWORD>(stored.size() + 1));
	RegCloseKey(handle);
	return set == ERROR_SUCCESS;
#else
	(void)path;
	std::string error;
	return !stand_in.empty() && io::write_whole(stand_in, stored.data(), stored.size(), error);
#endif
}

} // namespace opennova
