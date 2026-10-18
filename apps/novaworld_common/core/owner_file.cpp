#include "owner_file.h"

#include <algorithm>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opennova::novaworld_server::hosted {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32

// Grants the service's own account alone on the open file `file` (a protected
// DACL: nothing inherited from the directory).
bool restrict_to_owner(HANDLE file) {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<unsigned char> user(size);
	const bool got = size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size);
	CloseHandle(token);
	if (!got) return false;
	EXPLICIT_ACCESSW access{};
	access.grfAccessPermissions = GENERIC_ALL;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = NO_INHERITANCE;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_USER;
	access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid);
	PACL acl = nullptr;
	if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
	const DWORD set = SetSecurityInfo(file, SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
			nullptr);
	LocalFree(acl);
	return set == ERROR_SUCCESS;
}

// Writes `content` to a new `path`; a secret file is made the owner's alone
// before a byte is written.
bool write_file_impl(const fs::path &path, const std::string &content, bool secret, std::string &error) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | WRITE_DAC, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		error = "cannot create " + path.filename().u8string() + " (Windows error " +
				std::to_string(GetLastError()) + ")";
		return false;
	}
	bool ok = !secret || restrict_to_owner(file);
	if (!ok) error = "cannot make " + path.filename().u8string() + " the service's alone";
	size_t written = 0;
	while (ok && written < content.size()) {
		DWORD chunk = 0;
		const DWORD want = static_cast<DWORD>(std::min<size_t>(content.size() - written, 1u << 20));
		if (!WriteFile(file, content.data() + written, want, &chunk, nullptr) || chunk == 0) {
			error = "cannot write " + path.filename().u8string();
			ok = false;
			break;
		}
		written += chunk;
	}
	CloseHandle(file);
	return ok;
}

#else

bool write_file_impl(const fs::path &path, const std::string &content, bool secret, std::string &error) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, secret ? 0600 : 0644);
	if (fd < 0) {
		error = "cannot create " + path.filename().string() + ": " + std::strerror(errno);
		return false;
	}
	bool ok = !secret || ::fchmod(fd, 0600) == 0;
	if (!ok) error = "cannot make " + path.filename().string() + " the service's alone";
	size_t written = 0;
	while (ok && written < content.size()) {
		const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {
			error = "cannot write " + path.filename().string() + ": " + std::strerror(errno);
			ok = false;
			break;
		}
		written += static_cast<size_t>(n);
	}
	::close(fd);
	return ok;
}

#endif

} // namespace

bool write_owner_file(const fs::path &path, const std::string &content, bool secret, std::string &error) {
	return write_file_impl(path, content, secret, error);
}

bool make_owner_directory(const fs::path &dir, std::string &error) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		error = "cannot create " + dir.filename().u8string() + ": " + ec.message();
		return false;
	}
#ifndef _WIN32
	fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
	if (ec) {
		error = "cannot make " + dir.filename().u8string() + " the service's alone: " + ec.message();
		return false;
	}
#endif
	return true;
}

} // namespace opennova::novaworld_server::hosted
