// A registry key of this run's own for the tests that write the registry, which write only here
// (never a NovaLogic key): HKEY_CURRENT_USER\Software\OpenNovaTest\<stem>_<pid> (test_paths_unique,
// so two runs at once never touch each other's keys), deleted with everything under it on
// construction (an earlier run's leftover) and on destruction, as is
// HKEY_CURRENT_USER\Software\OpenNovaTest once nothing else is under it. `deny_writes` makes a key
// under it one this user may read and not write, as HKEY_LOCAL_MACHINE is to a token that is not
// elevated: its DACL grants Everyone KEY_READ alone, and the key's owner, this user, may always set
// its DACL again, which the delete does first. Windows only. Infrastructure only.
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

#include "common/test_paths.h"

namespace test_registry {

class ScratchKey {
public:
	explicit ScratchKey(const char *stem) :
			subkey_(std::string(kParent) + "\\" + test_paths_unique(stem)) {
		delete_tree(subkey_);
	}
	~ScratchKey() {
		delete_tree(subkey_);
		RegDeleteKeyA(HKEY_CURRENT_USER, kParent); // refused while another run's key is under it
	}
	ScratchKey(const ScratchKey &) = delete;
	ScratchKey &operator=(const ScratchKey &) = delete;

	// The full path of `child` under the key, as base/os_registry takes one.
	std::string path(const char *child) const {
		return "HKEY_CURRENT_USER\\" + subkey_ + "\\" + child;
	}

	// `child` created, then made one this user may read and not write.
	bool deny_writes(const char *child) {
		HKEY handle = nullptr;
		const std::string sub = subkey_ + "\\" + child;
		if (RegCreateKeyExA(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
					&handle, nullptr) != ERROR_SUCCESS)
			return false;
		const bool set = set_dacl(handle, "D:P(A;;KR;;;WD)");
		RegCloseKey(handle);
		return set;
	}

private:
	static constexpr const char *kParent = "Software\\OpenNovaTest";

	static bool set_dacl(HKEY handle, const char *sddl) {
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
					sddl, SDDL_REVISION_1, &descriptor, nullptr))
			return false;
		const LONG status = RegSetKeySecurity(handle, DACL_SECURITY_INFORMATION, descriptor);
		LocalFree(descriptor);
		return status == ERROR_SUCCESS;
	}

	// `sub` and everything under it deleted, each key's writes given back first.
	static void delete_tree(const std::string &sub) {
		HKEY handle = nullptr;
		if (RegOpenKeyExA(HKEY_CURRENT_USER, sub.c_str(), 0, WRITE_DAC, &handle) != ERROR_SUCCESS)
			return;
		set_dacl(handle, "D:(A;;KA;;;WD)");
		RegCloseKey(handle);
		std::vector<std::string> children;
		if (RegOpenKeyExA(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &handle) ==
				ERROR_SUCCESS) {
			char name[256];
			for (DWORD i = 0;; ++i) {
				DWORD length = sizeof(name);
				if (RegEnumKeyExA(handle, i, name, &length, nullptr, nullptr, nullptr, nullptr) !=
						ERROR_SUCCESS)
					break;
				children.emplace_back(name, length);
			}
			RegCloseKey(handle);
		}
		for (const std::string &child : children)
			delete_tree(sub + "\\" + child);
		RegDeleteKeyA(HKEY_CURRENT_USER, sub.c_str());
	}

	std::string subkey_;
};

} // namespace test_registry
#endif
