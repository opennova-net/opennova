// S12 Z (ADR 0046 S12, "Adding a document type"): the contract every registered document type
// keeps, checked over a file of its kinds. The types are the registry's own (every asset kind
// document_type_for answers, each type once), so a new type fails here until it has a file below.
// Over each file: the document loads unblocked; parse, serialize, parse again serializes the same
// bytes; every field of every record reads, carries a label (a def member only its line does not
// write as a number of its own may go by its id), a range that is one (a number's, low to high),
// the same id and type as the record's field_on gives it, and field_on never makes a read-only
// field writable; a writable field set to the value it reads leaves the serialized bytes as they
// were; every field a record defines a name by yields a symbol whose locator names that record,
// and Document::find resolves its name in a scope only to a definition of its kind and name
// there (nothing in a scope with none); every reference and definition kind has its
// reference_kinds row.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/document.h>
#include <formats/def/def_schema.h>

#include "common/file_io.h"
#include "common/test_paths.h"

using namespace opennova::editor;

namespace {

int g_failures = 0;
// What was checked, for the summary line: records, fields set to their own value, symbols, and
// lookups of a name in another scope that defines it too.
size_t g_records = 0, g_sets = 0, g_symbols = 0, g_other_scopes = 0;

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

// Every registered document type, once each, in the order the asset kinds are declared (the
// kinds run from 1 until the first value the kind table does not name).
std::vector<const DocumentType *> registered_types() {
	std::vector<const DocumentType *> out;
	for (int i = 1; std::string(asset_kind_token(static_cast<AssetKind>(i))) != "unknown"; ++i) {
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
	const FieldSchema field = document.field_on(address, schema);
	check(labelled(document, address, schema) && labelled(document, address, field), where,
	      "the field carries the label the editor names it by");
	Value value;
	check(document.get(address, field.id, value), where, "the field reads");
	check(field.id == schema.id && field.type == schema.type, where, "field_on keeps the field's id and type");
	check(!schema.read_only || field.read_only, where, "field_on never makes a read-only field writable");
	for (const FieldSchema *f : {&schema, &field})
		if (f->ranged)
			check(f->type != FieldType::Text && f->min <= f->max && f->step >= 0.0, where,
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
			const FieldSchema field = document.field_on(address, schema);
			Value value;
			if (field.read_only || !document.get(address, field.id, value)) continue;
			const std::string where = where_of(fixture, document, address, field.id);
			Edit edit;
			edit.operation = EditOperation::Set;
			edit.address = address;
			edit.field = field.id;
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
// by its name alone, to a definition of its kind and name.
void check_symbols(const Fixture &fixture, const Document &document, const std::vector<NodeAddress> &records) {
	Extracted extracted;
	extract_from_document(document, extracted);
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldSchema field = document.field_on(address, schema);
			if (field.defines == ReferenceKind::None || field.applies == Applicability::Ignored ||
			    !document.present(address, field.id))
				continue;
			const std::string where = where_of(fixture, document, address, field.id);
			Value value;
			const bool read = document.get(address, field.id, value);
			check(read, where, "a field that defines a name reads");
			if (!read || !defines_a_name(value)) continue;
			const auto symbol = std::find_if(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &s) {
				return s.address == address && s.field == field.id;
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
			check(document.find(symbol.display, found) && defined_as(symbol, found, false, std::string()), where,
			      "find resolves a name no lookup reaches by the name alone");
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
			const bool resolved = document.find(symbol.display, found, scope);
			const std::string in = where + " in '" + scope + "'";
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

void check_fixture(const DocumentType &type, const Fixture &fixture) {
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

	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document->fields(address.kind)) check_schema(fixture, *document, address, schema);
	check_symbols(fixture, *document, records);
	check_set_to_self(fixture, *document, records, first.text);
}

} // namespace

int main() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<Fixture> files = fixtures(repo);
	const std::vector<const DocumentType *> types = registered_types();
	for (const DocumentType *type : types) {
		size_t checked = 0;
		for (const Fixture &fixture : files) {
			if (!type->handles(fixture.kind)) continue;
			check(!fixture.bytes.empty(), fixture.name, "the fixture is present");
			if (fixture.bytes.empty()) continue;
			check_fixture(*type, fixture);
			++checked;
		}
		check(checked > 0, type->name, "the document type has a file here to check");
	}
	for (const Fixture &fixture : files)
		check(document_type_for(fixture.kind) != nullptr, fixture.name, "the file is of a registered type");
	check(g_other_scopes > 0, "the files", "a name defined in two scopes is looked up in the other");
	if (g_failures == 0)
		std::printf("editor_document_contract: all %zu document types keep the contract (%zu files, %zu records, "
		            "%zu fields set to their own value, %zu symbols, %zu lookups in another scope of the name)\n",
		            types.size(), files.size(), g_records, g_sets, g_symbols, g_other_scopes);
	return g_failures == 0 ? 0 : 1;
}
