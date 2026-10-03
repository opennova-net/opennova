#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The editor's own preferences, per machine and per user, not per project (ADR 0046 d6, S13 A2):
// the recent-projects list, the game runtime Play launches, the game install, whether Play runs
// the build in the game install, and whether an import brings the files the chosen ones need. A
// project's `.opennova/local.json` overrides the runtime for that project alone, and holds its own
// game install. Schema 2 (S13 A4) renamed the game install's keys ("game_install",
// "play_in_install"); schema 3 (the polish) keeps the recently placed items per game. Pre-1.0 there is
// no reader for another schema: such a file is set aside, read as absent (the defaults) with a warning
// naming what it held, and the next save writes a new file.
inline constexpr int kPreferencesSchemaVersion = 3;
inline constexpr size_t kRecentProjectsMax = 10;
inline constexpr size_t kRecentItemsMax = 12;

struct Preferences {
	std::vector<std::string> recent_projects; // project roots, most recent first
	std::string runtime_executable;           // "" = the runtime packaged beside the editor
	std::string game_install;                 // the game install (Joint Operations), on this machine
	bool play_in_install = false;             // Play runs the build in the game install
	// The import dialog's "Include the files these need" (ADR 0046 S11g): what a preview the
	// windows raise plans with; a store that does not say reads as on.
	bool import_dependencies = true;
	// The items most recently placed in a mission's viewport (ADR 0046 S15: its Place tool's palette
	// lists them first), by their items.def id, most recent first, kept per game (the polish: an id
	// names another item in another game's catalogs) by the game the project was for
	// (recent_items_game); a store that does not say reads as none.
	std::map<std::string, std::vector<int64_t>> recent_items;
};

// Where the preferences are kept (S13 A2). The embedder owns the store and hands the session a
// reference: the shell a file in the editor's user directory (FilePreferencesStore), a test or
// the command line memory (MemoryPreferencesStore). A store that keeps nothing yet reads as the
// defaults.
class PreferencesStore {
public:
	virtual ~PreferencesStore() = default;
	// What the store keeps into `out` (the defaults when it keeps nothing): true, with `finding` a
	// warning when it set aside what it kept (read as nothing kept, a file of another schema) and
	// left as it was given otherwise; false with `finding` the error when what it keeps cannot be
	// read.
	virtual bool load(Preferences &out, Diagnostic &finding) = 0;
	// `preferences` kept; false with `error` when they could not be.
	virtual bool save(const Preferences &preferences, Diagnostic &error) = 0;
};

// The preferences in memory: what the last save kept, the defaults (or what it was made with)
// before one. For a session whose preferences end with it: a test's, the command line's.
class MemoryPreferencesStore : public PreferencesStore {
public:
	MemoryPreferencesStore() = default;
	explicit MemoryPreferencesStore(Preferences preferences) : preferences_(std::move(preferences)) {}

	bool load(Preferences &out, Diagnostic &) override {
		out = preferences_;
		return true;
	}
	bool save(const Preferences &preferences, Diagnostic &) override {
		preferences_ = preferences;
		++saves_;
		return true;
	}

	const Preferences &preferences() const { return preferences_; }
	// How many saves it has taken.
	size_t saves() const { return saves_; }

private:
	Preferences preferences_;
	size_t saves_ = 0;
};

} // namespace opennova::editor
