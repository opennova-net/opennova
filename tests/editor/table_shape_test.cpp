// The one table shape (editor/model/table_shape.h, ADR 0046 S13 D10) as each table that is rows of it
// fills it: the menu's, the def catalog's (its items with their attachments, its weapons and carry
// limits, its powerup rows), the model's and the mission's. Over each table's records, read from a file
// of the format: the table keeps its invariants (each kind at its place, each list naming kinds of the
// table, no kind in two lists of one owner, each field's id its own); every record a list holds is of a
// kind the list holds; every field reads a value of its type (the value), a number whose field names
// its values reads one of them or bits of them (the choice), a field whose record or the records it lies
// in decide what it is answers on every record, given the records it lies in (the labelled field), and
// a writable field set to the value it reads leaves the file's bytes as they were; every list gives a
// copy of its first record, takes it back in at its end, gives it back out, and the file's bytes are as
// they were (the list ops), a new record of each kind it holds where the list makes one.
//
// The core's record document over a table (model/table_document.h), over a synthetic table of its own:
// a list of two kinds walked as the kind each record is, an Add of each through the list's make; a
// field the records it lies in decide (a book's pages by its shelf); a Move whose erase the list
// refuses leaves the record, its identities and the file as they were.
//
// And the cost of a keystroke (D2's count-based rules): a Set of a changed plain member in a def
// catalog resolves 4 field rows of its own kind (5 for a Set of the value it holds), each one probe of
// the kind's index, and no other field's row; a Set in a member the file writes in its own units (an
// item's player_speed in km/h) resolves the same 4 and runs the family's parser 3 times over the line
// alone and 2 times over the whole record, which the log line times. With the game install (a SKIP-LEG
// without OPENNOVA_JO_DIR), the game's powerup.def through the same sweep, written as the format's
// canonical writer writes it and a fixed point.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mnu_table.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/table_document.h>
#include <formats/def/def_schema.h>
#include <formats/def/def_write.h>
#include <formats/mission/bms.h>
#include <formats/mnu/mnu.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::editor;
namespace def = opennova::def;
namespace bms = opennova::bms;

namespace {

std::string fixture(const char *relative) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative; }

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A table and the records it reads, as natives the test owns: the rows' handles and how the file
// they make writes.
struct Subject {
	std::string name;
	const RecordTable *table = nullptr;
	std::function<std::vector<RecordHandle>()> roots;
	std::function<std::vector<uint8_t>()> write;
};

// What a sweep met, for the log line and the counts each subject keeps to.
struct Counts {
	size_t records = 0, fields = 0, set = 0, choices = 0, decided = 0, lists = 0, copies = 0, made = 0, refused = 0;
	std::map<NodeKind, size_t> kinds; // the records met, by kind
};

// Every record a row holds, the row's own first, each list's records with what they hold before the
// next, each given the records it lies in (the row down to its nearest owner).
void each_record(const RecordTable &table, const RecordHandle &record, std::vector<OwnerStep> &trail,
                 const std::function<void(const RecordHandle &, const RecordOwners &)> &fn) {
	fn(record, RecordOwners{trail.data(), trail.size()});
	const std::vector<TableList> &lists = table.kind(record.kind)->lists();
	for (size_t l = 0; l < lists.size(); ++l)
		for (size_t i = 0; i < lists[l].ops.size(record); ++i) {
			trail.push_back({record, l, i});
			each_record(table, lists[l].ops.at(record, i), trail, fn);
			trail.pop_back();
		}
}
void each_record(const RecordTable &table, const RecordHandle &root,
                 const std::function<void(const RecordHandle &, const RecordOwners &)> &fn) {
	std::vector<OwnerStep> trail;
	each_record(table, root, trail, fn);
}

bool of_type(const FieldSchema &field, const Value &value) {
	switch (field.type) {
	case FieldType::Text: return std::holds_alternative<std::string>(value);
	case FieldType::Real: return std::holds_alternative<double>(value);
	default: return std::holds_alternative<int64_t>(value);
	}
}

// A number one of the field's values names, or bits of them; a field open to others reads any.
bool names_it(const FieldSchema &field, const Value &value) {
	const int64_t *number = std::get_if<int64_t>(&value);
	if (field.choices.empty() || field.open_choices || !number) return true;
	if (field.flags) {
		int64_t bits = 0;
		for (const FieldChoice &choice : field.choices) bits |= choice.value;
		return (*number & ~bits) == 0;
	}
	for (const FieldChoice &choice : field.choices)
		if (choice.value == *number) return true;
	return false;
}

// An expectation inside a walk's callback: counted, the walk going on (TEST_EXPECT returns).
#define SHAPE_CHECK(expr)                                                                     \
	do {                                                                                      \
		if (!(expr)) {                                                                        \
			std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, #expr); \
			++failures;                                                                       \
		}                                                                                     \
	} while (0)

// The value, the choice and the labelled field over every record; then a set of each written field to
// the value it reads.
int fields(const Subject &subject, Counts &counts) {
	const RecordTable &table = *subject.table;
	const std::vector<uint8_t> before = subject.write();
	TEST_EXPECT(!before.empty());
	int failures = 0;
	for (const RecordHandle &root : subject.roots()) {
		each_record(table, root, [&](const RecordHandle &record, const RecordOwners &owners) {
			++counts.records;
			++counts.kinds[record.kind];
			const TableKind *kind = table.kind(record.kind);
			SHAPE_CHECK(kind && record.data);
			if (!kind) return;
			// A row has no owners; any other record its row's own first.
			SHAPE_CHECK(owners.empty() == kind->row().top || (owners.size && owners[0].owner.data == root.data));
			for (size_t place = 0; place < kind->fields().size(); ++place) {
				const FieldSchema &field = kind->fields()[place];
				const FieldValue &value = kind->value(place);
				Value read;
				if (!value.get(record, read) || !of_type(field, read)) {
					std::fprintf(stderr, "%s: %s.%s does not read a value of its type\n", subject.name.c_str(),
					             kind->row().token, field.id.c_str());
					++failures;
					continue;
				}
				++counts.fields;
				if (!field.choices.empty()) {
					++counts.choices;
					if (!names_it(field, read)) {
						std::fprintf(stderr, "%s: %s.%s reads %lld, none of its values\n", subject.name.c_str(),
						             kind->row().token, field.id.c_str(), (long long)std::get<int64_t>(read));
						++failures;
					}
				}
				if (kind->applies(place)) {
					const Applicability applies = kind->applies(place)(record, owners);
					SHAPE_CHECK(applies == Applicability::Reads || applies == Applicability::Ignored ||
					            applies == Applicability::Unverified);
					++counts.decided;
				}
				if (kind->reference(place)) {
					const ReferenceKind names = kind->reference(place)(record, owners);
					SHAPE_CHECK(reference_row(names).kind == names);
					++counts.decided;
				}
				// A field the file writes, set to what it reads (the core skips such a Set; the table's
				// own set keeps the bytes too).
				if (!value.set || field.read_only || (value.present && !value.present(record))) continue;
				std::string error;
				if (!value.set(record, read, error)) {
					std::fprintf(stderr, "%s: %s.%s refuses its own value: %s\n", subject.name.c_str(), kind->row().token,
					             field.id.c_str(), error.c_str());
					++failures;
					continue;
				}
				++counts.set;
			}
		});
	}
	if (subject.write() != before) {
		std::fprintf(stderr, "%s: a Set of every field to itself changed the bytes\n", subject.name.c_str());
		++failures;
	}
	TEST_EXPECT(failures == 0);
	return 0;
}

// The list ops: each list of each owner kind once (its first owner that holds a record of it), a copy
// of its first record in at its end and out again; a new record of each kind it holds in and out where
// the list makes one. The bytes as they were after each. The edit happens inside the walk on the owner
// the walk is at, changing that owner's own list alone and giving it back as it was, so every handle
// the walk makes after it is made again from the records as they stand.
int lists(const Subject &subject, Counts &counts) {
	const RecordTable &table = *subject.table;
	const std::vector<uint8_t> before = subject.write();
	std::set<std::pair<NodeKind, size_t>> done;
	int failures = 0;
	for (const RecordHandle &root : subject.roots()) {
		each_record(table, root, [&](const RecordHandle &owner, const RecordOwners &) {
			const std::vector<TableList> &held = table.kind(owner.kind)->lists();
			for (size_t l = 0; l < held.size(); ++l) {
				const TableList &list = held[l];
				const ListOps &ops = list.ops;
				SHAPE_CHECK(ops.size && ops.at && ops.insert && ops.erase && ops.copy);
				const size_t size = ops.size(owner);
				for (size_t i = 0; i < size; ++i) {
					const RecordHandle record = ops.at(owner, i);
					if (!record || !list.holds(record.kind)) {
						std::fprintf(stderr, "%s: %s's list %s holds a record of kind %d at %zu, not one it holds\n",
						             subject.name.c_str(), table.kind(owner.kind)->row().token, list.spec.label,
						             int(record.kind), i);
						++failures;
					}
				}
				if (list.spec.fixed || size == 0 || !done.insert({owner.kind, l}).second) continue;
				++counts.lists;
				std::string error;
				const DetachedRecord copy = ops.copy(owner, 0);
				SHAPE_CHECK(copy.data && list.holds(copy.kind));
				if (ops.insert(owner, size, &copy, error)) {
					SHAPE_CHECK(ops.size(owner) == size + 1 && ops.at(owner, size).kind == copy.kind);
					SHAPE_CHECK(ops.erase(owner, size) && ops.size(owner) == size);
					++counts.copies;
				} else {
					// Only a list that holds no more (a part, a powerup's one action block of a name).
					SHAPE_CHECK(list.spec.max && size >= list.spec.max);
				}
				for (NodeKind kind : list.held()) {
					error.clear();
					const bool made = list.kinds.empty() ? ops.insert(owner, size, nullptr, error)
					                                     : ops.make(owner, size, kind, error);
					if (made) {
						SHAPE_CHECK(ops.size(owner) == size + 1 && ops.at(owner, size).kind == kind &&
						            ops.erase(owner, size) && ops.size(owner) == size);
						++counts.made;
					} else {
						SHAPE_CHECK(!error.empty());
						++counts.refused;
					}
				}
				if (subject.write() != before) {
					std::fprintf(stderr, "%s: %s's list %s in and out changed the bytes\n", subject.name.c_str(),
					             table.kind(owner.kind)->row().token, list.spec.label);
					++failures;
				}
			}
		});
	}
	TEST_EXPECT(failures == 0);
	return 0;
}

int sweep(const Subject &subject, Counts &counts) {
	TEST_EXPECT(subject.table && subject.table->well_formed());
	if (fields(subject, counts) != 0 || lists(subject, counts) != 0) return 1;
	std::printf("%s: %zu records, %zu fields read (%zu with values by name, %zu decided by their record), %zu set "
	            "to themselves; %zu lists: %zu copies in and out, %zu new records made, %zu refused\n",
	            subject.name.c_str(), counts.records, counts.fields, counts.choices, counts.decided, counts.set,
	            counts.lists, counts.copies, counts.made, counts.refused);
	return 0;
}

// --- the subjects ------------------------------------------------------------------------------------

// The menu: every screen of a file of every widget, cloned from the document's rows.
int menu() {
	MnuDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(test_io::read_file(fixture("mnu/all_widgets.mnu")), "all_widgets.mnu",
	                                AssetKind::Menu, "JO", error));
	std::vector<std::shared_ptr<MenuScreen>> rows;
	for (const auto &row : document.rows())
		rows.push_back(std::static_pointer_cast<MenuScreen>(row->clone()));
	Subject subject{"menu", &menu_table(), nullptr, nullptr};
	subject.roots = [&] {
		std::vector<RecordHandle> out;
		for (const auto &row : rows) out.push_back(row->record());
		return out;
	};
	subject.write = [&] {
		opennova::mnu::Document native = document.native();
		native.screens.clear();
		for (const auto &row : rows) native.screens.push_back(row->screen);
		std::vector<uint8_t> bytes;
		std::string message;
		opennova::mnu::serialize_bytes(native, bytes, message);
		return bytes;
	};
	Counts counts;
	if (sweep(subject, counts) != 0) return 1;
	TEST_EXPECT(counts.records > 50 && counts.lists > 10 && counts.copies > 10 && counts.choices > 0 && counts.decided > 0);
	return 0;
}

// A def catalog of `kind` over `bytes`: its rows cloned, written through the family's writer.
int catalog(const char *name, AssetKind kind, const std::vector<uint8_t> &bytes, Counts &counts) {
	DefCatalogDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes, name, kind, "JO", error) && !document.blocked());
	std::vector<std::shared_ptr<CatalogRow>> rows;
	for (const auto &row : document.rows()) rows.push_back(std::static_pointer_cast<CatalogRow>(row->clone()));
	const CatalogFamily *family = catalog_family(kind);
	TEST_EXPECT(family);
	Subject subject{name, &catalog_table(), nullptr, nullptr};
	subject.roots = [&] {
		std::vector<RecordHandle> out;
		for (const auto &row : rows) out.push_back(row->record());
		return out;
	};
	subject.write = [&] {
		std::vector<const CatalogRecord *> natives;
		for (const auto &row : rows) natives.push_back(&row->native);
		const def::DefWriteResult written = family->write(natives, document.file_state());
		return written.ok() ? text_bytes(written.text) : std::vector<uint8_t>();
	};
	return sweep(subject, counts);
}

// powerup.def as the catalog opens it: a row of every key, both action blocks, two ammo rows.
const char *const kPowerups =
        "powerup \"PU_MED\"\r\nrespawn_time 30\r\nmax_respawns 3\r\nhp -1\r\nmana 5\r\nweapon WPN_TEST\r\n"
        "ammo AT_TEST -1\r\nammo AT_TWO 3\r\n"
        "action pickup\r\nfunction powerup_med\r\nsoundset SND_PICK\r\ndelaystart auto\r\ndelayend 10\r\nend\r\n"
        "action respawn\r\nparticle FX_BACK\r\nend\r\nend\r\n"
        "powerup \"PU_ALL\"\r\nweapon all\r\nallammo\r\nend\r\n";

// An item mounting two weapons on its user points (an emplacement each, the second with its arc): what
// the shared items.def fixture lacks, so the item's attachment list is met.
const char *const kMountedItem =
        "begin \"Mounted Gun Truck\"\r\n  id 101999\r\n  type vehicle\r\n  addeweap FX00 101291\r\n"
        "  addeweapg FX01 101292 10 20 30 40\r\nend\r\n";

int catalogs() {
	Counts items, weapons, powerups;
	const std::vector<uint8_t> shared = test_io::read_file(fixture("def/items.def"));
	if (catalog("items.def", AssetKind::ItemDefs, text_bytes(std::string(shared.begin(), shared.end()) + kMountedItem),
	            items) != 0)
		return 1;
	if (catalog("weapon.def", AssetKind::WeaponDefs,
	            text_bytes("ammoclass_max_carry bullets 300\nweapon \"WPN_TEST\"\ncategory 1\nround_type AT_TEST\n"
	                       "sights scope 0 0 640 480 blend\naction \"Fire\"\nfunction Shoot 1 2\ndelay auto\nend\nend\n"),
	            weapons) != 0)
		return 1;
	if (catalog("powerup.def", AssetKind::PowerupDefs, text_bytes(kPowerups), powerups) != 0) return 1;
	TEST_EXPECT(items.records > 10 && items.set > 1000 && weapons.lists >= 2 && powerups.records == 6 &&
	            powerups.lists == 3);
	// The item's attachments are met, each a record of the item's list (Item -> Attachment).
	TEST_EXPECT(items.kinds[node_kind(def::DefRecordKind::Attachment)] == 2);
	return 0;
}

// The model: a model of every engine feature, its two rows cloned and composed for the writer.
int model() {
	ModelDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(test_io::read_file(fixture("threedi/synth/armory.3di")), "armory.3di",
	                                AssetKind::Model, "JO", error));
	auto row = std::static_pointer_cast<ModelRow>(document.model_row()->clone());
	auto collision = std::static_pointer_cast<CollisionRow>(document.collision_row()->clone());
	Subject subject{"model", &model_table(), nullptr, nullptr};
	subject.roots = [&] { return std::vector<RecordHandle>{row->record(), collision->record()}; };
	subject.write = [&] {
		ComposedModel composed;
		compose_model(*row, collision.get(), composed);
		std::vector<uint8_t> bytes;
		opennova::threedi::threedi_3di3_write_memory(&composed.model, bytes);
		return bytes;
	};
	Counts counts;
	if (sweep(subject, counts) != 0) return 1;
	TEST_EXPECT(counts.records > 20 && counts.lists > 5 && counts.choices > 0);
	return 0;
}

// The mission: the synthetic dense mission (its rows: the mission and its own tables, the entities of
// the four pools, the paths and their stops, the zones, the events with their triggers and actions).
int mission() {
	// The mission document's rows, cloned: the natives the test owns.
	MissionDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(test_io::read_file(fixture("bms/synth_dense.bms")), "synth_dense.bms",
	                                AssetKind::Mission, "JO", error));
	std::vector<std::shared_ptr<const Node>> rows;
	for (const auto &row : document.rows()) rows.push_back(row->clone());
	Subject subject{"mission", &mission_table(), nullptr, nullptr};
	subject.roots = [&] {
		std::vector<RecordHandle> out;
		for (const auto &row : rows) out.push_back(static_cast<const TableRow &>(*row).record());
		return out;
	};
	subject.write = [&] {
		bms::File file;
		std::vector<uint8_t> out;
		std::string message;
		if (compose_mission(rows, file)) bms::write(file, out, message);
		return out;
	};
	Counts counts;
	if (sweep(subject, counts) != 0) return 1;
	// The lists the synthetic mission holds records of: the loadout, a path's stops, an event's triggers
	// and its actions (its availability rules and boxes none; its groups and layers fixed).
	TEST_EXPECT(counts.records > 200 && counts.lists == 4 && counts.choices > 0 && counts.decided > 0);
	return 0;
}

// --- the core over a table of its own ----------------------------------------------------------------

// A shelf holds books and notes in one order (a list of two kinds), and leans or not: a book's pages are
// read only off a shelf that stands (a field the records it lies in decide). The list's erase refuses
// while `g_erase_refused` is set (a list whose op refuses, as a fixed table's does).
constexpr NodeKind kShelf = 0, kBook = 1, kNote = 2;
bool g_erase_refused = false;

struct Leaf {
	NodeKind kind = kBook;
	int64_t pages = 0;
};

struct Shelf : TableRow {
	int64_t lean = 0;
	std::vector<Leaf> leaves;
	Shelf() { kind = kShelf; }
	std::shared_ptr<Node> clone() const override { return std::make_shared<Shelf>(*this); }
	std::string name() const override { return "shelf"; }
	RecordHandle record() const override { return {kShelf, const_cast<Shelf *>(this)}; }
	size_t footprint() const override { return sizeof(Shelf) + leaves.size() * sizeof(Leaf) + ids_footprint(); }
};

LabelledField integer_field(const char *id, int64_t Leaf::*member) {
	LabelledField out;
	out.schema.id = id;
	out.schema.type = FieldType::Integer;
	out.value.get = [member](const RecordHandle &record, Value &value) {
		value = record.as<Leaf>().*member;
		return true;
	};
	out.value.set = [member](const RecordHandle &record, const Value &value, std::string &error) {
		const int64_t *number = std::get_if<int64_t>(&value);
		if (!number) {
			error = "A whole number.";
			return false;
		}
		record.as<Leaf>().*member = *number;
		return true;
	};
	return out;
}

const RecordTable &shelf_table() {
	static const RecordTable table = [] {
		TableKind shelf(RecordKindRow{kShelf, "shelf", "Shelf", "Add shelf", true});
		LabelledField lean;
		lean.schema.id = "lean";
		lean.schema.type = FieldType::Integer;
		lean.value.get = [](const RecordHandle &record, Value &value) {
			value = record.as<Shelf>().lean;
			return true;
		};
		lean.value.set = [](const RecordHandle &record, const Value &value, std::string &) {
			record.as<Shelf>().lean = std::get<int64_t>(value);
			return true;
		};
		shelf.field(lean);
		ListOps ops;
		ops.size = [](const RecordHandle &owner) { return owner.as<Shelf>().leaves.size(); };
		ops.at = [](const RecordHandle &owner, size_t index) {
			std::vector<Leaf> &leaves = owner.as<Shelf>().leaves;
			return index < leaves.size() ? RecordHandle{leaves[index].kind, &leaves[index]} : RecordHandle{};
		};
		ops.insert = [](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
			if (!record || !record->data || (record->kind != kBook && record->kind != kNote)) {
				error = "A shelf takes a copy of a book or a note.";
				return false;
			}
			std::vector<Leaf> &leaves = owner.as<Shelf>().leaves;
			leaves.insert(leaves.begin() + std::ptrdiff_t(std::min(index, leaves.size())),
			              *static_cast<const Leaf *>(record->data.get()));
			return true;
		};
		ops.make = [](const RecordHandle &owner, size_t index, NodeKind kind, std::string &error) {
			if (kind != kBook && kind != kNote) {
				error = "A shelf holds books and notes.";
				return false;
			}
			std::vector<Leaf> &leaves = owner.as<Shelf>().leaves;
			leaves.insert(leaves.begin() + std::ptrdiff_t(std::min(index, leaves.size())), Leaf{kind, 0});
			return true;
		};
		ops.erase = [](const RecordHandle &owner, size_t index) {
			std::vector<Leaf> &leaves = owner.as<Shelf>().leaves;
			if (g_erase_refused || index >= leaves.size()) return false;
			leaves.erase(leaves.begin() + std::ptrdiff_t(index));
			return true;
		};
		ops.copy = [](const RecordHandle &owner, size_t index) {
			DetachedRecord out;
			const std::vector<Leaf> &leaves = owner.as<Shelf>().leaves;
			if (index >= leaves.size()) return out;
			out.kind = leaves[index].kind;
			out.data = std::make_shared<Leaf>(leaves[index]);
			return out;
		};
		Document::CollectionSpec spec;
		spec.kind = kBook;
		spec.label = "Leaves";
		shelf.list({spec, ops, {kBook, kNote}});
		TableKind book(RecordKindRow{kBook, "book", "Book", "", false});
		LabelledField pages = integer_field("pages", &Leaf::pages);
		// A book's pages are read only off a shelf that stands: the shelf is the first record it lies in.
		pages.applies = [](const RecordHandle &, const RecordOwners &owners) {
			return owners.size && owners[0].owner.as<Shelf>().lean ? Applicability::Ignored : Applicability::Reads;
		};
		book.field(pages);
		TableKind note(RecordKindRow{kNote, "note", "Note", "", false});
		note.field(integer_field("pages", &Leaf::pages));
		return RecordTable({shelf, book, note});
	}();
	return table;
}

// Lines "shelf <lean> B<pages> N<pages> ...".
class ShelfDocument : public TableDocument {
public:
	const RecordTable &table() const override { return shelf_table(); }
	SerializeResult serialize() const override {
		SerializeResult out;
		for (const auto &node : rows()) {
			const Shelf &shelf = static_cast<const Shelf &>(*node);
			out.text += "shelf " + std::to_string(shelf.lean);
			for (const Leaf &leaf : shelf.leaves)
				out.text += std::string(" ") + (leaf.kind == kBook ? "B" : "N") + std::to_string(leaf.pages);
			out.text += "\n";
		}
		return out;
	}
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<ShelfDocument>(*this); }

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &, std::vector<SourceIssue> &, Diagnostic &) override {
		std::istringstream lines(std::string(bytes.begin(), bytes.end()));
		std::string line;
		while (std::getline(lines, line)) {
			std::istringstream words(line);
			std::string word;
			auto shelf = std::make_shared<Shelf>();
			words >> word >> shelf->lean;
			while (words >> word) shelf->leaves.push_back({word[0] == 'B' ? kBook : kNote, std::stoll(word.substr(1))});
			shape(*shelf);
			rows.push_back(shelf);
		}
		return true;
	}
};

Edit edit_of(EditOperation operation, NodeAddress address, NodeId parent = 0, size_t position = SIZE_MAX) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.parent = parent;
	edit.position = position;
	return edit;
}

// The core over the shelves: a list of two kinds, a field its records' owners decide, a Move refused by
// its list's erase.
int core_over_a_table() {
	TEST_EXPECT(shelf_table().well_formed());
	ShelfDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(text_bytes("shelf 0 B10 N2 B30\nshelf 1 B5\n"), "shelves.txt", AssetKind::Unknown,
	                                "jo", error));
	const Node &first = *document.rows()[0];
	// The walk meets each leaf as the kind it is, in the list's one order.
	std::vector<NodeKind> met;
	document.walk_records(first, [&](const NodeAddress &record, const Document::Placement &) {
		met.push_back(record.kind);
		return true;
	});
	TEST_EXPECT((met == std::vector<NodeKind>{kBook, kNote, kBook}));
	const auto leaf = [&](size_t row, size_t index) {
		const Node &node = *document.rows()[row];
		const auto &shelf = static_cast<const Shelf &>(node);
		return NodeAddress{node.id, shelf.leaves[index].kind, shelf.ids.lists[0][index].id};
	};
	// An Add of each kind through the list's make, where the edit's kind says.
	TEST_EXPECT(document.apply(edit_of(EditOperation::Add, {first.id, kNote, 0}, 0, 1), error));
	TEST_EXPECT(document.apply(edit_of(EditOperation::Add, {first.id, kBook, 0}, 0, SIZE_MAX), error));
	TEST_EXPECT(document.serialize().text == "shelf 0 B10 N0 N2 B30 B0\nshelf 1 B5\n");
	// A book's pages: read on a shelf that stands, ignored on one that leans (the shelf it lies in).
	const FieldSchema &pages = shelf_table().kind(kBook)->fields()[0];
	TEST_EXPECT(document.field_on(leaf(0, 0), pages).applies == Applicability::Reads &&
	            document.field_on(leaf(1, 0), pages).applies == Applicability::Ignored);
	// A Move whose erase the list refuses: refused, the record, its identities and the file as they were.
	const std::string before = document.serialize().text;
	const RecordIds ids = static_cast<const Shelf &>(*document.rows()[0]).ids;
	g_erase_refused = true;
	TEST_EXPECT(!document.apply(edit_of(EditOperation::Move, leaf(0, 0), 0, 3), error));
	g_erase_refused = false;
	const Shelf &kept = static_cast<const Shelf &>(*document.rows()[0]);
	TEST_EXPECT(document.serialize().text == before && kept.ids.lists[0].size() == ids.lists[0].size() &&
	            ids_match(shelf_table(), kept.record(), kept.ids));
	for (size_t i = 0; i < ids.lists[0].size(); ++i) TEST_EXPECT(kept.ids.lists[0][i].id == ids.lists[0][i].id);
	// Allowed, the Move moves the record with its identity, of its own kind.
	const NodeAddress moved = leaf(0, 0);
	TEST_EXPECT(document.apply(edit_of(EditOperation::Move, moved, 0, 3), error));
	const Shelf &after = static_cast<const Shelf &>(*document.rows()[0]);
	TEST_EXPECT(document.serialize().text == "shelf 0 N0 N2 B30 B10 B0\nshelf 1 B5\n" &&
	            after.ids.lists[0][3].id == moved.child && ids_match(shelf_table(), after.record(), after.ids));
	// A note moved the same way, named by its own kind.
	TEST_EXPECT(document.apply(edit_of(EditOperation::Move, leaf(0, 1), 0, 0), error));
	TEST_EXPECT(document.serialize().text == "shelf 0 N2 N0 B30 B10 B0\nshelf 1 B5\n");
	std::printf("core: a list of two kinds walked and added as each, a field its shelf decides, a Move its list "
	            "refuses leaving the record, its identities and the file as they were\n");
	return 0;
}

// --- the cost of a keystroke ------------------------------------------------------------------------

struct Keystroke {
	TableDocument::TableStats stats;
	def::DefAuthoredParses parses;
};

// One Set of `field` on the first item of items.def, as the Inspector sends it while a number is typed
// (`coalesce`: folded into the field's open edit group, applied to the record as the group's first Set
// found it).
Keystroke keystroke(DefCatalogDocument &document, const char *field, int64_t value, bool coalesce = true) {
	const TableDocument &tables = *tables_of(document);
	const NodeAddress item{document.rows()[0]->id, node_kind(def::DefRecordKind::Item), 0};
	Edit edit;
	edit.address = item;
	edit.field = field;
	edit.value = value;
	edit.coalesce = coalesce;
	Diagnostic error;
	tables.clear_table_stats();
	const def::DefAuthoredParses before = def::def_authored_parses();
	Keystroke out;
	if (!document.apply(edit, error)) return out;
	out.stats = tables.table_stats();
	const def::DefAuthoredParses after = def::def_authored_parses();
	out.parses.lines = after.lines - before.lines;
	out.parses.records = after.records - before.records;
	return out;
}

// A Set in a def catalog resolves the field's own row of the record's own kind, one probe of the kind's
// index each time the core asks, and no other field's row; the record's member was found when the table
// was made, never again. The core asks four times for a changed value (the value before, whether it is
// written, the Set, the value after), five for the value the field holds (whether it is written after
// too: Document's apply skipping that Set). A member the file writes in its own units (player_speed, in
// km/h on its line) costs the family's parser besides: its read writes its line alone and reads it back
// (the stored word kept), its set writes the record with the new line and reads it back twice (the line
// as the parser takes it, then the record as written again). That cost, timed below, is a fraction of a
// millisecond a keystroke: no memo of a member's line.
int keystroke_in_a_catalog() {
	DefCatalogDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(test_io::read_file(fixture("def/items.def")), "items.def", AssetKind::ItemDefs, "JO",
	                                error));
	TEST_EXPECT(tables_of(document) && !document.rows().empty());
	const NodeAddress item{document.rows()[0]->id, node_kind(def::DefRecordKind::Item), 0};
	const TableKind &kind = *catalog_table().kind(item.kind);
	const FieldSchema *hp = &kind.fields()[kind.find("hp")];
	const FieldSchema *speed = &kind.fields()[kind.find("player_speed")];
	Value held;
	TEST_EXPECT(document.get(item, "hp", held));
	// A plain member, a changed value: four resolutions of its own row, one probe and one kind each.
	const Keystroke changed = keystroke(document, "hp", std::get<int64_t>(held) + 1);
	TEST_EXPECT(changed.stats.distinct.size() == 1 && changed.stats.distinct[0] == hp && !changed.stats.several);
	TEST_EXPECT(changed.stats.resolved == 4 && changed.stats.probes == 4 && changed.stats.kinds == 4);
	TEST_EXPECT(changed.parses.lines == 0 && changed.parses.records == 0);
	// The value it holds (a Set of its own, outside the typing's group): five.
	document.end_edit_group();
	const Keystroke same = keystroke(document, "hp", std::get<int64_t>(held) + 1, false);
	TEST_EXPECT(same.stats.distinct.size() == 1 && same.stats.resolved == 5 && same.stats.probes == 5 &&
	            same.stats.kinds == 5);
	std::printf("keystroke: the value it holds %zu resolution(s), %zu probe(s), %zu kind(s)\n", same.stats.resolved,
	            same.stats.probes, same.stats.kinds);
	// A member in its own units: the same four resolutions, and the parses.
	TEST_EXPECT(document.get(item, "player_speed", held));
	const int64_t speed_before = std::get<int64_t>(held);
	const Keystroke authored = keystroke(document, "player_speed", speed_before + 10);
	TEST_EXPECT(authored.stats.distinct.size() == 1 && authored.stats.distinct[0] == speed &&
	            authored.stats.resolved == 4 && authored.stats.probes == 4 && authored.stats.kinds == 4);
	TEST_EXPECT(authored.parses.lines == 3 && authored.parses.records == 2);
	TEST_EXPECT(document.get(item, "player_speed", held) && std::get<int64_t>(held) == speed_before + 10);
	// Timed: a run of keystrokes, each a changed number.
	constexpr int kRuns = 200;
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < kRuns; ++i) keystroke(document, "player_speed", speed_before + 11 + i);
	const double each =
	        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRuns;
	std::printf("keystroke: a plain member %zu row resolution(s) (%zu for the value it holds), %zu probe(s); "
	            "player_speed %zu resolution(s), %zu line parse(s), %zu record parse(s), %.1f us a keystroke\n",
	            changed.stats.resolved, same.stats.resolved, changed.stats.probes, authored.stats.resolved,
	            authored.parses.lines, authored.parses.records, each);
	return 0;
}

// The game's powerup.def (a SKIP-LEG without OPENNOVA_JO_DIR) as the catalog opens it: unblocked, every
// record through the table and every field set to itself, its bytes the format's canonical writer's
// over the format's own parse (no byte of the file carried through, ADR 0003), opened again the same
// bytes.
int retail_powerups() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (the game's powerup.def as the catalog's rows)");
	opennova::Vfs vfs;
	vfs.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
	TEST_EXPECT(vfs.mount_game(root, "", opennova::VfsMountMode::Packed));
	std::vector<uint8_t> bytes;
	TEST_EXPECT(vfs.read_file("powerup.def", bytes) && !bytes.empty());
	Counts counts;
	if (catalog("powerup.def", AssetKind::PowerupDefs, bytes, counts) != 0) return 1;
	DefCatalogDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes, "powerup.def", AssetKind::PowerupDefs, "JO", error) && !document.blocked());
	const SerializeResult written = document.serialize();
	def::DefPowerupFile parsed{};
	def::def_parse_powerup_memory(bytes.data(), bytes.size(), &parsed, nullptr);
	const def::DefWriteResult canonical = def::def_write_powerup(parsed);
	const size_t rows = parsed.count;
	def::def_free_powerup(&parsed);
	TEST_EXPECT(written.ok() && canonical.ok() && written.text == canonical.text);
	DefCatalogDocument again;
	TEST_EXPECT(again.load_bytes(text_bytes(written.text), "powerup.def", AssetKind::PowerupDefs, "JO", error) &&
	            again.serialize().text == written.text && again.rows().size() == rows);
	std::printf("retail powerup.def: %zu rows, %zu records, %zu fields set to themselves; written as the format's "
	            "writer writes it, a fixed point\n",
	            rows, counts.records, counts.set);
	return 0;
}

#undef SHAPE_CHECK

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (menu() != 0) return 1;
	if (catalogs() != 0) return 1;
	if (model() != 0) return 1;
	if (mission() != 0) return 1;
	if (core_over_a_table() != 0) return 1;
	if (keystroke_in_a_catalog() != 0) return 1;
	if (retail_powerups() != 0) return 1;
	std::printf("editor_table_shape: all tests passed\n");
	return 0;
}
