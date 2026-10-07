// The display-name service (display_names.h, ADR 0046 S15 Names).
#include <editor/graph/display_names.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>

#include <base/io/strutil.h>
#include <editor/documents/document_types.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/field_text.h>

namespace opennova::editor {

namespace {

const DocumentType *type_of(const Document &document) { return document_type_for(document.kind()); }

// What a pick of a choice sets the field to (the picker's picked_value): the text, or the number a
// name of a Record reference or a number field is.
bool choice_value(const FieldUse &field, const ReferenceChoice &choice, Value &out) {
	if (field.schema->type == FieldType::Text) {
		out = choice.name;
		return true;
	}
	const std::optional<int> number = strutil::parse_int(choice.name);
	if (!number) return false;
	out = int64_t(*number);
	return true;
}

// A name of `kind` the project lacks, in words.
std::string missing_words(ReferenceKind kind, const std::string &name) {
	if (kind == ReferenceKind::Item) return "No item " + name + " in the project";
	if (kind == ReferenceKind::TextId) return "No text " + name + " in the project's tables";
	return "No " + std::string(reference_row(kind).label) + " '" + name + "' in the project";
}

} // namespace

std::string record_display(const Document &document, const NodeAddress &address, const NameSource *names) {
	if (const DocumentType *type = type_of(document); type && type->record_label) {
		std::string label = type->record_label(document, address, names);
		if (!label.empty()) return label;
	}
	return document.record_title(address);
}

std::string record_brief(const Document &document, const NodeAddress &address, const NameSource *names) {
	const DocumentType *type = type_of(document);
	return type && type->record_brief ? type->record_brief(document, address, names) : std::string();
}

DisplayName value_display(const Document &document, const NodeAddress &address, const FieldUse &field, const Value &value,
                          const NameSource *names) {
	DisplayName out;
	if (!field.schema) return out;
	if (const DocumentType *type = type_of(document); type && type->value_label)
		if (type->value_label(document, address, field, value, names, out)) return out;
	out = DisplayName();
	// A %NAME% by what it stands for (the plain-words lane, the audit's 3.3): the value of the definition the
	// game reads, brand.mns over menu_style.mns, the last in each [orig: Menu_InitShellResources @ 0x552500]
	// (AssetGraph::style_binding), a colour as its swatch beside it.
	if (const auto *text = std::get_if<std::string>(&value); names && text && graph_names::is_style_reference(*text)) {
		out.raw = *text;
		if (const GraphSymbol *binding = names->symbol(ReferenceKind::StyleVar, *text)) {
			out.text = "= " + (binding->value.empty() ? std::string("(empty)") : binding->value);
			out.resolved = binding->value;
			out.source = binding->file;
		} else {
			out.text = "No stylesheet the game reads defines " + *text;
			out.dangling = true;
		}
		return out;
	}
	// A choice's name (a flags field's bits by their names).
	std::vector<FieldChoice> own;
	const std::vector<FieldChoice> &choices = document.choices_on(address, field, own);
	if (!choices.empty()) {
		const std::string text = field_text(*field.schema, choices, value);
		const std::string raw = std::holds_alternative<int64_t>(value) ? std::to_string(std::get<int64_t>(value))
		                        : std::holds_alternative<std::string>(value) ? std::get<std::string>(value)
		                                                                    : std::string();
		if (!text.empty() && text != raw) {
			out.text = text;
			out.raw = raw;
		}
		return out;
	}
	// What the reference names: a definition of the project by its words.
	ReferenceKind kind;
	std::string name, scope;
	if (!names || !reference_target(field, value, kind, name, scope)) return out;
	const ReferenceKindRow &row = reference_row(kind);
	if (row.resolution != ReferenceResolution::Symbol && row.resolution != ReferenceResolution::Record) return out;
	out.raw = name;
	if (const GraphSymbol *symbol = names->symbol(kind, name, scope)) {
		const std::string words = definition_words(*symbol, names);
		if (words != name) out.text = words;
		out.source = symbol->scope.empty() ? symbol->file : symbol->scope;
		return out;
	}
	out.text = missing_words(kind, name);
	out.dangling = true;
	return out;
}

void word_choices(const Document &document, const NodeAddress &address, const FieldUse &field, const NameSource *names,
                  std::vector<ReferenceChoice> &choices) {
	if (!field.schema) return;
	for (ReferenceChoice &choice : choices) {
		if (!choice.label.empty()) continue;
		Value value;
		if (!choice_value(field, choice, value)) continue;
		const DisplayName words = value_display(document, address, field, value, names);
		if (!words.text.empty() && words.text != choice.name) choice.label = words.text;
	}
}

FieldUse picked_as(const FieldUse &field) {
	FieldUse picking = field;
	if (picking.reference == ReferenceKind::None) picking.reference = field.picks;
	return picking;
}

bool key_number(ReferenceKind kind, const std::string &key, const char *prefix, int64_t &out) {
	if (!prefix) return false;
	const size_t length = std::strlen(prefix);
	if (key.size() <= length) return false;
	const bool exact = reference_row(kind).name_case == NameCase::Exact;
	if (exact ? key.compare(0, length, prefix) != 0 : !strutil::iequals(key.substr(0, length), prefix)) return false;
	const std::string digits = key.substr(length);
	if (!strutil::all_digits(digits)) return false;
	const std::optional<int> number = strutil::parse_int(digits);
	if (!number) return false;
	// The key the game forms from the number ("%s%03i") is this one, or no number forms it.
	char formed[32];
	std::snprintf(formed, sizeof(formed), "%03i", *number);
	if (digits != formed) return false;
	out = int64_t(*number);
	return true;
}

bool text_key_number(const std::string &key, const char *prefix, int64_t &out) {
	return key_number(ReferenceKind::TextId, key, prefix, out);
}

bool keyed_reference(const AssetGraph &graph, const FieldUse &field, const Value &value, FieldUse &out, Value &key) {
	const int64_t *number = std::get_if<int64_t>(&value);
	if (!field.key_prefix || field.picks == ReferenceKind::None || !number || *number < field.key_first ||
	    *number > field.key_last)
		return false;
	char formed[64];
	std::snprintf(formed, sizeof(formed), "%s%03i", field.key_prefix, int(*number));
	GraphEdge edge;
	edge.kind = field.picks;
	edge.scope = field.scope;
	if (!field.scope_alternate.empty()) edge.scope_alternate = field.scope_alternate;
	out = field;
	out.reference = field.picks;
	out.picks = ReferenceKind::None;
	out.key_prefix = nullptr;
	out.scope = graph.lookup_scope(edge);
	key = std::string(formed);
	return true;
}

namespace {

// The names of a field whose number forms one (FieldUse::key_prefix) of the kind it picks, in its scope
// as the game reads it (a text key's section of its own table where the project has it, else the
// alternate: AssetGraph::lookup_scope; a dialog's bank), each named by the number that forms it, only one
// the game looks a name up by (key_first..key_last) and the field can hold, the name kept for its preview.
std::vector<ReferenceChoice> keyed_choices(const AssetGraph &graph, const FieldUse &field) {
	GraphEdge edge;
	edge.kind = field.picks;
	edge.scope = field.scope;
	if (!field.scope_alternate.empty()) edge.scope_alternate = field.scope_alternate;
	const std::string scope = graph.lookup_scope(edge);
	std::vector<ReferenceChoice> out;
	for (ReferenceChoice &choice : graph.choices(field.picks, scope)) {
		int64_t number = 0;
		if (!key_number(field.picks, choice.name, field.key_prefix, number)) continue;
		if (number < field.key_first || number > field.key_last) continue;
		if (field.schema && field.schema->ranged && (double(number) < field.schema->min || double(number) > field.schema->max))
			continue;
		choice.symbol = std::move(choice.name);
		choice.symbol_scope = scope;
		choice.name = std::to_string(number);
		out.push_back(std::move(choice));
	}
	return out;
}

} // namespace

std::vector<ReferenceChoice> picker_choices(const AssetGraph *graph, const Document &document, const NodeAddress &address,
                                            const FieldUse &field, const NameSource *names) {
	const FieldUse picking = picked_as(field);
	std::vector<ReferenceChoice> choices;
	if (const DocumentType *type = type_of(document); type && type->game_choices) {
		std::vector<GameChoice> own;
		type->game_choices(document, address, picking, own);
		for (const GameChoice &game : own) {
			ReferenceChoice choice;
			choice.name = game.name;
			choice.label = game.label;
			choice.kind = picking.reference;
			choice.file = document.path();
			choices.push_back(std::move(choice));
		}
	}
	if (graph) {
		std::vector<ReferenceChoice> listed = field.picks != ReferenceKind::None && picking.key_prefix
		                                              ? keyed_choices(*graph, field)
		                                              : reference_choices(*graph, picking);
		// A number naming its definition by itself plus an offset (an ammo's tracer id, FieldUse::name_offset):
		// each definition by the number that names it (an item's id less 100000), one no such number names
		// (none above 0) left out; its own name kept as the symbol it stands for.
		if (picking.name_offset) {
			std::vector<ReferenceChoice> offset;
			for (ReferenceChoice &choice : listed) {
				const std::optional<int> id = strutil::parse_int(choice.name);
				if (!id || int64_t(*id) - picking.name_offset <= 0) continue;
				choice.symbol = std::move(choice.name);
				choice.symbol_scope = picking.scope;
				choice.name = std::to_string(int64_t(*id) - picking.name_offset);
				offset.push_back(std::move(choice));
			}
			listed = std::move(offset);
		}
		choices.insert(choices.end(), listed.begin(), listed.end());
	}
	word_choices(document, address, picking, names, choices);
	return choices;
}

size_t name_characters(const std::string &name) {
	size_t count = 0;
	for (const char c : name) count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
	return count;
}

size_t field_name_limit(const FieldUse &field) {
	if (!field.schema || field.schema->type != FieldType::Text || field.schema->width < 2) return 0;
	return field.schema->width - 1;
}

std::vector<ReferenceCompletion> complete_reference(const std::vector<ReferenceChoice> &choices, const FieldUse &field,
                                                    const std::string &typed) {
	const FieldUse picking = picked_as(field);
	// As the kind's lookup compares names: an item id's digits as written, any other without case (graph_names'
	// symbol_name keys; a file name's case and its slashes alike).
	const bool exact_case = reference_row(picking.reference).name_case == NameCase::Exact;
	const auto spelled = [exact_case](const std::string &text) { return exact_case ? text : strutil::to_upper(text); };
	const std::string wanted = spelled(typed);
	const size_t limit = field_name_limit(field);
	std::vector<ReferenceCompletion> first, then, unreached;
	for (const ReferenceChoice &choice : choices) {
		ReferenceCompletion completion;
		completion.choice = choice;
		const std::string name = spelled(choice.name);
		completion.prefix = name.compare(0, wanted.size(), wanted) == 0;
		completion.exact = completion.prefix && name.size() == wanted.size();
		completion.fits = !limit || name_characters(choice.name) <= limit;
		const bool holds = completion.prefix || name.find(wanted) != std::string::npos ||
		                   (!choice.label.empty() && strutil::to_upper(choice.label).find(strutil::to_upper(typed)) != std::string::npos);
		if (!holds) continue;
		(choice.inert ? unreached : completion.prefix ? first : then).push_back(std::move(completion));
	}
	first.insert(first.end(), std::make_move_iterator(then.begin()), std::make_move_iterator(then.end()));
	first.insert(first.end(), std::make_move_iterator(unreached.begin()), std::make_move_iterator(unreached.end()));
	return first;
}

std::string edge_record_words(const GraphEdge &edge) {
	if (edge.record_title.empty()) return edge.record;
	const size_t slash = edge.record.rfind('/');
	return slash == std::string::npos ? edge.record_title : edge.record.substr(0, slash + 1) + edge.record_title;
}

std::string field_words(AssetKind kind, NodeKind record_kind, const std::string &field) {
	const DocumentType *type = document_type_for(kind);
	if (!type || !type->fields || field.empty()) return std::string();
	for (const FieldSchema &schema : type->fields(record_kind))
		if (schema.id == field) {
			const std::string title = field_title(schema);
			return title == field ? std::string() : title;
		}
	return std::string();
}

std::string edge_field_words(const GraphEdge &edge, AssetKind source_kind) {
	const std::string words = field_words(source_kind, edge.address.kind, edge.field);
	return words.empty() ? edge.field : words;
}

std::string edge_place_words(const GraphEdge &edge, AssetKind source_kind) {
	const std::string record = edge_record_words(edge), field = edge_field_words(edge, source_kind);
	if (record.empty()) return field;
	return field.empty() ? record : record + " - " + field;
}

std::string symbol_preview(const AssetGraph &graph, ReferenceKind kind, const std::string &name, const std::string &scope) {
	const GraphSymbol *symbol = graph.resolve_symbol(kind, name, scope);
	if (!symbol) return std::string();
	std::string out;
	if (kind == ReferenceKind::Item) {
		out = symbol_words(*symbol) + " (item " + symbol->display + ", " + symbol->file + ")";
		// The model its graphic loads (the record's own edge of that field).
		for (const GraphEdge *edge : graph.references_of(symbol->file)) {
			if (edge->record != symbol->record || edge->field != "graphic") continue;
			std::string file;
			out += "\nModel: " + edge->value;
			if (graph.resolve(*edge, &file) != ReferenceStatus::Present) out += " (not in the project)";
			break;
		}
		return out;
	}
	if (kind == ReferenceKind::TextId) return "\"" + symbol_words(*symbol) + "\"\n" + (symbol->scope.empty() ? symbol->file : symbol->scope);
	const GraphNameSource names(graph);
	const std::string words = definition_words(*symbol, &names);
	return words != symbol->display ? words : std::string();
}

const std::string &DisplayNameCache::record(const Document &document, const NodeAddress &address, const NameSource *names) {
	Key key;
	key.document = document.identity();
	key.load = document.load_generation();
	key.revision = document.revision();
	key.has_names = names != nullptr;
	key.names = names ? names->generation() : 0;
	if (!(key == key_)) {
		// Held: only the revision moved, by the held gesture's batches alone, which leave every title as it
		// was.
		const bool kept = held_ && key.document == key_.document && key.load == key_.load && key.names == key_.names &&
		                  key.has_names == key_.has_names && document.gesture_alone_since(held_, key_.revision);
		key_ = key;
		if (!kept) {
			titles_.clear();
			++dropped_;
		}
	}
	const auto found = titles_.find(address);
	if (found != titles_.end()) return found->second;
	++made_;
	return titles_.emplace(address, record_display(document, address, names)).first->second;
}

} // namespace opennova::editor
