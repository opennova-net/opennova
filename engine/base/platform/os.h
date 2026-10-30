#pragma once

#include <base/platform/process.h>
#include <base/proc/child_process.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The operating system's services the engine asks for (ADR 0077 d2: the engine's platform API,
// backend-neutral): the command line the game was started with, its own process id, a child
// process (process.h), a URL opened in the user's browser, and the user's locale. What the game
// does with them (retail's switch walk over its line, the updater's arguments, the NovaWorld
// identity's locale trio) is the engine's.
//
// The design follows SDL3's (main's argc / argv, SDL_CreateProcess, SDL_OpenURL,
// SDL_GetPreferredLocales' SDL_Locale) and sokol_app's (sapp_desc's argc / argv); GLFW has none.
// The process line and the child are base/proc's (own_command_line, own_process_id, ChildProcess),
// so a desktop backend's Os forwards to them.
namespace opennova::platform {

// The user's preferred locale as SDL_Locale gives it: an ISO 639 language code ("en") and, where
// the user named one, an ISO 3166 country code ("US"); empty when the platform has none.
struct Locale {
	std::string language;
	std::string country;
};

// The locale as one "language_COUNTRY" tag ("en_US"; "en" with no country, "" with neither), the
// form the engine's NovaWorld session takes.
inline std::string locale_tag(const Locale &locale) {
	if (locale.language.empty()) return std::string();
	return locale.country.empty() ? locale.language : locale.language + "_" + locale.country;
}

class Os {
public:
	virtual ~Os() = default;

	// The arguments the game was started with, the program's own token left out, UTF-8.
	virtual std::vector<std::string> arguments() const = 0;
	// The command line as the OS holds it, the program's own token first, UTF-8: the line
	// retail's switches are walked over (base/proc own_command_line carries the rule); "" where
	// the OS holds none, only an argument array (POSIX).
	virtual std::string command_line() const = 0;
	// This process's id.
	virtual int64_t process_id() const = 0;

	// A child started from `options` (base/proc SpawnOptions: an argv array, no shell); null with
	// `error` when it did not start.
	virtual std::unique_ptr<Process> spawn(const proc::SpawnOptions &options, std::string &error) = 0;

	// `url` handed to the system's handler for it (the browser for http/https); false when the
	// system took none.
	virtual bool open_url(const std::string &url) = 0;

	// The user's preferred locale.
	virtual Locale locale() const = 0;
};

} // namespace opennova::platform
