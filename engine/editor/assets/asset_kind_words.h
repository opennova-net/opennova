#pragma once

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// Each kind of project file in a modder's words (ADR 0046, the UX round's plain-words lane): what a
// file of the kind holds and what in the game reads it and when, from the RE records and the cited
// code (each row its witness; "Unknown:" where no reader is witnessed), which the page of a file the
// editor has no editor for shows. One row per AssetKind in the enum's order (a test holds the two in
// step).
struct AssetKindWords {
	AssetKind kind;
	const char *what;
	const char *read_by;
	const char *cite;
};
const AssetKindWords &asset_kind_words(AssetKind kind);

} // namespace opennova::editor
