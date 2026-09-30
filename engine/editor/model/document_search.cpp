#include "document_search.h"

#include <editor/model/field_text.h>

#include <algorithm>
#include <cctype>

namespace opennova::editor {

size_t find_text(const std::string &in, const std::string &text, const SearchOptions &options) {
	if (text.empty()) return std::string::npos;
	if (options.match_case) return in.find(text);
	const auto same = [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	};
	const auto found = std::search(in.begin(), in.end(), text.begin(), text.end(), same);
	return found == in.end() ? std::string::npos : size_t(found - in.begin());
}

std::vector<DocumentHit> find_in_document(const Document &document, const std::string &text, const SearchOptions &options) {
	std::vector<DocumentHit> hits;
	if (text.empty()) return hits;
	// A record's own choices are the widgets' alone (Document::record_choices): a value naming
	// one is found as the file writes it (a register's index, not its name).
	static const std::vector<FieldChoice> none;
	const auto search = [&](const NodeAddress &address) {
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			if (schema.optional && !document.present(address, schema.id)) continue;
			if (field.applies == Applicability::Ignored && !written(document, address, schema)) continue;
			Value value;
			if (!document.get(address, schema.id, value)) continue;
			DocumentHit hit;
			hit.text = field_text(schema, field.own_choices ? none : schema.choices, value);
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
