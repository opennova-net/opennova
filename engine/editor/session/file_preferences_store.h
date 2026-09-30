#pragma once

#include <string>
#include <utility>

#include <editor/model/diagnostic.h>
#include <editor/session/preferences_store.h>

namespace opennova::editor {

// The preferences in a JSON file, the editor's settings file the shell names in its user
// directory (ADR 0046 d6, S13 A2): a missing file reads as the defaults, a present but invalid one
// is an error (`editor_settings.unreadable`, `.json`, `.schema_version.unsupported`: a file of
// another schema, an older editor's among them, is never migrated); a save writes the whole file
// atomically, its directory made first (`editor_settings.write`).
class FilePreferencesStore : public PreferencesStore {
public:
	explicit FilePreferencesStore(std::string path) : path_(std::move(path)) {}

	const std::string &path() const { return path_; }

	bool load(Preferences &out, Diagnostic &error) override;
	bool save(const Preferences &preferences, Diagnostic &error) override;

private:
	std::string path_;
};

} // namespace opennova::editor
