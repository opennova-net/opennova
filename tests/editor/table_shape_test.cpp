// The one table shape (editor/model/table_shape.h, ADR 0046 S13 D10) as each table that is rows of it
// fills it: the menu's, the def catalog's (its items, its weapons and carry limits, its powerup rows),
// the model's and the mission's. Over each table's records, read from a file of the format: the table
// keeps its invariants (each kind at its place, each list naming a kind of the table, each field's id
// its own); every record a list holds is of the list's kind and every handle carries its row's top;
// every field reads a value of its type (the value), a number whose field names its values reads one
// of them or bits of them (the choice), a field whose record decides what it is answers on every record
// (the labelled field), and a writable field set to the value it reads leaves the file's bytes as they
// were; every list gives a copy of its first record, takes it back in at its end, gives it back out,
// and the file's bytes are as they were (the list ops), a new record where the list makes one. And the
// cost of a keystroke (D2's count-based rules): a Set in a def catalog resolves its own field's row of
// its own kind, one probe each, and no other field's. With the game install (a SKIP-LEG without
// OPENNOVA_JO_DIR), the game's powerup.def, the family the catalog gained as rows, through the same
// sweep, written as the format's canonical writer writes it and a fixed point.
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mnu_table.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/table_document.h>
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
};

void each_record(const RecordTable &table, const RecordHandle &record, const std::function<void(const RecordHandle &)> &fn) {
	fn(record);
	for (const TableList &list : table.kind(record.kind)->lists())
		for (size_t i = 0; i < list.ops.size(record); ++i) each_record(table, list.ops.at(record, i), fn);
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
		const void *top = root.top;
		each_record(table, root, [&](const RecordHandle &record) {
			++counts.records;
			const TableKind *kind = table.kind(record.kind);
			SHAPE_CHECK(kind && record.data && record.top == top);
			if (!kind) return;
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
					const Applicability applies = kind->applies(place)(record);
					SHAPE_CHECK(applies == Applicability::Reads || applies == Applicability::Ignored ||
					            applies == Applicability::Unverified);
					++counts.decided;
				}
				if (kind->reference(place)) {
					const ReferenceKind names = kind->reference(place)(record);
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
// of its first record in at its end and out again; a new record in and out where the list makes one.
// The bytes as they were after each. The edit happens inside the walk on the owner the walk is at,
// changing that owner's own list alone and giving it back as it was, so every handle the walk makes
// after it is made again from the records as they stand.
int lists(const Subject &subject, Counts &counts) {
	const RecordTable &table = *subject.table;
	const std::vector<uint8_t> before = subject.write();
	std::set<std::pair<NodeKind, size_t>> done;
	int failures = 0;
	for (const RecordHandle &root : subject.roots()) {
		each_record(table, root, [&](const RecordHandle &owner) {
			const std::vector<TableList> &held = table.kind(owner.kind)->lists();
			for (size_t l = 0; l < held.size(); ++l) {
				const TableList &list = held[l];
				const ListOps &ops = list.ops;
				SHAPE_CHECK(ops.size && ops.at && ops.insert && ops.erase && ops.copy);
				const size_t size = ops.size(owner);
				for (size_t i = 0; i < size; ++i) {
					const RecordHandle record = ops.at(owner, i);
					if (!record || record.kind != list.spec.kind) {
						std::fprintf(stderr, "%s: %s's list %s holds a record of kind %d at %zu, not its own %d\n",
						             subject.name.c_str(), table.kind(owner.kind)->row().token, list.spec.label,
						             int(record.kind), i, int(list.spec.kind));
						++failures;
					}
				}
				if (list.spec.fixed || size == 0 || !done.insert({owner.kind, l}).second) continue;
				++counts.lists;
				std::string error;
				const DetachedRecord copy = ops.copy(owner, 0);
				SHAPE_CHECK(copy.data && copy.kind == list.spec.kind);
				if (ops.insert(owner, size, &copy, error)) {
					SHAPE_CHECK(ops.size(owner) == size + 1 && ops.at(owner, size).kind == list.spec.kind);
					SHAPE_CHECK(ops.erase(owner, size) && ops.size(owner) == size);
					++counts.copies;
				} else {
					// Only a list that holds no more (a part, a powerup's one action block of a name).
					SHAPE_CHECK(list.spec.max && size >= list.spec.max);
				}
				error.clear();
				if (ops.insert(owner, size, nullptr, error)) {
					SHAPE_CHECK(ops.size(owner) == size + 1 && ops.erase(owner, size) && ops.size(owner) == size);
					++counts.made;
				} else {
					SHAPE_CHECK(!error.empty());
					++counts.refused;
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

#undef SHAPE_CHECK

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

int catalogs() {
	Counts items, weapons, powerups;
	if (catalog("items.def", AssetKind::ItemDefs, test_io::read_file(fixture("def/items.def")), items) != 0) return 1;
	if (catalog("weapon.def", AssetKind::WeaponDefs,
	            text_bytes("ammoclass_max_carry bullets 300\nweapon \"WPN_TEST\"\ncategory 1\nround_type AT_TEST\n"
	                       "sights scope 0 0 640 480 blend\naction \"Fire\"\nfunction Shoot 1 2\ndelay auto\nend\nend\n"),
	            weapons) != 0)
		return 1;
	if (catalog("powerup.def", AssetKind::PowerupDefs, text_bytes(kPowerups), powerups) != 0) return 1;
	TEST_EXPECT(items.records > 10 && items.set > 1000 && weapons.lists >= 2 && powerups.records == 6 &&
	            powerups.lists == 3);
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

// The mission: the synthetic dense mission (four pools, paths, zones, events with their chains).
int mission() {
	const std::vector<uint8_t> bytes = test_io::read_file(fixture("bms/synth_dense.bms"));
	bms::File file;
	std::string message;
	TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
	Subject subject{"mission", &mission_table(), nullptr, nullptr};
	subject.roots = [&] { return std::vector<RecordHandle>{mission_record(file)}; };
	subject.write = [&] {
		std::vector<uint8_t> out;
		std::string error;
		bms::write(file, out, error);
		return out;
	};
	Counts counts;
	if (sweep(subject, counts) != 0) return 1;
	TEST_EXPECT(counts.records > 200 && counts.lists >= 8 && counts.choices > 0);
	return 0;
}

// --- the cost of a keystroke ------------------------------------------------------------------------

// One Set in a def catalog, as the Inspector sends it while a number is typed: the table resolves the
// field's own row of the record's own kind, one probe of the kind's index each time it is asked,
// and no other field's; the record's member was found when the table was made, never again.
int keystroke_in_a_catalog() {
	DefCatalogDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(test_io::read_file(fixture("def/items.def")), "items.def", AssetKind::ItemDefs, "JO",
	                                error));
	const TableDocument *tables = tables_of(document);
	TEST_EXPECT(tables && !document.rows().empty());
	const NodeAddress item{document.rows()[0]->id, node_kind(def::DefRecordKind::Item), 0};
	const TableKind &kind = *catalog_table().kind(item.kind);
	const FieldSchema *hp = &kind.fields()[kind.find("hp")];
	Value before;
	TEST_EXPECT(document.get(item, "hp", before));
	Edit edit;
	edit.address = item;
	edit.field = "hp";
	edit.value = std::get<int64_t>(before) + 1;
	edit.coalesce = true;
	tables->clear_table_stats();
	TEST_EXPECT(document.apply(edit, error));
	const TableDocument::TableStats &stats = tables->table_stats();
	std::printf("keystroke: %zu field row(s) resolved, %zu distinct, %zu kind(s)\n", stats.fields, stats.distinct.size(),
	            stats.kinds);
	// The core asks the field four times (the value before, whether it is written, the Set, the value
	// after: Document's apply skipping a Set of the value held), each one probe of the kind's index for
	// the field's own row; no other field's row is touched.
	TEST_EXPECT(stats.distinct.size() == 1 && stats.distinct[0] == hp && !stats.several);
	TEST_EXPECT(stats.fields == 4 && stats.kinds == 4);
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

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (menu() != 0) return 1;
	if (catalogs() != 0) return 1;
	if (model() != 0) return 1;
	if (mission() != 0) return 1;
	if (keystroke_in_a_catalog() != 0) return 1;
	if (retail_powerups() != 0) return 1;
	std::printf("editor_table_shape: all tests passed\n");
	return 0;
}
