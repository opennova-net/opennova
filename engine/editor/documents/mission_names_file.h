#pragma once

// The .mis beside a mission (ADR 0046 S24, Names): the original editor's text form of the mission, which the
// game never reads (docs/mission/mis-format-re.md, What the game reads) and the editor keeps as the place for the
// names the .bms has no room for: an event's name and an area trigger's and a waypoint path's description
// (formats/mission/mission_mis.h MisNames), and the group names a .bms the original editor wrote holds none of
// (its writer writes a group's name bytes as zero, bms.h GroupRecord). It is never packed and nothing imports or
// converts it (AssetKind::MissionText).
//
// Read with the mission (the session's DocumentType::companions): its names are the mission's where it matches
// the mission, its events the mission's events by their index, each holding the same trigger and action lines
// (what the .mis writes of them), and every area it names one the mission has; a .mis that does not match is
// listed (mission.names_unmatched) with what still matches offered (Take the names that still match, an edit of
// the mission) and Write it from the mission. Written after the mission's own save only where the editor may
// write it: where the project has none, where the editor made the one there (the writer writes exactly the bytes
// read back from what it reads: write_mis_text over parse_mis_text and parse_mis_names, a fixed point) or wrote
// it since, or where it matched at the read; and never over a file that changed outside the editor since it was
// read or written (mission.names_outside), unless Write it from the mission said to. What the writer refuses (an
// area whose zone id is outside 1..99, or two of one id: the original's .mis has a slot per zone id 1..99 alone,
// mission.names_unwritten) is listed, never a failed save of the mission.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/node.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_chains.h>
#include <formats/mission/mission_mis.h>

namespace opennova::editor {

class MissionDocument;

// What the editor read of the .mis beside a mission and what its last save made of it.
struct MissionNamesFile {
	enum class Found {
		Unread,     // no file read beside the mission (one read from bytes; before the session's read)
		None,       // the project has none
		Matched,    // its names are the mission's
		Unmatched,  // its events are not the mission's, or it names an area the mission lacks (`why`)
		Unreadable, // it does not read as a .mis (`why`)
	};
	Found found = Found::Unread;
	// The editor made it (the writer writes exactly the bytes read from what it reads) or wrote it since.
	bool ours = false;
	// The bytes read or last written (FNV-1a 64, the documents' own); 0 for none.
	uint64_t fingerprint = 0;
	std::string why;
	// What it holds (a .mis that reads): its names, its group descriptions by group, its events' triggers and
	// actions, what Take the names that still match compares with the mission's.
	mission::MisNames names;
	std::array<std::string, bms::kGroupRecordCount> groups{};
	std::vector<mission::EventChain> events;
	// The last save's: the file changed outside the editor since it was read or written (`outside`, its bytes'
	// fingerprint then: what Write it from the mission writes over), and what the writer refused.
	bool outside = false;
	uint64_t outside_fingerprint = 0;
	std::string unwritten;
};

// The file-wide value Write it from the mission sets on a mission document (Edit::field, SetFileValue): the
// fingerprint of the .mis a save then writes over (an integer: the 64 bits as a signed word), although it does
// not match or changed outside the editor.
inline constexpr const char *kMisOverwriteField = "mis_overwrite";
// A mission document's file-wide state: the .mis Write it from the mission said a save writes over.
struct MissionFileState : FileState {
	uint64_t mis_overwrite = 0;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<MissionFileState>(*this); }
	size_t footprint() const override { return sizeof(MissionFileState); }
};

// The .mis read against a mission (its file and its events' chains as parsed): what it holds, whether it
// matches, and whether the editor made it.
MissionNamesFile read_names_file(const bms::File &mission, const std::vector<mission::EventChain> &events,
                                 const std::vector<uint8_t> &bytes);

// The names the mission's rows hold, as the .mis carries them.
mission::MisNames mission_names(const MissionDocument &document);
// The .mis a save writes from the mission (its file as the writer takes it, its names): false with the
// writer's refusal.
bool mission_names_text(const MissionDocument &document, std::string &text, std::string &error);

// The edits Take the names that still match makes of the mission (one undo step): each event's name where the
// .mis's event of its index holds the same lines, each area's description by its zone id, each path's
// description, each group's name where its record holds none and the .mis's description is one
// (bms::set_group_name's rules), every name the rows do not hold already. None when nothing would change.
std::vector<Edit> take_matching_names(const MissionDocument &document);

// The .mis's findings on the mission (mission.names_unmatched, mission.names_outside, mission.names_unwritten),
// each with its fixes planned (Diagnostic::planned): Take the names that still match where it changes
// something, Write it from the mission where a save would leave the file as it is.
void names_file_findings(const MissionDocument &document, std::vector<Diagnostic> &out);

} // namespace opennova::editor
