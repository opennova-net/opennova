#pragma once

#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>

namespace opennova::editor {

// The mission document type's validator over one mission (DocumentType::validate_file), an open
// document standing in for its file: what the game makes of what the file holds, from the document
// alone. The source findings first (a rewrite that differs, the events' runs); then the records:
// an SSN or a zone id two records carry (an SSN's second worded by what reaches it: the lookups by
// SSN never, an area check every organic and item), an SSN a lookup scanning fewer pools finds no row
// of (an alive test's, a ChangeSingleAI's, a teleport's, a teammate operation's patient), a
// degenerate zone, a zone id outside the editor's 1..99,
// an event index past the table, a stop naming a marker the file lacks, a path's stored count past
// its slots, a one-stop path, an entity on an empty path or starting past its count, a group past
// the 64 the tables hold, a pool past the game's limits, two game mode bits, a trigger type the
// evaluator lacks (a sub-type only where its main type's case reads one), a bounding box with a
// corner past the other. References to other files are the
// asset graph's; what an entity's pool makes of its item's TYPE is the mission's use check
// (graph/use_checks.cpp, mission.pool), which reads the item through the graph.
std::vector<Diagnostic> validate_mission_file(const DocumentBase &document);

// The mission type's own finding codes (DocumentType::findings), each a row of its table
// (mission_validation.cpp, static_asserted into this order).
enum class MissionFinding {
	RewriteDiffers,
	InvalidInput, // the events' runs the chains cannot hold (mission.invalid_input, blocks the save)
	EventOrder,
	SsnDuplicate,
	SsnUnscanned, // an SSN only rows of pools its lookup does not scan carry (an alive test's marker)
	ZoneDuplicate,
	ZoneDegenerate,
	ZoneId,
	EventMissing,
	MarkerMissing,
	PathCount,
	PathOneShot,
	PathEmpty,
	PathStart,
	GroupRange,
	PoolLimit,
	GameMode,
	TriggerType,
	BoundingBox,
	Pool, // an entity in another pool than its item's TYPE places it in (the mission's use check)
	NoStart, // no marker a player of the mission's mode (and team) starts at: none moves the player there
	OffGround, // an entity the game leaves off the ground (the mission's project check, preview/mission_ground_check.h)
	kCount
};
const FindingCodeRow &finding_code(MissionFinding code);
FindingTable mission_finding_codes();

} // namespace opennova::editor
