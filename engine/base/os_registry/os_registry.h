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
// On Windows the value is the registry's, in the 32-bit view (KEY_WOW64_32KEY): the view a 32-bit
// NovaLogic executable is redirected to (HKLM\Software\WOW6432Node\... on a 64-bit Windows), so a
// value stock dfx2.exe wrote is the one read here and the reverse. Elsewhere, where no registry
// exists, the file `stand_in` holds the value as its whole text (OpenNova's stand-in, the
// embedder's file beside the executable).
#pragma once

#include <cstddef>
#include <string>

namespace opennova {

// The value `value` under `key`: a REG_SZ of at most `capacity` bytes, its NUL included, into
// `out` (to its first NUL). False where the key or the value is absent, the value is no string or
// is longer, or the stand-in cannot be read; `out` is then left alone.
// [orig: sub_61A1C0 @0x61a1c0 (the read) - NapiRegistry_OpenKeyPath with no create @0x61a1ca;
//  CScrollWnd_SetRange (a misnomer: the value's read) @0x619d70 - RegQueryValueExA @0x619da1, a
//  type other than REG_SZ refused @0x619dac..0x619db5]
bool os_registry_read_string(const std::string &key, const std::string &value, std::size_t capacity,
		const std::string &stand_in, std::string &out);

// Stores `text` as the REG_SZ `value` under `key`, the missing keys of the path created, the text
// and its NUL. False where a key cannot be opened or created (an unprivileged process on
// HKEY_LOCAL_MACHINE) or the value cannot be set, or the stand-in cannot be written.
// [orig: sub_61A220 @0x61a220 (the write) - NapiRegistry_OpenKeyPath with create @0x61a22a;
//  NapiRegistry_SetStringValue @0x619dc0 - RegSetValueExA(REG_SZ, strlen + 1) @0x619e00]
bool os_registry_write_string(const std::string &key, const std::string &value,
		const std::string &text, const std::string &stand_in);

} // namespace opennova
