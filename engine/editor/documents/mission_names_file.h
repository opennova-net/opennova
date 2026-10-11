#pragma once

// The .mis beside a mission (ADR 0046 S24, Names): the original editor's text form of the mission, which the
// game never reads (docs/mission/mis-format-re.md, What the game reads) and the editor keeps as the place for the
// names the .bms has no room for: an event's name and an area trigger's and a waypoint path's description
// (formats/mission/mission_mis.h MisNames), and the group names a .bms the original editor wrote holds none of
// (its writer writes a group's name bytes as zero, bms.h GroupRecord). It is never packed and nothing imports or
// converts it (AssetKind::MissionText). The one beside the mission's own file, by its folder: two missions of one
// name in two folders each have their own.
//
// Read with the mission (the session's DocumentType::companions): its names are the mission's where it matches
// the mission, its events the mission's events by their index, each holding the same trigger and action lines
// (what the .mis writes of them), and every area it names one the mission has; a name the editor's setters would
// refuse (mission::mis_name_allowed, bms::set_group_name) is left out and listed (mission.names_refused). One that
// does not match, does not read as a .mis, or cannot be read is listed (mission.names_unmatched), with what still
// matches offered (Take the names that still match, an edit of the mission) and Write it from the mission.
//
// Written after the mission's own save only where the editor may write it without losing what the file holds:
// where the project has none and the mission holds a name (an event's, an area's or a path's, a group's or a
// variable's), and where the one there is the editor's own (the writer writes exactly the bytes read back from what
// it reads: write_mis_text over parse_mis_text and parse_mis_names, a fixed point) and matches the mission. Never
// automatically over the original editor's (which writes what the editor's writer does not: its weapon
// availability, teleport targets, wav list and wind, colours, item keys, longer descriptions, base heights;
// mission.names_foreign), over one that does not match, or over one changed outside the editor since it was read
// or written (mission.names_outside, which can be read again): only where Write it from the mission said to write
// over those bytes, once. Where a save leaves it while the mission's names differ from what it holds, those names
// stay unsaved (DocumentBase::companion_unsaved: the unsaved prompt asks before the mission is dropped) and the
// row says so until they are written. What the writer refuses (an area whose zone id is outside 1..99, or two of
// one id: the original's .mis has a slot per zone id 1..99 alone) and what the file system refuses are listed
// (mission.names_unwritten), never a failed save of the mission.
//
// Each item's base height (extra_bheight) is the terrain's height under it, which the session samples at the save
// (MissionDocument::set_base_heights), every entity's, a marker's and an itemless one's too, with height_lock 1:
// the original editor keeps an item's height and its base (where it was placed, extra_building_height) relative
// to the terrain, and its save writes a height-locked item's both absolute, the terrain's height under it added,
// its read taking it off again [orig: JOTACmed.exe sub_44E640 @ 0x44e640, from MIS_SaveDocument @ 0x4546b5;
// sub_44E6A0 @ 0x44e6a0, from MIS_LoadDocument @ 0x45460e; the base set at placement, MissionItem_AppendByTypeId
// @ 0x455a16], so an entity of a .bms, which stands at its z over no base of its own, writes the terrain's height,
// sampled as the original editor samples it (mission::mis_terrain_height, its sampler sub_4226D0 @ 0x4226d0: the
// .cpt's height atlas the game loads, bilinear, over the .trn's own sector grid), so the original editor reads the
// item back on no base of its own and at the height it stands.

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

// What the editor read of the .mis beside a mission, or wrote there.
struct MissionNamesFile {
	enum class Found {
		Unread,     // no file read beside the mission (one read from bytes; before the session's read)
		None,       // the project has none
		Matched,    // its names are the mission's
		Unmatched,  // its events are not the mission's, or it names an area the mission lacks (`why`)
		Unreadable, // it does not read as a .mis (`why`)
		Unopened,   // it is there, and the file system would not read it (`why`)
	};
	Found found = Found::Unread;
	// The editor made it (the writer writes exactly the bytes read from what it reads) or wrote it.
	bool ours = false;
	// The bytes read or written (FNV-1a 64, the documents' own); 0 for none, or none read.
	uint64_t fingerprint = 0;
	std::string why;
	// What it holds (a .mis that reads): its names, its group descriptions by group, its events' triggers and
	// actions, what Take the names that still match compares with the mission's.
	mission::MisNames names;
	std::array<std::string, bms::kGroupRecordCount> groups{};
	std::vector<mission::EventChain> events;
	// What the mission takes of it (Matched: every name the editor's setters take; else none), the group names
	// among them (a group its record names none of), or what was written: the names the rows are measured
	// against, a name the rows hold otherwise unsaved until it is written.
	mission::MisNames kept;
	std::array<std::string, bms::kGroupRecordCount> kept_groups{};
	// What it names that the mission does not take, in words ("event 3's name: A name holds 59 bytes at most").
	std::vector<std::string> refused;
};

// What the last save made of the .mis where it left it as it is (MissionDocument::names_left).
struct MissionNamesLeft {
	// It changed outside the editor since it was read or written (MissionDocument::names_outside_file: those bytes
	// read against the mission).
	bool outside = false;
	// The writer's refusal, or the file system's (the file not read, or not written), a sentence.
	std::string unwritten;
};

// The file-wide values a mission document holds of its .mis (Edit::field, SetFileValue), each the fingerprint of
// the bytes it names as 16 hex digits (fingerprint_hex: a number past 2^53 does not survive JSON): Write it from
// the mission's, the .mis a save then writes over although it is not the editor's to write (once: the write it was
// for spends it); Read the names again's, the .mis the mission's names were read from since (an outside change
// the names were taken from, which a save then measures the file against).
inline constexpr const char *kMisOverwriteField = "mis_overwrite";
inline constexpr const char *kMisKnownField = "mis_known";
struct MissionFileState : FileState {
	uint64_t mis_overwrite = 0;
	uint64_t mis_known = 0;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<MissionFileState>(*this); }
	size_t footprint() const override { return sizeof(MissionFileState); }
};
std::string fingerprint_hex(uint64_t fingerprint);
// 16 hex digits, either case; false for anything else.
bool fingerprint_from_hex(const std::string &text, uint64_t &out);

// The .mis read against a mission (its file and its events' chains as parsed): what it holds, whether it
// matches, whether the editor made it, and what the mission takes of it.
MissionNamesFile read_names_file(const bms::File &mission, const std::vector<mission::EventChain> &events,
                                 const std::vector<uint8_t> &bytes);
// Read against the document's records as they stand.
MissionNamesFile read_names_file(const MissionDocument &document, const std::vector<uint8_t> &bytes);

// The names the mission's rows hold, as the .mis carries them (path 0, no path, none).
mission::MisNames mission_names(const MissionDocument &document);
// Two sets of names alike (an event past the other's list naming none).
bool same_names(const mission::MisNames &a, const mission::MisNames &b);
// Whether the mission holds a name the .mis carries: an event's, an area's or a path's, a group's, a variable's.
bool mission_holds_names(const MissionDocument &document);
// The .mis a save writes from the mission (its file as the writer takes it, its names, each entity's base height
// from `base_heights` where it has one, in the writer's order: mission::write_mis_text): false with the writer's
// refusal.
bool mission_names_text(const MissionDocument &document, const std::vector<int32_t> *base_heights, std::string &text,
                        std::string &error);

// The edits Take the names that still match makes of the mission (one undo step) from `file` (the .mis as read):
// each event's name where the .mis's event of its index holds the same lines, each area's description by its zone
// id, each path's description, each group's name where its record holds none and the .mis's description is one
// (bms::set_group_name's rules), every name the rows do not hold already and the editor's setters take. None when
// nothing would change.
std::vector<Edit> take_matching_names(const MissionDocument &document, const MissionNamesFile &file);

// The .mis's findings on the mission (mission.names_unmatched, mission.names_foreign, mission.names_outside,
// mission.names_unwritten, mission.names_refused), each with its fixes planned (Diagnostic::planned): Take the
// names that still match where it changes something, Read the names again where they changed outside the editor
// and still match, Write it from the mission where a save would leave the file as it is.
void names_file_findings(const MissionDocument &document, std::vector<Diagnostic> &out);

} // namespace opennova::editor
