// The display-name service (display_names.h, ADR 0046 S15 Names).
#include <editor/graph/display_names.h>

#include <optional>

#include <base/io/strutil.h>
#include <editor/documents/document_types.h>
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
		const std::string words = symbol_words(*symbol);
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
	const std::string words = symbol_words(*symbol);
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
		key_ = key;
		titles_.clear();
	}
	const auto found = titles_.find(address);
	if (found != titles_.end()) return found->second;
	++made_;
	return titles_.emplace(address, record_display(document, address, names)).first->second;
}

} // namespace opennova::editor
