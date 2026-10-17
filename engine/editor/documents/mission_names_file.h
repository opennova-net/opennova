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
// Each entity's height lock (S25: the field height_lock, bms::Entity::mis_height_lock) is the .mis's height_lock,
// which the .bms has no room for: read with the names from a matching .mis, each locked item of the file given to the
// mission's entity of its pool and SSN (one the mission has none of listed, mission.names_refused, and left
// unlocked), and written by its save. A lock alone makes the .mis due where the project has none, and a lock the file
// does not hold stays unsaved where the save leaves it, as a name does.
//
// Each entity's heights are written as the original editor's save writes them over the terrain's height under it,
// which the session samples at the save (MissionDocument::set_base_heights), every entity's, a marker's and an
// itemless one's too: the original editor keeps an item's height and its base relative to the terrain and writes a
// height-locked item's both absolute, an unlocked one's relative with no height_lock line [orig: JOTACmed.exe
// sub_44E640 @ 0x44e640, from MIS_SaveDocument @ 0x4546b5; @ 0x455889..0x45589a], so an entity of a .bms (unlocked:
// mission_mis.h's frames, D-MIS-4) writes its z less the terrain's height there, sampled as the original editor
// samples it (mission::mis_terrain_height, its sampler sub_4226D0 @ 0x4226d0: the .cpt's height atlas the game
// loads, bilinear, over the .trn's own sector grid), and the original editor reads it back at the height it
// stands. Where the mission's terrain does not read, no height is handed and each entity is written locked, its z
// absolute, exact there over whatever terrain it loads (D-MIS-17).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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
	// Its items' height locks (S25), each its pool (0 items, 1 buildings, 2 markers, 3 organics, as the reader sorts
	// an item by its type), its SSN and its lock, in the file's order; and what the mission takes of them (Matched;
	// else none): each of the mission's entities' lock in the writer's order (items, buildings, markers, organics),
	// the one the .mis gives the entity of its pool and SSN, else 0 (mission_locks' form). The locks the rows are
	// measured against, as `kept` is for the names. A lock is the int the file holds, any nonzero one a lock, kept as
	// read and written back so (the original editor's atol, tested nonzero: mission_mis.h).
	// A locked item's base comes with its lock: absolute, as the original editor keeps it through a terrain change
	// [orig: JOTACmed.exe sub_402770 @ 0x402c9f..0x402ccd] and writes it (master #1095, D-MIS-4); kept_bases each
	// entity's in the writer's order, none for one the .mis gives no lock (a base of 0 is one: the .mis holds it).
	// `locks` of a write (MissionDocument::names_file_written) are what it wrote, each base it chose kept so: a save
	// after it takes its base from them, never from the ground again.
	struct Lock {
		int pool = 0;
		int32_t id = 0;
		int32_t lock = 0;
		int32_t base = 0;
	};
	std::vector<Lock> locks;
	std::vector<int32_t> kept_locks;
	std::vector<std::optional<int32_t>> kept_bases;
};

// What the last save made of the .mis where it left it as it is (MissionDocument::names_left).
struct MissionNamesLeft {
	// It changed outside the editor since it was read or written (MissionDocument::names_outside_file: those bytes
	// read against the mission).
	bool outside = false;
	// The writer's refusal, or the file system's (the file not read, or not written), a sentence.
	std::string unwritten;
	// Why the file it left as it is no longer matches the mission (names_file_stale), "" where it does: the next open
	// of the mission takes none of its names (mission.names_unmatched, with Write it from the mission).
	std::string stale;
	// The unlocked entities the last write wrote height locked at their heights: how many, and the first
	// kLockedNamed's titles ("Item #12"): the terrain's height under them was not known, the mission's terrain not
	// reading (D-MIS-17; mission.names_locked).
	static constexpr size_t kLockedNamed = 12;
	size_t locked_count = 0;
	std::vector<std::string> locked;
};

// The file-wide values a mission document holds of its .mis (Edit::field, SetFileValue), each the fingerprint of
// the bytes it names as 16 hex digits (fingerprint_hex: a number past 2^53 does not survive JSON): Write it from
// the mission's, the .mis a save then writes over although it is not the editor's to write (once: the write it was
// for spends it); Read the names again's, the .mis the mission's names were read from since (an outside change
// the names were taken from, which a save then measures the file against).
inline constexpr const char *kMisOverwriteField = "mis_overwrite";
inline constexpr const char *kMisKnownField = "mis_known";
// The variables' names (ADR 0046 S24 Names; master #1095, D-MIS-13): the .mis's `//var` lines, which no .bms byte
// holds, a file-wide value each (Edit::position V0..V255, the value its name, "" none), read from a matching .mis.
inline constexpr const char *kMisVariableField = "variable_name";
struct MissionFileState : FileState {
	uint64_t mis_overwrite = 0;
	uint64_t mis_known = 0;
	std::array<std::string, mission::kVariableNameCount> variables{};
	std::shared_ptr<FileState> clone() const override { return std::make_shared<MissionFileState>(*this); }
	size_t footprint() const override {
		size_t bytes = sizeof(MissionFileState);
		for (const std::string &name : variables) bytes += name.capacity();
		return bytes;
	}
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

// Why a matching .mis no longer matches the mission's records as they stand (its events by their index, each with
// the same trigger and action lines, and every zone it describes one an area has; read_names_file's test), "" where
// it still does and for one that never matched.
std::string names_file_stale(const MissionDocument &document, const MissionNamesFile &file);

// The names the mission's rows hold, as the .mis carries them (path 0, no path, none).
mission::MisNames mission_names(const MissionDocument &document);
// Two sets of names alike (an event past the other's list naming none).
bool same_names(const mission::MisNames &a, const mission::MisNames &b);
// Whether the mission holds a name the .mis carries: an event's, an area's or a path's, a group's, a variable's.
bool mission_holds_names(const MissionDocument &document);
// The height locks the mission's entities hold (S25), each as it stands (0 unlocked, any other a lock), in the
// writer's order (items, buildings, markers, organics); whether two such lists agree (an entity past one's list
// unlocked); whether the mission locks any.
std::vector<int32_t> mission_locks(const MissionDocument &document);
bool same_locks(const std::vector<int32_t> &a, const std::vector<int32_t> &b);
bool mission_holds_locks(const MissionDocument &document);
// The .mis a save writes from the mission (its file as the writer takes it, its names, each entity's base height
// from `base_heights` where it has one, in the writer's order: mission::write_mis_text; its default kit the one
// the mission's loadout leads with over MissionDocument::weapon_table): false with the writer's refusal. A
// locked entity's base is the one the .mis as the mission knows it keeps for it (MissionNamesFile::
// kept_bases over the mission as it stands), else the ground under it from `base_heights`; `written` takes each
// entity's lock and base as written, in the writer's order (MissionNamesFile::locks).
bool mission_names_text(const MissionDocument &document, const std::vector<int32_t> *base_heights, std::string &text,
                        std::string &error, std::vector<MissionNamesFile::Lock> *written = nullptr);

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
