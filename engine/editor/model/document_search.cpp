#include "document_search.h"

#include <base/io/strutil.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/field_text.h>

namespace opennova::editor {

size_t find_text(const std::string &in, const std::string &text, const SearchOptions &options) {
	if (text.empty()) return std::string::npos;
	if (options.match_case) return in.find(text);
	return strutil::ifind(in, text);
}

std::vector<DocumentHit> find_in_document(const Document &document, const std::string &text, const SearchOptions &options) {
	std::vector<DocumentHit> hits;
	if (text.empty()) return hits;
	// A value naming one of its record's own choices is found by that choice's name, as the
	// Inspector shows it (Document::choices_on: a model's part by its label, a spawn slot by its
	// vehicle's id), the choices made into one list reused field after field. A value naming a
	// record of the file by its index (a Record reference, S13 D8) is found by that record's own
	// name, as the picker labels it (a CTRL register by its NAME): the record sets, read once.
	std::vector<FieldChoice> own, named;
	const std::vector<Document::TargetedCollection> &targets = document.targeted_collections();
	std::vector<std::vector<NodeAddress>> sets;
	bool sets_read = false;
	const auto record_named = [&](const FieldUse &field, const Value &value) {
		int64_t index = 0;
		if (!record_index(field.reference, value, index)) return false;
		for (size_t t = 0; t < targets.size(); ++t) {
			if (targets[t].reference != field.reference) continue;
			if (!sets_read) {
				sets = document.record_sets();
				sets_read = true;
			}
			if (size_t(index) >= sets[t].size()) return false;
			named.assign(1, FieldChoice{std::to_string(index), index, document.own_name(sets[t][size_t(index)])});
			return true;
		}
		return false;
	};
	const auto search = [&](const NodeAddress &address) {
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			if (schema.optional && !document.present(address, schema.id)) continue;
			if (field.applies == Applicability::Ignored && !written(document, address, schema)) continue;
			Value value;
			if (!document.get(address, schema.id, value)) continue;
			DocumentHit hit;
			hit.text = field_text(schema, record_named(field, value) ? named : document.choices_on(address, field, own),
			                      value);
			hit.at = find_text(hit.text, text, options);
			if (hit.at == std::string::npos) continue;
			hit.address = address;
			hit.locator = document.locator(address);
			hit.record = document.record_path(address);
			hit.field = schema.id;
			hit.label = field_title(schema);
			hits.push_back(std::move(hit));
		}
	};
	for (const auto &row : document.rows()) {
		if (!row) continue;
		search({row->id, row->kind, 0});
		document.walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
			search(record);
			return true;
		});
	}
	return hits;
}

} // namespace opennova::editor
