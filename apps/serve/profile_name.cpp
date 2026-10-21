#include "profile_name.h"

#include <runtime/profile/player_profiles.h>

#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace opennova::serve {

std::string process_account_name() {
#if defined(_WIN32)
	// The whole name, whose length the load's 16-byte rule then judges.
	char name[257] = {};
	DWORD size = sizeof(name);
	if (GetUserNameA(name, &size) == 0) return std::string();
	return std::string(name);
#else
	if (const char *login = getlogin()) return login;
	const char *user = std::getenv("USER");
	return user != nullptr ? std::string(user) : std::string();
#endif
}

std::string serve_profile_name(int32_t player_index, const std::string &account_name) {
	profile::PlayerProfiles profiles;
	profiles.select_slot(player_index);
	profile::PlayerProfiles::LoadInput input;
	input.user_name = account_name;
	profiles.load(input, profile::ProfileDefaults{});
	// The 32-byte name copy keeps 31 characters [orig: Napi_CopyString(.., 0x20) @0x551695].
	return profiles.current().name.substr(0, 31);
}

} // namespace opennova::serve
