#pragma once

#include <string>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// Each kind of project file in a modder's words (ADR 0046, the UX round's plain-words lane): what a
// file of the kind holds and what in the game reads it and when, from the RE records and the cited
// code (each row its witness; "Unknown:" where no reader is witnessed), which a file's card shows (Files'
// card, and the page of a file the editor has no editor for: session/file_card.h). One row per AssetKind in
// the enum's order (a static_assert holds the two in step).
struct AssetKindWords {
	AssetKind kind;
	const char *what;
	const char *read_by;
	const char *cite;
};
const AssetKindWords &asset_kind_words(AssetKind kind);

// Whether the game of `game_code` (a project's target game, a title code) never reads a file of `kind` named
// `name` (its basename): a definitions text of no other kind (OtherDefs) under none of the .def names the title's
// row says its executable opens (Title::read_defs, each witnessed at its open site: docs/required-resources.md,
// "The .def names the game opens"). The build still packs it, as the game's own archives carry such files. False
// for every other kind, and for a title whose row makes no claim (its binary unread for them) or an unknown code,
// which mark nothing.
bool game_never_reads(AssetKind kind, const std::string &name, const std::string &game_code);

} // namespace opennova::editor
