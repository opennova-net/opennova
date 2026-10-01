// The mission's table (editor/documents/mission_table.h, ADR 0046 S13 D10): a mission's records as
// rows of the one table shape, over the format's field rows (formats/mission/mission_field.h) and
// bms_edit's typed structure. Over the synthetic dense mission: the header reads as mission_info
// does; a Set keeps the format's rule and the editor refuses a value past the field (a byte past 255,
// an AI flag past the known attributes, a group flag past bits 0 and 1, an event's delay past its ten
// bits, a nameless loadout entry, a name past its slot), a full slot keeps every byte, a position is
// exact in mission units; the lists: a new item is the format's default with the next free id, a
// duplicate takes the next free id, a marker removed leaves the paths naming it as they were (what
// renumbers them is the mission document's, S13 D8), the fixed tables take nothing in or out, a path
// holds 32 stops of the file's markers, an event's chain moves with it in the file's tables (a trigger
// in at its end moves the later events' ranges, out again gives them back; an event duplicated brings
// a copy of its chain; one removed takes its chain), and the file reparses after every edit. With the
// game install (a SKIP-LEG without OPENNOVA_JO_DIR), every mission it ships reads through the table and
// a Set of every field to the value it reads changes no byte but where the format's rule rewrites
// (a fixed text slot's bytes past its text, a path's stored count past its 32 slots), counted.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;
namespace fs = std::filesystem;

namespace {

const RecordTable &T() { return mission_table(); }
constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

bool get(const RecordHandle &record, const std::string &id, Value &out) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	return place != TableKind::npos && kind.value(place).get(record, out);
}
int64_t number(const RecordHandle &record, const std::string &id) {
	Value value;
	return get(record, id, value) && std::holds_alternative<int64_t>(value) ? std::get<int64_t>(value) : INT64_MIN;
}
bool set(const RecordHandle &record, const std::string &id, const Value &value, std::string &error) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	if (place == TableKind::npos || !kind.value(place).set) {
		error = "No such writable field.";
		return false;
	}
	return kind.value(place).set(record, value, error);
}

// The mission's list of `kind` and the ops it is edited through.
const TableList &list_of(MissionKind owner, MissionKind kind) {
	for (const TableList &list : T().kind(k(owner))->lists())
		if (list.spec.kind == k(kind)) return list;
	static const TableList none;
	return none;
}

bool reparses(const bms::File &file) {
	std::vector<uint8_t> bytes;
	std::string error;
	bms::File back;
	return bms::write(file, bytes, error) && bms::parse(bytes.data(), bytes.size(), back, error) &&
	       bms::equal(file, back);
}

bool load(bms::File &file) {
	const std::vector<uint8_t> bytes =
	        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_dense.bms");
	std::string error;
	return !bytes.empty() && bms::parse(bytes.data(), bytes.size(), file, error);
}

int test_fields() {
	bms::File file;
	TEST_EXPECT(load(file));
	const RecordHandle root = mission_record(file);
	const mission::MissionInfo info = mission::mission_info(file);
	Value value;
	TEST_EXPECT(get(root, "mission_name", value) && std::get<std::string>(value) == info.mission_name);
	TEST_EXPECT(get(root, "terrain", value) && std::get<std::string>(value) == info.terrain);
	TEST_EXPECT(number(root, "minutes_per_day") == info.minutes_per_day);
	std::string error;
	// A terrain edit writes its own 16-byte slot alone (cnv_file and tt_file stay).
	std::memcpy(file.header.terrain + 16, "CNV", 4);
	TEST_EXPECT(set(root, "terrain", std::string("Island"), error) && mission::mission_info(file).terrain == "Island" &&
	            std::strcmp(file.header.terrain + 16, "CNV") == 0);
	TEST_EXPECT(!set(root, "terrain", std::string(17, 'x'), error));

	const RecordHandle item = list_of(MissionKind::Mission, MissionKind::Item).ops.at(root, 0);
	TEST_EXPECT(item.kind == k(MissionKind::Item) && item.top == &file);
	// The editor refuses past the field; the format's own rule clamps (bms_edit's setter by name).
	TEST_EXPECT(!set(item, "team", int64_t(300), error) && set(item, "team", int64_t(2), error) &&
	            file.items[0].team == 2);
	TEST_EXPECT(mission::set_entity_property_int(file, mission::EntityKind::Item, 0, "map_symbol", 300, error) &&
	            file.items[0].map_symbol == 255);
	// An AI flag past the known attributes is refused, by either way in.
	TEST_EXPECT(!set(item, "ai_flags", int64_t(1) << 3, error) &&
	            error == "Mission entity AI flags include unsupported bits");
	TEST_EXPECT(set(item, "ai_flags", int64_t(uint32_t(bms::BmsiAttributeFlags::Blind)), error) &&
	            file.items[0].bmsi_attributes == uint32_t(bms::BmsiAttributeFlags::Blind));
	// A name that fills its 8-byte slot keeps every byte; a ninth is past it.
	TEST_EXPECT(set(item, "name1", std::string("ABCDEFGH"), error) &&
	            std::memcmp(file.items[0].name1, "ABCDEFGH", 8) == 0 && get(item, "name1", value) &&
	            std::get<std::string>(value) == "ABCDEFGH");
	TEST_EXPECT(!set(item, "name1", std::string("ABCDEFGHI"), error));
	// A position exact in mission units (16.16), where a float would round it.
	const double x = 2047.12345678;
	TEST_EXPECT(set(item, "x", x, error) && file.items[0].x == bms::to_fixed_16_16(x) && get(item, "x", value) &&
	            std::get<double>(value) == double(file.items[0].x) / 65536.0);
	TEST_EXPECT(file.items[0].x != bms::to_fixed_16_16(float(x)));
	// The item is the items.def id its type names, shown only.
	TEST_EXPECT(number(item, "item") == mission::entity_item_id(file.items[0]) && !set(item, "item", int64_t(1), error));

	const RecordHandle group = list_of(MissionKind::Mission, MissionKind::Group).ops.at(root, 3);
	TEST_EXPECT(!set(group, "flags", int64_t(4), error) && set(group, "flags", int64_t(3), error) &&
	            file.group_records[3].flags == 3);
	const RecordHandle layer = list_of(MissionKind::Mission, MissionKind::Layer).ops.at(root, 0);
	TEST_EXPECT(set(layer, "name", std::string(20, 'L'), error) && std::memcmp(file.layer_records[0].name, std::string(20, 'L').data(), 20) == 0);
	TEST_EXPECT(!set(layer, "name", std::string(21, 'L'), error));

	const RecordHandle event = list_of(MissionKind::Mission, MissionKind::Event).ops.at(root, 1);
	TEST_EXPECT(!set(event, "delay", int64_t(2000), error) && set(event, "delay", int64_t(1023), error) &&
	            file.events[1].delay == 1023);
	// The internal bits the event holds stay; the author's are the value's.
	file.events[1].flags = static_cast<bms::EventFlags>(0x10u);
	TEST_EXPECT(set(event, "flags", int64_t(0x1), error) && uint32_t(file.events[1].flags) == 0x11u);

	const RecordHandle entry = list_of(MissionKind::Mission, MissionKind::Loadout).ops.at(root, 0);
	TEST_EXPECT(!set(entry, "name", std::string(), error) && error == "Weapon loadout entries require a name");
	TEST_EXPECT(set(entry, "flags", std::string(), error) && file.loadout.entries[0].flags == "-1");
	TEST_EXPECT(reparses(file));
	std::printf("fields: the rules kept, the editor's range first, a full slot whole, positions exact\n");
	return 0;
}

int test_lists() {
	bms::File file;
	TEST_EXPECT(load(file));
	const RecordHandle root = mission_record(file);
	std::string error;
	int max_id = 0;
	for (const auto *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
		for (const bms::Entity &e : *pool) max_id = std::max(max_id, e.id);

	// A new item: the format's defaults, the next free id.
	const ListOps &items = list_of(MissionKind::Mission, MissionKind::Item).ops;
	const size_t item_count = file.items.size();
	TEST_EXPECT(items.insert(root, 0, nullptr, error) && file.items.size() == item_count + 1 &&
	            file.items[0].id == max_id + 1 && file.items[0].perception2 == 100 &&
	            file.header.num_items == item_count + 1);
	// A copy comes back in with an id of its own.
	const DetachedRecord copy = items.copy(root, 0);
	TEST_EXPECT(items.insert(root, 1, &copy, error) && file.items[1].id == max_id + 2);
	TEST_EXPECT(items.erase(root, 1) && items.erase(root, 0) && file.items.size() == item_count);

	// A marker removed: the paths that name it keep their stops (the document renumbers them).
	const ListOps &markers = list_of(MissionKind::Mission, MissionKind::Marker).ops;
	const std::vector<uint32_t> path0 = file.waypoint_records[0].waypoint_numbers;
	TEST_EXPECT(!path0.empty() && markers.erase(root, path0[0]) &&
	            file.waypoint_records[0].waypoint_numbers == path0 && file.header.num_markers == file.markers.size());
	TEST_EXPECT(load(file));

	// The fixed tables: set, never added to or removed.
	for (MissionKind fixed : {MissionKind::WaypointPath, MissionKind::Group, MissionKind::Layer}) {
		const TableList &list = list_of(MissionKind::Mission, fixed);
		const DetachedRecord one = list.ops.copy(root, 0);
		TEST_EXPECT(list.spec.fixed && !list.ops.insert(root, 0, &one, error) && !list.ops.erase(root, 0));
	}

	// A path's stops: copies of its own, a marker the file holds, 32 at most.
	const RecordHandle path = list_of(MissionKind::Mission, MissionKind::WaypointPath).ops.at(root, 0);
	const ListOps &stops = list_of(MissionKind::WaypointPath, MissionKind::Stop).ops;
	const size_t held = stops.size(path);
	TEST_EXPECT(!stops.insert(path, 0, nullptr, error));
	DetachedRecord stop = stops.copy(path, 0);
	TEST_EXPECT(stops.insert(path, held, &stop, error) && stops.size(path) == held + 1 &&
	            file.waypoint_records[0].marker_count == held + 1);
	const RecordHandle last = stops.at(path, held);
	TEST_EXPECT(last.top == &file && !set(last, "marker", int64_t(file.markers.size()), error) &&
	            set(last, "marker", int64_t(file.markers.size() - 1), error) &&
	            file.waypoint_records[0].waypoint_numbers[held] == file.markers.size() - 1);
	*static_cast<uint32_t *>(stop.data.get()) = uint32_t(file.markers.size());
	TEST_EXPECT(!stops.insert(path, 0, &stop, error));
	*static_cast<uint32_t *>(stop.data.get()) = 0;
	while (stops.size(path) < mission::kMaxWaypointPathMarkers) TEST_EXPECT(stops.insert(path, 0, &stop, error));
	TEST_EXPECT(!stops.insert(path, 0, &stop, error));
	TEST_EXPECT(stops.erase(path, 0) && stops.size(path) == mission::kMaxWaypointPathMarkers - 1 && reparses(file));
	TEST_EXPECT(load(file));

	// An event's chain: a trigger in at the end of event 0 moves the later events' ranges, out again
	// gives them back.
	const RecordHandle event0 = list_of(MissionKind::Mission, MissionKind::Event).ops.at(root, 0);
	const ListOps &triggers = list_of(MissionKind::Event, MissionKind::Trigger).ops;
	const std::vector<bms::Event> events = file.events;
	const size_t chain = triggers.size(event0);
	const DetachedRecord trigger = triggers.copy(event0, 0);
	TEST_EXPECT(triggers.insert(event0, chain, &trigger, error) && triggers.size(event0) == chain + 1 &&
	            file.events[1].trigger_index == events[1].trigger_index + 1 &&
	            number(triggers.at(event0, chain), "param1") == number(triggers.at(event0, 0), "param1"));
	TEST_EXPECT(triggers.erase(event0, chain) && file.events[1].trigger_index == events[1].trigger_index &&
	            file.triggers.size() == size_t(file.trigger_count));
	// A trigger moved to another event: out of one, into the other.
	const RecordHandle event2 = list_of(MissionKind::Mission, MissionKind::Event).ops.at(root, 2);
	const size_t into = triggers.size(event2);
	TEST_EXPECT(triggers.erase(event0, 0) && triggers.size(event0) == chain - 1);
	const RecordHandle event2_again = list_of(MissionKind::Mission, MissionKind::Event).ops.at(root, 2);
	TEST_EXPECT(triggers.insert(event2_again, 0, &trigger, error) && triggers.size(event2_again) == into + 1 &&
	            reparses(file));
	(void)event2;
	TEST_EXPECT(load(file));

	// An event duplicated brings a copy of its chain, at the end of the file's tables; removed, it takes
	// it.
	const ListOps &event_ops = list_of(MissionKind::Mission, MissionKind::Event).ops;
	const size_t triggers_before = file.triggers.size(), actions_before = file.actions.size();
	const DetachedRecord event_copy = event_ops.copy(root, 2);
	TEST_EXPECT(event_ops.insert(root, file.events.size(), &event_copy, error) && file.events.size() == 4);
	const RecordHandle copied = event_ops.at(root, 3);
	TEST_EXPECT(triggers.size(copied) == 3 && file.events[3].trigger_index == int32_t(triggers_before) &&
	            file.triggers.size() == triggers_before + 3 && file.actions.size() == actions_before + 1 &&
	            number(triggers.at(copied, 2), "param2") == number(triggers.at(event_ops.at(root, 2), 2), "param2"));
	TEST_EXPECT(event_ops.erase(root, 3) && file.triggers.size() == triggers_before &&
	            file.actions.size() == actions_before && reparses(file));
	// A new event holds no chain; a new zone is every field zero.
	TEST_EXPECT(event_ops.insert(root, 0, nullptr, error) && file.events[0].trigger_count == 0 && reparses(file));
	const ListOps &areas = list_of(MissionKind::Mission, MissionKind::Area).ops;
	TEST_EXPECT(areas.insert(root, 0, nullptr, error) && file.header.area_trigger_count == 1 && reparses(file));
	// A loadout entry comes in as a copy: a new one would have no name.
	const ListOps &loadout = list_of(MissionKind::Mission, MissionKind::Loadout).ops;
	TEST_EXPECT(!loadout.insert(root, 0, nullptr, error));
	const DetachedRecord kit = loadout.copy(root, 1);
	TEST_EXPECT(loadout.insert(root, 0, &kit, error) && file.loadout.entries.size() == 5 && reparses(file));
	std::printf("lists: pools, fixed tables, stops, chains, events, zones and the loadout through the table\n");
	return 0;
}

struct RetailCounts {
	long fields = 0, set = 0, slot_tails = 0, over_counts = 0;
};

void each_record(const RecordHandle &record, const std::function<void(const RecordHandle &)> &fn) {
	fn(record);
	for (const TableList &list : T().kind(record.kind)->lists())
		for (size_t i = 0; i < list.ops.size(record); ++i) each_record(list.ops.at(record, i), fn);
}

template <class R> void append(std::vector<uint8_t> &out, const R &value) {
	const auto *p = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), p, p + sizeof(R));
}

// The record's own bytes as its struct holds them (a path's and a loadout entry's members one by one):
// what a Set of one of its fields to itself leaves as they were.
std::vector<uint8_t> record_bytes(const RecordHandle &record) {
	std::vector<uint8_t> out;
	switch (MissionKind(record.kind)) {
	case MissionKind::Mission: append(out, record.as<bms::File>().header); break;
	case MissionKind::Loadout: {
		const auto &entry = record.as<bms::WeaponLoadoutRecord>();
		for (const std::string *text : {&entry.name, &entry.ammo_primary, &entry.ammo_secondary, &entry.flags}) {
			out.insert(out.end(), text->begin(), text->end());
			out.push_back(0);
		}
		break;
	}
	case MissionKind::Item:
	case MissionKind::Building:
	case MissionKind::Marker:
	case MissionKind::Organic: append(out, record.as<bms::Entity>()); break;
	case MissionKind::WaypointPath: {
		const auto &path = record.as<bms::WaypointRecord>();
		append(out, path.flags);
		append(out, path.marker_count);
		for (uint32_t stop : path.waypoint_numbers) append(out, stop);
		out.insert(out.end(), path.padding.begin(), path.padding.end());
		break;
	}
	case MissionKind::Stop: append(out, record.as<uint32_t>()); break;
	case MissionKind::Group: append(out, record.as<bms::GroupRecord>()); break;
	case MissionKind::Layer: append(out, record.as<bms::LayerRecord>()); break;
	case MissionKind::Area: append(out, record.as<bms::AreaTrigger>()); break;
	case MissionKind::Event: append(out, record.as<bms::Event>()); break;
	case MissionKind::Trigger: append(out, record.as<bms::Trigger>()); break;
	case MissionKind::Action: append(out, record.as<bms::Action>()); break;
	default: break;
	}
	return out;
}

bool written(const bms::File &file, std::vector<uint8_t> &bytes) {
	std::string error;
	return bms::write(file, bytes, error);
}

// Every field of every record of a mission read and set to what it reads, its record's bytes compared
// around each Set: a change is the format's rule rewriting (a fixed text slot's bytes past its text
// zero-padded, a path's stored count past its 32 slots resynced), counted, or a failure; the file then
// written once, the same bytes where no rule rewrote.
int set_to_itself(const std::vector<uint8_t> &bytes, const std::string &name, RetailCounts &counts) {
	bms::File file;
	std::string error;
	if (!bms::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "%s: does not parse: %s\n", name.c_str(), error.c_str());
		return 1;
	}
	std::vector<uint8_t> before;
	TEST_EXPECT(written(file, before));
	int failures = 0;
	long rewritten = 0;
	each_record(mission_record(file), [&](const RecordHandle &record) {
		const TableKind &kind = *T().kind(record.kind);
		for (size_t place = 0; place < kind.fields().size(); ++place) {
			const FieldSchema &field = kind.fields()[place];
			Value value;
			if (!kind.value(place).get(record, value)) {
				std::fprintf(stderr, "%s: %s.%s does not read\n", name.c_str(), kind.row().token, field.id.c_str());
				++failures;
				continue;
			}
			++counts.fields;
			if (!kind.value(place).set || field.read_only) continue;
			const std::vector<uint8_t> was = record_bytes(record);
			if (!kind.value(place).set(record, value, error)) {
				std::fprintf(stderr, "%s: %s.%s refuses its own value: %s\n", name.c_str(), kind.row().token,
				             field.id.c_str(), error.c_str());
				++failures;
				continue;
			}
			++counts.set;
			if (record_bytes(record) == was) continue;
			++rewritten;
			if (field.type == FieldType::Text && field.width) ++counts.slot_tails;
			else if (record.kind == k(MissionKind::WaypointPath)) ++counts.over_counts;
			else {
				std::fprintf(stderr, "%s: %s.%s set to itself changed its record\n", name.c_str(), kind.row().token,
				             field.id.c_str());
				++failures;
			}
		}
	});
	std::vector<uint8_t> after;
	TEST_EXPECT(written(file, after));
	if (!rewritten && after != before) {
		std::fprintf(stderr, "%s: its fields set to themselves changed the file, no record's\n", name.c_str());
		++failures;
	}
	return failures;
}

int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every mission of the game install through the table)");
	// The base game's missions and each expansion's (each file once, by its archive and name).
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	int missions = 0, failures = 0;
	RetailCounts counts;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(fs::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			failures += set_to_itself(bytes, file.logical_name, counts);
			++missions;
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %d missions, %ld fields read, %ld set to themselves; rewritten by the format's rules: %ld "
	            "fixed slots past their text, %ld path counts past their slots\n",
	            missions, counts.fields, counts.set, counts.slot_tails, counts.over_counts);
	TEST_EXPECT(failures == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(T().well_formed());
	if (test_fields() != 0) return 1;
	if (test_lists() != 0) return 1;
	return test_retail();
}
