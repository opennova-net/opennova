// A string value of the machine's registry, as a NovaLogic executable reads and writes one: the
// one platform seam the CD-key store rides (net/novacrypto/cdkey.h KeyStore, a DFX2 client's
// activation in HKEY_LOCAL_MACHINE\Software\NovaLogic\Delta Force Xtreme 2\1.00.0000\CDKIID).
//
// `key` is the full path retail's helpers take, its first component one of the six root names
// they know (HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_USERS,
// HKEY_CURRENT_CONFIG, HKEY_DYN_DATA, in any case), the rest the subkey path; a path with no
// subkey, or another root, opens nothing.
// [orig: NapiRegistry_OpenKeyPath @0x619e80 - the root names @0x61a010..0x61a0ae, the
//  components opened in turn @0x61a0c4..0x61a165 (RegOpenKeyExA, KEY_QUERY_VALUE, or, creating,
//  RegCreateKeyExA, KEY_ALL_ACCESS), fewer than two components refused @0x619ee7]
//
// On Windows the value is the registry's as Windows hands it to a NovaLogic executable, which is
// a legacy image: 32-bit, its manifest asking no execution level. So its keys are the 32-bit view
// (KEY_WOW64_32KEY: HKLM\Software\WOW6432Node\... on a 64-bit Windows), and, under a token that is
// not elevated, UAC virtualizes them: a write the machine key refuses goes to the user's
// VirtualStore copy of it, and a read sees that copy's value first. A 64-bit process is never
// virtualized, so the access is made here as Windows makes it for those executables, decided from
// this process's own token (TokenElevation): not elevated, the VirtualStore copy's value read
// first and the machine key's where the copy holds none, a write the machine key refuses
// (ERROR_ACCESS_DENIED) made to the copy; elevated, the machine key alone. So a value stock
// dfx2.exe wrote is the one read here and the reverse, run by the same user at the same elevation
// (os_registry_virtual_store_key). (With UAC turned off a standard user's token is not elevated
// and nothing is virtualized: there the copy takes the write retail's machine key refuses.)
// Elsewhere, where no registry exists, the file `stand_in` holds the value as its whole text
// (OpenNova's stand-in, the embedder's file beside the executable).
#pragma once

#include <cstddef>
#include <string>

namespace opennova {

// The value `value` under `key`: a REG_SZ of at most `capacity` bytes, its NUL included, into
// `out` (to its first NUL). False where the key or the value is absent, the value is no string or
// is longer, or the stand-in cannot be read; `out` is then left alone.
// [orig: NapiRegistry_ReadStringAtPath @0x61a1c0 (the read) - NapiRegistry_OpenKeyPath with no
//  create @0x61a1ca; NapiRegistry_QueryStringValue (the value's read) @0x619d70 -
//  RegQueryValueExA @0x619da1, a type other than REG_SZ refused @0x619dac..0x619db5]
bool os_registry_read_string(const std::string &key, const std::string &value, std::size_t capacity,
		const std::string &stand_in, std::string &out);

// Stores `text` as the REG_SZ `value` under `key`, the missing keys of the path created, the text
// and its NUL. False where a key cannot be opened or created or the value cannot be set (for a
// token that is not elevated, in the machine key and its VirtualStore copy both), or the stand-in
// cannot be written.
// [orig: NapiRegistry_WriteStringAtPath @0x61a220 (the write) - NapiRegistry_OpenKeyPath with
//  create @0x61a22a; NapiRegistry_SetStringValue @0x619dc0 - RegSetValueExA(REG_SZ, strlen + 1)
//  @0x619e00]
bool os_registry_write_string(const std::string &key, const std::string &value,
		const std::string &text, const std::string &stand_in);

// The VirtualStore copy UAC keeps of `key` for a legacy 32-bit executable on a 64-bit Windows
// (the only one OpenNova builds for) whose token is not elevated:
// HKEY_LOCAL_MACHINE\Software\<rest>, "Software" in any case, as
// HKEY_CURRENT_USER\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\<rest>. Empty where
// `elevated` (an elevated token is never virtualized) or for a key outside
// HKEY_LOCAL_MACHINE\Software, which is never virtualized.
std::string os_registry_virtual_store_key(const std::string &key, bool elevated);

// The read and the write above with the VirtualStore copy named, where they take it from this
// process's token (os_registry_virtual_store_key(key, elevated)): `copy_key`'s value read first,
// `key`'s where the copy holds no such value; the write made to `key`, and to `copy_key` where
// `key` refuses it with ERROR_ACCESS_DENIED. An empty `copy_key` is `key` alone (an elevated
// token's access). The copy is the registry's own path, opened in no 32-bit view. Off Windows
// `copy_key` is unused.
bool os_registry_read_string_virtualized(const std::string &key, const std::string &copy_key,
		const std::string &value, std::size_t capacity, const std::string &stand_in,
		std::string &out);
bool os_registry_write_string_virtualized(const std::string &key, const std::string &copy_key,
		const std::string &value, const std::string &text, const std::string &stand_in);

} // namespace opennova
