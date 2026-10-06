#pragma once

// The admin console's seam onto the host's map rotation: g_MissionRotation @0xC86FDC (the
// 8-byte {catalog index, one-shot flag} entries with a cursor and an alternate cursor) over
// the mission catalog g_MissionList @0x2551118, which the round-end advance owns (D-NET-331;
// the list is `HostRotation`, mission_rotation.h). The console (admin_console.h) keeps the
// MISSION verbs' dispatch, their scene gates and every reply; the implementation over the
// rotation runs the list operations as retail's do, its witnessed faults excepted
// (D-NET-364..366). The record is docs/net/novaworld-net-re.md §5.70.1 and §5.70.8.

#include <runtime/inmatch/mission_rotation.h>
#include <runtime/mission/mission_catalog.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::inmatch {

class RotationAdmin {
public:
	struct Entry {
		size_t catalog_index = 0; // into catalog()
		bool one_shot = false;    // the entry's flag word
	};
	// The list as MISSION LIST and the QUERY report read it.
	struct List {
		bool exists = false; // g_MissionRotation is not null
		std::vector<Entry> entries;
		int32_t cursor = -1;
		int32_t alt_cursor = -1;
		bool half_flipped = false; // byte_24D217E (g_RotationIsFlipped)
	};
	// One catalog row as the console reads it: the file (+0), the title (+0x414) and the
	// launch option word (+0x1140, the AUTO_SWITCH_SIDES / `(2x)` side switch).
	struct CatalogRow {
		std::string file;
		std::string title;
		int32_t launch_option = 0;
	};

	virtual ~RotationAdmin() = default;

	virtual List list() const = 0;
	virtual std::vector<CatalogRow> catalog() const = 0;

	// MISSION ADD's catalog write, in any scene: the row's launch option word.
	// [orig: CAdminServer_HandleMissionAdd @0x403E5E..0x403E91]
	virtual void set_launch_option(size_t catalog_index, int32_t option) = 0;
	// MISSION ADD's list write, in the Game Loop only: MissionRotation_Append(row, one_shot)
	// with no INSERT_AT (which clears the row's launch option again for an objective or
	// non-team row), else MissionList_InsertEntryAtIndex(row, at, one_shot).
	// [orig: MissionRotation_Append @0x5019D0; MissionList_InsertEntryAtIndex @0x501AD0]
	virtual void add(size_t catalog_index, std::optional<int32_t> insert_at, bool one_shot) = 0;
	// MissionList_RemoveEntry(index): false when the index is out of range (the console replies
	// OK either way). [orig: MissionList_RemoveEntry @0x4FC6C0]
	virtual bool remove(int32_t index) = 0;
	// MISSION CLEAR: in the Game Loop the whole list freed (MissionList_FreeBuffer, the round
	// end then finds no entry); elsewhere every catalog row's host-screen mark +0x113C cleared.
	// [orig: CAdminServer_HandleMissionCommand @0x40657F (MissionList_FreeBuffer @0x4FC480),
	//  the mark store @0x4065B6]
	virtual void clear(bool in_game) = 0;
	// MISSION SETNEXT: MissionList_SetAltCursor(index), refused outside [0, count) (false), and
	// then the SETNEXT latch dword_24D2180 (g_RotationSetNextLatch) set either way.
	// [orig: MissionList_SetAltCursor @0x4FC730; the latch store @0x4066DF]
	virtual bool set_next(int32_t index) = 0;
};

// The seam over the host's own rotation (ADR 0051 PR5b): the list the round-end advance and
// the map change run (HostRotation, inmatch/mission_rotation.h) and the catalog its entries
// index. The list ops are MissionRotation's ports, the one-shot flag the advance's arm; SETNEXT
// sets the latch the map change's teardown consumes (inmatch/map_change.h).
class HostRotationAdmin final : public RotationAdmin {
public:
	HostRotationAdmin(HostRotation &rotation, const std::vector<mission_catalog::Row> &catalog)
			: rotation_(rotation), catalog_(catalog) {}

	List list() const override;
	std::vector<CatalogRow> catalog() const override;
	void set_launch_option(size_t catalog_index, int32_t option) override;
	void add(size_t catalog_index, std::optional<int32_t> insert_at, bool one_shot) override;
	bool remove(int32_t index) override;
	void clear(bool in_game) override;
	bool set_next(int32_t index) override;

private:
	HostRotation &rotation_;
	const std::vector<mission_catalog::Row> &catalog_;
};

} // namespace opennova::inmatch
