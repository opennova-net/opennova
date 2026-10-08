#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::def {

// Authoring findings contain locations and typed reasons, never source lines that
// could be replayed by a writer. Runtime callers may omit the report and retain
// the witnessed permissive parse behavior.
//
// Reinterpreted names a number the witnessed loader reads as another (a weapon's category
// past 0..11 read as 0 [orig: WeaponDefs_ParseLineCallback @ 0x5439a8], an item's unit_type
// past a byte read as its low byte [orig: ItemDef_ParseProperty @ 0x49ee47]): the record
// holds what the game reads; a save over the file's modeled layout (def_notes.h) writes the
// number in the file's spelling while the record holds what it read, the writer's own form
// writes what the game reads; reported, never a blocker.
//
// UnknownProperty names input the witnessed loaders read past without storing
// anything: an unrecognized key, an `attrib:` token outside the chain, a stray
// line outside a block. The game ignores it, so the typed model has nothing to
// carry: a save over the file's modeled layout writes it as the tokens the file has, the
// writer's own form leaves it out; it is reported, never a blocker. [orig:
// ItemDef_ParseProperty @ 0x49EB00 (the key chain ends without a store and the
// attrib: chain has no else arm), WeaponDef_ParseProperty @ 0x54D730 and
// AmmoDef_ParseProperty @ 0x40A2D0 return 0 for an unmatched key]. The other
// codes name input the typed model cannot carry faithfully: the writer refuses
// the record until the source is corrected and reloaded.
enum class DefIssueCode { UnknownProperty, Reinterpreted, InvalidValue, MalformedBlock, Unrepresentable };

inline bool def_issue_blocks(DefIssueCode code) {
	return code != DefIssueCode::UnknownProperty && code != DefIssueCode::Reinterpreted;
}

struct DefIssue {
	DefIssueCode code = DefIssueCode::InvalidValue;
	size_t line = 0;
	std::string record;
	std::string field;
	std::string message;
	bool blocks() const { return def_issue_blocks(code); }
};

using DefParseReport = std::vector<DefIssue>;

inline bool def_report_blocks(const DefParseReport &report) {
	for (const DefIssue &issue : report)
		if (issue.blocks()) return true;
	return false;
}

} // namespace opennova::def
