#pragma once

#include <string>

#include <editor/model/diagnostic.h>
#include <editor/session/preferences_store.h>

namespace opennova::editor {

// The editor's preferences (preferences_store.h) over the store its embedder owns (ADR 0046 S13
// A2): read once when the session starts; a change written from a copy and in effect once the
// store kept it, so a preference that could not be written is still the one in effect and a retry
// writes it again. The session shows them in its view (the recent projects, the game install, the
// runtime, Play in the game install, the import setting) and moves Preferences when they change.
class EditorPreferences {
public:
	explicit EditorPreferences(PreferencesStore &store) : store_(store) {}
	EditorPreferences(const EditorPreferences &) = delete;
	EditorPreferences &operator=(const EditorPreferences &) = delete;

	// What the store keeps, in effect: true, `finding` the warning when the store set aside what it
	// kept (the defaults in effect) and cleared otherwise; false with `finding` the error, the
	// defaults in effect, when it cannot be read.
	bool load(Diagnostic &finding);
	const Preferences &values() const { return values_; }
	// `next` kept by the store, then in effect; false with `error`, the values in effect as they
	// were, when it could not be.
	bool write(const Preferences &next, Diagnostic &error);
	// The values in effect kept by the store (after a change of the recent list).
	bool save(Diagnostic &error) { return store_.save(values_, error); }

	// `root` moved (or added) to the front of the recent list, capped at kRecentProjectsMax; or
	// dropped from it. In effect at once, kept by the next save.
	void remember_recent_project(const std::string &root);
	void forget_recent_project(const std::string &root);
	// `item` moved (or added) to the front of the recently placed items, capped at kRecentItemsMax.
	// In effect at once, kept by the next save; false when it was first already (nothing changed).
	bool remember_recent_item(int64_t item);

private:
	PreferencesStore &store_;
	Preferences values_;
};

} // namespace opennova::editor
