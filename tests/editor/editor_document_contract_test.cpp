// S12 Z (ADR 0046 S12, "Adding a document type"): the contract every registered document type
// keeps, checked over a file of its kinds. The types are the registry's own (every asset kind
// document_type_for answers, each type once), so a new type fails here until it has a file below.
// Over each file: the document loads unblocked; parse, serialize, parse again serializes the same
// bytes; every field of every record reads, carries a label (a def member only its line does not
// write as a number of its own may go by its id), a range that is one (a number's, low to high),
// the same id and type as the record's field_on gives it, and field_on never makes a read-only
// field writable; a writable field set to the value it reads leaves the serialized bytes as they
// were; every field a record defines a name by yields a symbol whose locator names that record,
// and find_definition resolves its name in a scope only to a definition of its kind and name
// there (nothing in a scope with none), the same over the document alone and over a graph whose
// slot is current for it (S13 D3); every reference and definition kind has its
// reference_kinds row. S13 D2 adds what needs no multi-row edit: every record's locator finds it
// again, and two loads of the file give every record the same identity; an optional field the
// game reads on its record left out and written again (Clear, Write), a Clear of one left out
// and a Write of one written no step, the undo giving the bytes back (a type refuses one only as
// always written; how many each type is asked and does is pinned); a real change of a value,
// the bytes before, after, after its undo and after its redo; the record and the field changed
// since the save (no other row's record), and the edits that give the field back (revert_edits)
// giving it its saved value, and the record and the bytes as the file held them, which every
// file reaches (a def member whose Set changes another of its record's, def_sync_derived, an open
// item, is checked but for its bytes, and the clause goes on to another field); two coalesced
// Sets of a field one undo step; a record copied and pasted where the type copies records, the
// undo giving the bytes back; and a snapshot of the changed document, which serializes its
// bytes, shares its identity, revision and records, answers record_change and field_changed as
// it does, refuses an edit, a save and a load (document.snapshot), and after its document's
// undo still finds every record where it was and answers what changed in it as it did. S13 D5
// adds the type's record kinds (kinds()): each named back by its token, no two sharing a kind or
// a token, a kind the outline adds a row of being a row of the file; every row of the file of a
// kind that is a row, and every record a collection holds of a kind the table has. S13 D4 adds the
// type's validate_file: the file's own findings from its document alone, each on the file and on
// a record the document holds, the same findings from a second load of the file, and a finding
// over each type's files (a flawed file of its own where its fixture has no flaw).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>
#include <formats/def/def_schema.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_paths.h"

using namespace opennova::editor;

namespace {

int g_failures = 0;
// What was checked, for the summary line: records, fields set to their own value, symbols, and
// lookups of a name in another scope that defines it too; optional fields left out and written
// again and those a type keeps written, files with a real change and its undo, files whose field
// took two coalesced Sets, records pasted, snapshots, and the findings validate_file made.
size_t g_records = 0, g_sets = 0, g_symbols = 0, g_other_scopes = 0;
size_t g_presences = 0, g_kept = 0, g_changes = 0, g_coalesced = 0, g_pastes = 0, g_snapshots = 0;
size_t g_findings = 0;
std::set<std::string> g_kinds; // each type's record kinds, by the type and the token

// A type's optional fields over its files: those asked to be left out or written again, and those
// it did.
struct TypeCounts {
	size_t optional = 0, presences = 0;
	size_t findings = 0; // what validate_file made over the type's files
};

// What each type's files ask of the presence clause and what the type does, as ADR 0046 S13 D2
// states them: every one left out and written again. A type not named has none. A change of a
// file above, or of a type's optional fields, moves them here and in the ADR together.
struct PinnedPresence {
	const char *type;
	size_t optional, presences;
};
const PinnedPresence kPinnedPresence[] = {{"menu", 301, 301}, {"catalog", 27, 27}};

// One clause of the contract, named with where it failed (the file, the record, the field).
void check(bool ok, const std::string &where, const char *clause) {
	if (ok) return;
	std::fprintf(stderr, "  %s: %s\n", where.c_str(), clause);
	++g_failures;
}

struct Fixture {
	AssetKind kind;
	std::string name;
	std::vector<uint8_t> bytes;
};

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A menu screen whose root window holds an EXIT button: two of them give one window name in two
// scopes (a lookup on screen B must reach B's EXIT, never A's).
std::string exit_screen(const char *name, const char *action) {
	return std::string("<SCREEN>\n  <NAME>") + name +
	       "</NAME>\n  <WINDOW type=\"window\" name=\"ROOT\">\n"
	       "    <POSITION>\n      <LEFT>0</LEFT>\n      <TOP>0</TOP>\n"
	       "      <RIGHT>640</RIGHT>\n      <BOTTOM>480</BOTTOM>\n    </POSITION>\n"
	       "    <WINDOW type=\"button\" name=\"EXIT\">\n"
	       "      <POSITION>\n        <LEFT>20</LEFT>\n        <TOP>440</TOP>\n"
	       "        <RIGHT>120</RIGHT>\n        <BOTTOM>470</BOTTOM>\n      </POSITION>\n"
	       "      <STRING>Exit</STRING>\n      " +
	       action + "\n    </WINDOW>\n  </WINDOW>\n</SCREEN>\n";
}

// A file of every registered type's kinds: the repo's fixtures, a weapon and an ammo table
// written here (the def fixture is an item table) with a record, a nested record and the
// written-unit, choice and reference fields the catalog shows, and a menu of two screens that
// each name a window EXIT.
std::vector<Fixture> fixtures(const std::string &repo) {
	const auto file = [&](const char *relative) { return test_io::read_file(repo + "/fixtures/" + relative); };
	return {
	        {AssetKind::ItemDefs, "items.def", file("def/items.def")},
	        {AssetKind::WeaponDefs, "weapon.def",
	         text_bytes("weapon \"WPN_CONTRACT\"\ncategory 11\nrank 3\nclipsize 30\nround_type AT_CONTRACT\n"
	                    "weaponweight 1.25\nerror_hiptheta 0.5\ncharfilter medic\nswitchcategory 3\nheat_sound SND_HEAT\n"
	                    "action \"FIRE\"\ndelayend 2\nend\nend\n")},
	        {AssetKind::AmmoDefs, "ammo.def",
	         text_bytes("ammo AT_CONTRACT\nmax_age 1.5\nvelocity 900\nturnrate_maxyaw 45\nlight_move 3 255 120 20\nend\n")},
	        {AssetKind::Strings, "synth_game.bin", file("rtxt/synth_game.bin")},
	        {AssetKind::Menu, "all_widgets.mnu", file("mnu/all_widgets.mnu")},
	        {AssetKind::Menu, "two_screens.mnu",
	         text_bytes(exit_screen("A", "<ACTION type=\"POP_SCREEN\"></ACTION>") + "\n" +
	                    exit_screen("B", "<ACTION type=\"SCREEN\" file=\"two_screens.mnu\">A</ACTION>"))},
	        {AssetKind::MenuStyle, "test_style.mns", file("mns/test_style.mns")},
	        {AssetKind::Model, "armory.3di", file("threedi/synth/armory.3di")},
	        {AssetKind::Animation, "walk.bad", file("anim/walk.bad")},
	        {AssetKind::AnimationMap, "soldier.adm", file("anim/soldier.adm")},
	};
}

// walk.bad's frames at 25 frames per second (every retail clip plays at 30): the clip's own
// finding (animation.fps). Empty when the clip does not read or write.
std::vector<uint8_t> clip_at_25fps(const std::vector<uint8_t> &walk) {
	opennova::bad::BadFile clip{};
	std::vector<uint8_t> out;
	if (walk.empty() || opennova::bad::bad_parse_buffer(walk.data(), walk.size(), &clip) != 0) return out;
	clip.fps = 25;
	if (opennova::bad::bad_write_buffer(&clip, out) != 0) out.clear();
	opennova::bad::bad_free(&clip);
	return out;
}

// A string table whose one section holds a key twice (strings.key_duplicate). Empty when the
// table does not write.
std::vector<uint8_t> table_with_a_key_twice() {
	opennova::rtxt::File table;
	table.sections.push_back({"menu", 2});
	table.entries.push_back({"KEY", "one", {}, 0});
	table.entries.push_back({"KEY", "two", {}, 0});
	std::vector<uint8_t> out;
	std::string error;
	if (!opennova::rtxt::write(table, out, error)) out.clear();
	return out;
}

// A file of each type whose fixture above makes no finding, holding a flaw the type's
// validate_file reports (a key twice in a section, two screens of one NAME, a CTRL register the
// engine does not know, a clip at 25 frames per second, a slot named twice): what the per-type
// findings clause reads with the fixtures, through check_validate_file alone.
std::vector<Fixture> flawed_files(const std::string &repo) {
	const auto file = [&](const char *relative) { return test_io::read_file(repo + "/fixtures/" + relative); };
	const std::string pop = "<ACTION type=\"POP_SCREEN\"></ACTION>";
	return {
	        {AssetKind::Strings, "key_twice.bin", table_with_a_key_twice()},
	        {AssetKind::Menu, "twin_screens.mnu", text_bytes(exit_screen("A", pop.c_str()) + "\n" + exit_screen("A", pop.c_str()))},
	        {AssetKind::Model, "mount_ctrl1_not_retail.3di", file("threedi/synth/mount_ctrl1_not_retail.3di")},
	        {AssetKind::Animation, "walk_25fps.bad", clip_at_25fps(file("anim/walk.bad"))},
	        {AssetKind::AnimationMap, "slot_twice.adm",
	         text_bytes("anim_reset\t\"idle.bad\"\r\nanim_idle\t\"idle.bad\"\r\nanim_idle\t\"walk.bad\"\r\n")},
	};
}

// Every registered document type, once each, in the order the asset kinds are declared.
std::vector<const DocumentType *> registered_types() {
	std::vector<const DocumentType *> out;
	for (size_t i = 1; i < kAssetKindCount; ++i) {
		const DocumentType *type = document_type_for(static_cast<AssetKind>(i));
		if (type && std::find(out.begin(), out.end(), type) == out.end()) out.push_back(type);
	}
	return out;
}

// Every record: each row, then what it holds in pre-order.
std::vector<NodeAddress> records_of(const Document &document) {
	std::vector<NodeAddress> out;
	for (const auto &row : document.rows()) {
		out.push_back({row->id, row->kind, 0});
		document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
			out.push_back(nested);
			return true;
		});
	}
	return out;
}

std::string where_of(const Fixture &fixture, const Document &document, const NodeAddress &address,
                     const std::string &field) {
	std::string out = fixture.name + " " + document.record_path(address);
	if (!field.empty()) out += " ." + field;
	return out;
}

// The reference_kinds row of a kind: its own, with a token that names it back.
bool has_row(ReferenceKind kind) {
	const ReferenceKindRow &row = reference_row(kind);
	ReferenceKind named = ReferenceKind::None;
	return row.kind == kind && *row.token && *row.label && reference_kind_from_token(row.token, named) && named == kind;
}

// The name a defining field's value gives (a text, or a number other than 0), as the graph's
// extraction reads it.
bool defines_a_name(const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) return !text->empty();
	if (const auto *number = std::get_if<int64_t>(&value)) return *number != 0;
	return false;
}

// Whether a field carries the label the editor names it by. The one field that may go without:
// a def member its line does not write as a number of its own, which keeps the def table's id
// (the native member's name, the tooltip saying the key its file writes where the two differ);
// a member written as a number of its own is labelled by its line (def_catalog_document's
// describe: the table's name for it, else the key). field_title's fallback to the id is not a
// label.
bool labelled(const Document &document, const NodeAddress &address, const FieldSchema &field) {
	if (!field.label.empty()) return true;
	return dynamic_cast<const DefCatalogDocument *>(&document) &&
	       opennova::def::def_authored(def_kind(address.kind), field.id) == opennova::def::DefAuthored::None;
}

// A record's field schema checks, as the record's field_on gives it.
void check_schema(const Fixture &fixture, const Document &document, const NodeAddress &address,
                  const FieldSchema &schema) {
	const std::string where = where_of(fixture, document, address, schema.id);
	const FieldUse field = document.field_on(address, schema);
	check(labelled(document, address, schema), where, "the field carries the label the editor names it by");
	Value value;
	check(document.get(address, schema.id, value), where, "the field reads");
	check(field.schema == &schema, where, "field_on points at the field's own schema (its id and type)");
	check(!schema.read_only || field.read_only, where, "field_on never makes a read-only field writable");
	if (schema.ranged)
		check(schema.type != FieldType::Text && schema.min <= schema.max && schema.step >= 0.0, where,
		      "a ranged field is a number whose range runs low to high");
	if (field.reference != ReferenceKind::None)
		check(has_row(field.reference), where, "the kind it references has its reference_kinds row");
	if (field.defines != ReferenceKind::None)
		check(has_row(field.defines), where, "the kind it defines has its reference_kinds row");
}

// Each writable field set to the value it reads: the serialized bytes stay as they were (the
// step, if the document takes one, undone after). A read-only field takes no set; one that
// does not read is check_schema's failure.
void check_set_to_self(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                       const std::string &serialized) {
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			Value value;
			if (field.read_only || !document.get(address, schema.id, value)) continue;
			const std::string where = where_of(fixture, document, address, schema.id);
			Edit edit;
			edit.operation = EditOperation::Set;
			edit.address = address;
			edit.field = schema.id;
			edit.value = value;
			Diagnostic error;
			const bool applied = document.apply(edit, error);
			check(applied, where + " (" + error.message + ")", "a writable field takes the value it reads");
			if (!applied) continue;
			++g_sets;
			const SerializeResult now = document.serialize();
			check(now.ok() && now.text == serialized, where, "a field set to the value it reads changes no byte");
			while (document.can_undo()) document.undo();
		}
}

// A scope no definition names: a section of the symbol's own file, or a file, of that name.
const char *const kNoScope = "CONTRACT_NO_SUCH_SCOPE";

// Every field a record defines a name by reads and yields the record's symbol: its locator names
// the record. find resolves the name in a scope only to a definition of the symbol's kind and
// name that a lookup reaches there: in its own scope this record, or the one of its name there a
// lookup finds first; in another scope of that kind's definitions the one there, or nothing where
// none is; in a scope no definition names nothing. A definition no lookup finds is still found,
// by its name alone, to a definition of its kind and name. Each find answers alike over the
// document alone and over a graph whose slot is current for it (the document's snapshot open in a
// graph of its one file), which it then reads the definitions from.
void check_symbols(const Fixture &fixture, const Document &document, const std::vector<NodeAddress> &records) {
	Extracted extracted;
	extract_from_document(document, extracted);
	AssetGraph slotted;
	AssetScan scan;
	AssetEntry entry;
	entry.logical_name = fixture.name;
	entry.relative_path = document.path();
	entry.kind = document.kind();
	scan.entries.push_back(entry);
	scan.index();
	slotted.update(ProjectPaths::for_root("."), ProjectDocument(), scan,
			{ std::shared_ptr<const Document>(document.snapshot()) });
	check(slotted.for_each_definition(document, [](const GraphSymbol &, bool) {}), fixture.name,
	      "a graph holding the document's snapshot has a slot current for the document");
	const auto find = [&](const std::string &where, const std::string &name, NodeAddress &found,
	                      const std::string &scope) {
		NodeAddress by_slot;
		const bool alone = find_definition(AssetGraph(), document, name, found, scope);
		const bool read = find_definition(slotted, document, name, by_slot, scope);
		check(read == alone && (!alone || by_slot == found), where,
				"find answers alike over the graph's slot");
		return alone;
	};
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			if (field.defines == ReferenceKind::None || field.applies == Applicability::Ignored ||
			    !document.present(address, schema.id))
				continue;
			const std::string where = where_of(fixture, document, address, schema.id);
			Value value;
			const bool read = document.get(address, schema.id, value);
			check(read, where, "a field that defines a name reads");
			if (!read || !defines_a_name(value)) continue;
			const auto symbol = std::find_if(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &s) {
				return s.address == address && s.field == schema.id;
			});
			check(symbol != extracted.symbols.end() && symbol->kind == field.defines, where,
			      "a field that defines a name yields its symbol");
		}
	const auto defined_as = [&](const GraphSymbol &symbol, const NodeAddress &found, bool reached, const std::string &scope) {
		return std::any_of(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &other) {
			return other.address == found && other.kind == symbol.kind && other.name == symbol.name &&
			       (!reached || (!other.inert && scope_matches(other.scope, scope)));
		});
	};
	for (const GraphSymbol &symbol : extracted.symbols) {
		++g_symbols;
		const std::string where = where_of(fixture, document, symbol.address, symbol.field) + " '" + symbol.display + "'";
		check(symbol.file == document.path() && symbol.locator == document.locator(symbol.address) &&
		              document.address_at(symbol.locator) == symbol.address,
		      where, "a symbol's locator names its defining record");
		NodeAddress found;
		if (symbol.inert) {
			check(find(where, symbol.display, found, std::string()) &&
							defined_as(symbol, found, false, std::string()),
					where, "find resolves a name no lookup reaches by the name alone");
			continue;
		}
		// Its own scope, every other scope a definition of its kind has here, and one none has.
		std::vector<std::string> scopes{symbol.scope};
		for (const GraphSymbol &other : extracted.symbols)
			if (other.kind == symbol.kind && std::find(scopes.begin(), scopes.end(), other.scope) == scopes.end())
				scopes.push_back(other.scope);
		const size_t slash = symbol.scope.find('/');
		scopes.push_back((slash == std::string::npos ? std::string() : symbol.scope.substr(0, slash + 1)) + kNoScope);
		for (const std::string &scope : scopes) {
			const bool there = std::any_of(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &other) {
				return !other.inert && other.kind == symbol.kind && other.name == symbol.name &&
				       scope_matches(other.scope, scope);
			});
			const std::string in = where + " in '" + scope + "'";
			const bool resolved = find(in, symbol.display, found, scope);
			if (!there) {
				check(!resolved, in, "find resolves a name in a scope that does not define it to nothing");
				continue;
			}
			if (scope == symbol.scope) {
				check(resolved && defined_as(symbol, found, true, scope), in,
				      "find resolves the symbol in its scope to its record, or the one of its name there first");
				continue;
			}
			++g_other_scopes;
			check(resolved && defined_as(symbol, found, true, scope), in,
			      "find resolves a name in another scope to the definition of its kind and name there");
		}
	}
}

Edit edit_of(EditOperation operation, const NodeAddress &address, const std::string &field, Value value = int64_t(0)) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// Every record's locator finds it again, and a second load of the same bytes gives every record
// the identity the first gave it (a rename reloads a document and finds its records so).
// The type's record kinds (kinds()): each named back by its token and holding a label, no two
// sharing a kind or a token, a kind the outline adds a row of being a row of the file; every row
// of the file of a kind that is a row, and every record a collection holds of a kind the table
// has (its token the one its locator and a batch's add name it by).
void check_kinds(const Fixture &fixture, const Document &document,
                 const std::vector<NodeAddress> &records) {
	const std::vector<RecordKindRow> &kinds = document.kinds();
	check(!kinds.empty(), fixture.name, "the type declares its record kinds");
	for (size_t i = 0; i < kinds.size(); ++i) {
		const RecordKindRow &row = kinds[i];
		const std::string where = fixture.name + " kind " + row.token;
		check(*row.token && *row.label && document.kind_from_name(row.token) == row.kind &&
		              document.kind_row(row.kind) == &row,
		      where, "a kind has a token and a label, and its token names it back");
		check(!*row.add_label || row.top, where, "a kind the outline adds is a row of the file");
		for (size_t j = 0; j < i; ++j)
			check(kinds[j].kind != row.kind && std::string(kinds[j].token) != row.token, where,
			      "no two kinds share a kind or a token");
		g_kinds.insert(std::string(typeid(document).name()) + "/" + row.token);
	}
	for (const NodeAddress &address : records) {
		const std::string where = where_of(fixture, document, address, "");
		const RecordKindRow *kind = document.kind_row(address.kind);
		check(kind && kind->top == (address.child == 0), where,
		      "a row of the file is of a kind that is a row, a nested record of one the table has");
		for (const Document::Collection &collection : document.collections_of(address))
			check(document.kind_row(collection.spec.kind) != nullptr, where,
			      "every kind a collection holds has its row");
	}
}

void check_places(const DocumentType &type, const Fixture &fixture, const Document &document,
                  const std::vector<NodeAddress> &records) {
	for (const NodeAddress &address : records)
		check(document.address_at(document.locator(address)) == address, where_of(fixture, document, address, ""),
		      "a record's locator finds it again");
	std::unique_ptr<Document> twin = type.make();
	Diagnostic error;
	const bool loaded = twin->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
	check(loaded && records_of(*twin) == records, fixture.name, "two loads of the file give the same identities");
}

// The one refusal a type gives a Clear or a Write of an optional field it keeps as its record
// writes it (a def line no tick of its own marks, a menu field no bit marks): always written.
bool always_written(const Diagnostic &error) {
	return error.code == "document.value" &&
	       (error.message == "This field is always written." || error.message == "This line is always written.");
}

// Each writable optional field the game reads on its record left out and written again (Clear,
// Write), its latent value kept; a Clear of a field left out and a Write of one written no step;
// the undo giving the bytes back. A type may keep such a field written (always_written); any
// other refusal fails, and counts says how many the type was asked and how many it did.
void check_presence(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                    const std::string &serialized, TypeCounts &counts) {
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			if (!schema.optional || schema.read_only) continue;
			Value latent;
			if (document.field_on(address, schema).applies == Applicability::Ignored || !document.get(address, schema.id, latent))
				continue;
			const std::string where = where_of(fixture, document, address, schema.id);
			const bool written = document.present(address, schema.id);
			const uint64_t revision = document.revision();
			Diagnostic error;
			const bool nothing = document.apply(edit_of(written ? EditOperation::Write : EditOperation::Clear, address, schema.id), error);
			check(nothing && document.revision() == revision, where, "a Write of a written field, a Clear of one left out: no step");
			++counts.optional;
			error = Diagnostic();
			if (!document.apply(edit_of(written ? EditOperation::Clear : EditOperation::Write, address, schema.id), error)) {
				check(always_written(error), where + " (" + error.code + ": " + error.message + ")",
				      "a type keeps an optional field as it is only as always written");
				++g_kept;
				continue;
			}
			++counts.presences;
			++g_presences;
			Value kept;
			check(document.present(address, schema.id) != written && document.get(address, schema.id, kept) && kept == latent,
			      where, "Clear leaves a field out and Write writes it again, its value kept");
			check(document.serialize().ok(), where, "a field left out or written again serializes");
			while (document.can_undo()) document.undo();
			check(document.present(address, schema.id) == written && document.serialize().text == serialized, where,
			      "the undo of a Clear or a Write gives the bytes back");
		}
}

// Values a field could take instead of `value`, the first ones its choices and range allow:
// another choice (a flags field's value with another bit), a number one or two either side, a
// text with a letter more or one less.
std::vector<Value> alternatives(const FieldSchema &field, const std::vector<FieldChoice> &choices, const Value &value) {
	std::vector<Value> out;
	const auto add = [&](Value candidate) {
		if (!(candidate == value) && std::find(out.begin(), out.end(), candidate) == out.end()) out.push_back(std::move(candidate));
	};
	if (const auto *number = std::get_if<int64_t>(&value)) {
		for (const FieldChoice &choice : choices) add(field.flags ? int64_t(*number ^ choice.value) : choice.value);
		double lo = field.ranged ? field.min : -1.0e15, hi = field.ranged ? field.max : 1.0e15;
		if (field.type != FieldType::Integer) lo = std::max(lo, 0.0);
		if (field.type == FieldType::Byte) hi = std::min(hi, 255.0);
		for (const int64_t step : {1, 2, -1, -2})
			if (double(*number + step) >= lo && double(*number + step) <= hi) add(int64_t(*number + step));
	} else if (const auto *real = std::get_if<double>(&value)) {
		for (const double step : {1.0, 2.0, -1.0, -2.0})
			if (!field.ranged || (*real + step >= field.min && *real + step <= field.max)) add(*real + step);
	} else if (const auto *text = std::get_if<std::string>(&value)) {
		for (const FieldChoice &choice : choices)
			if (!opennova::strutil::iequals(choice.name, *text)) add(choice.name);
		if (!field.width || text->size() + 3 <= field.width) {
			add(*text + "Q");
			add(*text + "QZ");
		}
		if (!text->empty()) add(text->substr(0, text->size() - 1));
	}
	return out;
}

// The fields a real change may be tried on, as they apply: writable, read by the game there,
// written, reading a value; with the values they could take instead.
template <class Try> void each_alternative(Document &document, const std::vector<NodeAddress> &records, Try try_one) {
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			Value value;
			if (field.read_only || field.applies == Applicability::Ignored ||
			    (schema.optional && !document.present(address, schema.id)) || !document.get(address, schema.id, value))
				continue;
			std::vector<FieldChoice> own;
			if (try_one(address, schema, alternatives(schema, document.choices_on(address, field, own), value))) return;
		}
}

// The def members whose Set changes another member of their record for good (def_sync_derived,
// def_schema.cpp): an item's powerup_def and default_aip set a bit of its attrib, an action's
// function_args_count clears the arguments past it, a weapon's filter counts the names past
// them. revert_edits gives the member back alone, so the other one stays as the Set made it: an
// open item each (ADR 0046 S13 D2), and the one reason a revert here is not held to the file's
// bytes. (An item's armor_kz also sets armor_blast, a mirror its armor line writes equal, so its
// revert gives the bytes back.)
bool derives_others(const Document &document, const NodeAddress &address, const std::string &field) {
	using opennova::def::DefRecordKind;
	static const std::pair<DefRecordKind, const char *> kCoupled[] = {
	        {DefRecordKind::Item, "powerup_def"},        {DefRecordKind::Item, "default_aip"},
	        {DefRecordKind::Action, "function_args_count"}, {DefRecordKind::Weapon, "charfilter_count"},
	        {DefRecordKind::Weapon, "teamfilter_count"}};
	if (!dynamic_cast<const DefCatalogDocument *>(&document)) return false;
	return std::any_of(std::begin(kCoupled), std::end(kCoupled), [&](const std::pair<DefRecordKind, const char *> &member) {
		return member.first == def_kind(address.kind) && field == member.second;
	});
}

// A real change of one value: the bytes before, after, after its undo and after its redo; the
// record and the field changed since the save and no record of another row; revert_edits giving
// the field its saved value back as one step, and the record and the bytes as the file held them.
// A member that changes another of its record's (derives_others) is checked but for those bytes,
// and the clause goes on to the next field, so every file reaches the bytes of a revert.
void check_real_change(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                       const std::string &serialized) {
	bool done = false, given_back = false;
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		const bool coupled = derives_others(document, address, schema.id);
		for (const Value &option : options) {
			Diagnostic error;
			if (!document.apply(edit_of(EditOperation::Set, address, schema.id, option), error)) continue;
			const SerializeResult after = document.serialize();
			if (!after.ok() || after.text == serialized) {
				while (document.can_undo()) document.undo();
				continue;
			}
			const std::string where = where_of(fixture, document, address, schema.id);
			done = true;
			++g_changes;
			check(document.dirty() && document.can_undo(), where, "a real change is a step");
			check(document.field_changed(address, schema.id) && document.record_change(address) == Document::RecordChange::Changed,
			      where, "the field and its record changed since the save");
			for (const NodeAddress &other : records)
				if (other.row != address.row)
					check(document.record_change(other) == Document::RecordChange::Unchanged, where_of(fixture, document, other, ""),
					      "a record of another row is unchanged");
			const std::vector<Edit> back = document.revert_edits(address, schema.id);
			Value saved, reverted;
			check(!back.empty() && document.apply(back, error) && !document.field_changed(address, schema.id) &&
			              document.saved_value(address, schema.id, saved) && document.get(address, schema.id, reverted) &&
			              reverted == saved,
			      where, "revert_edits gives the field its saved value back");
			if (!coupled) {
				given_back = true;
				check(document.record_change(address) == Document::RecordChange::Unchanged &&
				              document.serialize().text == serialized,
				      where, "a field given back leaves its record and the bytes as the file held them");
			}
			document.undo();
			check(document.serialize().text == after.text, where, "the revert is one step");
			document.undo();
			check(document.serialize().text == serialized && !document.dirty(), where, "the undo of a change gives the bytes back");
			document.redo();
			check(document.serialize().text == after.text, where, "its redo gives the change back");
			while (document.can_undo()) document.undo();
			return !coupled;
		}
		return false;
	});
	check(done, fixture.name, "a writable field takes another value");
	check(given_back, fixture.name, "a field given back is held to the bytes the file held");
}

// Two coalesced Sets of one field (typing): one undo step.
void check_coalescing(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                      const std::string &serialized) {
	bool done = false;
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		std::vector<Value> taken;
		for (const Value &option : options) {
			Edit set = edit_of(EditOperation::Set, address, schema.id, option);
			set.coalesce = true;
			Diagnostic error;
			if (!document.apply(set, error)) continue;
			taken.push_back(option);
			if (taken.size() == 2) break;
		}
		const bool two = taken.size() == 2 && document.serialize().text != serialized;
		if (two) {
			document.undo();
			check(document.serialize().text == serialized && !document.can_undo(),
			      where_of(fixture, document, address, schema.id), "two coalesced Sets of a field are one step");
			done = true;
			++g_coalesced;
		}
		while (document.can_undo()) document.undo();
		return two;
	});
	check(done, fixture.name, "a field takes two coalesced Sets");
}

// A record copied (where the type copies records) and pasted after its own: it is there, of its
// kind, the text reads back, and the undo gives the bytes back.
void check_copy_paste(const DocumentType &type, const Fixture &fixture, Document &document,
                      const std::vector<NodeAddress> &records, const std::string &serialized) {
	for (const NodeAddress &address : records) {
		if (!address.child) continue;
		const std::string payload = document.copy({address});
		Document::Placement at;
		if (payload.empty() || !document.placement(address, at)) continue;
		const std::string where = where_of(fixture, document, address, "");
		Edit paste = edit_of(EditOperation::Paste, {address.row, address.kind, 0}, std::string(), payload);
		paste.parent = at.owner.child;
		Diagnostic error;
		const bool pasted = document.apply(paste, error);
		check(pasted, where + " (" + error.message + ")", "a record copied pastes into its owner");
		if (!pasted) return;
		++g_pastes;
		const std::vector<NodeId> made = document.last_added_records();
		check(!made.empty() && document.address_of(made.front()).kind == address.kind &&
		              records_of(document).size() > records.size(),
		      where, "the pasted record is there, of its kind");
		const SerializeResult text = document.serialize();
		std::unique_ptr<Document> read = type.make();
		check(text.ok() && read->load_bytes(text_bytes(text.text), fixture.name, fixture.kind, "jo", error) && !read->blocked(),
		      where, "what a paste makes serializes and reads back");
		while (document.can_undo()) document.undo();
		check(document.serialize().text == serialized && !document.address_of(made.front()).row, where,
		      "the undo of a paste gives the bytes back");
		return;
	}
}

// A snapshot of the document with a real change in it: it serializes the document's bytes,
// shares its identity, revision, rows and records, answers record_change and field_changed as it
// does for every record and field, and refuses an edit, a save and a load (document.snapshot), its
// undo and redo doing nothing; after the document's own undo it serializes what it did, finds
// every record where it was (address_of) and answers what changed in each as it did.
void check_snapshot(const DocumentType &type, const Fixture &fixture, Document &document,
                    const std::vector<NodeAddress> &records, const std::string &serialized) {
	bool done = false;
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		for (const Value &option : options) {
			Diagnostic error;
			if (!document.apply(edit_of(EditOperation::Set, address, schema.id, option), error)) continue;
			const SerializeResult after = document.serialize();
			if (!after.ok() || after.text == serialized) {
				while (document.can_undo()) document.undo();
				continue;
			}
			done = true;
			const std::string where = where_of(fixture, document, address, schema.id) + " (snapshot)";
			const std::unique_ptr<Document> snapshot = document.snapshot();
			check(snapshot && snapshot->is_snapshot() && !document.is_snapshot(), where, "snapshot() makes a snapshot");
			if (!snapshot) {
				while (document.can_undo()) document.undo();
				return true;
			}
			++g_snapshots;
			const std::vector<NodeAddress> now = records_of(document);
			check(snapshot->serialize().text == after.text, where, "a snapshot serializes the document's bytes");
			check(snapshot->identity() == document.identity() && snapshot->revision() == document.revision() &&
			              snapshot->rows() == document.rows() && records_of(*snapshot) == now,
			      where, "a snapshot shares the document's identity, revision, rows and records");
			// Its answers, kept to hold it to after its document moves on.
			std::vector<Document::RecordChange> records_changed;
			std::vector<bool> fields_changed;
			for (const NodeAddress &record : now) {
				records_changed.push_back(snapshot->record_change(record));
				check(records_changed.back() == document.record_change(record), where_of(fixture, document, record, ""),
				      "a snapshot answers record_change as its document does");
				for (const FieldSchema &field : document.fields(record.kind)) {
					fields_changed.push_back(snapshot->field_changed(record, field.id));
					check(fields_changed.back() == document.field_changed(record, field.id),
					      where_of(fixture, document, record, field.id), "a snapshot answers field_changed as its document does");
				}
			}
			Value saved;
			check(document.saved_value(address, schema.id, saved), where, "the changed field has its saved value");
			Diagnostic refused;
			check(!snapshot->apply(edit_of(EditOperation::Set, address, schema.id, saved), refused) &&
			              refused.code == "document.snapshot",
			      where, "a snapshot refuses an edit (document.snapshot)");
			snapshot->undo();
			snapshot->redo();
			refused = Diagnostic();
			check(!snapshot->save(refused) && refused.code == "document.snapshot", where,
			      "a snapshot refuses a save (document.snapshot)");
			refused = Diagnostic();
			check(!snapshot->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", refused) &&
			              refused.code == "document.snapshot",
			      where, "a snapshot refuses a load (document.snapshot)");
			check(snapshot->revision() == document.revision() && snapshot->serialize().text == after.text &&
			              document.serialize().text == after.text,
			      where, "a snapshot's undo, redo and refusals leave it and its document as they were");
			while (document.can_undo()) document.undo();
			check(document.serialize().text == serialized && snapshot->serialize().text == after.text, where,
			      "the document's undo leaves its snapshot's bytes as they were");
			size_t field_at = 0;
			for (size_t i = 0; i < now.size(); ++i) {
				const NodeAddress &record = now[i];
				const std::string there = where_of(fixture, *snapshot, record, "") + " (snapshot, its document undone)";
				check(snapshot->address_of(record.child ? record.child : record.row) == record, there,
				      "a snapshot finds each record where it was after its document's undo");
				check(snapshot->record_change(record) == records_changed[i], there,
				      "a snapshot answers record_change as it did after its document's undo");
				for (const FieldSchema &field : snapshot->fields(record.kind))
					check(snapshot->field_changed(record, field.id) == fields_changed[field_at++], there + " ." + field.id,
					      "a snapshot answers field_changed as it did after its document's undo");
			}
			std::unique_ptr<Document> read = type.make();
			check(read->load_bytes(text_bytes(snapshot->serialize().text), fixture.name, fixture.kind, "jo", refused) &&
			              !read->blocked() && read->serialize().text == after.text,
			      where, "what a snapshot serializes reads back");
			return true;
		}
		return false;
	});
	check(done, fixture.name, "a document with a real change in it takes a snapshot");
}

// The type's validate_file (S13 D4): the file's own findings from its document alone, each on the
// file and, where it names a record, on one the document holds; a second load of the file, which
// gives its records the same identities, validates to the same findings.
void check_validate_file(const DocumentType &type, const Fixture &fixture,
		const Document &document, TypeCounts &counts) {
	const std::vector<Diagnostic> findings = type.validate_file(document);
	for (const Diagnostic &d : findings) {
		const std::string where = fixture.name + " " + d.code;
		check(d.asset == document.path(), where,
				"validate_file's findings are on the document's file");
		if (d.row_id)
			check(document.address_of(d.child_id ? d.child_id : d.row_id).row == d.row_id, where,
					"a finding names a record the document holds");
	}
	std::unique_ptr<Document> twin = type.make();
	Diagnostic error;
	check(twin->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error) &&
					type.validate_file(*twin) == findings,
			fixture.name, "a second load of the file validates to the same findings");
	g_findings += findings.size();
	counts.findings += findings.size();
}

void check_fixture(const DocumentType &type, const Fixture &fixture, TypeCounts &counts) {
	std::unique_ptr<Document> document = type.make();
	Diagnostic error;
	const bool loaded = document->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
	check(loaded && !document->blocked(), fixture.name + " (" + error.message + ")", "the file loads unblocked");
	if (!loaded || document->blocked()) return;
	const std::vector<NodeAddress> records = records_of(*document);
	check(!records.empty(), fixture.name, "the file has records to check");
	g_records += records.size();

	// Parse, serialize, parse again: the same bytes, the same records.
	const SerializeResult first = document->serialize();
	check(first.ok(), fixture.name, "the file serializes");
	std::unique_ptr<Document> again = type.make();
	const bool reloaded = again->load_bytes(text_bytes(first.text), fixture.name, fixture.kind, "jo", error);
	check(reloaded && !again->blocked(), fixture.name + " (" + error.message + ")", "what it serializes loads unblocked");
	if (reloaded) {
		const SerializeResult second = again->serialize();
		check(second.ok() && second.text == first.text && records_of(*again).size() == records.size(), fixture.name,
		      "parse, serialize, parse again serializes the same bytes and records");
	}

	check_kinds(fixture, *document, records);
	check_validate_file(type, fixture, *document, counts);
	check_places(type, fixture, *document, records);
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document->fields(address.kind)) check_schema(fixture, *document, address, schema);
	check_symbols(fixture, *document, records);
	check_set_to_self(fixture, *document, records, first.text);
	check_presence(fixture, *document, records, first.text, counts);
	check_real_change(fixture, *document, records, first.text);
	check_coalescing(fixture, *document, records, first.text);
	check_copy_paste(type, fixture, *document, records, first.text);
	check_snapshot(type, fixture, *document, records, first.text);
}

} // namespace

int main() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<Fixture> files = fixtures(repo);
	const std::vector<Fixture> flawed = flawed_files(repo);
	const std::vector<const DocumentType *> types = registered_types();
	for (const DocumentType *type : types) {
		size_t checked = 0;
		TypeCounts counts;
		for (const Fixture &fixture : files) {
			if (document_type_for(fixture.kind) != type) continue;
			check(!fixture.bytes.empty(), fixture.name, "the fixture is present");
			if (fixture.bytes.empty()) continue;
			check_fixture(*type, fixture, counts);
			++checked;
		}
		check(checked > 0, type->name, "the document type has a file here to check");
		for (const Fixture &fixture : flawed) {
			if (document_type_for(fixture.kind) != type) continue;
			std::unique_ptr<Document> document = type->make();
			Diagnostic error;
			const bool loaded = !fixture.bytes.empty() &&
			                    document->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
			check(loaded, fixture.name + " (" + error.message + ")", "the flawed file is made and loads");
			if (loaded) check_validate_file(*type, fixture, *document, counts);
		}
		// Per type: a validate_file that never took its own documents (its cast to another type)
		// would make nothing over its files.
		check(counts.findings > 0, type->name, "validate_file makes a finding over the type's files");
		PinnedPresence pinned{type->name, 0, 0};
		for (const PinnedPresence &pin : kPinnedPresence)
			if (std::string(pin.type) == type->name) pinned = pin;
		check(counts.optional == pinned.optional && counts.presences == pinned.presences, type->name,
		      "a type's optional fields asked and left out and written again are the ones pinned");
		std::printf("  %s: %zu optional fields asked, %zu left out and written again, %zu findings\n", type->name,
		            counts.optional, counts.presences, counts.findings);
	}
	for (const Fixture &fixture : files)
		check(document_type_for(fixture.kind) != nullptr, fixture.name, "the file is of a registered type");
	check(g_other_scopes > 0, "the files", "a name defined in two scopes is looked up in the other");
	check(g_presences > 0 && g_pastes > 0, "the files", "an optional field is left out and written, a record pasted");
	if (g_failures == 0)
		std::printf("editor_document_contract: all %zu document types keep the contract "
					"(%zu files, %zu records, %zu fields set to their own value, %zu symbols, "
					"%zu lookups in another scope of the name, %zu optional fields left out and "
					"written again, %zu kept always written, %zu real changes undone and redone, "
					"%zu coalesced, %zu records pasted, %zu snapshots, %zu record kinds, "
					"%zu findings validate_file made)\n",
				types.size(), files.size(), g_records, g_sets, g_symbols, g_other_scopes,
				g_presences, g_kept, g_changes, g_coalesced, g_pastes, g_snapshots, g_kinds.size(),
				g_findings);
	return g_failures == 0 ? 0 : 1;
}
