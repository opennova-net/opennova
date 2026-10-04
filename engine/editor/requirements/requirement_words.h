#pragma once

#include <cstddef>
#include <string>

namespace opennova::editor {

// What the game does without a file it reads by name, in a modder's words (the UX round's problems
// lane): one row per row of the witnessed manifest (base/gameprofile/required_resources.cpp), keyed by
// its role, its sentence the manifest row's witnessed failure said plainly ("The game shows "Unable to
// load game strings" and exits."). The manifest stays the witness and keeps no editor words (ADR 0046's
// Consequences); the editor's messages, the Problems banner and a refused build read this table, and the
// manifest row's failure and citation stay the cited detail (requirement_witness). Tooling, not a port:
// the meaning is the manifest's.
struct RequirementWords {
	const char *role = "";
	// What the game does without it: a sentence, a capital first and a full stop last.
	const char *without = "";
};

// The row of a manifest role; null for a role the table has no row for (a pattern, a boot archive,
// the player's own file: no checklist row names them).
const RequirementWords *requirement_words(const std::string &role);
// Its sentence, or "" for none.
std::string requirement_without(const std::string &role);
// The manifest row's own record of it, the detail a modder may follow: its witnessed failure and its
// citation ("missing -> ... [orig: ...]"); "" for a role the manifest has none of.
std::string requirement_witness(const std::string &role);

// The table, in the manifest's order (for the tests: every checklist row has its words).
size_t requirement_words_count();
const RequirementWords &requirement_words_at(size_t index);

} // namespace opennova::editor
