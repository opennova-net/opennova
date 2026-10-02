// The mission's table (editor/documents/mission_table.h, ADR 0046 S13 D10, S14): a mission's records
// as rows of the one table shape (a row per entity, path, area trigger and event, an event holding its
// triggers and its actions, the mission row holding the loadout, the availability rules, the groups,
// the layers and the bounding boxes), over the format's field rows (formats/mission/mission_field.h),
// read through the mission document's rows. Over the synthetic dense mission: the rows in the writer's
// order, each kind a band; what a record names of another (a stop's marker, an entity's group and
// path, an Event parameter's event by index; an entity's SSN and an area's zone id defined), a
// parameter's reference decided by its record's type; every byte the writer takes from a record is a
// field's but the stated ones (the header, an entity, an event, a trigger, an action, an area trigger,
// a group, a layer and a bounding box, each field set to a value differing in every byte); a Set keeps
// the format's rule and the editor refuses a value past the field (a byte past 255, a 16.16 number
// past its word, a real that is no finite number, a flag no name says where the names are all the
// record holds, a group flag past bits 0 and 1, an event's delay past its ten bits, a nameless
// loadout entry, a name past its slot), a full slot keeps every byte, a position is exact in mission
// units, an entity's item is set by its items.def id; the lists: the fixed tables take nothing in or
// out, a path holds 32 stops and a stop put in or taken out writes the path's count as its slots where
// a flags edit leaves a stored count past them as it was read, an event's chain holds 20 of each, a
// new bounding box is refused, and the file reparses after every edit. With the game install (a
// SKIP-LEG without OPENNOVA_JO_DIR), every mission it ships reads through the table and a Set of every
// field to the value it reads writes the same file but the bytes it names, each a fixed text slot's
// past its text in a slot the game is witnessed to read to its first NUL alone (the shipped missions'
// two: the terrain slot of TDH_I3A.bms and TKH_I3A.bms, three bytes each).
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
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/graph/reference_kinds.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_chains.h>

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

// A mission read through its document, its rows cloned: the natives the test reads and edits.
struct Loaded {
	MissionDocument document;
	std::vector<std::shared_ptr<const Node>> rows;
};

bool load(Loaded &m, const std::vector<uint8_t> &bytes, const std::string &name) {
	Diagnostic error;
	m.rows.clear();
	if (bytes.empty() || !m.document.load_bytes(bytes, name, AssetKind::Mission, "jo", error)) return false;
	for (const auto &row : m.document.rows()) m.rows.push_back(row->clone());
	return true;
}
bool load(Loaded &m) {
	return load(m, test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_dense.bms"),
	            "synth_dense.bms");
}

// The rows of one kind, in order.
std::vector<RecordHandle> rows_of(const Loaded &m, MissionKind kind) {
	std::vector<RecordHandle> out;
	for (const auto &row : m.rows)
		if (row->kind == k(kind)) out.push_back(static_cast<const TableRow &>(*row).record());
	return out;
}
RecordHandle root_of(const Loaded &m) { return rows_of(m, MissionKind::Mission)[0]; }
// The first record of a nested kind the file writes: the mission row's lists, the first event's.
RecordHandle nested(const Loaded &m, MissionKind kind, size_t index) {
	if (kind == MissionKind::Trigger || kind == MissionKind::Action)
		return list_of(MissionKind::Event, kind).ops.at(rows_of(m, MissionKind::Event)[0], index);
	return list_of(MissionKind::Mission, kind).ops.at(root_of(m), index);
}
RecordHandle record_of(const Loaded &m, MissionKind kind, size_t index) {
	return T().kinds()[size_t(k(kind))].top ? rows_of(m, kind)[index] : nested(m, kind, index);
}

bool compose(const Loaded &m, bms::File &file) { return compose_mission(m.rows, file); }
bool written(const Loaded &m, std::vector<uint8_t> &bytes) {
	bms::File file;
	std::string error;
	return compose(m, file) && bms::write(file, bytes, error);
}
bool reparses(const Loaded &m) {
	bms::File file, back;
	std::vector<uint8_t> bytes;
	std::string error;
	return compose(m, file) && bms::write(file, bytes, error) && bms::parse(bytes.data(), bytes.size(), back, error) &&
	       bms::equal(file, back);
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
using Prepare = void (*)(Loaded &m);
void with_area(Loaded &m) {
	m.rows.push_back(std::make_shared<AreaRow>(
	        k(MissionKind::Area), bms::AreaTrigger{3, 1, 2, 3, 4, 5, 6, bms::AreaTrigger::kFlagConstrainZ}));
}
void with_box(Loaded &m) {
	root_of(m).as<bms::File>().bounding_boxes.push_back(bms::BoundingBox{1, 2, 3, 4, 5, 6, 5, -1, 0});
}

// Every field of the record set to a value differing in every byte (the file reloaded between): the
// bytes of the record's span any field's Set changes, which must be every byte but `excluded` (the
// members no field is, mission_field.h), and no byte past the span.
int coverage(MissionKind kind, size_t index, const std::set<size_t> &excluded, const char *what,
             Prepare prepare = nullptr) {
	Loaded m;
	TEST_EXPECT(load(m));
	if (prepare) prepare(m);
	std::vector<uint8_t> before;
	bms::File laid;
	TEST_EXPECT(written(m, before) && compose(m, laid));
	const Span span = span_of(laid, kind, index);
	std::vector<bool> covered(span.size, false);
	int failures = 0;
	const TableKind &table = *T().kind(k(kind));
	for (const FieldSchema &field : table.fields()) {
		if (field.read_only) continue;
		Loaded edited;
		TEST_EXPECT(load(edited));
		if (prepare) prepare(edited);
		const RecordHandle record = record_of(edited, kind, index);
		Value value;
		TEST_EXPECT(get(record, field.id, value));
		const Value other = other_value(field, value);
		std::string error;
		if (!set(record, field.id, other, error)) {
			std::fprintf(stderr, "%s.%s refuses another value: %s\n", what, field.id.c_str(), error.c_str());
			++failures;
			continue;
		}
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

// The rows, in the writer's order; what the references count and name; the table's invariants.
int test_rows() {
	TEST_EXPECT(T().well_formed());
	Loaded m;
	TEST_EXPECT(load(m));
	bms::File file;
	TEST_EXPECT(compose(m, file));
	const size_t entities = file.items.size() + file.buildings.size() + file.markers.size() + file.organics.size();
	TEST_EXPECT(m.rows.size() == 1 + entities + bms::kWaypointRecordCount + file.area_triggers.size() + file.events.size());
	TEST_EXPECT(m.rows[0]->kind == k(MissionKind::Mission));
	// The rows stand in bands, in the writer's order; a trigger and an action are nested (no band).
	int band = 0;
	for (const auto &row : m.rows) {
		TEST_EXPECT(T().kinds()[size_t(row->kind)].top && mission_band(row->kind) >= band);
		band = mission_band(row->kind);
	}
	TEST_EXPECT(mission_band(k(MissionKind::Mission)) == 0 && mission_band(k(MissionKind::Event)) == 7 &&
	            mission_band(k(MissionKind::Trigger)) == -1 && mission_band(k(MissionKind::Stop)) == -1);
	// The Nth row of a kind is the file's Nth record of its table (what a Record reference's index
	// counts, RecordIndexSpace::File); an event holds the triggers and the actions of its runs.
	const std::vector<RecordHandle> markers = rows_of(m, MissionKind::Marker);
	TEST_EXPECT(markers.size() == file.markers.size() && markers[3].as<bms::Entity>().id == file.markers[3].id);
	size_t triggers = 0, actions = 0;
	for (const RecordHandle &event : rows_of(m, MissionKind::Event)) {
		triggers += list_of(MissionKind::Event, MissionKind::Trigger).ops.size(event);
		actions += list_of(MissionKind::Event, MissionKind::Action).ops.size(event);
	}
	TEST_EXPECT(triggers == file.triggers.size() && actions == file.actions.size());
	// An event's runs are what the writer derives: no field of the event.
	for (const char *run : {"trigger_index", "trigger_count", "action_index", "action_count"})
		TEST_EXPECT(T().kind(k(MissionKind::Event))->find(run) == TableKind::npos);
	// The references' collections are the table's kinds by token.
	for (ReferenceKind reference : {ReferenceKind::MissionMarker, ReferenceKind::MissionEvent, ReferenceKind::MissionGroup,
	                                ReferenceKind::MissionPath}) {
		const ReferenceKindRow &row = reference_row(reference);
		bool found = false;
		for (const RecordKindRow &kind : T().kinds()) found = found || std::string(kind.token) == row.collection;
		TEST_EXPECT(row.resolution == ReferenceResolution::Record && found);
	}
	TEST_EXPECT(schema(MissionKind::Stop, "marker").reference == ReferenceKind::MissionMarker);
	TEST_EXPECT(schema(MissionKind::Organic, "group").reference == ReferenceKind::MissionGroup &&
	            schema(MissionKind::Organic, "waypoint_id").reference == ReferenceKind::MissionPath &&
	            schema(MissionKind::Organic, "item").reference == ReferenceKind::Item &&
	            schema(MissionKind::Organic, "name2").reference == ReferenceKind::AiProfile);
	TEST_EXPECT(schema(MissionKind::Mission, "terrain").reference == ReferenceKind::Terrain &&
	            schema(MissionKind::Mission, "environment").reference == ReferenceKind::Environment &&
	            schema(MissionKind::Loadout, "name").reference == ReferenceKind::Weapon);
	// An entity is named by its SSN and an area trigger by its zone id: symbols, by id.
	TEST_EXPECT(schema(MissionKind::Item, "id").defines == ReferenceKind::MissionEntity &&
	            schema(MissionKind::Area, "id").defines == ReferenceKind::MissionZone &&
	            reference_row(ReferenceKind::MissionEntity).resolution == ReferenceResolution::Symbol &&
	            reference_row(ReferenceKind::MissionZone).severity_when_missing == DiagnosticSeverity::Warning);
	// A parameter names what its record's type says: the dense mission's triggers compare a mission
	// variable (no reference), its actions reset an event; a trigger made an Event trigger names one.
	const RecordHandle trigger = nested(m, MissionKind::Trigger, 0), action = nested(m, MissionKind::Action, 0);
	TEST_EXPECT(names(trigger, "param1") == ReferenceKind::None && names(action, "param1") == ReferenceKind::MissionEvent);
	trigger.as<bms::Trigger>().main_type = bms::TriggerMainType::Event;
	TEST_EXPECT(names(trigger, "param1") == ReferenceKind::MissionEvent && names(trigger, "param2") == ReferenceKind::None);
	trigger.as<bms::Trigger>().main_type = bms::TriggerMainType::Single;
	trigger.as<bms::Trigger>().sub_type = 10; // SingleIsWithinArea: the SSN, then the zone
	TEST_EXPECT(names(trigger, "param1") == ReferenceKind::MissionEntity && names(trigger, "param2") == ReferenceKind::MissionZone);
	// An entity's waypoint number is an SSN beside a command 123..125, a stop's number beside a path.
	const RecordHandle organic = rows_of(m, MissionKind::Organic)[0];
	TEST_EXPECT(names(organic, "wp_number") == ReferenceKind::None);
	organic.as<bms::Entity>().waypoint_id = 124;
	TEST_EXPECT(names(organic, "wp_number") == ReferenceKind::MissionEntity);
	std::printf("rows: %zu in the writer's order, in bands; the references by index and by id\n", m.rows.size());
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
	// packs them), its runs (@4, @8 and the counts @21, @22, which the writer derives from the chain it
	// holds), its latch and its pad (written zero).
	if (coverage(MissionKind::Event, 1, bytes({{1, 3}, {4, 8}, {12, 2}, {16, 2}, {20, 4}}), "event") != 0) return 1;
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
	Loaded m;
	TEST_EXPECT(load(m));
	const RecordHandle root = root_of(m);
	bms::File &file = root.as<bms::File>(); // the mission row's own: the header and its tables
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

	const RecordHandle item = rows_of(m, MissionKind::Item)[0];
	bms::Entity &entity = item.as<bms::Entity>();
	// The editor refuses past the field; the format's own rule clamps (bms_edit's setter by name).
	TEST_EXPECT(!set(item, "team", int64_t(300), error) && set(item, "team", int64_t(2), error) && entity.team == 2);
	bms::File whole;
	TEST_EXPECT(compose(m, whole));
	TEST_EXPECT(mission::set_entity_property_int(whole, mission::EntityKind::Item, 0, "map_symbol", 300, error) &&
	            whole.items[0].map_symbol == 255);
	// An AI flag past the known attributes is refused, by the editor (its names are every bit the
	// record holds) and by the format's rule.
	TEST_EXPECT(!schema(MissionKind::Item, "ai_flags").open_choices && !set(item, "ai_flags", int64_t(1) << 3, error));
	TEST_EXPECT(!mission::set_entity_property_int(whole, mission::EntityKind::Item, 0, "ai_flags", 1 << 3, error) &&
	            error == "Mission entity AI flags include unsupported bits");
	TEST_EXPECT(set(item, "ai_flags", int64_t(uint32_t(bms::BmsiAttributeFlags::Blind)), error) &&
	            entity.bmsi_attributes == uint32_t(bms::BmsiAttributeFlags::Blind));
	// A name that fills its 8-byte slot keeps every byte; a ninth is past it.
	TEST_EXPECT(set(item, "name1", std::string("ABCDEFGH"), error) && std::memcmp(entity.name1, "ABCDEFGH", 8) == 0 &&
	            get(item, "name1", value) && std::get<std::string>(value) == "ABCDEFGH");
	TEST_EXPECT(!set(item, "name1", std::string("ABCDEFGHI"), error));
	// A position exact in mission units (16.16), where a float would round it; one past the word, or
	// no finite number, is refused.
	const double x = 2047.12345678;
	TEST_EXPECT(set(item, "x", x, error) && entity.x == bms::to_fixed_16_16(x) && get(item, "x", value) &&
	            std::get<double>(value) == double(entity.x) / 65536.0);
	TEST_EXPECT(entity.x != bms::to_fixed_16_16(float(x)));
	for (double bad : {40000.0, -32768.5, std::numeric_limits<double>::infinity(), std::nan("")})
		TEST_EXPECT(!set(item, "x", bad, error) && entity.x == bms::to_fixed_16_16(x));
	TEST_EXPECT(schema(MissionKind::Item, "x").ranged && schema(MissionKind::Item, "x").min == bms::kFixed16Min);
	// The item: the items.def id its type names, set by it.
	TEST_EXPECT(number(item, "item") == mission::entity_item_id(entity));
	TEST_EXPECT(set(item, "item", int64_t(mission::kItemIdOffset + 42), error) && entity.type_id == 42 &&
	            number(item, "item") == mission::kItemIdOffset + 42);
	// The members the binding never named: the id, the second accuracy, the critical flag, the next SSN.
	TEST_EXPECT(set(item, "id", int64_t(9001), error) && entity.id == 9001 &&
	            set(item, "w_accuracy2", int64_t(-3), error) && entity.w_accuracy2 == -3 &&
	            set(item, "mission_critical", int64_t(1), error) && entity.mission_critical == 1 &&
	            set(item, "next_ssn", int64_t(77), error) && entity.next_ssn == 77);
	TEST_EXPECT(set(item, "crouch_timer", int64_t(0x1234), error) && entity.crouch_timer == 0x34 && entity.unk15a == 0x12);
	// What the game reads of a member: the blink pairs never, a marker's name index, another pool's
	// not witnessed.
	TEST_EXPECT(schema(MissionKind::Item, "blink_parent_a").applies == Applicability::Ignored &&
	            schema(MissionKind::Item, "next_ssn").applies == Applicability::Unverified);
	const TableKind &entities = *T().kind(k(MissionKind::Item));
	const size_t name_index = entities.find("name_index");
	TEST_EXPECT(entities.applies(name_index)(item, RecordOwners{}) == Applicability::Unverified &&
	            T().kind(k(MissionKind::Marker))->applies(name_index)(rows_of(m, MissionKind::Marker)[0], RecordOwners{}) ==
	                    Applicability::Reads);

	const RecordHandle group = nested(m, MissionKind::Group, 3);
	TEST_EXPECT(!set(group, "flags", int64_t(4), error) && set(group, "flags", int64_t(3), error) &&
	            file.group_records[3].flags == 3);
	const RecordHandle layer = nested(m, MissionKind::Layer, 0);
	TEST_EXPECT(set(layer, "name", std::string(20, 'L'), error) &&
	            std::memcmp(file.layer_records[0].name, std::string(20, 'L').data(), 20) == 0);
	TEST_EXPECT(!set(layer, "name", std::string(21, 'L'), error));

	const RecordHandle event = rows_of(m, MissionKind::Event)[1];
	bms::Event &native = event.as<mission::EventChain>().event;
	TEST_EXPECT(!set(event, "delay", int64_t(2000), error) && set(event, "delay", int64_t(1023), error) && native.delay == 1023);
	// The internal bits the event holds stay; the author's are the value's; a bit no name says (the
	// writer would drop it) is refused.
	native.flags = static_cast<bms::EventFlags>(0x10u);
	TEST_EXPECT(set(event, "flags", int64_t(0x1), error) && uint32_t(native.flags) == 0x11u);
	TEST_EXPECT(!schema(MissionKind::Event, "flags").open_choices && !set(event, "flags", int64_t(0x8), error));
	// Where the names are the values the format knows, another is the record's (an action type, an
	// attribute bit, a parameter that takes named values).
	TEST_EXPECT(schema(MissionKind::Action, "action_type").open_choices && schema(MissionKind::Mission, "attrib_flags").open_choices &&
	            schema(MissionKind::Trigger, "param1").open_choices);
	TEST_EXPECT(set(nested(m, MissionKind::Action, 0), "action_type", int64_t(29), error));
	// A parameter the record's type does not read is Ignored (a ResetEvent's second).
	const TableKind &actions = *T().kind(k(MissionKind::Action));
	const RecordHandle reset = list_of(MissionKind::Event, MissionKind::Action).ops.at(rows_of(m, MissionKind::Event)[1], 0);
	TEST_EXPECT(actions.applies(actions.find("param1"))(reset, RecordOwners{}) == Applicability::Reads &&
	            actions.applies(actions.find("param2"))(reset, RecordOwners{}) == Applicability::Ignored);

	const RecordHandle entry = nested(m, MissionKind::Loadout, 0);
	TEST_EXPECT(!set(entry, "name", std::string(), error) && error == "Weapon loadout entries require a name");
	TEST_EXPECT(set(entry, "flags", std::string(), error) && file.loadout.entries[0].flags == "-1");
	TEST_EXPECT(reparses(m));
	std::printf("fields: the rules kept, the editor's range and names first, a full slot whole, positions exact\n");
	return 0;
}

int test_lists() {
	Loaded m;
	TEST_EXPECT(load(m));
	const RecordHandle root = root_of(m);
	bms::File &file = root.as<bms::File>();
	std::string error;

	// The fixed tables: set, never added to or removed.
	for (MissionKind fixed : {MissionKind::Group, MissionKind::Layer}) {
		const TableList &list = list_of(MissionKind::Mission, fixed);
		const DetachedRecord one = list.ops.copy(root, 0);
		TEST_EXPECT(list.spec.fixed && !list.ops.insert(root, 0, &one, error) && !list.ops.erase(root, 0));
	}

	// A path's stops: 32 at most, any marker index (a Record reference the mission document checks),
	// a new one visiting the first marker; one put in or taken out writes the count as the slots.
	const RecordHandle path = rows_of(m, MissionKind::WaypointPath)[0];
	bms::WaypointRecord &record = path.as<MissionPath>().record;
	const size_t markers = rows_of(m, MissionKind::Marker).size();
	const ListOps &stops = list_of(MissionKind::WaypointPath, MissionKind::Stop).ops;
	const size_t held = stops.size(path);
	TEST_EXPECT(held > 0 && stops.insert(path, held, nullptr, error) && stops.size(path) == held + 1 &&
	            record.marker_count == held + 1 && number(stops.at(path, held), "marker") == 0);
	const RecordHandle last = stops.at(path, held);
	TEST_EXPECT(set(last, "marker", int64_t(markers), error) && record.waypoint_numbers[held] == markers &&
	            names(last, "marker") == ReferenceKind::MissionMarker);
	DetachedRecord stop = stops.copy(path, 0);
	*static_cast<uint32_t *>(stop.data.get()) = 0;
	while (stops.size(path) < mission::kMaxWaypointPathMarkers) TEST_EXPECT(stops.insert(path, 0, &stop, error));
	TEST_EXPECT(!stops.insert(path, 0, &stop, error));
	// A count past the 32 slots (CP19.bms ships 39): a flags edit leaves it as it was read, a stop taken
	// out writes it as the slots.
	record.marker_count = 39;
	TEST_EXPECT(set(path, "flags", int64_t(1), error) && record.marker_count == 39 && number(path, "marker_count") == 39);
	TEST_EXPECT(stops.erase(path, 0) && stops.size(path) == mission::kMaxWaypointPathMarkers - 1 &&
	            record.marker_count == mission::kMaxWaypointPathMarkers - 1 && reparses(m));
	TEST_EXPECT(load(m));
	const RecordHandle again = root_of(m);
	bms::File &own = again.as<bms::File>();
	(void)file;

	// An event's triggers and actions: a new record every word zero, 20 of each at most.
	const RecordHandle event = rows_of(m, MissionKind::Event)[0];
	const TableList &triggers = list_of(MissionKind::Event, MissionKind::Trigger);
	const size_t chained = triggers.ops.size(event);
	TEST_EXPECT(triggers.spec.max == kMaxEventRecords && triggers.ops.insert(event, chained, nullptr, error) &&
	            number(triggers.ops.at(event, chained), "main_type") == 0);
	while (triggers.ops.size(event) < kMaxEventRecords) TEST_EXPECT(triggers.ops.insert(event, 0, nullptr, error));
	TEST_EXPECT(!triggers.ops.insert(event, 0, nullptr, error) && reparses(m));
	TEST_EXPECT(load(m));
	const RecordHandle top = root_of(m);
	bms::File &native = top.as<bms::File>();
	(void)own;

	// A loadout entry and an availability rule come in as copies (a new one would have no name); a
	// bounding box too (what a new one would hold is not known); each keeps the header's lengths and
	// counts.
	const ListOps &loadout = list_of(MissionKind::Mission, MissionKind::Loadout).ops;
	TEST_EXPECT(!loadout.insert(top, 0, nullptr, error));
	const DetachedRecord kit = loadout.copy(top, 1);
	TEST_EXPECT(loadout.insert(top, 0, &kit, error) && native.loadout.entries.size() == 5 && reparses(m));
	const ListOps &availability = list_of(MissionKind::Mission, MissionKind::Availability).ops;
	TEST_EXPECT(!availability.insert(top, 0, nullptr, error));
	DetachedRecord rule;
	rule.kind = k(MissionKind::Availability);
	rule.data = std::make_shared<bms::ItemAvailabilityEntry>(bms::ItemAvailabilityEntry{"WPN_TEST", 3});
	TEST_EXPECT(availability.insert(top, 0, &rule, error) && native.item_availability.size() == 1 &&
	            native.header.secondary_chunk_len > 0 && reparses(m));
	TEST_EXPECT(availability.erase(top, 0) && native.header.secondary_chunk_len == 0);
	const ListOps &boxes = list_of(MissionKind::Mission, MissionKind::BoundingBox).ops;
	const size_t box_count = boxes.size(top);
	TEST_EXPECT(!boxes.insert(top, box_count, nullptr, error) && !error.empty());
	DetachedRecord box;
	box.kind = k(MissionKind::BoundingBox);
	box.data = std::make_shared<bms::BoundingBox>(bms::BoundingBox{1, 2, 3, 4, 5, 6, 5, -1, 0});
	TEST_EXPECT(boxes.insert(top, box_count, &box, error) && native.bounding_box_count == int32_t(box_count + 1) &&
	            reparses(m));
	TEST_EXPECT(boxes.erase(top, box_count) && native.bounding_box_count == int32_t(box_count) && reparses(m));
	std::printf("lists: the fixed tables, a path's stops and its count, an event's chain, the loadout, the "
	            "availability rules and the boxes through the table\n");
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
			Loaded m;
			if (!load(m)) return {};
			std::vector<uint8_t> before, after;
			written(m, before);
			const RecordHandle record = record_of(m, kind, 0);
			Value value;
			std::string error;
			get(record, field.id, value);
			set(record, field.id, other_value(field, value), error);
			written(m, after);
			bms::File laid;
			compose(m, laid);
			const Span span = span_of(laid, kind, 0);
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
// compared with the file the document writes untouched: the same bytes but those a text slot's Set
// zero-padded past its text (copy_fixed_field pads a slot with zeros past the text it copies), each
// named, each in a slot kCitedSlots names.
int set_to_itself(const std::vector<uint8_t> &bytes, const std::string &name,
                  const std::map<MissionKind, std::vector<Slot>> &slots, RetailCounts &counts) {
	Loaded m;
	if (!load(m, bytes, name) || m.document.blocked()) {
		std::fprintf(stderr, "%s: does not open as a mission document\n", name.c_str());
		return 1;
	}
	std::vector<uint8_t> before;
	TEST_EXPECT(written(m, before));
	int failures = 0;
	std::string error;
	for (const auto &row : m.rows)
		each_record(static_cast<const TableRow &>(*row).record(), [&](const RecordHandle &record) {
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
	bms::File file;
	TEST_EXPECT(written(m, after) && after.size() == before.size() && compose(m, file));
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
