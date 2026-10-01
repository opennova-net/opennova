// The mission's table (editor/documents/mission_table.h, ADR 0046 S13 D10): a mission's records as
// rows of the one table shape (a row per entity, path, area trigger, event, trigger and action, the
// mission row holding the loadout, the availability rules, the groups, the layers and the bounding
// boxes), over the format's field rows (formats/mission/mission_field.h). Over the synthetic dense
// mission: the rows in the writer's order, the Nth trigger row the file's Nth trigger (what a Record
// reference's index counts); a stop's marker and an event's run are Record references, a run of none
// naming none; every byte the writer takes from a record is a field's but the stated ones (the header,
// an entity, an event, a trigger, an action, an area trigger, a group, a layer and a bounding box, each
// field set to a value differing in every byte); a Set keeps the format's rule and the editor refuses
// a value past the field (a byte past 255, a 16.16 number past its word, a real that is no finite
// number, a flag no name says where the names are all the record holds, a group flag past bits 0 and
// 1, an event's delay past its ten bits, a nameless loadout entry, a name past its slot), a full slot
// keeps every byte, a position is exact in mission units, an entity's item is set by its items.def id;
// the lists: the fixed tables take nothing in or out, a path holds 32 stops and a stop put in or taken
// out writes the path's count as its slots where a flags edit leaves a stored count past them as it
// was read, and the file reparses after every edit. With the game install (a SKIP-LEG without
// OPENNOVA_JO_DIR), every mission it ships reads through the table and a Set of every field to the
// value it reads writes the same file but the bytes it names, each a fixed text slot's past its text in
// a slot the game is witnessed to read to its first NUL alone (the shipped missions' two: the terrain
// slot of TDH_I3A.bms and TKH_I3A.bms, three bytes each).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/mission_table.h>
#include <editor/graph/reference_kinds.h>
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
const FieldSchema &schema(MissionKind kind, const char *id) {
	const TableKind &row = *T().kind(k(kind));
	return row.fields()[row.find(id)];
}
// What the field names on the record: the record's own answer where its kind's row decides it.
ReferenceKind names(const RecordHandle &record, const char *id) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	return kind.reference(place) ? kind.reference(place)(record, RecordOwners{}) : kind.fields()[place].reference;
}

// A kind's list of `kind` and the ops it is edited through.
const TableList &list_of(MissionKind owner, MissionKind kind) {
	for (const TableList &list : T().kind(k(owner))->lists())
		if (list.spec.kind == k(kind)) return list;
	static const TableList none;
	return none;
}

// The file's rows of one kind, in order.
std::vector<RecordHandle> rows_of(bms::File &file, MissionKind kind) {
	std::vector<RecordHandle> out;
	for (const RecordHandle &row : mission_rows(file))
		if (row.kind == k(kind)) out.push_back(row);
	return out;
}

bool written(const bms::File &file, std::vector<uint8_t> &bytes) {
	std::string error;
	return bms::write(file, bytes, error);
}

bool reparses(const bms::File &file) {
	std::vector<uint8_t> bytes;
	std::string error;
	bms::File back;
	return written(file, bytes) && bms::parse(bytes.data(), bytes.size(), back, error) && bms::equal(file, back);
}

bool load(bms::File &file) {
	const std::vector<uint8_t> bytes =
	        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_dense.bms");
	std::string error;
	return !bytes.empty() && bms::parse(bytes.data(), bytes.size(), file, error);
}

// --- where a record's bytes lie in the written file -------------------------------------------------

struct Span {
	size_t at = 0, size = 0;
};

// The bytes of a record of the file as the writer lays them out (bms.cpp write): the header, the two
// chunks, the four pools, the 128 paths, the groups, the layers, the area triggers, the three counts,
// the events, the triggers, the actions, the box count and the boxes.
Span span_of(const bms::File &file, MissionKind kind, size_t index) {
	size_t at = bms::kHeaderSize + file.header.weapon_loadout_chunk_len + file.header.secondary_chunk_len;
	const auto skip = [&at](size_t count, size_t size) { at += count * size; };
	switch (kind) {
	case MissionKind::Mission: return {0, bms::kHeaderSize};
	case MissionKind::Item: return {at + index * bms::kEntitySize, bms::kEntitySize};
	default: break;
	}
	skip(file.items.size() + file.buildings.size() + file.markers.size() + file.organics.size(), bms::kEntitySize);
	skip(bms::kWaypointRecordCount, bms::kWaypointRecordSize);
	if (kind == MissionKind::Group) return {at + index * bms::kGroupRecordSize, bms::kGroupRecordSize};
	skip(bms::kGroupRecordCount, bms::kGroupRecordSize);
	if (kind == MissionKind::Layer) return {at + index * bms::kLayerRecordSize, bms::kLayerRecordSize};
	skip(bms::kLayerRecordCount, bms::kLayerRecordSize);
	if (kind == MissionKind::Area) return {at + index * bms::kAreaTriggerSize, bms::kAreaTriggerSize};
	skip(file.area_triggers.size(), bms::kAreaTriggerSize);
	at += 12;
	if (kind == MissionKind::Event) return {at + index * bms::kEventSize, bms::kEventSize};
	skip(file.events.size(), bms::kEventSize);
	if (kind == MissionKind::Trigger) return {at + index * bms::kTriggerSize, bms::kTriggerSize};
	skip(file.triggers.size(), bms::kTriggerSize);
	if (kind == MissionKind::Action) return {at + index * bms::kActionSize, bms::kActionSize};
	skip(file.actions.size(), bms::kActionSize);
	at += 4;
	return {at + index * bms::kBoundingBoxSize, bms::kBoundingBoxSize};
}

// A value of the field that differs from `value` in every byte the field writes: a number's
// complement inside its range (a flag word's within the bits its names say, where they are all it
// holds; a delay's within its ten bits), a real whose 16.16 word or float is the complement, a full
// slot of letters other than the text's.
Value other_value(const FieldSchema &field, const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) {
		std::string out(field.width ? field.width - 1 : text->size() + 1, 'Q');
		for (size_t i = 0; i < text->size() && i < out.size(); ++i)
			if ((*text)[i] == 'Q') out[i] = 'R';
		return out;
	}
	if (const auto *real = std::get_if<double>(&value)) {
		if (field.max > 65536.0) { // a float: a number none of whose bytes is its own
			float now = float(*real);
			uint32_t was = 0;
			std::memcpy(&was, &now, 4);
			for (float candidate : {-1234.5678f, 98765.4375f, 0.00012345f, -7.25f}) {
				uint32_t bits = 0;
				std::memcpy(&bits, &candidate, 4);
				bool every = true;
				for (int b = 0; b < 4; ++b) every = every && ((bits >> (8 * b)) & 0xFF) != ((was >> (8 * b)) & 0xFF);
				if (every) return double(candidate);
			}
			return *real;
		}
		const int32_t word = bms::to_fixed_16_16(*real);
		return double(~word) / 65536.0;
	}
	const int64_t number = std::get<int64_t>(value);
	if (!field.choices.empty() && !field.open_choices) {
		int64_t bits = 0;
		for (const FieldChoice &choice : field.choices) bits |= choice.value;
		return number ^ bits;
	}
	const int64_t span = int64_t(field.max) - int64_t(field.min);
	if (field.min < 0) return int64_t(field.max) - (number - int64_t(field.min)); // the complement in its range
	return number ^ (span >= 0xFFFFFFFFll ? 0xFFFFFFFFll : span);
}

// The synthetic mission, with a record of a table it holds none of where the test needs one.
using Prepare = void (*)(bms::File &file);
void with_area(bms::File &file) {
	file.area_triggers.push_back(bms::AreaTrigger{3, 1, 2, 3, 4, 5, 6, bms::AreaTrigger::kFlagConstrainZ});
	mission::sync_counts(file);
}
void with_box(bms::File &file) {
	file.bounding_boxes.push_back(bms::BoundingBox{1, 2, 3, 4, 5, 6, 5, -1, 0});
	mission::sync_counts(file);
}

// Every field of the record set to a value differing in every byte (the file reloaded between): the
// bytes of the record's span any field's Set changes, which must be every byte but `excluded` (the
// members no field is, mission_field.h), and no byte past the span.
int coverage(MissionKind kind, size_t index, const std::set<size_t> &excluded, const char *what,
             Prepare prepare = nullptr) {
	bms::File file;
	TEST_EXPECT(load(file));
	if (prepare) prepare(file);
	std::vector<uint8_t> before;
	TEST_EXPECT(written(file, before));
	const Span span = span_of(file, kind, index);
	std::vector<bool> covered(span.size, false);
	int failures = 0;
	const TableKind &table = *T().kind(k(kind));
	for (const FieldSchema &field : table.fields()) {
		if (field.read_only) continue;
		bms::File edited;
		TEST_EXPECT(load(edited));
		if (prepare) prepare(edited);
		const std::vector<RecordHandle> rows = kind == MissionKind::Mission ? std::vector<RecordHandle>{mission_rows(edited)[0]}
		                                                                   : std::vector<RecordHandle>{};
		RecordHandle record;
		if (kind == MissionKind::Mission) record = rows[0];
		else if (kind == MissionKind::Group || kind == MissionKind::Layer || kind == MissionKind::BoundingBox)
			record = list_of(MissionKind::Mission, kind).ops.at(mission_rows(edited)[0], index);
		else record = rows_of(edited, kind)[index];
		Value value;
		TEST_EXPECT(get(record, field.id, value));
		const Value other = other_value(field, value);
		std::string error;
		if (!set(record, field.id, other, error)) {
			std::fprintf(stderr, "%s.%s refuses another value: %s\n", what, field.id.c_str(), error.c_str());
			++failures;
			continue;
		}
		mission::sync_counts(edited);
		std::vector<uint8_t> after;
		TEST_EXPECT(written(edited, after) && after.size() == before.size());
		size_t changed = 0;
		for (size_t i = 0; i < before.size(); ++i) {
			if (before[i] == after[i]) continue;
			if (i < span.at || i >= span.at + span.size) {
				std::fprintf(stderr, "%s.%s changed byte %zu past its record\n", what, field.id.c_str(), i);
				++failures;
				continue;
			}
			covered[i - span.at] = true;
			++changed;
		}
		if (!changed) {
			std::fprintf(stderr, "%s.%s changed no byte\n", what, field.id.c_str());
			++failures;
		}
	}
	for (size_t i = 0; i < span.size; ++i)
		if (covered[i] == (excluded.count(i) != 0)) {
			std::fprintf(stderr, "%s: byte %zu is %s\n", what, i, covered[i] ? "a field's, but excluded" : "no field's");
			++failures;
		}
	TEST_EXPECT(failures == 0);
	return 0;
}

std::set<size_t> bytes(std::initializer_list<std::pair<size_t, size_t>> ranges) {
	std::set<size_t> out;
	for (const auto &range : ranges)
		for (size_t i = range.first; i < range.first + range.second; ++i) out.insert(i);
	return out;
}

// --- the tests -------------------------------------------------------------------------------------

// The rows, in the writer's order; what the Record references count; the table's invariants.
int test_rows() {
	TEST_EXPECT(T().well_formed());
	bms::File file;
	TEST_EXPECT(load(file));
	const std::vector<RecordHandle> rows = mission_rows(file);
	const size_t entities = file.items.size() + file.buildings.size() + file.markers.size() + file.organics.size();
	TEST_EXPECT(rows.size() == 1 + entities + bms::kWaypointRecordCount + file.area_triggers.size() +
	                                   file.events.size() + file.triggers.size() + file.actions.size());
	TEST_EXPECT(rows[0].kind == k(MissionKind::Mission) && rows[0].data == &file);
	for (const RecordHandle &row : rows) TEST_EXPECT(T().kinds()[size_t(row.kind)].top);
	// The Nth row of a kind is the file's Nth record of its table: what a Record reference's index
	// counts (RecordIndexSpace::File).
	const std::vector<RecordHandle> triggers = rows_of(file, MissionKind::Trigger);
	const std::vector<RecordHandle> markers = rows_of(file, MissionKind::Marker);
	TEST_EXPECT(triggers.size() == file.triggers.size() && triggers.back().data == &file.triggers.back());
	TEST_EXPECT(markers.size() == file.markers.size() && markers[3].data == &file.markers[3]);
	// The references' collections are the table's kinds by token.
	for (ReferenceKind reference : {ReferenceKind::MissionMarker, ReferenceKind::MissionTrigger, ReferenceKind::MissionAction}) {
		const ReferenceKindRow &row = reference_row(reference);
		bool found = false;
		for (const RecordKindRow &kind : T().kinds()) found = found || std::string(kind.token) == row.collection;
		TEST_EXPECT(row.resolution == ReferenceResolution::Record && found);
	}
	TEST_EXPECT(schema(MissionKind::Stop, "marker").reference == ReferenceKind::MissionMarker);
	TEST_EXPECT(schema(MissionKind::Event, "trigger_index").reference == ReferenceKind::MissionTrigger &&
	            schema(MissionKind::Event, "action_index").reference == ReferenceKind::MissionAction);
	// An event's run names its first trigger while it holds one; a run of none names none.
	const RecordHandle event = rows_of(file, MissionKind::Event)[0];
	TEST_EXPECT(file.events[0].trigger_count > 0 && names(event, "trigger_index") == ReferenceKind::MissionTrigger);
	file.events[0].trigger_count = 0;
	TEST_EXPECT(names(event, "trigger_index") == ReferenceKind::None);
	std::printf("rows: %zu in the writer's order, a stop's marker and an event's runs Record references\n", rows.size());
	return 0;
}

// Every byte the writer takes from a record is a field's but the members mission_field.h states.
int test_complete() {
	// The header: its magic, the blocks bms.h leaves unnamed (unknown0 @140, unknown1 @154, unknown2
	// @163, unknown3 @204, unknown4 @236, something1 @251, unknown5 @260, unknown6 @552, unknown9 @588),
	// its counts @164..183 and its count and chunk lengths @576, @578, @582.
	if (coverage(MissionKind::Mission, 0,
	             bytes({{0, 4}, {140, 12}, {154, 4}, {163, 1}, {164, 20}, {204, 16}, {236, 10}, {251, 1}, {260, 4},
	                    {552, 2}, {576, 4}, {582, 2}, {588, 28}}),
	             "header") != 0)
		return 1;
	// An entity: its four reserved words, which the writer writes as zero (unk22 @82, gen_reserved0
	// @151, unk42b @166, unk43 @168).
	if (coverage(MissionKind::Item, 0, bytes({{82, 2}, {151, 1}, {166, 2}, {168, 4}}), "entity") != 0) return 1;
	// An event: its flags word past the known bits and its delays' low 22 bits (the writer masks and
	// packs them), its latch and its pad (written zero).
	if (coverage(MissionKind::Event, 1, bytes({{1, 3}, {12, 2}, {16, 2}, {20, 1}, {23, 1}}), "event") != 0) return 1;
	// A trigger's and an action's reserved words (written zero).
	if (coverage(MissionKind::Trigger, 0, bytes({{28, 4}}), "trigger") != 0) return 1;
	if (coverage(MissionKind::Action, 0, bytes({{0, 4}, {28, 4}}), "action") != 0) return 1;
	if (coverage(MissionKind::Area, 0, {}, "area trigger", with_area) != 0) return 1;
	// A group: the words the writer writes as constants (@4 zero, @12 ten, @16..31 zero) and its flags
	// word past bits 0 and 1.
	if (coverage(MissionKind::Group, 0, bytes({{1, 3}, {4, 4}, {12, 20}}), "group") != 0) return 1;
	if (coverage(MissionKind::Layer, 0, {}, "layer") != 0) return 1;
	// A bounding box's reserved word (written zero).
	if (coverage(MissionKind::BoundingBox, 0, bytes({{32, 4}}), "bounding box", with_box) != 0) return 1;
	std::printf("complete: every byte the writer takes from a header, an entity, an event, a trigger, an action, an "
	            "area trigger, a group, a layer and a bounding box is a field's, but the stated members\n");
	return 0;
}

int test_fields() {
	bms::File file;
	TEST_EXPECT(load(file));
	const RecordHandle root = mission_rows(file)[0];
	const mission::MissionInfo info = mission::mission_info(file);
	Value value;
	TEST_EXPECT(get(root, "mission_name", value) && std::get<std::string>(value) == info.mission_name);
	TEST_EXPECT(get(root, "terrain", value) && std::get<std::string>(value) == info.terrain);
	TEST_EXPECT(number(root, "minutes_per_day") == info.minutes_per_day);
	std::string error;
	// A terrain edit writes its own 16-byte slot alone (cnv_file and tt_file stay, each a field of its
	// own).
	std::memcpy(file.header.terrain + 16, "CNV", 4);
	TEST_EXPECT(set(root, "terrain", std::string("Island"), error) && mission::mission_info(file).terrain == "Island" &&
	            std::strcmp(file.header.terrain + 16, "CNV") == 0 && get(root, "cnv_file", value) &&
	            std::get<std::string>(value) == "CNV");
	TEST_EXPECT(!set(root, "terrain", std::string(17, 'x'), error));
	// A colour packed 0xRRGGBB, red first; a score byte; the mana.
	TEST_EXPECT(set(root, "fog_color", int64_t(0x102030), error) && file.header.fog_color[0] == 0x10 &&
	            file.header.fog_color[2] == 0x30 && number(root, "fog_color") == 0x102030);
	TEST_EXPECT(!set(root, "fog_color", int64_t(0x1000000), error));
	TEST_EXPECT(set(root, "win_scores[2]", int64_t(7), error) && file.header.win_scores[2] == 7 &&
	            set(root, "mana", int64_t(55), error) && file.header.mana == 55);
	// A real that is no finite number, or past what a float holds, is none (the map zoom).
	for (double bad : {std::numeric_limits<double>::infinity(), std::nan(""), 1e300})
		TEST_EXPECT(!set(root, "map_zoom", bad, error));
	TEST_EXPECT(set(root, "map_zoom", 2.5, error) && file.header.map_zoom == 2.5f);

	const RecordHandle item = rows_of(file, MissionKind::Item)[0];
	TEST_EXPECT(item.data == &file.items[0]);
	// The editor refuses past the field; the format's own rule clamps (bms_edit's setter by name).
	TEST_EXPECT(!set(item, "team", int64_t(300), error) && set(item, "team", int64_t(2), error) &&
	            file.items[0].team == 2);
	TEST_EXPECT(mission::set_entity_property_int(file, mission::EntityKind::Item, 0, "map_symbol", 300, error) &&
	            file.items[0].map_symbol == 255);
	// An AI flag past the known attributes is refused, by the editor (its names are every bit the
	// record holds) and by the format's rule.
	TEST_EXPECT(!schema(MissionKind::Item, "ai_flags").open_choices && !set(item, "ai_flags", int64_t(1) << 3, error));
	TEST_EXPECT(!mission::set_entity_property_int(file, mission::EntityKind::Item, 0, "ai_flags", 1 << 3, error) &&
	            error == "Mission entity AI flags include unsupported bits");
	TEST_EXPECT(set(item, "ai_flags", int64_t(uint32_t(bms::BmsiAttributeFlags::Blind)), error) &&
	            file.items[0].bmsi_attributes == uint32_t(bms::BmsiAttributeFlags::Blind));
	// A name that fills its 8-byte slot keeps every byte; a ninth is past it.
	TEST_EXPECT(set(item, "name1", std::string("ABCDEFGH"), error) &&
	            std::memcmp(file.items[0].name1, "ABCDEFGH", 8) == 0 && get(item, "name1", value) &&
	            std::get<std::string>(value) == "ABCDEFGH");
	TEST_EXPECT(!set(item, "name1", std::string("ABCDEFGHI"), error));
	// A position exact in mission units (16.16), where a float would round it; one past the word, or
	// no finite number, is refused.
	const double x = 2047.12345678;
	TEST_EXPECT(set(item, "x", x, error) && file.items[0].x == bms::to_fixed_16_16(x) && get(item, "x", value) &&
	            std::get<double>(value) == double(file.items[0].x) / 65536.0);
	TEST_EXPECT(file.items[0].x != bms::to_fixed_16_16(float(x)));
	for (double bad : {40000.0, -32768.5, std::numeric_limits<double>::infinity(), std::nan("")})
		TEST_EXPECT(!set(item, "x", bad, error) && file.items[0].x == bms::to_fixed_16_16(x));
	TEST_EXPECT(schema(MissionKind::Item, "x").ranged && schema(MissionKind::Item, "x").min == bms::kFixed16Min);
	// The item: the items.def id its type names, set by it.
	TEST_EXPECT(number(item, "item") == mission::entity_item_id(file.items[0]));
	TEST_EXPECT(set(item, "item", int64_t(mission::kItemIdOffset + 42), error) && file.items[0].type_id == 42 &&
	            number(item, "item") == mission::kItemIdOffset + 42);
	// The members the binding never named: the id, the second accuracy, the critical flag, the next SSN.
	TEST_EXPECT(set(item, "id", int64_t(9001), error) && file.items[0].id == 9001 &&
	            set(item, "w_accuracy2", int64_t(-3), error) && file.items[0].w_accuracy2 == -3 &&
	            set(item, "mission_critical", int64_t(1), error) && file.items[0].mission_critical == 1 &&
	            set(item, "next_ssn", int64_t(77), error) && file.items[0].next_ssn == 77);
	TEST_EXPECT(set(item, "crouch_timer", int64_t(0x1234), error) && file.items[0].crouch_timer == 0x34 &&
	            file.items[0].unk15a == 0x12);

	const RecordHandle group = list_of(MissionKind::Mission, MissionKind::Group).ops.at(root, 3);
	TEST_EXPECT(!set(group, "flags", int64_t(4), error) && set(group, "flags", int64_t(3), error) &&
	            file.group_records[3].flags == 3);
	const RecordHandle layer = list_of(MissionKind::Mission, MissionKind::Layer).ops.at(root, 0);
	TEST_EXPECT(set(layer, "name", std::string(20, 'L'), error) &&
	            std::memcmp(file.layer_records[0].name, std::string(20, 'L').data(), 20) == 0);
	TEST_EXPECT(!set(layer, "name", std::string(21, 'L'), error));

	const RecordHandle event = rows_of(file, MissionKind::Event)[1];
	TEST_EXPECT(!set(event, "delay", int64_t(2000), error) && set(event, "delay", int64_t(1023), error) &&
	            file.events[1].delay == 1023);
	// The internal bits the event holds stay; the author's are the value's; a bit no name says (the
	// writer would drop it) is refused.
	file.events[1].flags = static_cast<bms::EventFlags>(0x10u);
	TEST_EXPECT(set(event, "flags", int64_t(0x1), error) && uint32_t(file.events[1].flags) == 0x11u);
	TEST_EXPECT(!schema(MissionKind::Event, "flags").open_choices && !set(event, "flags", int64_t(0x8), error));
	// Where the names are the values the format knows, another is the record's (an action type, an
	// attribute bit).
	TEST_EXPECT(schema(MissionKind::Action, "action_type").open_choices && schema(MissionKind::Mission, "attrib_flags").open_choices);
	TEST_EXPECT(set(rows_of(file, MissionKind::Action)[0], "action_type", int64_t(29), error));

	const RecordHandle entry = list_of(MissionKind::Mission, MissionKind::Loadout).ops.at(root, 0);
	TEST_EXPECT(!set(entry, "name", std::string(), error) && error == "Weapon loadout entries require a name");
	TEST_EXPECT(set(entry, "flags", std::string(), error) && file.loadout.entries[0].flags == "-1");
	TEST_EXPECT(reparses(file));
	std::printf("fields: the rules kept, the editor's range and names first, a full slot whole, positions exact\n");
	return 0;
}

int test_lists() {
	bms::File file;
	TEST_EXPECT(load(file));
	const RecordHandle root = mission_rows(file)[0];
	std::string error;

	// The fixed tables: set, never added to or removed.
	for (MissionKind fixed : {MissionKind::Group, MissionKind::Layer}) {
		const TableList &list = list_of(MissionKind::Mission, fixed);
		const DetachedRecord one = list.ops.copy(root, 0);
		TEST_EXPECT(list.spec.fixed && !list.ops.insert(root, 0, &one, error) && !list.ops.erase(root, 0));
	}

	// A path's stops: copies of its own, 32 at most, any marker index (a Record reference the mission
	// document checks); one put in or taken out writes the count as the slots.
	const RecordHandle path = rows_of(file, MissionKind::WaypointPath)[0];
	const ListOps &stops = list_of(MissionKind::WaypointPath, MissionKind::Stop).ops;
	const size_t held = stops.size(path);
	TEST_EXPECT(!stops.insert(path, 0, nullptr, error));
	DetachedRecord stop = stops.copy(path, 0);
	TEST_EXPECT(stops.insert(path, held, &stop, error) && stops.size(path) == held + 1 &&
	            file.waypoint_records[0].marker_count == held + 1);
	const RecordHandle last = stops.at(path, held);
	TEST_EXPECT(set(last, "marker", int64_t(file.markers.size()), error) &&
	            file.waypoint_records[0].waypoint_numbers[held] == file.markers.size() &&
	            names(last, "marker") == ReferenceKind::MissionMarker);
	*static_cast<uint32_t *>(stop.data.get()) = 0;
	while (stops.size(path) < mission::kMaxWaypointPathMarkers) TEST_EXPECT(stops.insert(path, 0, &stop, error));
	TEST_EXPECT(!stops.insert(path, 0, &stop, error));
	// A count past the 32 slots (CP19.bms ships 39): a flags edit leaves it as it was read, a stop taken
	// out writes it as the slots.
	file.waypoint_records[0].marker_count = 39;
	TEST_EXPECT(set(path, "flags", int64_t(1), error) && file.waypoint_records[0].marker_count == 39 &&
	            number(path, "marker_count") == 39);
	TEST_EXPECT(stops.erase(path, 0) && stops.size(path) == mission::kMaxWaypointPathMarkers - 1 &&
	            file.waypoint_records[0].marker_count == mission::kMaxWaypointPathMarkers - 1 && reparses(file));
	TEST_EXPECT(load(file));

	// A loadout entry and an availability rule come in as copies (a new one would have no name); a
	// bounding box comes in as zero; each keeps the header's lengths and counts.
	const ListOps &loadout = list_of(MissionKind::Mission, MissionKind::Loadout).ops;
	TEST_EXPECT(!loadout.insert(root, 0, nullptr, error));
	const DetachedRecord kit = loadout.copy(root, 1);
	TEST_EXPECT(loadout.insert(root, 0, &kit, error) && file.loadout.entries.size() == 5 && reparses(file));
	const ListOps &availability = list_of(MissionKind::Mission, MissionKind::Availability).ops;
	TEST_EXPECT(!availability.insert(root, 0, nullptr, error));
	DetachedRecord rule;
	rule.kind = k(MissionKind::Availability);
	rule.data = std::make_shared<bms::ItemAvailabilityEntry>(bms::ItemAvailabilityEntry{"WPN_TEST", 3});
	TEST_EXPECT(availability.insert(root, 0, &rule, error) && file.item_availability.size() == 1 &&
	            file.header.secondary_chunk_len > 0 && reparses(file));
	TEST_EXPECT(availability.erase(root, 0) && file.header.secondary_chunk_len == 0);
	const ListOps &boxes = list_of(MissionKind::Mission, MissionKind::BoundingBox).ops;
	const size_t box_count = boxes.size(root);
	TEST_EXPECT(boxes.insert(root, box_count, nullptr, error) && file.bounding_box_count == int32_t(box_count + 1) &&
	            reparses(file));
	TEST_EXPECT(boxes.erase(root, box_count) && file.bounding_box_count == int32_t(box_count) && reparses(file));
	std::printf("lists: the fixed tables, a path's stops and its count, the loadout, the availability rules and the "
	            "boxes through the table\n");
	return 0;
}

// --- the retail sweep ----------------------------------------------------------------------------------

void each_record(const RecordHandle &record, const std::function<void(const RecordHandle &)> &fn) {
	fn(record);
	for (const TableList &list : T().kind(record.kind)->lists())
		for (size_t i = 0; i < list.ops.size(record); ++i) each_record(list.ops.at(record, i), fn);
}

// The record a written byte lies in, by its kind and index, and the byte's place in it (the writer's
// layout, span_of's); false for a byte of the chunks and the counts.
bool record_at(const bms::File &file, size_t offset, MissionKind &kind, size_t &index, size_t &place) {
	const auto in = [&](MissionKind as, size_t count, size_t size, size_t &at) {
		if (offset >= at && offset < at + count * size) {
			kind = as;
			index = (offset - at) / size;
			place = (offset - at) % size;
			return true;
		}
		at += count * size;
		return false;
	};
	size_t at = 0;
	if (in(MissionKind::Mission, 1, bms::kHeaderSize, at)) return true;
	at += file.header.weapon_loadout_chunk_len + file.header.secondary_chunk_len;
	return in(MissionKind::Item, file.items.size(), bms::kEntitySize, at) ||
	       in(MissionKind::Building, file.buildings.size(), bms::kEntitySize, at) ||
	       in(MissionKind::Marker, file.markers.size(), bms::kEntitySize, at) ||
	       in(MissionKind::Organic, file.organics.size(), bms::kEntitySize, at) ||
	       in(MissionKind::WaypointPath, bms::kWaypointRecordCount, bms::kWaypointRecordSize, at) ||
	       in(MissionKind::Group, bms::kGroupRecordCount, bms::kGroupRecordSize, at) ||
	       in(MissionKind::Layer, bms::kLayerRecordCount, bms::kLayerRecordSize, at) ||
	       in(MissionKind::Area, file.area_triggers.size(), bms::kAreaTriggerSize, at) ||
	       (at += 12, in(MissionKind::Event, file.events.size(), bms::kEventSize, at)) ||
	       in(MissionKind::Trigger, file.triggers.size(), bms::kTriggerSize, at) ||
	       in(MissionKind::Action, file.actions.size(), bms::kActionSize, at) ||
	       (at += 4, in(MissionKind::BoundingBox, file.bounding_boxes.size(), bms::kBoundingBoxSize, at));
}

// A fixed text slot of a record: its field and its bytes in the record, found on the synthetic
// mission by the bytes a Set of a full slot of other letters moves (the header's, an entity's, a
// layer's).
struct Slot {
	std::string field;
	Span span;
};
std::map<MissionKind, std::vector<Slot>> text_slots() {
	std::map<MissionKind, std::vector<Slot>> out;
	for (MissionKind kind : {MissionKind::Mission, MissionKind::Item, MissionKind::Layer}) {
		for (const FieldSchema &field : T().kind(k(kind))->fields()) {
			if (field.type != FieldType::Text || !field.width || field.read_only) continue;
			bms::File file;
			if (!load(file)) return {};
			std::vector<uint8_t> before, after;
			written(file, before);
			const RecordHandle record = kind == MissionKind::Mission ? mission_rows(file)[0]
			                            : kind == MissionKind::Layer
			                                    ? list_of(MissionKind::Mission, kind).ops.at(mission_rows(file)[0], 0)
			                                    : rows_of(file, kind)[0];
			Value value;
			std::string error;
			get(record, field.id, value);
			set(record, field.id, other_value(field, value), error);
			written(file, after);
			const Span span = span_of(file, kind, 0);
			size_t first = 0, last = 0;
			bool any = false;
			for (size_t i = span.at; i < span.at + span.size; ++i)
				if (before[i] != after[i]) {
					if (!any) first = i;
					last = i;
					any = true;
				}
			if (any) out[kind].push_back({field.id, {first - span.at, last - first + 1}});
		}
	}
	return out;
}

struct RetailCounts {
	long fields = 0, set = 0, padded = 0;
	// Each run of bytes a Set of a text to itself zero-padded past its text, named: the mission, the
	// record, the field and the bytes of the slot.
	std::vector<std::string> named;
};

// The text slots whose bytes past their text the game is witnessed never to read, each cited where the
// format's row is (mission_field.cpp): the header's terrain, which every reader copies or prints to its
// first NUL. A Set that pads another slot's tail is a failure until its readers are cited too.
const char *const kCitedSlots[] = {"terrain"};

// Every field of every record of a mission set to the value it reads, then the file written once and
// compared with the file the parse writes: the same bytes but those a text slot's Set zero-padded past
// its text (copy_fixed_field pads a slot with zeros past the text it copies), each named, each in a
// slot kCitedSlots names.
int set_to_itself(const std::vector<uint8_t> &bytes, const std::string &name,
                  const std::map<MissionKind, std::vector<Slot>> &slots, RetailCounts &counts) {
	bms::File file;
	std::string error;
	if (!bms::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "%s: does not parse: %s\n", name.c_str(), error.c_str());
		return 1;
	}
	std::vector<uint8_t> before;
	TEST_EXPECT(written(file, before));
	int failures = 0;
	for (const RecordHandle &row : mission_rows(file))
		each_record(row, [&](const RecordHandle &record) {
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
				// A field the file leaves out (a loadout entry's fourth string) is not set: a Set writes
				// it, and the core never sets a field to the value it reads (Document::apply).
				if (kind.value(place).present && !kind.value(place).present(record)) continue;
				if (!kind.value(place).set(record, value, error)) {
					std::fprintf(stderr, "%s: %s.%s refuses its own value: %s\n", name.c_str(), kind.row().token,
					             field.id.c_str(), error.c_str());
					++failures;
					continue;
				}
				++counts.set;
			}
		});
	std::vector<uint8_t> after;
	TEST_EXPECT(written(file, after) && after.size() == before.size());
	size_t i = 0;
	while (i < before.size()) {
		if (before[i] == after[i]) {
			++i;
			continue;
		}
		size_t end = i;
		while (end < before.size() && before[end] != after[end]) ++end;
		// The run's record and the text slot it lies in, past the slot's text (its first NUL before the
		// run), every moved byte a zero.
		MissionKind kind = MissionKind::Mission;
		size_t index = 0, place = 0;
		const Slot *slot = nullptr;
		if (record_at(file, i, kind, index, place)) {
			const MissionKind layout = kind == MissionKind::Building || kind == MissionKind::Marker ||
			                                           kind == MissionKind::Organic
			                                   ? MissionKind::Item
			                                   : kind;
			const auto found = slots.find(layout);
			if (found != slots.end())
				for (const Slot &each : found->second)
					if (place >= each.span.at && place + (end - i) <= each.span.at + each.span.size) slot = &each;
		}
		bool padding = slot != nullptr;
		if (slot) {
			bool cited = false;
			for (const char *field : kCitedSlots) cited = cited || slot->field == field;
			padding = cited;
			const size_t start = i - place + slot->span.at;
			size_t text = 0;
			while (text < slot->span.size && after[start + text] != 0) ++text;
			padding = padding && start + text <= i;
			for (size_t j = i; j < end; ++j) padding = padding && after[j] == 0;
		}
		if (!padding) {
			std::fprintf(stderr, "%s: bytes %zu..%zu moved, no cited text slot's padding past its text%s%s\n",
			             name.c_str(), i, end - 1, slot ? ": " : "", slot ? slot->field.c_str() : "");
			++failures;
		} else {
			char line[256];
			std::snprintf(line, sizeof(line), "%s: %s %zu, %s, its bytes %zu..%zu (file @%zu..%zu)", name.c_str(),
			              T().kinds()[size_t(kind)].token, index, slot->field.c_str(), place - slot->span.at,
			              place - slot->span.at + (end - i) - 1, i, end - 1);
			counts.named.push_back(line);
			counts.padded += long(end - i);
		}
		i = end;
	}
	return failures;
}

int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every mission of the game install through the table)");
	const std::map<MissionKind, std::vector<Slot>> slots = text_slots();
	TEST_EXPECT(slots.size() == 3 && slots.at(MissionKind::Mission).size() == 9 && slots.at(MissionKind::Item).size() == 3);
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
			failures += set_to_itself(bytes, file.logical_name, slots, counts);
			++missions;
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	for (const std::string &line : counts.named) std::printf("  zero-padded past its text: %s\n", line.c_str());
	std::printf("retail: %d missions, %ld fields read, %ld set to themselves, each file written again the same but "
	            "%ld byte(s) in %zu slot(s) a text's Set padded with zeros past its text\n",
	            missions, counts.fields, counts.set, counts.padded, counts.named.size());
	TEST_EXPECT(failures == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_rows() != 0) return 1;
	if (test_complete() != 0) return 1;
	if (test_fields() != 0) return 1;
	if (test_lists() != 0) return 1;
	return test_retail();
}
