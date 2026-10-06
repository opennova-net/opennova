// The admin console's seam over the host's rotation (rotation_admin.h).
#include <runtime/inmatch/rotation_admin.h>

namespace opennova::inmatch {

// The block as MISSION LIST and QUERY walk it: the live entries, the two cursors and the
// half toggle. [orig: CAdminServer_HandleMissionCommand @0x406338..0x406483 over
//  g_MissionRotation @0xC86FDC and g_RotationIsFlipped (byte_24D217E)]
RotationAdmin::List HostRotationAdmin::list() const {
	const MissionRotation &rotation = rotation_.list;
	List out;
	out.exists = rotation.allocated;
	for (int32_t i = 0; i < rotation.count; ++i) {
		const MissionRotationEntry &entry = rotation.entry(i);
		out.entries.push_back(Entry{static_cast<size_t>(entry.catalog_index), entry.flag != 0});
	}
	out.cursor = rotation.cursor;
	out.alt_cursor = rotation.alt_cursor;
	out.half_flipped = rotation_.is_flipped;
	return out;
}

// The catalog rows the console reads: the file (+0), the title (+0x414) and the launch option
// word (+0x1140), which the port keeps beside the catalog on the rotation.
// [orig: g_MissionList @0x2551118, stride 0x11E8]
std::vector<RotationAdmin::CatalogRow> HostRotationAdmin::catalog() const {
	std::vector<CatalogRow> rows;
	rows.reserve(catalog_.size());
	const std::vector<int32_t> &options = rotation_.list.launch_options;
	for (size_t i = 0; i < catalog_.size(); ++i)
		rows.push_back(CatalogRow{catalog_[i].file, catalog_[i].title, i < options.size() ? options[i] : 0});
	return rows;
}

// [orig: CAdminServer_HandleMissionAdd @0x403E5E..0x403E91, the row's +0x1140 store]
void HostRotationAdmin::set_launch_option(size_t catalog_index, int32_t option) {
	if (catalog_index >= catalog_.size()) return;
	rotation_.list.launch_option(catalog_, static_cast<int32_t>(catalog_index)) = option;
}

// The ADD's list write: the flag is the literal ONESHOT's 1.
// [orig: CAdminServer_HandleMissionAdd -- MissionList_InsertEntryAtIndex(row, atol(at),
//  oneshot) with a third argument, else MissionRotation_Append(row, oneshot)]
void HostRotationAdmin::add(size_t catalog_index, std::optional<int32_t> insert_at, bool one_shot) {
	const int32_t row = static_cast<int32_t>(catalog_index);
	const int32_t flag = one_shot ? 1 : 0;
	if (insert_at.has_value())
		rotation_.list.insert_entry(catalog_, row, *insert_at, flag);
	else
		rotation_.list.append(catalog_, row, flag);
}

// [orig: MissionList_RemoveEntry @0x4FC6C0]
bool HostRotationAdmin::remove(int32_t index) {
	return rotation_.list.remove_entry(index);
}

// In the Game Loop the whole block is freed: the next round end finds no entry and the session
// ends. Outside it retail clears every catalog row's host-screen mark (+0x113C), the
// SELECTED_MISSIONS state of a host screen; the host rotation carries no host screen, so there
// is nothing to clear here (a shell with one clears its own).
// [orig: CAdminServer_HandleMissionCommand @0x40657F (MissionList_FreeBuffer @0x4FC480), the
//  mark store @0x4065B6]
void HostRotationAdmin::clear(bool in_game) {
	if (in_game) rotation_.list.free_buffer();
}

// The alt cursor (refused outside [0, count)), then the SETNEXT latch either way: the map
// change's teardown consumes it on a launch-option map (inmatch/map_change.h).
// [orig: MissionList_SetAltCursor @0x4FC730; g_RotationSetNextLatch = 1 @0x4066DF]
bool HostRotationAdmin::set_next(int32_t index) {
	const bool set = rotation_.list.set_alt_cursor(index);
	rotation_.setnext_latch = true;
	return set;
}

} // namespace opennova::inmatch
